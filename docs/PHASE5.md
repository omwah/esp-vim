# Phase 5 — Emulator bring-up over UART

**Status: complete.** Vim is usable interactively over the UART, on both targets. The
same emulator gate passes unchanged on the **ESP32-P4** (the Tab5) and on the
**ESP32-S3** build variant.

| | ESP32-P4 | ESP32-S3 |
|---|---|---|
| Firmware | 2.18 MB (70% of app partition free) | 1.98 MB (73% free) |
| PSRAM given in emulation | 32 MB | 8 MB |
| `pixi run vim-test` / `vim-test-s3` | 16/16 pass | 16/16 pass |
| Code changes needed for S3 | none | none |

## The two gates

- **`esp-vim/test/roundtrip.sh`** (Phases 3–4): keys injected at boot, persistence
  across a reboot. Covers edit, working directory, runtime/filetypes, and regexp
  timeout.
- **`esp-vim/test/interactive.py`** (new): Vim used the way a person uses it.
  `esp-vim/test/uart_session.py` is a small expect() over esp-emu's `--uart-tcp`
  socket that types **after** the screen is drawn, while Vim is busy, and answers a
  terminal size query. It checks size adoption, typing, CTRL-C, poll cost, mouse
  reporting, no `OOPS`, and no Vim errors.

`pixi run vim-test` runs both on the P4, and `pixi run vim-test-s3` runs both on the
S3. The target comes from `ESPVIM_TARGET`, and each target builds in its own
`esp-vim/build-<target>/` with its own `sdkconfig`, via `scripts/vim-build.sh`.

## Findings

**1. CTRL-C needed no code.** Without signals I expected to have to turn a raw 0x03
into `got_int` myself. Vim already does it. In raw mode `mch_breakcheck()` polls the
console with a zero-timeout `select()`, and `fill_input_buf()` converts a raw CTRL-C
into an interrupt. Test: a `:while 1` loop is interrupted after 3 s and the editor
recovers. (An early failure was the test's fault: Vim **flushes typeahead** on
interrupt, so keys typed immediately after CTRL-C are discarded. A person waits;
the test now does too.)

**2. Vim's "is a key waiting?" poll cost 2 ms, and now costs ~25 µs.** ESP-IDF's
`select()` converts a timeout to ticks as `ceil(ms/tick) + 1`, so a **zero** timeout
still sleeps for up to one FreeRTOS tick (10 ms at the default 100 Hz). Vim makes
this poll constantly: during loops, redraws, and `char_avail()`. Measured: 200 polls
took **400 ms** against 9 ms for 200 plain loop iterations. The port now wraps
`select()`. A zero-timeout call on fds with a registered non-blocking "input
pending?" check is answered immediately, and everything else goes to ESP-IDF
unchanged. 200 polls: **14 ms**. The UART registers its check in `app_main`, and
the Phase 10 display console will register its own through the same hook
(`port/esp_vim_port.h`).

**3. Vim printed `OOPS` and never enabled the mouse. Upstream bug, patch 0006.**
Without a terminal library Vim expands escape sequences with a minimal `tgoto()`
that only understands `%i %d %+ %%`, and returns the literal string `"OOPS"` for
anything else. A scan of `builtin_xterm` found exactly one such entry that's live in
a no-terminfo build: `t_XM`, the mouse on/off string. It uses a terminfo
conditional and, unlike its neighbours, has no non-`TERMINFO` alternative. So every
mouse toggle sent `OOPS` to the screen and the mouse stayed off, even though
`defaults.vim` sets `mouse=a`. The patch moves it inside `#ifdef TERMINFO`, and
`mch_setmouse()` then falls back to the standard sequences. The system vimrc pins
`ttymouse=sgr`, the only xterm mouse protocol that works past column 223. Verified:
`?1002h` + `?1006h` on at start, both off at exit, and no `OOPS`.

**4. The terminal's size is now probed, not hardcoded.** A UART has no
`TIOCGWINSZ`. Before starting Vim, `app_main` parks the cursor at `999;999` and asks
for a cursor position report (`CSI 6n`). A real terminal clamps to its size and
answers, and those numbers become `LINES`/`COLUMNS`. With no answer within 300 ms (a
scripted run, a log capture) the 24×80 default stays. The boot log says which:
`ESPVIM-TERM 40x120 (probed)` or `... (default: no answer from terminal)`.

**5. The S3 variant needed configuration, not code.** The shim layer sits on
newlib + VFS + FreeRTOS, which are identical on both chips, as Phase 3 was designed
to be. `sdkconfig.defaults` is now target-neutral, and `sdkconfig.defaults.esp32s3`
sets octal PSRAM, 240 MHz (it defaults to 160), and a 64 KB data cache, since Vim's
heap is in PSRAM.

**Harness lessons**, recorded so they aren't relearned:
- Match screen output only against markers that **can't appear in the echoed command
  line** (`'L' . '='`, not `'L='`).
- Terminate every value read back from the screen (`… . '|'`). UART output arrives in
  arbitrary chunks, and `\d+` happily matches `40x12` while the final `0` is still in
  flight.
- A terminal query can straddle two socket reads, so scan a rolling window.

## Using it by hand

```sh
pixi run vim-build                                      # or vim-build-s3
pixi run emu esp-vim -- --uart-tcp 127.0.0.1:5555       # terminal 1
pixi run emu-tty                                        # terminal 2 (Ctrl-] detaches)
```

For the S3: `pixi run emu esp-vim --chip esp32s3 --psram 8M -- --uart-tcp 127.0.0.1:5555`.
The size probe asks your real terminal through `socat`, so Vim opens at your window's
size.

## Open items

- A terminal *resize* during a session isn't noticed (there's no `SIGWINCH`).
  Rerunning `:set lines= columns=` works. The Tab5 console knows its own size (Phase 10).
- The P4 core-pinning root cause is still open (Phase 9, on silicon).

## Addendum: fixes from first hands-on use

Three problems were reported within minutes of using `pixi run emu-tty`, and one of them
exposed a fourth. All four are now in the interactive gate (18 checks).

**Backspace did nothing.** Terminals send DEL (0x7f) for Backspace, and Vim learns which
byte it is from the tty's erase character, `c_cc[VERASE]`. ESP-IDF's UART `tcgetattr()`
zeroes the entire `struct termios` and never fills `c_cc[]`, so Vim got 0 and kept
`builtin_xterm`'s `^H`. `tcgetattr()` is now wrapped. When a console leaves every control
character zero, it reports the POSIX/Linux defaults (`VERASE` = `^?`, `VINTR` = `^C`, and
so on). A console that sets its own values is left alone.

**`:e z<Tab>` completed nothing.** `mch_getperm()` and `mch_isdir()` in `os_unix.c`
call `stat()` directly (upstream comment: "Keep the #ifdef outside of stat(), it may be a
macro"). So they bypassed the redirected `mch_*` macros and never saw the userspace
working directory: `isdirectory('.')` was 0, and relative globs, and with them
completion, found nothing. **Every path-taking libc call is now interposed at link time
with `--wrap`**: `open fopen stat access unlink remove rmdir chmod`, alongside the
existing `chdir getcwd rename mkdir opendir`. The `mch_*` macro redirection is gone, and
so is **patch 0005**, which only existed to make those macros overridable.

**`:help` is now a device guide.** `/vimrt/doc/help.txt` is an amended version of Vim's
help.txt. It keeps Vim's navigation preamble and replaces the index of documentation that
isn't on the device with sections on files and storage, filetypes, keys and the terminal,
differing settings, personal configuration, and what isn't available. It's rendered at
build time from `esp-vim/runtime-image/doc/help.txt.in`. The filetype table is generated
from `filetypes.conf`, so it can't disagree with detection. `doc/tags` is generated too,
and **the build fails if any `|link|` points to a tag that doesn't exist**.

**Getting `:help` working exposed a real bug: Vim thought every file was the same file.**
FATFS reports `st_dev = st_ino = 0` for all files (as Phase 1 recorded), and Vim decides
"same file?" by comparing exactly those fields (`fullpathcmp()`, buffer identity). So
`:help` concluded the help file *was* the current buffer, and searched the wrong buffer
(`E434`). Switching between two existing files was equally unreliable. The shim now
synthesises an identity: a 32-bit FNV-1a hash of the normalised, **case-folded**
absolute path (FAT is case-insensitive), split across the 16-bit `st_dev` and 16-bit
`st_ino`, and never equal to Vim's `(dev_T)-1` "stat failed" sentinel. `fstat()` must
agree, or every `:w` over an existing file fails with `E949: File changed while writing`
(`bufwrite.c` compares the two). So fds opened through the wrappers remember their
identity until `close()`/`fclose()`.

Accepted limitation: two different files whose paths hash identically (about 1 in 4×10⁹
per pair) would be treated as one.

## Addendum: quitting starts a new session

`:q` used to park the Vim task and leave the device dead. Vim *is* the device, so there
was nowhere to go. Now the last window closing shows

    All buffers closed.  Vim is forever.
    Press any key to start a new session.

and the next key starts Vim again **in place, without a reboot**. Other tasks and (in
later phases) services keep running.

Vim was never written to be started twice: its globals are initialised once and its
heap is never freed. So a new session puts **all** of Vim's state back to power-on:

| State | How it is reset |
|---|---|
| `.data` (initialised globals) | restored from a snapshot taken in `app_main` before any Vim code ran |
| `.bss` (zeroed globals) | zeroed |
| heap | every block Vim allocates carries a 16-byte header linking it into a list; the list is freed. Blocks come from ESP-IDF's PSRAM heap, under a budget (half of free PSRAM, at most 16 MB) |
| open files / directories | closed, but **only those Vim opened** (see below) |
| regexp-timeout timer | deleted |
| the call stack | each session is a **fresh FreeRTOS task**; `exit()` ends the task and a supervisor in `app_main` starts the next one after a key press |

Two earlier designs were replaced. A dedicated `multi_heap` arena for Vim corrupted its
TLSF bookkeeping on the S3 (below), and a `setjmp`/`longjmp` back into `app_main`
left the old call chain's stack in an undefined state. A task per session makes the
stack question disappear. Tracked blocks from the system heap avoid a second allocator,
and a foreign `free()` can't damage them the way it can an arena carved out of another
heap.

`components/vim/linker.lf` brackets every writable section of `libvim.a` with
`_vim_{data,bss}_{start,end}`. On RISC-V the component is built with
`-msmall-data-limit=0`: otherwise small globals like `got_int` and `curbuf` land in
`.sdata`/`.sbss`, which ESP-IDF's `data`/`bss` schemes don't bracket, and the "reset"
would leave exactly the most important state behind. Verified from the linker map on
both chips: every live Vim section is inside the brackets, nothing else is, and there
are no small-data sections.

**Lesson — ownership.** The first version closed every file the wrappers had seen, and
immediately crashed on boot. `--wrap` is global, so ESP-IDF's own `fopen()` of stdout
at startup had been recorded, and session reset **closed stdout**. Only files and
directories opened *on the Vim task during a session* now belong to Vim. That matters
even more once a web server shares the filesystem (Phase 6).

The gate checks that sessions 2, 3 and 4 each start at power-on state (after deliberately
dirtying the previous session with a global, an option, the working directory, buffers,
`:help`, netrw and an armed timer), and that internal RAM doesn't leak across quits.

**Found on the way — a Phase 4 runtime hole.** Opening any Vim-script file failed with
`E282`/`E1053`: `indent/vim.vim` does `import autoload '../autoload/dist/vimindent.vim'`,
and the Phase 4 resolver only followed `name#func()` calls, not Vim9 `import` paths.
Phase 4's "no errors" gate never opened a `.vim` file. The resolver now follows all three
import forms, and **the build fails if any import in the image doesn't resolve**. The
runtime gate scenario now opens a Vim-script file too.

## Addendum: splash, help files and a busy indicator

Three things a first-time user sees, all found by hands-on use.

**The intro screen names the chip.** It now opens with the title line *Vim running on
ESP32-P4* (or *ESP32-S3*). The name is derived from `IDF_TARGET` by the component
CMakeLists (`esp32p4` → `ESP32-P4`) and passed as `ESP_VIM_CHIP`. Patch
`0007-version-intro-chip-title` adds the line, and `:version`'s "Compiled by" line uses
the same name. `pathdef.c` had hard-coded `esp32p4` and the RISC-V compiler, which was
wrong on the S3.

**The intro screen no longer lies about help.** It advertises `:help version9`,
`:help sponsor` and `:help Kuwasha`, and none of them existed. The runtime image now
ships, next to our `help.txt`:

| File | Size | Note |
|---|---|---|
| `version9.txt` | 55 KB | upstream is **2 MB**, 98% of it the one-line-per-patch lists; those four sections keep their heading and tags, and lose their body |
| `uganda.txt`, `sponsor.txt` | 15 KB | as upstream |
| `netrw.txt` | 147 KB | the file browser's manual, which `help.txt` points to |

These files are written against Vim's full documentation, so most of their links point at
files that aren't on the device. The image builder turns those links into plain text
(666 of them), leaving links that work. It skips example blocks, where `|x|` is code.
Our own `help.txt` is still held to the strict rule: a dangling link fails the build. The
runtime partition went from 74% to 84% full.

`help.txt` names the chip it was built for (`@CHIP@` in the template). That makes the
runtime image **per target**: `build-deps/vimrt-esp32p4/` and `vimrt-esp32s3/`.

**Busy indicator** (`port/esp_busy.c`). On a microcontroller a new file type's syntax,
`:help`, or a large file can take a noticeable moment, and a silent terminal looks like
a hung device. After 0.3 s of work a spinner (`| / - \`) turns on the bottom row, one
cell in from the right, until Vim next waits for a key. `:let g:esp_busy = 0` turns it
off. It needs no Vim patch, because the select() wrapper already sees both signals:

- a zero-timeout poll of the console is Vim checking for CTRL-C mid-work, so draw a frame;
- a real wait is Vim idle, so put the cell back from Vim's own screen model (`screen_char`)
  and return the cursor.

Both run **on the Vim task, through Vim's output buffer**. So a frame can never land
inside one of Vim's escape sequences, which a separate task writing to the UART could.
Frames are wrapped in DECSC/DECRC, so Vim's cursor and attributes are untouched. It isn't
the very last cell because `builtin_xterm` has no `xn`: Vim won't redraw that cell, and
writing it can scroll some terminals.

The gate checks the splash title, `:help version9`/`sponsor`/`Kuwasha`/`netrw`, the chip
name in `help.txt` and `:version` (and no "Tab5"), and that spinner frames appear.

## Known issue: heap corruption on the ESP32-S3 emulator -- an emulator defect

**Cause found (2026-09-30): esp-emu, not Vim or the port.** On the S3, some emulator
runs die with a TLSF assertion in ESP-IDF's heap (`block_trim_free`/`block_merge_prev`:
"block must be free"), a `LoadStoreError` inside `tlsf_malloc`, or a silent reboot (the
emulator's log then says `Bus fault: write32 unmapped`), almost always while Vim is
sourcing syntax files. The P4 has never shown it.

**The defect.** The S3 ROM's `strcpy()` copies in a zero-overhead loop (Xtensa's `LOOP`)
and leaves it by branching to the loop's own end address (`LEND`):

```
400555ca: loop  a8, 400555e1        ; LEND = 400555e1
            ...
400555dc:   bnone a8, a7, 400555e1  ; the NUL was the word's last byte: leave
400555e1: retw.n
```

(its byte-at-a-time loop, used when source and destination alignments differ, ends the
same way). On silicon a taken branch to `LEND` leaves the loop. Under esp-emu, when an
interrupt is taken at that branch, the loop goes round again, and the copy carries on to
the *next* NUL. GCC emits the same pattern for a `break` out of a counted loop, so ROM
`strcpy` is only the most frequent victim.

**How it was found.** A debug build with guarded heap blocks
(`CONFIG_ESP_VIM_HEAP_GUARD`, below) caught the damage as it happened, in
`syn_cmd_keyword()`/`add_keyword()` while `syntax/cmake.vim` loaded: a new keyword entry
held its keyword and NUL, and right after it, over the guard, the *next* keyword from the
command's buffer. No live block overlapped it and the heap's header for it was sane, so
nothing had overrun it and the heap had not handed out the same memory twice: the copy
itself had gone on too far. Then a standalone S3 app (`esp-vim/test/emu-s3-loop/`, no Vim
code, `pixi run emu-s3-loop`) showed it on its own:

| | overran, of 960,000 `strcpy()` calls |
|---|---|
| esp-emu 0.43.0 | 34 (every alignment, every byte position of the NUL) |
| esp-emu 0.44.0 | 54 |
| esp-emu 0.43.0, batch size 500 to 1,000,000 | 32 to 38: not the emulator's batching |
| esp-emu 0.43.0, interrupts masked around each copy | **0** |

Two strings overwritten into TLSF's free-list pointers were legible in the bus faults:
`SIZEOF_DATA_PTR` and `STANDARD_DEFAULT`, both from `cmake.vim`'s keyword lists.

**Silicon.** The same stress (every filetype, then large files highlighted, repeatedly)
ran on the Freenove FNK0115 through a serial bridge. It showed no heap corruption.
It did run Vim out of its 2.5 MB budget (37 buffers, each with its own syntax
tables), and Vim then crashed rather than reporting `E342`: `findtags_state_init()` in
upstream `tag.c` uses an `ALLOC_ONE()` result without checking it. The backtrace
reaches it through a `:help` run while `syntax/vim.vim` was being sourced, which is not
yet understood (perhaps a line mangled by the failed allocations). That is a separate,
upstream bug.

**Consequences.**
- The S3 emulator gate stays informational: esp-emu 0.44.0 is no better (3 of 5
  interactive runs failed, against 2 of 6 on 0.43.0). `pixi run emu-s3-loop` says when
  a new esp-emu can be trusted.
- It explains why the private `multi_heap` arena and plain `heap_caps_malloc` failed
  alike: the allocator was never the problem. Whether it also explains the boot fault
  with ESP-IDF's heap poisoning on is not checked.
- To report upstream (espressif/esp-emulator), with `emu-s3-loop` as the reproducer.

**The guard** (`CONFIG_ESP_VIM_HEAP_GUARD`, a debug option, off in every build): each
Vim block gets a canary on both sides and remembers its allocator; freed blocks are
filled and held in a quarantine, so a write after free or a double free is caught; the
newest 32 blocks are checked on every call, and everything (with ESP-IDF's heap) every
`CONFIG_ESP_VIM_HEAP_GUARD_WALK` calls. A failure prints `ESPVIM-GUARD` lines naming the
block, its neighbours and the bytes around it. The harness takes `ESPVIM_EMU` (another
esp-emu) and `ESPVIM_EMU_LOG` (keep the emulator's own messages).
