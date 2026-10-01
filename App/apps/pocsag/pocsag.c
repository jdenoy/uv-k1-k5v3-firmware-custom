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

/* POCSAG streaming decoder, see pocsag.h. */

#include "pocsag.h"

#define SYNC_CW   0x7CD215D8u
#define IDLE_CW   0x7A89C197u
#define BCH_POLY  0x769u     /* x^10+x^9+x^8+x^6+x^5+x^3+1 */
#define DC_SHIFT  9          /* plain DC pull: 512 samples (53 ms)              */
#define AMP_SHIFT 6          /* level estimate: 64 samples                     */
#define PLL_SHIFT 2          /* DPLL: move 1/4 of the phase error per transition */
#define NO_EDGE   (-32768)   /* no crossing seen near this boundary yet          */
#define PREAMBLE_MIN  24u    /* alternating bits that mark a preamble            */
#define PREAMBLE_HOLD 64u    /* bits the app keeps sampling after it             */
#define SYNC_TOL  2          /* bit errors accepted on the first sync          */
#define RESYNC_TOL 3         /* ... and on the sync of the following batches   */

enum { HUNT = 0, WORDS = 1, RESYNC = 2 };

_Static_assert((POC_HIST & (POC_HIST - 1u)) == 0u, "POC_HIST must be a power of 2");

/* Phase steps for 512 / 1200 / 2400 bps at 9.6 kHz: 65536 * rate / 9600. */
static const uint16_t INC[3] = { 3495u, 8192u, 16384u };

/* Rebuild corners off, 60, 250, 1000, 1500 Hz: 2 pi fc / 9600 in Q16 (1500 Hz
 * is close to the 16-bit limit). POC_CEDGE uses the edge latch instead, and
 * POC_CAUTO picks what the K1 needs: edge latch at 512 bps, 1000 Hz above
 * (README). */
static const uint16_t KC[POC_CEDGE] = { 0u, 2574u, 10723u, 42893u, 64340u };

/* ---- codeword checks ---- */
uint16_t poc_syndrome(uint32_t cw)
{
    uint32_t w = cw >> 1;                     /* 31-bit BCH codeword, parity dropped */
    for (int8_t i = 30; i >= 10; i--)
        if (w & (1u << i)) w ^= BCH_POLY << (i - 10);
    return (uint16_t)w;
}

static uint8_t parity(uint32_t v)
{
    v ^= v >> 16; v ^= v >> 8; v ^= v >> 4; v ^= v >> 2; v ^= v >> 1;
    return (uint8_t)(v & 1u);
}

int poc_fix(uint32_t *cw)
{
    uint16_t s = poc_syndrome(*cw);
    uint8_t  p = parity(*cw);
    if (!s) {
        if (!p) return 0;
        *cw ^= 1u;                            /* only the parity bit was wrong */
        return 1;
    }
    /* single error: s must be x^k mod g for the BCH bit k (codeword bit k+1) */
    uint16_t r = 1u;
    for (uint8_t k = 0; k < 31u; k++) {
        if (r == s) {
            if (!p) return -1;                /* even parity with a syndrome: 2+ errors */
            *cw ^= 1u << (k + 1u);
            return 1;
        }
        r <<= 1;
        if (r & 0x400u) r ^= BCH_POLY;
    }
    return -1;
}

static uint8_t dist(uint32_t x)               /* bit count, stops early above 4 */
{
    uint8_t n = 0;
    while (x && n < 5u) { x &= x - 1u; n++; }
    return n;
}

/* ---- message ring ---- */
const poc_msg_t *poc_get(const poc_t *d, uint8_t i)
{
    uint8_t n = d->count < POC_HIST ? d->count : (uint8_t)POC_HIST;
    if (i >= n) return 0;
    return &d->msg[(uint8_t)(d->cur - i) & (POC_HIST - 1u)];
}

static poc_msg_t *building(poc_t *d) { return &d->msg[(uint8_t)(d->cur + 1u) & (POC_HIST - 1u)]; }

static bool commit(poc_t *d)
{
    if (!d->open) return false;
    d->open = 0;
    d->cur = (uint8_t)(d->cur + 1u) & (POC_HIST - 1u);
    if (d->count < 255u) d->count++;
    return true;
}

static bool codeword(poc_t *d, uint32_t w)
{
    d->nCw++;
    int r = poc_fix(&w);
    if (r > 0) d->nFix++;
    if (r < 0) d->nBad++;

    if (r >= 0 && w == IDLE_CW) return commit(d);

    if (!(w & 0x80000000u)) {                 /* address codeword */
        bool done = commit(d);
        if (r < 0) return done;               /* address unreliable: drop the message */
        poc_msg_t *m = building(d);
        m->ric   = (((w >> 13) & 0x3FFFFu) << 3) | (uint32_t)(d->cw >> 1);
        m->func  = (uint8_t)((w >> 11) & 3u);
        m->nbits = 0;
        m->flags = r ? POC_F_FIXED : 0u;
        d->open  = 1;
        return done;
    }

    if (!d->open) return false;               /* message codeword without address */
    poc_msg_t *m = building(d);
    if (r > 0) m->flags |= POC_F_FIXED;
    if (r < 0) m->flags |= POC_F_BAD;
    uint32_t data = (w >> 11) & 0xFFFFFu;
    for (int8_t i = 19; i >= 0; i--) {
        uint16_t n = m->nbits;
        if (n >= POC_MAXBITS) { m->flags |= POC_F_TRUNC; break; }
        uint8_t mask = (uint8_t)(0x80u >> (n & 7u));
        if (data & (1u << i)) m->bits[n >> 3] |= mask;
        else                  m->bits[n >> 3] &= (uint8_t)~mask;
        m->nbits = (uint16_t)(n + 1u);
    }
    return false;
}

/* ---- framing: one demodulated bit ---- */
static bool bit(poc_t *d, uint8_t b)
{
    d->sr = (d->sr << 1) | b;
    switch (d->state) {
    case HUNT: {
        uint32_t x = d->sr ^ SYNC_CW;
        if (dist(x) <= SYNC_TOL)       d->inv = 0;
        else if (dist(~x) <= SYNC_TOL) d->inv = 1;
        else return false;
        d->nSync++;
        d->state = WORDS; d->nb = 0; d->cw = 0;
        return false; }
    case WORDS: {
        if (++d->nb < 32u) return false;
        d->nb = 0;
        bool done = codeword(d, d->inv ? ~d->sr : d->sr);
        if (++d->cw == 16u) d->state = RESYNC;
        return done; }
    default: {                                /* RESYNC */
        if (++d->nb < 32u) return false;
        d->nb = 0;
        uint32_t w = d->inv ? ~d->sr : d->sr;
        if (dist(w ^ SYNC_CW) <= RESYNC_TOL) { d->nSync++; d->cw = 0; d->state = WORDS; return false; }
        d->state = HUNT;
        return commit(d); }
    }
}

/* ---- demodulator ---- */
void poc_config(poc_t *d, uint8_t rate, uint8_t corner)
{
    if (rate > POC_2400) rate = POC_1200;
    if (corner >= POC_NCORNER) corner = POC_CAUTO;
    if (corner == POC_CAUTO) corner = rate == POC_512 ? POC_CEDGE : POC_C1000;
    d->inc  = INC[rate];
    d->edge = corner == POC_CEDGE;
    d->k    = d->edge ? 0u : KC[corner];
}

void poc_init(poc_t *d, uint8_t rate, uint8_t corner)
{
    uint8_t *p = (uint8_t *)d;
    for (uint16_t i = 0; i < sizeof(*d); i++) p[i] = 0;
    poc_config(d, rate, corner);
    d->cPrev = d->cNext = NO_EDGE;
    d->cur = POC_HIST - 1u;                   /* first message goes to slot 0 */
}

bool poc_push(poc_t *d, uint16_t sample)
{
    int32_t x = (int32_t)sample << 4;
    if (!d->primed) { d->base = x; d->primed = 1; }

    /* AC coupling in the audio path (a 1-pole high-pass) makes a run of equal
     * bits droop towards the centre: what it removed is a low-pass of the
     * original levels. Quantized feedback rebuilds it from the decided levels
     * (+amp / -amp) and adds it back. Its corner is set by poc_config: on
     * the K1 the audio path is close to a differentiator (about 1 kHz), see
     * README.md. At 512 bps the edge latch below is used instead. */
    int32_t v  = x - d->base;
    int32_t y;
    if (d->edge) {
        /* Edge latch (512 bps on the K1): each bit edge is a sharp pulse
         * followed by an opposite shelf of nearly the same area, so neither
         * integrating nor rebuilding the levels works. A pulse beyond half the
         * tracked peak sets the level to its sign; quiet samples feed the DC. */
        int32_t av = v < 0 ? -v : v;
        d->pk -= d->pk >> 8;
        if (av > d->pk) d->pk = av;
        if (v >  (d->pk >> 1)) d->lvl = 1;
        if (v < -(d->pk >> 1)) d->lvl = 0;
        if (av < (d->pk >> 2)) d->base += v >> DC_SHIFT;
        y = d->lvl ? 4096 : -4096;
    } else {
        y = v + d->w;
        int32_t ay = y < 0 ? -y : y;
        int32_t lag = (y >= 0 ? d->amp : -d->amp) - d->w;
        d->amp += (ay - d->amp) >> AMP_SHIFT;
        d->w += (d->k * lag) >> 16;
        d->base += y >> DC_SHIFT;                       /* slow plain DC pull */
    }
    uint8_t sg = y >= 0;

    /* Bit clock. A zero crossing is a candidate edge; the one closest to each
     * slot boundary is kept (first half of a slot: the boundary at its start,
     * second half: the one at its end). The loop only moves when the two bits
     * around a boundary differ: inside a run of equal bits, crossings are
     * ripple (the K1 audio carries strong 1-3 kHz content) and once dragged
     * the clock a whole bit off. The correction is applied at mid-slot, so it
     * can never push the phase across a boundary. */
    if (sg != d->sign) {
        d->sign = sg;
        int16_t e = (int16_t)d->ph;             /* >= 0: just after the boundary */
        int16_t *c = e >= 0 ? &d->cPrev : &d->cNext;
        if (*c == NO_EDGE || (e < 0 ? -e : e) < (*c < 0 ? -*c : *c)) *c = e;
    }
    d->acc += y;
    uint16_t op = d->ph;
    uint16_t np = (uint16_t)(op + d->inc);
    bool wrap = np < op;                      /* before the correction: it never crosses 0 */
    if (op < 0x8000u && np >= 0x8000u) {         /* mid-slot: apply the pending correction */
        int32_t e = d->pend;
        np = (uint16_t)(np - (e >= 0 ? e >> PLL_SHIFT : -((-e) >> PLL_SHIFT)));
        d->pend = 0;
    }
    d->ph = np;
    if (!wrap) return false;
    uint8_t b = d->acc < 0;                   /* logical 1 = lower frequency */
    d->acc = 0;
    if (b != d->lastBit && d->cPrev != NO_EDGE) d->pend = d->cPrev;
    /* Preamble detector for poc_busy(): 24 alternating bits in a row (1 in
     * 16 million on noise) mark a preamble; the app then keeps sampling for
     * PREAMBLE_HOLD more bits, which covers the sync word that follows. */
    if (b != d->lastBit) { if (d->alt < 255u) d->alt++; } else d->alt = 0;
    if (d->alt >= PREAMBLE_MIN) d->hold = PREAMBLE_HOLD;
    else if (d->hold) d->hold--;
    d->lastBit = b;
    d->cPrev = d->cNext;
    d->cNext = NO_EDGE;
    return bit(d, b);
}

bool poc_flush(poc_t *d)
{
    d->state = HUNT;
    d->sr = 0;
    return commit(d);
}

/* ---- text ---- */
uint8_t poc_text(const poc_msg_t *m, bool alpha, char *out)
{
    static const char NUM[] = "0123456789*U -)(";
    uint8_t  len = 0, w = alpha ? 7u : 4u, k = 0, c = 0;
    for (uint16_t n = 0; n < m->nbits; n++) {
        if (m->bits[n >> 3] & (0x80u >> (n & 7u))) c |= (uint8_t)(1u << k);
        if (++k < w) continue;
        if (!alpha)                               out[len++] = NUM[c];
        else if (c == 0u || c == 3u || c == 4u)   ;              /* NUL / ETX / EOT */
        else if (c == 10u || c == 13u)            out[len++] = ' ';
        else if (c < 0x20u || c == 0x7Fu)         out[len++] = '.';
        else                                      out[len++] = (char)c;
        k = 0; c = 0;
    }
    while (len && out[len - 1u] == ' ') len--;   /* numeric padding / trailing CR */
    out[len] = '\0';
    return len;
}
