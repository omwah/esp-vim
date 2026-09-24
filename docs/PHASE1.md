# Phase 1 — Capability spike results

**Verdict: GO.** `isatty(0)` is true, so Vim will render rather than fall into filter
mode. 20 of 21 gates pass; the one failure is `chdir`, which has a known shim.

Run on emulated ESP32-P4 (rev 3.01, 2 cores) under `esp-emu` 0.43.0 with
`--psram-size 32M`, ESP-IDF **v5.5.5**, 2026-09-23. Raw output:
[`phase1-spike-results.txt`](phase1-spike-results.txt). Source:
`esp-vim/test/spike/`. Reproduce with:

```sh
. ~/esp/esp-idf/export.sh
cd esp-vim/test/spike && idf.py build
../../../scripts/run-emu.sh . --exit-on SPIKE-DONE --timeout 60s
```

## The gate

| # | Question | Result |
|---|---|---|
| 1 | **`isatty(0)`** | **true** — before *and* after installing the UART driver. **The port is viable.** |
| 2 | `select()` on fd 0 | blocks correctly: 256 ms of a 250 ms timeout. No busy-loop risk. |
| 3 | `tcgetattr`/`tcsetattr` | work — **but only after `uart_vfs_dev_use_driver()`**; before it, `tcgetattr` fails `EINVAL`. Raw-mode flags read back as set. |
| 4 | Writable FAT `/fat` | mount, mkdir, write, read, opendir, readdir, rename, unlink all pass. |
| 5 | Read-only FAT `/vimrt` | mounts, reads, `opendir` works, and writes are correctly refused (`EACCES`). |
| 6 | Glob on the startup path | answered from source, below. |

## Findings that change the plan

### 1. ESP-IDF has no working directory at all — `chdir` is the one failing gate

`chdir()` returns `ENOSYS` and `getcwd()` always answers `/`. This is not a missing
Kconfig option: grepping IDF v5.5.5 finds **no `chdir` implementation anywhere** in
`components/`. It is newlib's stub.

Vim needs a CWD for `:cd`/`:lcd`, for `mch_dirname()`, and to resolve relative paths.
**The port must supply a userspace one** in `port/esp_shims.c`: an `esp_cwd` string, with
`mch_chdir()`/`mch_dirname()` operating on it and relative paths resolved to absolute
before they reach the VFS.

The good news is that this is cheap to wire in, because Vim funnels file access through
two macros defined at `vim.h:2582-2583`:

```c
# define mch_open(n, m, p)  open((n), (m), (p))
# define mch_fopen(n, p)    fopen((n), (p))
```

Redefining those in the port's config header routes every open through a CWD-resolving
wrapper **without patching a single call site**. `stat`, `opendir`, `unlink`, `mkdir` and
`rename` need the same treatment.

### 2. Most of the POSIX surface Vim expects is simply absent

Probed with weak symbols so absence is a runtime observation rather than a link error:

| Present | Absent |
|---|---|
| `system()` (returns -1), `chmod()` | `fork()`, `getuid()`, `geteuid()`, `getpwuid()`, `getpwnam()`, `signal()`, `lstat()`, `readlink()`, `symlink()`, `link()`, `umask()` |

`fork()` being absent is expected and fine — it is why `USE_SYSTEM` exists. The rest all
need shims. Note **`lstat` is not even declared** in IDF's headers: it is a *compile*
error before it is ever a link error, so the port must supply the declaration too.

`signal()` being absent confirms the plan's design: CTRL-C must be recognised as byte
`0x03` in the input reader and turned into `got_int`. There is no other path.

### 3. `stat` is as degraded as predicted, plus no clock

`st_ino = 0`, `st_dev = 0` — so Vim's same-file detection is blind, confirming that
backup-by-rename must stay off. Also `st_mtime = 315532800` — 1980-01-01, the FAT epoch —
because there is no RTC. Anything comparing file times needs to tolerate that.

### 4. Question 6 answered from source: stubbing `mch_expand_wildcards` is safe

`gen_expand_wildcards()` (`filepath.c:4100`) delegates to `mch_expand_wildcards()` — the
shell-globbing path — in only three cases:

1. re-entrant calls, which return `FAIL` outright when `SPECIAL_WILDCHAR` is undefined;
2. patterns containing a `SPECIAL_WILDCHAR`, which is `` "`'{" `` (`os_unix.h:383`);
3. a UNIX-only fallback when `expand_env()` leaves a `$` or `~` unexpanded.

**Undefining `SPECIAL_WILDCHAR` for ESP removes cases 1 and 2 entirely**, and case 3
cannot fire for runtime sourcing once `HOME` and `VIMRUNTIME` are set. Everything else
uses Vim's internal matcher. So returning `FAIL` from `mch_expand_wildcards()` is safe for
startup; the cost is that `{a,b}` brace expansion and backtick expansion stop working —
an acceptable documented limitation.

## Other measurements

- Free heap at `app_main`: **34,126,983 bytes** — PSRAM is mapped and usable.
- `sizeof(void*)` = 4 (32-bit).
- Spike app: 0x4ef90 (~323 KB) — no signal yet about Vim's eventual size.
- Bootloader: 0x5cb0, with only 0x350 (3%) free in its 0x2000 slot. Worth watching if
  bootloader options change.

## Consequences for later phases

- **Phase 2** gains a required item: `mch_open`/`mch_fopen` redefinition plus the CWD
  shim, and `config.h` must declare `lstat` rather than merely disable `HAVE_LSTAT`.
- **Phase 3**'s stub list is now evidence-based rather than predicted.
- The console must call `uart_vfs_dev_use_driver()` **before** Vim touches termios.
