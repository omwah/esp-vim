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

## 6e — The web interface (2026-09-24)

**Result:** `:EspWebStart` serves a password-protected HTTPS page. From it a browser can:
- browse, download, upload (including drag and drop), make folders, rename and delete;
- change editor settings, which Vim applies within a second and again at every start;
- watch what's being edited: file, cursor, line/word/character counts, modified, mode.

`:EspWebStop`, `:EspWebStatus` and `:EspWebPasswd` complete it. The new gate script
`esp-vim/test/web.py` drives it from the host over real HTTPS: all 23 checks pass.

### `components/esp_web`

ESP-IDF's `esp_https_server`, with the plan's security model implemented as written:

| Requirement | How |
|---|---|
| HTTPS only | self-signed **ECDSA P-256** certificate made on the device at first start (mbedTLS X.509 writer), kept in NVS; its SHA-256 fingerprint is shown by `:EspWebStart` |
| no default password | refuses to start until `:EspWebPasswd` sets one (8+ characters) |
| password storage | **PBKDF2-HMAC-SHA256**, 20,000 iterations, random 16-byte salt, compared in constant time; setting it logs everyone out |
| sessions | random 256-bit id in a cookie: `HttpOnly; Secure; SameSite=Strict`; at most 4; an hour's idle ends one |
| CSRF | every state-changing request needs the session's token in `X-CSRF-Token` |
| brute force | after each failure, logins are refused for 2^n seconds (at most 64) |
| file access | only through `esp_fs`, which gained validated streaming calls: `esp_fs_open_read`, and `esp_fs_create_part`/`esp_fs_finish_part`, so an upload writes `.part` and replaces the target only when complete |
| page | `index.html` plus `app.js`, embedded; `Content-Security-Policy` allows scripts only from the device, plus `nosniff`, `X-Frame-Options: DENY`, `no-store` |

**Threading, as the plan required:** the server never touches Vim. Two things cross over,
under one mutex:
- **the status snapshot**, written by the Vim task (`esp_web_publish`) and read by the
  server;
- **settings**, which the server validates against an allow-list (tabstop, shiftwidth,
  expandtab, number, relativenumber, wrap, background, and a shipped colorscheme), writes
  to NVS, and flags. The Vim task takes the flag and applies them itself.

The Vim side is a **once-a-second Vim timer**, started by `:EspWebStart`. It runs on
Vim's main loop, so applying settings is as safe as a user typing `:set`. It republishes
the status only when buffer, changedtick, cursor or mode changed, so `wordcount()`
isn't recomputed for nothing. Stored settings are applied again at every start.

**Deviation from the plan: status is polled once a second, not pushed with Server-Sent
Events.** `esp_http_server` handles requests on one task, and a held-open event stream
would block every other request. Polling a small JSON endpoint is simple, and at this
rate it costs nothing.

### Networking in the emulator: port forwarding

The browser, or the test, runs on the host, so it must reach *into* the emulated device.
`esp-emu`'s user networking supports QEMU-style forwarding although `--help` doesn't
mention it: `--net user,hostfwd=tcp:127.0.0.1:HOST-:443`. `uart_session.Session` takes
`hostfwd=[(host_port, device_port)]`.

### Tests (`esp-vim/test/web.py`, part of `pixi run vim-test`)

- no password means no server;
- a short password is refused;
- the certificate the host receives has **exactly the fingerprint Vim printed**;
- the page is served;
- the API refuses a browser that hasn't logged in;
- a wrong password gets 401, and an immediate retry gets 429;
- the right password gets a cookie and a CSRF token;
- listing works;
- a change without the CSRF token gets 403;
- upload works, and replacing a file needs `overwrite=1`;
- download works;
- mkdir, move and recursive delete work;
- a `..` path out to `/vimrt` is refused, and the target file is intact;
- an out-of-range setting is refused;
- settings saved in the browser show up in Vim's `&tabstop`/`&number`;
- the live status reports file, lines, words and cursor;
- logout ends the session;
- `:EspWebStop` closes the port;
- no Vim errors in the whole session.

## 6f, part 1 — Serial, I2C, ADC, sensors (2026-09-24)

**Result:** the chip's peripherals from Vim. `esp-vim/test/hw.py` (part of
`pixi run vim-test`) passes; the serial port is tested for real against a host socket.

| Command | Builtins | Notes |
|---|---|---|
| `:EspSerial {port} {baud} [{tx} {rx}]` | `esp_serial_open/read/write/close()` | a window showing what arrives, live (polled ten times a second); `s` sends a line, `q` closes; UART0, the console, is refused |
| `:EspSerialSend {text}` | | appends `g:esp_serial_eol` (`\r\n`) |
| `:EspI2cScan [{sda} {scl}]` | `esp_i2c_scan()` | a bus built for the call on any free controller, probing 0x08–0x77 |
| `:EspSensors [{sda} {scl}]` | `esp_sensors()` | names what it recognises, by address **and** ID register where the chip has one (BMI270, BME/BMP280, FT6336, GT911, ES8311, PI4IOE5V6408, the Tab5 keyboard, ...) |
| `:EspAdc {pin}` | `esp_adc_read()` | raw value, plus millivolts by ESP-IDF's curve-fitting calibration |

- Default pins come from a new menuconfig menu, **"esp-vim board"** (`main/Kconfig.projbuild`):
  - serial: GPIO22/23 on the P4, 17/18 on the S3;
  - I2C: GPIO31/32 on the P4 (the Tab5's internal bus), 8/9 on the S3.

  Every pin passes the same check as `:EspGpio`.
- **Found: the emulator build's Ethernet collides with the Tab5's I2C bus.** The P4's EMAC
  claims its RMII pins, GPIO28–31, 34, 35, 49, 50 and 52, so GPIO31 is correctly refused
  in the Ethernet-enabled emulator build. A Tab5 build must set `ESP_VIM_NET=NONE`
  (PLAN.md, Phase 9).
- `:EspSensors` identifies devices; it doesn't read them. Reading the BMI270 means
  uploading its 8 KB configuration blob first, which is Tab5 bring-up work (Phase 9).
- The harness learned that a Vim timer tick makes Vim hide and show the cursor, and that
  this alone isn't "busy". `quiet()` ignores output made only of those bytes; they can
  arrive split across reads, so it tests bytes, not whole sequences.
- `uart_session.Session` now picks a **free** UART port per session. The gate starts four
  emulators back to back, and a fixed port was sometimes still held by the previous one.

## 6f, part 2 — WiFi on the ESP32-S3 (2026-09-25)

**Result:** `:EspWifiScan`, `:EspWifiConnect {ssid} [{password}]` (asks for the password
if it isn't given; waits for an address with progress), `:EspWifiDisconnect` and
`:EspWifiStatus`, with `esp_wifi_scan()`, `esp_wifi_connect()`, `esp_wifi_disconnect()`.
`esp_net_status()` gains `ssid` and `rssi`. The network is stored in NVS and rejoined at
every boot. `esp-vim/test/wifi.py` passes on the S3 against esp-emu's soft access point:
- a scan finds it, with its security;
- connect gets an address;
- HTTP works over WiFi;
- disconnect forgets the network.

It skips on builds whose network isn't WiFi.

`esp_net` gains a WiFi interface choice, the default where the chip has a radio. The code
uses only the standard `esp_wifi_*` API, so it will serve the Tab5 unchanged through
`esp_wifi_remote` (part 3).

### Three fixes WiFi forced on the S3

The S3 has about 135 KB of internal RAM for everything. With WiFi up, boot failed in a
loop, first as "Unhandled interrupt 12", then as "cannot create the Vim task". A
standalone WiFi app, even with this project's full sdkconfig, worked fine, which pointed
at the app's own memory and ordering:

1. **Vim's task stack is reserved at link time** (`xTaskCreateStaticPinnedToCore`,
   `main/esp_vim_main.c`). Taken from the heap at the first session, 64 KB in one piece
   no longer existed once WiFi had run. The supervisor now waits until the previous
   session's task is fully deleted before reusing the stack.
2. **On the S3, Vim's `.bss` (40 KB) lives in PSRAM** (`ESP_VIM_BSS_IN_PSRAM`,
   `components/vim/linker.lf`). The static stack alone overflowed internal DRAM by 6 KB.
   ESP-IDF can put `.bss`, though not `.data`, in external RAM. The linker fragment needs
   a scheme that retargets the *standard* `bss` fragment, not a new one: only then does
   the generator also exclude `libvim.a` from the internal `.bss` catch-all. A first
   attempt left the bracket empty for exactly that reason. Only the Vim task, and the
   file wrappers from task context, touch it; never an interrupt or cache-off code.
3. **Boot order: NVS, network, console, storage.** WiFi needs NVS, and bringing the
   network up after the console UART driver left WiFi's interrupt unhandled in the
   emulator.

Plus WiFi's internal-RAM appetite is reduced on the S3:
- IRAM speed-ups off (`ESP_WIFI_IRAM_OPT`, `ESP_WIFI_RX_IRAM_OPT`, `LWIP_IRAM_OPTIMIZATION`);
- 4 static RX buffers (block-ack window 6);
- buffers from PSRAM (`SPIRAM_TRY_ALLOCATE_WIFI_LWIP`).

None of this matters at an editor's traffic levels.

## 6f, part 3 — WiFi on the Tab5, through the ESP32-C6 (2026-09-25)

**Result:** a `tab5` build variant (`pixi run vim-build-tab5`, `esp-vim/build-tab5/`):
the ESP32-P4 with `ESP_VIM_NET_WIFI_REMOTE`. `esp_wifi_remote` supplies the ordinary
`esp_wifi_*` API and esp-hosted carries it over SDIO to the C6, so `esp_net`'s WiFi code
is the S3's, unchanged; the `:EspWifi*` commands behave the same. The C6's firmware is
esp-hosted's own co-processor example, built by `scripts/c6-build.sh` (`pixi run c6-build`)
from the component registry at the version the P4 side pins. Nothing from it is committed.

**Not yet verified at run time.** The two-emulator fixture is in place
(`ESPVIM_TARGET=tab5 python esp-vim/test/wifi.py`): `uart_session.py` starts a C6
emulator with the network and soft AP, then the P4 linked to it by
`--hosted bridge:host:…`. But esp-emu (0.43.0, and 0.44.0 of 2026-09-25) never completes an
esp-hosted session. The link comes up and the P4 reads the C6's boot event. Then the
P4's first request never arrives, or arrives minutes late. Espressif's own examples fail
the same way (IDF's `wifi/getting_started/station` over esp-hosted 2.12.13, and
esp-hosted 3.0.7 and 3.0.8's `mcu_host` + `cp`), so the fault is in the emulator's bridge,
not in this build. The C6-to-P4 direction works. So WiFi through the C6 is checked on
the Tab5 itself, in Phase 9, and the fixture waits for an emulator that can run it.

What the builds settle:

- **Build variants.** `scripts/vim-build.sh` takes a variant, not just a chip: `tab5` is
  `esp32p4` plus `sdkconfig.defaults.tab5`, in its own build directory with its own
  component lock (`dependencies.lock.tab5`). `run-emu.sh --variant` and
  `ESPVIM_TARGET=tab5` select it for the emulator.
- **Only the Tab5 build carries esp-hosted.** esp_net's `idf_component.yml` adds
  `esp_hosted` and `esp_wifi_remote` behind a Kconfig rule, so the Ethernet P4 and S3
  builds don't include them. The rule leads with a target test, because on a chip with
  its own radio the Kconfig symbol doesn't exist and the component manager fails on a
  missing symbol instead of treating it as false.
- **esp-hosted 2.12.13, not 3.x.** See DECISIONS.md.
- **Pins.** The SDIO pins and the C6 reset GPIO are esp-hosted's P4 defaults
  (CLK 18, CMD 19, D0–D3 14–17, reset 54: Espressif's P4 function EV board). The Tab5's
  wiring is checked in Phase 9, along with whether the C6's factory firmware speaks this
  esp-hosted version.
