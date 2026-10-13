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
 * ToneCall tone sequence builder: sequential tone calls (ZVEI-1, ZVEI-2,
 * CCIR-1, CCIR-2), no hardware dependency (host test in test/).
 *
 * Tones, no gap between them; a digit equal to the previous tone is replaced by
 * the repeat tone:
 *            0    1    2    3    4    5    6    7    8    9   repeat  tone
 *   ZVEI  2400 1060 1160 1270 1400 1530 1670 1830 2000 2200           70 ms
 *     ZVEI-1 repeat 2600 Hz, ZVEI-2 repeat 970 Hz
 *   CCIR  1981 1124 1197 1275 1358 1446 1540 1640 1747 1860   2110
 *     CCIR-1 100 ms, CCIR-2 70 ms
 */

#ifndef TONECALL_SEQ_H
#define TONECALL_SEQ_H

#include <stdint.h>

enum {
    TC_ZVEI1 = 0,
    TC_ZVEI2,
    TC_CCIR1,
    TC_CCIR2,
    TC_TYPE_COUNT
};

#define TC_MAX_DIGITS   8u        /* longest code accepted by TC_BuildTones */

/* Display names of the types ("ZVEI-1", ...). */
extern const char *const tcTypeNames[TC_TYPE_COUNT];

/* Tone frequencies (Hz) for a code given as a NUL-terminated digit string
 * ("01234" keeps its leading zero), repeat tone applied. Returns the number of
 * tones, or 0 if the type is invalid, the code is empty, too long or holds a
 * non-digit. */
uint8_t  TC_BuildTones(uint8_t type, const char *digits, uint16_t *freqs);

/* Tone duration (ms) of a type, 0 if invalid. */
uint16_t TC_ToneMs(uint8_t type);

#endif /* TONECALL_SEQ_H */
