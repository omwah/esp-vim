# Draft issue for espressif/esp-emulator (not yet filed)

**Title:** ESP32-S3: an interrupt taken at a branch to `LEND` restarts the zero-overhead loop (ROM `strcpy` overruns)

---

**Version:** esp-emu 0.43.0 and 0.44.0 (x86_64 Linux release builds), `--chip esp32s3`.
ESP-IDF v5.5.5.

**Summary.** When a taken branch inside an Xtensa zero-overhead loop (`LOOP`) targets the
loop's end address `LEND`, silicon leaves the loop. Under esp-emu, if an interrupt is taken
at that branch, execution goes round the loop again instead. The ESP32-S3 ROM's `strcpy()`
exits its loops this way, so now and then it copies past the terminating NUL, up to the
next one. In a real application this corrupts heap metadata a few minutes into a run
(TLSF assertions such as "block must be free", `LoadStoreError` in `tlsf_malloc`, or
`Bus fault: write32 unmapped` in the emulator's log).

**The code.** ROM `strcpy` at `0x40055580`, word loop:

```
400555ca: loop  a8, 400555e1        ; LEND = 400555e1
            ...
400555dc:   bnone a8, a7, 400555e1  ; NUL in the word's last byte: leave the loop
400555e1: retw.n
```

The byte-at-a-time loop, used when source and destination alignments differ, leaves the
same way (`beqz a8, LEND`). GCC emits the same pattern for a `break` out of a counted loop,
so this is not specific to the ROM.

**Reproducer.** An app that calls `strcpy` 960,000 times with every length from 0 to 39
and every source and destination alignment. After each copy it checks the byte after the
destination string, which should be untouched.

`CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(emu-s3-loop)
```

`main/CMakeLists.txt`:

```cmake
idf_component_register(SRCS "main.c" PRIV_REQUIRES esp_timer)
```

`main/main.c`:

```c
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
            for (int so = 0; so < 4; so++) {
                for (int dof = 0; dof < 4; dof++) {
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
```

Build and run:

```
idf.py set-target esp32s3 && idf.py build
cd build
python -m esptool --chip esp32s3 merge_bin -o merged.bin --flash_size 4MB @flash_args
esp-emu --chip esp32s3 --firmware merged.bin --exit-on EMU-S3-LOOP-RESULT --timeout 600s
```

**Results.**

| | copies that overran, of 960,000 |
|---|---|
| esp-emu 0.43.0 | 34 (at every alignment and every NUL position) |
| esp-emu 0.44.0 | 54 |
| esp-emu 0.43.0, `--batch-size` 500 to 1,000,000 | 32 to 38 |
| esp-emu 0.43.0, interrupts masked around each `strcpy` | 0 |

**Expected:** `0 of 960000 copies overran`, `EMU-S3-LOOP-RESULT PASS`, as on silicon.

**Likely cause** (a guess; we have not read the emulator's source). When the interrupt is
taken, the emulator seems to decide the loop
exit by comparing the next PC with `LEND` (the rule that applies when execution falls
through to `LEND`). A taken branch whose target happens to be `LEND` should leave the loop
without decrementing `LCOUNT`, and without jumping back to `LBEG`. That should hold whether
or not an interrupt is taken at that point, and also for the return from the interrupt.
