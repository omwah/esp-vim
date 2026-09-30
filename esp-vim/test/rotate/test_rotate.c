/*
 * Host test of components/esp_board/include/esp_board_rotate.h: the turn
 * between Vim's picture and a panel mounted sideways, which the display draws
 * with and the touch controller's points are turned back by.
 * `pixi run rotate-test`.
 *
 * For every rotation, at the Tab5's size (1280x720 on a 720x1280 panel) and a
 * small odd one: every logical point lands inside the panel, no two on the same
 * pixel, and turning back gives the point again; the rectangle a logical one
 * covers is exactly the pixels its points land on; and upside down (:EspFlip,
 * the rotation plus 180) matches turning the point over first -- which is how
 * the display, drawing at rot + 180, and the touch path, turning back at rot
 * and then unflipping, stay in agreement.
 */

#include "esp_board_rotate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;

#define CHECK(cond, ...) do { \
        checks++; \
        if (!(cond)) { fails++; printf("  FAIL  line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } \
    } while (0)

static void native_size(int rot, int w, int h, int *nw, int *nh)
{
    int side = rot == 90 || rot == 270;
    *nw = side ? h : w;
    *nh = side ? w : h;
}

static void test_size(int w, int h)
{
    static const int rots[] = { 0, 90, 180, 270 };
    for (int r = 0; r < 4; r++) {
        int rot = rots[r], nw, nh;
        native_size(rot, w, h, &nw, &nh);
        unsigned char *hit = calloc((size_t)nw * nh, 1);
        int outside = 0, twice = 0, back = 0, flip = 0;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int nx, ny, lx, ly;
                esp_board_rot_to_native(rot, w, h, x, y, &nx, &ny);
                if (nx < 0 || ny < 0 || nx >= nw || ny >= nh) {
                    outside++;
                    continue;
                }
                if (hit[(size_t)ny * nw + nx]++)
                    twice++;
                esp_board_rot_to_logical(rot, w, h, nx, ny, &lx, &ly);
                if (lx != x || ly != y)
                    back++;
                /* Upside down: drawn at rot + 180 ... */
                int fx, fy;
                esp_board_rot_to_native((rot + 180) % 360, w, h, x, y, &fx, &fy);
                /* ... a touch there turned back at rot, then unflipped. */
                int tx, ty;
                esp_board_rot_to_logical(rot, w, h, fx, fy, &tx, &ty);
                tx = w - 1 - tx;
                ty = h - 1 - ty;
                if (tx != x || ty != y)
                    flip++;
            }
        CHECK(outside == 0, "%dx%d at %d: %d points off the panel", w, h, rot, outside);
        CHECK(twice == 0, "%dx%d at %d: %d pixels hit twice", w, h, rot, twice);
        CHECK(back == 0, "%dx%d at %d: %d points don't turn back", w, h, rot, back);
        CHECK(flip == 0, "%dx%d at %d: %d points disagree upside down", w, h, rot, flip);

        /* Rectangles, as the display draws them: a cell run of a row. */
        static const int rects[][4] = { { 0, 0, 1, 1 }, { 0, 0, 192, 24 }, { 5, 3, 17, 11 } };
        for (size_t i = 0; i < sizeof rects / sizeof rects[0]; i++) {
            int x0 = rects[i][0], y0 = rects[i][1], x1 = rects[i][2], y1 = rects[i][3];
            if (x1 > w || y1 > h)
                continue;
            int nx0, ny0, nx1, ny1;
            esp_board_rot_rect(rot, w, h, x0, y0, x1, y1, &nx0, &ny0, &nx1, &ny1);
            int bad = 0;
            for (int y = y0; y < y1; y++)
                for (int x = x0; x < x1; x++) {
                    int nx, ny;
                    esp_board_rot_to_native(rot, w, h, x, y, &nx, &ny);
                    if (nx < nx0 || nx >= nx1 || ny < ny0 || ny >= ny1)
                        bad++;
                }
            CHECK(bad == 0, "%dx%d at %d: rect %d: %d points outside its native rect", w, h, rot, (int)i, bad);
            CHECK((nx1 - nx0) * (ny1 - ny0) == (x1 - x0) * (y1 - y0),
                  "%dx%d at %d: rect %d: native rect %dx%d is not the same area", w, h, rot, (int)i,
                  nx1 - nx0, ny1 - ny0);
        }
        free(hit);
    }
}

int main(void)
{
    test_size(1280, 720);       /* the Tab5 */
    test_size(7, 5);

    /* The Tab5 at 90: the picture's top left is the panel's top right, and its
     * top right the panel's bottom right. */
    int nx, ny;
    esp_board_rot_to_native(90, 1280, 720, 0, 0, &nx, &ny);
    CHECK(nx == 719 && ny == 0, "90: (0,0) -> (%d,%d), want (719,0)", nx, ny);
    esp_board_rot_to_native(90, 1280, 720, 1279, 0, &nx, &ny);
    CHECK(nx == 719 && ny == 1279, "90: (1279,0) -> (%d,%d), want (719,1279)", nx, ny);
    esp_board_rot_to_native(270, 1280, 720, 0, 0, &nx, &ny);
    CHECK(nx == 0 && ny == 1279, "270: (0,0) -> (%d,%d), want (0,1279)", nx, ny);

    printf("rotate: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
