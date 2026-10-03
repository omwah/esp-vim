# Phase 9 — M5Stack Tab5 hardware

Written before the board arrived (2026-09-30): everything below builds
(`pixi run vim-build-tab5`) but none of it has run on a Tab5 yet. The last section
is the checklist for the first flash, in order, with what each step should print and
what a failure points to.

## The board, as the code sees it

Sources: Espressif's board support package `m5stack_tab5_noglib` 1.3.1 (which the
build uses), M5Stack's M5Unified and M5GFX libraries, and M5Stack's Tab5 page.

| Part | Where | Notes |
|---|---|---|
| Console | USB-C, the P4's USB Serial/JTAG (GPIO24/25) | an assumption until checked: M5Stack calls the port "USB 2.0 OTG". UART0 (GPIO37/38) only reaches the M5-Bus connector, so `:EspSerial` defaults to it |
| Internal I2C | SDA 31, SCL 32 (the BSP's port 1) | shared: expanders, touch, clock, battery monitor, IMU, audio |
| IO expander 0x43 | PI4IOE5V6408 | P0 antenna (low: internal), P1 speaker enable, P2 Grove 5 V, P4 LCD reset, P5 touch reset, P6 camera reset, P7 headphone detect |
| IO expander 0x44 | PI4IOE5V6408 | P0 C6 power, P3 USB-A 5 V, P4 power-off pulse, P5 fast charge (low: on), P6 charge status, P7 charge enable |
| Display | MIPI-DSI, 2 lanes, 720 x 1280 portrait | three revisions: ILI9881C with GT911 touch (0x14); ST7123 (from Oct 2025) and ST7121 (from Apr 2026) with touch at 0x55 |
| Backlight | GPIO22, PWM (LEDC) | |
| Touch INT | GPIO23 | not used: the touch controller is polled every 16 ms |
| ESP32-C6 | SDIO slot 1: CLK 12, CMD 13, D0-D3 11/10/9/8; reset 15, active low | esp-hosted 1.4.0 (the C6 ships with 1.4.1). Powered by expander 0x44 P0, which must be on before esp-hosted starts |
| microSD | SDMMC slot 0: CLK 43, CMD 44, D0-D3 39-42; LDO channel 4 | shares the SDMMC controller with the C6 |
| Clock chip | RX8130CE, 0x32 | |
| Battery monitor | INA226, 0x41, 5 mΩ shunt | bus voltage = the battery, NP-F550, two cells |
| IMU | BMI270, 0x68 | named by `:EspSensors`; not read |
| Audio | ES8388 (0x10), ES7210 (0x40), I2S 26-30 | not used; the speaker is switched off |
| Camera | SC2356, MIPI-CSI; clock on 36 | not used; its power is off |

## What was written for it

- **`components/esp_board`**: the board layer. `esp_board_init()` runs first in
  `app_main`. It brings up the internal I2C bus, powers the C6, the USB-A port and the
  touch controller, and switches the speaker and camera off. It also sets the expander
  pins the BSP leaves alone (internal antenna, charging on, no fast charge) and reserves
  the board's pins so `:EspGpio` can't take them. Other code shares the bus through
  `esp_board_i2c_bus()`: the display and touch, the clock chip, the battery monitor and
  `:EspI2cScan`.
- **Display** (`ESP_VIM_DISP_DSI`). `esp_board_panel_new()` works out the revision
  from the touch controller, the way the BSP does: firmware 1 at 0x55 is an ST7121, 3
  is an ST7123, a GT911 at 0x14 is the ILI9881C. The BSP then brings the panel up; the
  ST7121 gets its slower lane rate, 965 Mbit/s. `esp_display` draws cells straight into
  the frame buffer, turned 270° by default (`ESP_VIM_DISP_ROTATION`; 90° was upside down on the first board), and writes the
  rows it touched back from the cache after each burst. Fonts: Terminus 12x24 (106x30
  cells) by default, 10x20, 16x32 and Spleen 8x16 with `:EspFont`.
- **Touch** (`ESP_VIM_TOUCH_BOARD`): the BSP's driver for whichever controller is
  fitted, polled every 16 ms. Points are turned back with the same function the
  display turns the picture with (`esp_board_rotate.h`; host test
  `pixi run rotate-test`), so the two can't disagree, upside down included.
- **WiFi**: esp-hosted's SDIO pins, reset pin and polarity set for the Tab5. At boot a
  background task starts WiFi, so waiting for the C6 doesn't hold up the console. It
  prints `ESPVIM-NET wifi through the co-processor: <result> (<ms>)`.
- **microSD**: slot 0, powered from LDO channel 4, 40 MHz. If four lines don't
  answer, it retries on one.
- **Clock**: an RX8130 driver in `esp_time`. The supply-failed flag marks the time as
  lost; setting the time clears it and turns on the backup battery's charging.
- **Battery**: the INA226's bus voltage in `esp_power`, with the charge estimate for
  two cells. Below 6.9 V on battery with nothing unsaved, the Tab5 goes into deep
  sleep. USB power is expander 0x44 P6, or the battery not discharging.
- **`:EspInfo`** shows the board and its display controller.
- **The C6's firmware** (`:EspC6`, `esp_net_cp.c`). `:EspC6` shows esp-hosted's
  version on each side. `:EspC6 update {file}` sends the C6 a new app image over the
  link (esp-hosted's own update: `esp_hosted_slave_ota_begin`/`write`/`end`, then
  `activate`), then restarts the Tab5. The image is `pixi run c6-build`'s
  `network_adapter.bin`, copied to /sd or /fat, not the merged image. It checks
  the image first: an ESP32-C6 app with an app description. The C6 writes it to its
  other slot and checks it before it boots it, so a stopped or failed transfer
  leaves it on the firmware it had. This is esp-hosted 2.x's update: on 1.x, which
  the Tab5 build uses now, the command says it can't.

### Keyboards

- **The keyboard accessory** (`ESP_VIM_KBD_TAB5`, `components/esp_kbd`). It's an
  STM32 at 0x6D on Ext.Port1 (SDA 0, SCL 1, interrupt 50), with the register
  protocol from M5Stack's driver (M5Tab5-Keyboard-UserDemo).
  - It runs in "normal" mode: each key's row and column, down and up.
  - A task waits for the interrupt, and looks every 100 ms anyway. With no keyboard
    attached it looks for one every two seconds, so clipping it on after boot works.
    It prints `ESPVIM-KBD tab5 keyboard: firmware 0x.., ESP_OK` and `... detached`.
  - `esp_kbd_tab5.c` turns the events into the same reports a USB keyboard sends.
    The bytes, xterm sequences and key repeat are then `esp_kbd`'s, as for every
    other keyboard. Host test: `pixi run kbd-test`.
  - The layout follows the key legends; Sym gives the second legends. The keyboard
    has no F-key row, so Sym also gives F1-F12 on `1`..`0` `-` `+`, Insert on Del,
    and PgUp, Home, PgDn and End on the arrows.
  - Aa shifts letters, Tab and the arrows; a symbol key sends its own legend either
    way.
  - Every modifier works held, or tapped for the next key only (tap it again to
    drop it). Aa tapped twice is Caps Lock.
  - The two LEDs show what's in effect. The first is Sym (blue), Aa (amber) or Caps
    Lock (red); the second is Ctrl (cyan) or Alt (white).
- **USB keyboards** on the USB-A port (`ESP_VIM_USB_KBD`, `components/esp_usbkbd`),
  through the USB Host Library and Espressif's HID class driver.
  - Boot-protocol keyboards are switched to boot reports.
  - Others are read through their report descriptors, with the same parser
    Bluetooth keyboards use.
  - Up to four at once. Each prints `ESPVIM-KBD USB keyboard N: vid:pid` when
    plugged in and `... unplugged` when removed.
- **BLE keyboards through the C6.** The Tab5 build has NimBLE's host with no local
  controller: esp-hosted carries its HCI to the C6. With esp-hosted 1.x NimBLE's
  transport brings the link up itself; with 2.x `esp_ble` connects and enables the
  C6's controller first. `:EspBtKeyboard` and the pairing screen then work as on the
  S3. The factory C6 firmware has Bluetooth LE (HCI over SDIO).
- The pairing screen stays away while the keyboard accessory or a USB keyboard is
  attached (`esp_kbd_wired()`).

## In the emulator, without the board

The `tab5uart` variant (`scripts/vim-build.sh tab5uart`) is the Tab5 build with its
console on UART0: esp-emu shows UART0 and not USB Serial/JTAG. Run with none of the
Tab5's hardware, it checks that the build copes when parts are missing, the way a
board with a fault would.

- Boot: `ESPVIM-BOARD tab5: expander 0x43 missing, 0x44 missing; ...`, then the
  display, touch and SD card each report that they're not there, and `ESPVIM-READY`.
  Vim runs over serial.
  - Before this, a missing expander restarted the chip: the BSP's
    `ESP_ERROR_CHECK`, now off (`CONFIG_BSP_ERROR_CHECK=n`).
  - Then the USB host driver asserted with no USB controller. USB keyboards now
    start only once the board has switched the USB-A port's power on.
  - With no keyboard on Ext.Port1, the I2C driver logged an error every two
    seconds, onto Vim's screen. Its log is now off during that probe.
- `esp_info`, `esp_display`, `esp_sd`, `esp_i2c_scan`, `esp_sensors`, `esp_power`,
  `esp_time`, `esp_heap`, `esp_net_status` and `esp_bt_keyboard_list` all return,
  with the parts reported missing.
- The round trip, interactive, peripheral and Python tests pass
  (`ESPVIM_TARGET=tab5uart`).
- Not testable here: WiFi, Bluetooth and `:EspC6`. The harness starts an emulated C6
  as well, and the C6 now gets as far as receiving the P4's init message. But then
  the emulated P4 stops altogether. esp-emu's SDIO bridge still can't carry an
  esp-hosted session (PHASE6.md, 6f part 3).

## The first board

It arrived on 2026-09-30. `esptool flash_id` in download mode: an ESP32-P4,
**revision v1.3**, 16 MB flash, its USB-C the P4's USB Serial/JTAG (`303a:1001`).

- The revision decided the build. ESP-IDF builds for P4s before v3.0 or from v3.0
  on, never both. Its default, v3.01 at least, would have had the bootloader refuse
  this chip. So `sdkconfig.defaults.tab5` selects revisions before v3, from v1.0.
  `tab5uart` sets v3 back for the emulator, whose P4 is v3.1, so it doesn't boot on
  this board. A v3 Tab5 would need a variant of its own.
- The factory firmware, all 16 MB, is backed up outside the repository. One
  `read_flash` of the whole chip failed part way ("Corrupt data"): USB through the
  development VM drops data now and then. Reading in 256 KB pieces, each after a
  reset into download mode, and then `verify_flash` against the chip's own MD5,
  works.
- First boots, and what each needed:
  - A boot loop before `app_main`: `assert failed: sdio_mempool_create`.
    esp-hosted 2.12.13 allocates its SDIO buffers (about 47 KB of DMA-capable
    RAM) in a C constructor, and on a pre-v3 P4 the large internal region (178 KiB)
    joins the heap only after start-up. Its PSRAM option fixed that. With 1.4.0,
    used now, the pool is made once the link is up.
  - `panel unknown`: nothing answered at 0x55 or 0x14. The ST7121's touch
    controller is part of the display chip and stays silent while the display is
    held in reset. `esp_board_panel_new()` now does what the BSP and M5Stack do:
    the display out of reset, a touch reset pulse, then the probe.
  - Upside down at 90°: the default is now 270°.
  - A tap crashed it, and later so did typing (the interrupt watchdog, the screen
    blue). The touch handler's start point had the same names as the grid's
    offset (`s_x0`/`s_y0`), so C merged them into one variable. Each tap moved the
    picture's origin, and the next redraw wrote past the frame buffer. Found with
    a core dump to flash: the USB console didn't survive the crash.
- Working: the display (ST7121, 106x30 in Terminus 12x24), touch (tap, scroll,
  drag), the keyboard accessory (firmware 0x01), a USB keyboard on USB-A, WiFi
  (the saved network joined at boot) and Bluetooth LE through the C6: scans, and
  a bonded keyboard that reconnects by itself after a restart.
  Internal RAM free at the prompt: about 186 KB.
- The C6 didn't answer esp-hosted 2.12.13: after the reset, every SDIO CMD5
  timed out (`sdmmc_init_ocr: send_op_cond (1) returned 0x107`). M5Stack's
  factory firmware (M5Tab5-UserDemo, esp-hosted 1.4.0) reached it on the same
  pins, slot, clock and reset. Built against 1.4.0, the link comes up: the C6
  runs esp-hosted 1.4.1, a WiFi scan found 14 networks, and a Bluetooth LE scan
  31 devices. The cause was 2.x's reset level, and two of 1.4's defaults then had
  to be set for the Tab5 (D1's pin, the slave chip). See DECISIONS.md, 2026-09-30.
  With a keyboard bonded, Bluetooth starts at boot and connects to the C6 there;
  the saved network is joined at boot too.
- No battery in the back and only USB power (a charger): the Tab5 went into deep
  sleep straight after boot. `esp_power` saw no computer on the USB port, so it
  took the board for running on its battery, and the INA226's reading with none
  fitted for a flat one (below 6.9 V, nothing unsaved: deep sleep). Fixed: the
  source is USB when expander 0x44 P6 (USB_DET, an input, pulled down) is high --
  a computer or a charger -- or when the battery isn't discharging (INA226 shunt
  register at most 50, about 25 mA through its 5 mOhm); and a reading under
  2.5 V a cell is no battery, never a low one. Measured: on battery P6 0, shunt
  450-490; on USB P6 1, shunt about -400 to -900 charging, about 0 full or with
  no battery.
- Checklist, 2026-10-01: 32 MB PSRAM, revision v1.3, 360 MHz, Vim's budget 16 MB,
  162 KB internal heap free with WiFi up (lowest 125 KB). The microSD card (4-bit,
  40 MHz) and WiFi together: five HTTPS downloads of a 621 KB file straight to `/sd`,
  all whole. `:EspSensors` finds everything expected, and something at 0x28 it
  doesn't know. The clock, set by NTP, reads the same from the RX8130, and after a
  power cycle with no battery and no network it is still right, from the RX8130.
  A full-screen scroll (`:help`, `<C-F>` held) is smooth. The web interface over
  WiFi: the login page in about 5.8 s the first time (a 3.6 s TLS handshake), its
  certificate the one Vim prints, the API refused without a session, a wrong
  password refused and the third in a row held off; internal heap at least 94 KB
  through it.
- The slow web interface was WiFi's modem sleep, not TLS: with hardware SHA, AES,
  MPI and ECC, a handshake takes 0.23-0.8 s; but the C6 dozes between beacons, and
  pings ran 57-137 ms (average 99), a 5.8 KB page up to 3 s. `esp_power` now turns
  modem sleep off while USB powers the board (`esp_net_power_save`), keeping it
  on battery: pings 4-90 ms (average 36), the page in 0.33-0.43 s.
- 0x28, on the internal bus, is in none of M5Stack's lists. It isn't the
  keyboard (it stays with the keyboard off), and doesn't act like a register
  chip: `esp_i2c_read(0x28, {reg}, n)` returns the same whatever {reg} -- 0xFD,
  then 0x4D bytes (just after boot once 0xFE, then 0xD2). A scan at each step of
  `esp_board_init` and the panel's start: absent with the C6 powered and held in
  reset, absent up to the LCD's reset; there, with the touch controller at 0x55,
  as soon as the panel leaves reset (100 ms later). So it is the display chip's
  (the ST7121's) second address, and `:EspSensors` names it so.
- Open: once, just after one of those downloads finished (its file whole), the
  Tab5 restarted, untouched and unnoticed on its screen. Not seen again in four
  tries, with no panic output caught. `esp_info()` said "watchdog"
  (`ESP_RST_WDT`), but a press of the Tab5's RESET button reads the same, so it
  may not have been a crash. The likeliest cause: esp-hosted restarts the host
  on purpose when one SDIO read of the C6's interrupt register fails. That is
  now patched to fail the link instead (DECISIONS.md, 2026-10-01): if it happens
  again, WiFi stops with "co-processor not responding" in `:EspLog`, and Vim
  stays.
- Log lines (ESP-IDF's, from any task) went over Vim's screen on the serial
  console. During a session they go to a 16 KB buffer instead (`esp_logbuf`),
  which `:EspLog` shows; at boot and between sessions they print, and are kept
  too. A failed DNS lookup's four esp-tls lines: none on the console, all four
  in `esp_log()`.
- The USB console doesn't always come back after a reset: the port can be
  missing for 30 s and more. After a software reset (esptool's watchdog reset)
  it was back in 3 s; the slow returns were after the RESET button, and once
  it didn't come back at all until replugged. Download mode (BOOT held, RESET
  pressed) brings it at once. esptool reaches the bootloader with no buttons
  (`--before usb_reset`), and resets out of it (`--after watchdog_reset`).

## First flash: the checklist

Do the steps in order. Each gives the next one something to stand on.

1. **Back up the factory firmware.** Put the Tab5 in download mode if needed (hold BOOT
   and press RESET), then read out all 16 MB:
   `python -m esptool --chip esp32p4 -p /dev/ttyACM0 read_flash 0 0x1000000 tab5-factory.bin`.
   Keep the file outside the repository. It is the only way back to M5Stack's demo
   firmware.
2. **Which port.** With the Tab5 on USB-C, `lsusb` should list `303a:1001`, Espressif's
   USB JTAG/serial debug unit, as `/dev/ttyACM*`. If it shows something else, the
   console assumption is wrong. The Vim console then needs UART0 on the M5-Bus pins,
   and the display still works.
3. **Flash the `tab5` build**, the app, bootloader, partition table and runtime, with
   the `write_flash` line `idf.py` prints. On a USB Serial/JTAG port, a terminal that
   changes DTR/RTS as it opens the port resets the chip (as on the ES3C28P). Open it
   with both left alone.
4. **Boot log, in order:**
   - `ESPVIM-BOARD tab5: expander 0x43 ok, 0x44 ok; C6 power ESP_OK, ...`. A missing
     expander means the I2C bus or its pins are wrong; nothing else on the board will
     work.
   - `ESPVIM-BOARD tab5: display ST7121, 720x1280 MIPI-DSI: ESP_OK` (or ST7123 /
     ILI9881C). If it says `panel unknown`, run `:EspI2cScan` over serial and look for
     0x55 or 0x14.
   - `ESPVIM-BOARD tab5: touch: ESP_OK`.
   - `ESPVIM-TERM 30x106 (display)`, then `ESPVIM-READY`.
5. **The screen.** You should see Vim's splash, in landscape. If it's upside down, run
   `:EspFlip` (kept in NVS). If it's mirrored, or shows in portrait, the rotation is
   wrong: note which, and change `ESP_VIM_DISP_ROTATION`. A black screen with the log
   above means the backlight or the lane rate is wrong. Try `:EspFont` to rule out
   drawing.
6. **Touch.** Tap a word: the cursor should go there. Drag up: the text should scroll
   down. If taps land in the wrong place, the touch rotation doesn't match the
   display's, which the host test says can't happen. Look at the raw points first.
7. **Memory.** In `:EspInfo`, check 32 MB PSRAM, and that the chip revision is v1.x:
   the `tab5` build is for revisions before v3 (see below). In `:EspHeap`, Vim's budget should
   be 16 MB.
8. **The C6.**
   - The boot log shows esp-hosted's `Identified slave [esp32c6]` and the slave's
     firmware version. Our side is 1.4.0. A different major version may not talk to
     us.
   - `:EspWifiScan`, then `:EspWifiConnect`, then `:EspGet` of a small file.
   - `:EspC6` shows both versions: "1.x" here and 1.4.1 on the C6, as shipped.
     `:EspC6 update` needs esp-hosted 2.x on this side; on 1.x it says so.
   - If the link never comes up, the `ESPVIM-NET` line says so after esp-hosted's
     retries. First check the C6's power (expander 0x44 P0). With no link,
     `:EspC6 update` can't help: the C6 then needs flashing through its own pins,
     which M5Stack doesn't bring out as a port. That is still to be found out.
9. **microSD.** With a FAT or exFAT card in, `:EspSd` should show it mounted, with
   the bus (4-bit or 1-bit) and the clock. Also check that WiFi still works with the
   card mounted: the two share the SDMMC controller.
10. **Sensors and clock.**
    - `:EspSensors` should list 0x32 RX8130, 0x41 INA226, 0x43/0x44 expanders, 0x68
      BMI270, the codecs and the touch controller.
    - `:EspTime`: after NTP has set the time once, it should survive a power cycle
      without the network.
    - `:EspPower`: about 7-8.4 V on the battery.
11. **Keyboards.**
    - Clip on the keyboard: the `ESPVIM-KBD tab5 keyboard` line should appear, then
      typing should work.
    - Check `Esc`, `Ctrl-W`, `Ctrl-[`, Sym+1 (F1, `:help`), Sym+arrows, and a tapped
      Ctrl then `r` (redo).
    - If no firmware line appears, nothing answers at 0x6D on SDA 0 / SCL 1. The
      driver holds those pins, so `:EspI2cScan` can't look there. Check the
      keyboard is seated, then the pin numbers against M5Stack's pinmap.
    - Then plug a USB keyboard into USB-A (`ESPVIM-KBD USB keyboard 0`).
    - Then `:EspBtKeyboard scan` with a BLE keyboard in pairing mode. This needs the
      C6's Bluetooth; see step 8.
12. **Heap and speed.** A full-screen redraw (`:help`, then `<C-F>` held) should look
    smooth. Note `:EspHeap`'s internal free space with WiFi up.

Record the results here: the revision found, the rotation that's right, the port, the
C6's firmware, and the timings.

## The SD card as the place to work

The Tab5's card is far bigger than `/fat`, so with a card mounted at startup the
device now uses it by default. All of these changes are in the stock `/vimrt/vimrc`
(plus a status field for the web page), so a user's `/fat/.vimrc` can change them
back. Without a card, or on a board without a slot, nothing changes.

- **Starting directory.** The vimrc does `cd /sd` when `esp_sd().mounted`. A `:cd`
  in `/fat/.vimrc`, which runs later, sets your own. The C default (`/fat`,
  `ESP_VIM_CWD_INITIAL`) is unchanged, so the emulator and boards without a card
  start where they did. `:EspSd eject` moves Vim back to `/fat` when it was inside
  the card.
- **The web page** opens in Vim's current directory. Vim's once-a-second publish
  (`autoload/esp/web.vim`) now carries `getcwd()`, and the page reads it from
  `/api/status` before its first listing. It doesn't follow later `:cd`s, so it
  won't jump while you browse. If the directory can't be listed, it opens `/fat`.
- **The full help on the card.** `/sd/vim` goes on `'runtimepath'` after
  `/fat/.vim` and before `/vimrt`. `pixi run sd-help DEST` (`scripts/sd-help.py`)
  writes all of Vim's `doc/*.txt` for the same Vim as the firmware, plus netrw's
  and a `tags` file, to `DEST/vim/doc`: 155 files, 10.4 MB, about 12,100 tags.
  - Vim's own `help.txt` and any tag the device's `help.txt` defines are left out,
    so the device's page stays first.
  - The card's complete `version9.txt` (and `uganda`, `sponsor`) is found before
    `/vimrt`'s trimmed one.
  - Checked in desktop Vim against the generated runtime: `:help usr_toc` and
    `:help version9` open from the card, `:help` and `:help EspSd` from `/vimrt`.
- **Spell downloads** go to `g:esp_spell_dir`, which is `/sd/vim/spell` with a
  card. Vim's `spellfile.vim` would otherwise use whichever `spell/` folder
  happened to exist. As with the quiet loaders, a `SourcePost` autocommand replaces
  its `spellfile#GetDirChoices()` once it loads. The new version puts that folder
  first (creating it) and still offers any other one already in use. Vim's file
  is not patched.

- **A vimrc on the card**, `/sd/vim/vimrc`, so settings move with the card. When
  it exists, the stock vimrc sets `$VIMINIT`, which Vim runs instead of looking
  for a vimrc. It sources the card's vimrc, then `/fat/.vimrc` (or
  `/fat/.vim/vimrc`), so the device's own settings win. Both count as the user's
  vimrc, so `defaults.vim` is skipped, as it is for any vimrc. `$MYVIMRC` is the
  card's.
  - The environment outlives a session (`:q` starts a new one), so the vimrc
    clears both variables when it set them and the card has gone.
  - Checked in desktop Vim: card then `/fat`; only `/fat` with no card vimrc; and
    unchanged, with the variables cleared, with no card.

As with `/fat/.vim`, scripts in `/sd/vim/plugin/` run at startup, so a card can
add plugins, and its vimrc runs on any device it goes into. `:help esp-sd-help` and `esp-vimrc` say so.

Tested on the Tab5 (2026-10-02), with a card in:
- Vim starts in `/sd`, with `/sd/vim` on `'runtimepath'` and `g:esp_spell_dir`
  set.
- `spellfile#GetDirChoices()` offers `/sd/vim/spell` and creates it.
- The card's vimrc runs first and `/fat/.vimrc` second, with `$MYVIMRC` the card's
  and `defaults.vim` skipped. With no card vimrc, Vim starts as before.
- `:EspSd eject` moves Vim to `/fat`.

The first card-vimrc try ran only the card's file. Vim reads the system vimrc
while `'compatible'` is still set, and `'cpoptions'` then has `C`, which turns off
`\` line continuation. The vimrc now uses none (desktop Vim run with `-N` hid
this).

- The web page opens in `/sd`, Vim's directory.

Not yet tested on the board: the card's help.

### A new session panicked: Vim's data split between two regions

Ending a session (`:qa!`, then a key) panicked as the next session started, and
the board rebooted, with `esp_info().reset` reading "panic". This was older than
the SD work. At first it was the heap allocator's assertion, on the new
session's first allocations:

```
assert failed: block_next tlsf_block_functions.h:161 (!block_is_last(block))
  esp_vim_malloc (esp_shims.c:408) <- evalvars_init <- eval_init <- common_init_1
  <- vim_main <- vim_task
```

With `CONFIG_ESP_VIM_HEAP_GUARD` it became a double free in the same place:
`set_vim_var_dict()` freeing the old value of `v:event`. In a fresh session that
slot is empty; this one held the last session's dict, already freed with the
rest of its heap. So `vimvars[]` had not been reset.

**Cause.** A new session restores libvim.a's `.data` from a snapshot and zeroes
its `.bss`, between bounds that `components/vim/linker.lf` puts around them with
`SURROUND`. The Tab5's P4 is revision 1.3, and the build for revisions before v3
(`ESP32P4_SELECTS_REV_LESS_V3`) has a second internal data region.
- ESP-IDF makes its `dram0_data` and `dram0_bss` placements in both regions and
  links with `--enable-non-contiguous-regions`, so the linker filled the low
  region with Vim's data and put the rest in the high one: 68 KB low (`vimvars`
  among it), 71 KB high.
- Each `SURROUND` was emitted in both regions, so `_vim_data_start` and the other
  bounds were each assigned twice, and the second value won. The bounds covered
  only the high part.
- A new session therefore left the low part as the last session had left it,
  with pointers into a heap that had since been freed.
- The emulator's esp32p4 build (v3) and the S3 boards have one region and were
  never affected.

**Fix.** `patches/esp-idf/0003` gives the high region targets only it has
(`dram1_data`, `dram1_bss`). On those builds `linker.lf` maps Vim's data and bss
there, once each, whole. `scripts/check-vim-sections.py` now runs after every
build (`scripts/vim-build.sh`). It reads the link map and fails if any of
libvim.a's writable sections is outside its bounds, if a bound is assigned more
than once, or if anything else is inside them. It flags the old tab5 build and
passes all seven variants.

**Checked on the board.**
- Five `:qa!` restarts in a row on the normal build: sessions 2 to 6 started
  without a reboot, uptime kept growing, and each session started without the
  last one's variables.
- Internal free memory held at about 155 KB per session.
- The card vimrc across real restarts: `$VIMINIT` and `$MYVIMRC` are cleared
  once the card's vimrc is gone, and `defaults.vim` is back.

### Emulator gate, 2026-10-02

Results after the SD work and the session fix. tab5uart and the S3 are
informational: tab5uart has no network in esp-emu (above), and the S3 has the
esp-emu defect in DECISIONS (2026-09-30).

| Suite | esp32p4 | tab5uart | esp32s3 |
|---|---|---|---|
| roundtrip | 8/8 | 8/8 | 8/8 |
| interactive | 73/73 | 57/57 | 64/65 |
| web | 24/24, twice, after the fix below | skipped: no network | `ESP_ERR_HTTPD_TASK`, see below |
| hw | 10/10 | 10/10 | 7/8 |
| wifi | none (Ethernet build) | 2/8 | 8/8 |
| ble | none (no Bluetooth) | 3/7 | 7/7 |
| git | 47/47 | 23/25 | not run |
| py | 37/37 | 37/37 | 37/37 |

- **tab5uart:** the wifi and ble failures are its known gap: no C6 session over
  esp-emu's SDIO bridge, and no expander to power it. git's two failures are its
  clone over HTTP, which has no network to use.
- **esp32s3:**
  - The interactive failure is a heap-corruption panic in the allocator while a
    syntax file loads, the known esp-emu defect.
  - The hw failure is an interrupt watchdog in `esp_adc_read()`: esp-emu never
    finishes the S3's ADC conversion.
- **Stale runtime images in the first S3 run:** that run used runtime images
  made before the vimrc lost its `\` continuation lines, so every scenario had
  E10. `vim-build.sh` doesn't regenerate them (`pixi run runtime`, or the pixi
  build tasks, does). After regenerating, roundtrip passes.

**The web interface's certificate, fixed.** The esp32p4 web run once failed at
the first connection: Python's OpenSSL 3.6 said "ASN1 lib". The cause was the
certificate's random 16-byte serial number. It was made positive by clearing the
top bit only, so a serial starting `00` and then a byte below `80`, 1 in 256 of
them, was a non-minimal DER INTEGER. mbedTLS writes such a serial as it is, and
OpenSSL refuses the whole certificate. Shown by editing a served certificate's
serial: `00 12` fails to decode, `00 92` decodes. The first byte is now
`0x40`–`0x7f`. A device whose stored certificate drew such a serial keeps it in
NVS; that is rare, and it has never been seen on a board.

**Open: the S3's web server can't start its task in esp-emu.** With WiFi up the
S3 has about 23 KB of internal RAM free, largest block 13.8 KB (72 KB and 31 KB
at boot). That is too little for the server's 10 KB stack plus the rest of what
it allocates, so `:EspWebStart` gives `ESP_ERR_HTTPD_TASK`. Nothing new among
the largest static allocations explains it, and PHASE6 recorded the same failure
once before (Bluetooth's controller in IRAM). Check on the FNK0115 and the
ES3C28P before treating it as a firmware regression.
