/*
 * esp_pairui: pairing a Bluetooth keyboard by touch, with no keyboard at all
 * (docs/PLAN.md, Phase 11: "the touch overlay").
 *
 * A board with a screen and a touch panel but no keyboard of its own (a CYD)
 * shows a full-screen overlay when there's no Bluetooth keyboard: 10 s after
 * boot, or 15 s after the keyboard went away. It lists the keyboards in range;
 * tap one, then "Pair". "Not now" hides it until a keyboard has come and gone
 * again. It closes as soon as a keyboard connects. Serial input doesn't count
 * as a keyboard: nobody may be on the other end.
 *
 * Builds without a display, touch panel and Bluetooth keyboards get a stub.
 */

#pragma once

/* Start watching (its own task). Call once at boot, after esp_ble_kbd_boot()
 * and the display and touch panel. */
void esp_pairui_start(void);
