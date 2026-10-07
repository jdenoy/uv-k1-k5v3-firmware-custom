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
 * KISS TNC, step 0: USB link test with APRSdroid (see README.md). No radio
 * yet: the app reads KISS from the USB serial port (API level 3), decodes the
 * frames APRSdroid sends, and echoes each data frame back, so APRSdroid shows
 * its own packet as received. Key 2 sends a test APRS frame to the phone.
 *
 * Screen: host state (the port opened with DTR or not), bytes and frames in,
 * frames out, KISS errors, the last frame's source and length, the last bytes
 * received in hex.
 *
 * Keys: 1 echo on/off · 2 send a test frame · EXIT quit.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "../app_api.h"

#define FEND        0xC0
#define FESC        0xDB
#define TFEND       0xDC
#define TFESC       0xDD

#define FRAME_MAX   340          /* AX.25: 2 + 8 x 7 address bytes + 256 info */
#define TICK_MS     10
#define DRAW_TICKS  10           /* redraw every 100 ms */
#define HEX_LEN     8            /* last bytes received, shown in hex */

struct globals {
    bool     echo, host, inFrame, esc, dirty, quit;
    uint8_t  prevKey, drawTick, hexHead;
    uint16_t len, lastLen;
    uint32_t bytesIn, framesIn, framesOut, errors;
    uint8_t  hex[HEX_LEN];
    char     src[10];            /* last source callsign, "CALL-SS" */
    const app_api_t *api;
    uint8_t *frame;              /* FRAME_MAX bytes, on app_main's stack */
};
static struct globals g;
#define A (g.api)

/* ---- formatting ---- */
static char *put(char *o, const char *s){ while(*s) *o++ = *s++; return o; }
static char *putu(char *o, uint32_t u){
    char t[10]; uint8_t n = 0;
    do { uint64_t qr = A->uidivmod(u, 10u); t[n++] = (char)('0' + (uint32_t)(qr >> 32)); u = (uint32_t)qr; } while(u);
    while(n) *o++ = t[--n];
    return o;
}
static char hexd(uint8_t v){ v &= 15u; return (char)(v < 10u ? '0' + v : 'A' - 10 + v); }

/* ---- KISS out ---- */
static uint8_t outBuf[64];
static uint8_t outLen;

static void outFlush(void){
    if(outLen){ g.host = A->serial_write(outBuf, outLen); outLen = 0; }
}
static void outByte(uint8_t b){
    if(outLen == sizeof(outBuf)) outFlush();
    outBuf[outLen++] = b;
}
/* One KISS data frame (port 0, command 0) with FEND / FESC escaped. */
static void kissSend(const uint8_t *f, uint16_t n){
    outByte(FEND); outByte(0x00);
    for(uint16_t i = 0; i < n; i++){
        uint8_t b = f[i];
        if(b == FEND){ outByte(FESC); outByte(TFEND); }
        else if(b == FESC){ outByte(FESC); outByte(TFESC); }
        else outByte(b);
    }
    outByte(FEND);
    outFlush();
    g.framesOut++;
}

/* AX.25 address field: 6 shifted characters, then the SSID byte. */
static uint8_t *ax25Addr(uint8_t *o, const char *call, uint8_t ssid, bool last){
    uint8_t i = 0;
    for(; i < 6 && call[i]; i++) *o++ = (uint8_t)(call[i] << 1);
    for(; i < 6; i++) *o++ = ' ' << 1;
    *o++ = (uint8_t)(0x60u | (ssid << 1) | (last ? 1u : 0u));
    return o;
}

/* A test APRS status frame from the boot-message callsign to the phone. */
static void sendTest(void){
    char call[8];
    A->boot_callsign(call, sizeof call);
    if(!call[0]) put(call, "NOCALL")[0] = '\0';
    uint8_t *o = ax25Addr(g.frame, "APZK1", 0, false);
    o = ax25Addr(o, call, 0, true);
    *o++ = 0x03; *o++ = 0xF0;                  /* UI frame, no layer 3 */
    o = (uint8_t *)put((char *)o, ">UV-K1 KISS TNC test");
    kissSend(g.frame, (uint16_t)(o - g.frame));
}

/* Source callsign of a frame for the screen: address 2, bytes 7 to 13. */
static void decodeSrc(void){
    char *o = g.src;
    if(g.len < 16){ put(o, "?")[0] = '\0'; return; }
    for(uint8_t i = 7; i < 13; i++){
        char c = (char)(g.frame[i] >> 1);
        if(c != ' ') *o++ = c;
    }
    uint8_t ssid = (g.frame[13] >> 1) & 15u;
    if(ssid){ *o++ = '-'; o = putu(o, ssid); }
    *o = '\0';
}

/* ---- KISS in ---- */
static void frameDone(void){
    /* first byte: port (high nibble) and command (low); 0 = data */
    if(g.len > 1 && (g.frame[0] & 0x0Fu) == 0u){
        g.framesIn++;
        g.len--;                                   /* drop the command byte */
        for(uint16_t i = 0; i < g.len; i++) g.frame[i] = g.frame[i + 1];
        g.lastLen = g.len;
        decodeSrc();
        if(g.echo) kissSend(g.frame, g.len);
    }
    g.len = 0;
}

static void kissByte(uint8_t b){
    g.bytesIn++;
    g.hex[g.hexHead] = b; g.hexHead = (uint8_t)((g.hexHead + 1u) % HEX_LEN);
    if(b == FEND){
        if(g.inFrame && g.len) frameDone();
        g.inFrame = true; g.esc = false; g.len = 0;
        return;
    }
    if(!g.inFrame) return;
    if(g.esc){
        g.esc = false;
        if(b == TFEND) b = FEND;
        else if(b == TFESC) b = FESC;
        else { g.errors++; g.inFrame = false; return; }
    } else if(b == FESC){ g.esc = true; return; }
    if(g.len >= FRAME_MAX){ g.errors++; g.inFrame = false; return; }
    g.frame[g.len++] = b;
}

static void pollSerial(void){
    uint8_t buf[32];
    uint16_t n;
    while((n = A->serial_read(buf, sizeof buf)) != 0){
        for(uint16_t i = 0; i < n; i++) kissByte(buf[i]);
        g.dirty = true;
    }
}

/* ---- screen ---- */
static void line(uint8_t row, const char *label, uint32_t v){
    char s[20]; *putu(put(s, label), v) = '\0';
    A->print_normal(s, 0, 0, row);
}

static void draw(void){
    char s[24], *o;
    A->display_clear();
    A->status_clear();
    A->print_inverse("KISS TNC", 2, 0, true, true, 34);
    A->print_inverse(g.host ? "USB OK" : "NO HOST", 40, 0, true, true, g.host ? 64 : 68);
    if(g.echo) A->print_inverse("ECHO", 74, 0, true, true, 92);
    A->draw_battery();

    line(0, "Bytes in ", g.bytesIn);
    o = putu(put(s, "Frames "), g.framesIn); o = put(o, " / "); *putu(o, g.framesOut) = '\0';
    A->print_normal(s, 0, 0, 1);
    line(2, "KISS errors ", g.errors);
    o = put(s, "Last "); o = put(o, g.src[0] ? g.src : "-"); o = put(o, " "); *putu(o, g.lastLen) = '\0';
    A->print_normal(s, 0, 0, 3);

    o = s;
    for(uint8_t i = 0; i < HEX_LEN; i++){
        uint8_t b = g.hex[(uint8_t)((g.hexHead + i) % HEX_LEN)];
        *o++ = hexd(b >> 4); *o++ = hexd(b);
    }
    *o = '\0';
    A->print_normal(s, 0, 0, 5);
    A->print_normal("1 echo  2 test", 0, 0, 6);
}

/* ---- keys ---- */
static void handleKeys(void){
    uint8_t key = A->get_key();
    if(key == APP_KEY_INVALID || key == g.prevKey){ g.prevKey = key; return; }
    g.prevKey = key;
    A->backlight_on();
    g.dirty = true;
    if(key == APP_KEY_EXIT) g.quit = true;
    else if(key == APP_KEY_1) g.echo = !g.echo;
    else if(key == APP_KEY_2) sendTest();
}

__attribute__((section(".text.entry"), used))
void app_main(const app_api_t *api){
    uint8_t frame[FRAME_MAX];   /* the frame buffer, on the stack: not in the overlay */
    A = api; g.frame = frame;
    g.echo = true; g.prevKey = APP_KEY_INVALID; g.dirty = true;
    g.host = A->serial_write(NULL, 0);     /* 0 bytes: only reports DTR */
    A->backlight_on();

    while(!g.quit){
        pollSerial();
        handleKeys();
        if(++g.drawTick >= DRAW_TICKS){
            g.drawTick = 0;
            g.host = A->serial_write(NULL, 0);
            g.dirty = true;
        }
        if(g.dirty){
            g.dirty = false;
            draw();
            A->blit_status();
            A->blit_full();
        }
        A->delay_ms(TICK_MS);
        A->backlight_update();
        A->battery_sample();
    }
}
