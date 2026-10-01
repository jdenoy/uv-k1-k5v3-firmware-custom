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

/* Selcall tone sequence builder: no hardware dependency (host-tested, see
 * tools/selcall/test_selcall_seq.c). */

#include "app/selcall.h"

const char *const gSelCallTypeNames[SELCALL_TYPE_COUNT] = {
    "OFF", "ZVEI-1", "ZVEI-2", "CCIR-1", "CCIR-2"
};

static const uint16_t ZVEI_DIGIT_HZ[10] = {
    2400, 1060, 1160, 1270, 1400, 1530, 1670, 1830, 2000, 2200
};

static const uint16_t CCIR_DIGIT_HZ[10] = {
    1981, 1124, 1197, 1275, 1358, 1446, 1540, 1640, 1747, 1860
};

typedef struct {
    const uint16_t *digitHz;
    uint16_t        repeatHz;
    uint16_t        toneMs;
} SelCallStd_t;

static const SelCallStd_t STANDARDS[SELCALL_TYPE_COUNT] = {
    [SELCALL_ZVEI1] = { ZVEI_DIGIT_HZ, 2600,  70 },
    [SELCALL_ZVEI2] = { ZVEI_DIGIT_HZ,  970,  70 },
    [SELCALL_CCIR1] = { CCIR_DIGIT_HZ, 2110, 100 },
    [SELCALL_CCIR2] = { CCIR_DIGIT_HZ, 2110,  70 },
};

uint16_t SELCALL_ToneMs(uint8_t type)
{
    return (type != SELCALL_OFF && type < SELCALL_TYPE_COUNT) ? STANDARDS[type].toneMs : 0;
}

uint8_t SELCALL_BuildTones(uint8_t type, uint32_t code, uint16_t *freqs)
{
    static const uint32_t P10[SELCALL_DIGITS] = { 10000u, 1000u, 100u, 10u, 1u };

    if (type == SELCALL_OFF || type >= SELCALL_TYPE_COUNT || code > SELCALL_CODE_MAX)
        return 0;

    const SelCallStd_t *std = &STANDARDS[type];
    uint16_t prev = 0;
    for (uint8_t i = 0; i < SELCALL_DIGITS; i++) {
        uint8_t digit = 0;
        while (code >= P10[i]) {            /* no division: the M0+ has none */
            code -= P10[i];
            digit++;
        }
        uint16_t f = std->digitHz[digit];
        if (f == prev)                      /* same tone twice: send the repeat tone */
            f = std->repeatHz;
        freqs[i] = f;
        prev = f;
    }
    return SELCALL_DIGITS;
}
