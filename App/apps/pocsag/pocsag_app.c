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
 * POCSAG (RX only) - pager message decoder. Tune the VFO (FM, wide) to a
 * POCSAG channel, e.g. DAPNET 439.9875 MHz (1200 bps), then launch.
 *
 * Same signal path as EPIRB 406: the receiver is switched to RAW (RX HPF300 /
 * LPF3K / de-emphasis and AFC off), the RX audio reaches PA4 (the voice DAC
 * pin, unused while voice is disabled), held at mid-scale by the MCU DAC
 * (unbuffered) and sampled on ADC channel 4 at 9.6 kHz, timed from SysTick,
 * continuously (no squelch: the sync word is the detector). Samples go
 * straight into the pocsag decoder (see README.md). Every decoded message
 * beeps; the last 4 stay in memory.
 *
 * Keys (UV-K5 and UV-K1): UP/DOWN browse the messages (newest first)
 *   1 bit rate 512/1200/2400 · 2 AC-coupling corner · 3 text AUTO/ALPHA/NUM
 *   4 beep on/off · MENU clear · EXIT quit (keys are read between
 *   transmissions, not during one).
 * The speaker plays the transmissions: lower the volume. Settings are saved.
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
#define TITLE "POCSAG v" APP_VERSION
#include "pocsag.c"

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
#define CYC_PER_SAMPLE (48000000u / POC_FS)        /* 5000 */
#define CAP_MAX_CYC    (48000000u * 10u)           /* 10 s on a continuous transmission */

/* ---- BK4829 RAW receive ---- */
#define REG_2B      0x2B
#define REG_73      0x73

#define BEEP_HZ     1750
#define BEEP_MS     80
#define CFG_MAGIC   0xB6   /* v1.2: new corner table, old settings dropped */
#define ROWS        5      /* text rows, 32 characters each               */

enum { MODE_AUTO = 0, MODE_ALPHA, MODE_NUM, MODE_COUNT };

static const app_api_t *A;
static poc_t    d;
static uint8_t  rate = POC_1200, corner = POC_C1000, mode = MODE_AUTO, beepOn = 1;
static uint8_t  view, prevKey;
static uint16_t total;                 /* messages decoded since launch / clear */
static uint32_t savedSqr3, savedSmpr3, savedModer, savedDac, savedRcc, savedDhr;
static int16_t  rssi;
static bool     running, saver;
static uint32_t tPrev, tCyc;
static char     str[34];

/* ---- formatting (no division: no __udivsi3 in the 4 KiB overlay) ---- */
static char *put(char *o,const char *s){ while(*s)*o++=*s++; return o; }
static uint8_t sub(uint32_t *v,uint32_t d){ uint8_t q=0; while(*v>=d){ *v-=d; q++; } return q; }
static char *puti(char *o,int32_t v){
    static const uint32_t P10[]={1000000u,100000u,10000u,1000u,100u,10u,1u};
    uint32_t u;
    if(v<0){*o++='-';u=(uint32_t)(-v);} else u=(uint32_t)v;
    bool lead=false;
    for(uint8_t i=0;i<7;i++){
        uint8_t c=sub(&u,P10[i]);
        if(c||lead||i==6u){ *o++=(char)('0'+c); lead=true; }
    }
    return o;
}
static void tiny(uint8_t x,uint8_t y,char *end){ *end='\0'; A->print_tiny(str,x,y,false,true); }

/* ---- SysTick cycle counter (call at least every 10 ms) ---- */
static void clkStart(void){ tPrev=SYST_VAL; tCyc=0; }
static uint32_t clkCyc(void){
    uint32_t v=SYST_VAL;
    tCyc += (v<=tPrev) ? tPrev-v : tPrev+(SYST_LOAD+1u-v);
    tPrev=v;
    return tCyc;
}

/* ---- PA4 bias: AC-coupled audio with no DC reference; the unbuffered MCU DAC
 * holds it at mid-scale (see the EPIRB 406 README). Restored on exit. ---- */
static void biasOn(void){
    GPIOA_MODER=savedModer|(3u<<8);
    RCC_APBENR1=savedRcc|RCC_DACEN;
    DAC_CR=DAC_CR_BIAS;
    DAC_DHR12R1=BIAS_CODE;
    DAC_SWTRIGR=1u;
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

/* ---- continuous listening: sample PA4 at 9.6 kHz into the decoder. Every
 * 1024 samples (107 ms) it hands over to keys and display, but only while
 * poc_busy() is false: no batch being decoded and no preamble just seen. v1.2
 * also paused during the preamble, which at 512 bps (55 bits per 1024 samples)
 * cost the sync word almost every time; on a continuous transmission it stops after CAP_MAX_CYC anyway. No RSSI
 * trigger: on the K1 the idle RSSI sat 11 dB above the floor measured at
 * launch and the old trigger fired on noise (v1.1). The sync word is the
 * detector. Returns the number of messages decoded. ---- */
static uint8_t listen(void){
    uint8_t n=0;
    uint16_t k=0;
    adcSelPA4();
    clkStart();
    uint32_t next=0;
    for(;;){
        while(clkCyc()<next){}
        if(poc_push(&d,adcRead())) n++;
        next+=CYC_PER_SAMPLE;
        if(++k & 1023u) continue;
        if(!poc_busy(&d)) break;
        if(next>=CAP_MAX_CYC){ if(poc_flush(&d)) n++; break; }
    }
    adcRestore();
    return n;
}

static void beep(uint8_t n){
    if(n>3u) n=3u;
    while(n--){
        A->prepare_tone();
        A->play_tone_raw(BEEP_HZ,BEEP_MS);
        A->tones_off_rx();
        A->delay_ms(60);
    }
    A->set_af(APP_AF_FM);          /* tones_off_rx mutes the AF output */
}

/* ---- display ---- */
static bool looksAlpha(const poc_msg_t *m,char *txt){
    uint8_t len=poc_text(m,true,txt), bad=0;
    for(uint8_t i=0;i<len;i++) if(txt[i]=='.') bad++;
    return len && bad*4u<=len;
}

static void draw(void){
    static const char RATE[3][5]={"512","1200","2400"};
    static const char CORNER[POC_NCORNER][5]={"off","60","250","1k","1k2","1k5"};
    static const char MODE[MODE_COUNT]={'?','A','N'};
    const poc_msg_t *m=poc_get(&d,view);
    char txt[POC_MAXBITS/4u+1u];      /* on the stack: the 4 KiB overlay is full */
    char *o;
    A->display_clear();
    A->status_clear();
    A->print_inverse(TITLE,2,0,true,true,(uint8_t)(2u+(sizeof(TITLE)-1u)*4u));
    A->draw_battery();

    if(!m){
        A->print_normal("Waiting...",0,0,1);
    } else {
        bool alpha = mode==MODE_ALPHA || (mode==MODE_AUTO && looksAlpha(m,txt));
        o=puti(str,(int32_t)m->ric); o=put(o," F"); *o++=(char)('0'+m->func);
        *o++=' '; *o++= !m->nbits?'T':alpha?'A':'N';     /* T = tone only */
        o=put(o,"  "); o=puti(o,view+1); *o++='/'; o=puti(o,d.count<POC_HIST?d.count:POC_HIST);
        *o='\0'; A->print_normal(str,0,0,0);

        uint8_t len=poc_text(m,alpha,txt);
        for(uint8_t r=0,i=0;r<ROWS && i<len;r++){
            o=str;
            for(uint8_t c=0;c<32u && i<len;c++) *o++=txt[i++];
            tiny(0,(uint8_t)(10u+7u*r),o);
        }
    }

    o=put(str,RATE[rate]); o=put(o," AC"); o=put(o,CORNER[corner]);
    *o++=' '; *o++=MODE[mode]; *o++=' ';
    o=puti(o,rssi); o=put(o,"dBm");
    o=put(o," #"); o=puti(o,total);
    if(m && (m->flags&POC_F_BAD)) o=put(o," BAD");
    else if(m && (m->flags&POC_F_FIXED)) o=put(o," fix");
    if(!beepOn) o=put(o," mute");
    tiny(0,48,o);
}

static void refresh(void){
    if(saver) return;
    draw();
    A->blit_status();
    A->blit_full();
}

/* ---- config (staged, committed by the loader on exit) ---- */
static void loadCfg(void){
    uint8_t c[5]; A->cfg_load(c,5);
    if(c[0]!=CFG_MAGIC) return;
    if(c[1]<3u) rate=c[1];
    if(c[2]<POC_NCORNER) corner=c[2];
    if(c[3]<MODE_COUNT) mode=c[3];
    if(c[4]<=1u) beepOn=c[4];
}
static void saveCfg(void){ uint8_t c[5]={CFG_MAGIC,rate,corner,mode,beepOn}; A->cfg_save(c,5); }
static uint8_t cyc(uint8_t v,uint8_t n){ return (uint8_t)(v+1u<n ? v+1u : 0u); }

/* ---- input ---- */
static void handleKeys(void){
    uint8_t key=A->get_key();
    if(key==APP_KEY_SAVER){ saver=true; return; }
    if(key==APP_KEY_WAKE){ saver=false; return; }
    if(key==APP_KEY_INVALID||key==prevKey){ prevKey=key; return; }
    prevKey=key;
    saver=false;
    A->backlight_on();
    switch(key){
        case APP_KEY_EXIT: running=false; break;
        case APP_KEY_1:    rate=cyc(rate,3); poc_config(&d,rate,corner); saveCfg(); break;
        case APP_KEY_2:    corner=cyc(corner,POC_NCORNER); poc_config(&d,rate,corner); saveCfg(); break;
        case APP_KEY_3:    mode=cyc(mode,MODE_COUNT); saveCfg(); break;
        case APP_KEY_4:    beepOn^=1u; saveCfg(); break;
        case APP_KEY_MENU: poc_init(&d,rate,corner); view=0; total=0; break;
        case APP_KEY_UP:
        case APP_KEY_DOWN: {
            int8_t n=(int8_t)(view+A->nav_dir(key));
            uint8_t have=d.count<POC_HIST?d.count:POC_HIST;
            if(n>=0 && n<(int8_t)have) view=(uint8_t)n;
            break; }
        default: break;
    }
}

__attribute__((section(".text.entry"),used))
void app_main(const app_api_t *api){
    A=api;
    view=0; total=0; saver=false; prevKey=APP_KEY_INVALID;
    loadCfg();
    poc_init(&d,rate,corner);

    savedSqr3=ADC_SQR3; savedSmpr3=ADC_SMPR3; savedModer=GPIOA_MODER; savedDac=DAC_CR;
    savedRcc=RCC_APBENR1; savedDhr=DAC_DHR12R1;
    biasOn();
    A->backlight_on();
    A->bk_write(REG_2B,(uint16_t)((A->bk_read(REG_2B)|0x0700u)&~0x0007u));
    A->bk_write(REG_73,(uint16_t)(A->bk_read(REG_73)|0x0010u));
    A->audio_path(true);
    A->set_af(APP_AF_FM);
    A->delay_ms(50);

    running=true;
    while(running){
        uint8_t n=listen();
        if(n){
            total=(uint16_t)(total+n); view=0;
            saver=false; A->backlight_on();
            if(beepOn) beep(n);
        }
        rssi=A->rssi_dbm();
        handleKeys();
        refresh();
        A->battery_sample();
        for(uint8_t i=0;i<10u;i++) A->backlight_update();   /* ~10 ticks of 10 ms per pass */
    }

    adcRestore();
    biasOff();
    A->set_af(APP_AF_MUTE);
    A->audio_path(false);
}
