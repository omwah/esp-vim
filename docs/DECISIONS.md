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

The directory is named `vim-tiny-p4`, but `+small`/`+big` no longer exist upstream
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
