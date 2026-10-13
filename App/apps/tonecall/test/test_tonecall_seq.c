/* Host test for the ToneCall tone sequence builder (../tonecall_seq.c).
 *   cc -o /tmp/test_tonecall test/test_tonecall_seq.c tonecall_seq.c && /tmp/test_tonecall
 */
#include <stdio.h>
#include <string.h>
#include "../tonecall_seq.h"

static int fails;

static void check(const char *name, uint8_t type, const char *code, const uint16_t *want, uint8_t wantN)
{
    uint16_t got[TC_MAX_DIGITS] = {0};
    const uint8_t n = TC_BuildTones(type, code, got);
    int ok = n == wantN && (n == 0 || memcmp(got, want, n * sizeof(uint16_t)) == 0);
    printf("%s %-34s", ok ? "ok  " : "FAIL", name);
    for (uint8_t i = 0; i < n; i++) printf(" %u", got[i]);
    printf("\n");
    fails += !ok;
}

static void checkMs(const char *name, uint8_t type, uint16_t want)
{
    const uint16_t got = TC_ToneMs(type);
    printf("%s %-34s %u ms\n", got == want ? "ok  " : "FAIL", name, got);
    fails += got != want;
}

int main(void)
{
    /* ZVEI */
    const uint16_t z12345[]   = { 1060, 1160, 1270, 1400, 1530 };
    const uint16_t z00000_1[] = { 2400, 2600, 2400, 2600, 2400 };     /* 0 R 0 R 0 */
    const uint16_t z00000_2[] = { 2400,  970, 2400,  970, 2400 };
    const uint16_t z11223_1[] = { 1060, 2600, 1160, 2600, 1270 };     /* 1 R 2 R 3 */
    const uint16_t z11223_2[] = { 1060,  970, 1160,  970, 1270 };
    const uint16_t z01234[]   = { 2400, 1060, 1160, 1270, 1400 };     /* leading zero kept */
    const uint16_t z98765[]   = { 2200, 2000, 1830, 1670, 1530 };
    const uint16_t z99999_1[] = { 2200, 2600, 2200, 2600, 2200 };
    const uint16_t z21414_2[] = { 1160, 1060, 1400, 1060, 1400 };     /* Canal E alarm */
    const uint16_t z111_1[]   = { 1060, 2600, 1060 };                 /* 1 R 1, short code */
    const uint16_t z8x_1[]    = { 2000, 2600, 2000, 2600, 2000, 2600, 2000, 2600 };
    /* CCIR */
    const uint16_t c12345[]   = { 1124, 1197, 1275, 1358, 1446 };
    const uint16_t c11223[]   = { 1124, 2110, 1197, 2110, 1275 };     /* 1 R 2 R 3 */
    const uint16_t c00000[]   = { 1981, 2110, 1981, 2110, 1981 };
    const uint16_t c06789[]   = { 1981, 1540, 1640, 1747, 1860 };

    check("ZVEI-1 12345",              TC_ZVEI1, "12345", z12345, 5);
    check("ZVEI-2 12345 (no repeat)",  TC_ZVEI2, "12345", z12345, 5);
    check("ZVEI-1 00000",              TC_ZVEI1, "00000", z00000_1, 5);
    check("ZVEI-2 00000",              TC_ZVEI2, "00000", z00000_2, 5);
    check("ZVEI-1 11223",              TC_ZVEI1, "11223", z11223_1, 5);
    check("ZVEI-2 11223",              TC_ZVEI2, "11223", z11223_2, 5);
    check("ZVEI-1 01234",              TC_ZVEI1, "01234", z01234, 5);
    check("ZVEI-1 98765",              TC_ZVEI1, "98765", z98765, 5);
    check("ZVEI-1 99999",              TC_ZVEI1, "99999", z99999_1, 5);
    check("ZVEI-2 21414",              TC_ZVEI2, "21414", z21414_2, 5);
    check("ZVEI-1 111 (3 digits)",     TC_ZVEI1, "111", z111_1, 3);
    check("ZVEI-1 88888888 (max)",     TC_ZVEI1, "88888888", z8x_1, 8);
    check("CCIR-1 12345",              TC_CCIR1, "12345", c12345, 5);
    check("CCIR-2 12345",              TC_CCIR2, "12345", c12345, 5);
    check("CCIR-1 11223",              TC_CCIR1, "11223", c11223, 5);
    check("CCIR-2 00000",              TC_CCIR2, "00000", c00000, 5);
    check("CCIR-1 06789",              TC_CCIR1, "06789", c06789, 5);
    check("type out of range refused", TC_TYPE_COUNT, "12345", NULL, 0);
    check("empty code refused",        TC_ZVEI1, "", NULL, 0);
    check("non-digit refused",         TC_ZVEI1, "12a45", NULL, 0);
    check("9 digits refused",          TC_CCIR1, "123456789", NULL, 0);

    checkMs("ZVEI-1 tone",   TC_ZVEI1, 70);
    checkMs("ZVEI-2 tone",   TC_ZVEI2, 70);
    checkMs("CCIR-1 tone",   TC_CCIR1, 100);
    checkMs("CCIR-2 tone",   TC_CCIR2, 70);
    checkMs("invalid tone",  TC_TYPE_COUNT, 0);

    printf("type names:");
    for (uint8_t t = 0; t < TC_TYPE_COUNT; t++) printf(" %s", tcTypeNames[t]);
    printf("\n%s: %d failure(s)\n", fails ? "FAILED" : "PASSED", fails);
    return fails != 0;
}
