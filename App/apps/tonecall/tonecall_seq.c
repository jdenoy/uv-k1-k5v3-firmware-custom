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

#include "tonecall_seq.h"

const char *const tcTypeNames[TC_TYPE_COUNT] = {
    "ZVEI-1", "ZVEI-2", "CCIR-1", "CCIR-2"
};

static const uint16_t ZVEI_DIGIT_HZ[10] = {
    2400, 1060, 1160, 1270, 1400, 1530, 1670, 1830, 2000, 2200
};

static const uint16_t CCIR_DIGIT_HZ[10] = {
    1981, 1124, 1197, 1275, 1358, 1446, 1540, 1640, 1747, 1860
};

/* Per type: the digit table follows from the type (ZVEI for ZVEI-1/2, CCIR for
 * CCIR-1/2), the repeat tone and the tone duration do not. */
typedef struct {
    uint16_t repeatHz;
    uint16_t toneMs;
} TcStd_t;

static const TcStd_t STANDARDS[TC_TYPE_COUNT] = {
    [TC_ZVEI1] = { 2600,  70 },
    [TC_ZVEI2] = {  970,  70 },
    [TC_CCIR1] = { 2110, 100 },
    [TC_CCIR2] = { 2110,  70 },
};

uint16_t TC_ToneMs(uint8_t type)
{
    return type < TC_TYPE_COUNT ? STANDARDS[type].toneMs : 0;
}

uint8_t TC_BuildTones(uint8_t type, const char *digits, uint16_t *freqs)
{
    if (type >= TC_TYPE_COUNT)
        return 0;

    const uint16_t *digitHz = type >= TC_CCIR1 ? CCIR_DIGIT_HZ : ZVEI_DIGIT_HZ;
    uint8_t n = 0;
    for (; digits[n]; n++) {
        const uint8_t d = (uint8_t)(digits[n] - '0');
        if (d > 9 || n >= TC_MAX_DIGITS)
            return 0;
        freqs[n] = digitHz[d];
        if (n && freqs[n] == freqs[n - 1])     /* same tone twice: send the repeat tone */
            freqs[n] = STANDARDS[type].repeatHz;
    }
    return n;
}
