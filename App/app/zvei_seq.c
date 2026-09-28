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

/* ZVEI tone sequence builder: no hardware dependency (host-tested, see
 * tools/zvei/test_zvei_seq.c). */

#include "app/zvei.h"

static const uint16_t ZVEI_DIGIT_HZ[10] = {
    2400, 1060, 1160, 1270, 1400, 1530, 1670, 1830, 2000, 2200
};

static const uint16_t ZVEI_REPEAT_HZ[ZVEI_TYPE_COUNT] = { 0, 2600, 970 };

uint8_t ZVEI_BuildTones(uint8_t type, uint32_t code, uint16_t *freqs)
{
    static const uint32_t P10[ZVEI_DIGITS] = { 10000u, 1000u, 100u, 10u, 1u };

    if (type == ZVEI_OFF || type >= ZVEI_TYPE_COUNT || code > ZVEI_CODE_MAX)
        return 0;

    uint16_t prev = 0;
    for (uint8_t i = 0; i < ZVEI_DIGITS; i++) {
        uint8_t digit = 0;
        while (code >= P10[i]) {        /* no division: the M0+ has none */
            code -= P10[i];
            digit++;
        }
        uint16_t f = ZVEI_DIGIT_HZ[digit];
        if (f == prev)                  /* same tone twice: send the repeat tone */
            f = ZVEI_REPEAT_HZ[type];
        freqs[i] = f;
        prev = f;
    }
    return ZVEI_DIGITS;
}
