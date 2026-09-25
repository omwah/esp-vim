# Phase 6 — `:Esp*` commands, file manager, transports

Delivered in stages; see the table in [PLAN.md](PLAN.md), Phase 6.

## 6a — Builtins and the device commands (2026-09-24)

**Result:** six commands that show and change the device from inside Vim, backed by
twelve `esp_*()` builtins that scripts can call directly. P4 gate: all checks pass,
including eight new ones.

| Command | Builtins | What it does |
|---|---|---|
| `:EspInfo` | `esp_info()` | chip, revision, cores/MHz, flash, PSRAM, MAC, versions, uptime, reset reason |
| `:EspHeap` | `esp_heap()` | internal / PSRAM / DMA heap: free, largest block, lowest, total; Vim's own use, peak, budget |
| `:EspTasks` | `esp_tasks()` | FreeRTOS tasks: state, priority, core, least stack free |
| `:EspGpio {pin} [action]` | `esp_gpio_pins/read/write/mode()` | read, `on`/`off`/`toggle`, `in`/`pullup`/`pulldown`/`out`/`od`/`reset` |
| `:EspNvs[!] [ns [key [value]]]` | `esp_nvs_list/get/set/erase()` | browse and edit NVS; numbers stored as i64, else strings |
| `:EspReboot[!]` | `esp_reboot()` | real reset; refuses with unsaved buffers unless `!` |

Views open in a bottom window (`R` refresh, `q` close). `<Tab>` completes pins, actions,
namespaces and keys. `:help esp-commands` documents them all.

### How it is built

- **C in `components/vim/api/`**, `esp_api_{sys,nvs,gpio}.c`. The plan said a separate
  `esp_vim_api` component. These files need Vim's private headers (`typval_T`, `dict_add_*`)
  and must allocate through Vim's heap, so they belong to the Vim component. Prototypes
  are in `port/esp_vim_api.h`.
- **Patch `0008-evalfunc-esp-builtins`** adds the twelve rows in one block between
  `escape` and `eval`, plus the include. **`scripts/check-builtins.py` runs on every
  build** and fails if the table is out of `strcmp` order. A misplaced row compiles fine
  and silently breaks lookup of *unrelated* builtins.
- **Vim script**: `plugin/esp.vim` only defines commands (cheap at startup);
  `autoload/esp.vim` does the work on first use. All formatting is there, so it changes
  without a reflash.
- The builtins return **data, not text**: Dicts, Lists, Numbers.

### Decisions and findings

- **GPIO safety.** A pin is refused unless it's a valid GPIO, isn't a console UART pin,
  and isn't claimed in ESP-IDF's `esp_gpio_reserve` mask, which the flash, PSRAM and UART
  drivers fill. Reconfiguring a flash or PSRAM line would hang the chip. A pin the session
  hasn't configured is first set up with `gpio_config()` (input to read, input+output to
  write, so it reads back), because it may not be routed to the GPIO matrix yet.
  `esp_gpio_read()` returns **-1** with an error for a refused pin: a builtin that errors
  still returns a value, and `0` would look like a real low level.
- **GPIO works in the emulator**: pin 54 drives high and low and reads back on the P4.
- **NVS** was not initialised by `app_main` before; it is now. A full or newer-format
  partition is erased and re-initialised, ESP-IDF's documented recovery. Changing a key's
  type (string to number) erases it first, since NVS refuses a set that changes type.
- **`sdkconfig.defaults` edits were silently ignored.** `build-<target>/sdkconfig`,
  once generated, wins over the defaults, so the new `CONFIG_FREERTOS_USE_TRACE_FACILITY`
  never applied. `scripts/vim-build.sh` now regenerates it whenever a defaults file is
  newer. Nothing hand-made lives in the generated file.
- `TaskStatus_t.xCoreID` isn't present in this configuration; `xTaskGetCoreID()` is used.

### Measured (P4, emulator)

- Vim task: 34.5 KB of its 64 KB stack free at its lowest, after the full interactive gate.
- Tasks at idle: 7 (`ipc0/1`, `esp_timer`, `vim`, `main` supervisor, `IDLE0/1`).
- Runtime image: 85% of `vimrt` (was 84%).

### Tests (`esp-vim/test/interactive.py`)

`esp_info()` names the chip; `:EspInfo` opens its view and `q` closes it; `:EspHeap` shows
PSRAM and Vim's own use; `:EspTasks` lists the `vim` task; `:EspNvs` stores a number and
a string and `:EspNvs!` erases; `:EspGpio` drives a free pin high and low and reads it
back; the console TX pin is refused.

## 6b — The file-operations core and `:EspFiles` (2026-09-24)

**Result:** a two-pane, Midnight-Commander-style file manager, over one validated
file-operations core that the web interface (6e) will share. P4 gate: all checks pass,
including eleven new ones.

### `components/esp_fs`: the one core

Copy, move, delete, mkdir, list and free space, used by every front end that changes
files. It's a **separate component, not part of the Vim one**: the web server will call it
from its own task, so it must not use Vim's allocator or globals. Being outside
`linker.lf`'s brackets, its mutex also survives a Vim session restart.

Path validation happens here and nowhere else:

- Paths must be absolute, and are normalised lexically (`//`, `.`, `..` folded).
  FAT has no symlinks, so the lexical result is the real location. Control characters
  are refused.
- The result must lie inside a **mounted** root: `/fat` and `/sd` (read-write) or
  `/vimrt` (read-only). `/` exists only as a listing of those roots.
- A root itself can be listed but never deleted, moved, copied or overwritten.
- A directory can't be copied or moved into itself.

Operations hold a recursive mutex, keep their scratch memory (paths plus a 4 KB copy
buffer) on the heap per call, and never leave a truncated copy behind. Moves within a
root are renames. FAT won't rename onto an existing name, so an allowed overwrite
removes the target first, and only for files. Moves across roots are copy-then-delete.
Recursion is capped at 24 levels.

**Progress callback.** Long copies and deletes call back after every chunk. The Vim
builtins pass one that runs Vim's own break check, so **CTRL-C stops a big copy and the
busy spinner turns during it**, with nothing Vim-specific in the core.

### `esp_fs_*()` builtins

`esp_fs_list()`, `esp_fs_copy()`, `esp_fs_move()`, `esp_fs_delete()`, `esp_fs_mkdir()`,
`esp_fs_info()`, `esp_fs_roots()`, added to patch 0008 (19 rows now, still one block).
Relative paths are made absolute against Vim's working directory before the core sees
them.

### `:EspFiles`

`autoload/espfiles.vim`: two panes in a tab page of their own. Every MC function key has
a letter alias (`F3`/`v`, `F4`/`e`, `F5`/`c`, `F6`/`r`, `F7`/`m`, `F8`/`d`, `F10`/`q`),
because the Tab5 keyboard has no F-keys. Tagging (`Space`/`t`), a rename when moving
within one directory, overwrite prompts with "All", the free space for each pane in its
status line, and double-click to open. Edit and view open a new tab, so `:q` returns to
the manager.

The plan put the manager in `pack/esp/start/espfiles/`. It's an autoload file instead, so
nothing is parsed at startup until `:EspFiles` is first used, which matters on this
hardware.

### Tests

The core refuses five attacks through the builtins: writing into `/vimrt`, `..` out of
`/fat`, deleting a root, listing outside the roots, and copying a directory into itself.
`/vimrt/vimrc` is intact afterwards. The manager is driven entirely by keystrokes:
`F5` and `c` copy, the overwrite prompt appears and is honoured, `r` renames, `F7`
makes a directory, `d` deletes one after asking, `F3` views read-only, `Space`/`t` tag,
`F8` deletes the tagged entries, and `F10` closes.

## Screenshots, and a colour bug they found (2026-09-24)

`pixi run screenshots` regenerates the README images from the real firmware. It boots it
in the emulator, drives it over the UART, feeds every byte the device sends into a
terminal emulator (`pyte`), and renders that screen as SVG. Nothing is mocked. `pyte`
chokes on Vim's private SGR sequences (the modifyOtherKeys handshake), which change
nothing on screen, so the script ignores them.

The first images showed `~` lines and directory names in **red** where Vim's light
colour scheme has blue. The system vimrc set `t_Co=256` before `t_AF`, and setting
`t_Co` makes Vim recompute its default highlight colours at once. It chooses xterm colour
numbering only if `t_AF` already ends in `m`; otherwise it uses PC numbering, where 1 is
blue. So every default group (`NonText`, `Directory`, ...) was wrong on the device from
the start. Syntax colours load later and were right, which is why nobody noticed. The
vimrc now sets `t_AF`/`t_AB` first, and the gate checks `NonText` is 12 and `Directory` is 4.

Two checks added alongside: the README's Vim-script example is read out of `README.md`,
written to the device and sourced, so the published example can't silently break; and
`:diffthis` is exercised, since the README lists diff among the features. The emulator
time cap in `uart_session.py` went from 360 s to 600 s, because the interactive gate now
takes about six minutes.

## 6c — Network, HTTP(S) and spell download (2026-09-24)

**Result:** the P4 build has a network in the emulator. `:EspNet` shows it, `:EspGet` and
`esp_http_get()` download over http and https (certificates checked), `:e https://…`
opens a page through netrw, and `:set spell` downloads its dictionary on first use. P4
gate: all checks pass, including six new ones.

### Networking in the emulator: the P4's Ethernet

A throwaway test app answered the plan's open question. `esp-emu` models the P4's EMAC
(`dw_gmac`), and with `--net user` ESP-IDF's EMAC driver plus the **generic 802.3 PHY**
driver gets a DHCP lease (192.168.4.2, gateway 192.168.4.1), resolves DNS, and reaches the
internet. The host's `127.0.0.1` is reachable from the device **as the gateway**, so tests
run their own HTTP server with no internet at all.

So networking arrives on the P4 without the C6 or the two-emulator setup. That's
**emulator scaffolding**: the Tab5 has no Ethernet, and its network is WiFi through the
C6 (6f). The interface is a Kconfig choice (`ESP_VIM_NET`: Ethernet by default where the
chip has an EMAC, else none), so a board without a PHY just never gets a link.

### `components/esp_net`

Interface bring-up (non-blocking; DHCP continues in the background) and an HTTP(S)
client. It's outside the Vim component for the same reasons as `esp_fs`: it outlives Vim
sessions, and the web server (6e) will use it. The client:
- streams with `esp_http_client`, `open`/`read`;
- follows up to 5 redirects;
- treats anything but a final 200 as an error;
- checks certificates against ESP-IDF's CA bundle;
- takes the same progress callback as `esp_fs`, so **CTRL-C and the spinner work during
  a download**.

Downloads validate the destination with `esp_fs_check` and write `{dest}.part`, replacing
the target only when complete: a failed or stopped download leaves nothing behind.
mbedTLS allocates from PSRAM (`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC`); TLS needs tens of KB
per connection, and the S3 has only about 100 KB of internal RAM free.

### Vim side

- `esp_net_status()` and `esp_http_get(url [, file])`, in patch 0008 (21 builtins,
  still one block). `:EspNet` shows the interface; `:EspGet[!] {url} [{file}]` downloads,
  refusing to overwrite without `!`.
- **Patch `0009-netrw-native-http`**: netrw's http method shells out to wget or curl. When
  `esp_http_get()` exists it fetches natively instead: one hunk, every other platform
  unchanged. That also fixes **spell download**, since `spellfile.vim` fetches through
  `:Nread`. The dictionary lands in `/fat/.vim/spell/`.
- **`TMPDIR=/fat/.tmp`**, emptied at every boot. Vim tries `$TMPDIR`, `/tmp`, `.` and
  `$HOME` for temp files; there's no `/tmp`, and `.` may be the read-only `/vimrt`. netrw
  downloads into a temp file. A session that ends by restarting never runs Vim's own
  temp-directory cleanup, hence the boot-time clean.

### Tests

The harness now starts every emulator with `--net user` and serves a temp directory over
HTTP on a free port. New checks:
- the network comes up with an address;
- `:EspGet` downloads a file;
- a 404 is an error and leaves no file or `.part`;
- `:e http://…` opens a page through netrw;
- spell download: `g:spellfile_URL` points at the local server, which serves a
  dictionary built by the host's Vim (`mkspell`) from four words; the test answers
  `spellfile.vim`'s prompts, then checks `spellbadword('helo world')`;
- an **https** request to a real site, skipped when the host has no internet.

The spell check is skipped if the host has no `vim`.

## 6d — SCP and SFTP (2026-09-24)

**Result:** files on other machines work like local ones:
- `:e scp://me@host/file` and `:w` read and write them through netrw;
- `:EspFiles` panes can be remote (`sftp://me@host/…`), with copy and move between local
  and remote panes (directories included), mkdir, delete and rename;
- the device makes its own key with `:EspSshKeygen`.

P4 gate: all checks pass, against a **real OpenSSH server** the harness starts on the host.

### `components/esp_ssh`

libssh2 1.11 through the registry component **`skuodi/libssh2_esp` 1.1.0** (BSD-3-Clause,
mbedTLS backend), pinned in `components/esp_ssh/idf_component.yml`, with
`dependencies.lock` committed. It's unpatched, so per Phase 0's rule it isn't vendored.

- **URLs follow netrw's convention**: `scp://[user@]host[:port]/rel` is relative to
  the login directory, `//abs` is absolute. The same text works in `:e`, `:EspFiles` and
  the builtins. The scheme picks the protocol for get/put; list, mkdir, remove and
  rename always use SFTP, which SCP can't do.
- **One cached session** per user@host:port, reused across calls. Without it, every
  file-manager refresh would pay a full handshake.
- **Host keys:** trust on first use, recorded in `/fat/.ssh/known_hosts` (OpenSSH format).
  An unknown host is an error carrying the key type and SHA256 fingerprint, formatted as
  `ssh-keygen -l` prints it, until `esp_ssh_trust()`. A **changed** key is refused
  outright and never accepted automatically.
- **Auth:** `/fat/.ssh/id_ecdsa`, then `id_rsa`, then a password if one is given. The
  public half is passed explicitly: libssh2's mbedTLS backend can't derive it from an EC
  private key.
- **No Ed25519** in libssh2's mbedTLS backend, so **`esp_ssh_keygen()` makes ECDSA
  P-256**: mbedTLS generates the key and writes it as PEM (`CONFIG_MBEDTLS_PEM_WRITE_C`),
  and we build the OpenSSH public-key line (`ecdsa-sha2-nistp256`) ourselves.
- Downloads write `.part` and rename; transfers take the progress callback (CTRL-C,
  spinner); a failed SCP transfer drops the session rather than reusing a broken channel.

### Vim side

- Nine `esp_ssh_*()` builtins (patch 0008, now 30 rows). Their errors say what happened,
  so **`autoload/esp/ssh.vim`** can hold the conversation: trust an unknown host (showing
  its fingerprint), or ask for a password and retry. A typed password is kept only in a
  script variable for the rest of that Vim session.
- **Patch `0010-netrw-native-scp-sftp`**: in front of netrw's scp and sftp read and write
  methods, which run `scp`/`sftp` commands, a branch that calls `esp#ssh#Get`/`Put`.
  netrw can't list remote *directories* this way (it runs `ssh … ls`); `:EspFiles` is the
  remote browser, as the help says.
- **`:EspFiles` remote panes.** Local↔remote copy and move recurse into directories; a
  move within one host is a rename there. Remote-to-remote copies are refused.
- **`scripts/mkpatch.sh` takes paths** to limit a new patch to them. Needed because each
  stage adds rows to 0008 *and* a new netrw patch at once.

### Bugs the tests found

- **`:EspFiles` could not open a second time** while a manager tab still existed (E95,
  duplicate buffer name). It now goes to the existing manager, repointing its panes if
  directories are given. Pane names are also made unique.
- **A needless "Press ENTER" after every confirmation** (delete, overwrite): the
  follow-up message didn't fit under the dialog. Now redrawn first. Found because the
  prompt swallowed the test's next key.
- **An error during exit froze the device.** When an exit autocommand raised an error,
  Vim's `getout()` waited for Enter "to give the user a chance to read the message". It
  did so after restoring the terminal, so no prompt was ever drawn and the device looked
  hung until a key was pressed. **Patch `0011-main-no-exit-prompt`** skips that wait on
  ESP. Vim is restarting anyway, so the message is lost. Tested with a `VimLeavePre`
  autocommand that fails.
- **Open: a timed `search()` occasionally overran its timeout** after a long first
  session, until input arrived. GDB caught Vim in the backtracking engine
  (`regmatch`), searching a buffer with the pathological test pattern. It isn't the
  emulator failing to deliver timer interrupts: a standalone app shows a 50 ms
  `esp_timer` interrupting a pure-CPU loop on time, every time. 150 timed searches in a
  row, before and after network use and across a session restart, also each took
  exactly 50 ms. It only appeared in the full suite, and only when the test typed its
  next command while the search was still running. The test now waits for the command to
  finish, which is what it meant to do anyway. The underlying cause isn't identified;
  suspects are a leaked nesting level in Vim's `init_regexp_timeout()`, or the terminal
  briefly leaving raw mode so no break checks run. Tracked here until explained.
- In the harness: a value wider than the terminal comes back through the UART with
  cursor-movement escapes in it (Vim wraps it). The SSH public key was, so
  `authorized_keys` got garbage. Long values are now read in pieces (`probe_long`).

### Tests

The harness starts an unprivileged `sshd` from pixi's OpenSSH on a free port, with an
ECDSA host key, key-only auth, and internal SFTP. The client-side tools get `-F
/dev/null`, since pixi's `ssh` rejects a user's `~/.ssh/config` with unusual permissions.
New checks:
- `:EspSshKeygen` makes a key, and the host installs it;
- an unknown host reports the **same fingerprint `ssh-keygen -l` gives on the host**;
- trust is remembered;
- SFTP and SCP downloads work;
- `:e scp://…` reads and `:w` writes back;
- an `:EspFiles` remote pane lists, uploads, makes a directory and deletes.

The emulator time cap is now 1500 s; the full interactive gate takes about 7 minutes.
