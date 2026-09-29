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

/*
 * ZVEI selective calling (5-tone), transmit only, sent on demand.
 *
 * Each memory channel (and each VFO) can hold a ZVEI type (off / ZVEI-1 /
 * ZVEI-2) and two 5-digit codes, e.g. the "test" and "production" codes that
 * open a repeater's logic. The key actions "ZVEI 1" / "ZVEI 2" key up with the
 * channel's normal TX settings (CTCSS included), wait ZVEI_PRELOAD_MS, send the
 * code and unkey. Nothing is sent automatically.
 *
 * Tones (70 ms each, no gap): 0 2400, 1 1060, 2 1160, 3 1270, 4 1400, 5 1530,
 * 6 1670, 7 1830, 8 2000, 9 2200 Hz; a digit equal to the previous tone is
 * replaced by the repeat tone: 2600 Hz (ZVEI-1) or 970 Hz (ZVEI-2).
 *
 * Storage: one 8-byte record per channel in each config bank, outside the
 * EEPROM-compatible map (never written by CHIRP):
 *   ZVEI_BASE + index * 8, index = MR channel 0..1023, then 1024 + band * 2 + vfo
 *   [0] type  [1..3] code 1 (24-bit LE)  [4..6] code 2  [7] reserved
 * Erased flash (0xFF) reads as type off and no code.
 *
 * Serial access (CHIRP): EEPROM-compatible addresses ZVEI_EEPROM_BASE (0xD000)
 * to 0xF06F map to this table in the active bank (driver/eeprom_compat.c).
 */

#ifndef APP_ZVEI_H
#define APP_ZVEI_H

#include <stdbool.h>
#include <stdint.h>

enum {
    ZVEI_OFF = 0,
    ZVEI_TYPE_1,
    ZVEI_TYPE_2,
    ZVEI_TYPE_COUNT
};

#define ZVEI_DIGITS       5
#define ZVEI_CODE_MAX     99999u
#define ZVEI_CODE_NONE    100000u     /* menu value and API value for "no code" */
#define ZVEI_TONE_MS      70u
#define ZVEI_PRELOAD_MS   300u        /* carrier (+ CTCSS) before the tones */

/* Record table: physical address in each config bank, and the window of the
 * EEPROM-compatible map through which the serial link (CHIRP) reaches it. */
#define ZVEI_BASE         0x00B000u   /* physical, 3 sectors: 0xB000-0xDFFF   */
#define ZVEI_REC_SIZE     8u
#define ZVEI_RECORDS      (1024u + 14u)   /* MR channels, then 7 bands x 2 VFOs */
#define ZVEI_EEPROM_BASE  0xD000u     /* logical window 0xD000-0xF06F          */
#define ZVEI_EEPROM_END   (ZVEI_EEPROM_BASE + ZVEI_RECORDS * ZVEI_REC_SIZE)

typedef struct {
    uint8_t  type;                    /* ZVEI_OFF / ZVEI_TYPE_1 / ZVEI_TYPE_2 */
    uint32_t code[2];                 /* 0..99999, or ZVEI_CODE_NONE          */
} ZVEI_Channel_t;

/* Pure: tone frequencies (Hz) for a code, repeat tone applied. Returns the
 * number of tones (ZVEI_DIGITS), or 0 if type or code is invalid. */
uint8_t ZVEI_BuildTones(uint8_t type, uint32_t code, uint16_t *freqs);

#ifdef ENABLE_FEAT_ZVEI
/* Per-channel storage (channel = CHANNEL_SAVE, vfo = 0/1 for VFO records). */
void ZVEI_Load(uint16_t channel, uint8_t vfo, ZVEI_Channel_t *out);
void ZVEI_Save(uint16_t channel, uint8_t vfo, const ZVEI_Channel_t *in);
void ZVEI_Clear(uint16_t channel);
void ZVEI_Copy(uint16_t fromChannel, uint8_t fromVfo, uint16_t toChannel);
void ZVEI_EraseAll(void);

/* On-demand transmission. */
extern bool gZveiTx;                  /* a ZVEI burst is on air               */
extern bool gZveiEndTx;               /* burst sent: main loop must unkey     */
bool ZVEI_Request(uint8_t which);     /* which = 0 (code 1) or 1 (code 2)     */
bool ZVEI_Pending(void);
void ZVEI_Cancel(void);               /* TX refused: drop the pending burst   */
void ZVEI_Transmit(void);             /* from FUNCTION_Transmit, blocking     */
#endif

#endif /* APP_ZVEI_H */
