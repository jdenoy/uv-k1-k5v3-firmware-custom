/* Host harness: run the POCSAG decoder over a file of little-endian uint16 ADC
 * samples at 9.6 kHz and print every committed message, then the statistics.
 *
 *   host_pocsag 512|1200|2400 samples.u16 [corner]
 *
 * corner: AC coupling the decoder compensates, 0 30 60 100 150 250 (Hz,
 * default 60, the reference for the tests; the app defaults to 0 since v1.1).
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
    /* POC_HOLES=N emulates a capture loop that stalls every 256 samples for N
     * sample periods and then catches up: those N samples all read the value
     * at the end of the stall. */
    const char *hv = getenv("POC_HOLES");
    int holes = hv ? atoi(hv) : 0;
    unsigned char b[2];
    int msgs = 0;
    long n = 0;
    uint16_t buf[64];
    while (fread(b, 1, 2, f) == 2) {
        uint16_t s = (uint16_t)(b[0] | (b[1] << 8));
        if (holes > 0 && n % 256 == 255) {         /* stall: buffer the next samples */
            int k = 0;
            buf[k++] = s;
            while (k < holes && fread(b, 1, 2, f) == 2) buf[k++] = (uint16_t)(b[0] | (b[1] << 8));
            for (int i = 0; i < k; i++)
                if (poc_push(&d, buf[k - 1])) { show(poc_last(&d)); msgs++; }
            n += k;
            continue;
        }
        n++;
        if (poc_push(&d, s)) { show(poc_last(&d)); msgs++; }
    }
    if (poc_flush(&d)) { show(poc_last(&d)); msgs++; }
    fclose(f);
    printf("messages  : %d\nsyncs %u codewords %u fixed %u bad %u inverted %u\n",
           msgs, d.nSync, d.nCw, d.nFix, d.nBad, d.inv);
    return msgs ? 0 : 1;
}
