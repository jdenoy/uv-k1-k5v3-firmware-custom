/* Host harness for the KISS TNC step-0 app: scripted USB input, captured output. */
#define ENABLE_FEAT_F4HWN_OVERLAY_INFO
#define _Static_assert(...)   /* the API size check assumes 32-bit pointers */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../app_api.h"
static uint8_t IN[8192]; static size_t inN, inPos; static uint8_t FB[8][128], ST[128];
static int tick; static const char *KEYS;  /* one char per tick: '.' none, '2' test, 'x' exit */
static uint16_t s_read(uint8_t *b, uint16_t len){
    size_t chunk = 1 + (inPos * 7 + 3) % 37;            /* odd USB packet sizes */
    if(chunk > len) chunk = len; if(chunk > inN - inPos) chunk = inN - inPos;
    memcpy(b, IN + inPos, chunk); inPos += chunk; return (uint16_t)chunk;
}
static bool s_write(const uint8_t *b, uint16_t len){ if(len){ printf("OUT"); for(int i=0;i<len;i++) printf(" %02X", b[i]); printf("\n"); } return true; }
static uint8_t keyf(void){ char c = KEYS[tick] ? KEYS[tick] : 'x'; tick++;
    return c=='2'?APP_KEY_2: c=='1'?APP_KEY_1: c=='x'?APP_KEY_EXIT: APP_KEY_INVALID; }
static void nop(void){} static void nopb(bool b){(void)b;} static void dly(uint32_t m){(void)m;}
static void pn(const char *s,uint8_t a,uint8_t b,uint8_t l){(void)a;(void)b; printf("LINE%u %s\n",l,s);}
static void pi(const char *s,uint8_t x,uint8_t l,bool sb,bool f,uint8_t e){(void)x;(void)l;(void)sb;(void)f;(void)e; printf("TAG %s\n",s);}
static void call(char *b,uint8_t n){ (void)n; strcpy(b,"F4WAT"); }
static uint64_t udm(uint32_t n,uint32_t d){ return ((uint64_t)(n%d)<<32)|(n/d); }
#include "app.c"   /* kisstnc_app.c without its section attribute (test_kiss.py) */
int main(int argc,char**argv){
    FILE *f=fopen(argv[1],"rb"); inN=fread(IN,1,sizeof IN,f); fclose(f); KEYS=argv[2];
    static app_api_t a; a.fb=FB; a.status_line=ST; a.display_clear=nop; a.status_clear=nop; a.blit_full=nop; a.blit_status=nop;
    a.get_key=keyf; a.delay_ms=dly; a.print_normal=pn; a.print_inverse=pi; a.draw_battery=nop; a.backlight_on=nop;
    a.backlight_update=nop; a.battery_sample=nop; a.boot_callsign=call; a.uidivmod=udm; a.serial_read=s_read; a.serial_write=s_write;
    app_main(&a); return 0;
}
