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
 * pocsag: streaming POCSAG (ITU-R M.584) decoder fed with FM-discriminator
 * audio samples (the BK4829 RAW RX audio as read on PA4 at 9.6 kHz).
 * Freestanding C (no libc, no division), shared by the overlay app and the host
 * test harness.
 *
 * Signal: 512, 1200 or 2400 bps NRZ 2-FSK (+/-4.5 kHz), so the discriminator
 * output is the bit stream itself. Quantized feedback (corner set at
 * init) undoes the droop the audio AC coupling puts on runs of equal bits, each bit is
 * integrated over its slot, and a DPLL on the zero crossings keeps the slots on
 * the bit boundaries. The sync codeword 0x7CD215D8 is searched in both
 * polarities (the receive chain may invert), then each batch of 16 codewords is
 * checked with BCH(31,21) + even parity (one bit corrected per codeword) and
 * assembled into messages: RIC (21 bits: 18 from the address codeword, 3 from
 * its frame position), function bits, and the raw message bits, decoded as
 * numeric or alphanumeric only when displayed.
 */

#ifndef POCSAG_H
#define POCSAG_H

#include <stdint.h>
#include <stdbool.h>

#define POC_FS       9600u   /* sample rate expected by the decoder (Hz)          */
#define POC_MAXBITS  560u    /* message bits kept: 80 alphanumeric characters      */
#define POC_HIST     4u      /* messages kept (ring), the newest is shown          */

enum { POC_512 = 0, POC_1200 = 1, POC_2400 = 2 };

/* AC-coupling corner the decoder compensates: off, 30, 60, 100, 150, 250 Hz. */
enum { POC_COFF = 0, POC_C30, POC_C60, POC_C100, POC_C150, POC_C250, POC_NCORNER };

enum {                       /* poc_msg_t.flags                                   */
    POC_F_FIXED = 1u << 0,   /* at least one codeword needed a bit corrected       */
    POC_F_BAD   = 1u << 1,   /* at least one message codeword was uncorrectable   */
    POC_F_TRUNC = 1u << 2,   /* longer than POC_MAXBITS, the end is cut            */
};

typedef struct {
    uint32_t ric;            /* 21-bit receiver identity code                     */
    uint16_t nbits;          /* message bits stored (0 = tone only)                */
    uint8_t  func;           /* function bits 0..3                                 */
    uint8_t  flags;          /* POC_F_*                                            */
    uint8_t  bits[POC_MAXBITS / 8u]; /* message bits in transmit order, MSB first  */
} poc_msg_t;

typedef struct {
    /* demodulator */
    int32_t  base;           /* DC estimate, x16                                  */
    int32_t  amp;            /* signal level (mean |sample - centre|), x16        */
    int32_t  w;              /* rebuilt low-frequency part lost to AC coupling    */
    int32_t  k;              /* its corner, 2 pi fc / fs in Q16                    */
    int32_t  acc;            /* sum of centred samples over the current bit       */
    uint16_t ph;             /* bit phase, wraps at the bit boundary              */
    uint16_t inc;            /* phase step per sample                              */
    uint8_t  sign;           /* sign of the previous centred sample                */
    uint8_t  primed;         /* dcQ initialised                                    */
    /* framing */
    uint32_t sr;             /* last 32 bits, newest in bit 0                      */
    uint8_t  state;          /* 0 hunt, 1 codewords, 2 expecting the next sync     */
    uint8_t  nb;             /* bits received in the current codeword              */
    uint8_t  cw;             /* codeword index in the batch, 0..15                 */
    uint8_t  inv;            /* 1 = receive chain inverts                          */
    /* messages */
    uint8_t  open;           /* a message is being assembled in msg[cur]           */
    uint8_t  cur;            /* ring slot being assembled / last committed         */
    uint8_t  count;          /* messages committed (saturates at 255)              */
    uint16_t nSync, nCw, nFix, nBad;   /* statistics                              */
    poc_msg_t msg[POC_HIST];
} poc_t;

/* Reset everything (messages and statistics included) for a bit rate POC_512..
 * POC_2400 and an AC-coupling corner POC_COFF..POC_C250. */
void poc_init(poc_t *d, uint8_t rate, uint8_t corner);

/* Change the bit rate / corner, keeping the messages (takes effect at once;
 * call poc_flush first if a transmission is in progress). */
void poc_config(poc_t *d, uint8_t rate, uint8_t corner);

/* Feed one sample (any unsigned ADC scale). Returns true when a message has just
 * been committed: it is poc_last(d). */
bool poc_push(poc_t *d, uint16_t sample);

/* End of transmission: commit the message being assembled, if any, and go back
 * to sync hunt. Returns true when a message was committed. */
bool poc_flush(poc_t *d);

/* i-th most recent committed message (0 = newest), or 0 if there is none. */
const poc_msg_t *poc_get(const poc_t *d, uint8_t i);
static inline const poc_msg_t *poc_last(const poc_t *d) { return poc_get(d, 0); }

/* Text of a message into out (size >= POC_MAXBITS / 4 + 1): alpha = 7-bit
 * characters, else numeric BCD. Non-printable characters become '.'.
 * Returns the length. */
uint8_t poc_text(const poc_msg_t *m, bool alpha, char *out);

/* Bit 31..0 codeword helpers, exposed for the tests. */
uint16_t poc_syndrome(uint32_t cw);
int      poc_fix(uint32_t *cw);   /* 0 clean, 1 one bit corrected, -1 uncorrectable */

#endif
