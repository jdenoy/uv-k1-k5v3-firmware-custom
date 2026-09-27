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
 * EPIRB 406 (RX only) - decodes first-generation Cospas-Sarsat 406 MHz beacon
 * messages on the radio. Tune the VFO (FM, wide; 406.031 MHz covers the
 * 406.025-406.040 channels, or a test generator's frequency), then launch.
 *
 * The receiver is switched to RAW (RX HPF300 / LPF3K / de-emphasis and AFC off)
 * and the RX audio is left on: it reaches PA4 (the voice DAC pin, unused while
 * voice is disabled), held at mid-scale by the MCU DAC (unbuffered) and sampled
 * on ADC channel 4 at 9.6 kHz, timed from
 * SysTick, from the moment RSSI shows a burst. Samples go straight into dec406
 * (see README.md); decoded messages are kept in a short history.
 *
 * Keys (UV-K5 and UV-K1): UP/DOWN browse history · MENU clear history · EXIT quit. The speaker plays the bursts: lower the volume.
 * The loader re-runs RADIO_SetupRegisters on exit; the app restores the ADC,
 * PA4, the DAC and its clock itself.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "../app_api.h"

#ifndef APP_VERSION
#define APP_VERSION "dev"   /* set by build.sh from APP_VER */
#endif
#define TITLE "EPIRB 406 v" APP_VERSION
#include "dec406.c"

/* ---- MCU registers (PY32F071, core and SysTick at 48 MHz, 10 ms period) ---- */
#define SYST_LOAD   (*(volatile uint32_t *)0xE000E014u)
#define SYST_VAL    (*(volatile uint32_t *)0xE000E018u)
#define ADC_SR      (*(volatile uint32_t *)0x40012400u)
#define ADC_CR2     (*(volatile uint32_t *)0x40012408u)
#define ADC_SMPR3   (*(volatile uint32_t *)0x40012414u)
#define ADC_SQR3    (*(volatile uint32_t *)0x40012438u)
#define ADC_DR      (*(volatile uint32_t *)0x40012450u)
#define GPIOA_MODER (*(volatile uint32_t *)0x50000000u)
#define DAC_CR      (*(volatile uint32_t *)0x40007400u)
#define DAC_SWTRIGR (*(volatile uint32_t *)0x40007404u)
#define DAC_DHR12R1 (*(volatile uint32_t *)0x40007408u)
#define RCC_APBENR1 (*(volatile uint32_t *)0x4002103Cu)
#define RCC_DACEN   (1u << 29)
/* DAC channel 1 on, output buffer off, software trigger (TSEL1 = 111) */
#define DAC_CR_BIAS ((1u << 0) | (1u << 1) | (1u << 2) | (7u << 3))
#define BIAS_CODE   2048u
#define ADC_SR_EOC     (1u << 1)
#define ADC_CR2_START  ((1u << 22) | (1u << 20))   /* SWSTART | EXTTRIG */
#define SMP8_POS       24u
#define SMP4_POS       12u
#define ADC_CH_PA4     4u
#define CYC_PER_SAMPLE (48000000u / DEC406_FS)     /* 5000 */
#define CAP_MAX_CYC    (48000000u / 1000u * 900u)  /* 900 ms after trigger */

/* ---- BK4829 RAW receive ---- */
#define REG_2B      0x2B
#define REG_73      0x73

#define TRIG_DB     10
#define REARM_DB    5
#define COOL_MS     1500
#define TICK_MS     50
#define HIST        2

typedef struct { dec406_info_t in; int16_t rssi; uint8_t seq, inv; } entry_t;

static const app_api_t *A;
static dec406_t d;
static entry_t  hist[HIST];
static uint8_t  nHist, view, seq, prevKey, lastErr;
static uint16_t nSync, nFail;
static uint32_t savedSqr3, savedSmpr3, savedModer, savedDac, savedRcc, savedDhr;
static int16_t  rssi, burstRssi;
static int32_t  floorQ;                 /* noise floor, dBm x64 */
static bool     running;
static uint32_t tPrev, tCyc;
static char     str[34];

/* ---- formatting ---- */
static uint8_t slen(const char *s){ uint8_t n=0; while(s[n])n++; return n; }
static char *put(char *o,const char *s){ while(*s)*o++=*s++; return o; }
/* Formatting by repeated subtraction: no division, so no __udivsi3 in the
 * 4 KiB overlay. Values printed stay below 100000. */
static uint8_t sub(uint32_t *v,uint32_t d){ uint8_t q=0; while(*v>=d){ *v-=d; q++; } return q; }
static char *puti(char *o,int32_t v){
    static const uint16_t P10[]={10000u,1000u,100u,10u,1u};
    uint32_t u;
    if(v<0){*o++='-';u=(uint32_t)(-v);} else u=(uint32_t)v;
    bool lead=false;
    for(uint8_t i=0;i<5;i++){
        uint8_t c=sub(&u,P10[i]);
        if(c||lead||i==4u){ *o++=(char)('0'+c); lead=true; }
    }
    return o;
}
/* arc seconds -> "49.27111" (5 decimals, truncated) followed by the hemisphere */
static char *putPos(char *o,int32_t s,char pos,char neg){
    uint32_t a=(uint32_t)(s<0?-s:s);
    o=puti(o,sub(&a,3600u)); *o++='.';
    for(uint8_t k=0;k<5;k++){ a*=10u; *o++=(char)('0'+sub(&a,3600u)); }
    *o++= s<0?neg:pos;
    return o;
}
static void tiny(uint8_t x,uint8_t y,char *end){ *end='\0'; A->print_tiny(str,x,y,false,true); }
static void line(uint8_t l,char *end){ *end='\0'; A->print_normal(str,0,0,l); }

/* ---- SysTick cycle counter (call at least every 10 ms) ---- */
static void clkStart(void){ tPrev=SYST_VAL; tCyc=0; }
static uint32_t clkCyc(void){
    uint32_t v=SYST_VAL;
    tCyc += (v<=tPrev) ? tPrev-v : tPrev+(SYST_LOAD+1u-v);
    tPrev=v;
    return tCyc;
}

/* ---- PA4 bias: the pin has no DC reference of its own (AC-coupled audio), so
 * 9.6 kHz sampling dragged it to 0 V and clipped half the message (406 Lab scope).
 * The MCU DAC on PA4, output buffer off, at mid-scale, holds it at ~VDD/2.
 * Set for the whole run, restored on exit. ---- */
static void biasOn(void){
    GPIOA_MODER=savedModer|(3u<<8);           /* PA4 analog                     */
    RCC_APBENR1=savedRcc|RCC_DACEN;
    DAC_CR=DAC_CR_BIAS;
    DAC_DHR12R1=BIAS_CODE;
    DAC_SWTRIGR=1u;                           /* DHR -> DOR                     */
}
static void biasOff(void){ DAC_CR=savedDac; DAC_DHR12R1=savedDhr; RCC_APBENR1=savedRcc; GPIOA_MODER=savedModer; }

/* ---- ADC on PA4 (channel 4); channel 8 (battery) restored afterwards ---- */
static void adcSelPA4(void){
    ADC_SMPR3=(savedSmpr3&~(7u<<SMP4_POS))|(((savedSmpr3>>SMP8_POS)&7u)<<SMP4_POS);
    ADC_SQR3=(savedSqr3&~0x1Fu)|ADC_CH_PA4;
}
static void adcRestore(void){ ADC_SQR3=savedSqr3; ADC_SMPR3=savedSmpr3; }
static uint16_t adcRead(void){
    ADC_CR2|=ADC_CR2_START;
    for(uint16_t g=0; !(ADC_SR&ADC_SR_EOC) && g<2000u; g++){}
    return (uint16_t)(ADC_DR&0x0FFFu);
}

/* ---- one burst: sample PA4 at 9.6 kHz into the decoder ---- */
static void capture(void){
    burstRssi=rssi;
    dec406_init(&d,true);        /* INT: integrate the discriminator pulses */
    adcSelPA4();
    clkStart();
    uint32_t next=0;
    bool done=false;
    while(next<CAP_MAX_CYC){
        while(clkCyc()<next){}
        if(dec406_push(&d,adcRead())){ done=true; break; }
        next+=CYC_PER_SAMPLE;
    }
    adcRestore();

    if(!done){ nFail++; lastErr = d.state==DEC406_DATA ? 2u : 1u; return; }  /* 1 no sync, 2 cut */
    nSync++; lastErr=0;
    for(uint8_t i=HIST-1u;i>0;i--) hist[i]=hist[i-1u];
    dec406_parse(&d,&hist[0].in);
    hist[0].rssi=burstRssi; hist[0].seq=++seq; hist[0].inv=d.inv;
    if(nHist<HIST) nHist++;
    view=0;
}

/* ---- display ---- */
static void draw(void){
    char *o;
    A->display_clear();
    A->status_clear();
    A->print_inverse(TITLE,2,0,true,true,(uint8_t)(2u+(sizeof(TITLE)-1u)*4u));
    A->draw_battery();

    if(!nHist){
        o=put(str,"Waiting..."); line(1,o);
    } else {
        const entry_t *e=&hist[view];
        const dec406_info_t *in=&e->in;
        o=put(str,in->id); line(0,o);                                   /* 15-hex ID */
        o=puti(str,in->country); *o++=' '; o=put(o,dec406_proto_name(in)); line(1,o);
        if(in->hasPos){ o=putPos(str,in->latS,'N','S'); *o++=' '; o=putPos(o,in->lonS,'E','W'); }
        else o=put(str,"no position");
        line(2,o);

        /* raw end of the last received frame (bits 105-144), for bench diagnosis */
        o=put(str,"END ");
        for(uint8_t i=10;i<15;i++){ *o++=HX[d.bits[i]>>4]; *o++=HX[d.bits[i]&15u]; }
        tiny(0,26,o);

        o=put(str,in->selftest?"SELF-TEST ":"");
        o=put(o,in->longMsg?"LONG":"SHORT");
        o=put(o," BCH "); o=put(o,in->bch1?"OK":"ERR"); *o++='/'; o=put(o,in->bch2?"OK":"ERR");
        tiny(0,33,o);
        o=put(str,"#"); o=puti(o,e->seq); o=put(o," "); o=puti(o,e->rssi); o=put(o,"dBm");
        if(in->longMsg){ o=put(o,in->internalPos?" int":" ext"); if(in->homing) o=put(o," 121.5"); }
        if(in->hasPos && !in->hasFine) o=put(o," coarse");
        if(in->idRaw) o=put(o," rawID");
        tiny(0,40,o);
    }

    o=puti(str,rssi); *o++='/'; o=puti(o,floorQ/64); o=put(o,"dBm ok"); o=puti(o,nSync);
    o=put(o," err"); o=puti(o,nFail);
    if(lastErr) o=put(o,lastErr==1u?" nosync":" cut");
    tiny(0,48,o);
    if(nHist){ o=puti(str,view+1u); *o++='/'; o=puti(o,nHist); tiny((uint8_t)(127u-slen(str)*4u),33,o); }
}

/* ---- input ---- */
static void handleKeys(void){
    uint8_t key=A->get_key();
    if(key==APP_KEY_INVALID||key==prevKey){ prevKey=key; return; }
    prevKey=key;
    A->backlight_on();
    switch(key){
        case APP_KEY_EXIT: running=false; break;
        case APP_KEY_MENU: nHist=0; view=0; nSync=nFail=0; lastErr=0; break;
        case APP_KEY_UP:
        case APP_KEY_DOWN:
            if(nHist){ if(A->nav_dir(key)>0) view=view?(uint8_t)(view-1u):(uint8_t)(nHist-1u); else view=(uint8_t)(view+1u<nHist?view+1u:0u); }
            break;
        default: break;
    }
}

static void cooldown(void){
    for(uint16_t ms=0; ms<COOL_MS; ms+=10){
        if(A->rssi_dbm() < floorQ/64+REARM_DB) break;
        A->delay_ms(10);
    }
}

__attribute__((section(".text.entry"),used))
void app_main(const app_api_t *api){
    A=api;
    nHist=view=seq=0; nSync=nFail=0; lastErr=0; prevKey=APP_KEY_INVALID;

    savedSqr3=ADC_SQR3; savedSmpr3=ADC_SMPR3; savedModer=GPIOA_MODER; savedDac=DAC_CR;
    savedRcc=RCC_APBENR1; savedDhr=DAC_DHR12R1;
    biasOn();
    A->backlight_on();
    A->bk_write(REG_2B,(uint16_t)((A->bk_read(REG_2B)|0x0700u)&~0x0007u));
    A->bk_write(REG_73,(uint16_t)(A->bk_read(REG_73)|0x0010u));
    A->audio_path(true);
    A->set_af(APP_AF_FM);
    A->delay_ms(50);

    int32_t f=0;
    for(uint8_t i=0;i<16;i++){ f+=A->rssi_dbm(); A->delay_ms(5); }
    floorQ=f*4;

    running=true;
    while(running){
        for(uint8_t i=0;i<TICK_MS;i++){
            rssi=A->rssi_dbm();
            if(rssi>=floorQ/64+TRIG_DB){
                capture();
                draw(); A->blit_status(); A->blit_full();
                cooldown();
                break;
            }
            floorQ+=((int32_t)rssi*64-floorQ)/64;
            A->delay_ms(1);
        }
        handleKeys();
        if(!running) break;
        draw();
        A->blit_status();
        A->blit_full();
        A->battery_sample();
        A->backlight_update();
    }

    adcRestore();
    biasOff();
    A->set_af(APP_AF_MUTE);
    A->audio_path(false);
}
