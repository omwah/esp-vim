# Phase 2 — Build system and generated files

**Status: complete.** All 127 Vim sources plus the 6 bundled xdiff sources compile
for ESP32-P4 with **zero errors**. What remains is 16 undefined symbols at link —
all genuine POSIX gaps, which constitute the Phase 3 worklist.

Reproduce: `pixi run deps && pixi run vim-build`.

## Measured

| | |
|---|---|
| Vim component `.text` | **1,817 KB** at `-Os` (2,050 KB at `-Og`) |
| `.data` / `.bss` | 138,646 / 39,810 bytes |
| Objects compiled | 136 |
| Largest TUs | `regexp.c` 174 KB, `evalfunc.c` 71 KB, `ex_docmd.c` 67 KB, `option.c` 60 KB |

1.82 MB sits comfortably inside the provisional 7 MB app partition, even before
MicroPython and the rest of Phase 6-8 land. `CONFIG_COMPILER_OPTIMIZATION_SIZE=y`
is now in `sdkconfig.defaults` — the partition table is the one thing that cannot
be resized once `/fat` holds user data.

## The three generated files

Vim's autoconf cannot cross-compile (its probes are `AC_TRY_RUN` tests that must
execute on target), so `configure` runs once on the **host** as a generator and its
output is curated by hand.

- **`port/auto/config.h`** — 70 defines, 70 documented `#undef`s, every deviation
  from the host's answer commented with its evidence. Type sizes were probed with
  `riscv32-esp-elf-gcc` rather than assumed, which caught the combination that bites:
  a 32-bit target with a **64-bit `time_t`** and a **32-bit `off_t`**.
- **`port/auto/osdef.h`** — near-empty by design. Supplies only what IDF neither
  declares nor defines: `lstat`/`readlink`/`symlink`/`link`, and the `wait` family
  (IDF ships `<sys/wait.h>` but declares nothing in it).
- **`port/pathdef.c`** — hand-written. The values that matter are
  `default_vim_dir` / `default_vimruntime_dir`, both `/vimrt`.

Note the layout: these live under `port/auto/` because `vim.h` includes
`"auto/config.h"` and `"auto/osdef.h"` by those exact paths.

## Patches to upstream (4)

Kept minimal and each to one concern, since they are re-applied against every
future Vim.

| Patch | Why |
|---|---|
| `0001-feature-h-esp-undefs` | One `#ifdef ESP_PLATFORM` block at the end of `feature.h` disabling clipboard/X11/Wayland, printer, cscope, netbeans, sound, sodium, xattr, profile — and **undefining `SPECIAL_WILDCHAR`**, which is what makes stubbing `mch_expand_wildcards()` safe (Phase 1). |
| `0002-os_unix-guard-tiocgwinsz` | `mch_calc_cell_size()` uses `TIOCGWINSZ` unguarded, while `mch_get_shellsize()` at line 4317 guards the same call. **An upstream oversight**, not a platform quirk — the fix is upstreamable. |
| `0003-os_unix-guard-term-set-winsize` | `term_set_winsize()` is defined inside `#if defined(HAVE_TGETENT)` in `term.c`, so the call site needs the same guard. Upstream's own comment at the call site predicts exactly this link error. |
| `0004-xdiff-config-include-path` | `xdiff/xinclude.h` includes `"../auto/config.h"`, relative to `xdiff/`. Changed to `"auto/config.h"` so it resolves via `-I`. |

## Component wiring

`components/vim/CMakeLists.txt` mirrors `BASIC_SRC` from Vim's own `src/Makefile`,
kept faithful rather than pruned — files that do not apply (GUI, X11, pango, cairo)
reduce to empty translation units via their own `#ifdef`s, so diffing against a
newer Vim's `BASIC_SRC` stays mechanical. Two things upstream's Makefile does that
had to be reproduced: `-Iproto` for the generated prototype headers, and the
separate `XDIFF_SRC` list — Vim bundles its own xdiff, which is what lets
`FEAT_DIFF` work with no external `diff` binary.

`-Dmain=vim_main` renames the entry point; `-include port/auto/config.h` forces our
config ahead of everything, so no Vim source needed editing to find it.

## Phase 3 worklist — the 16 undefined symbols

Empirically derived, not predicted:

| Group | Symbols |
|---|---|
| Process | `execvp`, `pipe`, `dup`, `waitpid` |
| Signals | `signal`, `sigaction`, `sigprocmask`, `sigpending`, `setitimer` |
| Identity | `getuid`, `getgid`, `getpwuid`, `getgrgid`, `gethostname` |
| Misc | `umask`, `nanosleep` |

`nanosleep` is the one surprise: `config.h` claims `HAVE_NANOSLEEP` because the
declaration exists, but there is no implementation. Phase 3 must supply one (a
`vTaskDelay` wrapper) or the define must go.

The process and signal groups are unreachable at runtime — there is no `fork()` and
`USE_SYSTEM` routes `:!` through `system()` — but they are compiled, so they need
stubs to link.
