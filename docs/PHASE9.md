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
| ESP32-C6 | SDIO slot 1: CLK 12, CMD 13, D0-D3 11/10/9/8; reset 15, active low | esp-hosted 2.12.13. Powered by expander 0x44 P0, which must be on before esp-hosted starts |
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
  the frame buffer, turned 90° by default (`ESP_VIM_DISP_ROTATION`), and writes the
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
  two cells. Below 6.9 V with nothing unsaved, the Tab5 goes into deep sleep.
- **`:EspInfo`** shows the board and its display controller.

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
  controller: esp-hosted carries its HCI to the C6. `esp_ble` connects to the C6 and
  enables its controller before it starts NimBLE. `:EspBtKeyboard` and the pairing
  screen then work as on the S3. Our C6 image (`pixi run c6-build`) has Bluetooth; the
  factory one may not.
- The pairing screen stays away while the keyboard accessory or a USB keyboard is
  attached (`esp_kbd_wired()`).

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
7. **Memory.** In `:EspInfo`, check 32 MB PSRAM, and that the chip revision is v1.x or
   v3.x. Pre-v3 P4s need their own build variant. In `:EspHeap`, Vim's budget should
   be 16 MB.
8. **The C6.**
   - The boot log shows esp-hosted's `Identified slave [esp32c6]` and the slave's
     firmware version. Our side is 2.12.13. A different major version may not talk to
     us.
   - `:EspWifiScan`, then `:EspWifiConnect`, then `:EspGet` of a small file.
   - If the link never comes up, the `ESPVIM-NET` line says so after esp-hosted's
     retries. First check the C6's power (expander 0x44 P0). Then find out what the
     C6 runs: M5Stack's C6 firmware may need replacing with `pixi run c6-build`'s
     image. How M5Stack flashes the C6 is still to be found out.
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
