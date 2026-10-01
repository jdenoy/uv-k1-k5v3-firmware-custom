/* Copyright 2026 Johan Denoyer F4WAT
 * https://github.com/jdenoy
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 */

/* Selective calling (ZVEI-1/2, CCIR-1/2): per-channel storage and on-demand
 * transmission (see selcall.h for the format and the behaviour). */

#include "app/selcall.h"
#include "audio.h"
#include "driver/bk4819.h"
#include "driver/bk4819-regs.h"
#include "driver/py25q16.h"
#include "driver/system.h"
#include "misc.h"
#include "radio.h"
#include "settings.h"
#include <string.h>

/* Free physical area of each config bank: the bank uses 0x0000-0x886E,
 * 0x9000-0x90E8 and 0xA000-0xA170 (see driver/eeprom_compat.c); CHIRP writes
 * up to 0xA170 in its own map. 1038 records x 8 bytes = 8304 bytes -> 3 sectors
 * (SELCALL_BASE and the record layout are in selcall.h). */
#define SELCALL_SECTORS      3u
#define SELCALL_TONE_GAIN    66u         /* TONE1 tuning gain, same as the 1750 Hz tone */
#define SELCALL_NO_INDEX     0xFFFFu

bool gSelCallTx;
bool gSelCallEndTx;
uint8_t gSelCallPending;

static uint16_t sPendingTones[SELCALL_DIGITS];
static uint16_t sPendingToneMs;

static uint16_t RecordIndex(uint16_t channel, uint8_t vfo)
{
    if (IS_MR_CHANNEL(channel))
        return channel;
    if (IS_FREQ_CHANNEL(channel))
        return (uint16_t)(MR_CHANNELS_MAX + (channel - FREQ_CHANNEL_FIRST) * 2u + (vfo & 1u));
    return SELCALL_NO_INDEX;               /* NOAA: no transmit anyway */
}

/* Raw 8-byte record. No record (NOAA) reads as erased flash: type off, no code. */
static void ReadRecord(uint16_t channel, uint8_t vfo, uint8_t *rec)
{
    const uint16_t idx = RecordIndex(channel, vfo);
    if (idx == SELCALL_NO_INDEX)
        memset(rec, 0xFF, SELCALL_REC_SIZE);
    else
        PY25Q16_ReadBuffer(SELCALL_BASE + (uint32_t)idx * SELCALL_REC_SIZE, rec, SELCALL_REC_SIZE);
}

static void WriteRecord(uint16_t channel, uint8_t vfo, const uint8_t *rec)
{
    const uint16_t idx = RecordIndex(channel, vfo);
    if (idx != SELCALL_NO_INDEX)
        PY25Q16_WriteBuffer(SELCALL_BASE + (uint32_t)idx * SELCALL_REC_SIZE, rec, SELCALL_REC_SIZE, false);
}

/* Field value from a record: type (invalid -> off) or 24-bit LE code
 * (above 99999, erased included -> no code). */
static uint32_t GetField(const uint8_t *rec, uint8_t field)
{
    if (field == SELCALL_F_TYPE)
        return rec[0] < SELCALL_TYPE_COUNT ? rec[0] : SELCALL_OFF;
    const uint8_t *p = &rec[field * 3u - 2u];     /* code 1 at [1..3], code 2 at [4..6] */
    const uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
    return v <= SELCALL_CODE_MAX ? v : SELCALL_CODE_NONE;
}

int32_t SELCALL_GetField(uint8_t field)
{
    uint8_t rec[SELCALL_REC_SIZE];
    ReadRecord(gTxVfo->CHANNEL_SAVE, gEeprom.TX_VFO, rec);
    return (int32_t)GetField(rec, field);
}

void SELCALL_SetField(uint8_t field, int32_t value)
{
    uint8_t rec[SELCALL_REC_SIZE];
    uint32_t v = (uint32_t)value;
    ReadRecord(gTxVfo->CHANNEL_SAVE, gEeprom.TX_VFO, rec);
    if (field == SELCALL_F_TYPE) {
        rec[0] = v < SELCALL_TYPE_COUNT ? (uint8_t)v : SELCALL_OFF;
    } else {
        uint8_t *p = &rec[field * 3u - 2u];
        if (v > SELCALL_CODE_MAX)
            v = 0xFFFFFFu;                       /* erased pattern = no code */
        p[0] = (uint8_t)v;
        p[1] = (uint8_t)(v >> 8);
        p[2] = (uint8_t)(v >> 16);
    }
    WriteRecord(gTxVfo->CHANNEL_SAVE, gEeprom.TX_VFO, rec);
}

void SELCALL_Clear(uint16_t channel)
{
    uint8_t rec[SELCALL_REC_SIZE];
    memset(rec, 0xFF, sizeof(rec));               /* erased: type off, no code */
    WriteRecord(channel, 0, rec);
}

void SELCALL_Copy(uint16_t fromChannel, uint8_t fromVfo, uint16_t toChannel)
{
    uint8_t rec[SELCALL_REC_SIZE];
    ReadRecord(fromChannel, fromVfo, rec);
    WriteRecord(toChannel, 0, rec);
}

void SELCALL_EraseAll(void)
{
    for (uint32_t i = 0; i < SELCALL_SECTORS; i++)
        PY25Q16_SectorErase(SELCALL_BASE + i * 0x1000u);
}

bool SELCALL_Request(uint8_t which)
{
    const uint8_t type = (uint8_t)SELCALL_GetField(SELCALL_F_TYPE);
    const uint32_t code = (uint32_t)SELCALL_GetField((uint8_t)(SELCALL_F_CODE1 + (which & 1u)));
    gSelCallPending  = SELCALL_BuildTones(type, code, sPendingTones);
    sPendingToneMs = SELCALL_ToneMs(type);
    return gSelCallPending != 0;
}

void SELCALL_Transmit(void)
{
    const uint8_t count = gSelCallPending;

    gSelCallPending = 0;
    gSelCallTx = true;

    /* Carrier and the channel's CTCSS/DCS are already on (RADIO_SetTxParameters):
     * mute the microphone path, let the repeater open, then send the tones. */
    BK4819_EnterTxMute();

    const bool sideTone = gEeprom.DTMF_SIDE_TONE;
    if (sideTone) {
        AUDIO_AudioPathOn();
        gEnableSpeaker = true;
    }
    BK4819_SetAF(sideTone ? BK4819_AF_BEEP : BK4819_AF_MUTE);

    BK4819_WriteRegister(BK4819_REG_70,
        BK4819_REG_70_ENABLE_TONE1 | (SELCALL_TONE_GAIN << BK4819_REG_70_SHIFT_TONE1_TUNING_GAIN));
    BK4819_EnableTXLink();

    SYSTEM_DelayMs(SELCALL_PRELOAD_MS);

    for (uint8_t i = 0; i < count; i++)
        BK4819_PlayToneRaw(sPendingTones[i], sPendingToneMs);    /* unmute, tone, mute */

    if (sideTone) {
        AUDIO_AudioPathOff();
        gEnableSpeaker = false;
    }
    BK4819_SetAF(BK4819_AF_MUTE);
    BK4819_WriteRegister(BK4819_REG_70, 0x0000);
    BK4819_WriteRegister(BK4819_REG_30, 0xC1FE);               /* as BK4819_PlaySingleTone */
    BK4819_ExitTxMute();

    gSelCallEndTx = true;                  /* the main loop unkeys like a PTT release */
}
