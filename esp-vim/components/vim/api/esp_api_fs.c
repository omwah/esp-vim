/*
 * esp_fs_*(): Vim script access to the esp_fs core (components/esp_fs), which
 * validates every path and does the work. Behind :EspFiles.
 *
 * Paths may be relative: they are made absolute against Vim's working
 * directory here, then esp_fs normalises and checks them. During long
 * operations the core calls back into Vim's break check, so CTRL-C stops a
 * big copy and the busy spinner turns.
 */

#include "vim.h"
#include "esp_vim_api.h"
#include "esp_fs.h"

/* {tv} as an absolute path, allocated; NULL (error given) on failure. */
static char *abs_arg(typval_T *tv)
{
    char_u *s = tv_get_string_chk(tv);
    if (s == NULL)
        return NULL;
    if (*s == '/')
        return (char *)vim_strsave(s);
    char_u *full = FullName_save(s, TRUE);
    if (full == NULL)
        emsg("esp_fs: cannot make the path absolute");
    return (char *)full;
}

static bool progress(void *ctx UNUSED, uint64_t bytes UNUSED)
{
    ui_breakcheck();
    return !got_int;
}

static void report(const char *fn, esp_fs_err_t *err)
{
    semsg("%s(): %s", fn, err->msg);
}

static bool add_entry(void *ctx, const esp_fs_entry_t *e)
{
    dict_T *d = dict_alloc();
    if (d == NULL)
        return false;
    dict_add_string(d, "name", (char_u *)e->name);
    dict_add_string(d, "type", (char_u *)(e->dir ? "dir" : "file"));
    dict_add_number(d, "size", (varnumber_T)e->size);
    dict_add_number(d, "mtime", (varnumber_T)e->mtime);
    return list_append_dict((list_T *)ctx, d) == OK;
}

/* esp_fs_list({dir}) -> List of Dicts: name, type ("file"/"dir"), size, mtime.
 * "/" lists the storage roots. */
void f_esp_fs_list(typval_T *argvars, typval_T *rettv)
{
    if (rettv_list_alloc(rettv) == FAIL)
        return;
    char *dir = abs_arg(&argvars[0]);
    if (dir == NULL)
        return;
    esp_fs_err_t err;
    if (esp_fs_list(dir, add_entry, rettv->vval.v_list, &err) != 0)
        report("esp_fs_list", &err);
    vim_free(dir);
}

static void root_dict(dict_T *d, const esp_fs_root_t *r)
{
    dict_add_string(d, "path", (char_u *)r->path);
    dict_add_bool(d, "readonly", r->readonly);
    dict_add_number(d, "total", (varnumber_T)r->total);
    dict_add_number(d, "free", (varnumber_T)r->free);
}

static bool add_root(void *ctx, const esp_fs_root_t *r)
{
    dict_T *d = dict_alloc();
    if (d == NULL)
        return false;
    root_dict(d, r);
    return list_append_dict((list_T *)ctx, d) == OK;
}

/* esp_fs_roots() -> List of Dicts: path, readonly, total, free. */
void f_esp_fs_roots(typval_T *argvars UNUSED, typval_T *rettv)
{
    if (rettv_list_alloc(rettv) == FAIL)
        return;
    esp_fs_err_t err;
    if (esp_fs_roots(add_root, rettv->vval.v_list, &err) != 0)
        report("esp_fs_roots", &err);
}

/* esp_fs_info({path}) -> Dict of the root holding {path}: path, readonly,
 * total, free. */
void f_esp_fs_info(typval_T *argvars, typval_T *rettv)
{
    if (rettv_dict_alloc(rettv) == FAIL)
        return;
    char *path = abs_arg(&argvars[0]);
    if (path == NULL)
        return;
    esp_fs_root_t r;
    esp_fs_err_t err;
    if (esp_fs_space(path, &r, &err) == 0)
        root_dict(rettv->vval.v_dict, &r);
    else
        report("esp_fs_info", &err);
    vim_free(path);
}

static unsigned flags_arg(typval_T *tv)
{
    return tv->v_type != VAR_UNKNOWN && tv_get_bool(tv) ? ESP_FS_OVERWRITE : 0;
}

/* esp_fs_copy({src}, {dst} [, {overwrite}]) and esp_fs_move(): {dst} is the
 * complete new name. TRUE on success. */
static void copy_or_move(typval_T *argvars, typval_T *rettv, bool move)
{
    const char *fn = move ? "esp_fs_move" : "esp_fs_copy";
    char *src = abs_arg(&argvars[0]);
    char *dst = abs_arg(&argvars[1]);
    if (src != NULL && dst != NULL) {
        esp_fs_err_t err;
        int rc = move ? esp_fs_move(src, dst, flags_arg(&argvars[2]), progress, NULL, &err)
                      : esp_fs_copy(src, dst, flags_arg(&argvars[2]), progress, NULL, &err);
        if (rc == 0)
            rettv->vval.v_number = TRUE;
        else
            report(fn, &err);
    }
    vim_free(src);
    vim_free(dst);
}

void f_esp_fs_copy(typval_T *argvars, typval_T *rettv) { copy_or_move(argvars, rettv, false); }
void f_esp_fs_move(typval_T *argvars, typval_T *rettv) { copy_or_move(argvars, rettv, true); }

/* esp_fs_delete({path}): a file, or a directory with everything in it. */
void f_esp_fs_delete(typval_T *argvars, typval_T *rettv)
{
    char *path = abs_arg(&argvars[0]);
    if (path == NULL)
        return;
    esp_fs_err_t err;
    if (esp_fs_delete(path, progress, NULL, &err) == 0)
        rettv->vval.v_number = TRUE;
    else
        report("esp_fs_delete", &err);
    vim_free(path);
}

/* esp_fs_mkdir({path}) */
void f_esp_fs_mkdir(typval_T *argvars, typval_T *rettv)
{
    char *path = abs_arg(&argvars[0]);
    if (path == NULL)
        return;
    esp_fs_err_t err;
    if (esp_fs_mkdir(path, &err) == 0)
        rettv->vval.v_number = TRUE;
    else
        report("esp_fs_mkdir", &err);
    vim_free(path);
}
