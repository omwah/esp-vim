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
scripts/prepare-deps.sh vim            # clean, unpatched (or patched) tree
cd build-deps/vim
git init -q && git add -A && git commit -qm base   # throwaway repo, inside the tree
#   ... make the edit ...
git diff > ../../patches/vim/0005-short-description.patch
cd ../.. && scripts/prepare-deps.sh vim            # confirm it applies from scratch
```

Rules that matter:

- **Generate patches against the extracted archive, never against a git clone of
  upstream.** Context lines can differ, and a patch that applies to the clone may fail
  against the archive.
- Numbered prefixes set apply order (`0001-`, `0002-`, …), applied lexically.
- `prepare-deps.sh` tries `git apply --whitespace=nowarn` first and falls back to
  `patch -p1`. It **stops at the first failure** rather than continuing with a
  half-patched tree; re-running resets the tree from the archive.
- Keep each patch to one concern with a descriptive name. These are re-applied against
  every future Vim, so a reviewer must be able to tell what each one is for without
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
