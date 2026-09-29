/* Host harness: run the POCSAG decoder over a file of little-endian uint16 ADC
 * samples at 9.6 kHz and print every committed message, then the statistics.
 *
 *   host_pocsag 512|1200|2400 samples.u16 [corner]
 *
 * corner: AC coupling the decoder compensates, 0 30 60 100 150 250 (Hz,
 * default 60 as in the app).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../pocsag.h"

static void show(const poc_msg_t *m)
{
    char a[POC_MAXBITS / 4 + 1], n[POC_MAXBITS / 4 + 1];
    poc_text(m, true, a);
    poc_text(m, false, n);
    printf("RIC %07lu F%u bits %u%s%s%s\n", (unsigned long)m->ric, m->func, m->nbits,
           m->flags & POC_F_FIXED ? " fixed" : "", m->flags & POC_F_BAD ? " BAD" : "",
           m->flags & POC_F_TRUNC ? " trunc" : "");
    printf("  alpha: [%s]\n  num  : [%s]\n", a, n);
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s 512|1200|2400 samples.u16 [corner]\n", argv[0]); return 2; }
    int rate = atoi(argv[1]);
    static const int HZ[POC_NCORNER] = { 0, 30, 60, 100, 150, 250 };
    int hz = argc > 3 ? atoi(argv[3]) : 60, corner = POC_NCORNER;
    for (int i = 0; i < POC_NCORNER; i++) if (HZ[i] == hz) corner = i;
    if (corner == POC_NCORNER) { fprintf(stderr, "corner must be 0 30 60 100 150 or 250\n"); return 2; }
    FILE *f = fopen(argv[2], "rb");
    if (!f) { perror(argv[2]); return 2; }
    static poc_t d;
    poc_init(&d, rate == 512 ? POC_512 : rate == 2400 ? POC_2400 : POC_1200, (uint8_t)corner);
    unsigned char b[2];
    int msgs = 0;
    while (fread(b, 1, 2, f) == 2)
        if (poc_push(&d, (uint16_t)(b[0] | (b[1] << 8)))) { show(poc_last(&d)); msgs++; }
    if (poc_flush(&d)) { show(poc_last(&d)); msgs++; }
    fclose(f);
    printf("messages  : %d\nsyncs %u codewords %u fixed %u bad %u inverted %u\n",
           msgs, d.nSync, d.nCw, d.nFix, d.nBad, d.inv);
    return msgs ? 0 : 1;
}
