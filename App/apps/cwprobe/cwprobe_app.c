/* CW Probe: throwaway bench app for the CW transceiver study (STUDY.md in
 * ../cwtrx). The PTT keys the transmitter as a straight key; every option the
 * study left open is a key, and the timings it needs are measured on screen.
 *
 * Keys: PTT key · 1 mode CARR/TONE · 2 sidetone S0..S3 · 3 shape HARD/RAMP
 *       4 hang time · 5 tone pitch · MENU clear measures · EXIT quit.
 *
 * Sidetone methods (local tone through the BK4819 AF_BEEP path):
 *   S0  none: AF muted. CARR = pure carrier, TONE = MCW keyed by TX mute.
 *   S1  AF_BEEP, tone keyed by TX mute (REG_50). In CARR the tone also
 *       modulates the carrier: the reference that a sidetone is heard at all.
 *   S2  AF_BEEP, TX mute held on, tone keyed by the REG_70 enable bit.
 *       Pure A1A if the speaker still hears the tone.
 *   S3  AF_BEEP, TX mute off, tone keyed by REG_70 with tuning gain 0.
 *       Pure A1A if gain 0 removes the deviation but not the local tone.
 * In TONE mode the RF tone is always keyed by TX mute; S1..S3 only switch
 * the local AF_BEEP monitor on.
 */

#include <stdint.h>
#include <stdbool.h>
#include "../app_api.h"

#ifndef APP_VERSION
#define APP_VERSION "?"
#endif

#define REG_36       0x36
#define REG_70       0x70
#define TONE1_EN     0x8000u
#define TX_GAIN      66u          /* BK4819_TransmitTone's TONE1 tuning gain */
#define AF_BEEP      3u
#define RAMP_STEPS   5u           /* 1 ms each: 5 ms rise and fall */
#define DEBOUNCE     3u           /* equal raw samples, 1 ms apart */
#define BOUNCE_WIN   20u          /* ms after an accepted edge still watched */
#define TOT_MS       10000u       /* key held this long: TX cut */
#define SETTLE_DB    3
#define SETTLE_MAX   1000u
#define DRAW_MS      250u

static const uint16_t HANG[] = { 200, 400, 800, 1500 };
static const uint32_t PLACE[] = { 100000000u, 10000000u, 1000000u, 100000u,
                                  10000u, 1000u, 100u, 10u, 1u };

static struct {
    const app_api_t *A;
    bool     running, carr, ramp, tx, down, raw, tot, denied, hold, settling, dirty;
    uint8_t  side, hang, pitch, stable, prevKey, bounces, bounceWin;
    uint8_t  bias;                 /* REG_36 bias read after tx_set_params */
    uint16_t reg36;
    uint16_t setupMs, endMs, settleMs, markMs, bounceMs, bounceSpan;
    int16_t  base16;               /* RX RSSI baseline, dBm x 16 (EMA) */
    int16_t  rssi;
    uint8_t  settleOk;
    uint32_t edgeAt, lastDraw, lastSample, endAt;
} g;
#define A (g.A)

/* ---- formatting, division-free ---- */
static char *put(char *o, const char *s) { while (*s) *o++ = *s++; return o; }
static char *putu(char *o, uint32_t v)
{
    bool lead = true;
    for (uint8_t i = 0; i < sizeof(PLACE) / 4u; i++) {
        uint8_t d = 0;
        while (v >= PLACE[i]) { v -= PLACE[i]; d++; }
        if (d || i == sizeof(PLACE) / 4u - 1u) lead = false;
        if (!lead) *o++ = (char)('0' + d);
    }
    return o;
}
static char *puti(char *o, int32_t v) { if (v < 0) { *o++ = '-'; v = -v; } return putu(o, (uint32_t)v); }
static char *putms(char *o, const char *label, uint32_t v)
{
    o = put(o, label); o = putu(o, v); return put(o, "ms ");
}

static uint16_t toneHz(void) { return (uint16_t)(500u + g.pitch * 100u); }

/* ---- drawing ---- */
static void line(char *s, char *e, uint8_t n) { *e = '\0'; A->print_normal(s, 0, 0, n); }

static void draw(void)
{
    char s[24], *o;
    A->display_clear();
    A->status_clear();
    A->print_inverse("CW PROBE " APP_VERSION, 2, 0, true, true, 60);
    A->draw_battery();

    o = put(s, g.carr ? "CARR S" : "TONE S"); *o++ = (char)('0' + g.side);
    o = put(o, g.ramp ? " RAMP" : " HARD");
    line(s, o, 0);
    o = put(s, "HANG "); o = putu(o, HANG[g.hang]); o = put(o, " TONE "); o = putu(o, toneHz());
    line(s, o, 1);
    o = putms(s, "UP ", g.setupMs); o = putms(o, "END ", g.endMs);
    line(s, o, 2);
    o = put(s, "SETTLE ");
    if (g.settling) o = put(o, "...");
    else if (g.settleMs >= SETTLE_MAX) o = put(o, ">1s");
    else { o = putu(o, g.settleMs); o = put(o, "ms"); }
    line(s, o, 3);
    o = putms(s, "MARK ", g.markMs);
    line(s, o, 4);
    o = put(s, "BOUNCE "); o = putu(o, g.bounceMs); *o++ = ' '; o = putu(o, g.bounceSpan); o = put(o, "ms");
    line(s, o, 5);
    if (g.tx) o = put(s, "TX");
    else { o = put(s, "RX "); o = puti(o, g.rssi); o = put(o, "dBm"); }
    if (g.tot) o = put(o, " TOT");
    if (g.denied) o = put(o, " TX DENIED");
    line(s, o, 6);
    /* TX frequency, 10 Hz units, as MHz with 5 decimals */
    { char *e = putu(s, A->tx_freq());
      e[1] = '\0';
      for (uint8_t k = 0; k < 5u; k++, e--) e[0] = e[-1];
      e[0] = '.';
      A->print_normal(s, 0, 127, 7); }

    A->blit_status();
    A->blit_full();
    g.dirty = false;
}

/* ---- keying ---- */
static void setBias(uint8_t b) { A->bk_write(REG_36, (uint16_t)((g.reg36 & 0x00FFu) | ((uint16_t)b << 8))); }

static void rampTo(bool up)
{
    for (uint8_t i = 1; i <= RAMP_STEPS; i++) {
        uint8_t k = up ? i : (uint8_t)(RAMP_STEPS - i);
        uint16_t b = 0;
        for (uint8_t j = 0; j < k; j++) b += g.bias;   /* bias * k / RAMP_STEPS */
        uint8_t q = 0; while (b >= RAMP_STEPS) { b -= RAMP_STEPS; q++; }
        setBias(q);
        A->delay_ms(1);
    }
}

static void toneKey(bool on)
{
    if (!g.carr || g.side <= 1u) { A->tx_mute(!on); return; }   /* TONE, S0, S1 */
    A->bk_write(REG_70, on ? (uint16_t)(TONE1_EN | ((g.side == 2u ? TX_GAIN : 0u) << 8)) : 0u);
}

static void keyOn(void)
{
    if (g.carr) {
        if (g.ramp) { setBias(0); A->tx_carrier(true); rampTo(true); }
        else A->tx_carrier(true);
        if (g.side) toneKey(true);
    } else
        toneKey(true);
}

static void keyOff(void)
{
    if (g.carr) {
        if (g.side) toneKey(false);
        if (g.ramp) rampTo(false);
        A->tx_carrier(false);
    } else
        toneKey(false);
}

static bool txStart(void)
{
    if (A->tx_state() != 0) return false;
    const uint32_t t0 = A->ticks_ms();
    A->tx_set_params();                 /* PA on */
    if (g.carr) A->tx_carrier(false);
    g.reg36 = A->bk_read(REG_36);
    g.bias = (uint8_t)(g.reg36 >> 8);
    A->tx_tone(toneHz());               /* 50 ms inside, ends unmuted */
    A->tx_mute(true);
    if (g.carr && g.side >= 2u) {       /* tone keyed by REG_70 */
        A->bk_write(REG_70, 0);
        if (g.side == 3u) A->tx_mute(false);
    }
    A->set_af(g.side ? AF_BEEP : APP_AF_MUTE);
    A->audio_path(g.side != 0);
    g.setupMs = (uint16_t)(A->ticks_ms() - t0);
    g.tx = true;
    g.tot = g.denied = false;
    g.settling = false;
    return true;
}

static void txStop(void)
{
    keyOff();
    A->tx_mute(true);
    A->bk_write(REG_36, g.reg36);
    const uint32_t t0 = A->ticks_ms();
    A->tx_end();
    A->set_af(APP_AF_MUTE);
    A->audio_path(false);
    g.endAt = A->ticks_ms();
    g.endMs = (uint16_t)(g.endAt - t0);
    g.tx = false;
    g.settling = true;
    g.settleOk = 0;
    g.dirty = true;
}

/* ---- PTT: raw sample every loop (~1 ms), debounced, bounce measured ----
 * The first raw change opens a BOUNCE_WIN window; every further change inside
 * it is a bounce, and the last one sets the span. */
static void ptt(bool raw, uint32_t now)
{
    if (raw != g.raw) {
        g.raw = raw;
        g.stable = 0;
        if (!g.bounceWin) { g.bounceWin = BOUNCE_WIN; g.bounces = 0; g.bounceSpan = 0; }
        else { g.bounces++; g.bounceSpan = (uint16_t)(BOUNCE_WIN - g.bounceWin); }
    } else if (g.stable < DEBOUNCE)
        g.stable++;
    if (g.bounceWin && --g.bounceWin == 0) { g.bounceMs = g.bounces; g.dirty = true; }

    if (g.down && g.tx && now - g.edgeAt >= TOT_MS) {   /* stuck key */
        g.tot = g.hold = true; g.down = false; txStop();
        return;
    }
    if (g.stable < DEBOUNCE) return;
    if (!raw) g.hold = false;
    if (raw == g.down || (raw && g.hold)) return;   /* after a TOT or a denial: release first */
    g.down = raw;
    if (raw) {
        if (!g.tx && !txStart()) { g.down = false; g.hold = g.denied = g.dirty = true; return; }
        g.edgeAt = A->ticks_ms();
        keyOn();
    } else if (g.tx) {
        keyOff();
        g.markMs = (uint16_t)(now - g.edgeAt);
        g.edgeAt = now;
        g.dirty = true;
    }
}

/* ---- RX: RSSI baseline, and settling time after TX ---- */
static void rx(uint32_t now)
{
    if (now - g.lastSample < 10u) return;
    g.lastSample = now;
    g.rssi = A->rssi_dbm();
    if (g.settling) {
        int16_t d = (int16_t)(g.rssi - (g.base16 >> 4));
        if (d < 0) d = (int16_t)-d;
        const uint32_t t = now - g.endAt;
        if (d <= SETTLE_DB) { if (++g.settleOk >= 3u) { g.settling = false; g.settleMs = (uint16_t)(t > 20u ? t - 20u : 0u); g.dirty = true; } }
        else g.settleOk = 0;
        if (t >= SETTLE_MAX) { g.settling = false; g.settleMs = SETTLE_MAX; g.dirty = true; }
        return;
    }
    if (!g.base16) g.base16 = (int16_t)(g.rssi * 16);
    else g.base16 = (int16_t)(g.base16 + g.rssi - (g.base16 >> 4));
}

static void keys(uint8_t key)
{
    if (key == g.prevKey) return;
    g.prevKey = key;
    switch (key) {
        case APP_KEY_EXIT: g.running = false; return;
        case APP_KEY_1: g.carr = !g.carr; break;
        case APP_KEY_2: if (++g.side > 3u) g.side = 0; break;
        case APP_KEY_3: g.ramp = !g.ramp; break;
        case APP_KEY_4: if (++g.hang >= sizeof(HANG) / 2u) g.hang = 0; break;
        case APP_KEY_5: if (++g.pitch > 4u) g.pitch = 0; break;
        case APP_KEY_MENU: g.setupMs = g.endMs = g.settleMs = g.markMs = g.bounceMs = g.bounceSpan = 0; break;
        default: return;
    }
    A->backlight_on();
    g.dirty = true;
}

__attribute__((section(".text.entry"), used))
void app_main(const app_api_t *api)
{
    A = api;
    g.running = true;
    g.carr = true;
    g.side = 1;
    g.hang = 1;
    g.pitch = 2;                        /* 700 Hz */
    g.prevKey = APP_KEY_INVALID;
    A->set_af(APP_AF_MUTE);
    A->audio_path(false);
    A->backlight_on();
    draw();

    while (g.running) {
        const uint32_t now = A->ticks_ms();
        const uint8_t key = A->get_key();
        ptt(key == APP_KEY_PTT, now);
        if (g.tx) {
            if (!g.down && now - g.edgeAt >= HANG[g.hang]) txStop();
        } else {
            rx(now);
            if (key != APP_KEY_PTT) keys(key);
        }
        /* never redraw mid-element: only key up, at least 150 ms into the gap */
        const bool quiet = !g.down && (!g.tx || now - g.edgeAt >= 150u);
        if (quiet && (g.dirty || (!g.tx && now - g.lastDraw >= DRAW_MS))) {
            g.lastDraw = now;
            draw();
            if (!g.tx) A->battery_sample();
        }
        A->backlight_update();
        A->delay_ms(1);
    }

    if (g.tx) txStop();
    A->tx_end();                        /* safety: PA off, RX restored */
    A->set_af(APP_AF_MUTE);
    A->audio_path(false);
}
