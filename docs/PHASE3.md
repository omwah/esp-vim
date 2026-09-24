# Phase 3 — The OS shim layer

**Status: complete.** Vim links, boots on the emulated ESP32-P4, draws its screen,
takes keystrokes, edits, saves, and quits cleanly. The file survives a reboot. The
userspace working directory works end to end. `pixi run vim-test` asserts all of it.

| | |
|---|---|
| Firmware | 2.18 MB (`esp-vim.bin`), **70% of the 7 MB app partition free** |
| Undefined symbols | **0** (was 16 at the end of Phase 2) |
| Stubs fired in the gate | **0** |
| Gate | `pixi run vim-test`: edit round trip + CWD round trip, both across a reboot |

## What the gate proves

`esp-vim/test/roundtrip.sh` runs two scenarios. Each is a pair of emulator runs:
the first injects keystrokes and saves flash state (`--save-state`), the second
boots the *saved* image, where `app_main` prints `/fat/vimtest.txt` back.

- **edit**: insert mode, ESC, `:w /fat/vimtest.txt`, `:q!` → reads back
  `hello from interactive vim`.
- **cwd**: `:call mkdir('sub')`, `:cd sub`, write `getcwd()` into the buffer,
  `:w ../vimtest.txt` → reads back **`cwd=/fat/sub`**. That covers a relative
  `mkdir`, the userspace `chdir`, `getcwd`, and `..` normalisation in one line.

It also fails on any stub firing, any `abort()`/assert/panic, or a missing clean exit.

## How the shim is built (`port/esp_shims.c`)

*(Superseded in Phase 5: the `mch_*` macro redirection below missed direct `stat()` calls in `os_unix.c`, so all path calls now use `--wrap`, and patch 0005 is retired. See [PHASE5.md](PHASE5.md), addendum.)*

Three mechanisms, each chosen for a reason:

| Mechanism | Used for | Why this one |
|---|---|---|
| Vim's `mch_*` macros redirected | `open`, `fopen`, `stat`, `lstat`, `access`, `unlink`, `rmdir` | Scoped to Vim only; no global effect on ESP-IDF. Needs patch 0005's `#ifndef` guards. |
| `-Wl,--wrap` | `chdir`, `getcwd`, `rename`, `mkdir`, `opendir`, `system`, `exit`, `_exit` | These already exist in ESP-IDF, so defining our own is a duplicate symbol, and defining them under their own names would recurse. `--wrap` gives `__real_*`. |
| Plain definitions | `getuid`/`getpwuid`/… , `signal`/`sigaction`/… , `pipe`, `dup`, `execvp`, `waitpid`, `setitimer`, `nanosleep`, `gethostname`, `umask` | Genuinely absent from ESP-IDF. |

`mch_rename` is the one macro **not** redirected: it's also a real function
declared in `proto/os_unix.pro`, and a function-like macro mangles that declaration.
`rename()` is wrapped instead.

Symbol collisions with ESP-IDF were found by diffing `libvim.a`'s globals against
every IDF archive: `f_mkdir` and `f_readdir` (Vim names its Vimscript builtins
`f_<name>`, which collides with FatFs's API) and `timer_start`. They're renamed with
`-D` inside the Vim component, so no upstream patch is needed.

## Bugs found and fixed on the way

**`exit()` panics the chip.** ESP-IDF's `_exit()` is literally `abort()`
(`components/newlib/src/syscalls.c:121`). Vim's `:q` reached it, so quitting became
a crash-and-reboot loop. `__wrap_exit` now parks the task and prints
`ESPVIM-EXIT rc=N ESPVIM-END` for the harness. The plan had predicted this, but it
wasn't implemented until the crash made it concrete.

**Crashes looked like silent hangs.** The P4's default secondary console is
USB-Serial-JTAG. The panic handler spins forever on its TX FIFO, which nothing
drains in the emulator (or on a board with no USB host attached). So crashes never
printed. Now fixed with `CONFIG_ESP_CONSOLE_SECONDARY_NONE=y`. The fault was found
by attaching GDB to the emulator and unwinding the Vim task by hand from its FreeRTOS
TCB, because the GDB stub only exposes the running hart.

**Interactive `select()` crashed when Vim ran on core 1.** It failed with
`assert failed: spinlock_acquire (lock)` under `xQueueSemaphoreTake` inside
`esp_vfs_select`. **Pinning the Vim task to core 0, the core that installed the UART
driver, fixed it completely.** The root cause is **not established**: it could be
ESP-IDF's cross-core select path or the emulator's multi-hart model. Retesting
unpinned on real silicon is now a Phase 9 gate. Keeping the editor on its console's
core costs nothing either way.

**`setitimer` was not unreachable.** It was written as an `ENOSYS` stub, and the
first-call log fired immediately. Vim arms it around regexp matching (`start_timeout`
in `os_unix.c`, called only from `regexp.c`), and turned the failure into
`E1286: Could not set timeout` on screen. With no signal delivery the alarm can never
fire, so it now succeeds and never times out. A slow regexp just runs to completion.
This is exactly what the log-on-first-call design was for.

**Test-harness races**, recorded so they aren't repeated:
- esp-emu's `--inject` understands only `\n` and `\r`. A literal `\x1b` is typed as
  four characters, so the harness passes a real ESC byte.
- `--exit-on` stops the emulator the moment its string appears, which truncated
  `ESPVIM-EXIT rc=0` mid-line. The line now ends in `ESPVIM-END`, and the harness
  triggers on that.
- Redirecting the emulator's stdin from `/dev/null` gives the UART an immediate EOF
  and Vim quits. The harness holds stdin open with `sleep`.
- `run-emu.sh --reuse` boots the previous run's saved flash instead of a fresh copy.
  That's what makes the two-run gate possible.

## Correction to Phase 1

`docs/PHASE1.md` said IDF has "no `chdir` implementation anywhere". That's
inaccurate: IDF *defines* `chdir()` and `getcwd()` in
`components/newlib/src/realpath.c:112-125`, as stubs. `chdir` is
`errno = ENOSYS; return -1` and `getcwd` hardcodes `"/"`. The conclusion stands
(there is no working directory), but the symbols exist. That's why Phase 3 has to
use `--wrap` rather than define them.

## Open items

- `E1187: Failed to source defaults.vim`: expected, since `/vimrt` is empty until
  Phase 4.
- Core-pinning root cause: retest unpinned on hardware (Phase 9).
- CTRL-C → `got_int` and raw-mode handling via the UART VFS (`mch_settmode`) are
  not yet done. Both are console-layer work for Phase 5.
