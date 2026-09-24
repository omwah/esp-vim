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
