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

## 2026-09-24 — SSH through the registry's libssh2, unvendored; device keys are ECDSA

`skuodi/libssh2_esp` (libssh2 1.11, mbedTLS) works unmodified, so it's a pinned managed
component rather than an LFS archive with patches: that is Phase 0's rule for
dependencies we don't patch. Its mbedTLS backend has no Ed25519, so keys the device makes
are ECDSA P-256. That's also what the web server's certificate will use (6e), and it's
far cheaper than RSA on these chips.

Host keys are trust-on-first-use with a fingerprint the user can check. A changed key is
never accepted by the software; the user must delete the old line. SSH exists here to
move files and, later, to push git. Silently accepting a changed key would defeat it.

## 2026-09-24 — Web status is polled, not pushed

The plan chose Server-Sent Events for the live status. `esp_http_server` serves requests
on one task, so a held-open event stream would stall every other request, or need an
async-handler worker task for one feature. A once-a-second poll of `/api/status` is
simpler, survives reconnects for free, and costs nothing at this rate. The Vim side
publishes only on change.

## 2026-09-25 — Vim's stack is static; on the S3 its .bss is in PSRAM

WiFi on the ESP32-S3 left too little contiguous internal RAM to create Vim's 64 KB task
stack at run time. So the stack is reserved at link time, and to make that fit, Vim's
40 KB `.bss` moves to PSRAM on the S3 only (the P4 has room). A PSRAM *stack* was the
alternative and was rejected: a task whose stack is in PSRAM can't do flash operations,
and Vim writes files. `.bss` in PSRAM is safe because only task-level code touches it,
never interrupts or cache-off paths. It costs some speed on the S3's hottest globals,
which hasn't been measured on hardware yet.

## 2026-09-25 — The Tab5 talks to its C6 with esp-hosted 2.12.13

*Superseded on 2026-09-30: 1.4.0, see below.*

esp-hosted 3.0 (July 2026) restructured the project: new Kconfig names, a new
co-processor example (`wifi/sta/cp`), and software-aggregated SDIO by default. 2.12.x
is the long-running line that `esp_wifi_remote` was written against (it asks for
`>=2.11`). Its co-processor example also offers BLE over HCI, which Phase 6f's BLE work
and Phase 11's keyboards need. Neither line could be tested end to end, because
esp-emu's SDIO bridge stalls on host-to-co-processor traffic for both (see PHASE6.md,
6f part 3). So the older, more widely deployed line is the lower-risk choice for first
contact with real hardware. Whichever version is chosen, both ends must match, so
`scripts/c6-build.sh` builds the C6 firmware from the version esp_net pins. Revisit
3.x once the Tab5 works.

## 2026-09-25 — Build variants, not just targets

The Tab5 and the emulator's P4 are the same chip with different networks (WiFi through
the C6, or Ethernet), and the Ethernet MAC claims the Tab5's internal I2C pins. So the
unit of building is a *variant*: a chip plus an optional `sdkconfig.defaults.<variant>`,
with its own build directory and component lock. The alternative, one P4 build with the
network picked in menuconfig, would make the emulator build and the shipped build
silently the same directory, one sdkconfig edit apart.

## 2026-09-25 — Bluetooth is tested against Bumble, and starts on first use

esp-emu forwards the firmware's HCI traffic to a TCP server (`--ble-hci`). Bumble, a
Python Bluetooth stack with a virtual controller and a virtual radio link, makes both
ends of a scan scriptable in one process, with no radio hardware or BlueZ on the build
host. The alternative, the host's own adapter through `hci0`, needs an HCI user channel,
which takes elevated privileges (CAP_NET_ADMIN), and depends on what is on the air
nearby. Bumble is PyPI-only and
comes in through pixi's `pypi-dependencies`. It also has HID support, which is how Phase
11's keyboard pairing can be tested on the S3.

NimBLE starts on the first `:EspBleScan`, not at boot. On the S3, internal RAM is already
the limit with WiFi up, and an editor session that never scans shouldn't pay for
Bluetooth. Only the observer role is built; Phase 11 adds what keyboards need.

## 2026-09-25 — ESP-IDF fixes are patches applied to a copy of the component

ESP-IDF 5.5.5's `esp_hid` has a bug (its NimBLE HID host skips the HID service).
ESP-IDF is installed separately and isn't ours to edit, and the repo commits no
third-party source. So the fix is a patch, `patches/esp-idf/0001-...`.
`esp-vim/CMakeLists.txt` copies `esp_hid` into the build directory at configure
time, applies the patch, and adds the copy as a project component, which overrides
ESP-IDF's by name. The copy is redone whenever the patch changes. A different ESP-IDF
version that the patch doesn't fit stops the build, rather than building unpatched.

## 2026-09-25 — WiFi and Bluetooth start only when wanted

On the ESP32-S3, the WiFi driver, the Bluetooth stack and the display don't all fit
in internal RAM at once. Nothing radio-related starts at boot unless it has
something to do:
- WiFi starts at boot only if a network is stored (a flag in NVS, set by a connect and
  cleared by a disconnect), otherwise at the first scan or connect.
- Bluetooth starts at boot only if a keyboard is bonded, otherwise at the first scan
  or pairing.

A board used as a Bluetooth-keyboard editor never pays for WiFi.

## 2026-09-25 — Bluetooth keyboards pair with "Just Works"

HID keyboards differ: some take a passkey typed on them, many take none and
reject one. Just Works bonds with every keyboard that accepts it. Asking to pair, by command now and on the touch screen later, is
the user's confirmation, as the plan's security notes require. Passkey keyboards
need esp_hidh's passkey request handled, which ESP-IDF's HID host doesn't do; that
waits for a keyboard that needs it.

## 2026-09-25 — Switching build variants reconfigures

The component manager has one `managed_components/` per project, not per build
directory. Configuring one variant prunes components that only others use: the
display driver, esp-hosted. A later incremental build of another variant then
fails on missing sources. `scripts/vim-build.sh` records which variant configured
last (`build-deps/managed-components-variant`) and reconfigures when it changes,
which fetches that variant's components back.

## 2026-09-25 — Narrow screens get their help reflowed, from the same source

The ES3C28P's screen is 53 columns and Vim's help is written for 78, so on it
nearly every line of help.txt wrapped. A second, hand-written narrow help would
drift from the real one with every edit. So `scripts/helpfmt.py` reflows the one
source at runtime-image build time, for variants that declare a narrow screen
(`VARIANTS` in make-runtime-image.py: es3c28p, 53). It knows the constructs our
help uses:
- rules, and headings and commands with right-aligned tags;
- paragraphs, keeping Vim's two spaces after a sentence;
- two-column tables, kept as columns when the terms are short, with each
  description under its term otherwise;
- examples, re-indented, with trailing comments moved to their own line.

The one part that doesn't reflow sensibly, the navigation block at the top, has
a hand-written narrow alternative in the template (`@WIDE@ … @NARROW@ … @END@`).
Every other variant gets the 78-column help exactly as before. Runtime images are
now per variant: `vimrt-<variant>`, falling back to the chip's for a variant
without one. Upstream help files (netrw, version9, ...) are not reflowed.

## 2026-09-28 — Git is libgit2, not Python on MicroPython (stage 6z)

The plan chose a pure-Python git because no ESP-IDF port of libgit2 existed. The 6z gate
ported libgit2 1.9.7 as Vim was ported: its own CMake component over the extracted
archive, a hand-written `git2_features.h`, a few port shims and five `ESP_PLATFORM`
patches. It passed in the emulator (Phase 8 has the numbers): local init, commit, status
and checkout on FAT; clone, push and fetch over HTTP, git:// and SSH, with the host's
`git fsck --strict` clean after each push; HTTPS with a verified certificate. libgit2 is
392 KB of flash. The Python design would have spent its effort writing the hardest part,
packfile reading with delta resolution, which libgit2 already has along with merge. So git
no longer waits for MicroPython.

What the port had to change, and why:
- **Stack.** libgit2 puts 64 KB I/O buffers on the stack. The first run jumped past the
  whole task stack and both cores faulted at PC 0, with the end-of-stack watchpoint
  never hit, since one frame skipped over it. 4 KB buffers (patch 0002) bring the
  deepest use to 13.7 KB.
- **Heap.** Without mmap, libgit2 reads pack "windows" into malloc'd memory, 32 MB each by
  default on 32-bit, and caches up to 16 MB of delta bases. The device sets 256 KB windows,
  a 1 MB mapped limit and a 1 MB object cache through `git_libgit2_opts`. Patches 0003-0004
  cap two compile-time sizes (the push compress buffer and the pack cache). Push went from
  1.65 MB to 0.64 MB, and a 620 KB pack clone from 5.7 MB to 2.0 MB.
- **FAT.** `rename()` won't replace a file and `link()` copies one, so `p_rename` removes
  the target first (patch 0001), as libgit2 does on Windows. libgit2's own probes set
  `core.filemode` and `core.symlinks` to false.
- **Names.** Vim links its own xdiff and has a `p_write` option. libgit2's copies are
  renamed by macros in a force-included header, rather than by patching either.
- **Certificates.** No CA file exists, so libgit2's mbedTLS stream attaches ESP-IDF's
  bundle (patch 0005), as `esp_http_client` does.

The self-test was its own build variant (`p4git`), so the everyday builds didn't
carry it. Phase 8 replaced both the next day: git is now in every build.

## 2026-09-28 — Git in Vim: libgit2 on the Vim task, memory from PSRAM

**On the Vim task.** The `esp_git_*()` builtins call libgit2 directly, as the SSH
builtins call libssh2. A git task with a queue would have let Vim redraw during a clone,
but everything about a clone is sequential and the user waits for it anyway. Progress on
the command line and CTRL-C (the callbacks return an error; libgit2 unwinds and a clone
deletes what it made) give what a task would have, with one caller for a library built
without threads. libgit2 needs 14 KB of Vim's 64 KB stack.

**Policy in `esp_git`, glue in the Vim component.** Credentials, certificates, memory
limits and gc don't need Vim, so they sit in `components/esp_git`, which could serve a web
page or MicroPython later. The conversion to Vim's types stays with the other builtins.

**Host keys through `esp_ssh`.** libgit2's SSH transport hands its certificate callback
the server's raw key. `esp_ssh_check_hostkey()` checks that key against `known_hosts` by
the same rules as `:EspFiles` and netrw (the same file, the same messages). So the Vim side
asks the same question, and `esp_ssh_trust()` records the answer. The device has one list
of trusted hosts, not two.

**PSRAM first.** ESP-IDF serves every `malloc()` under 16 KB
(`SPIRAM_MALLOC_ALWAYSINTERNAL`) from internal RAM. A clone makes thousands of small
allocations. On the Freenove, with WiFi up, they took internal RAM to 12 bytes, and TLS
then failed with mbedTLS's generic error, since lwIP had no socket buffer left. Lowering
the threshold would move every component's small allocations too, and some drivers need
internal RAM. So only libgit2 and its bundled zlib are redirected: `malloc`, `calloc` and
`realloc` map to `heap_caps_*_prefer(PSRAM, then default)` in the header force-included into
their sources. The same clone then kept internal RAM above 3.6 KB.

**No global identity by default.** A commit needs `user.name` and `user.email`. When they
aren't set, the device doesn't invent them; the command says how to set them device-wide
(`/fat/.gitconfig`, found because HOME is `/fat`). An unset clock gets a warning instead
of a refusal: a board with no clock chip and no network should still be able to commit.

## 2026-09-29 — MicroPython: a trimmed archive, its own CMake, and Vim behind a table

**A trimmed archive.** MicroPython's 1.29.0 release is 160 MB (1.5 GB unpacked), almost
all of it the submodules of other ports: CMSIS, the Pico SDK, lwIP, mbedTLS, TinyUSB. The
vendored archive keeps what this build reads: `py/`, `extmod/`, `shared/`, `tools/` and
`lib/{uzlib,re1.5,crypto-algorithms,oofatfs}`, about 1 MB. It is repacked
reproducibly (sorted, fixed dates, `gzip -n`), and the manifest records the release
asset's sha256 as well as ours, as it does for Vim's `git archive`.

**MicroPython's own CMake, not the embed port.** `ports/embed` exists for exactly this
case, but its generator copies `py/` alone: no `json`, `re`, `os`, `time`, `hashlib`. The
ESP32 port's approach works inside an ESP-IDF component instead: `py/py.cmake` lists the
core, and `py/mkrules.cmake` generates the qstr tables at build time. The build
preprocesses the sources with ESP-IDF's compiler and include paths, then runs
MicroPython's Python scripts on the output. Our part is `mpconfigport.h`, a HAL, and the
bridge.

**Vim behind a table.** Vim's and MicroPython's headers are never included together. The
interpreter component sees a Vim value as an opaque pointer, and reads, builds, runs
commands and evaluates through an `esp_py_host_t` of function pointers that
`esp_api_py.c` (a Vim file) fills in. That keeps the qstr pass away from Vim's headers,
avoids a dependency cycle between the two components, and puts all the Vim-internal code
in one file. It is the same shape as `if_py_both.h`, down to how a Vim error becomes a
Python exception (`++trylevel`, then read `msg_list` or `current_exception`).

**The Python-side modules are Python, frozen into the firmware.** `vim.py` and `esp.py`
sit on top of a four-function C module (`_vim`). Buffers, windows, `vars` and `options` are
a few lines each over Vim's own `getbufline()`, `setbufline()`, `win_execute()` and `:let`,
so they get Vim's undo, redraw and checks for free. `esp.<name>()` is a module
`__getattr__` that calls `esp_<name>()`: every device function, present and future, with
one implementation.

They first lived in `/vimrt/python`, to be changed without a reflash, at the cost of a
parse and compile each time Python started. Since 2026-09-30 they are frozen: compiled to
bytecode at build time, run from flash. A Python restart went from 25 ms to 1 ms in the
emulated P4, and 8 KB of bytecode left the Python heap, for 4.4 KB of app. The flexibility
is kept: `/fat/python` comes before the frozen modules in `sys.path`, so a changed
`vim.py` there is used instead. The compiler, `mpy-cross`, is built for the host from the
vendored MicroPython archive, and `tools/makemanifest.py` is called from our CMake:
`mkrules.cmake`'s own frozen step requires micropython-lib, which this build doesn't use.

**Double floats.** Vim's Float is a double, and a value passed through Python should keep
its digits. MicroPython's ESP32 port uses single precision; the cost here is speed in
float-heavy scripts, which an editor's scripts rarely are.

**One patch.** With `VfsPosix` on, `extmod/vfs_posix_file.c` defines `sys.stdout` as a
file on fd 1: the unix port's choice. Here, fd 1 is the UART under Vim's screen, so
`print()` scribbled on the display. Patch 0001 lets a port leave those definitions out
(`MICROPY_VFS_POSIX_NO_SYS_STDFILES`); `shared/runtime/sys_stdio_mphal.c` then routes
`sys.stdout` through `mp_hal_stdout_tx_strn()`, which is ours. The known `__assert_func`
clash was in the embed port's `embed_util.c`, which this build does not use.

**Files are the ordinary POSIX calls.** A `VfsPosix` mounted at `/` makes `open()`,
`os.listdir()` and `import` go through `open()`, `stat()` and `opendir()`. The firmware
interposes those at link time (`esp_shims.c`), so relative paths follow Vim's
working directory with no extra code, and `os.chdir()` is `:cd`. Files that Python leaves
open belong to the session's task, and `esp_vim_session_begin()` closes them with Vim's.
That is why a new session starts a new interpreter without running the old one's
finalisers: it doesn't call `mp_deinit()`, which would close those descriptors a second
time.

## 2026-09-29 — Keyboards: reports from the report map, one connection, bonds remembered

**The report map, not the boot layout.** The boot layout (a modifier byte and six key
codes) is what a keyboard must offer a BIOS, not what it sends a host in report mode.
Many send an N-key rollover bitmap as well, one bit per key: the test keyboard's report
2, 20 bytes. Hard-coding that one layout would fix one keyboard. Every keyboard
describes its reports in its HID report map, which ESP-IDF's HID host already fetches,
so `esp_kbd_hid.c` parses it. It keeps only page-7 (Keyboard/Keypad) input fields, and
their bit positions per report ID. The decoder, and the mapping from keys to terminal
bytes, are pure C, built and tested on the host (`pixi run kbd-test`). The emulator can't
be a keyboard, and the real one isn't always at hand.

**Each report's keys kept apart (amended the same day, on the board).** The first
design took each report as the keyboard's whole state. The Air75 BT5.0 splits its keys:
the boot-style report has the modifiers and the first five, and the bitmap report only
the keys beyond them. So the keyboard's state is the union of each report's latest
keys, with the modifiers from the reports that carry them. That also covers the other
style, where the bitmap holds every key and the boot report says "too many": a rollover
report leaves its own keys as they were. A history of the last 16 raw reports
(`esp_bt_keyboard().reports`) showed this at once, and it stays as a diagnostic for the
next keyboard.

**xterm's modifier forms.** With a modifier held, the special keys send what xterm
sends: `CSI 1;{m}X` and `CSI {n};{m}~`, where m = 1 + Shift + 2·Alt + 4·Ctrl. Vim's
builtin xterm termcap already decodes these, so `<C-Right>` and `<S-F5>` mappings work
with no termcap change. Alt with a character stays Escape-then-character, as in a
terminal.

**One keyboard connected, several bonded.** Each BLE connection costs the S3 internal
RAM, and with the display and WiFi that RAM is short (31 KB free with one keyboard). So up
to four keyboards are bonded, but one is connected at a time. The HID host waits up to
30 s for each connection attempt, and it has no way to wait for several at once, so the
order of attempts matters: the last keyboard used is tried first. Names are kept in NVS
beside the bonds, because the list is only useful with names, and a keyboard's name is
only known while it is connected.

**The "Paired" notice waits for Vim.** A pairing on the touch screen happens on the
overlay's task, and nothing may call into Vim from there. The Bluetooth side leaves one
message, and Vim takes it at `SafeState`: the first moment it is idle, typically the
first key typed on the new keyboard. That needs no timer, and so no periodic wake-ups.
It costs one C call that returns at once when nothing is waiting.

## 2026-09-29 — SD cards: FAT and exFAT, never formatted

**exFAT through a patch.** Cards over 32 GB come formatted exFAT. Reformatting one to FAT32
would be a chore for the user, and would make it unusual on every other device. FatFs has
exFAT; ESP-IDF compiles it out and offers no option. So `patches/esp-idf/0002` turns it on,
in the patched-copy mechanism already used for `esp_hid`, now generalised to a list. The
FAT volumes on flash are unaffected.

**Not LittleFS on the card.** LittleFS survives power loss and is case-sensitive, both
better for a device's own storage. But a card is for moving files to and from a computer,
which can't read LittleFS without special tools. Moving `/fat` to LittleFS would be a
separate decision, since it means reformatting internal storage.

**Never format.** ESP-IDF's mount can format a card that won't mount. That is right for
internal flash, and wrong for a card that may be a camera's or a phone's, one that just
needs a different filesystem, or one that is failing. A card that won't mount is
reported, with the reason, and left as it is.

**The SPI interrupt path out of IRAM, per board.** ESP-IDF keeps the SPI master driver's
interrupt path in IRAM, so that transfers carry on during flash writes. On the Freenove
that is 9 KB of internal RAM, which Bluetooth and WiFi need more. There the SD card is the
only SPI device, and waiting out a flash write is harmless. Boards whose display is on SPI
(the ES3C28P, the RLCD-4.2) keep the default.


## 2026-09-30 — The S3 emulator's heap corruption is esp-emu's; its gate stays informational

The S3 emulator's intermittent heap corruption (PHASE5.md, known issue) is an esp-emu
defect: when an interrupt is taken at a branch to a zero-overhead loop's end address, the
loop goes round again. The ROM's `strcpy()` leaves its loops exactly that way, so about one
copy in 28,000 runs on past the NUL. A 30-line app shows it with no Vim code
(`pixi run emu-s3-loop`), on 0.43.0 and 0.44.0, and never with interrupts masked.

**Not worked around in the firmware.** Replacing `strcpy()` would hide the case we found
and leave every other loop GCC compiles the same way; and a firmware changed for its test
bench would be tested as something other than what ships. So the S3 gate stays
informational, the P4 gate is the one that must pass, and S3 behaviour is checked on
silicon (the Freenove and the ES3C28P), as it has been. `emu-s3-loop` turning PASS on a new
esp-emu is the signal to make the S3 gate count again.

**Not moved to esp-emu 0.44.0.** It has the same defect (and one more flaky file-manager
check); 0.43.0 stays vendored.

**The heap guard stays**, as a debug option (`CONFIG_ESP_VIM_HEAP_GUARD`), off in every
build: it is what caught this, and the next heap bug will want it too.


## 2026-09-30 — The Tab5: Espressif's board support package underneath, written before the board

The Tab5 support was written before the board arrived (PHASE9.md), so its choices lean
towards code that others have run on the hardware.

**The panel through the BSP, not panel code of our own.** Three revisions of the Tab5
are sold: ILI9881C with GT911 touch, then ST7123, then ST7121. Each needs its own
power-up sequence and DSI timing. Espressif's `m5stack_tab5_noglib` (1.3.1) has all
three, picks between them the way M5Stack's own library does (the touch controller's
address and firmware version), and brings up the I2C bus and the IO expanders the panel
depends on. It is a managed component, fetched for the `tab5` build only, like the
panel drivers of the other boards. `components/esp_board` wraps it, so nothing else
includes it. What we still do ourselves: the drawing, straight into its frame buffer
turned to landscape; the expander pins the BSP leaves alone (antenna, charging); and
the ST7121's lane rate, which the BSP picks only in its LVGL path.

**One rotation function for display and touch** (`esp_board_rotate.h`, host-tested).
The picture is turned in software because DSI panels can't turn it. A second copy of
the mapping in the touch driver could drift from the first; so could the swap and
mirror settings the other boards use.

**The keyboard accessory in normal mode, turned into HID reports.** The keyboard's
firmware can also send characters or HID reports itself. Its character mode would take
Sym and Aa for itself, and leave no way to make F-keys (the plan's reason). Row and
column events are turned into the same reports a USB keyboard sends, so every keyboard
shares one path to Vim's bytes. The layout comes from M5Stack's demo; F-keys,
PgUp/Home/PgDn/End and Insert are ours, on Sym.

**Assumptions to check on the first flash**: that USB-C is the P4's USB Serial/JTAG
port (the console is put there), which way up the picture is (`:EspFlip`, or
`ESP_VIM_DISP_ROTATION`), and which esp-hosted version the C6 runs.

## 2026-09-30 — The Tab5 build is for P4 revisions before v3

The first Tab5 is an ESP32-P4 revision v1.3. ESP-IDF 5.5 supports P4s before v3.0 and
from v3.0 on only in separate builds (`ESP32P4_SELECTS_REV_LESS_V3`: the silicon
changed too much), and its default needs v3.01, so the bootloader would refuse this
board. `sdkconfig.defaults.tab5` therefore selects revisions before v3, minimum v1.0,
the board in hand. Keeping v3 as the default with a `tab5v1` variant was the
alternative, but the only Tab5 we have would then need the variant. esp-emu's P4 is
v3.1, so `tab5uart`, the emulator build, sets v3 back and doesn't boot on this
board. A v3 Tab5 gets a variant of its own when one turns up.

## 2026-09-30 — esp-hosted 1.4.0 on the Tab5, to match the C6 it ships with

On the board, esp-hosted 2.12.13 never got an answer from the C6: every SDIO
CMD5 timed out after the reset. M5Stack's factory firmware (M5Tab5-UserDemo,
esp-hosted 1.4.0 on ESP-IDF 5.4) reaches it on the same pins, slot, clock and
reset. Ruled out on our side, one by one: the SD card's slot 0 on the same
controller (no mount at boot changed nothing), the pads' pull-ups, the reset
sequence, the C6's power switch. Built against 1.4.0 with esp_wifi_remote 0.8.5,
the pair M5Stack ships, the link comes up at once: the C6 runs 1.4.1, and WiFi
scans and Bluetooth LE scans work.

Why 2.x failed, found afterwards: the C6's reset line. On the Tab5, GPIO15 low
holds the C6 in reset and high lets it run. M5Stack's config says "active low",
but 1.4 ignores that: its `#ifdef H_RESET_ACTIVE_HIGH` is always true, since the
macro is defined as 0 or 1. So 1.4 always ends its reset pulse high. 2.x honours
the setting, ends it low, and leaves the C6 held in reset, so nothing answers
CMD5. A 2.x build for the Tab5 would want `ESP_HOSTED_SDIO_RESET_ACTIVE_HIGH`.

Two of 1.4's defaults are wrong for the Tab5, and both crash it, so
`sdkconfig.defaults.tab5` sets them:
- D1 in 4-bit mode (`ESP_HOSTED_SDIO_PRIV_PIN_D1_4BIT_BUS`) defaults to 15 on a
  P4, the C6's reset line here. SDIO traffic then reset the C6 under the link,
  and the read task retried without end, starving everything else.
- The slave target from esp_wifi_remote 0.8 defaults to an ESP32. esp-hosted
  asserts when the chip it finds isn't the configured one, so every connection
  attempt restarted the Tab5.

The board layer also holds the C6 in reset from its power-on until esp-hosted
connects. Otherwise a C6 still linked from before a restart of ours answers
with stale state.

What 1.x costs:
- `:EspC6 update {file}`: 1.x updates the co-processor only from a URL. The
  command says so; serving the file from the Tab5's own web server is a way
  back to it.
- 1.x aborts (`ESP_ERROR_CHECK`) when the C6 never answers. Nothing asks it
  unless the board switched the C6's power on (`esp_board_coprocessor_powered()`),
  which also keeps the emulator, with no expanders, away from it. (Patched
  since, so a silent C6 fails the link instead: see 2026-10-01.)
- `scripts/c6-build.sh` now builds the 1.4.0 slave, to match.

2.x stays possible: `esp_ble` and `esp_net_cp.c` keep their 2.x paths, chosen
by which headers esp-hosted provides. Moving to it means updating the C6 too,
over a link that works first.

## 2026-10-01 — esp-hosted vendored and patched: a silent C6 fails the link, not the board

esp-hosted 1.4.0 has no way to give up on a co-processor. When the C6 doesn't
answer, it either restarts the host or aborts:
- its SDIO card set-up wraps every register access in `ESP_ERROR_CHECK`;
- the read task, if the card set-up fails, returns from its task function,
  which FreeRTOS on ESP-IDF treats as fatal;
- `esp_hosted_reconfigure()` and `esp_wifi_remote_init()` abort on a failed
  reconfigure, and Bluetooth's `ble_transport_ll_init()` the same;
- three SDIO paths call `esp_restart()` deliberately: the C6's buffer count
  unreadable, a write that fails twice, and the interrupt register unreadable
  ("Host is reseting itself", logged at INFO level, below our WARN filter).

The last of those is the likeliest cause of the Tab5 restarting once, just after
a download, with nothing on the console: one failed SDIO read, while the SD card
shares the controller, is enough. That's a guess. It fits everything seen, and
nothing else found does.

A restart loses whatever Vim had unsaved, and a C6 that never answers made the
Tab5 restart without end. So esp-hosted is vendored like Vim and MicroPython
(third_party/esp_hosted-1.4.0.tar.gz, the registry's archive repacked) with one
patch, `patches/esp_hosted/0001-link-failure-instead-of-restart.patch`:
- every one of those paths calls `hosted_link_failed(why)` instead: it logs once
  ("co-processor not responding (...): link down until restart"), marks the
  transport down, so sends fail at once rather than queue, and parks the SDIO
  task that found it. Only esp-hosted's own tasks reach these paths;
- the card set-up returns its errors (`SDIO_TRY`);
- reconfigure returns ESP_FAIL as soon as the link has failed, instead of
  waiting out its 1000 one-second retries (17 minutes, on Vim's task for
  `:EspC6`);
- `esp_hosted_link_failed()`, with `ESP_HOSTED_LINK_FAILED_API` defined, says
  so. `esp_net_cp_failed()` wraps it for esp_net and esp_ble: WiFi, `:EspC6` and
  Bluetooth then say "the co-processor stopped answering (restart to try
  again)" without asking it.

No recovery without a restart: the parked task stays parked. Saving the
editor's work and restarting by hand is the trade.

esp_net and esp_ble take it with `override_path` to build-deps/esp_hosted, the
version pinned at 1.4.0 all the same. The C6's own firmware (`c6-build`) still
comes from the registry: the patch is all on the host side.
