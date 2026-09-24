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
