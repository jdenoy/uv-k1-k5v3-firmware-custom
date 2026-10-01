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

/* Free physical area of each config bank: the bank uses 0x0000-0x886E,
 * 0x9000-0x90E8 and 0xA000-0xA170 (see driver/eeprom_compat.c); CHIRP writes
 * up to 0xA170 in its own map. 1038 records x 8 bytes = 8304 bytes -> 3 sectors
 * (SELCALL_BASE and the record layout are in selcall.h). */
#define SELCALL_SECTORS      3u
#define SELCALL_TONE_GAIN    66u         /* TONE1 tuning gain, same as the 1750 Hz tone */
#define SELCALL_NO_INDEX     0xFFFFu

bool gSelCallTx;
bool gSelCallEndTx;

static uint16_t sPendingTones[SELCALL_DIGITS];
static uint8_t  sPendingCount;
static uint16_t sPendingToneMs;

static uint16_t RecordIndex(uint16_t channel, uint8_t vfo)
{
    if (IS_MR_CHANNEL(channel))
        return channel;
    if (IS_FREQ_CHANNEL(channel))
        return (uint16_t)(MR_CHANNELS_MAX + (channel - FREQ_CHANNEL_FIRST) * 2u + (vfo & 1u));
    return SELCALL_NO_INDEX;               /* NOAA: no transmit anyway */
}

static uint32_t GetCode(const uint8_t *p)
{
    const uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
    return v <= SELCALL_CODE_MAX ? v : SELCALL_CODE_NONE;
}

static void PutCode(uint8_t *p, uint32_t v)
{
    if (v > SELCALL_CODE_MAX)
        v = 0xFFFFFFu;                  /* erased pattern = no code */
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
}

void SELCALL_Load(uint16_t channel, uint8_t vfo, SelCall_Channel_t *out)
{
    uint8_t rec[SELCALL_REC_SIZE];
    const uint16_t idx = RecordIndex(channel, vfo);

    out->type    = SELCALL_OFF;
    out->code[0] = SELCALL_CODE_NONE;
    out->code[1] = SELCALL_CODE_NONE;
    if (idx == SELCALL_NO_INDEX)
        return;

    PY25Q16_ReadBuffer(SELCALL_BASE + (uint32_t)idx * SELCALL_REC_SIZE, rec, sizeof(rec));
    out->type    = rec[0] < SELCALL_TYPE_COUNT ? rec[0] : SELCALL_OFF;
    out->code[0] = GetCode(&rec[1]);
    out->code[1] = GetCode(&rec[4]);
}

static void SaveIndex(uint16_t idx, const SelCall_Channel_t *in)
{
    uint8_t rec[SELCALL_REC_SIZE];

    rec[0] = in->type < SELCALL_TYPE_COUNT ? in->type : SELCALL_OFF;
    PutCode(&rec[1], in->code[0]);
    PutCode(&rec[4], in->code[1]);
    rec[7] = 0xFF;
    PY25Q16_WriteBuffer(SELCALL_BASE + (uint32_t)idx * SELCALL_REC_SIZE, rec, sizeof(rec), false);
}

void SELCALL_Save(uint16_t channel, uint8_t vfo, const SelCall_Channel_t *in)
{
    const uint16_t idx = RecordIndex(channel, vfo);
    if (idx != SELCALL_NO_INDEX)
        SaveIndex(idx, in);
}

void SELCALL_Clear(uint16_t channel)
{
    const SelCall_Channel_t none = { SELCALL_OFF, { SELCALL_CODE_NONE, SELCALL_CODE_NONE } };
    SELCALL_Save(channel, 0, &none);
}

void SELCALL_Copy(uint16_t fromChannel, uint8_t fromVfo, uint16_t toChannel)
{
    SelCall_Channel_t c;
    SELCALL_Load(fromChannel, fromVfo, &c);
    SELCALL_Save(toChannel, 0, &c);
}

void SELCALL_EraseAll(void)
{
    for (uint32_t i = 0; i < SELCALL_SECTORS; i++)
        PY25Q16_SectorErase(SELCALL_BASE + i * 0x1000u);
}

bool SELCALL_Request(uint8_t which)
{
    SelCall_Channel_t c;

    SELCALL_Load(gTxVfo->CHANNEL_SAVE, gEeprom.TX_VFO, &c);
    sPendingCount  = SELCALL_BuildTones(c.type, c.code[which & 1u], sPendingTones);
    sPendingToneMs = SELCALL_ToneMs(c.type);
    return sPendingCount != 0;
}

bool SELCALL_Pending(void)
{
    return sPendingCount != 0;
}

void SELCALL_Cancel(void)
{
    sPendingCount = 0;
}

void SELCALL_Transmit(void)
{
    const uint8_t count = sPendingCount;

    sPendingCount = 0;
    gSelCallTx = true;

    /* Carrier and the channel's CTCSS/DCS are already on (RADIO_SetTxParameters):
     * mute the microphone path, let the repeater open, then send the tones. */
    BK4819_EnterTxMute();

    if (gEeprom.DTMF_SIDE_TONE) {
        AUDIO_AudioPathOn();
        gEnableSpeaker = true;
        BK4819_SetAF(BK4819_AF_BEEP);
    } else {
        BK4819_SetAF(BK4819_AF_MUTE);
    }

    BK4819_WriteRegister(BK4819_REG_70,
        BK4819_REG_70_ENABLE_TONE1 | (SELCALL_TONE_GAIN << BK4819_REG_70_SHIFT_TONE1_TUNING_GAIN));
    BK4819_EnableTXLink();

    SYSTEM_DelayMs(SELCALL_PRELOAD_MS);

    for (uint8_t i = 0; i < count; i++)
        BK4819_PlayToneRaw(sPendingTones[i], sPendingToneMs);    /* unmute, tone, mute */

    if (gEeprom.DTMF_SIDE_TONE) {
        AUDIO_AudioPathOff();
        gEnableSpeaker = false;
    }
    BK4819_SetAF(BK4819_AF_MUTE);
    BK4819_WriteRegister(BK4819_REG_70, 0x0000);
    BK4819_WriteRegister(BK4819_REG_30, 0xC1FE);               /* as BK4819_PlaySingleTone */
    BK4819_ExitTxMute();

    gSelCallEndTx = true;                  /* the main loop unkeys like a PTT release */
}
