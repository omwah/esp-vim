/*
 * esp_ssh_*(): Vim script access to components/esp_ssh (SFTP/SCP). Behind
 * :EspFiles' remote panes, netrw's scp:// and sftp:// (patch 0010) and
 * :EspSshKeygen.
 *
 * Errors are Vim errors whose text says what happened, so autoload/esp/ssh.vim
 * can react: "unknown host key" (ask the user, then esp_ssh_trust()), "host key
 * CHANGED" (refuse), "authentication failed" (ask for a password, retry).
 * The optional {opts} Dict takes "password" and "key" (a private key file).
 */

#include "vim.h"
#include "esp_vim_api.h"
#include "esp_ssh.h"

typedef struct {
    esp_ssh_auth_t auth;
    char_u *password, *key;             /* owned copies */
} opts_t;

static void opts_get(typval_T *tv, opts_t *o)
{
    memset(o, 0, sizeof *o);
    if (tv->v_type != VAR_DICT || tv->vval.v_dict == NULL)
        return;
    o->password = dict_get_string(tv->vval.v_dict, "password", TRUE);
    o->key = dict_get_string(tv->vval.v_dict, "key", TRUE);
    if (o->key != NULL) {
        char_u *full = FullName_save(o->key, TRUE);
        vim_free(o->key);
        o->key = full;
    }
    o->auth.password = (char *)o->password;
    o->auth.keyfile = (char *)o->key;
}

static void opts_free(opts_t *o)
{
    if (o->password != NULL) {
        memset(o->password, 0, STRLEN(o->password));    /* do not leave it lying about */
        vim_free(o->password);
    }
    vim_free(o->key);
}

static bool progress(void *ctx UNUSED, uint64_t bytes UNUSED)
{
    ui_breakcheck();
    return !got_int;
}

/* An argument as an absolute local path (allocated), or NULL. */
static char_u *local_arg(typval_T *tv)
{
    char_u *s = tv_get_string_chk(tv);
    return s == NULL ? NULL : FullName_save(s, TRUE);
}

static char_u *str_arg(typval_T *tv)
{
    char_u *s = tv_get_string_chk(tv);
    return s == NULL ? NULL : vim_strsave(s);
}

#define REPORT(fn) semsg("%s(): %s", fn, err)

/* esp_ssh_hostkey({url}) -> Dict: type, fingerprint, status ("known",
 * "unknown" or "changed"). Connects only far enough to see the key. */
void f_esp_ssh_hostkey(typval_T *argvars, typval_T *rettv)
{
    if (rettv_dict_alloc(rettv) == FAIL)
        return;
    char_u *url = str_arg(&argvars[0]);
    if (url == NULL)
        return;
    char err[256];
    esp_ssh_hostkey_t k;
    if (esp_ssh_hostkey((char *)url, &k, err, sizeof err) == 0) {
        dict_T *d = rettv->vval.v_dict;
        dict_add_string(d, "type", (char_u *)k.type);
        dict_add_string(d, "fingerprint", (char_u *)k.fingerprint);
        dict_add_string(d, "status", (char_u *)(k.status == 0 ? "known"
                        : k.status == ESP_SSH_E_HOSTKEY_CHANGED ? "changed" : "unknown"));
    } else {
        REPORT("esp_ssh_hostkey");
    }
    vim_free(url);
}

/* esp_ssh_trust({url}): record the host's key in /fat/.ssh/known_hosts. */
void f_esp_ssh_trust(typval_T *argvars, typval_T *rettv)
{
    char_u *url = str_arg(&argvars[0]);
    if (url == NULL)
        return;
    char err[256];
    if (esp_ssh_trust((char *)url, err, sizeof err) == 0)
        rettv->vval.v_number = TRUE;
    else
        REPORT("esp_ssh_trust");
    vim_free(url);
}

/* esp_ssh_get({url}, {file} [, {opts}]) -> Dict: size, path; {} on error. */
void f_esp_ssh_get(typval_T *argvars, typval_T *rettv)
{
    if (rettv_dict_alloc(rettv) == FAIL)
        return;
    char_u *url = str_arg(&argvars[0]);
    char_u *file = local_arg(&argvars[1]);
    opts_t o;
    opts_get(&argvars[2], &o);
    if (url != NULL && file != NULL) {
        char err[256];
        uint64_t size = 0;
        if (esp_ssh_get((char *)url, (char *)file, &o.auth, progress, NULL, &size,
                        err, sizeof err) == 0) {
            dict_add_number(rettv->vval.v_dict, "size", (varnumber_T)size);
            dict_add_string(rettv->vval.v_dict, "path", file);
        } else {
            REPORT("esp_ssh_get");
        }
    }
    opts_free(&o);
    vim_free(url);
    vim_free(file);
}

/* esp_ssh_put({file}, {url} [, {opts}]) -> Dict: size; {} on error. */
void f_esp_ssh_put(typval_T *argvars, typval_T *rettv)
{
    if (rettv_dict_alloc(rettv) == FAIL)
        return;
    char_u *file = local_arg(&argvars[0]);
    char_u *url = str_arg(&argvars[1]);
    opts_t o;
    opts_get(&argvars[2], &o);
    if (url != NULL && file != NULL) {
        char err[256];
        uint64_t size = 0;
        if (esp_ssh_put((char *)file, (char *)url, &o.auth, progress, NULL, &size,
                        err, sizeof err) == 0)
            dict_add_number(rettv->vval.v_dict, "size", (varnumber_T)size);
        else
            REPORT("esp_ssh_put");
    }
    opts_free(&o);
    vim_free(url);
    vim_free(file);
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

/* esp_ssh_list({url} [, {opts}]) -> List of Dicts, as esp_fs_list() returns. */
void f_esp_ssh_list(typval_T *argvars, typval_T *rettv)
{
    if (rettv_list_alloc(rettv) == FAIL)
        return;
    char_u *url = str_arg(&argvars[0]);
    opts_t o;
    opts_get(&argvars[1], &o);
    if (url != NULL) {
        char err[256];
        if (esp_ssh_list((char *)url, &o.auth, add_entry, rettv->vval.v_list, err, sizeof err) != 0)
            REPORT("esp_ssh_list");
    }
    opts_free(&o);
    vim_free(url);
}

void f_esp_ssh_mkdir(typval_T *argvars, typval_T *rettv)
{
    char_u *url = str_arg(&argvars[0]);
    opts_t o;
    opts_get(&argvars[1], &o);
    if (url != NULL) {
        char err[256];
        if (esp_ssh_mkdir((char *)url, &o.auth, err, sizeof err) == 0)
            rettv->vval.v_number = TRUE;
        else
            REPORT("esp_ssh_mkdir");
    }
    opts_free(&o);
    vim_free(url);
}

/* esp_ssh_remove({url} [, {opts}]): a file, or a directory with its contents. */
void f_esp_ssh_remove(typval_T *argvars, typval_T *rettv)
{
    char_u *url = str_arg(&argvars[0]);
    opts_t o;
    opts_get(&argvars[1], &o);
    if (url != NULL) {
        char err[256];
        if (esp_ssh_remove((char *)url, &o.auth, progress, NULL, err, sizeof err) == 0)
            rettv->vval.v_number = TRUE;
        else
            REPORT("esp_ssh_remove");
    }
    opts_free(&o);
    vim_free(url);
}

/* esp_ssh_rename({url}, {newpath} [, {opts}]): {newpath} is a remote path. */
void f_esp_ssh_rename(typval_T *argvars, typval_T *rettv)
{
    char_u *url = str_arg(&argvars[0]);
    char_u *to = str_arg(&argvars[1]);
    opts_t o;
    opts_get(&argvars[2], &o);
    if (url != NULL && to != NULL) {
        char err[256];
        if (esp_ssh_rename((char *)url, (char *)to, &o.auth, err, sizeof err) == 0)
            rettv->vval.v_number = TRUE;
        else
            REPORT("esp_ssh_rename");
    }
    opts_free(&o);
    vim_free(url);
    vim_free(to);
}

/* esp_ssh_keygen([{file} [, {comment}]]) -> the public key line. Generates an
 * ECDSA P-256 pair: {file} (default /fat/.ssh/id_ecdsa) and {file}.pub. */
void f_esp_ssh_keygen(typval_T *argvars, typval_T *rettv)
{
    rettv->v_type = VAR_STRING;
    rettv->vval.v_string = NULL;
    char_u *file = argvars[0].v_type == VAR_UNKNOWN
                 ? vim_strsave((char_u *)ESP_SSH_DIR "/id_ecdsa") : local_arg(&argvars[0]);
    char_u *comment = argvars[0].v_type != VAR_UNKNOWN && argvars[1].v_type != VAR_UNKNOWN
                    ? str_arg(&argvars[1]) : vim_strsave((char_u *)"esp-vim");
    if (file != NULL && comment != NULL) {
        char err[256], pub[256];
        if (esp_ssh_keygen((char *)file, (char *)comment, pub, sizeof pub, err, sizeof err) == 0)
            rettv->vval.v_string = vim_strsave((char_u *)pub);
        else
            REPORT("esp_ssh_keygen");
    }
    vim_free(file);
    vim_free(comment);
}
