# Decisions

Reasoning behind choices in [PLAN.md](PLAN.md), and the working conventions that the plan
assumes but does not spell out. PLAN.md says *what*; this says *why*, and records changes
of mind. Append new entries at the end with a date.

---

## Working conventions

### Patch authoring

Upstream source is never committed. `third_party/` holds LFS archives, `patches/<dep>/`
holds our changes, and `scripts/prepare-deps.sh` extracts and applies them into the
git-ignored `build-deps/`. Nobody hand-writes a `.patch`, so the workflow is:

```sh
pixi run deps                                  # clean tree, existing patches applied
$EDITOR build-deps/vim/src/whatever.c          # make the change
pixi run mkpatch vim 0006-short-description    # diff against a pristine reference
pixi run deps                                  # confirm it applies from scratch
```

`mkpatch` extracts a pristine copy of the archive, applies the existing series to it,
and diffs that against your edited tree, so the patch holds exactly your new change.

Rules that matter:

- **Never run git inside `build-deps/`.** It sits inside this repository's worktree,
  so a git command there operates on the *outer* repo. See the 2026-09-23 entry on
  `mkpatch` below for how that went wrong.
- Numbered prefixes set the apply order (`0001-`, `0002-`, …), applied lexically.
- `prepare-deps.sh` applies with `patch -p1` (never `git apply`) and **verifies each
  patch changed what it claims to**. A no-op apply is a hard error, not a silent
  success.
- Keep each patch to one concern, with a descriptive name. They're re-applied against
  every future Vim, so a reviewer must be able to tell what each is for without
  reading the diff.

### Re-syncing to a newer upstream

1. `scripts/add-dep.sh` (or regenerate the archive) for the new version; update the
   manifest record and its sha256.
2. `scripts/prepare-deps.sh vim` and see which patches fail.
3. Refresh those by the workflow above.
4. Re-run the Phase 5 emulator round trip — it is the regression gate for everything.

---

## 2026-09-23 — Vim, not Neovim

Neovim was assessed and rejected on three independent grounds, any one of which is
disqualifying:

- **LuaJIT has no RISC-V backend.** No RV32 port exists, so the P4 cannot run Neovim's
  default Lua.
- **The PUC-Lua fallback is broken upstream** — neovim/neovim#36516, "building NeoVim for
  RISC-V *without* LuaJIT", is open, with build scripts assuming LuaJIT's presence.
- **libuv is the deeper blocker.** Neovim's event loop is libuv, which has no ESP-IDF
  platform and no generic `poll()` backend — it wants `epoll`, which ESP-IDF's lwIP does
  not provide. Porting libuv exceeds the cost of porting Vim outright.

Vim's `os_unix.c` is a few dozen `mch_*` functions over plain POSIX. That is the tractable
surface.

## 2026-09-23 — FEAT_NORMAL, not FEAT_TINY

The project was first named `vim-tiny-p4`, but `+small`/`+big` no longer exist upstream
(`feature.h:44-59` aliases them to `TINY`/`NORMAL`) and 16 MB flash / 32 MB PSRAM means
size is not the binding constraint. `FEAT_TINY` has no `+eval` at all — no `defaults.vim`,
no syntax, no scripting — and upstream issue #18393 suggests it is a lightly exercised
configuration. Chose `FEAT_NORMAL` directly rather than bringing up on TINY first, at the
cost of a larger porting surface on the first boot: `+eval` and `+syntax` mean the runtime
partition must work before Vim will even start.

## 2026-09-23 — Git in MicroPython, not C

There is no libgit2 port for ESP-IDF, so git had to be implemented either way. Once
MicroPython was in scope, pure Python became the better language for it: far less flash
than a compiled git, editable on the device, and `benhoyt/pygit` is proven prior art —
~500 lines of stdlib Python doing init/add/commit/status/diff **and push**.

The hot paths stay in C (`hashlib.sha1`, `deflate`, the HTTPS/SSH transports, Vim's
bundled `xdiff`), so Python only orchestrates. Dulwich was rejected as CPython-scale: it
needs `typing`, `dataclasses`, `os.scandir`, `tempfile`, `urllib` and ABCs.

Consequence: **git depends on MicroPython**, fixing Phase 7 before Phase 8.

## 2026-09-23 — Tab5 keyboard in Normal mode

The keyboard accessory's firmware offers Normal (row/column), HID, and Character modes.
Character mode looks convenient but **consumes `Sym`/`Aa` inside the keyboard**, and the
device has **no F1–F12 row**. Normal mode hands us raw matrix coordinates so we own the
whole keymap, which is the only way to guarantee `Esc` is never swallowed, that `Ctrl-`
combinations reach Vim intact, and that F-keys can be synthesised on the `Sym` layer.

Consequence: the two-pane file manager cannot depend on F3–F8 and carries letter aliases
as equal citizens.

## 2026-09-23 — The three cross-task shared objects

Vim's globals are single-threaded by construction, but the web server runs handlers in its
own task. Rather than let that spread, exactly three objects are shared, each with one
writer:

| Object | Writer | Reader |
|---|---|---|
| `esp_api_fs.c` state | either task, under a mutex | either task |
| status snapshot | **Vim task only** | httpd |
| settings queue | httpd enqueues | **Vim task applies** |

The hard rule: **httpd handlers never call into Vim's API** — not even `emsg()`. MicroPython
runs on the Vim task and inherits its guarantee rather than needing its own.

## 2026-09-23 — Userspace CWD via the mch_open/mch_fopen macros

Phase 1 found ESP-IDF has no working directory at all: `chdir()` is newlib's `ENOSYS` stub
and `getcwd()` always returns `/`. Grepping IDF v5.5.5 confirms there is no implementation
to enable — this is not a Kconfig option we missed.

Vim needs a CWD for `:cd`, `mch_dirname()`, and relative-path resolution. Rather than
patch Vim's many call sites, the port keeps an `esp_cwd` string and redefines the two
macros Vim already funnels file access through (`vim.h:2582-2583`):

```c
# define mch_open(n, m, p)  open((n), (m), (p))
# define mch_fopen(n, p)    fopen((n), (p))
```

Redefining these in the port's config header resolves relative paths to absolute before
they reach the VFS, with no upstream patch. `stat`, `opendir`, `unlink`, `mkdir` and
`rename` get the same wrapper treatment.

## 2026-09-23 — Undefine SPECIAL_WILDCHAR

`gen_expand_wildcards()` only shells out to `mch_expand_wildcards()` for re-entrant calls,
for patterns containing `` "`'{" `` (`SPECIAL_WILDCHAR`, `os_unix.h:383`), and for a UNIX
fallback when `expand_env()` leaves `$`/`~` unexpanded. Undefining `SPECIAL_WILDCHAR`
eliminates the first two; setting `HOME`/`VIMRUNTIME` eliminates the third for runtime
sourcing.

So `mch_expand_wildcards()` can safely return `FAIL` and everything routes to Vim's
internal matcher. Cost: no brace or backtick expansion. Accepted — there is no shell to
expand them with anyway.

## 2026-09-23 — Host tools via pixi, not apt

Phase 2 needs ncurses headers (Vim's `configure` refuses to finish without a terminal
library, even though the ESP build undefines `HAVE_TGETENT`), Phase 5 needs `socat`, and
Phase 8 needs a local `sshd`. None were installed.

Chose pixi over `apt` for three reasons: no root, versions pinned so a fresh clone gets
the same host toolchain, and it is the tool already in use here. All three are on
conda-forge.

Pinning **python 3.13** in the same environment solved a separate problem. The system
Python is 3.14 with no `ensurepip` and no `python3.14-venv` package, so ESP-IDF's
installer — which runs `python -m venv` — could not bootstrap. The first attempt worked
around this by creating IDF's venv with a third-party tool; pinning a Python that has
`ensurepip` removes the workaround entirely and lets IDF bootstrap itself the normal way.

Consequence: ESP-IDF's venv is built against pixi's Python, so **pixi must be active
before `export.sh`**. `scripts/env.sh` does both in order, and every pixi task that
touches the cross toolchain sources it.

## 2026-09-23 — The spike is kept as a regression test

`esp-vim/test/spike/` outlived Phase 1 deliberately. Its findings are not documentation —
they are the assumptions the entire shim layer is built on ("chdir is ENOSYS", "signal is
absent", "termios is partial", "st_ino is 0"), and those can change underneath us.

Re-run it at exactly two points:

1. **On real Tab5 silicon (Phase 9).** Every Phase 1 answer came from an emulator. Diff
   the output against `docs/phase1-spike-results.txt`.
2. **On any ESP-IDF version change.** IDF 6.x already flipped the VFS TERMIOS default. The
   case worth catching is the inverse: if IDF gains a real `chdir`, the userspace CWD shim
   should be *deleted*, not carried forever.

It is **not** a per-board test. An ESP32-S3 run would answer nearly identically, because
the POSIX surface comes from newlib + VFS and is chip-independent; what differs between P4
and S3 is ISA, PSRAM ceiling and peripherals, none of which the spike probes.

If a future maintainer finds neither trigger has fired in a long time, deleting it is a
reasonable call — but delete it consciously, not by letting it rot.

## 2026-09-23 — scripts/mkpatch.sh, and never running git inside build-deps/

The patch-authoring workflow first written here said to `git init` a throwaway repo
inside `build-deps/<dep>`. That is a trap, for two compounding reasons:

1. `scripts/prepare-deps.sh` re-extracts the tree on every run, deleting that `.git`.
2. `build-deps/` sits **inside this repository's worktree**, so once the throwaway
   `.git` is gone, a git command run there silently operates on the *outer* repo
   instead. `git diff` then produces an empty patch, and `git stash` touches real work.

The same root cause produced a worse bug: `git apply` resolves patch paths against the
repository root, so `a/src/feature.h` was read as `<repo>/src/feature.h`, fell outside
`build-deps/vim`, and was **silently ignored with exit code 0** — reporting success
having changed nothing.

Two fixes:

- `prepare-deps.sh` applies patches with `patch -p1`, never `git apply`, and verifies
  by hashing the files each patch claims to touch before and after. A patch that
  reports success but changes nothing is now a hard error.
- `scripts/mkpatch.sh` generates patches without any git involvement: it extracts a
  pristine reference copy, applies the existing series to it, and diffs that against
  the edited tree. The difference is exactly the new change.

**Do not run git commands inside `build-deps/`.**

## 2026-09-23 — Three interposition mechanisms, not one

Phase 3 needed to intercept file and process calls and ended up using three
mechanisms. Each exists for a reason, so they shouldn't be "simplified" into one.

1. **Redirect Vim's own `mch_*` macros** (`open`, `fopen`, `stat`, `lstat`, `access`,
   `unlink`, `rmdir`). Preferred wherever it works, because it's scoped to Vim and has
   no effect on ESP-IDF. Costs one small upstream patch (0005) adding `#ifndef` guards.
2. **`-Wl,--wrap`** (`chdir`, `getcwd`, `rename`, `mkdir`, `opendir`, `system`,
   `exit`, `_exit`). Used when ESP-IDF already defines the symbol, since defining our
   own is a duplicate. It's also used when the override needs the original, since
   defining `rename()` inside `rename()` recurses. The cost is global scope: ESP-IDF's
   own calls go through the wrapper too. That's harmless here, because absolute paths
   pass through unchanged.
3. **Plain definitions** for what ESP-IDF genuinely lacks.

The failed attempts, so they aren't retried:

- A function-like macro for `chdir`/`getcwd`. It also rewrites the prototype in
  `<unistd.h>`, where `getcwd(char *__buf, size_t __size)` expands as though the
  parameter declarations were arguments.
- Overriding `mch_rename`. It's also a real function declared in `proto/os_unix.pro`,
  so the macro mangles that declaration.

## 2026-09-23 — The Vim task is pinned to core 0 (root cause open)

Unpinned, the Vim task could run on core 1, and its first `select()` on the console
crashed inside `esp_vfs_select` with `assert failed: spinlock_acquire (lock)`. Pinned
to core 0, where the UART driver and its ISR were installed, the crash went away
completely and the full gate passes.

**The root cause isn't established.** It could be ESP-IDF's cross-core select path or
esp-emu's multi-hart model, and nothing so far distinguishes them. The pin stays
because it costs nothing: an editor has no use for the second core, and keeping it on
its console's core is sensible regardless. Phase 9 retests unpinned on silicon.

If the pin is ever removed, rerun `pixi run vim-test` *and* the interactive case. The
crash only happened when Vim reached its input wait.

## 2026-09-23 — Stubs must be honest about "cannot happen" vs "cannot do"

Every stub originally did the same thing: fail loudly with `ENOSYS` and log on first call.
`setitimer` showed the flaw. It was assumed unreachable, but it's on the regexp path,
and its `ENOSYS` surfaced to the user as `E1286: Could not set timeout`.

The rule now:

- If a call is **truly unreachable** (`execvp`, `pipe`, `waitpid`, `dup`), fail and log.
- If it's **reachable but meaningless** here (`setitimer`, `signal`, `sigaction`,
  `sigprocmask`, `umask`), succeed quietly and document why the missing effect doesn't
  matter.

Log-on-first-call is what exposed the misclassification within one run. Keep it on
every stub in the first group.

## 2026-09-24 — Vim's heap lives in PSRAM (reverses the Phase 3 plan)

The plan said "heap from PSRAM, small allocations kept internal". Under ESP-IDF's
`SPIRAM_MALLOC_ALWAYSINTERNAL=16384`, that meant nearly all of Vim's allocations went to
internal RAM, and loading the real runtime exhausted it. ESP-IDF's internal-only
allocations then failed (flash reads with `ESP_ERR_NO_MEM`, an `abort()` in newlib's lock
init).

Vim's `malloc`/`calloc`/`realloc` are renamed by `-D` inside the Vim component, to
wrappers that prefer PSRAM and fall back to any heap. The change was deliberately *not*
made globally (e.g. `ALWAYSINTERNAL=0`), because drivers and networking benefit from
internal RAM. `free()` needs no rename. The exit line prints `int_min`, the internal-RAM
low-water mark, on every run so a regression is visible.

## 2026-09-24 — Backtracking regexp engine, with a real timeout

`regexpengine=1`, set in the system vimrc. Measured on stock desktop Vim, the first file
opened costs 26.5 MB under the automatic/NFA engine and 0.6 MB under backtracking. The
reason originally given for NFA ("backtracking recurses on the stack") was wrong:
`regexp_bt.c` uses a heap `regstack`.

Backtracking's real risk is exponential time. That's covered by making Vim's regexp
timeout work: `setitimer()` arms an `esp_timer`, which invokes the SIGALRM handler Vim
registered through `sigaction()`. The gate proves a pathological search is cut off at
300 ms.

## 2026-09-24 — Filetypes are a configured subset, and filetype.vim is generated

Stock `filetype.vim` has ~1600 detection rules, each compiled into a regexp program on
first use. The device needs perhaps 30. `esp-vim/filetypes.conf` lists them, and
`make-runtime-image.py` generates `filetype.vim` from it and ships only those types'
syntax/ftplugin/indent files. Idle memory dropped from 1.7 MB to 0.35 MB, and startup
no longer sources a 56 KB detector.

Extensibility is preserved without reflashing through Vim's standard `ftdetect/` hook
(`/fat/.vim/ftdetect/*.vim`). The `scripts.vim` fallback still recognises `#!` scripts.

Consequence to remember: a filetype that isn't in the config gets **no** detection at
all, not a best-effort guess. That's intentional and tested (`x.php` → no filetype).

## 2026-09-24 — PATH is set to an empty directory

Leaving `$PATH` unset looked harmless, since there are no programs. But Vim's
`mch_can_exe()` returns -1 when it can't search, and Vim script treats -1 as true, so
`executable()` claimed every tool existed. `PATH=/fat/bin` (nonexistent/empty) makes it
answer 0. Anything that probes for an external program now gets the honest answer.

## 2026-09-24 — Zero-timeout select() is answered by the port

ESP-IDF's `select()` turns a zero timeout into a wait of up to one tick. Vim polls
"is a key waiting?" with a zero timeout constantly, and each poll measured 2 ms. The
port wraps `select()` and answers zero-timeout calls itself, but **only** for fds
that have a registered non-blocking "input pending?" callback
(`esp_vim_register_input_poll`). Any other fd, or any real timeout, goes to ESP-IDF
untouched.

The callback is registered by whoever owns the console (the UART in `app_main`
today, the display console in Phase 10), so the shim never hard-codes a device.

## 2026-09-24 — Terminal size by cursor-position probe

With no `TIOCGWINSZ` over a UART, `app_main` asks the terminal directly: cursor to
`999;999`, then `CSI 6n`. The reply becomes `LINES`/`COLUMNS`. A 300 ms timeout keeps
the 24×80 default for non-interactive runs. A live resize isn't tracked (no
`SIGWINCH`), and that's an accepted limitation.

## 2026-09-24 — Patch 0006: t_XM is terminfo-only

Third upstream fix of the same class as 0002/0003: a terminfo-only construct not
guarded by `#ifdef TERMINFO`. Here it was the mouse enable string, which the fallback
`tgoto()` turns into the literal `OOPS`. A scan of `builtin_xterm` for directives the
fallback can't expand found only this one, so the class is closed for xterm.

## 2026-09-24 — Per-target build directories

`scripts/vim-build.sh <target>` builds in `esp-vim/build-<target>/` with its sdkconfig
inside it. `sdkconfig.defaults` never names a target, and `sdkconfig.defaults.<target>`
carries the chip-specific settings. `run-emu.sh` picks `build-<chip>/` when it exists,
and still falls back to `build/` for single-target projects such as the spike.

## 2026-09-24 — One interposition mechanism: link-time --wrap (supersedes the three)

The "three interposition mechanisms" entry above is superseded. Redirecting Vim's `mch_*`
macros turned out to be incomplete: `os_unix.c`, which *implements* `mch_getperm()`,
`mch_isdir()` and others, calls `stat()`/`open()` directly, on purpose. Relative paths
leaked past the userspace working directory, and filename completion found nothing.

All path-taking calls are now interposed with `-Wl,--wrap`, which catches every caller
however it's written. Patch 0005, which added `#ifndef` guards to the macros, is
**retired**. The numbering gap is deliberate, so that references to 0006 stay valid.
Absolute paths (everything ESP-IDF itself uses) pass through the wrappers unchanged,
so the global scope that made macros look attractive costs nothing in practice.

Plain definitions remain only for what ESP-IDF genuinely lacks (`getuid`, `signal`,
`pipe`, …).

## 2026-09-24 — Synthesized file identity (st_dev/st_ino)

FATFS gives every file `st_dev = st_ino = 0`. Phase 1 recorded this and concluded only
that "backup-by-rename must stay off". That underestimated it: Vim's same-file test
(`fullpathcmp()`, buffer identity) compares exactly those fields, so all existing files
were one file to Vim. `stat()`/`fstat()` now report an identity hashed from the
normalised, case-folded path. `fstat()` must agree with `stat()` because `bufwrite.c`
compares them on every overwrite (`E949` otherwise). See docs/PHASE5.md, addendum.

## 2026-09-24 — The device's :help is generated and link-checked

Only one help file ships: an amended `help.txt` rendered from
`esp-vim/runtime-image/doc/help.txt.in`, plus a generated `doc/tags`. The build refuses a
`|link|` to a nonexistent tag, so the help can't quietly point at documentation that
isn't there. The filetype section is generated from `filetypes.conf`. Anything else asked
of `:help` gives `E149`, and the file says so and points at vimhelp.org.

## 2026-09-24 — :q restarts Vim in place, not by rebooting

Quitting Vim must not leave the device dead. A reboot would be simpler, but it takes a
second, prints boot noise, and would kill every other service (web server, networking;
Phase 6). So a new session restores Vim's own memory to power-on instead: `.data` from a
snapshot, `.bss` zeroed (bracketed by `components/vim/linker.lf`), every heap block Vim
allocated freed (each carries a tracking header), and each session runs as a fresh
FreeRTOS task, so no stale stack is ever resumed. See docs/PHASE5.md, addendum.

Two consequences to respect:

- **Everything with session lifetime must live inside Vim's sections or be cleaned up
  explicitly.** The session object and the supervisor live in `main`, outside, because
  they have to survive the reset. The console poll registry
  lives inside, so it's re-registered each session.
- **Cleanup touches only what Vim owns** (opened on the Vim task during a session). The
  wrappers are global, and the first version closed ESP-IDF's stdout.

The heap budget also caps Vim's memory, which Phases 7–8 (MicroPython, git) will want.

Superseded on the way: a private `multi_heap` arena (a second allocator, and it showed
the S3 corruption below as readily as anything else) and `setjmp`/`longjmp` back into
`app_main` (a new session would run on top of the dead one's stack).

## 2026-09-24 — The runtime resolver must follow every way scripts load scripts

Twice now a runtime file was missing because the resolver didn't know a loading
mechanism: C code loading files by name (Phase 4), and Vim9 `import` paths (Phase 5).
Both now have build-time checks. The general rule: when the resolver learns a new
reference form, add a **validation** that fails the build, not just inclusion logic, and
make the gate open a file of every kind that exercises it.

## 2026-09-24 — Ship the help files the intro screen names; strip their dead links

The intro screen tells you to type `:help version9`, `:help sponsor` and `:help Kuwasha`.
Rather than patch those lines out, we ship the files: `version9.txt` without its patch
lists (55 KB instead of 2 MB), `uganda.txt`, `sponsor.txt`, and netrw's manual. Links in
upstream files that point at documentation not on the device become plain text at image
build time. A visible link that answers E149 is worse than no link. Our own help
stays strict: a dangling link there fails the build.

`help.txt` names the chip, so the runtime image is built per target.

## 2026-09-24 — The busy indicator lives in the select() wrapper, on the Vim task

A spinner drawn by a separate task (a timer, say) would be simpler to reason about, and
wrong: its bytes could land in the middle of one of Vim's escape sequences on the shared
UART. Instead the existing select() wrapper, which already tells "Vim is polling for
CTRL-C mid-work" (zero timeout) from "Vim is waiting for a key" (real wait), draws and
erases on the Vim task through Vim's own output buffer. No Vim patch. The cost: work that
never polls for CTRL-C shows no spinner. Every long loop in Vim polls, because that's
how CTRL-C works.

## 2026-09-24 — The ESP32-S3 gate is informational until tested on silicon

An intermittent S3-only heap corruption predates Phase 5's restart work and survives
every allocator arrangement and single-core mode. It doesn't reproduce in a Vim-free
PSRAM stress test under the same emulator. See docs/PHASE5.md, "Known issue". The P4
gate must pass. The S3 gate is run and reported, but doesn't block, until real S3
hardware says whether this is the emulator or us.

## 2026-09-24 — prepare-deps: `git check-ignore` needs the trailing slash

The guard that refuses to extract into a tracked directory used
`git check-ignore -q build-deps`. `.gitignore` says `/build-deps/`, a directory
pattern, and on a fresh clone where `build-deps/` doesn't exist yet the slashless form
doesn't match, so the script refused to run. It now checks `build-deps/`. Found by
building the previous commit in a fresh worktree.

## 2026-09-24 — The esp_*() builtins live in the Vim component

The plan put them in a separate `components/esp_vim_api/`. They need Vim's private
headers and must allocate through Vim's (session-tracked) heap, so a separate component
would have to reach into the Vim component's private include path and allocator anyway.
They're in `components/vim/api/`. The single-site patch (0008) is unchanged in spirit,
and a build-time check (`scripts/check-builtins.py`) guards the table's sort order.

## 2026-09-24 — Generated sdkconfig is regenerated when the defaults change

`build-<target>/sdkconfig` is an output. Once it exists, ESP-IDF prefers it to
`sdkconfig.defaults*`, so edits to the defaults silently never applied. `vim-build.sh`
deletes it when a defaults file is newer. Consequence: `menuconfig` changes don't
survive a defaults edit. That's intended: configuration belongs in the committed
defaults.

## 2026-09-24 — esp_fs is its own component; the file manager is autoloaded

The file-operations core (`components/esp_fs`) is the only place paths are validated, and
the web server will call it from another task. So it can't live in the Vim component,
where `malloc` is Vim's session-tracked heap and a restart resets everything. Vim reaches
it through thin `esp_fs_*()` builtins. Long operations take a progress callback rather
than knowing about Vim, which is how CTRL-C and the spinner work during a copy.

`:EspFiles` is `autoload/espfiles.vim`, not a `pack/*/start` package. A start package's
plugin files are sourced at every startup; an autoload file costs nothing until first use.

## 2026-09-24 — In the emulator, the P4's network is its Ethernet

The plan expected emulator networking to need the two-emulator esp-hosted setup (a C6
slave image) or an unverified EMAC path. A throwaway test app showed `esp-emu` models the
P4 EMAC well enough for ESP-IDF's driver with the generic PHY: DHCP, DNS, HTTP. So 6c
builds and tests the whole network stack (HTTP, TLS, netrw, spell download) on one
emulator. The cost, to keep in view: this isn't the Tab5's production path (WiFi through
the C6), which 6f and Phase 9 must still prove.

## 2026-09-24 — netrw's http method calls esp_http_get(), by patch

netrw is where Vim's network file access lives, and `spellfile.vim` depends on it. Its
http method builds a wget/curl command line. Rather than re-implementing `:Nread` or
replacing `spellfile.vim`, patch 0009 adds a branch in front: if `esp_http_get()` exists,
use it. One hunk, inert everywhere else, and every netrw feature built on http reads
(`:e`, `:Nread`, spell download) works unchanged.
