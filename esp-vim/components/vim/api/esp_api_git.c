/*
 * esp_git_*(): git for Vim script, over libgit2 (docs/PLAN.md, Phase 8).
 * Behind the :EspGit* commands (autoload/esp/git.vim).
 *
 * Each takes a {path} inside a working tree (the repository is found from
 * there, as git does) and returns plain data; formatting is Vim script's job.
 * File arguments are relative to Vim's current directory, or absolute, and
 * must be inside the working tree.
 *
 * libgit2 runs here, on the Vim task, and nowhere else: it is built without
 * threads. Network operations show their progress on the command line and
 * stop at CTRL-C (the callbacks return an error; libgit2 cleans up). Errors
 * are Vim errors "esp_git_<fn>(): <why>"; the host-key ones carry esp_ssh's
 * words ("unknown host key", "host key CHANGED") so the Vim side can ask.
 */

#include "vim.h"
#include "esp_vim_api.h"
#include "esp_git.h"
#include "esp_timer.h"

#include <time.h>

#define PROGRESS_US 250000      /* at most four progress updates a second */

/* ---------------------------------------------------------------- common */

typedef struct {
    esp_git_auth_t auth;        /* first: esp_git's callbacks cast the payload */
    const char *fn;
    int64_t last;
    char rejected[200];         /* a push the server refused */
} op_t;

static bool ensure_init(const char *fn)
{
    if (esp_git_init() == ESP_OK)
        return true;
    semsg("%s(): libgit2 did not start", fn);
    return false;
}

/* Report the failure of {fn}: ours (the auth/host-key message), an interrupt,
 * or libgit2's own. */
static void report(const char *fn, op_t *op)
{
    const git_error *e = git_error_last();
    if (got_int)
        semsg("%s(): interrupted", fn);
    else if (op != NULL && op->auth.err[0])
        semsg("%s(): %s", fn, op->auth.err);
    else if (op != NULL && op->rejected[0])
        semsg("%s(): %s", fn, op->rejected);
    else
        semsg("%s(): %s", fn, e && e->message ? e->message : "failed");
}

/* A string argument, or NULL (an error is given). */
static char_u *str_arg(typval_T *tv)
{
    return tv_get_string_chk(tv);
}

/* The repository {tv} (a path in its working tree) is in. */
static git_repository *open_repo(typval_T *tv, const char *fn)
{
    char_u *s = str_arg(tv);
    git_repository *repo = NULL;
    if (s == NULL || !ensure_init(fn))
        return NULL;
    char_u *full = FullName_save(*s ? s : (char_u *)".", TRUE);
    if (full == NULL)
        return NULL;
    if (git_repository_open_ext(&repo, (char *)full, 0, NULL) < 0) {
        semsg("%s(): not in a git repository: %s", fn, full);
        repo = NULL;
    }
    vim_free(full);
    return repo;
}

/* The working tree without its trailing slash (allocated). */
static char_u *workdir(git_repository *repo)
{
    const char *w = git_repository_workdir(repo);
    char_u *s = vim_strsave((char_u *)(w ? w : git_repository_path(repo)));
    size_t n = s ? STRLEN(s) : 0;
    if (n > 1 && s[n - 1] == '/')
        s[n - 1] = NUL;
    return s;
}

/*
 * File arguments -> paths relative to the working tree, as libgit2 wants.
 * {tv}: a List of strings, a string, or missing (none). Returns FAIL (with an
 * error) for a file outside the tree.
 */
static int paths_arg(typval_T *tv, git_repository *repo, git_strarray *out, const char *fn)
{
    out->strings = NULL;
    out->count = 0;
    if (tv->v_type == VAR_UNKNOWN)
        return OK;
    list_T *l = NULL;
    size_t n = 1;
    if (tv->v_type == VAR_LIST) {
        l = tv->vval.v_list;
        n = (size_t)list_len(l);
    }
    if (n == 0)
        return OK;
    out->strings = ALLOC_CLEAR_MULT(char *, n);
    if (out->strings == NULL)
        return FAIL;
    const char *wd = git_repository_workdir(repo);
    size_t wlen = wd ? strlen(wd) : 0;
    listitem_T *li = l ? l->lv_first : NULL;
    for (size_t i = 0; i < n; i++) {
        char_u *s = tv_get_string_chk(l ? &li->li_tv : tv);
        if (l)
            li = li->li_next;
        if (s == NULL)
            return FAIL;
        char_u *full = FullName_save(s, FALSE);
        if (full == NULL)
            return FAIL;
        if (wd == NULL || STRNCMP(full, wd, wlen) != 0) {
            if (wd != NULL && STRLEN(full) + 1 == wlen && STRNCMP(full, wd, wlen - 1) == 0) {
                STRCPY(full, ".");                  /* the root itself */
            } else {
                semsg("%s(): not in the working tree: %s", fn, full);
                vim_free(full);
                return FAIL;
            }
        } else {
            mch_memmove(full, full + wlen, STRLEN(full + wlen) + 1);
            if (*full == NUL)
                STRCPY(full, ".");
        }
        out->strings[out->count++] = (char *)full;
    }
    return OK;
}

static void paths_free(git_strarray *a)
{
    for (size_t i = 0; i < a->count; i++)
        vim_free(a->strings[i]);
    vim_free(a->strings);
    a->strings = NULL;
    a->count = 0;
}

static void ret_string(typval_T *rettv, const char *s)
{
    rettv->v_type = VAR_STRING;
    rettv->vval.v_string = vim_strsave((char_u *)s);
}

/* The current branch's short name; "" when HEAD is detached. Unborn: the
 * branch HEAD names. */
static void branch_name(git_repository *repo, char *out, size_t n)
{
    git_reference *head = NULL;
    out[0] = '\0';
    if (git_reference_lookup(&head, repo, "HEAD") < 0)
        return;
    if (git_reference_type(head) == GIT_REFERENCE_SYMBOLIC) {
        const char *t = git_reference_symbolic_target(head);
        if (t && strncmp(t, "refs/heads/", 11) == 0)
            snprintf(out, n, "%s", t + 11);
    }
    git_reference_free(head);
}

/* HEAD's commit, or NULL on an unborn branch. */
static git_commit *head_commit(git_repository *repo)
{
    git_oid id;
    git_commit *c = NULL;
    if (git_reference_name_to_id(&id, repo, "HEAD") < 0 || git_commit_lookup(&c, repo, &id) < 0)
        return NULL;
    return c;
}

/* ------------------------------------------------------------- progress */

static void show(const char *s)
{
    if (msg_silent != 0)
        return;
    msg_start();
    msg_puts((char *)s);
    msg_clr_eos();
    out_flush();
}

/* Time for an update (and a CTRL-C check)? */
static bool tick(op_t *op)
{
    int64_t now = esp_timer_get_time();
    if (now - op->last < PROGRESS_US)
        return false;
    op->last = now;
    return true;
}

static bool stop(void)
{
    ui_breakcheck();
    return got_int;
}

static int transfer_cb(const git_indexer_progress *p, void *payload)
{
    op_t *op = payload;
    if (!tick(op))
        return 0;
    char buf[100];
    if (p->total_objects && p->received_objects < p->total_objects)
        vim_snprintf(buf, sizeof buf, "Receiving objects: %u%% (%u/%u), %u KB",
                     p->received_objects * 100 / p->total_objects, p->received_objects,
                     p->total_objects, (unsigned)(p->received_bytes / 1024));
    else if (p->total_deltas)
        vim_snprintf(buf, sizeof buf, "Resolving deltas: %u%% (%u/%u)",
                     p->indexed_deltas * 100 / p->total_deltas, p->indexed_deltas, p->total_deltas);
    else
        vim_snprintf(buf, sizeof buf, "%s...", op->fn);
    show(buf);
    return stop() ? -1 : 0;
}

static int sideband_cb(const char *str, int len, void *payload)
{
    (void)str; (void)len;
    op_t *op = payload;
    return tick(op) && stop() ? -1 : 0;
}

static int push_transfer_cb(unsigned int current, unsigned int total, size_t bytes, void *payload)
{
    op_t *op = payload;
    if (!tick(op))
        return 0;
    char buf[100];
    vim_snprintf(buf, sizeof buf, "Writing objects: %u%% (%u/%u), %u KB",
                 total ? current * 100 / total : 100, current, total, (unsigned)(bytes / 1024));
    show(buf);
    return stop() ? -1 : 0;
}

static int push_ref_cb(const char *ref, const char *status, void *payload)
{
    op_t *op = payload;
    if (status != NULL)
        vim_snprintf(op->rejected, sizeof op->rejected,
                     "the server refused %s: %s (pull first?)", ref, status);
    return 0;
}

static void checkout_progress_cb(const char *path, size_t done, size_t total, void *payload)
{
    op_t *op = payload;
    (void)path;
    if (!tick(op))
        return;
    char buf[80];
    vim_snprintf(buf, sizeof buf, "Checking out files: %u%% (%u/%u)",
                 (unsigned)(total ? done * 100 / total : 100), (unsigned)done, (unsigned)total);
    show(buf);
    ui_breakcheck();        /* libgit2 can't stop a checkout from here */
}

static void op_begin(op_t *op, const char *fn, const char *url)
{
    memset(op, 0, sizeof *op);
    esp_git_auth_begin(&op->auth, url);
    op->fn = fn;
    op->last = esp_timer_get_time() - PROGRESS_US;
    got_int = FALSE;
}

static void callbacks(git_remote_callbacks *cb, op_t *op)
{
    cb->credentials = esp_git_credentials_cb;
    cb->certificate_check = esp_git_certificate_cb;
    cb->transfer_progress = transfer_cb;
    cb->sideband_progress = sideband_cb;
    cb->push_transfer_progress = push_transfer_cb;
    cb->push_update_reference = push_ref_cb;
    cb->payload = op;
}

static void checkout_opts(git_checkout_options *co, op_t *op, unsigned int strategy)
{
    co->checkout_strategy = strategy;
    co->progress_cb = checkout_progress_cb;
    co->progress_payload = op;
}

/* The remote to use: {tv} if given, else the branch's upstream remote, else
 * "origin". */
static void remote_name(git_repository *repo, typval_T *tv, char *out, size_t n)
{
    char_u *s = tv->v_type == VAR_UNKNOWN ? NULL : tv_get_string_chk(tv);
    char branch[128];
    git_buf buf = GIT_BUF_INIT;
    if (s != NULL && *s) {
        vim_snprintf(out, n, "%s", s);
        return;
    }
    vim_snprintf(out, n, "origin");
    branch_name(repo, branch, sizeof branch);
    if (branch[0]) {
        char ref[160];
        vim_snprintf(ref, sizeof ref, "refs/heads/%s", branch);
        if (git_branch_upstream_remote(&buf, repo, ref) == 0)
            vim_snprintf(out, n, "%s", buf.ptr);
        git_buf_dispose(&buf);
    }
}

/* ----------------------------------------------------------------- local */

/* esp_git_init({path}) -> the new working tree. The first branch is "main"
 * unless init.defaultBranch says otherwise. */
void f_esp_git_init(typval_T *argvars, typval_T *rettv)
{
    char_u *s = str_arg(&argvars[0]);
    if (s == NULL || !ensure_init("esp_git_init"))
        return;
    char_u *full = FullName_save(s, TRUE);
    git_repository *repo = NULL;
    git_config *cfg = NULL;
    git_buf def = GIT_BUF_INIT;
    git_repository_init_options o = GIT_REPOSITORY_INIT_OPTIONS_INIT;
    o.flags = GIT_REPOSITORY_INIT_MKPATH;
    if (git_config_open_default(&cfg) < 0 || git_config_get_string_buf(&def, cfg, "init.defaultBranch") < 0)
        o.initial_head = "main";
    if (full != NULL && git_repository_init_ext(&repo, (char *)full, &o) == 0) {
        char_u *wd = workdir(repo);
        ret_string(rettv, wd ? (char *)wd : "");
        vim_free(wd);
    } else {
        report("esp_git_init", NULL);
    }
    git_buf_dispose(&def);
    git_config_free(cfg);
    git_repository_free(repo);
    vim_free(full);
}

static char status_char(unsigned int st, bool index)
{
    if (index) {
        if (st & GIT_STATUS_INDEX_NEW)        return 'A';
        if (st & GIT_STATUS_INDEX_MODIFIED)   return 'M';
        if (st & GIT_STATUS_INDEX_DELETED)    return 'D';
        if (st & GIT_STATUS_INDEX_RENAMED)    return 'R';
        if (st & GIT_STATUS_INDEX_TYPECHANGE) return 'T';
        return ' ';
    }
    if (st & GIT_STATUS_WT_NEW)        return '?';
    if (st & GIT_STATUS_WT_MODIFIED)   return 'M';
    if (st & GIT_STATUS_WT_DELETED)    return 'D';
    if (st & GIT_STATUS_WT_RENAMED)    return 'R';
    if (st & GIT_STATUS_WT_TYPECHANGE) return 'T';
    return ' ';
}

static const char *state_name(int state)
{
    switch (state) {
    case GIT_REPOSITORY_STATE_MERGE:          return "merge";
    case GIT_REPOSITORY_STATE_REVERT:
    case GIT_REPOSITORY_STATE_REVERT_SEQUENCE: return "revert";
    case GIT_REPOSITORY_STATE_CHERRYPICK:
    case GIT_REPOSITORY_STATE_CHERRYPICK_SEQUENCE: return "cherry-pick";
    case GIT_REPOSITORY_STATE_REBASE:
    case GIT_REPOSITORY_STATE_REBASE_INTERACTIVE:
    case GIT_REPOSITORY_STATE_REBASE_MERGE:   return "rebase";
    case GIT_REPOSITORY_STATE_NONE:           return "";
    default:                                  return "other";
    }
}

/* esp_git_status({path}) -> Dict: root, branch ("" detached), head (id, ""
 * unborn), upstream, ahead, behind, state ("", "merge", ...), files: a List
 * of {path, x, y} as in git status --short (x: index, y: working tree; "??"
 * untracked, "UU" conflicted). */
void f_esp_git_status(typval_T *argvars, typval_T *rettv)
{
    if (rettv_dict_alloc(rettv) == FAIL)
        return;
    git_repository *repo = open_repo(&argvars[0], "esp_git_status");
    if (repo == NULL)
        return;
    dict_T *d = rettv->vval.v_dict;
    git_status_list *sl = NULL;
    git_reference *head = NULL, *up = NULL;
    char branch[128];

    char_u *wd = workdir(repo);
    dict_add_string(d, "root", wd);
    vim_free(wd);
    branch_name(repo, branch, sizeof branch);
    dict_add_string(d, "branch", (char_u *)branch);
    git_oid hid;
    dict_add_string(d, "head", (char_u *)(git_reference_name_to_id(&hid, repo, "HEAD") == 0
                                          ? git_oid_tostr_s(&hid) : ""));
    dict_add_string(d, "state", (char_u *)state_name(git_repository_state(repo)));
    size_t ahead = 0, behind = 0;
    if (git_repository_head(&head, repo) == 0 && git_branch_upstream(&up, head) == 0) {
        dict_add_string(d, "upstream", (char_u *)git_reference_shorthand(up));
        const git_oid *a = git_reference_target(head), *b = git_reference_target(up);
        if (a && b)
            git_graph_ahead_behind(&ahead, &behind, repo, a, b);
    } else {
        dict_add_string(d, "upstream", (char_u *)"");
    }
    dict_add_number(d, "ahead", (varnumber_T)ahead);
    dict_add_number(d, "behind", (varnumber_T)behind);

    list_T *files = list_alloc();
    if (files == NULL)
        goto out;
    dict_add_list(d, "files", files);
    git_status_options so = GIT_STATUS_OPTIONS_INIT;
    so.show = GIT_STATUS_SHOW_INDEX_AND_WORKDIR;
    so.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED | GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS
             | GIT_STATUS_OPT_RENAMES_HEAD_TO_INDEX | GIT_STATUS_OPT_SORT_CASE_SENSITIVELY;
    if (git_status_list_new(&sl, repo, &so) < 0) {
        report("esp_git_status", NULL);
        goto out;
    }
    for (size_t i = 0; i < git_status_list_entrycount(sl); i++) {
        const git_status_entry *e = git_status_byindex(sl, i);
        const git_diff_delta *dd = e->index_to_workdir ? e->index_to_workdir : e->head_to_index;
        if (e->status == GIT_STATUS_CURRENT || e->status & GIT_STATUS_IGNORED || dd == NULL)
            continue;
        dict_T *f = dict_alloc();
        if (f == NULL)
            break;
        char x[2] = { status_char(e->status, true), 0 }, y[2] = { status_char(e->status, false), 0 };
        if (e->status & GIT_STATUS_CONFLICTED)
            x[0] = y[0] = 'U';
        else if (e->status & GIT_STATUS_WT_NEW)
            x[0] = '?';
        dict_add_string(f, "path", (char_u *)dd->new_file.path);
        dict_add_string(f, "x", (char_u *)x);
        dict_add_string(f, "y", (char_u *)y);
        list_append_dict(files, f);
    }
out:
    git_status_list_free(sl);
    git_reference_free(up);
    git_reference_free(head);
    git_repository_free(repo);
}

/* esp_git_add({path} [, {files}]): stage {files} (a List or a name; deleted
 * files are staged as deleted); all changes when there are none. */
void f_esp_git_add(typval_T *argvars, typval_T *rettv)
{
    git_repository *repo = open_repo(&argvars[0], "esp_git_add");
    git_index *index = NULL;
    git_strarray paths;
    int rc = -1;
    if (repo == NULL || paths_arg(&argvars[1], repo, &paths, "esp_git_add") == FAIL) {
        git_repository_free(repo);
        return;
    }
    if (git_repository_index(&index, repo) < 0)
        goto out;
    if (paths.count == 0) {
        char *all[] = { "*" };
        git_strarray star = { all, 1 };
        rc = git_index_add_all(index, &star, GIT_INDEX_ADD_DEFAULT, NULL, NULL);
        if (rc == 0)
            rc = git_index_update_all(index, &star, NULL, NULL);    /* deletions */
    } else {
        /* Existing files are added; the rest, and directories, go through
         * the pathspec calls, which also stage deletions. */
        rc = git_index_add_all(index, &paths, GIT_INDEX_ADD_DEFAULT, NULL, NULL);
        if (rc == 0)
            rc = git_index_update_all(index, &paths, NULL, NULL);
    }
    if (rc == 0)
        rc = git_index_write(index);
out:
    if (rc < 0)
        report("esp_git_add", NULL);
    else
        rettv->vval.v_number = TRUE;
    paths_free(&paths);
    git_index_free(index);
    git_repository_free(repo);
}

/* esp_git_reset({path}, {files}): unstage {files} (the index takes HEAD's
 * version again; the working tree is untouched). */
void f_esp_git_reset(typval_T *argvars, typval_T *rettv)
{
    git_repository *repo = open_repo(&argvars[0], "esp_git_reset");
    git_strarray paths;
    if (repo == NULL || paths_arg(&argvars[1], repo, &paths, "esp_git_reset") == FAIL) {
        git_repository_free(repo);
        return;
    }
    git_commit *head = head_commit(repo);
    if (git_reset_default(repo, (git_object *)head, &paths) < 0)
        report("esp_git_reset", NULL);
    else
        rettv->vval.v_number = TRUE;
    git_commit_free(head);
    paths_free(&paths);
    git_repository_free(repo);
}

/* esp_git_restore({path}, {files}): throw away the working tree's changes to
 * {files}: they take the index's version. */
void f_esp_git_restore(typval_T *argvars, typval_T *rettv)
{
    git_repository *repo = open_repo(&argvars[0], "esp_git_restore");
    git_strarray paths;
    if (repo == NULL || paths_arg(&argvars[1], repo, &paths, "esp_git_restore") == FAIL) {
        git_repository_free(repo);
        return;
    }
    git_checkout_options co = GIT_CHECKOUT_OPTIONS_INIT;
    co.checkout_strategy = GIT_CHECKOUT_FORCE | GIT_CHECKOUT_DISABLE_PATHSPEC_MATCH;
    co.paths = paths;
    if (paths.count == 0)
        emsg("esp_git_restore(): which files?");
    else if (git_checkout_index(repo, NULL, &co) < 0)
        report("esp_git_restore", NULL);
    else
        rettv->vval.v_number = TRUE;
    paths_free(&paths);
    git_repository_free(repo);
}

static int mergehead_cb(const git_oid *id, void *payload)
{
    git_oid *ids = payload;
    for (int i = 1; i < 8; i++)
        if (git_oid_is_zero(&ids[i])) {
            git_oid_cpy(&ids[i], id);
            return 0;
        }
    return 0;
}

/* esp_git_commit({path}, {message}) -> the new commit's id. Commits the
 * index; finishes a merge (its MERGE_HEAD becomes a parent). Comment lines
 * ("#") are dropped from {message}, as git commit does. */
void f_esp_git_commit(typval_T *argvars, typval_T *rettv)
{
    git_repository *repo = open_repo(&argvars[0], "esp_git_commit");
    char_u *msg = str_arg(&argvars[1]);
    git_signature *sig = NULL;
    git_index *index = NULL;
    git_tree *tree = NULL;
    git_buf text = GIT_BUF_INIT;
    git_commit *parents[8] = { 0 };
    git_oid ids[8], tid, out;
    size_t np = 0;
    if (repo == NULL || msg == NULL)
        goto done;
    if (git_message_prettify(&text, (char *)msg, 1, '#') < 0 || text.size == 0) {
        emsg("esp_git_commit(): the message is empty");
        goto done;
    }
    if (git_signature_default(&sig, repo) < 0) {
        emsg("esp_git_commit(): who is committing? :EspGitConfig --global user.name "
             "'Your Name' and user.email you@example.com");
        goto done;
    }
    if (git_repository_index(&index, repo) < 0)
        goto fail;
    if (git_index_has_conflicts(index)) {
        emsg("esp_git_commit(): there are conflicts: fix the files, :EspGitAdd them, then commit");
        goto done;
    }
    if (git_index_write_tree(&tid, index) < 0 || git_tree_lookup(&tree, repo, &tid) < 0)
        goto fail;
    memset(ids, 0, sizeof ids);
    if ((parents[0] = head_commit(repo)) != NULL)
        np = 1;
    if (git_repository_state(repo) == GIT_REPOSITORY_STATE_MERGE) {
        git_repository_mergehead_foreach(repo, mergehead_cb, ids);
        for (int i = 1; i < 8 && !git_oid_is_zero(&ids[i]); i++)
            if (git_commit_lookup(&parents[np], repo, &ids[i]) == 0)
                np++;
    }
    if (np == 1 && git_oid_equal(git_commit_tree_id(parents[0]), &tid)) {
        emsg("esp_git_commit(): nothing to commit (:EspGitAdd the changes first)");
        goto done;
    }
    if (git_commit_create(&out, repo, "HEAD", sig, sig, NULL, text.ptr, tree, np,
                          (const git_commit **)parents) < 0)
        goto fail;
    if (np > 1)
        git_repository_state_cleanup(repo);
    ret_string(rettv, git_oid_tostr_s(&out));
    goto done;
fail:
    report("esp_git_commit", NULL);
done:
    for (size_t i = 0; i < 8; i++)
        git_commit_free(parents[i]);
    git_buf_dispose(&text);
    git_tree_free(tree);
    git_index_free(index);
    git_signature_free(sig);
    git_repository_free(repo);
}

/* esp_git_log({path} [, {max}]) -> List of {id, author, email, time,
 * summary, parents}, newest first; {max} 100 by default. */
void f_esp_git_log(typval_T *argvars, typval_T *rettv)
{
    if (rettv_list_alloc(rettv) == FAIL)
        return;
    git_repository *repo = open_repo(&argvars[0], "esp_git_log");
    if (repo == NULL)
        return;
    long max = argvars[1].v_type == VAR_UNKNOWN ? 100 : (long)tv_get_number(&argvars[1]);
    git_revwalk *walk = NULL;
    git_oid id;
    if (git_revwalk_new(&walk, repo) < 0) {
        report("esp_git_log", NULL);
    } else if (git_revwalk_push_head(walk) == 0) {      /* else unborn: no history */
        /* Children before their parents, then by time, as git log shows it. */
        git_revwalk_sorting(walk, GIT_SORT_TOPOLOGICAL | GIT_SORT_TIME);
        for (long n = 0; n < max && git_revwalk_next(&id, walk) == 0; n++) {
            git_commit *c = NULL;
            if (git_commit_lookup(&c, repo, &id) < 0)
                break;
            dict_T *d = dict_alloc();
            if (d == NULL) {
                git_commit_free(c);
                break;
            }
            const git_signature *a = git_commit_author(c);
            dict_add_string(d, "id", (char_u *)git_oid_tostr_s(&id));
            dict_add_string(d, "author", (char_u *)a->name);
            dict_add_string(d, "email", (char_u *)a->email);
            dict_add_number(d, "time", (varnumber_T)a->when.time);
            const char *sum = git_commit_summary(c);
            dict_add_string(d, "summary", (char_u *)(sum ? sum : ""));
            dict_add_number(d, "parents", (varnumber_T)git_commit_parentcount(c));
            list_append_dict(rettv->vval.v_list, d);
            git_commit_free(c);
        }
    }
    git_revwalk_free(walk);
    git_repository_free(repo);
}

/* A diff as lines of patch text, appended to {l}. */
static int diff_lines(git_diff *diff, list_T *l)
{
    git_buf buf = GIT_BUF_INIT;
    if (git_diff_to_buf(&buf, diff, GIT_DIFF_FORMAT_PATCH) < 0)
        return -1;
    char *p = buf.ptr, *end = buf.ptr + buf.size;
    while (p < end) {
        char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t len = nl ? (size_t)(nl - p) : (size_t)(end - p);
        list_append_string(l, (char_u *)p, (int)len);
        p += len + 1;
    }
    git_buf_dispose(&buf);
    return 0;
}

/* esp_git_diff({path} [, {opts}]) -> List of lines (a patch). {opts}:
 * "cached": the index against HEAD (default: the working tree against the
 * index); "files": limit to these. */
void f_esp_git_diff(typval_T *argvars, typval_T *rettv)
{
    if (rettv_list_alloc(rettv) == FAIL)
        return;
    git_repository *repo = open_repo(&argvars[0], "esp_git_diff");
    if (repo == NULL)
        return;
    bool cached = false;
    git_strarray paths = { NULL, 0 };
    if (argvars[1].v_type == VAR_DICT && argvars[1].vval.v_dict != NULL) {
        dict_T *o = argvars[1].vval.v_dict;
        cached = dict_get_bool(o, "cached", FALSE);
        dictitem_T *fi = dict_find(o, (char_u *)"files", -1);
        if (fi != NULL && paths_arg(&fi->di_tv, repo, &paths, "esp_git_diff") == FAIL) {
            git_repository_free(repo);
            return;
        }
    }
    git_diff_options dopt = GIT_DIFF_OPTIONS_INIT;
    dopt.pathspec = paths;
    git_diff *diff = NULL;
    git_commit *head = NULL;
    git_tree *tree = NULL;
    int rc;
    if (cached) {
        if ((head = head_commit(repo)) != NULL)
            git_commit_tree(&tree, head);
        rc = git_diff_tree_to_index(&diff, repo, tree, NULL, &dopt);
    } else {
        rc = git_diff_index_to_workdir(&diff, repo, NULL, &dopt);
    }
    if (rc < 0 || diff_lines(diff, rettv->vval.v_list) < 0)
        report("esp_git_diff", NULL);
    git_diff_free(diff);
    git_tree_free(tree);
    git_commit_free(head);
    paths_free(&paths);
    git_repository_free(repo);
}

/* esp_git_show({path}, {rev}) -> List of lines: the commit's header and
 * message, then its patch (against its first parent). */
void f_esp_git_show(typval_T *argvars, typval_T *rettv)
{
    if (rettv_list_alloc(rettv) == FAIL)
        return;
    git_repository *repo = open_repo(&argvars[0], "esp_git_show");
    char_u *rev = str_arg(&argvars[1]);
    if (repo == NULL || rev == NULL) {
        git_repository_free(repo);
        return;
    }
    list_T *l = rettv->vval.v_list;
    git_object *obj = NULL;
    git_commit *c = NULL, *parent = NULL;
    git_tree *t = NULL, *pt = NULL;
    git_diff *diff = NULL;
    if (git_revparse_single(&obj, repo, (char *)rev) < 0
            || git_object_peel((git_object **)&c, obj, GIT_OBJECT_COMMIT) < 0)
        goto fail;
    char line[300];
    const git_signature *a = git_commit_author(c);
    vim_snprintf(line, sizeof line, "commit %s", git_oid_tostr_s(git_commit_id(c)));
    list_append_string(l, (char_u *)line, -1);
    if (git_commit_parentcount(c) > 1) {
        char p1[12], p2[12];
        git_oid_tostr(p1, 8, git_commit_parent_id(c, 0));
        git_oid_tostr(p2, 8, git_commit_parent_id(c, 1));
        vim_snprintf(line, sizeof line, "Merge: %s %s", p1, p2);
        list_append_string(l, (char_u *)line, -1);
    }
    vim_snprintf(line, sizeof line, "Author: %s <%s>", a->name, a->email);
    list_append_string(l, (char_u *)line, -1);
    time_t when = (time_t)a->when.time;
    struct tm tm;
    char date[64];
    localtime_r(&when, &tm);
    strftime(date, sizeof date, "%a %b %e %H:%M:%S %Y %z", &tm);
    vim_snprintf(line, sizeof line, "Date:   %s", date);
    list_append_string(l, (char_u *)line, -1);
    list_append_string(l, (char_u *)"", 0);
    const char *m = git_commit_message(c);
    while (m && *m) {
        const char *nl = strchr(m, '\n');
        size_t len = nl ? (size_t)(nl - m) : strlen(m);
        vim_snprintf(line, sizeof line, "    %.*s", (int)len, m);
        list_append_string(l, (char_u *)line, -1);
        m += len + (nl ? 1 : 0);
    }
    list_append_string(l, (char_u *)"", 0);
    if (git_commit_tree(&t, c) < 0)
        goto fail;
    if (git_commit_parentcount(c) > 0 && (git_commit_parent(&parent, c, 0) < 0
                                          || git_commit_tree(&pt, parent) < 0))
        goto fail;
    if (git_diff_tree_to_tree(&diff, repo, pt, t, NULL) < 0 || diff_lines(diff, l) < 0)
        goto fail;
    goto done;
fail:
    report("esp_git_show", NULL);
done:
    git_diff_free(diff);
    git_tree_free(pt);
    git_tree_free(t);
    git_commit_free(parent);
    git_commit_free(c);
    git_object_free(obj);
    git_repository_free(repo);
}

/* esp_git_branch({path}) -> List of {name, current, upstream, remote}: the
 * local branches, then the remote-tracking ones (remote: TRUE).
 * esp_git_branch({path}, {name}): make a branch at HEAD.
 * esp_git_branch({path}, {name}, {'delete': 1}): delete one. */
void f_esp_git_branch(typval_T *argvars, typval_T *rettv)
{
    git_repository *repo = open_repo(&argvars[0], "esp_git_branch");
    if (repo == NULL)
        return;
    if (argvars[1].v_type == VAR_UNKNOWN) {
        if (rettv_list_alloc(rettv) == FAIL)
            goto out;
        git_branch_iterator *it = NULL;
        git_reference *ref = NULL;
        git_branch_t type;
        if (git_branch_iterator_new(&it, repo, GIT_BRANCH_ALL) < 0) {
            report("esp_git_branch", NULL);
            goto out;
        }
        while (git_branch_next(&ref, &type, it) == 0) {
            dict_T *d = dict_alloc();
            if (d != NULL) {
                const char *name = NULL;
                git_reference *up = NULL;
                git_branch_name(&name, ref);
                dict_add_string(d, "name", (char_u *)(name ? name : ""));
                dict_add_bool(d, "current", git_branch_is_head(ref) == 1);
                dict_add_bool(d, "remote", type == GIT_BRANCH_REMOTE);
                dict_add_string(d, "upstream", (char_u *)(type == GIT_BRANCH_LOCAL
                        && git_branch_upstream(&up, ref) == 0 ? git_reference_shorthand(up) : ""));
                git_reference_free(up);
                list_append_dict(rettv->vval.v_list, d);
            }
            git_reference_free(ref);
        }
        git_branch_iterator_free(it);
        goto out;
    }
    char_u *name = str_arg(&argvars[1]);
    if (name == NULL)
        goto out;
    bool del = argvars[2].v_type == VAR_DICT && argvars[2].vval.v_dict != NULL
               && dict_get_bool(argvars[2].vval.v_dict, "delete", FALSE);
    git_reference *ref = NULL;
    int rc;
    if (del) {
        rc = git_branch_lookup(&ref, repo, (char *)name, GIT_BRANCH_LOCAL);
        if (rc == 0)
            rc = git_branch_delete(ref);
    } else {
        git_commit *head = head_commit(repo);
        if (head == NULL) {
            emsg("esp_git_branch(): no commits yet to start a branch at");
            goto out;
        }
        rc = git_branch_create(&ref, repo, (char *)name, head, 0);
        git_commit_free(head);
    }
    if (rc < 0)
        report("esp_git_branch", NULL);
    else
        rettv->vval.v_number = TRUE;
    git_reference_free(ref);
out:
    git_repository_free(repo);
}

/* esp_git_checkout({path}, {ref}): switch to a local branch; to a new local
 * branch tracking a remote one of that name (origin/{ref}); or detach at a
 * commit or tag. Refuses to overwrite local changes. */
void f_esp_git_checkout(typval_T *argvars, typval_T *rettv)
{
    git_repository *repo = open_repo(&argvars[0], "esp_git_checkout");
    char_u *name = str_arg(&argvars[1]);
    git_reference *ref = NULL, *remote = NULL;
    git_object *target = NULL;
    char full[200];
    op_t op;
    int rc = -1;
    if (repo == NULL || name == NULL)
        goto out;
    op_begin(&op, "esp_git_checkout", NULL);
    git_checkout_options co = GIT_CHECKOUT_OPTIONS_INIT;
    checkout_opts(&co, &op, GIT_CHECKOUT_SAFE);

    if (git_branch_lookup(&ref, repo, (char *)name, GIT_BRANCH_LOCAL) != 0) {
        /* A remote branch of that name: a local branch that tracks it. */
        char rname[200];
        vim_snprintf(rname, sizeof rname, "origin/%s", name);
        if (git_branch_lookup(&remote, repo, rname, GIT_BRANCH_REMOTE) == 0) {
            git_commit *c = NULL;
            if (git_commit_lookup(&c, repo, git_reference_target(remote)) == 0
                    && git_branch_create(&ref, repo, (char *)name, c, 0) == 0)
                git_branch_set_upstream(ref, rname);
            git_commit_free(c);
        }
    }
    if (ref != NULL) {
        rc = git_reference_peel(&target, ref, GIT_OBJECT_COMMIT);
        if (rc == 0)
            rc = git_checkout_tree(repo, target, &co);
        vim_snprintf(full, sizeof full, "%s", git_reference_name(ref));
        if (rc == 0)
            rc = git_repository_set_head(repo, full);
    } else {
        rc = git_revparse_single(&target, repo, (char *)name);
        if (rc == 0) {
            git_object *c = NULL;
            rc = git_object_peel(&c, target, GIT_OBJECT_COMMIT);
            if (rc == 0)
                rc = git_checkout_tree(repo, c, &co);
            if (rc == 0)
                rc = git_repository_set_head_detached(repo, git_object_id(c));
            git_object_free(c);
        }
    }
    if (rc < 0)
        report("esp_git_checkout", NULL);
    else
        rettv->vval.v_number = TRUE;
out:
    git_object_free(target);
    git_reference_free(remote);
    git_reference_free(ref);
    git_repository_free(repo);
}

/* esp_git_config({path}, {key} [, {value}]) -> the value ("" when unset).
 * {path} "" is the device-wide config, /fat/.gitconfig (HOME is /fat);
 * otherwise the repository's, which also reads the device-wide one. A
 * {value} sets it there; "" removes it. */
void f_esp_git_config(typval_T *argvars, typval_T *rettv)
{
    char_u *path = str_arg(&argvars[0]);
    char_u *key = str_arg(&argvars[1]);
    if (path == NULL || key == NULL || !ensure_init("esp_git_config"))
        return;
    git_repository *repo = NULL;
    git_config *cfg = NULL, *snap = NULL;
    git_buf buf = GIT_BUF_INIT;
    int rc;
    if (*path == NUL) {
        git_buf gpath = GIT_BUF_INIT;
        if (git_config_find_global(&gpath) < 0) {
            /* No file yet: the one libgit2 would look for. */
            char_u *home = (char_u *)getenv("HOME");
            git_buf_dispose(&gpath);
            char f[200];
            vim_snprintf(f, sizeof f, "%s/.gitconfig", home ? (char *)home : "/fat");
            rc = git_config_open_ondisk(&cfg, f);
        } else {
            rc = git_config_open_ondisk(&cfg, gpath.ptr);
        }
        git_buf_dispose(&gpath);
    } else {
        repo = open_repo(&argvars[0], "esp_git_config");
        if (repo == NULL)
            return;
        rc = git_repository_config(&cfg, repo);
    }
    if (rc < 0)
        goto fail;
    if (argvars[2].v_type != VAR_UNKNOWN) {
        char_u *val = str_arg(&argvars[2]);
        if (val == NULL)
            goto out;
        rc = *val ? git_config_set_string(cfg, (char *)key, (char *)val)
                  : git_config_delete_entry(cfg, (char *)key);
        if (rc < 0 && !(rc == GIT_ENOTFOUND && *val == NUL))
            goto fail;
        ret_string(rettv, (char *)val);
        goto out;
    }
    if (git_config_snapshot(&snap, cfg) < 0)
        goto fail;
    rc = git_config_get_string_buf(&buf, snap, (char *)key);
    ret_string(rettv, rc == 0 ? buf.ptr : "");
    goto out;
fail:
    report("esp_git_config", NULL);
out:
    git_buf_dispose(&buf);
    git_config_free(snap);
    git_config_free(cfg);
    git_repository_free(repo);
}

/* ---------------------------------------------------------------- remote */

/* esp_git_remote({path}) -> List of {name, url}.
 * esp_git_remote({path}, {name}, {url}): add it, or change its URL; {url}
 * "" removes it. */
void f_esp_git_remote(typval_T *argvars, typval_T *rettv)
{
    git_repository *repo = open_repo(&argvars[0], "esp_git_remote");
    if (repo == NULL)
        return;
    if (argvars[1].v_type == VAR_UNKNOWN) {
        git_strarray names = { NULL, 0 };
        if (rettv_list_alloc(rettv) == FAIL || git_remote_list(&names, repo) < 0)
            goto out;
        for (size_t i = 0; i < names.count; i++) {
            git_remote *r = NULL;
            dict_T *d = dict_alloc();
            if (d == NULL)
                break;
            dict_add_string(d, "name", (char_u *)names.strings[i]);
            dict_add_string(d, "url", (char_u *)(git_remote_lookup(&r, repo, names.strings[i]) == 0
                                                 && git_remote_url(r) ? git_remote_url(r) : ""));
            git_remote_free(r);
            list_append_dict(rettv->vval.v_list, d);
        }
        git_strarray_dispose(&names);
        goto out;
    }
    char_u *name = str_arg(&argvars[1]);
    char_u *url = argvars[2].v_type == VAR_UNKNOWN ? (char_u *)"" : str_arg(&argvars[2]);
    if (name == NULL || url == NULL)
        goto out;
    git_remote *r = NULL;
    int rc;
    if (*url == NUL)
        rc = git_remote_delete(repo, (char *)name);
    else if (git_remote_lookup(&r, repo, (char *)name) == 0)
        rc = git_remote_set_url(repo, (char *)name, (char *)url);
    else
        rc = git_remote_create(&r, repo, (char *)name, (char *)url);
    git_remote_free(r);
    if (rc < 0)
        report("esp_git_remote", NULL);
    else
        rettv->vval.v_number = TRUE;
out:
    git_repository_free(repo);
}

/* esp_git_clone({url}, {dir}) -> the new working tree. On failure or CTRL-C
 * libgit2 removes what it made. */
void f_esp_git_clone(typval_T *argvars, typval_T *rettv)
{
    char_u *url = str_arg(&argvars[0]);
    char_u *dir = str_arg(&argvars[1]);
    if (url == NULL || dir == NULL || !ensure_init("esp_git_clone"))
        return;
    char_u *full = FullName_save(dir, FALSE);
    if (full == NULL)
        return;
    op_t op;
    op_begin(&op, "Cloning", (char *)url);
    git_clone_options o = GIT_CLONE_OPTIONS_INIT;
    callbacks(&o.fetch_opts.callbacks, &op);
    checkout_opts(&o.checkout_opts, &op, GIT_CHECKOUT_SAFE);
    git_repository *repo = NULL;
    if (git_clone(&repo, (char *)url, (char *)full, &o) < 0) {
        report("esp_git_clone", &op);
    } else {
        char_u *wd = workdir(repo);
        ret_string(rettv, wd ? (char *)wd : "");
        vim_free(wd);
    }
    git_repository_free(repo);
    vim_free(full);
}

static void stats_dict(dict_T *d, git_remote *r)
{
    const git_indexer_progress *s = git_remote_stats(r);
    dict_add_number(d, "objects", (varnumber_T)s->received_objects);
    dict_add_number(d, "bytes", (varnumber_T)s->received_bytes);
}

static int fetch(git_repository *repo, const char *remote, op_t *op, git_remote **out)
{
    git_remote *r = NULL;
    if (git_remote_lookup(&r, repo, remote) < 0)
        return -1;
    esp_git_auth_begin(&op->auth, git_remote_url(r));
    git_fetch_options fo = GIT_FETCH_OPTIONS_INIT;
    callbacks(&fo.callbacks, op);
    if (git_remote_fetch(r, NULL, &fo, "fetch") < 0) {
        git_remote_free(r);
        return -1;
    }
    *out = r;
    return 0;
}

/* esp_git_fetch({path} [, {remote}]) -> Dict: remote, objects, bytes. */
void f_esp_git_fetch(typval_T *argvars, typval_T *rettv)
{
    if (rettv_dict_alloc(rettv) == FAIL)
        return;
    git_repository *repo = open_repo(&argvars[0], "esp_git_fetch");
    if (repo == NULL)
        return;
    char remote[64];
    git_remote *r = NULL;
    op_t op;
    remote_name(repo, &argvars[1], remote, sizeof remote);
    op_begin(&op, "Fetching", NULL);
    if (fetch(repo, remote, &op, &r) < 0) {
        report("esp_git_fetch", &op);
    } else {
        dict_add_string(rettv->vval.v_dict, "remote", (char_u *)remote);
        stats_dict(rettv->vval.v_dict, r);
    }
    git_remote_free(r);
    git_repository_free(repo);
}

/* esp_git_pull({path} [, {remote}]) -> Dict: result ("up to date",
 * "fast-forward", "merged" or "conflicts"), head, conflicts (a List of files,
 * left with conflict markers: fix them, esp_git_add(), esp_git_commit()).
 * Fetches, then merges the branch's upstream (or {remote}/<branch>). */
void f_esp_git_pull(typval_T *argvars, typval_T *rettv)
{
    if (rettv_dict_alloc(rettv) == FAIL)
        return;
    git_repository *repo = open_repo(&argvars[0], "esp_git_pull");
    if (repo == NULL)
        return;
    dict_T *d = rettv->vval.v_dict;
    char remote[64], branch[128], ref[200];
    git_remote *r = NULL;
    git_reference *head = NULL, *up = NULL, *moved = NULL;
    git_annotated_commit *theirs = NULL;
    git_commit *their_commit = NULL, *ours = NULL;
    git_index *index = NULL;
    git_tree *tree = NULL;
    git_signature *sig = NULL;
    op_t op;

    remote_name(repo, &argvars[1], remote, sizeof remote);
    branch_name(repo, branch, sizeof branch);
    if (!branch[0]) {
        emsg("esp_git_pull(): HEAD is detached: check out a branch first");
        goto out;
    }
    op_begin(&op, "Pulling", NULL);
    if (fetch(repo, remote, &op, &r) < 0)
        goto fail;
    stats_dict(d, r);

    /* Theirs: the upstream, else {remote}/{branch}. */
    vim_snprintf(ref, sizeof ref, "refs/heads/%s", branch);
    if (git_reference_lookup(&head, repo, ref) == 0 && git_branch_upstream(&up, head) == 0) {
        /* found */
    } else {
        vim_snprintf(ref, sizeof ref, "refs/remotes/%s/%s", remote, branch);
        if (git_reference_lookup(&up, repo, ref) < 0) {
            semsg("esp_git_pull(): %s has no branch %s", remote, branch);
            goto out;
        }
    }
    /* As fetched: MERGE_MSG then reads "Merge branch 'main' of <url>", as
     * git's does, rather than naming the remote-tracking ref. */
    const char *uname = git_reference_name(up);
    char prefix[100], rbranch[200];
    vim_snprintf(prefix, sizeof prefix, "refs/remotes/%s/", remote);
    vim_snprintf(rbranch, sizeof rbranch, "refs/heads/%s", strncmp(uname, prefix, strlen(prefix)) == 0
                 ? uname + strlen(prefix) : branch);
    if (git_annotated_commit_from_fetchhead(&theirs, repo, rbranch, git_remote_url(r),
                                            git_reference_target(up)) < 0
            && git_annotated_commit_from_ref(&theirs, repo, up) < 0)
        goto fail;
    git_merge_analysis_t an;
    git_merge_preference_t pref;
    const git_annotated_commit *heads[1] = { theirs };
    if (git_merge_analysis(&an, &pref, repo, heads, 1) < 0)
        goto fail;
    const git_oid *tid = git_annotated_commit_id(theirs);
    git_checkout_options co = GIT_CHECKOUT_OPTIONS_INIT;

    if (an & GIT_MERGE_ANALYSIS_UP_TO_DATE) {
        dict_add_string(d, "result", (char_u *)"up to date");
    } else if (an & (GIT_MERGE_ANALYSIS_FASTFORWARD | GIT_MERGE_ANALYSIS_UNBORN)) {
        checkout_opts(&co, &op, GIT_CHECKOUT_SAFE);
        if (git_commit_lookup(&their_commit, repo, tid) < 0
                || git_checkout_tree(repo, (git_object *)their_commit, &co) < 0)
            goto fail;
        vim_snprintf(ref, sizeof ref, "refs/heads/%s", branch);
        if (git_reference_create(&moved, repo, ref, tid, 1, "pull: fast-forward") < 0)
            goto fail;
        dict_add_string(d, "result", (char_u *)"fast-forward");
    } else {
        git_merge_options mo = GIT_MERGE_OPTIONS_INIT;
        checkout_opts(&co, &op, GIT_CHECKOUT_SAFE | GIT_CHECKOUT_ALLOW_CONFLICTS);
        if (git_merge(repo, heads, 1, &mo, &co) < 0 || git_repository_index(&index, repo) < 0)
            goto fail;
        if (git_index_has_conflicts(index)) {
            list_T *l = list_alloc();
            git_index_conflict_iterator *it = NULL;
            const git_index_entry *anc, *our, *their;
            dict_add_string(d, "result", (char_u *)"conflicts");
            if (l != NULL) {
                dict_add_list(d, "conflicts", l);
                if (git_index_conflict_iterator_new(&it, index) == 0)
                    while (git_index_conflict_next(&anc, &our, &their, it) == 0)
                        list_append_string(l, (char_u *)(our ? our->path : their ? their->path
                                                         : anc->path), -1);
                git_index_conflict_iterator_free(it);
            }
        } else {
            /* A clean merge: commit it now, as git pull does. */
            git_oid treeid, mid;
            git_buf msg = GIT_BUF_INIT;
            if (git_signature_default(&sig, repo) < 0) {
                emsg("esp_git_pull(): merged, but who is committing? :EspGitConfig --global "
                     "user.name 'Your Name' (then :EspGitCommit finishes the merge)");
                goto out;
            }
            if (git_index_write_tree(&treeid, index) < 0 || git_tree_lookup(&tree, repo, &treeid) < 0
                    || (ours = head_commit(repo)) == NULL
                    || git_commit_lookup(&their_commit, repo, tid) < 0)
                goto fail;
            /* MERGE_MSG, which git_merge() wrote. */
            const char *text = git_repository_message(&msg, repo) == 0 ? msg.ptr : "Merge\n";
            const git_commit *parents[2] = { ours, their_commit };
            int rc = git_commit_create(&mid, repo, "HEAD", sig, sig, NULL, text, tree, 2, parents);
            git_buf_dispose(&msg);
            if (rc < 0)
                goto fail;
            git_repository_state_cleanup(repo);
            dict_add_string(d, "result", (char_u *)"merged");
        }
    }
    git_oid now;
    dict_add_string(d, "head", (char_u *)(git_reference_name_to_id(&now, repo, "HEAD") == 0
                                          ? git_oid_tostr_s(&now) : ""));
    goto out;
fail:
    report("esp_git_pull", &op);
out:
    git_signature_free(sig);
    git_tree_free(tree);
    git_index_free(index);
    git_commit_free(ours);
    git_commit_free(their_commit);
    git_annotated_commit_free(theirs);
    git_reference_free(moved);
    git_reference_free(up);
    git_reference_free(head);
    git_remote_free(r);
    git_repository_free(repo);
}

/* esp_git_push({path} [, {remote} [, {branch}]]) -> Dict: remote, branch.
 * The current branch by default; the first push sets it to track
 * {remote}/{branch}. */
void f_esp_git_push(typval_T *argvars, typval_T *rettv)
{
    if (rettv_dict_alloc(rettv) == FAIL)
        return;
    git_repository *repo = open_repo(&argvars[0], "esp_git_push");
    if (repo == NULL)
        return;
    char remote[64], branch[128], spec[300];
    git_remote *r = NULL;
    git_reference *local = NULL, *up = NULL;
    op_t op;
    remote_name(repo, &argvars[1], remote, sizeof remote);
    if (argvars[1].v_type != VAR_UNKNOWN && argvars[2].v_type != VAR_UNKNOWN) {
        char_u *b = str_arg(&argvars[2]);
        if (b == NULL)
            goto out;
        vim_snprintf(branch, sizeof branch, "%s", b);
    } else {
        branch_name(repo, branch, sizeof branch);
    }
    if (!branch[0]) {
        emsg("esp_git_push(): HEAD is detached: which branch?");
        goto out;
    }
    op_begin(&op, "Pushing", NULL);
    if (git_remote_lookup(&r, repo, remote) < 0)
        goto fail;
    esp_git_auth_begin(&op.auth, git_remote_url(r));
    vim_snprintf(spec, sizeof spec, "refs/heads/%s:refs/heads/%s", branch, branch);
    char *specs[1] = { spec };
    git_strarray refs = { specs, 1 };
    git_push_options po = GIT_PUSH_OPTIONS_INIT;
    callbacks(&po.callbacks, &op);
    if (git_remote_push(r, &refs, &po) < 0 || op.rejected[0])
        goto fail;
    /* Track what was pushed, if nothing is tracked yet. */
    if (git_branch_lookup(&local, repo, branch, GIT_BRANCH_LOCAL) == 0
            && git_branch_upstream(&up, local) != 0) {
        char name[200];
        vim_snprintf(name, sizeof name, "%s/%s", remote, branch);
        git_branch_set_upstream(local, name);
    }
    dict_add_string(rettv->vval.v_dict, "remote", (char_u *)remote);
    dict_add_string(rettv->vval.v_dict, "branch", (char_u *)branch);
    goto out;
fail:
    report("esp_git_push", &op);
out:
    git_reference_free(up);
    git_reference_free(local);
    git_remote_free(r);
    git_repository_free(repo);
}

/* ------------------------------------------------------------------- gc */

/* esp_git_gc({path} [, {auto}]) -> Dict: packed, loose_removed,
 * packs_removed, skipped. {auto}: only past about that many loose objects. */
void f_esp_git_gc(typval_T *argvars, typval_T *rettv)
{
    if (rettv_dict_alloc(rettv) == FAIL)
        return;
    char_u *s = str_arg(&argvars[0]);
    if (s == NULL || !ensure_init("esp_git_gc"))
        return;
    size_t autolimit = argvars[1].v_type == VAR_UNKNOWN ? 0 : (size_t)tv_get_number(&argvars[1]);
    char_u *full = FullName_save(*s ? s : (char_u *)".", TRUE);
    esp_git_gc_t g;
    char err[256];
    if (full != NULL && esp_git_gc((char *)full, autolimit, &g, err, sizeof err) == 0) {
        dict_T *d = rettv->vval.v_dict;
        dict_add_number(d, "packed", (varnumber_T)g.packed);
        dict_add_number(d, "loose_removed", (varnumber_T)g.loose_removed);
        dict_add_number(d, "packs_removed", (varnumber_T)g.packs_removed);
        dict_add_bool(d, "skipped", g.skipped);
    } else if (full != NULL) {
        semsg("esp_git_gc(): %s", err);
    }
    vim_free(full);
}
