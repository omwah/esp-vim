# Phase 4 — Storage and the curated runtime

**Status: complete.** Vim starts with a real `$VIMRUNTIME`: `defaults.vim`, filetype
detection, 256-colour syntax highlighting, filetype plugins and indent, netrw. It
reports **no errors**, and memory use is small and bounded. `pixi run vim-test` covers
all of it.

| | |
|---|---|
| Runtime image | 135 files, **~2.2 MB of the 3 MB `vimrt` partition (74%)** |
| Filetypes | **30**, from `esp-vim/filetypes.conf` (stock Vim: ~1600 rules) |
| PSRAM used by Vim | **0.35 MB** idle, **0.47 MB** after opening a file |
| Internal RAM low-water | ~302 KB free (it reached zero before this phase's fix) |
| Gate | `pixi run vim-test`: edit, cwd, runtime, timeout. Fails on any Vim error, any stub, any panic, or >8 MB PSRAM |

## What was built

**`esp-vim/filetypes.conf`** is the single source of truth for filetype support.
Each line is a filetype and its file patterns. `scripts/make-runtime-image.py` reads it
to **generate** `/vimrt/filetype.vim` with exactly those rules, and to ship only those
filetypes' `syntax/`, `ftplugin/` and `indent/` files. The generated file keeps three
things from the stock one:

- backup-suffix handling (`foo.c.orig`, `foo.c~` detect as `c`)
- the standard `runtime! ftdetect/*.vim` hook, so types can be added **on the device**
  in `/fat/.vim/ftdetect/` without reflashing
- the `scripts.vim` fallback, so extensionless `#!` scripts are still recognised

The 30 shipped types include `kconfig` and `conf` for `Kconfig` and `sdkconfig`, so the
device can edit ESP-IDF projects.

**`scripts/make-runtime-image.py`** (`pixi run runtime`, run automatically by
`vim-build`) copies a curated slice of Vim's runtime out of `build-deps/`, overlays our
files from `esp-vim/runtime-image/`, and writes `build-deps/vimrt/`. The firmware build
turns that into the FAT image. Dependencies are resolved to a fixed point:
`syntax/`, `ftplugin/`, `indent/`, `autoload/` and `:compiler` references are all
followed. Anything that's referenced but never loaded by default is excluded, with the
reason recorded in the script. It **refuses to build** if the estimated FAT footprint
exceeds 90% of the partition. FAT rounds every file up to a 4 KB cluster, so raw bytes
understate the real size.

**`esp-vim/runtime-image/vimrc`** is the system vimrc (`$VIM/vimrc`), and holds
what's platform-specific: `encoding=utf-8`, no swap/backup/undo/viminfo (flash wear),
`nomodeline` (files arrive over the network), `regexpengine=1`, and colour setup.

## Findings

**1. Vim's heap was exhausting internal RAM.** *This was the Phase 3 plan's mistake.*
With `SPIRAM_MALLOC_ALWAYSINTERNAL=16384`, every allocation under 16 KB (which is nearly
all of Vim's) went to internal RAM first. Sourcing the real runtime exhausted it, and
ESP-IDF's own internal-only allocations then failed. Flash reads returned
`ESP_ERR_NO_MEM`, and newlib `abort()`ed while creating a FILE lock. **Fix:** Vim's
`malloc`/`calloc`/`realloc` are renamed with `-D` *inside the Vim component only*, to
wrappers that prefer PSRAM. Internal RAM stays free for FreeRTOS, drivers and, later,
networking.

**2. The NFA regexp engine cost ~20 MB just to open a file.** Opening any file with a
filetype took ~20 MB of PSRAM. This isn't a port bug: a stock desktop Vim built from
the same source spends **26.5 MB** in the same scenario. The cost is compiling
filetype.vim's ~1600 autocmd patterns as NFA programs. With the backtracking engine it
drops to **0.6 MB** on the desktop. My original reason for choosing NFA ("the
backtracking engine recurses deeply") was **wrong**. `regexp_bt.c` keeps its state on
a heap `regstack` capped by `'maxmempattern'`, not on the C stack. Its real weakness is
exponential *time* on pathological patterns, which is what the regexp timeout exists
for. So:

**3. The regexp timeout is now real.** `setitimer(ITIMER_REAL)` arms a one-shot
`esp_timer`, which calls the SIGALRM handler Vim registered via `sigaction`. That
handler just sets the flag both engines poll. Proven by the gate: `\v(a|aa)+b` against
40 `a`s, exponential under backtracking, is abandoned at **300 ms** by `search()`'s
timeout.

**4. Then the filetype list was cut to what the device needs** (your call). That took
Vim's footprint from 1.7 MB idle / 1.0 MB per first open to **0.35 / 0.47 MB**.

**5. `builtin_xterm` has no colour entries.** A normal build gets `t_Co`/`t_AB`/`t_AF`
from terminfo, which this build doesn't have (`HAVE_TGETENT` is off). So `t_Co` was 0,
`defaults.vim` never turned syntax on, and highlighting would have been monochrome
anyway. The system vimrc sets `t_Co=256` and the plain `"\e[3%dm"` forms, which Vim's
`term_color()` upgrades to `38;5;N` by itself. Verified: 46 colour escapes and six
distinct colours when a Vim file is displayed.

**6. `$PATH` unset made every tool look installed.** With `PATH` unset,
`mch_can_exe()` returns -1 ("can't tell"), and Vim script treats -1 as **true**. So
`executable('shellcheck')` said yes, and `ftplugin/sh.vim` failed with `E666`.
`app_main` now sets `PATH=/fat/bin`, an empty directory, so `executable()` honestly
answers 0.

**7. Runtime files loaded from C are invisible to a Vimscript resolver.** `highlight.c`
sources `colors/lists/default.vim` to resolve GUI colour names (`E254` when it's
missing), and `:options` needs `optwin.vim`. Both were found by grepping `src/*.c` and
`src/*.h` for `"*.vim"` literals, and are now listed explicitly with the reason.

**Guard added:** `vim.h`'s `#include "auto/config.h"` searches `vim.h`'s own directory
before any `-I` path. So running Vim's `configure` on the host (as this phase did, to
build a desktop Vim for comparison) would silently compile the **host's** settings into
the firmware. The component's CMake now refuses to configure if
`build-deps/vim/src/auto/config.h` exists.

## Open items

- The partition table is still provisional (7 MB app / 3 MB `vimrt` / ~5.5 MB `/fat`)
  until MicroPython and git exist (Phases 7–8). Current use: app 2.18 MB, runtime 2.2 MB.
- Omni-completion engines aren't shipped. `CTRL-X CTRL-O` in e.g. CSS gives `E117`.
- `:help` still has no documentation (a 12 MB `doc/` tree won't fit).
