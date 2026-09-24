# vim-tiny-p4

Porting [Vim](https://github.com/vim/vim) to the ESP32-P4, targeting the
[M5Stack Tab5](https://docs.m5stack.com/en/core/Tab5).

- **[docs/PLAN.md](docs/PLAN.md)** — the living plan, phase by phase, with status
- **[docs/PHASE1.md](docs/PHASE1.md)**: capability spike results (verdict: **GO**)
- **[docs/PHASE2.md](docs/PHASE2.md)**: Vim compiles for the P4 (1.82 MB text at `-Os`)
- **[docs/PHASE3.md](docs/PHASE3.md)**: Vim runs, edits and saves in the emulator
- **[docs/PHASE4.md](docs/PHASE4.md)**: curated runtime, generated filetype detection, memory
- **[docs/PHASE5.md](docs/PHASE5.md)**: interactive over UART; ESP32-P4 and ESP32-S3
- **[docs/DECISIONS.md](docs/DECISIONS.md)** — why things are the way they are

## Getting set up

```sh
git clone <this repo> && cd vim-tiny-p4
git lfs pull              # fetch the vendored upstream archives
pixi install              # host toolchain (ncurses, socat, openssh, python)
pixi run deps             # verify, extract and patch upstream into build-deps/
```

Then install ESP-IDF **v5.5.5** out-of-tree, from inside the pixi environment so
IDF builds its venv against pixi's Python:

```sh
git clone -b v5.5.5 --depth 1 --recurse-submodules --shallow-submodules \
    https://github.com/espressif/esp-idf.git ~/esp/esp-idf
pixi run -- bash -c 'cd ~/esp/esp-idf && ./install.sh esp32p4'
pixi run -- bash -c 'cd ~/esp/esp-idf && \
    python tools/idf_tools.py install cmake ninja'   # not installed by default on Linux
```

Check it took:

```sh
pixi run env-check
#   env: python 3.13.15
#        idf     ESP-IDF v5.5.5
#        target  14.2.0 (riscv32-esp-elf)
```

## Tasks

Everything runs through pixi, so the incantations live in the repo rather than in
someone's shell history. `pixi task list` shows them all.

| Task | What |
|---|---|
| `pixi run deps` | Verify, extract and patch vendored upstream into `build-deps/` |
| `pixi run add-dep` | Onboard a new third-party archive into LFS — the only networked script |
| `pixi run env-check` | Print the active python / ESP-IDF / cross-compiler versions |
| `pixi run spike` | Build and run the Phase 1 capability spike under the emulator |
| `pixi run spike-build` | Build the spike only |
| `pixi run spike-report` | Re-record spike output into `docs/phase1-spike-results.txt` |
| `pixi run configure-vim` | Run Vim's `configure` on the host to emit a baseline `auto/config.h` |
| `pixi run runtime` | Curate `$VIMRUNTIME` into `build-deps/vimrt/` and generate `filetype.vim` (checks it fits the partition) |
| `pixi run vim-build` | Build the Vim firmware for ESP32-P4 (`esp-vim/build-esp32p4/`); runs `runtime` first |
| `pixi run vim-build-s3` | Build the ESP32-S3 variant (`esp-vim/build-esp32s3/`) |
| `pixi run vim-test` | **The regression gate** on ESP32-P4: builds, then round trips across reboots plus interactive console checks |
| `pixi run vim-test-s3` | The same gate on the ESP32-S3 variant |
| `pixi run vim-size` | Report the Vim component's text/data/bss |
| `pixi run mkpatch <dep> <name>` | Turn edits in `build-deps/<dep>` into a new patch (see *Patch authoring*) |
| `pixi run emu <dir>` | Run any built project under `esp-emu` (merges the flash image first) |
| `pixi run emu-tty` | Attach a raw terminal to an emulator started with `--uart-tcp 127.0.0.1:5555` |

Extra arguments pass through after `--`:

```sh
pixi run emu esp-vim/test/spike -- --gdb 1234 --gdb-halt
```

For an interactive session, start the emulator with a TCP UART in one terminal and
attach from another:

```sh
pixi run vim-build
pixi run emu esp-vim -- --uart-tcp 127.0.0.1:5555   # terminal 1
pixi run emu-tty                                    # terminal 2
```

`emu-tty` uses `socat`, not `nc` — `nc` cannot put the terminal in raw mode, so arrow
keys arrive as literal escape sequences and echo doubles. Detach with `Ctrl-]`.

## Help on the device

`:help` on the device opens a device-specific guide: an amended version of Vim's
`help.txt` covering storage, filetypes, keys, settings and what isn't available. Edit it
in `esp-vim/runtime-image/doc/help.txt.in`. The build generates the tags and rejects
broken links.

## Choosing which filetypes the device supports

Edit **`esp-vim/filetypes.conf`**: one line per filetype, followed by its file patterns.
It generates the device's `filetype.vim` and decides which syntax, ftplugin and indent
files are shipped. Then run `pixi run vim-build`. Types not listed there aren't detected
at all. To add one on a running device without reflashing, use a standard Vim
`ftdetect/` script in `/fat/.vim/ftdetect/`.

## Why the host tools are pinned

`pixi.toml` pins four host tools, each for a specific reason:

- **ncurses** — Vim's `./configure` refuses to finish without a terminal library, even
  though the ESP build deliberately runs with `HAVE_TGETENT` undefined and uses Vim's
  builtin termcaps. We only need configure to *complete* so it emits a `config.h` to
  curate.
- **socat** — raw-mode terminal for interactive sessions over the emulated UART.
- **openssh** — a local `sshd` and bare repo to test SCP/SFTP and `git push` (Phase 8).
- **python 3.13** — ESP-IDF builds its own venv with `python -m venv`, which needs
  `ensurepip`. The system Python has none, so IDF's bootstrap fails against it.

Pinning them here keeps a fresh clone reproducible and needs no root, which `apt` would.

## How third-party source is handled

Upstream source is never committed as files. Each dependency is a **Git LFS archive** in
`third_party/` plus a **patch series** in `patches/<dep>/`, extracted and applied into the
git-ignored `build-deps/` at build time. This keeps the repository reviewable, makes our
changes to upstream explicit, and makes re-syncing to a newer Vim a bounded job.

`third_party/manifest.txt` records each archive's provenance and sha256. Note the sha256
attests the blob *we vendored*, not an upstream-published digest — see the manifest header
for why that distinction is necessary.

To change upstream source: see *Patch authoring* in [docs/DECISIONS.md](docs/DECISIONS.md).

## Layout

| Path | What |
|---|---|
| `docs/` | plan, phase results, decisions |
| `third_party/` | LFS archives + manifest. No source files. |
| `patches/<dep>/` | our changes to upstream, applied in lexical order |
| `scripts/` | dependency prep, environment, emulator harness |
| `esp-vim/` | the ESP-IDF project (`test/spike/` is Phase 1) |
| `build-deps/` | extracted + patched upstream (git-ignored) |
