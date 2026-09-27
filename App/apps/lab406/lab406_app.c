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
 * 406 Lab (RX only) - feasibility probe for an on-radio 406 MHz beacon decoder.
 *
 * Question: can an app get at the demodulated signal? The MCU has no ADC on the
 * audio, so this probe looks for (a) a BK4829 register that follows the
 * discriminator output and (b) whether PB1 (ADC channel 9, configured analog but
 * never read by the firmware) or PA4 (the voice DAC pin, unused while voice is
 * disabled, wired towards the audio amplifier) carries anything.
 *
 * Use with a beacon-format test generator (e.g. rpitx on a ham frequency): tune the
 * VFO to it (FM, wide), launch. The app switches the receiver to RAW (RX HPF300,
 * LPF3K, de-emphasis and AFC off, as BK4819_EnterRaw does), waits for a burst on
 * RSSI, then polls one group of 8 registers plus PB1 for ~430 ms, keeping two
 * windows per channel:
 *     A = 15..130 ms after trigger  (unmodulated carrier)
 *     B = 200..420 ms               (biphase PSK message)
 * Activity = mean |v[n]-v[n-1]| per sample. A register that tracks the
 * demodulated signal is quiet in A and busy in B. After each burst the next group
 * is probed (16 groups cover 0x00-0x7F; 0x5F, the FSK FIFO, is never read).
 *
 * Keys (UV-K5 and UV-K1): UP/DOWN group · 1 auto-advance · 2 speaker (RAW audio)
 *   3 RAW on/off · 5 scope mode (PA4 at 9.6 kHz, clipping
 *   counts; UP/DOWN then step the AF DAC gain, REG_48 bits 3:0) · 6 in scope mode:
 *   PA4 bias on/off (MCU DAC unbuffered at mid-scale) · MENU clear · EXIT.
 * The loader re-runs RADIO_SetupRegisters on exit, so every radio register the
 * probe touches is restored by the firmware; the app restores the ADC itself.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "../app_api.h"

#ifndef APP_VERSION
#define APP_VERSION "dev"   /* set by build.sh from APP_VER */
#endif
#define TITLE "406 LAB v" APP_VERSION

/* ---- MCU registers (PY32F071, SysTick at 48 MHz, 10 ms period) ---- */
#define SYST_LOAD   (*(volatile uint32_t *)0xE000E014u)
#define SYST_VAL    (*(volatile uint32_t *)0xE000E018u)
#define ADC_SR      (*(volatile uint32_t *)0x40012400u)
#define ADC_CR2     (*(volatile uint32_t *)0x40012408u)
#define ADC_SMPR3   (*(volatile uint32_t *)0x40012414u)
#define ADC_SQR3    (*(volatile uint32_t *)0x40012438u)
#define ADC_DR      (*(volatile uint32_t *)0x40012450u)
#define GPIOA_MODER (*(volatile uint32_t *)0x50000000u)
#define DAC_CR      (*(volatile uint32_t *)0x40007400u)
#define DAC_DHR12R1 (*(volatile uint32_t *)0x40007408u)
#define RCC_APBENR1 (*(volatile uint32_t *)0x4002103Cu)
#define RCC_DACEN   (1u << 29)
#define DAC_SWTRIGR (*(volatile uint32_t *)0x40007404u)
#define DAC_DOR1    (*(volatile uint32_t *)0x4000742Cu)
/* channel on, output buffer off, trigger enabled with TSEL1 = software (111) */
#define DAC_CR_BIAS ((1u << 0) | (1u << 1) | (1u << 2) | (7u << 3))
#define BIAS_CODE   2048u                        /* mid-scale, about VDD/2        */
#define ADC_SR_EOC       (1u << 1)
#define ADC_CR2_START    ((1u << 22) | (1u << 20))   /* SWSTART | EXTTRIG */
#define SMP8_POS         24u
#define CPU_MHZ          48u

/* ---- BK4829 ---- */
#define REG_2B      0x2B   /* bits 10/9/8: disable RX HPF300 / LPF3K / de-emphasis */
#define REG_73      0x73   /* bit 4: AFC disable                                    */
#define REG_FIFO    0x5F   /* FSK FIFO: reading pops data, never probed            */
#define REG_48      0x48   /* bits 3:0: AF DAC gain, last RX audio gain stage      */
#define CYC_PER_SAMPLE 5000u  /* scope mode: 48 MHz / 9.6 kHz, the decoder's rate */

/* ---- probe ---- */
#define GROUPS      16
#define CH          9      /* 8 registers + PB1 */
#define CH_ADC      8
#define WIN_A0      15000u
#define WIN_A1      130000u
#define WIN_B0      200000u
#define WIN_B1      420000u
#define CAP_END     430000u
#define TRIG_DB     10
#define REARM_DB    5
#define COOL_MS     1500
#define TICK_MS     50

typedef struct { uint32_t tv; uint16_t n, mn, mx; } st_t;

static const app_api_t *A;
static st_t     S[CH][2];
static uint16_t prv[CH];
static uint16_t adcIdle;
static uint32_t savedSqr3, savedSmpr3, savedModer, savedDac, savedRcc, savedDhr;
static bool     bias;                 /* DAC unbuffered at mid-scale as a weak bias on PA4 */
static uint8_t  adcCh;                /* 9 = PB1, 4 = PA4 (voice DAC pin) */
static uint16_t saved2B, saved73, saved48, clipLo, clipHi;
static uint8_t  dacGain;
static bool     scope;                /* PA4 alone at 9.6 kHz, AF DAC gain on UP/DOWN */
static int16_t  rssi;
static int32_t  floorQ;               /* noise floor, dBm x64 */
static uint8_t  group, capGroup, bursts, prevKey;   /* group = next to probe, capGroup = shown */
static bool     autoAdv, speaker, rawOn, running, captured;
static uint32_t tPrev, tCyc, tUs, tCycAll;
static char     str[34];

/* ---- formatting ---- */
static char *put(char *o,const char *s){ while(*s)*o++=*s++; return o; }
static char *puti(char *o,int32_t v){
    uint32_t u;
    if(v<0){*o++='-';u=(uint32_t)(-v);} else u=(uint32_t)v;
    char t[10]; int8_t n=0;
    do{t[n++]=(char)('0'+u%10u);u/=10u;}while(u&&n<10);
    while(n--)*o++=t[n];
    return o;
}
static char *puthex2(char *o,uint8_t v){
    static const char H[]="0123456789ABCDEF";
    *o++=H[v>>4]; *o++=H[v&15]; return o;
}
static void row(uint8_t x,uint8_t r,char *end){ *end='\0'; A->print_tiny(str,x,(uint8_t)(r*7u+1u),false,true); }

/* ---- microsecond clock from SysTick (call at least every 10 ms) ---- */
static void clkStart(void){ tPrev=SYST_VAL; tCyc=0; tUs=0; tCycAll=0; }
static uint32_t clkCyc(void);
static uint32_t clkUs(void){
    uint32_t v=SYST_VAL;
    uint32_t dc = (v<=tPrev) ? tPrev-v : tPrev+(SYST_LOAD+1u-v);
    tCyc += dc; tCycAll += dc;
    tPrev=v;
    tUs+=tCyc/CPU_MHZ; tCyc%=CPU_MHZ;
    return tUs;
}

static uint32_t clkCyc(void){ clkUs(); return tCycAll; }

/* ---- ADC: probe channel only while probing, channel 8 (battery) otherwise.
 * PA4 is the voice DAC output (feeds the audio amplifier); voice is disabled in
 * every preset, but the DAC is switched off and PA4 set analog while it is read. ---- */
static void adcSel9(void){
    uint32_t pos=3u*adcCh;
    if(adcCh==4){ if(!bias) DAC_CR=savedDac&~1u; GPIOA_MODER=savedModer|(3u<<8); }
    ADC_SMPR3=(savedSmpr3 & ~(7u<<pos)) | (((savedSmpr3>>SMP8_POS)&7u)<<pos);
    ADC_SQR3=(savedSqr3 & ~0x1Fu) | adcCh;
}
static void adcRestore(void){ ADC_SQR3=savedSqr3; ADC_SMPR3=savedSmpr3; if(!bias){ GPIOA_MODER=savedModer; DAC_CR=savedDac; } }

/* PA4 floats (AC-coupled audio, no DC path): fast sampling drags it to 0 V. The
 * DAC with its output buffer off acts as a weak resistor to a fixed voltage. */
static void setBias(bool on){
    bias=on;
    if(on){
        RCC_APBENR1=savedRcc|RCC_DACEN;
        GPIOA_MODER=savedModer|(3u<<8);
        DAC_CR=DAC_CR_BIAS;
        DAC_DHR12R1=BIAS_CODE;
        DAC_SWTRIGR=1u;                         /* DHR -> DOR */
    } else {
        DAC_CR=savedDac; DAC_DHR12R1=savedDhr; RCC_APBENR1=savedRcc; GPIOA_MODER=savedModer;
    }
}
static uint16_t adcRead(void){
    ADC_CR2|=ADC_CR2_START;
    for(uint16_t g=0; !(ADC_SR&ADC_SR_EOC) && g<2000u; g++){}
    return (uint16_t)(ADC_DR & 0x0FFFu);
}

/* ---- radio ---- */
static void setRaw(void){
    if(rawOn){
        A->bk_write(REG_2B,(uint16_t)((saved2B|0x0700u)&~0x0007u));
        A->bk_write(REG_73,(uint16_t)(saved73|0x0010u));
    } else {
        A->bk_write(REG_2B,saved2B);
        A->bk_write(REG_73,saved73);
    }
}
static void setSpeaker(void){
    if(speaker){ A->audio_path(true); A->set_af(APP_AF_FM); }
    else       { A->set_af(APP_AF_MUTE); A->audio_path(false); }
}
static uint8_t regOf(uint8_t ch){ return (uint8_t)(group*8u+ch); }
static void setDac(void){ A->bk_write(REG_48,(uint16_t)((saved48&~0x000Fu)|dacGain)); }
static uint8_t capReg(uint8_t ch){ return (uint8_t)(capGroup*8u+ch); }

/* ---- capture one burst ---- */
static uint16_t sample(uint8_t ch){
    if(ch==CH_ADC) return adcRead();
    uint8_t r=regOf(ch);
    return r==REG_FIFO ? 0u : A->bk_read(r);
}

static uint16_t act(const st_t *s){ return s->n ? (uint16_t)((s->tv*10u)/s->n) : 0u; } /* tenths */

static void capture(void){
    capGroup=group;
    for(uint8_t c=0;c<CH;c++){
        for(uint8_t w=0;w<2;w++){ S[c][w].tv=0; S[c][w].n=0; S[c][w].mn=0xFFFF; S[c][w].mx=0; }
    }
    adcSel9();
    if(scope){
        /* PA4 alone, paced like the decoder: one sample every 5000 cycles */
        clipLo=clipHi=0;
        prv[CH_ADC]=adcRead();
        clkStart();
        uint32_t next=0;
        for(;;){
            while(clkCyc()<next){}
            uint32_t t=tCycAll/CPU_MHZ;
            if(t>=CAP_END) break;
            uint16_t v=adcRead();
            int8_t w = (t>=WIN_A0&&t<WIN_A1) ? 0 : (t>=WIN_B0&&t<WIN_B1) ? 1 : -1;
            if(w>=0){
                st_t *st=&S[CH_ADC][w];
                st->tv+= v>prv[CH_ADC] ? (uint32_t)(v-prv[CH_ADC]) : (uint32_t)(prv[CH_ADC]-v);
                st->n++;
                if(v<st->mn) st->mn=v;
                if(v>st->mx) st->mx=v;
                if(w){ if(v==0u) clipLo++; if(v>=4095u) clipHi++; }
            }
            prv[CH_ADC]=v;
            next+=CYC_PER_SAMPLE;
        }
        adcRestore();
        captured=true; bursts++;
        return;
    }
    for(uint8_t c=0;c<CH;c++) prv[c]=sample(c);
    clkStart();
    for(;;){
        uint32_t t=clkUs();
        if(t>=CAP_END) break;
        int8_t w = (t>=WIN_A0&&t<WIN_A1) ? 0 : (t>=WIN_B0&&t<WIN_B1) ? 1 : -1;
        for(uint8_t c=0;c<CH;c++){
            uint16_t v=sample(c);
            if(w>=0){
                st_t *s=&S[c][w];
                s->tv+= v>prv[c] ? (uint32_t)(v-prv[c]) : (uint32_t)(prv[c]-v);
                s->n++;
                if(v<s->mn) s->mn=v;
                if(v>s->mx) s->mx=v;
            }
            prv[c]=v;
        }
    }
    adcRestore();

    captured=true;
    bursts++;
}

/* ---- drawing ---- */
static void draw(void){
    char *o;
    A->display_clear();
    A->status_clear();
    A->print_inverse(TITLE,2,0,true,true,(uint8_t)(2u+(sizeof(TITLE)-1u)*4u));
    A->draw_battery();

    /* row 0: group, bursts, RSSI vs floor */
    o=put(str,"G"); o=puti(o,capGroup); o=put(o,">"); o=puti(o,group);
    o=put(o," B"); o=puti(o,bursts); o=put(o," "); o=puti(o,rssi); o=put(o,"/"); o=puti(o,floorQ/64);
    row(0,0,o);

    /* rows 1-4: scope summary, or two registers per row (activity A/B) */
    if(scope){
        o=put(str,"SCOPE  gain "); o=puti(o,dacGain); row(0,1,o);
        for(uint8_t w=0;w<2 && captured;w++){
            const st_t *st=&S[CH_ADC][w];
            o=put(str,w?"msg ":"car "); o=puti(o,st->mn); *o++='-'; o=puti(o,st->mx);
            o=put(o," act "); o=puti(o,act(st)/10u); o=put(o," n"); o=puti(o,st->n);
            row(0,(uint8_t)(2u+w),o);
        }
        if(captured){ o=put(str,"clip 0: "); o=puti(o,clipLo); o=put(o,"  4095: "); o=puti(o,clipHi); row(0,4,o); }
    } else
    for(uint8_t c=0;c<8;c++){
        o=puthex2(str,capReg(c));
        if(capReg(c)==REG_FIFO) o=put(o," --");
        else if(captured){ *o++=' '; o=puti(o,act(&S[c][0])/10u); *o++='/'; o=puti(o,act(&S[c][1])/10u); }
        row((uint8_t)((c&1u)*64u),(uint8_t)(1u+c/2u),o);
    }

    /* row 5: scope mode: bias state and PA4 idle value (each fits in 128 px);
     * register mode: PA4 idle value, activity and range in the message window */
    if(scope){
        if(bias){ o=put(str,"BIAS ON dor "); o=puti(o,(int32_t)(DAC_DOR1&0xFFFu)); } else o=put(str,"bias off");
        o=put(o,"  idle "); o=puti(o,adcIdle);
    } else {
    o=put(str,"PA4 "); o=puti(o,adcIdle);
    if(captured){ o=put(o," "); o=puti(o,act(&S[CH_ADC][0])/10u); *o++='/'; o=puti(o,act(&S[CH_ADC][1])/10u);
                  o=put(o," "); o=puti(o,S[CH_ADC][1].mn); *o++='-'; o=puti(o,S[CH_ADC][1].mx);
                  o=put(o," n"); o=puti(o,S[0][1].n); }
    }
    row(0,5,o);

    /* row 7: switches */
    o=put(str,rawOn?"RAW":"raw"); o=put(o,speaker?" SPK":" spk"); o=put(o,autoAdv?" AUTO":" man");
    o=put(o,captured?"  WAIT":"  WAIT 1st");
    row(0,7,o);
}

/* ---- input ---- */
static void clearAll(void){
    group=0; capGroup=0; bursts=0; captured=false;
}
static void handleKeys(void){
    uint8_t key=A->get_key();
    if(key==APP_KEY_INVALID||key==prevKey){ prevKey=key; return; }
    prevKey=key;
    A->backlight_on();
    switch(key){
        case APP_KEY_EXIT: running=false; break;
        case APP_KEY_1:    autoAdv=!autoAdv; break;
        case APP_KEY_2:    speaker=!speaker; setSpeaker(); break;
        case APP_KEY_3:    rawOn=!rawOn; setRaw(); break;
        case APP_KEY_5:    scope=!scope; if(scope) adcCh=4; captured=false; break;
        case APP_KEY_6:    if(scope){ setBias(!bias); captured=false; } break;
        case APP_KEY_MENU: clearAll(); break;
        case APP_KEY_UP:
        case APP_KEY_DOWN: {
            int8_t d=A->nav_dir(key);
            if(scope){ dacGain=(uint8_t)((dacGain+(d>0?1u:15u))&15u); setDac(); captured=false; break; }
            group=(uint8_t)((group+(d>0?1u:GROUPS-1u))%GROUPS);
            if(!captured) capGroup=group;
            break; }
        default: break;
    }
}

/* Wait until the burst is over before re-arming. */
static void cooldown(void){
    for(uint16_t ms=0; ms<COOL_MS; ms+=10){
        if(A->rssi_dbm() < floorQ/64+REARM_DB) break;
        A->delay_ms(10);
    }
}

__attribute__((section(".text.entry"),used))
void app_main(const app_api_t *api){
    A=api;
    autoAdv=true; speaker=false; rawOn=true; prevKey=APP_KEY_INVALID;
    clearAll();

    savedSqr3=ADC_SQR3; savedSmpr3=ADC_SMPR3; savedModer=GPIOA_MODER; savedDac=DAC_CR;
    savedRcc=RCC_APBENR1; savedDhr=DAC_DHR12R1; bias=false;
    adcCh=4;                              /* PB1 (ch 9) is tied low: PA4 only */
    saved2B=A->bk_read(REG_2B); saved73=A->bk_read(REG_73); saved48=A->bk_read(REG_48);
    dacGain=(uint8_t)(saved48&15u); scope=false;
    A->backlight_on();
    setRaw();
    setSpeaker();
    A->delay_ms(50);

    int32_t f=0;
    for(uint8_t i=0;i<16;i++){ f+=A->rssi_dbm(); A->delay_ms(5); }
    floorQ=f*4;                                         /* sum of 16 -> mean x64 */

    running=true;
    while(running){
        /* ~TICK_MS of fast RSSI polling for the burst leading edge */
        for(uint8_t i=0;i<TICK_MS;i++){
            rssi=A->rssi_dbm();
            if(rssi>=floorQ/64+TRIG_DB){
                capture();
                if(autoAdv) group=(uint8_t)((group+1u)%GROUPS);
                draw(); A->blit_status(); A->blit_full();
                cooldown();
                break;
            }
            floorQ+=((int32_t)rssi*64-floorQ)/64;
            A->delay_ms(1);
        }

        handleKeys();
        if(!running) break;

        adcSel9(); adcIdle=adcRead(); adcRestore();
        draw();
        A->blit_status();
        A->blit_full();
        A->battery_sample();
        A->backlight_on();             /* keep the screen lit while waiting for bursts */
    }

    setBias(false);
    adcRestore();
    A->set_af(APP_AF_MUTE);
    A->audio_path(false);
}
