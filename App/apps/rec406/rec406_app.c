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
 * 406 Rec (RX only) - records one 406 beacon burst exactly as EPIRB 406 sees it
 * (RAW RX, PA4 held at mid-scale by the DAC, ADC channel 4 at 9.6 kHz from the
 * RSSI trigger) and sends it over the programming cable (USART1, 38400 8N1, as
 * set up by the firmware) for analysis with the host decoder.
 *
 * Storage: 4260 samples (444 ms, from 80 ms after the trigger: end of the carrier,
 * preamble, sync and the whole 144-bit frame), 4 bits each, non-linear code
 * "comp1" around c = mean of the first 32 samples (carrier; the first sample
 * stands in for c until then). Code n counts the thresholds TH[] at or below
 * x - c; n = 0 or 15 counts as clipped.
 * Output, text:
 *   REC406 <ver> fs 9600 n 4260 t0 80 code comp1 c <c> rssi <dBm> clip <count>
 *   <43 lines of up to 100 hex digits, one code per sample>
 *   END
 * Keys (UV-K5 and UV-K1): 1 resend the last recording · EXIT quit.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "../app_api.h"

#ifndef APP_VERSION
#define APP_VERSION "dev"   /* set by build.sh from APP_VER */
#endif
#define TITLE "406 REC v" APP_VERSION

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

#define REG_2B      0x2B
#define REG_73      0x73
#define NSAMP       4260u                  /* 444 ms at 9.6 kHz: what fits in 4 KiB */
#define DELAY_CYC   (48000u * 80u)         /* start 80 ms after the trigger          */
#define TRIG_DB     10
#define TICK_MS     50

static const app_api_t *A;
static uint8_t  buf[NSAMP / 2u];
static uint32_t savedSqr3, savedSmpr3, savedModer, savedDac, savedRcc, savedDhr;
static uint32_t tPrev, tCyc;
static int32_t  floorQ, center;
static int16_t  rssi, burstRssi;
static uint16_t clip, nRec;
static bool     running, have;
static uint8_t  prevKey;
static char     str[34];

static const char HX[] = "0123456789ABCDEF";
/* Code n = 0..15 stands for LV[n] = -300 -200 -130 -80 -45 -22 -8 0 8 22 45 80
 * 130 200 300 420 (ADC LSB from c); thresholds are the midpoints. Fine near zero,
 * coarse at the peaks: a linear 4-bit code broke decoding on the host tests. */
static const int16_t TH[15] = { -250,-165,-105,-63,-34,-15,-4,4,15,34,63,105,165,250,360 };

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

static void send(void){
    txs("REC406 " APP_VERSION " fs 9600 n 4260 t0 80 code comp1 c ");  /* longer than str[] */
    char *o=puti(str,center); o=put(o," rssi "); o=puti(o,burstRssi); o=put(o," clip "); o=puti(o,clip);
    o=put(o,"\r\n"); *o='\0'; txs(str);
    uint8_t col=0;
    for(uint16_t i=0;i<NSAMP;i++){
        tx(HX[(buf[i>>1]>>((i&1u)?0:4))&15u]);
        if(++col==100u){ col=0; txs("\r\n"); }
    }
    txs("END\r\n");
}

/* ---- timing, ADC, bias (as in EPIRB 406) ---- */
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
    burstRssi=rssi;
    ADC_SMPR3=(savedSmpr3&~(7u<<SMP4_POS))|(((savedSmpr3>>SMP8_POS)&7u)<<SMP4_POS);
    ADC_SQR3=(savedSqr3&~0x1Fu)|4u;
    tPrev=SYST_VAL; tCyc=0;
    uint32_t next=DELAY_CYC;                /* skip the start of the carrier */
    int32_t sum=0;
    clip=0;
    for(uint16_t i=0;i<NSAMP;i++){
        while(clkCyc()<next){}
        int32_t x=adcRead();
        if(i==0u) center=x;                          /* provisional until 32 samples */
        if(i<32u){ sum+=x; if(i==31u) center=(sum+16)>>5; }
        /* non-linear 4-bit code: count of thresholds at or below x - c */
        int32_t e=x-center;
        uint8_t n=0;
        for(uint8_t k=0;k<15u;k++) if(e>=TH[k]) n++;
        if(n==0u||n==15u) clip++;
        if(i&1u) buf[i>>1]|=n; else buf[i>>1]=(uint8_t)(n<<4);
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
    o=puti(str,rssi); *o++='/'; o=puti(o,floorQ/64); o=put(o," dBm"); line(2,o);
    if(have){
        o=put(str,"rec "); o=puti(o,nRec); o=put(o," c "); o=puti(o,center); line(3,o);
        o=put(str,"clip "); o=puti(o,clip); o=put(o," / 4260"); line(4,o);
    }
    line(6,put(str,"1:resend EXIT:quit"));    /* 18 chars x 7 px = 126 px */
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
                for(uint16_t ms=0; ms<1500u && A->rssi_dbm()>=floorQ/64+5; ms+=10) A->delay_ms(10);
                break;
            }
            floorQ+=((int32_t)rssi*64-floorQ)/64;
            A->delay_ms(1);
        }
        uint8_t key=A->get_key();
        if(key!=prevKey && key!=APP_KEY_INVALID){
            if(key==APP_KEY_EXIT) running=false;
            if(key==APP_KEY_1 && have){ draw("TX"); send(); }
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
