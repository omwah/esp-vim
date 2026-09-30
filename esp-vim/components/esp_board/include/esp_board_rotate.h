/*
 * Between the picture Vim sees (w x h, "logical") and a panel mounted turned
 * (its own, "native", coordinates): one place for both directions, so the
 * display, which draws, and the touch controller, which reports where a finger
 * is, can't disagree. The Tab5's panel is portrait (720 x 1280) and Vim's
 * picture landscape (1280 x 720).
 *
 * rot is how far the picture is turned on the panel, clockwise: 0, 90, 180 or
 * 270. At 90 or 270 the native panel is h wide and w tall. Header-only, so the
 * host test (esp-vim/test/rotate) checks the same code.
 */
#pragma once

/* Logical (x, y) to native. */
static inline void esp_board_rot_to_native(int rot, int w, int h, int x, int y, int *nx, int *ny)
{
    switch (rot) {
    case 90:  *nx = h - 1 - y; *ny = x;         break;
    case 180: *nx = w - 1 - x; *ny = h - 1 - y; break;
    case 270: *nx = y;         *ny = w - 1 - x; break;
    default:  *nx = x;         *ny = y;         break;
    }
}

/* Native (nx, ny) to logical: the inverse. */
static inline void esp_board_rot_to_logical(int rot, int w, int h, int nx, int ny, int *x, int *y)
{
    switch (rot) {
    case 90:  *x = ny;         *y = h - 1 - nx; break;
    case 180: *x = w - 1 - nx; *y = h - 1 - ny; break;
    case 270: *x = w - 1 - ny; *y = nx;         break;
    default:  *x = nx;         *y = ny;         break;
    }
}

/* The native rectangle [*nx0, *nx1) x [*ny0, *ny1) a logical one covers. */
static inline void esp_board_rot_rect(int rot, int w, int h, int x0, int y0, int x1, int y1,
                                      int *nx0, int *ny0, int *nx1, int *ny1)
{
    int ax, ay, bx, by;
    esp_board_rot_to_native(rot, w, h, x0, y0, &ax, &ay);
    esp_board_rot_to_native(rot, w, h, x1 - 1, y1 - 1, &bx, &by);
    *nx0 = ax < bx ? ax : bx;
    *nx1 = (ax > bx ? ax : bx) + 1;
    *ny0 = ay < by ? ay : by;
    *ny1 = (ay > by ? ay : by) + 1;
}
