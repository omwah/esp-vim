/*
 * esp_fs: the ONE file-operations core behind every front end that changes
 * files -- the :EspFiles manager (via the esp_fs_*() Vim builtins), and later
 * the web file manager and the transfer paths (docs/PLAN.md, Phase 6).
 *
 * Why one core: a file manager reachable over the network is exactly where a
 * path-traversal bug becomes remote arbitrary file write. So path validation
 * lives here, once, audited, and nothing else is allowed to skip it:
 *
 *   - every path is absolute and is normalised lexically ("." and ".." folded;
 *     FAT has no symlinks, so the lexical result is the real one);
 *   - it must lie inside a mounted root: /fat, /sd (read-write), /vimrt
 *     (read-only). "/" itself exists only as a list of those roots;
 *   - a root itself can be listed but never deleted, moved or overwritten.
 *
 * Thread safety: callable from any task. Each operation holds one mutex, and
 * all scratch memory is per call. This component knows nothing about Vim.
 *
 * Errors: functions return 0, or -1 with a message (naming the path) in *err.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char msg[192];
} esp_fs_err_t;

typedef struct {
    const char *name;           /* entry name, no directory part */
    bool dir;
    uint64_t size;              /* bytes; 0 for directories */
    time_t mtime;
} esp_fs_entry_t;

typedef struct {
    const char *path;           /* "/fat", "/vimrt", "/sd" */
    bool readonly;
    uint64_t total, free;       /* bytes; 0 if unknown */
} esp_fs_root_t;

/* Called during long operations with the bytes done so far in this call.
 * Return false to stop: the operation fails with "interrupted". */
typedef bool (*esp_fs_progress_cb)(void *ctx, uint64_t bytes);

/* Called for each entry of a listing; return false to stop early. */
typedef bool (*esp_fs_entry_cb)(void *ctx, const esp_fs_entry_t *entry);
typedef bool (*esp_fs_root_cb)(void *ctx, const esp_fs_root_t *root);

#define ESP_FS_OVERWRITE  (1u << 0)     /* copy/move may replace an existing file */

/* Create the mutex. Call once at boot, before any task uses esp_fs. */
esp_err_t esp_fs_init(void);

/*
 * Normalise and validate {path}; the result goes to {out}. With {for_write},
 * the path must be writable and must not be a root itself. Exposed so front
 * ends can validate before doing anything of their own.
 */
int esp_fs_check(const char *path, bool for_write, char *out, size_t outlen, esp_fs_err_t *err);

/* The mounted roots. */
int esp_fs_roots(esp_fs_root_cb cb, void *ctx, esp_fs_err_t *err);

/* The root containing {path}, with its space; *root->path is static. */
int esp_fs_space(const char *path, esp_fs_root_t *root, esp_fs_err_t *err);

/* Entries of a directory ("." and ".." left out). "/" lists the roots. */
int esp_fs_list(const char *dir, esp_fs_entry_cb cb, void *ctx, esp_fs_err_t *err);

/* Copy a file or (recursively) a directory. {dst} is the full new name. */
int esp_fs_copy(const char *src, const char *dst, unsigned flags,
                esp_fs_progress_cb progress, void *ctx, esp_fs_err_t *err);

/* Move or rename; across roots this is copy-then-delete. */
int esp_fs_move(const char *src, const char *dst, unsigned flags,
                esp_fs_progress_cb progress, void *ctx, esp_fs_err_t *err);

/* Delete a file, or a directory and everything in it. */
int esp_fs_delete(const char *path, esp_fs_progress_cb progress, void *ctx, esp_fs_err_t *err);

/* Create a directory (its parent must exist). */
int esp_fs_mkdir(const char *path, esp_fs_err_t *err);

/*
 * Streaming, for front ends that move data themselves (the web server).
 *
 * esp_fs_open_read(): a validated file open for reading; its size in *size.
 * esp_fs_create_part(): validate {path} for writing (refusing an existing file
 *   unless {overwrite}) and open "{path}.part" for the data.
 * esp_fs_finish_part(): with {ok}, replace {path} by the finished part;
 *   otherwise delete the part. A failed upload never leaves a truncated file.
 * All return -1 with a message on failure; the open calls return an fd.
 */
int esp_fs_open_read(const char *path, uint64_t *size, esp_fs_err_t *err);
int esp_fs_create_part(const char *path, bool overwrite, esp_fs_err_t *err);
int esp_fs_finish_part(const char *path, bool ok, esp_fs_err_t *err);

#ifdef __cplusplus
}
#endif
