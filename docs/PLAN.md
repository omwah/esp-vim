# Porting Vim to the ESP32-P4 (M5Stack Tab5)

> **Living document.** This is the canonical plan; it is kept current as work lands.
> Phases are ticked off below, provisional numbers are replaced with measured ones, and
> decisions are amended in place when reality disagrees — with the reasoning recorded in
> [DECISIONS.md](DECISIONS.md).

## Status

| Phase | State |
|---|---|
| 0 — Repository bootstrap | **done** (2026-09-23) |
| 1 — Capability spike (GO/NO-GO) | **done — GO** (2026-09-23), see [PHASE1.md](PHASE1.md) |
| 2 — Build system + generated files | **done** (2026-09-23), see [PHASE2.md](PHASE2.md) — Vim compiles, 1.82 MB text at `-Os` |
| 3 — OS shim layer | **done** (2026-09-23), see [PHASE3.md](PHASE3.md) — Vim runs, edits, saves; `pixi run vim-test` |
| 4 — Storage + curated runtime | **done** (2026-09-24), see [PHASE4.md](PHASE4.md) — runtime 74% of `vimrt`, 30 filetypes, 0.47 MB PSRAM to open a file |
| 5 — Emulator bring-up over UART | **done** (2026-09-24), see [PHASE5.md](PHASE5.md) — interactive over UART; `:q` restarts in place; chip-named splash, device help, busy indicator. P4 gate green. **S3: open intermittent heap corruption under the emulator**, gate informational until tested on silicon |
| 6 — `:Esp*` commands, file manager, transports, web | **in progress**, see [PHASE6.md](PHASE6.md). 6a (builtins + device commands) and 6b (`esp_fs` core + `:EspFiles`) done 2026-09-24 |
| 7 — MicroPython | not started |
| 8 — Git | not started. **Being reconsidered:** libgit2 instead of pure Python; test build scheduled after Phase 6 (see Phase 8) |
| 9 — Tab5 hardware over UART | not started |
| 10 — Tab5 display console | not started |
| 10b — ESP32-S3 CYD display console | not started; see Phase 10b |
| 11 — Bluetooth keyboards (late-stage goal) | not started; BLE (HOGP) only, see Phase 11 |

Toolchain: ESP-IDF **v5.5.5**, riscv32-esp-elf 14.2.0, emulator esp-emu 0.43.0.

Measured: Vim component **1.82 MB `.text`** at `-Os` (Phase 2); firmware 2.18 MB of a
7 MB app partition; curated runtime **~2.2 MB of the 3 MB `vimrt` partition (74%)**;
Vim uses **0.35 MB** of PSRAM idle and **0.47 MB** after opening a file (Phase 4). The
partition table stays provisional until MicroPython and git are in (Phases 7–8).

**Phase 1 changed the plan** — see [PHASE1.md](PHASE1.md). In short: the `isatty(0)` gate
passed, but ESP-IDF has **no working directory** (`chdir` is `ENOSYS`, `getcwd` always
`/`), so the port must supply a userspace CWD; and `getuid`, `getpwuid`, `signal`,
`lstat`, `readlink`, `symlink`, `link` and `umask` are all absent, with `lstat` not even
declared. `SPECIAL_WILDCHAR` can be undefined to make stubbing `mch_expand_wildcards()`
safe.

## Context

We want a real, self-hosted Vim running on an ESP32-P4 — eventually on the M5Stack Tab5
(ESP32-P4NRW32, dual RISC-V @400 MHz, 16 MB flash, 32 MB PSRAM, 1280×720 MIPI-DSI touch
panel, USB-A host, microSD, ESP32-C6-MINI-1U radio co-processor). The first milestone is
Vim running over UART inside an emulator, with no hardware in the loop, so the port can be
developed and regression-tested on the desktop.

**Hardware targets** (added 2026-09-24): the M5Stack Tab5 (primary); **ESP32-S3
"Cheap Yellow Display" (CYD) boards**, but only the variants with 16 MB flash and 8 MB
PSRAM (N16R8 module), since the original CYD (ESP32-2432S028) is a classic ESP32 with 4 MB
flash and no PSRAM; and bare ESP32-S3 dev boards over serial. The README's Hardware
section is the user-facing list. Board-specific details belong in a board layer, never
in the shared code.

Beyond the editor itself, the device should be usable as a hands-on board tool: an `:Esp*`
command family exposing WiFi, GPIO, sensors, BLE, NVS and serial; a two-pane
Midnight-Commander-style file manager; real file transfer in and out of the device (SCP,
SFTP, USB mass storage, and a password-protected web interface that also carries settings
and a live view of what is being edited); on-demand spell-file download; embedded
**MicroPython** for scripting both the board and the editor; **git** for version control;
and support for the Tab5's optional clip-on keyboard alongside USB keyboards.

Vim has never been ported to ESP-IDF as far as any search shows, so every OS-facing
assumption has to be established rather than inherited. The strategy is therefore: get the
smallest possible thing rendering over UART in the emulator, prove each OS dependency
there, then add the command/plugin layer, then hardware, then display.

**Phase ordering note.** Phase 0 (repository bootstrap) comes first; Phases 1–6 are then a
dependency chain. After that, Phase 7
(MicroPython) → Phase 8 (git) is a hard dependency — git is written in Python — but that
pair is independent of Phase 9 (hardware) and Phase 10 (display/keyboard), which are their
own chain. Either branch can be pulled forward; the device is a usable editor after Phase 6
and a usable *tablet* after Phase 10 regardless of where git sits.

**Decisions taken:**

| Decision | Choice |
|---|---|
| Feature level | `FEAT_NORMAL` (+eval, +syntax, +quickfix, +persistent_undo, **+spell**) |
| Storage | Wear-levelled FATFS on internal flash; Tab5 microSD added in the hardware phase |
| Command prefix | `Esp` — `:EspWifiScan`, `:EspGpio 12 on`, `:EspFiles`, … |
| File manager | Purpose-built two-pane plugin **plus** netrw |
| File transfer | Core: local/SD copy, **SCP + SFTP**, **USB-C mass storage**, **password-protected web file manager**. Stretch: HTTP/HTTPS download |
| Spell files | None shipped; downloaded on demand to `/fat/spell` |
| Keyboard | Tab5 I2C keyboard accessory **and** USB-HID keyboards |
| Scripting | **MicroPython** embedded — run scripts *and* script the editor |
| Version control | **Full git, including push** |
| Web server | File management + settings + live edit status |
| Scope | Fully detailed through the Tab5 DSI display console |

## Vendored dependencies

Phase 0 converted both upstream trees to LFS archives; neither is a checked-in source tree.

| Archive | What | sha256 (vendored blob) |
|---|---|---|
| `third_party/vim-9.2.1125.tar.gz` | Vim upstream, `git archive` of `12c69acc` (2026-09-21) | `1c243758…cbb2d5` |
| `third_party/esp-emu-0.43.0-x86_64-unknown-linux-gnu.tar.gz` | Espressif emulator, upstream release asset | `ec875a31…200243` |

`scripts/prepare-deps.sh` verifies and extracts these into `build-deps/` and applies
`patches/`. It uses no network. Full records in `third_party/manifest.txt`.

---

## Key findings that shape the plan

**The emulator is `esp-emu`, not QEMU.** Upstream/Espressif QEMU has no ESP32-P4 machine —
the IDF QEMU guide 404s for the `esp32p4` target. Espressif's newer `esp-emulator`
(v0.43.0) does list `esp32p4` and models the P4's dual HP cores, CLIC, UART, SPI flash,
GDMA, I2C, RMT and PSRAM. Relevant flags: `--chip esp32p4`, `--psram-size 32M` (the
firmware cannot choose — the emulated die's mode registers decide), `--uart-tcp`,
`--uart1-tcp`, `--inject`/`--inject-on`, `--exit-on`, `--save-state`, `--net user`,
`--ble-hci`, `--hosted`, `--gdb`, `--trace`. **No MIPI-DSI model** — the display phase is
hardware-only.

**The ESP32-P4 has no radio.** This is the single most consequential finding for the new
command set. There is no WiFi and no Bluetooth on the P4 die. On the Tab5, both come from
the **ESP32-C6-MINI-1U** over SDIO, via `esp-hosted` + `esp_wifi_remote`, which re-exports
the normal `esp_wifi_*` API across the link. So `:EspWifiScan` and `:EspBluetooth` are
**remote calls to a second chip**, with the attendant latency and a failure mode ("radio
co-processor not responding") that has no equivalent on a single-chip ESP32. Fortunately
`esp-emu` models exactly this: `--hosted bridge:slave:/sock` for a C6 instance and
`--hosted bridge:host:/sock` for the P4, i.e. **two emulator processes** linked by a Unix
socket. Networking to the host is available via `--net user` (slirp), so SCP and HTTPS
downloads are testable in emulation.

**Vim can build with no terminal library at all.** `term.c` is fully functional without
`HAVE_TGETENT`, falling back to compiled-in termcaps (`builtin_xterm`, `builtin_ansi`,
`builtin_dumb`). No ncurses/terminfo port needed — a large win. `TERM=xterm` resolves
entirely in-binary, and the F1–F12 sequences the file manager needs come from there.

**`+small` and `+big` no longer exist** — `feature.h:44-59` aliases them to `TINY`/`NORMAL`.
A set of formerly-optional features are now *unconditionally* on: `+windows +autocmd
+multi_byte +cmdline_compl +textobjects +builtin_terms +float +user_commands +path_extra`.
Notably **`mbyte.c` cannot be compiled out**, so UTF-8 is a given.

**`BASIC_SRC` (`src/Makefile:1503`) is the whole file list** — ~95 `.c` files, all compiled
at every feature level, self-disabling internally via `#ifdef`. The CMake component mirrors
that one list rather than pruning it.

**Vim bundles `src/libvterm/` (556 KB)** — a complete VT100/xterm state machine and screen
model. The Tab5 display console reuses it instead of writing a terminal emulator.

**`autoload/spellfile.vim` is only 203 lines and routes every download through netrw's
`:Nread`.** Combined with the SCP requirement, this converges on one design: replace
netrw's *transport layer* — which normally shells out to `curl`/`wget`/`scp` — with native
functions. One shim then serves both SCP transfer and the spell downloader, unmodified.

**`libssh2` has a maintained ESP-IDF port.** `skuodi/libssh2_esp` is on the ESP Component
Registry (v1.1.0, libssh2 1.11.1, BSD-3-Clause), wrapping unmodified upstream libssh2 with
ESP-IDF's build system and lwIP, using the **mbedTLS already bundled in IDF** as its crypto
engine. `libssh2_scp_recv2`/`libssh2_scp_send64` give us SCP directly. (Note one report of
a mbedTLS API break on IDF ≥5.3 in a *different* fork — validate the chosen port against
the pinned IDF early.)

**The vim runtime tree is 51 MB** (`syntax/` 21 MB, `doc/` 12 MB, `spell/` 5 MB,
`ftplugin/` 2 MB, `autoload/` 1.9 MB, `indent/` 1.6 MB, `pack/` 1.3 MB). It cannot ship
whole in 16 MB of flash — a curated subset on a read-only partition is a required
deliverable. Measured pieces we care about: `pack/dist/opt/netrw` **628 KB**, English spell
`en.utf-8.spl` 608 KB + `.sug` 584 KB (**not shipped** — downloaded on demand).

**The Tab5 keyboard accessory has no function-key row, and that changes the file manager.**
The optional [Keyboard for Tab5](https://shop.m5stack.com/products/keyboard-for-tab5) is a
70-key 14×5 matrix driven by an **STM32F030C8T6**, attached to **Ext.Port1** over **I2C at
address 0x6D** (SDA `G0`, SCL `G1`, plus a dedicated **interrupt line on `G50`** — so key
events are interrupt-driven, not polled, which matters for editor latency). Modifiers are
`Aa`, `Ctrl`, `Sym`, `Alt`, `Tab`, `Esc` and arrows. Its firmware offers three modes:

- **Normal** — returns key state plus **row/column coordinates**.
- **HID** — returns HID reports, for forwarding as a USB/BLE HID device.
- **Character** — returns the key's name string plus Ctrl/Alt state, but **`Sym`/`Aa` are
  consumed by the keyboard itself**.

**Use Normal mode.** Raw row/column means we own the entire keymap in firmware, which is
the only way to guarantee the three things Vim cannot function without: `Esc` is never
swallowed, `Ctrl-` combinations (`Ctrl-W`, `Ctrl-R`, `Ctrl-V`, `Ctrl-[`) arrive intact, and
— since **there are no F1–F12 keys on this device** — we can synthesise them on the `Sym`
layer. Character mode would surrender the `Sym` layer to the keyboard and make that
impossible. Consequence for Phase 6: the two-pane file manager **cannot depend on F3–F8
alone** and needs letter-key aliases as equal citizens.

**MicroPython has a real ESP32-P4 target, and an embed port.** MicroPython **v1.29.0**
(2026-08-24) ships an official `ESP32_GENERIC_P4` board, which requires ≥16 MB flash — the
Tab5 has exactly that. For our purposes the relevant piece is not that port but
**`ports/embed`**, designed to link MicroPython into an existing C application via
`micropython_embed.h`; prior art exists for wrapping it as an ESP-IDF component
(`agatti/micropython-idf-component`, `robdobsn/MicroPythonESP32Embedding`). Two things to
plan around: a known **`__assert_func` redefinition clash with IDF's newlib**, and the fact
that P4 silicon changed at **chip revision 3.0**, so pre-rev3 parts need a build variant.

**Vim's `+python3` interface cannot be reused for MicroPython.** `if_python3.c` is written
against the CPython C-API, which MicroPython does not provide. The bridge is purpose-built
either way, so it is built as `esp_*()` eval builtins plus a MicroPython module — not by
enabling `FEAT_PYTHON3`.

**Git: no libgit2 port exists, so write it in MicroPython, not C.** I searched and found no
libgit2 ESP-IDF port, so git means implementing it — and since MicroPython is already being
embedded, **pure Python is the better implementation language here.** The prior art is
directly on point: **`benhoyt/pygit` is ~500 lines of stdlib-only Python that does `init`,
`add`, `commit`, `status`, `diff`, `cat-file`, `ls-files`, `hash-object` **and pushes itself
to GitHub** over smart-HTTP. That is most of the requested scope, already proven.

This is strictly better than a C component on every axis that matters here: far less flash
(Python/`.mpy` source versus a compiled git), editable and debuggable on the device, and it
reuses machinery the build already has rather than duplicating it. Two consequences:
**the MicroPython phase becomes a hard dependency of the git phase**, and the heavy lifting
must stay in C:

| Need | Source |
|---|---|
| SHA-1 | MicroPython `hashlib`, mbedTLS-backed — needs `MICROPY_PY_HASHLIB_SHA1` |
| deflate/inflate | MicroPython `deflate` — **decompression is on by default but compression needs `MICROPY_PY_DEFLATE_COMPRESS`** ("full features" level, i.e. a custom build — which we are doing anyway) |
| HTTPS / SSH transport | the existing native `esp_http_*` and libssh2 builtins, exposed to Python — *not* reimplemented in Python, and not `urllib.request`, which MicroPython lacks |
| diff | Vim's bundled `xdiff/` exposed as a builtin — not `difflib`, which MicroPython lacks |

What pygit does **not** provide is the read side: it pushes but never clones or fetches, so
**packfile reading with delta resolution (ofs-delta and ref-delta) is net-new work** and is
the largest remaining piece. Conversely, the thing that makes *push* affordable is that
**a packfile containing no deltas is still a valid packfile** — every object can be written
as a full zlib entry, so `git-receive-pack` is satisfiable with no delta compression at all.
That costs bandwidth, not correctness.

Dulwich was considered and rejected: it is a complete pure-Python git, but it is
CPython-scale and leans on `typing`, `dataclasses`, `os.scandir`, `shutil`, `tempfile`,
`urllib` and ABCs — none of which MicroPython provides.

**ESP-IDF version: pin 5.5.x.** `esp-bsp/bsp/m5stack_tab5/idf_component.yml` declares
`idf: ">=5.4"`. IDF 6.x moved VFS defaults around (TERMIOS now off by default per its own
storage migration guide), so 5.5.x is lower-friction and satisfies the BSP.

### Neovim: assessed and rejected

- **LuaJIT has no RISC-V backend at all** — no RV32 port, so the P4 cannot run Neovim's
  default Lua.
- The `PREFER_LUA=ON` fallback to PUC Lua 5.1 is **currently broken upstream** —
  neovim/neovim issue #36516, "building NeoVim for RISC-V *without* LuaJIT", is open.
- **libuv is the deeper blocker.** Neovim's event loop is libuv, which has no ESP-IDF
  platform and no generic `poll()` backend — it wants `epoll`, which ESP-IDF's lwIP does
  not provide. Porting libuv is a larger job than porting Vim.
- Neovim additionally needs host-side code generators, tree-sitter and a terminfo database,
  and its TUI is architecturally a separate RPC client.

Vim's `os_unix.c` is a few dozen `mch_*` functions over plain POSIX. **This plan ports Vim.**

### ESP32-S3 portability

**The entire shim layer is chip-independent** — it sits on newlib + VFS + FreeRTOS, identical
on S3. What differs:

| | ESP32-P4 | ESP32-S3 |
|---|---|---|
| ISA | RISC-V RV32IMAFC, dual 400 MHz | Xtensa LX7, dual 240 MHz |
| PSRAM | up to 32 MB | up to 8 MB (octal) |
| Radio | **none** — C6 over esp-hosted | **native** WiFi + BLE, simpler `:Esp*` path |
| Console | MIPI-DSI 1280×720 | SPI / RGB LCD, much smaller |
| USB host | USB 2.0 HS OTG | USB 1.1 FS OTG (HID host still fine) |
| Stack cost | flat | **windowed ABI — deeper frames** |
| Emulator | `--chip esp32p4` | `--chip esp32s3` |

Design for it: keep chip-specific decisions in the board layer; put the radio behind one
interface with native and hosted implementations (S3 is the *easier* case); the 8 MB PSRAM
ceiling wants smaller `'maxmem'`/`'maxmemtot'` and a trimmed runtime; and Xtensa's register
windows make Vim's recursive regexp/eval more stack-hungry, so stack size is a per-target
Kconfig value, not a constant. Target S3 as a **build variant from Phase 5 onward**, not a
fork.

---

## Phase 0 — Repository bootstrap

Everything below assumes this exists first. `git` 2.53.0 and `git-lfs` 3.7.1 are already
installed on this machine; the directory is **not yet a repository**.

1. `git init`, `git lfs install`, and a `.gitattributes` tracking
   `third_party/**/*.tar.gz`, `*.tar.xz`, `*.zip` through LFS.
2. **No third-party source is committed as files — only LFS-tracked archives plus our
   patches.** The two trees already on disk are converted, not kept:
   - `vim/` is a shallow clone at `12c69acc` (9.2.1125). Produce
     `third_party/vim-9.2.1125.tar.gz` from that exact commit, then **delete the `vim/`
     working tree** so it cannot be edited by accident.
   - `tools/esp-emu-0.43.0-x86_64-unknown-linux-gnu.tar.gz` is already the upstream archive
     and moves to `third_party/` as-is, keeping the verified `SHA256SUMS`.
3. **Host toolchain is pinned in `pixi.toml`** — ncurses (Vim's `configure` will not
   finish without a terminal library, even though the ESP build undefines
   `HAVE_TGETENT`), socat (raw-mode terminal for the emulator UART; `nc` cannot do it),
   openssh (local `sshd` for Phase 8 push tests), and python 3.13 (ESP-IDF's venv
   bootstrap needs `ensurepip`, which the system Python lacks). `scripts/env.sh` enters
   pixi + ESP-IDF in one step; `pixi task list` is the task index. No root required.
4. **The LFS archive is the source of truth — a fresh clone needs zero network beyond
   `git lfs pull`.** There is deliberately no fetch-at-build-time step: LFS holds the
   archive, and `scripts/prepare-deps.sh` extracts it into a git-ignored `build-deps/` and
   applies our patch series in order. It must be **idempotent**: re-extract clean rather
   than patch an already-patched tree. Nothing in `build-deps/` is ever committed.
5. `third_party/manifest.txt` records name, version, upstream URL and sha256 — and its
   hash **attests the committed LFS blob, not upstream**. That distinction is deliberate:
   a tarball we generate with `git archive` depends on our local git and gzip, and GitHub's
   auto-generated tag tarballs are not byte-stable over time, so neither can be checked
   against a published upstream digest. The URL is provenance; the hash is integrity of
   what we vendored. `scripts/add-dep.sh` is the only thing that touches the network — used
   when onboarding a *new* upstream archive, it downloads, records the hash, and stages the
   file into LFS.
6. **Patch authoring workflow** — write this into `docs/DECISIONS.md`, because Phase 2
   opens by producing four patches and nobody hand-writes a `.patch`:
   `prepare-deps.sh` to get a clean `build-deps/vim/` → `git init && git add -A && git
   commit` a throwaway repo *inside the extracted tree* → edit → `git diff >
   patches/vim/000N-name.patch` → re-run `prepare-deps.sh` from scratch to confirm it
   applies clean. Patches are generated against the **extracted archive**, never against
   the shallow clone — context lines can differ. `prepare-deps.sh` applies them with
   `git apply --whitespace=nowarn` (falling back to `patch -p1`), and stops on first
   failure rather than continuing with a half-patched tree.
7. `docs/PLAN.md` — this document becomes **the canonical living plan**; any earlier
   draft outside the repository is abandoned at that point so there is only ever one. Keep it
   current as work lands: check off phases, replace provisional numbers with measured ones
   (the `size-components` output, the final partition table), and amend decisions in place
   when reality disagrees. `docs/DECISIONS.md` carries the reasoning so the plan stays
   readable.
8. Initial commit, `main` branch.

**Which dependencies get the archive+patch treatment:** everything we build from source or
patch — Vim and MicroPython certainly. ESP-IDF **managed components** (`libssh2_esp`, the
BSP, LVGL) are declared in `idf_component.yml` with pinned versions and a committed
`dependencies.lock`; the component manager fetches them reproducibly, so they do not need
vendoring unless we end up patching one — at which point it moves to `third_party/` under
the same rules.

---

## Repository layout

Upstream source is never committed and never edited in place — it is an archive plus a
patch series, which is also what makes re-syncing to a newer Vim a tractable operation.

```
esp-vim/
  .gitattributes                  # LFS: third_party/**/*.tar.gz etc.
  docs/
    PLAN.md                       # this document, kept current
    DECISIONS.md
  third_party/                    # LFS-tracked archives ONLY, never extracted source
    manifest.txt                  # name, version, url, sha256
    vim-9.2.1125.tar.gz
    esp-emu-0.43.0-x86_64-unknown-linux-gnu.tar.gz
    micropython-1.29.0.tar.gz
  patches/
    vim/
      0001-feature-h-esp-undefs.patch
      0002-os_unix-expand-wildcards.patch
      0003-evalfunc-esp-builtins.patch
      0004-netrw-native-transports.patch
    micropython/
      0001-idf-assert-func-clash.patch
  pixi.toml                       # pinned host toolchain + task index
  scripts/
    env.sh                        # source: enters pixi + ESP-IDF in one step
    prepare-deps.sh               # verify sha256, extract to build-deps/, apply patches
    add-dep.sh                    # the ONLY networked script: onboard a new upstream archive
    make-runtime-image.py         # curate $VIMRUNTIME -> build-deps/vimrt/, generate filetype.vim
    merge-image.sh                # esptool merge-bin incl. vimrt + data
    run-emu.sh                    # esp-emu wrappers, incl. the two-process hosted fixture
  build-deps/                     # .gitignore'd: extracted + patched upstream
  esp-vim/                        # the ESP-IDF project
    CMakeLists.txt
    sdkconfig.defaults{,.esp32p4,.esp32s3}
    partitions.csv
    main/                           # app_main, task setup, console selection
    components/
      vim/
        CMakeLists.txt              # mirrors BASIC_SRC from build-deps/vim/src
        port/
          config.h                  # generated once on host, then curated
          osdef.h                   # hand-written stub
          pathdef.c                 # hand-written
          esp_shims.c               # the mch_* replacements
          esp_console_uart.c
          esp_console_dsi.c         # Phase 10
      esp_vim_api/                  # C: the esp_*() eval builtins
        esp_api_wifi.c  esp_api_gpio.c  esp_api_sensors.c
        esp_api_ble.c   esp_api_sys.c   esp_api_nvs.c
        esp_api_serial.c
        esp_api_fs.c                # ONE file-op core + path validation
        esp_api_net.c               # scp / sftp / http transports
        esp_api_usb.c               # MSC device mode
        esp_api_web.c               # https file manager
        web/                        # embedded static UI assets
        esp_api_status.c            # snapshot published BY the Vim task, read by httpd
      esp_kbd/                      # Tab5 I2C keyboard + USB-HID keyboard
      esp_micropython/              # MicroPython embed port as an IDF component
        mp_mod_vim.c                # the `vim` module (buffers, cursor, eval)
        mp_mod_esp.c                # the `esp` module (gpio/i2c/wifi/nvs/fs)
        frozen/
          espgit/                   # pure-Python git, frozen to .mpy
    c6-slave/                       # esp-hosted slave app, built for esp32c6
    runtime-image/                  # OUR files only; vim's runtime is copied in at
      plugin/esp.vim                #   build time from build-deps/ by the script
      pack/esp/start/espfiles/      # two-pane file manager
    test/                           # emulator regression harness
```

Note `runtime-image/` holds only files we author. netrw and the curated `syntax/`,
`colors/`, `autoload/` and `spellfile.vim` are copied out of `build-deps/vim/runtime/` by
`make-runtime-image.py`: committing them would be checking in third-party source under
another name.

`-Dmain=vim_main` renames Vim's entry point; no patch needed for that.

---

## Phase 1 — Capability spike (GO/NO-GO GATE)

**Do this before anything else.** The whole UART-console premise rests on facts that have
not been verified, and discovering any of them false after building `config.h` would waste
days. A ~60-line IDF app, built for `esp32p4`, run under `esp-emu`, reporting:

1. `isatty(0)` on the UART VFS fd. **If false, Vim starts in filter mode and never
   renders.** The single most important line in the spike.
2. `select()` on fd 0 — does it block, and does it wake on input? (`--inject` supplies it.)
3. `tcgetattr(0,&t)` / `tcsetattr(0,TCSANOW,&t)` with `CONFIG_VFS_SUPPORT_TERMIOS=y` — do
   they return 0?
4. Writable FS: mount FATFS at `/fat`; `mkdir`/`open`/`write`/`stat`/`opendir`/`chdir`/
   `getcwd`; report what `stat` fills in for `st_ino`, `st_dev`, `st_mtime`.
5. **Read-only runtime partition** — mount an image partition at `/vimrt`, `open`/`read`/
   `stat` a file and `opendir` a subdirectory. Because the target is `FEAT_NORMAL` from the
   first boot, `+eval` and `+syntax` are live immediately and Vim sources
   `$VIMRUNTIME/defaults.vim` and `syntax/synload.vim` *during startup*. If this is not
   proven here, the first real boot fails inside runtime sourcing with no way to tell
   whether the fault is the partition, the mount, or `pathdef.c`.
6. **Glob on the startup path.** Runtime sourcing globs, so `mch_expand_wildcards()` is a
   startup dependency under `FEAT_NORMAL`, not an `:e *.c` convenience. Determine whether
   returning `FAIL` makes Vim fall back to the internal matcher in `findfile.c`, or whether
   `synload`'s discovery breaks — this decides which Phase 3 option is viable.

Free to record while here: does `system()` link? `fork()`? `getpwuid()`? does
`signal(SIGINT,h)` link, and does anything deliver?

**Gate:** if `isatty(0)` is false, decide the fix *here* — a VFS shim reporting a tty, or
patching `mch_input_isatty()` — before writing the build.

---

## Phase 2 — Build system and the three generated files

Vim's autoconf cannot cross-compile (`AC_TRY_RUN` probes must execute on target), so
`configure` is used once, on the host, as a *generator*, and its output curated by hand into
a checked-in file.

**`port/config.h`** — baseline from:

```
cd build-deps/vim/src && ./configure --with-features=normal --enable-gui=no --without-x \
  --disable-gpm --disable-sysmouse --disable-netbeans --disable-channel \
  --disable-nls --disable-selinux --disable-canberra --disable-libsodium \
  --disable-acl --disable-xsmp --disable-gtktest
```

Force **off**: `HAVE_TGETENT` (use builtin termcaps), `HAVE_FORK`, `HAVE_SETPGID`,
`HAVE_SETSID`, `HAVE_UNION_WAIT`, `HAVE_LSTAT`, `HAVE_READLINK`, `HAVE_ICONV*`,
`HAVE_LIBINTL_H`, `HAVE_GETPWNAM`/`HAVE_GETPWUID`, `HAVE_SYS_PTMS_H`, `HAVE_TERMCAP_H`,
`HAVE_SIGALTSTACK`, `HAVE_SIGSTACK`, `HAVE_SELINUX`, `HAVE_POSIX_OPENPT`.
Keep **on**: `HAVE_SELECT`, `HAVE_TERMIOS_H`, `HAVE_DIRENT_H`, `HAVE_GETTIMEOFDAY`,
`HAVE_NANOSLEEP`, `HAVE_QSORT`, `HAVE_STRFTIME`, `HAVE_SETENV`, `HAVE_GETCWD`,
`HAVE_FSYNC`, `HAVE_FCHDIR` (verify in spike). Add `#define USE_SYSTEM`, `#define UNIX`.

**`port/osdef.h`** — vim's Makefile generates this by preprocessing system headers via
`osdef.sh`; that machinery will not work here. Hand-write a near-empty stub (`vim.h`
includes it unconditionally).

**`port/pathdef.c`** — hand-written: `all_cflags`, `all_lflags`, `compiled_user`,
`compiled_sys`, and crucially `default_vim_dir` / `default_vimruntime_dir` = `/vimrt`.

**`patches/0001-feature-h-esp-undefs.patch`** — append an `#ifdef ESP_PLATFORM` block to
`feature.h` undefining what the platform cannot serve: `FEAT_CLIPBOARD`, `FEAT_XCLIPBOARD`,
`FEAT_WAYLAND*`, `FEAT_PRINTER`/hardcopy, `FEAT_CSCOPE`, `FEAT_NETBEANS_INTG`, `FEAT_SOUND`,
`FEAT_PROFILE`, `FEAT_SODIUM`.
Leave **on**: `FEAT_EVAL`, `FEAT_SYN_HL`, `FEAT_QUICKFIX` (`:vimgrep` works even though
`:make` cannot), `FEAT_DIFF` (vim's bundled `xdiff/` is internal — no external `diff`),
`FEAT_PERSISTENT_UNDO`, and **`FEAT_SPELL`** — the code compiles in; only the `.spl` data
is omitted, fetched at runtime. Never define `FEAT_JOB_CHANNEL` or `FEAT_TERMINAL` (both
need fork+pty).

**`components/vim/CMakeLists.txt`** — list the `BASIC_SRC` files from `src/Makefile:1503`
by relative path into `build-deps/vim/src/`. GUI/X11/pango/cairo files in that list compile to
empty translation units without their feature macros; keep them for list fidelity. Flags:
`-DHAVE_CONFIG_H -DFEAT_NORMAL -Dmain=vim_main -include port/config.h`, `-I port/`,
`-I <build-deps>/vim/src/`, plus `-Wno-*` for upstream's older-C idioms.

---

## Phase 3 — The OS shim layer (`port/esp_shims.c`)

The substance of the port.

**Shell and globbing.** `USE_SYSTEM` routes `mch_call_shell()` through `system()` — provide
one that emits `E371: Command not found` and returns -1. **`USE_SYSTEM` does not cover
`mch_expand_wildcards()`**, which independently forks a shell to glob, and under
`FEAT_NORMAL` is on the **startup path**. Per the Phase 1 finding, either return `FAIL` and
let Vim fall back to the internal matcher in `findfile.c`, or route it there directly.

**Terminal mode.** Do *not* drive raw mode through termios — IDF's UART VFS termios is a
partial `c_lflag` mapping. Enable `CONFIG_VFS_SUPPORT_TERMIOS=y` only so Vim's calls don't
hard-fail, and reimplement `mch_settmode()` against the UART VFS driver:
`uart_vfs_dev_port_set_rx_line_endings(UART_NUM_0, ESP_LINE_ENDINGS_LF)` and the matching
`_tx_line_endings`, plus non-blocking reads.

**Window size.** No `TIOCGWINSZ`; `mch_get_shellsize()` already falls back to
`$LINES`/`$COLUMNS`, so set them. No `SIGWINCH` either — on the DSI console call
`shell_resized()` directly.

**Signals.** Nothing delivers them. **CTRL-C arrives as byte 0x03 in the input stream** —
the input reader must recognise it and set `got_int`, or interrupt will not work at all.
Ignore CTRL-Z (no job control).

**Environment**, set in `app_main` before entering Vim: `HOME=/fat`, `TERM=xterm`,
`SHELL=` (empty), `VIM=/vimrt`, `VIMRUNTIME=/vimrt`, `LINES`, `COLUMNS`.

**Remaining stubs:** `getpwuid`/`getpwnam` → a static "esp" entry so `~` expands;
`mch_get_user_name()` → `"esp"`; `uname()` → `"ESP-IDF"`; `getuid`/`geteuid` → 0;
`lstat` → `stat`; `readlink`/`symlink` → `FAIL`; `mch_exit()` must **not** call `exit()`
(IDF's `exit()` restarts the chip) — return to the launching task or `esp_restart()`
deliberately.

**Compiled-in defaults:** `noswapfile nobackup nowritebackup noundofile viminfo=
encoding=utf-8 nomodeline`. FATFS reports `st_ino`/`st_dev` as 0, so Vim's same-file
detection degrades — another reason backup-by-rename stays off.

**Memory and stack.** *(Amended in Phase 4. The original plan said "small allocations kept
internal" and "default `re=2` (NFA engine)". Both were wrong; see
[PHASE4.md](PHASE4.md).)* **All of Vim's heap goes to PSRAM**. Under IDF's default
policy, Vim's thousands of small allocations exhausted internal RAM and starved ESP-IDF
itself. The **backtracking regexp engine (`re=1`)** is used, not NFA. It keeps its state
on a heap `regstack` rather than the C stack, and NFA cost ~20 MB just to open a file.
The regexp timeout is implemented for real (`esp_timer` → SIGALRM) so pathological
patterns are cut off. Give the Vim task **48–64 KB** of stack as a per-target Kconfig
value (higher on S3 for the windowed ABI).

---

## Phase 4 — Storage and the curated runtime

`partitions.csv` for 16 MB, single app slot (no OTA — the space is better spent):

| Partition | Size | Purpose |
|---|---|---|
| `nvs`, `phy_init` | **64 KB** + 4 KB | `:EspNvs`, plus TLS cert+key, PBKDF2 hash+salt, WiFi credentials, SSH host keys (these grow), keyboard config — the default 24 KB is too small once the web server lands |
| `factory` (app) | **7 MB, provisional** | vim + IDF + mbedTLS + libssh2 + wifi_remote + usb host + TinyUSB + httpd + BSP/LVGL + **MicroPython** + frozen git `.mpy` + embedded web assets |
| `vimrt` (ro FAT or SPIFFS) | 3 MB | curated `$VIMRUNTIME`, mounted **`/vimrt`** |
| `data` (FAT + wear levelling) | ~5.5 MB | user files, git repos, `/fat/spell`, mounted **`/fat`** |

Mount points are fixed here and referenced by Phase 1's spike, Phase 3's environment and
`pathdef.c`. On Tab5, microSD mounts as a third root (`/sd`) in Phase 9.

**Flash is now the binding constraint, and this table is provisional until measured.** The
feature set has grown well past what a 16 MB part absorbs comfortably: Vim NORMAL, mbedTLS
with the X.509 writers enabled, libssh2, esp-hosted/wifi_remote, usb_host + TinyUSB,
`esp_https_server`, the BSP/LVGL stack, MicroPython, and the frozen git package. The
partition table is also the single thing that is painful to change later — once `/fat` holds
user data, resizing means wiping it.

So: **run `idf.py size-components` at the end of Phase 2 and again as each of Phases 6–8
lands**, and finalise the table before any real user data exists. If 7 MB proves
insufficient, the levers in preference order are (1) trim the curated runtime further — it
is the most compressible item, (2) move the runtime image *into* the app partition as
embedded read-only data and drop `vimrt` entirely, (3) drop the `.sug` half of any
downloaded spell data, (4) build MicroPython at a lower feature level, keeping only
`DEFLATE_COMPRESS` and `HASHLIB_SHA1` which git requires. There is no OTA slot to reclaim —
that was already spent.

**Curated runtime image**, built by a script from `build-deps/vim/runtime/`:

- `defaults.vim`, `syntax/synload.vim`, `syntax/syntax.vim`
- **a generated `filetype.vim`** covering only the filetypes in `esp-vim/filetypes.conf`
  (30 as shipped), instead of stock Vim's ~1600 rules. The same file selects which
  `syntax/`, `ftplugin/` and `indent/` files ship.
- `colors/` (a few), and only the `autoload/` that `synload` pulls
- `autoload/spellfile.vim` + `plugin/spellfile.vim` (**203 lines total** — the downloader)
- `pack/dist/opt/netrw/` (628 KB, transports rerouted — Phase 6) **and the top-level
  `plugin/netrwPlugin.vim`**. Both are required and neither is optional: netrw lives in an
  **opt** package, so it is not loaded without `packadd` — and `autoload/spellfile.vim:15-18`
  reads *"If the netrw plugin isn't loaded we silently skip everything"*, guarded on
  `exists(":Nread")`. Ship the package without its loader and spell download fails with no
  error whatsoever. The top-level `plugin/netrwPlugin.vim` is exactly that loader — a 9-line
  shim that runs `packadd netrw` behind a `g:loaded_netrw` guard, so shipping both is
  correct and does not double-load.
- `pack/esp/start/espfiles/` (the two-pane manager — Phase 6)
- `plugin/esp.vim` (the `:Esp*` surface — Phase 6)

**Exclude**: `doc/` (12 MB — `:help` will not work; a stated limitation), `spell/*.spl`
(downloaded on demand), `tutor/`, `lang/`, and the long tail of `ftplugin`/`indent`.

---

## Phase 5 — Emulator bring-up over UART

**Building the image the emulator wants.** `--firmware` takes a *merged* flash image, but
`idf.py build` emits bootloader, partition table, app and the two data partitions
separately. The harness must merge explicitly, including `vimrt` and `data` — otherwise the
emulator boots with empty data partitions and Phase 1's runtime failure mode arrives anyway:

```
esptool merge-bin -o build/merged.bin --flash-size 16MB \
  0x2000 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0x10000 build/esp-vim.bin \
  <vimrt-off> runtime-image/vimrt.img \
  <data-off>  build/data.img
esp-emu --chip esp32p4 --psram-size 32M --firmware build/merged.bin
```

**Persistence between runs.** `esp-emu` starts from the image each time, so a second run
does not see what the first wrote — the two-run check below would assert nothing. Use
`--save-state`, which writes flash/NVS back **over the firmware file**; the harness must
`cp build/merged.bin build/run.bin` and run the copy so the pristine image survives.

**The host-side raw-mode trap.** `esp-emu` bridges UART0 to stdin/stdout but does not put
*your* terminal in raw mode. Running it plainly shows doubled echo and arrow keys as literal
escapes — that is the host tty, not the port. Two distinct test paths:

- **Automated (the CI gate, no tty involved):** `--inject-on` matches UART TX, so it must
  trigger on a **stable string `app_main` itself prints** — e.g. `ESPVIM-READY` before
  entering `vim_main` — never on Vim output, which is not stable. Run one, against the
  copied image with `--save-state`:
  `--inject "ihello world:w /fat/t.txt\r:q!\r" --inject-on "ESPVIM-READY"
  --exit-on "ESPVIM-EXIT"`. Run two, against the *saved* image, has `app_main` read
  `/fat/t.txt` back over UART before launching Vim; the harness asserts its contents.
- **Interactive:** `--uart-tcp 127.0.0.1:5555` plus
  `socat -,raw,echo=0,escape=0x1d TCP:127.0.0.1:5555`. Use `socat`, not `nc` — `nc` will
  not give raw mode.

`--gdb 1234 --gdb-halt` with `riscv32-esp-elf-gdb` for the inevitable stack overflow;
`--trace` + `--elf` for a task timeline if redraw is slow.

**Bring-up order:** boots → `vim_main` reached → `isatty` path taken → screen clears and the
`~` column renders → cursor keys move → insert text → `:w` to `/fat` → `:q` → reopen and
verify → syntax highlighting on a `.c` file (proves the runtime partition and `+eval`) →
`:vimgrep` (proves quickfix).

**Add the S3 variant here**, not later: `--chip esp32s3 --psram-size 8M` with
`sdkconfig.defaults.esp32s3`. Keeping both green from the start is what stops chip-specific
assumptions from setting.

---

## Phase 6 — The `:Esp*` command layer, file manager and transports

**Delivered in stages**, each ending with a green P4 gate, because the phase is too large
for one:

| Stage | Scope | State |
|---|---|---|
| 6a | builtin plumbing (patch, build check); `:EspInfo` `:EspHeap` `:EspTasks` `:EspGpio` `:EspNvs` `:EspReboot` | **done** |
| 6b | `esp_fs` (one file-ops core with path validation) and the two-pane `:EspFiles` manager, local roots | **done** |
| 6c | networking in the emulator (EMAC + `--net user`), `esp_http_get`, spell download | next |
| 6d | libssh2: SCP/SFTP builtins, netrw transports, remote panes | |
| 6e | web file manager, settings, live status | |
| 6f | radio via esp-hosted/C6 (`:EspWifi*`, `:EspBle*`), `:EspSerial`, `:EspI2cScan`, `:EspAdc`, `:EspSensors`. Also establish, for Phase 11: can esp-hosted carry a BLE **HID host** (NimBLE on the P4, controller on the C6), and what does `esp-emu --ble-hci` bridge to? | |
| — | `:EspUsbMsc` | hardware only, Phase 9 |
| 6z | **libgit2 feasibility build** (see Phase 8): decides how Phase 8 is built | after 6f, before Phase 7 |

### Architecture: thin C, thick vimscript

Two layers, chosen so that the command surface stays editable without reflashing and the
upstream patch footprint stays at a single site:

1. **C builtins** in `components/vim/api/` (not a separate component: they need Vim's
   private headers and allocator), registered by `patches/vim/0008-evalfunc-esp-builtins`
   into `global_functions[]` in `evalfunc.c`. **This table cannot be appended to** —
   `find_internal_func_opt()` (`evalfunc.c:~2000`) does a `STRCMP` **binary search** over it,
   so out-of-order rows silently break lookup for *unrelated* builtins, surfacing as bizarre
   `E117` errors far from the cause. Entries must go in alphabetical position. The saving
   grace of the `esp_` prefix: every one of them sorts between `{"escape"}` (`evalfunc.c:2280`)
   and `{"eval"}` (`:2282`), so the patch is **one contiguous hunk at a single site**, not
   scattered. Row shape is `{name, min_argc, max_argc, FEARG_*, arg_type_check_fn,
   ret_type_fn, impl_fn}` — the `arg_*`/`ret_*` slots are mandatory and a row omitting them
   will not compile. Each function returns a **Dict or List**, never formatted text —
   formatting is vimscript's job. Roughly:
   `esp_wifi_scan()`, `esp_wifi_connect()`, `esp_wifi_status()`, `esp_gpio()`, `esp_adc()`,
   `esp_i2c_scan()`, `esp_sensor()`, `esp_ble_scan()`, `esp_nvs_get()`, `esp_nvs_set()`,
   `esp_heap()`, `esp_tasks()`, `esp_info()`, `esp_reboot()`, `esp_serial_*()`,
   `esp_http_get()`, `esp_scp_get()`, `esp_scp_put()`.
2. **`plugin/esp.vim`** in the runtime image, defining every user-facing command with
   `:command` — argument counts, `-bang`, `-complete=customlist` for pin numbers, SSIDs and
   remote paths — and rendering results into scratch buffers with syntax highlighting.

This depends on `+eval`, which `FEAT_NORMAL` gives us.

### Command surface

| Command | Notes |
|---|---|
| `:EspWifiScan` | scratch buffer: SSID / RSSI / channel / auth, sortable |
| `:EspWifiConnect {ssid} [pass]` | credentials persisted to NVS |
| `:EspWifiStatus` | IP, RSSI, uptime; also reports C6 link health |
| `:EspGpio {pin} {on\|off\|read\|in\|out}` | `-complete` offers valid pins per target |
| `:EspAdc {channel}` | raw + calibrated mV |
| `:EspI2cScan` | **per-bus**, with buses named — Ext.Port1 (`G0`/`G1`, where the keyboard lives) is a different bus instance from the internal one carrying GT911 / BMI270 / PI4IOE5V6408, and a single flat address list would be misleading |
| `:EspSensors` | BMI270 IMU on Tab5, plus whatever `:EspI2cScan` identifies |
| `:EspBluetooth` / `:EspBleScan` | BLE advertisement list |
| `:EspNvs [key [value]]` | browse/edit the NVS partition |
| `:EspHeap` / `:EspTasks` / `:EspInfo` | free heap by capability; FreeRTOS task table; chip/IDF/vim versions |
| `:EspSerial {port} {baud}` | UART1 as a buffer-backed session — emulator-testable via `--uart1-tcp` |
| `:EspReboot[!]` | `!` skips the unsaved-buffer check |
| `:EspFiles` | the two-pane file manager |
| `:EspUsbMsc {on\|off}` | expose `/fat` over USB-C; unmounts locally first (see below) |
| `:EspWebStart` / `:EspWebStop` / `:EspWebStatus` | the web file manager; refuses to start without a password |
| `:EspWebPasswd` | set the web credential (PBKDF2 hash to NVS) |
| `:EspKeyboard` | Tab5 keyboard status, layer state, RGB indicator control |
| `:EspPyRun [file]` / `:EspPy {expr}` / `:EspPyReset` | MicroPython — Phase 7 |
| `:EspGitInit` `:EspGitAdd` `:EspGitStatus` `:EspGitCommit` `:EspGitLog` `:EspGitDiff` `:EspGitCheckout` `:EspGitPush` `:EspGitClone` `:EspGitFetch` | Phase 8 |

**Radio commands are cross-chip calls.** Per the finding above, WiFi/BLE go through
`esp_wifi_remote` + `esp-hosted` to the C6. Build the API so the radio sits behind one
interface with a hosted implementation (P4/Tab5) and a native one (S3) — and make
"co-processor not responding" a first-class error the commands report cleanly, because it
is a failure mode single-chip ESP32 code never has to handle.

**Emulator testing needs two processes *and two firmware images*.** Start the slave first,
then the host:

```
esp-emu --chip esp32c6 --firmware build-c6-slave/merged.bin \
        --hosted bridge:slave:/tmp/hosted.sock &
esp-emu --chip esp32p4 --firmware build/run.bin \
        --hosted bridge:host:/tmp/hosted.sock
```

The C6 side is **the esp-hosted slave application built as its own ESP-IDF project** for
`esp32c6` — separate target, separate sdkconfig, separate merged image. Add
`esp-vim/c6-slave/` to the repo layout; it is a missing build artifact, not a flag. Script
the pair as a harness fixture with ordered startup and socket cleanup.

### Native transports (`esp_api_net.c`) — this is what makes SCP and spell download work

**In the emulator, bring transports up over EMAC rather than WiFi.** Otherwise the cert
bundle, TLS, `esp_http_get`, libssh2 and the spellfile round trip are all gated behind the
two-process hosted fixture — four independent risks stacked on one bring-up. `esp-emu`
models the **P4's built-in EMAC** (Synopsys DesignWare GMAC), so if it is driveable with
`--net user` in a *single* process, the whole transport stack is provable with no C6, no
bridge and no second firmware. **Verify first which instance `--net user` attaches to when
`--hosted` is in play** — if it attaches to the C6 side, that alone settles the argument.

**But be clear that this is emulator-only scaffolding: the Tab5 has no Ethernet.** The P4's
EMAC is a MAC with no PHY or magjack on this board — nothing in the Tab5 spec lists
Ethernet. So on real hardware *every* network transport rides esp-hosted to the C6. The
convenience of the EMAC path is real, but it means Phase 6 does not exercise the production
network path at all; Phase 9 carries an explicit gate for that.

There is no fork, so netrw cannot shell out to `curl`/`wget`/`scp` as it does on Unix.
Patch netrw's *transport dispatch* (`0004-netrw-native-transports.patch`, targeting its
`NetrwMethod` / `s:NetrwExe` path) to call the native builtins instead:

- **scp** → `esp_scp_get()`/`esp_scp_put()` over **`skuodi/libssh2_esp`** (libssh2 1.11.1,
  mbedTLS backend, from the Component Registry), using `libssh2_scp_recv2` /
  `libssh2_scp_send64`. Key-based and password auth; host keys in NVS.
- **sftp** → `esp_sftp_*()` on the same libssh2 connection. Worth having alongside SCP
  because **SFTP can list directories and SCP cannot** — this is what lets a pane of the
  two-pane manager *browse* a remote host rather than only push and pull blind.
- **http/https** *(stretch)* → `esp_http_get()` over `esp_http_client` with the **mbedTLS
  certificate bundle** enabled for verification.
- **ftp/rcp/rsync** → removed; they have no native backend and would fail confusingly.

**Spell download then needs no extra work.** `autoload/spellfile.vim` (203 lines) does every
fetch via `:Nread`, so once netrw's https transport is native, `:set spelllang=en spell`
prompts and downloads `en.utf-8.spl` to `/fat/spell` unmodified. Set `spellfile` and prepend
a writable `/fat` entry to `'runtimepath'` so the download has somewhere to land — `/vimrt`
is read-only. Note this makes spell support depend on the *stretch* HTTPS transport; if
HTTP slips, spell slips with it.

### One file-operations core, three front ends (`esp_api_fs.c`)

The two-pane manager, the web interface and the MSC/transfer paths must not each grow their
own copy/move/delete/mkdir. Put **one** C layer underneath all of them, and put path
validation there — canonicalise, reject `..` traversal, and confine every operation to the
mounted roots (`/fat`, `/vimrt` read-only, `/sd`). A file manager reachable over the network
is exactly where a traversal bug becomes a remote arbitrary-file-write, so this belongs in
one audited place rather than three.

**This is a concurrency boundary, not just code reuse — design it that way from the start.**
`esp_http_server` runs its handlers in **its own task**, so they call `esp_api_fs.c`
concurrently with the Vim task calling the same functions from eval builtins. Therefore:

- Every operation takes an explicit mutex, and path-validation scratch buffers are
  per-call, never `static`.
- **The httpd task must never touch Vim's API** — no `emsg()`, no buffer or option access,
  no Vim allocator. Vim's globals are single-threaded by construction, and a web handler
  that reports an error through Vim's message system is heap corruption that reproduces
  once a week. The rule: httpd handlers talk only to `esp_api_fs.c` and return an HTTP
  status; anything that must surface in the editor goes on a queue the **Vim task** drains.
- Same family of hazard, cheaper to fix: the web manager can delete or rename a file Vim
  holds open in a modified buffer, and MSC unmounting `/fat` pulls the filesystem out from
  under a live httpd. Interlock them — `:EspUsbMsc on` refuses while the web server runs,
  and the web server refuses to start while MSC is live.

**There are exactly three shared objects across tasks. Keep the list short and explicit, so
the invariant is checkable during implementation rather than rediscovered during debugging:**

| Object | Writer | Reader | Discipline |
|---|---|---|---|
| `esp_api_fs.c` state | either task | either task | mutex per operation; per-call buffers, nothing `static` |
| status snapshot (`esp_api_status.c`) | **Vim task only** | httpd | double-buffered; httpd never writes |
| settings queue | httpd enqueues | **Vim task applies** | single consumer; httpd never calls `:set` |

Everything else stays task-local. In particular the Vim API has exactly one caller — the Vim
task — and MicroPython runs there too (Phase 7), so it inherits that guarantee rather than
needing its own.

### USB-C mass storage

TinyUSB MSC device mode on the **USB-C** port exposes `/fat` (optionally `/sd`) to a host PC
as a plain USB drive — drag-and-drop with no network, no credentials and no client software.
It does not conflict with the USB-A keyboard host port; those are separate controllers.

**The hazard to design for: FAT cannot be safely mounted read-write by two initiators.** If
the PC writes while Vim has `/fat` mounted, the volume corrupts. So `:EspUsbMsc on` must
**unmount `/fat` from the device's VFS** before exposing the LUN, refuse if any buffer is
modified or any file is open, and remount on `:EspUsbMsc off` / cable removal. Offering a
read-only LUN as the default is the safer variant and worth doing.

### Web file manager (`esp_api_web.c`)

More than upload/download: browse, download, upload, rename, move, delete, mkdir — the same
operations as the two-pane manager, over the same `esp_api_fs.c` core, from a browser.
Served by `esp_https_server`. **Off by default**, started explicitly with `:EspWebStart` and
stopped with `:EspWebStop`; `:EspWebStatus` shows the bound URL and active sessions.

Security is not optional here — this publishes the device's filesystem onto the network:

- **HTTPS, not HTTP.** Self-signed cert+key persisted in NVS, fingerprint printed to the
  Vim command line so the user can verify it; be honest in the docs that this is
  trust-on-first-use. **Runtime generation needs two Kconfig symbols that IDF disables by
  default** to save footprint — `CONFIG_MBEDTLS_X509_CRT_WRITE_C` and
  `CONFIG_MBEDTLS_PEM_WRITE_C`. Enabling them is the plan of record, with two mitigations:
  generate an **ECDSA P-256** key rather than RSA-2048 (much faster on the P4, far smaller
  in NVS — PEM RSA-2048 alone is ~3 KB), and if the flash cost proves unaffordable against
  the already-tight app partition, fall back to a build-time `EMBED_TXTFILES` cert and
  state "not per-device" as a limitation.
- **Password required to start.** No default credential and no blank-password path — refuse
  to start until one is set via `:EspWebPasswd`. Store a **PBKDF2 hash with a per-device
  salt** in NVS, never the plaintext.
- **Session cookies** (`HttpOnly`, `Secure`, `SameSite=Strict`) after login, with CSRF
  tokens on every mutating request — a bare `Authorization` header on a file-delete endpoint
  is trivially CSRF-able.
- **Failed-login backoff** and a cap on concurrent sessions; the P4 has no room to absorb a
  brute-force loop.
- Path handling delegated entirely to `esp_api_fs.c`; upload size caps and a bounded
  streaming write so a large upload cannot exhaust PSRAM.

Serve the UI as a couple of static assets embedded in the app partition (`EMBED_FILES`) —
no CDN, since the device may be offline and the page must work regardless.

**Settings panel.** The same authenticated UI edits what currently lives only in NVS and
`:Esp*` commands: WiFi credentials, web-server port and password, editor defaults
(`tabstop`, `expandtab`, `number`, colorscheme), keyboard layer and LED behaviour, MSC
exposure mode, SSH known-hosts and keys, and git author identity. All of it reads and writes
NVS through the same queue discipline as everything else — the httpd task never pokes Vim's
options directly; it enqueues a change and the Vim task applies it with `:set`, so there is
exactly one writer to Vim state.

**Live edit status.** This is precisely where the "httpd must never touch Vim's API" rule
would be violated by the obvious implementation, so invert it: **the Vim task publishes, the
httpd task only reads.** A small mutex-protected (double-buffered) snapshot struct holds
filename, cursor line/column, total lines, word/char/byte counts, modified flag, filetype
and the buffer list. Vim populates it from autocmds — `CursorHold`, `TextChanged`,
`TextChangedI`, `BufEnter`, `BufWritePost` — calling `esp_status_publish()` with values it
already has: **`wordcount()` is a stock Vim builtin** (`evalfunc.c:3310`, 0 args) returning
a dict of `bytes`/`chars`/`words` plus `cursor_*` variants, so the line and word counts need
no new C at all. Rate-limit publishes (`updatetime`-driven) so a fast typist does not
generate a snapshot per keystroke.

Push it to the browser with **Server-Sent Events** — one-way, a plain `text/event-stream`
handler, auto-reconnecting in the browser, and far simpler than the alternative.
WebSockets are available if bidirectional control is wanted later
(`CONFIG_HTTPD_WS_SUPPORT`, with an `esp_http_server` pre-handshake callback), but status
does not need them.

### The two-pane file manager (`pack/esp/start/espfiles/`)

A purpose-built vimscript plugin — netrw is single-pane and its remote half is being
replaced anyway. Wired to `:EspFiles`.

- Two vertical splits, `Tab` switches the active pane, each pane a scratch buffer listing
  name / size / mtime with syntax highlighting for directories, executables and symlinks.
- **Dual key map, buffer-local.** MC F-keys — `F3` view, `F4` edit, `F5` copy, `F6`
  move/rename, `F7` mkdir, `F8` delete, `F10` quit, `Insert`/`Space` tag, `Enter` descend —
  resolve through `builtin_xterm` with no termcap work. But **the Tab5 keyboard has no
  F-key row**, so every one of these gets an equal-status letter alias (`v`, `e`, `c`, `r`,
  `m`, `d`, `q`), and the keyboard driver additionally synthesises F1–F12 on the `Sym`
  layer. Neither input path is second-class: the letter bindings are what make the manager
  usable on the tablet keyboard, the F-keys are what make it feel like MC over UART.
- Roots: `/fat`, `/vimrt`, `/sd` (Tab5), and **remote `scp://` / `sftp://` paths**. A remote
  pane can be *browsed* over SFTP (SCP cannot list), and F5 between a local and a remote
  pane is just `esp_scp_put`/`esp_sftp_*`.
- All mutations go through `esp_api_fs.c`, shared with the web interface.
- Progress and errors in the command line; no blocking UI during transfers.

Netrw ships alongside for familiar single-pane `:Explore`/`:Sex` and directory-buffer
editing.

---

## Phase 7 — MicroPython

Embed MicroPython via **`ports/embed`** (`micropython_embed.h`) as an ESP-IDF component,
following `agatti/micropython-idf-component` / `robdobsn/MicroPythonESP32Embedding`. Pin
**v1.29.0**. Expect and plan for the known **`__assert_func` redefinition clash with IDF's
newlib**; note P4 **chip revision 3.0** as a build-variant axis for pre-rev3 silicon.

Build options to enable explicitly, because they are off at MicroPython's default feature
level and the later git phase depends on them: **`MICROPY_PY_DEFLATE_COMPRESS`** (compression
— decompression alone is the default) and **`MICROPY_PY_HASHLIB_SHA1`**.

**Threading model is the whole design.** MicroPython is not reentrant and Vim's globals are
single-threaded, so the interpreter runs **on the Vim task** — that is what lets the `vim`
module touch buffers directly, and it is non-negotiable given the Phase 7 bridge design.

But "on the Vim task" must not mean "blocks the editor for the duration." A runaway loop and
a blocking socket read are different problems and need different answers:

- **Runaway scripts** — MicroPython's VM hook polls an interrupt flag set by `CTRL-C`
  (byte `0x03`) in the input reader, reusing the `got_int` path from Phase 3.
- **Long I/O** — the VM hook does nothing for a socket blocked in `recv()`. So **every
  blocking operation exposed to Python is chunked and re-entrant**: the native builtin
  performs one bounded unit of work, returns to Python, and the driver loop yields to Vim's
  main loop between units so it can redraw, update the status line and check `got_int`.
  This is exactly why the HTTPS/SSH transports live in C builtins rather than in Python —
  it is what makes the chunking possible.

**This matters most in Phase 8**: a `:EspGitClone` that blocked the editor for an entire
network transfer would be unusable. Git's transport must drive the chunked builtins and pump
Vim between chunks, so a clone shows progress, redraws, and can be interrupted.

Give the heap a fixed PSRAM arena, sized by Kconfig, so a script cannot starve the editor.

**Two bridges, one interpreter:**

1. **`vim` module** — buffers (read/insert/delete lines), the current window and cursor,
   registers, options, `vim.command()` and `vim.eval()`. This is what "script the editor"
   means; it is the MicroPython analogue of `if_python3.c`, which cannot be reused because
   it targets the CPython C-API.
2. **`esp` module** — the same GPIO / ADC / I2C / WiFi / NVS / filesystem calls that back
   the `:Esp*` commands, so both front ends sit on one implementation. Plus `machine`-style
   conveniences where they are free.

**Commands:** `:EspPyRun [file]` (defaults to the current buffer — the edit-run-edit loop
that makes an on-device editor worth having) with output and tracebacks in a scratch buffer
whose errors are `:cwindow`-navigable via quickfix; `:EspPy {expr}` for one-liners;
`:EspPyReset` to reinitialise the interpreter state.

All of this is emulator-testable — no hardware needed.

---

## Phase 8 — Git

> **Under reconsideration (2026-09-24): libgit2 instead of pure Python.** The design
> below chose pure Python because no ESP-IDF port of libgit2 exists. That underrated
> porting it the way Vim was ported, which is likely less work than writing git,
> because packfile reading with delta resolution, the hardest Python piece, is what
> libgit2 already does. libgit2 would also bring full clone, fetch, push and merge, and
> run far faster than interpreted Python. It would remove Phase 8's dependency on
> MicroPython. Its transfer callbacks can cancel, so CTRL-C and the spinner fit the
> `esp_fs` progress pattern. It can use mbedTLS (SHA-1, HTTPS), bundles zlib and its
> HTTP parser, runs without threads, and takes SSH from the libssh2 that 6d brings.
> Licence: GPLv2 with a linking exception, compatible with linking into this firmware.
>
> **Gate, after Phase 6 and before Phase 7:** build libgit2 as an ESP-IDF component for
> the P4 and measure its flash size. Check that the no-`mmap` fallback works on real
> packs, and that FAT settings (`core.symlinks=false`, `core.filemode=false`) behave.
> Then init, commit, clone and push against a host server in the emulator, measuring
> RAM and stack. If it passes, this phase is rewritten around `esp_git_*()` builtins and
> `:EspGit*` commands, and DECISIONS records why. If not, the pure-Python design stands.

**Depends on Phase 7.** Implemented as a MicroPython package (frozen into the app as `.mpy`),
modelled on `benhoyt/pygit`, with the hot paths in C as tabulated in the findings above:
`hashlib.sha1`, `deflate`, the native HTTPS/SSH transports and Vim's `xdiff`.

**Stage 1 — local repository.** A real on-disk `.git` with loose objects, so the repo is
valid for real git the moment you pull the SD card: object read/write (blob, tree, commit,
tag), index v2 read/write, refs and `packed-refs`, `HEAD` and branches. Commands:
`:EspGitInit`, `:EspGitAdd`, `:EspGitStatus`, `:EspGitCommit`, `:EspGitLog`, `:EspGitDiff`,
`:EspGitCheckout`. `:EspGitDiff` renders through Vim's existing `FEAT_DIFF`/xdiff rather
than a Python differ.

**Loose objects on FAT will bite.** A `.git` accumulating thousands of loose objects on a
5.5 MB FATFS partition is pathological for both slack space (every object rounds up to a
cluster) and directory-scan time. `:EspGitGc` — pack loose objects into a packfile — is the
mitigation, and it is nearly free because Stage 2 already builds packfile *writing*. Run it
automatically past a loose-object threshold.

**Stage 2 — push.** pkt-line framing, capability negotiation, `git-receive-pack` over
smart-HTTPS and over SSH (`libssh2` exec'ing `git-receive-pack` on the remote — the same
connection code as SCP/SFTP). **Write delta-free packfiles**: every object a full zlib
entry. Costs bandwidth, saves the single most complex piece of git.

**Stage 3 — clone and fetch.** The genuinely new work, since pygit has no read side:
`git-upload-pack`, want/have negotiation, and **packfile reading with delta resolution for
both `ofs-delta` and `ref-delta`**. Restrict merging to fast-forward only; a real merge
engine is out of scope and should be stated as such.

**Be clear-eyed about this phase.** It is the largest item in the plan and the one most
likely to slip; the pure-Python route makes it affordable, not small. Stage 1 alone delivers
most of the practical value (version history for files edited on the device), so ship the
stages independently rather than as one gate. Performance scales with repo size — fine for a
handful of files, slow for a large tree — and that is an accepted trade.

Also emulator-testable end to end: run a real `sshd` and a bare repo on the host, reachable
over `--net user`.

---

## Phase 9 — Tab5 hardware over UART

Same firmware, real silicon. Flash over USB-C, console on UART0. What the emulator cannot
tell you and this phase will: real flash timing (`memline.c` swap-less behaviour on a slow
medium), PSRAM cache-miss cost on full-screen redraw, actual free heap, whether 32 MB PSRAM
is genuinely present and mapped, and **real C6 SDIO latency** for the radio commands. Add
the **microSD mount** here (`bsp_sdcard_mount`) as `/sd`, and validate `:EspSensors`
against the actual BMI270 and `:EspI2cScan` against the real Grove/M5BUS buses.

---

## Phase 10 — The Tab5 display console

Add `espressif/esp-bsp`'s `m5stack_tab5` component (declares `idf: ">=5.4"`, brings
ILI9881C + GT911 + `esp_lvgl_port`).

**Reuse Vim's own `src/libvterm/`** rather than writing a terminal emulator. The
architecture is a loopback: Vim writes ANSI escapes to its output fd → a VFS device backed
by a ring buffer → `libvterm`'s parser/state/screen maintains the cell grid → a renderer
paints damaged cells to the DSI framebuffer. Keyboard bytes go the other way into Vim's
input fd. Vim believes it is talking to an xterm throughout, so **nothing in the Vim tree
changes for this phase** — which is the point of doing it this way.

- **Geometry:** 1280×720 with an 8×16 bitmap font = 160×45 cells; a 12×24 font gives a much
  more readable 106×30. Make it a build option.
- **Rendering:** direct dirty-rectangle draws beat an LVGL canvas for a character grid — use
  `esp_lcd_panel_draw_bitmap` on the BSP's panel handle with its double framebuffer. Keep
  LVGL out of the hot path.
- **Keyboard — two independent input devices**, both feeding the same byte stream into Vim's
  input fd (`components/esp_kbd/`):

  1. **Tab5 keyboard accessory** (I2C `0x6D` on Ext.Port1, SDA `G0` / SCL `G1`, IRQ `G50`).
     Put the module in **Normal mode** and read row/column events on the interrupt — no
     polling. We then own the full keymap: `Esc` and every `Ctrl-` combination pass through
     untouched, and because **the device has no F-key row**, the `Sym` layer synthesises
     F1–F12 (`Sym`+`1`…`0`, `Sym`+`-`/`=`). Character mode is explicitly rejected — it
     consumes `Sym`/`Aa` inside the keyboard and would make that synthesis impossible.
     Debounce, modifier latching, auto-repeat and n-key rollover live here; the two
     WS2812E LEDs make a good `Sym`/`Ctrl` layer indicator, driven via `:EspKeyboard`.
  2. **USB-HID keyboards** on the USB-A host port (`usb_host` + the HID class driver),
     mapping HID usage codes to the sequences `builtin_xterm` expects — F1–F12,
     Home/End/PgUp/PgDn, modifiers, auto-repeat.

  Both normalise into one internal key event type before byte translation, so the keymap,
  the `Sym`/Fn layer logic and the file manager's bindings are written once.

- **Merging three input sources.** With UART, the I2C keyboard and USB HID all potentially
  live, they converge on a **single producer queue feeding Vim's one input fd** — each
  source is a producer, the console layer is the sole consumer, and Vim never sees more than
  one stream. Under the "both mirrored" Kconfig console this is what keeps a UART debug
  session and the on-device keyboard from interleaving mid-escape-sequence: escape
  sequences are enqueued atomically per key event, never byte-at-a-time.
- **Touch:** GT911 is present; tap-to-position-cursor and drag-to-scroll fed through Vim's
  existing `+mouse` xterm-protocol path, so again no Vim changes. The file manager benefits
  most. Optional.
- **Resize:** no `SIGWINCH` — call `shell_resized()` directly on font/geometry change.
- **Console selection:** a Kconfig choice (UART / DSI / both mirrored) so the emulator path
  keeps working unchanged. Mirroring both is very useful when debugging the display.

## Phase 11 — Bluetooth keyboards (late-stage goal)

Pair a Bluetooth keyboard and type into Vim, on the Tab5 or any board, with the keyboard
reconnecting by itself after a reboot.

**BLE keyboards only, and this has to be said everywhere.** Neither target has Bluetooth
Classic: the Tab5's radio is its ESP32-C6, which is BLE-only, and so is the ESP32-S3.
Supported keyboards are those speaking **HID over GATT (HOGP)**. Most current keyboards do,
often alongside Classic. Older Classic-only keyboards can't be supported on this
hardware.

**Depends on:** Phase 6f (BLE through esp-hosted on the P4, native on the S3) and Phase
10's input layer (`components/esp_kbd`). It doesn't depend on the display, so it also works
on UART-console builds, e.g. an S3 dev board with a BLE keyboard.

### Stack

- **Host stack NimBLE on the P4, controller on the C6**, with HCI carried over the
  esp-hosted link. Whether esp-hosted-mcu supports that split for a HID host (not just
  scanning) is **the first thing to verify**, and belongs in Phase 6f's BLE bring-up. On
  the S3, NimBLE and the controller are both native.
- **ESP-IDF's `esp_hidh`** (HID host) does GATT discovery and report-map parsing, with
  boot-protocol keyboards as the fallback.
- Flash cost of NimBLE plus `esp_hidh`: **to be measured** against the app partition,
  alongside MicroPython and (if Phase 8 goes that way) libgit2.

### Input path

HID reports become the **same normalised key events** as USB HID and the Tab5 keyboard
(Phase 10), and join the single input queue feeding Vim's one input fd. The keymap, the
F-key handling and the file manager's bindings stay written once. Specific to Bluetooth:

- **Auto-repeat is ours.** A HID keyboard reports key state, not repeats, so the input
  layer generates repeat, as for USB HID.
- **Layouts:** HID usage codes are positions, not characters. The layout table (US by
  default, selectable) is shared with USB HID, not duplicated.
- **Latency:** request a short connection interval (7.5–15 ms) for typing. On the Tab5,
  WiFi and BLE share the C6's single radio, so **measure keystroke latency during a
  WiFi transfer** and tune coexistence if it's noticeable.
- **Battery level** from the keyboard's Battery Service, shown by `:EspBtKeyboard` and
  available to the status line.

### Commands

| Command | Does |
|---|---|
| `:EspBtKeyboard scan` | list advertising BLE keyboards (HID appearance) in a view |
| `:EspBtKeyboard pair {n}` | pair and bond with entry {n}; shows a passkey to type on the keyboard if it asks |
| `:EspBtKeyboard` | status: bonded keyboards, which is connected, battery, latency |
| `:EspBtKeyboard forget [{n}]` | remove a bond (all, with no argument) |

Bonds are stored in NVS by the BT stack. On boot, bonded keyboards reconnect when you
press a key; nothing scans unless asked, to save power on battery.

### Pairing with no keyboard: the touch overlay (boards with a touch screen)

A board with a touch screen (the Tab5, an S3 CYD) must be able to pair its first Bluetooth
keyboard **with nothing else attached**: no built-in keyboard, no USB keyboard, no serial
cable. On a CYD, which has no keyboard of its own, this is how every keyboard gets paired. Pairing only needs taps, because a
BLE passkey is typed on the Bluetooth keyboard itself, so a touch overlay is enough and
no on-screen keyboard is needed. Decided with the user, 2026-09-24:

| Question | Decision |
|---|---|
| How pairing starts | **Automatically.** No other way is needed without a keyboard |
| UI toolkit | **LVGL overlay** (LVGL comes with the Tab5 BSP), not a Vim view |
| On-screen keyboard | **None.** Pairing only; a general OSK is out of scope |
| Boards without a screen | **Not supported.** They pair over serial or USB with `:EspBtKeyboard` |
| A bonded keyboard doesn't show up at boot | **Wait ~10 s** (a keypress wakes most keyboards), then show "Waiting for <name>… / Pair a different keyboard" |
| The only keyboard disconnects mid-session | **Show the overlay automatically** once it has been gone ~15 s |
| "Just Works" keyboards (no passkey) | **Allowed after a tap** on "Pair", which a nearby attacker can't do |
| How many | **Several bonded (up to 4), one active**: whichever connects is used |

**When it appears.** "No keyboard" means no connected Bluetooth keyboard, no Tab5 keyboard
answering at I2C 0x6D, and no USB HID keyboard enumerated. Serial input doesn't count,
since there's no telling whether anyone is on the other end. The overlay never appears
while a keyboard is present, and closes itself as soon as one connects.

**What it shows.**
- Nearby keyboards advertising HID (name, signal strength), refreshed as they appear, and
  the bonded ones marked as such.
- Tap one to pair. A passkey keyboard gets the six digits in large type: "Type 123456 on
  the keyboard, then Enter". A Just Works keyboard gets a "Pair" confirmation.
- "Not now" dismisses it until the next boot or disconnect. Long-press a bonded keyboard
  to forget it, which frees a slot when all four are used.

**How it's built.**
- LVGL runs in **its own task**, independent of Vim. The overlay works while Vim is busy,
  and in the gap between sessions after `:q`.
- **Display ownership:** Phase 10's terminal renderer draws straight to the panel
  (dirty rectangles, no LVGL in the hot path). While the overlay is up, the renderer stops
  drawing but libvterm keeps its screen model current. LVGL takes the panel, and touch goes
  to LVGL instead of the touch-as-mouse path. On dismiss, the terminal repaints fully. One
  owner at a time: no compositing.
- The overlay talks to the BT host through the same thread-safe pairing API that
  `:EspBtKeyboard` uses, so both front ends share one implementation. It never touches
  Vim. Notices Vim should show (e.g. "Paired: <name>") go on the Vim-drained queue.

**Defaults I chose; not yet confirmed:**
- the 10 s and 15 s timeouts;
- the limit of 4 bonds;
- long-press to forget;
- the overlay can't be opened by hand; with a keyboard present, use `:EspBtKeyboard`.

### Threading and security

- NimBLE callbacks run on the BT host task. As with the web server (Phase 6), they
  **never touch Vim**. Key events go into the input queue, which is already
  multi-producer. Pairing prompts, passkeys and connect/disconnect notices go on a queue
  the **Vim task** drains, a fourth entry in Phase 6's short list of shared objects.
- **A keyboard is a keystroke injector** into an editor that writes files and runs
  MicroPython. Input is accepted **only from bonded devices**, bonding needs an explicit
  `:EspBtKeyboard pair` with passkey or numeric comparison where the keyboard supports
  it, and "Just Works" pairing is refused unless the user confirms it.

### Testing

`esp-emu` has a `--ble-hci` option. **Find out in Phase 6f what it bridges.** If it can
reach the host's BlueZ adapter, a real keyboard (or a software HOGP peripheral on the
host) can drive the emulated device, and pairing, typing, reconnection and forgetting
become part of the gate. If not, this phase is tested on hardware only.

### Phase 10b — Display console on ESP32-S3 CYD boards

The same design as Phase 10 (Vim thinks it's talking to an xterm; libvterm keeps the cell
grid; a renderer paints damaged cells), on a different panel:

- **Panel:** S3 CYDs use SPI panels (e.g. ILI9341/ST7796-class) or 16-bit parallel RGB
  panels (e.g. 800×480), driven through `esp_lcd`. The renderer's panel access sits
  behind a small per-board interface, so Tab5 DSI and CYD SPI/RGB share everything above it.
- **Geometry:** far smaller screens. At 8×16, 800×480 gives 100×30 cells and 480×320
  gives 60×20. A smaller font (6×12) is worth offering; `'columns'` below 80 needs
  checking against the `:Esp` views and `:EspFiles` (which already drops its date column
  under 50 columns per pane).
- **RGB panels and PSRAM bandwidth:** a parallel RGB panel streams its framebuffer from
  PSRAM continuously, and on the S3 that competes with Vim's heap for the same bus. Measure
  redraw and typing latency, and consider bounce buffers (`esp_lcd` RGB supports them).
- **Touch:** resistive (XPT2046) on some variants, capacitive (GT911 and similar) on
  others. Both feed the touch-as-mouse path and the Phase 11 pairing overlay.
- **Input:** no built-in keyboard, so **BLE keyboards (Phase 11) are the main input**,
  paired by touch. USB host depends on the board.
- **Board definitions:** one per supported CYD variant (panel, pins, touch, backlight).
  Start with one widely available N16R8 variant and add others as they're tested.

#### First CYD board: Hosyond ES3C28P (2.8", 240×320, capacitive)

Sold as "Hosyond ESP32-S3 2.8" 240x320 IPS Touchscreen"; the board is the **ES3C28P**
(LCD Wiki: "2.8inch ESP32-S3 Display"; ES3N28P is the same board without touch). Details
below come from the user's hands-on notes for this exact board, verified on it:

| | |
|---|---|
| Module | ESP32-S3-WROOM-1 **N16R8**: 16 MB flash (quad), 8 MB PSRAM (**octal**, `CONFIG_SPIRAM_MODE_OCT`, as already set) |
| USB | the S3's **USB-Serial/JTAG only**; no UART bridge |
| Panel | **ILI9341V**, 240×320 IPS, SPI: SCLK 12, MOSI 11, MISO 13, CS 10, DC 46, backlight 45; **no reset GPIO** (tied to chip reset: software reset only); **colour inversion required**; landscape is 320×240 |
| SPI clock | **60 MHz** ceiling (80 MHz drops bytes, and the whole image shifts sideways); a full frame is about 20 ms; use SPI3 |
| Touch | **FT6336G** capacitive, I2C SDA 16 / SCL 15, INT 17, RST 18, address **0x38** (the audio codec also answers, at 0x18: bind 0x38 explicitly); reports portrait coordinates, landscape mapping `x = raw_y, y = 240 - raw_x`; hardware reset before the first read |
| SD card | SDMMC 4-bit: CLK 38, CMD 40, D0–D3 39/41/48/47, which becomes `/sd` |
| Other | ES8311 codec + FM8002E amplifier (I2S 4/5/7/8/6, amplifier enable GPIO1 active LOW), battery ADC GPIO9, BOOT button GPIO0, UART0 43/44 |

What it changes in the plan:

- **Console over USB-Serial/JTAG.** The port's console is UART0, which this board only
  exposes on pins. Add a Kconfig choice for the console transport (UART or USB-Serial/JTAG;
  ESP-IDF has a VFS driver with `select()` support for the latter). Then check the
  port's assumptions against it: raw mode, line endings, the zero-timeout poll, the size
  probe, and CTRL-C. The Phase 5 note about USB-JTAG panic output hanging the console
  (hence `CONSOLE_SECONDARY_NONE`) needs re-checking in that configuration.
- **Do this before 10b, as the first real-hardware run.** Vim over USB-Serial/JTAG on this
  board needs no display work, and it **settles the open S3 heap corruption**
  (PHASE5.md, known issue): run the S3 gate on real silicon. If it passes there
  repeatedly, the emulator is the suspect; if it fails, the bug is ours and now
  debuggable on hardware.
- **A tiny grid.** 320×240 is 40×15 cells at 8×16, 53×20 at 6×12, 64×30 at 5×8.
  Which font, and whether Vim is usable at that size, **is a decision for when 10b starts**.
  Every `:Esp` view and `:EspFiles` must work at 40–60 columns.
- **Input:** no built-in keyboard, and the USB-C port is the debug console, so **BLE
  keyboards paired by touch (Phase 11) are the input**, plus serial for development.
- **Extras worth taking:** the speaker for Vim's bell; the battery ADC for `:EspInfo` and
  the status line; `/sd`.
- **Flashing safety:**
  - The board ships with a single `app0` partition and no OTA slot, so the factory
    firmware has no other copy. **Dump the full 16 MB flash before the first write**
    (`esptool read-flash 0x0 0x1000000 …`, with `--after no-reset`: the board
    re-enumerates on reset) and keep the dump outside the repo.
  - Always address the board by its `/dev/serial/by-id/…` path, never `/dev/ttyACM*`,
    whose numbering changes between replugs. Other Espressif boards with the same USB
    VID:PID may be attached.

---

## Verification

| Gate | How |
|---|---|
| Phase 0 | Clone fresh into an empty directory, `git lfs pull`, run `prepare-deps.sh` — **with the network disconnected** — and get a complete `build-deps/`; every sha256 verified, every patch applied clean, and a second consecutive run produces an identical tree |
| Phase 1 | Spike prints all six capability results; `isatty(0)` true, `/vimrt` readable |
| Phase 2 | Component links; report `.text`/`.data` from `idf.py size-components` |
| Phase 3 | No undefined symbols; every stub logs on first call so dead paths are visible |
| Phase 4 | Runtime image ≤ 3.5 MB; `stat` on a runtime file succeeds from firmware |
| Phase 5 | **CI:** scripted `--inject`/`--save-state`/`--exit-on` edit-save-verify round trip, both P4 and S3 |
| Phase 5 | **Manual:** `socat` raw session — cursor keys, insert, `:w`, `:q`, syntax colors |
| Phase 6 | Single-process, EMAC + `--net user`: `esp_http_get` with cert-bundle TLS, SCP to a host `sshd`, `:set spell` downloads `en.utf-8.spl` to `/fat/spell`, `:EspSerial` round trip against `--uart1-tcp` |
| Phase 6 | Only then the two-emulator `--hosted` fixture (P4 + C6 slave image): `:EspWifiScan` returns a list, `:EspBleScan` runs; `:EspGpio`/`:EspHeap`/`:EspTasks` render |
| Phase 6 | `:EspFiles` — navigate, tag, copy local→local and local→`scp://`, delete; exercised **twice**, once via F-keys and once via the letter aliases |
| Phase 6 | `esp_api_fs.c` path validation: a traversal suite (`../`, absolute escapes, symlink-ish paths, writes into read-only `/vimrt`) must be rejected from *both* front ends |
| Phase 6 | Web manager: refuses to start with no password; TLS up with the fingerprint printed; login, browse, upload, download, rename, delete, mkdir; CSRF token required on mutations; failed-login backoff observed |
| Phase 6 | Web settings panel round trip: change an editor option in the browser, observe Vim apply it via the queue; SSE status stream shows live line and word counts as you type, and `wordcount()` values match `g CTRL-G` |
| Phase 7 | `:EspPyRun` on a buffer with output and a traceback in quickfix; `vim` module edits a buffer; `esp` module toggles a GPIO; `CTRL-C` interrupts an infinite loop without killing Vim; heap returns to baseline after `:EspPyReset` |
| Phase 8 | Stage 1: `:EspGitInit`/`add`/`commit`/`log`/`diff`, then **`git fsck` and `git log` on the host** against the copied-off repo — the real-git-compatibility check is the gate |
| Phase 8 | Stage 2: push to a bare repo on the host over both HTTPS and SSH; host-side `git log` shows the commits |
| Phase 8 | Stage 3: clone and verify checked-out blobs byte-for-byte. **Both delta forms must be exercised deliberately** — real servers send `ofs-delta` almost exclusively once the client advertises it, so `ref-delta` is only reached by omitting that capability from our advertisement (or by constructing such a packfile by hand). Test both paths explicitly or the gate silently covers one |
| Phase 8 | `:EspGitGc` packs loose objects; repo still passes host-side `git fsck` afterwards |
| Phase 9 | **Re-test the Vim task unpinned** (drop `xTaskCreatePinnedToCore(..., 0)`). Under the emulator, running Vim on core 1 crashed `esp_vfs_select` with a NULL-spinlock assert; the cause (IDF cross-core select vs the emulator's multi-hart model) is unresolved — see PHASE3.md |
| Phase 9 | **Re-run the Phase 1 spike on real silicon** (`pixi run spike-build` + flash) and diff against `docs/phase1-spike-results.txt` — the emulator's answers are assumptions until confirmed on a real UART and real flash |
| Phase 9 | Scripted round trip on Tab5 over real UART; heap, redraw timing and C6 latency recorded |
| Phase 9 | **Transports re-tested over the production path** — SCP, SFTP, git push and the web manager over the C6 link on real hardware, not EMAC, including the co-processor-unresponsive error path |
| Phase 9 | `:EspUsbMsc on` with a host PC — `/fat` mounts as a drive, refuses while a buffer is modified or the web server runs, and the volume is intact after remount (the concurrency hazard is the thing being tested) |
| Phase 10 | USB keyboard drives an interactive session on the panel with no UART attached; F-keys reach the file manager |
| Phase 10 | Tab5 keyboard: `Esc` reaches Vim, `Ctrl-W`/`Ctrl-R`/`Ctrl-[` work, `Sym`+digit produces F-keys, and the file manager is fully drivable from it |
| Phase 11 | **With nothing else attached** (no Tab5 keyboard, no USB keyboard, no serial): the overlay appears at boot, a passkey keyboard and a Just Works keyboard each pair by touch, the overlay closes when the keyboard connects, and reappears ~15 s after it is switched off. Also: pair a BLE keyboard (passkey), type into Vim with no other input attached, reboot and confirm it reconnects on a keypress, `:EspBtKeyboard forget` stops its input; an unbonded device's input is refused; keystroke latency measured with and without a WiFi transfer (Tab5) |

The Phase 5 scripted round trip is the regression test for everything after it, and the
thing to re-run after each upstream Vim re-sync.

**Keeping the plan honest.** `docs/PLAN.md` is a living document, not a record of what we
once intended. As each phase lands: tick it off, replace provisional numbers with measured
ones (the `size-components` output and the final partition table especially), and amend
decisions in place when reality disagrees — with the reasoning in `docs/DECISIONS.md`. A
plan that quietly diverges from the build is worse than no plan.

## Known limitations to state up front

- No `:!command`, no `:terminal`, no jobs/channels — no fork on ESP-IDF, ever. The `:Esp*`
  commands exist precisely because shelling out is impossible.
- No `:help` (the 12 MB `doc/` tree does not fit).
- Spell checking requires a one-time download, which rides on the *stretch* HTTPS
  transport — if HTTP/HTTPS slips, spell slips with it. Offline first-boot has no spell data.
- netrw's ftp/rcp/rsync methods are removed; scp/sftp are native, http/https are stretch.
- USB mass storage and on-device filesystem access are **mutually exclusive** — FAT cannot
  be mounted read-write by two initiators, so `/fat` is unmounted from Vim while MSC is live.
- The web file manager uses a self-signed certificate; browsers will warn, and trust is
  established on first use by checking the printed fingerprint.
- The Tab5 keyboard accessory has no F-key row; F1–F12 exist only as a `Sym` layer.
- Git is pure Python on MicroPython: correct and real-git-compatible, but performance scales
  with repo size — fine for a working set of files, slow on a large tree. Merging is
  fast-forward only.
- MicroPython runs synchronously on the Vim task, so a long script blocks the editor until
  it finishes or is interrupted with `CTRL-C`.
- Flash, not RAM, is the limiting resource; see the Phase 4 levers if the app partition
  overflows.
- No clipboard beyond Vim's internal registers.
- FATFS `st_ino`/`st_dev` are 0, so same-file detection is weaker than on Unix.
- `:make` cannot run; `:vimgrep` is the working half of quickfix.
- On P4, every WiFi/BLE command depends on the C6 co-processor being alive.
- Bluetooth keyboards must support **BLE** (HID over GATT). Neither chip has Bluetooth
  Classic, so Classic-only keyboards can't pair.
