/*
 * Vim links its own copy of xdiff (src/xdiff), with the same xdl_* names as
 * libgit2's (deps/xdiff): libgit2's are renamed here so the two coexist.
 * Every global xdl_ symbol libgit2's copy defines (riscv32-esp-elf-nm).
 */
#pragma once

#define xdl_alloc_grow_helper git2_xdl_alloc_grow_helper
#define xdl_blankline git2_xdl_blankline
#define xdl_bogosqrt git2_xdl_bogosqrt
#define xdl_build_script git2_xdl_build_script
#define xdl_cha_alloc git2_xdl_cha_alloc
#define xdl_cha_free git2_xdl_cha_free
#define xdl_cha_init git2_xdl_cha_init
#define xdl_change_compact git2_xdl_change_compact
#define xdl_diff git2_xdl_diff
#define xdl_do_diff git2_xdl_do_diff
#define xdl_do_histogram_diff git2_xdl_do_histogram_diff
#define xdl_do_patience_diff git2_xdl_do_patience_diff
#define xdl_emit_diff git2_xdl_emit_diff
#define xdl_emit_diffrec git2_xdl_emit_diffrec
#define xdl_emit_hunk_hdr git2_xdl_emit_hunk_hdr
#define xdl_fall_back_diff git2_xdl_fall_back_diff
#define xdl_free_env git2_xdl_free_env
#define xdl_free_script git2_xdl_free_script
#define xdl_get_hunk git2_xdl_get_hunk
#define xdl_guess_lines git2_xdl_guess_lines
#define xdl_hash_record git2_xdl_hash_record
#define xdl_hashbits git2_xdl_hashbits
#define xdl_merge git2_xdl_merge
#define xdl_mmfile_first git2_xdl_mmfile_first
#define xdl_mmfile_size git2_xdl_mmfile_size
#define xdl_num_out git2_xdl_num_out
#define xdl_prepare_env git2_xdl_prepare_env
#define xdl_recmatch git2_xdl_recmatch
#define xdl_recs_cmp git2_xdl_recs_cmp
