/*
 * Does strcpy() stop at the NUL? The ESP32-S3 ROM's strcpy copies in a
 * zero-overhead loop (Xtensa's LOOP instruction) and leaves it by branching
 * to the loop's end address. Under esp-emu 0.43.0 and 0.44.0, when an
 * interrupt is taken at that branch, the loop goes round again instead of
 * ending, and the copy carries on to the next NUL. On silicon it does not.
 * About 35 in 960,000 copies overrun under the emulator; none with interrupts
 * masked around the copy.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "esp_timer.h"

static char src[96] __attribute__((aligned(4)));
static char dst[96] __attribute__((aligned(4)));

void app_main(void)
{
    unsigned long bad = 0, runs = 0;
    int64_t t0 = esp_timer_get_time();
    for (int rep = 0; rep < 1500; rep++) {
        for (int len = 0; len < 40; len++) {
            for (int so = 0; so < 4; so++) {            /* every source ... */
                for (int dof = 0; dof < 4; dof++) {     /* ... and destination alignment */
                    char *s = src + so, *d = dst + dof;
                    memset(src, 'x', sizeof src);
                    memset(s, 'a', len);
                    s[len] = 0;
                    s[len + 9] = 0;                     /* a next string, 8 bytes on */
                    memset(dst, '#', sizeof dst);
                    strcpy(d, s);
                    runs++;
                    if (d[len + 1] != '#' && bad++ < 5)
                        printf("EMU-S3-LOOP strcpy copied past the NUL: length %d, source +%d,"
                               " destination +%d\n", len, so, dof);
                }
            }
        }
    }
    printf("EMU-S3-LOOP %lu of %lu copies overran (%lld ms)\n", bad, runs,
           (esp_timer_get_time() - t0) / 1000);
    printf("EMU-S3-LOOP-RESULT %s\n", bad ? "FAIL" : "PASS");
}
