/* Host test for the ZVEI tone sequence builder (App/app/zvei_seq.c).
 *   cc -I App -o /tmp/test_zvei tools/zvei/test_zvei_seq.c App/app/zvei_seq.c && /tmp/test_zvei
 */
#include <stdio.h>
#include <string.h>
#include "app/zvei.h"

static int fails;

static void check(const char *name, uint8_t type, uint32_t code, const uint16_t *want, uint8_t wantN)
{
    uint16_t got[ZVEI_DIGITS] = {0};
    const uint8_t n = ZVEI_BuildTones(type, code, got);
    int ok = n == wantN && (n == 0 || memcmp(got, want, n * sizeof(uint16_t)) == 0);
    printf("%s %-34s", ok ? "ok  " : "FAIL", name);
    for (uint8_t i = 0; i < n; i++) printf(" %u", got[i]);
    printf("\n");
    fails += !ok;
}

int main(void)
{
    const uint16_t c12345[] = { 1060, 1160, 1270, 1400, 1530 };
    const uint16_t c00000_1[] = { 2400, 2600, 2400, 2600, 2400 };     /* 0 R 0 R 0 */
    const uint16_t c00000_2[] = { 2400,  970, 2400,  970, 2400 };
    const uint16_t c11223_1[] = { 1060, 2600, 1160, 2600, 1270 };     /* 1 R 2 R 3 */
    const uint16_t c11223_2[] = { 1060,  970, 1160,  970, 1270 };
    const uint16_t c01234[] = { 2400, 1060, 1160, 1270, 1400 };       /* leading zero kept */
    const uint16_t c98765[] = { 2200, 2000, 1830, 1670, 1530 };
    const uint16_t c99999_1[] = { 2200, 2600, 2200, 2600, 2200 };

    check("ZVEI-1 12345",              ZVEI_TYPE_1, 12345, c12345, 5);
    check("ZVEI-2 12345 (no repeat)",  ZVEI_TYPE_2, 12345, c12345, 5);
    check("ZVEI-1 00000",              ZVEI_TYPE_1, 0,     c00000_1, 5);
    check("ZVEI-2 00000",              ZVEI_TYPE_2, 0,     c00000_2, 5);
    check("ZVEI-1 11223",              ZVEI_TYPE_1, 11223, c11223_1, 5);
    check("ZVEI-2 11223",              ZVEI_TYPE_2, 11223, c11223_2, 5);
    check("ZVEI-1 01234",              ZVEI_TYPE_1, 1234,  c01234, 5);
    check("ZVEI-1 98765",              ZVEI_TYPE_1, 98765, c98765, 5);
    check("ZVEI-1 99999",              ZVEI_TYPE_1, 99999, c99999_1, 5);
    check("type OFF refused",          ZVEI_OFF,    12345, NULL, 0);
    check("type out of range refused", 7,           12345, NULL, 0);
    check("code NONE refused",         ZVEI_TYPE_1, ZVEI_CODE_NONE, NULL, 0);
    check("code > 99999 refused",      ZVEI_TYPE_1, 123456, NULL, 0);

    printf("%s: %d failure(s)\n", fails ? "FAILED" : "PASSED", fails);
    return fails != 0;
}
