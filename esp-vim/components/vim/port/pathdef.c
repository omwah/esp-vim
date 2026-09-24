/*
 * auto/pathdef.c replacement for the ESP-IDF build.
 *
 * Upstream generates this at build time from the Makefile's variables. Here the
 * values are fixed by the firmware image, so it is written by hand.
 *
 * The two that matter are the runtime directories: they must name the read-only
 * partition the curated $VIMRUNTIME is flashed to (docs/PLAN.md Phase 4), or
 * FEAT_NORMAL will fail during startup when it tries to source defaults.vim.
 */

#include "vim.h"

char_u *default_vim_dir        = (char_u *)"/vimrt";
char_u *default_vimruntime_dir = (char_u *)"/vimrt";
char_u *all_cflags             = (char_u *)"riscv32-esp-elf-gcc (ESP-IDF v5.5.5)";
char_u *all_lflags             = (char_u *)"esp-idf component link";
char_u *compiled_user          = (char_u *)"esp";
char_u *compiled_sys           = (char_u *)"esp32p4";
