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
 * POCSAG Rec (RX only) - records part of a POCSAG transmission exactly as the
 * POCSAG app sees it (RAW RX, PA4 held at mid-scale by the DAC, ADC channel 4
 * at 9.6 kHz from the RSSI trigger) and sends it over the programming cable
 * (USART1, 38400 8N1, as set up by the firmware) for analysis on the Mac
 * (pocrec_capture.py). Derived from 406 Rec.
 *
 * Storage: NSAMP samples from t0 ms after the trigger (UP/DOWN: 0-2000 ms in
 * 100 ms steps; 400 ms puts the end of a 1200 bps preamble, the sync and the
 * first codewords in the window). 4 bits each, linear code "lin48": code n
 * stands for c + (n - 8) * 48 + 24 LSB, c = mean of the first 32 samples
 * (the first sample until then). Codes 0 and 15 count as clipped; the raw
 * minimum and maximum are sent too.
 * Output, text:
 *   POCREC <ver> fs 9600 n <NSAMP> t0 <ms> code lin48 c <c> min <min> max <max> rssi <dBm> clip <count>
 *   <lines of up to 100 hex digits, one code per sample>
 *   END
 * Keys (UV-K5 and UV-K1): UP/DOWN start delay · 1 resend the last recording
 *   · EXIT quit.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "../app_api.h"

#ifndef APP_VERSION
#define APP_VERSION "dev"   /* set by build.sh from APP_VER */
#endif
#define TITLE "POC REC v" APP_VERSION

/* ---- MCU registers (PY32F071) ---- */
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
#define USART1_SR   (*(volatile uint32_t *)0x40013800u)
#define USART1_DR   (*(volatile uint32_t *)0x40013804u)
#define USART_TXE   (1u << 7)
#define RCC_DACEN   (1u << 29)
#define DAC_CR_BIAS ((1u << 0) | (1u << 1) | (1u << 2) | (7u << 3))
#define ADC_SR_EOC     (1u << 1)
#define ADC_CR2_START  ((1u << 22) | (1u << 20))
#define SMP8_POS       24u
#define SMP4_POS       12u
#define CYC_PER_SAMPLE 5000u               /* 48 MHz / 9.6 kHz */
#define CYC_PER_MS     48000u

#define REG_2B      0x2B
#define REG_73      0x73
#ifndef NSAMP
#define NSAMP       3700u                  /* 385 ms at 9.6 kHz: what fits in 4 KiB */
#endif
#define STEP        48                     /* LSB per code step                     */
#define T0_MAX      2000u
#define TRIG_DB     10
#define TICK_MS     50

static const app_api_t *A;
static uint8_t  buf[NSAMP / 2u];
static uint32_t savedSqr3, savedSmpr3, savedModer, savedDac, savedRcc, savedDhr;
static uint32_t tPrev, tCyc;
static int32_t  floorQ, center;
static int16_t  rssi, burstRssi, t0 = 400, recT0;
static uint16_t clip, nRec, vmin, vmax;
static bool     running, have;
static uint8_t  prevKey;
static char     str[40];

static const char HX[] = "0123456789ABCDEF";

static char *put(char *o,const char *s){ while(*s)*o++=*s++; return o; }
static uint8_t sub(uint32_t *v,uint32_t d){ uint8_t q=0; while(*v>=d){ *v-=d; q++; } return q; }
static char *puti(char *o,int32_t v){
    static const uint16_t P10[]={10000u,1000u,100u,10u,1u};
    uint32_t u; bool lead=false;
    if(v<0){*o++='-';u=(uint32_t)(-v);} else u=(uint32_t)v;
    for(uint8_t i=0;i<5;i++){ uint8_t c=sub(&u,P10[i]); if(c||lead||i==4u){ *o++=(char)('0'+c); lead=true; } }
    return o;
}
static void line(uint8_t l,char *end){ *end='\0'; A->print_normal(str,0,0,l); }

/* ---- UART (USART1 already initialised by the firmware) ---- */
static void tx(char c){
    for(uint16_t g=0; !(USART1_SR&USART_TXE) && g<60000u; g++){}
    USART1_DR=(uint8_t)c;
}
static void txs(const char *s){ while(*s) tx(*s++); }
static void txkv(const char *k,int32_t v){ char *o=put(str,k); o=puti(o,v); *o='\0'; txs(str); }

static void send(void){
    txkv("POCREC " APP_VERSION " fs 9600 n ",NSAMP);
    txkv(" t0 ",recT0);
    txkv(" code lin48 c ",center);
    txkv(" min ",vmin); txkv(" max ",vmax);
    txkv(" rssi ",burstRssi); txkv(" clip ",clip);
    txs("\r\n");
    uint8_t col=0;
    for(uint16_t i=0;i<NSAMP;i++){
        tx(HX[(buf[i>>1]>>((i&1u)?0:4))&15u]);
        if(++col==100u){ col=0; txs("\r\n"); }
    }
    txs("\r\nEND\r\n");
}

/* ---- timing, ADC (as in EPIRB 406) ---- */
static uint32_t clkCyc(void){
    uint32_t v=SYST_VAL;
    tCyc += (v<=tPrev) ? tPrev-v : tPrev+(SYST_LOAD+1u-v);
    tPrev=v;
    return tCyc;
}
static uint16_t adcRead(void){
    ADC_CR2|=ADC_CR2_START;
    for(uint16_t g=0; !(ADC_SR&ADC_SR_EOC) && g<2000u; g++){}
    return (uint16_t)(ADC_DR&0x0FFFu);
}

static void record(void){
    burstRssi=rssi; recT0=t0;
    ADC_SMPR3=(savedSmpr3&~(7u<<SMP4_POS))|(((savedSmpr3>>SMP8_POS)&7u)<<SMP4_POS);
    ADC_SQR3=(savedSqr3&~0x1Fu)|4u;
    tPrev=SYST_VAL; tCyc=0;
    uint32_t next=(uint32_t)t0*CYC_PER_MS;
    int32_t sum=0;
    clip=0; vmin=4095; vmax=0;
    for(uint16_t i=0;i<NSAMP;i++){
        while(clkCyc()<next){}
        int32_t x=adcRead();
        if(x<vmin) vmin=(uint16_t)x;
        if(x>vmax) vmax=(uint16_t)x;
        if(i==0u) center=x;                          /* provisional until 32 samples */
        if(i<32u){ sum+=x; if(i==31u) center=(sum+16)>>5; }
        /* linear 4-bit code: n = 8 + floor((x - c) / 48), clamped to 0..15 */
        int32_t e=x-center+8*STEP;
        int32_t n=0;
        while(n<15 && e>=STEP){ e-=STEP; n++; }
        if(n==0||n==15) clip++;
        if(i&1u) buf[i>>1]|=(uint8_t)n; else buf[i>>1]=(uint8_t)(n<<4);
        next+=CYC_PER_SAMPLE;
    }
    ADC_SQR3=savedSqr3; ADC_SMPR3=savedSmpr3;
    have=true; nRec++;
}

static void draw(const char *state){
    char *o;
    A->display_clear();
    A->status_clear();
    A->print_inverse(TITLE,2,0,true,true,(uint8_t)(2u+(sizeof(TITLE)-1u)*4u));
    A->draw_battery();
    line(0,put(str,state));
    o=put(str,"t0 "); o=puti(o,t0); o=put(o," ms"); line(1,o);
    o=puti(str,rssi); *o++='/'; o=puti(o,floorQ/64); o=put(o," dBm"); line(2,o);
    if(have){
        o=put(str,"rec "); o=puti(o,nRec); o=put(o," c "); o=puti(o,center); line(3,o);
        o=puti(str,vmin); *o++='-'; o=puti(o,vmax); o=put(o," clip "); o=puti(o,clip); line(4,o);
    }
    line(6,put(str,"1:resend EXIT:quit"));
    A->blit_status(); A->blit_full();
}

__attribute__((section(".text.entry"),used))
void app_main(const app_api_t *api){
    A=api;
    have=false; nRec=0; prevKey=APP_KEY_INVALID;
    savedSqr3=ADC_SQR3; savedSmpr3=ADC_SMPR3; savedModer=GPIOA_MODER; savedDac=DAC_CR;
    savedRcc=RCC_APBENR1; savedDhr=DAC_DHR12R1;

    A->backlight_on();
    A->bk_write(REG_2B,(uint16_t)((A->bk_read(REG_2B)|0x0700u)&~0x0007u));
    A->bk_write(REG_73,(uint16_t)(A->bk_read(REG_73)|0x0010u));
    A->audio_path(true);
    A->set_af(APP_AF_FM);
    GPIOA_MODER=savedModer|(3u<<8);
    RCC_APBENR1=savedRcc|RCC_DACEN;
    DAC_CR=DAC_CR_BIAS; DAC_DHR12R1=2048u; DAC_SWTRIGR=1u;
    A->delay_ms(50);

    int32_t f=0;
    for(uint8_t i=0;i<16;i++){ f+=A->rssi_dbm(); A->delay_ms(5); }
    floorQ=f*4;

    running=true;
    while(running){
        for(uint8_t i=0;i<TICK_MS;i++){
            rssi=A->rssi_dbm();
            if(rssi>=floorQ/64+TRIG_DB){
                draw("REC");
                record();
                draw("TX");
                send();
                for(uint16_t ms=0; ms<3000u && A->rssi_dbm()>=floorQ/64+5; ms+=10) A->delay_ms(10);
                break;
            }
            floorQ+=((int32_t)rssi*64-floorQ)/64;
            A->delay_ms(1);
        }
        uint8_t key=A->get_key();
        if(key!=prevKey && key!=APP_KEY_INVALID){
            if(key==APP_KEY_EXIT) running=false;
            if(key==APP_KEY_1 && have){ draw("TX"); send(); }
            if(key==APP_KEY_UP||key==APP_KEY_DOWN){
                int16_t n=(int16_t)(t0+100*A->nav_dir(key));
                if(n>=0 && n<=(int16_t)T0_MAX) t0=n;
            }
        }
        prevKey=key;
        draw(have?"Sent, WAIT":"WAIT");
        A->battery_sample();
        A->backlight_on();
    }

    DAC_CR=savedDac; DAC_DHR12R1=savedDhr; RCC_APBENR1=savedRcc; GPIOA_MODER=savedModer;
    A->set_af(APP_AF_MUTE);
    A->audio_path(false);
}
