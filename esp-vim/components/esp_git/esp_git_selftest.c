/*
 * The libgit2 feasibility self-test (stage 6z, docs/PLAN.md).
 *
 * A task waits for /fat/gittest.conf (written by esp-vim/test/git.py), then
 * runs every step the file asks for and prints one line per step:
 *     GIT-ST <step> ok|FAIL ms=<time> peak=<libgit2 heap peak> <details>
 * and at the end
 *     GIT-ST-END stack=<bytes used> peak=<heap peak> int_min=<internal RAM low>
 * The conf file is renamed to .done first, so a crash doesn't loop.
 *
 * conf lines, key=value:
 *     dir=/fat/gt                  where the repositories go (emptied first)
 *     name=... / email=...         the commit author
 *     http= / git= / ssh= / https= a remote to clone, fetch and push each
 *                                  (https: clone only), any of them
 *     big=                         a larger repository to clone, then delete:
 *                                  its pack is several pack windows long
 * SSH authenticates with /fat/.ssh/id_ecdsa (esp_ssh_keygen()).
 *
 * libgit2 runs on this task alone, never on Vim's; it knows nothing of Vim.
 */

#include "esp_git.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_fs.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "git2.h"
#include "git2/sys/alloc.h"
#include "git2/sys/errors.h"

#define CONF    "/fat/gittest.conf"
#define KEYFILE "/fat/.ssh/id_ecdsa"

/* ---- counting allocator: libgit2's own heap use, current and peak ---- */

static size_t s_cur, s_peak, s_peak_all;

typedef union { size_t n; long long align; } hdr_t;

/* Where the big allocations come from: each one of 128 KB or more. */
static void big(size_t n, const char *file, int line)
{
    if (n >= 128 * 1024) {
        const char *base = strrchr(file ? file : "?", '/');
        printf("GIT-ALLOC %u %s:%d\n", (unsigned)n, base ? base + 1 : file, line);
    }
}

static void *a_malloc(size_t n, const char *file, int line)
{
    big(n, file, line);
    hdr_t *h = malloc(sizeof(hdr_t) + n);
    if (h == NULL)
        return NULL;
    h->n = n;
    s_cur += n;
    if (s_cur > s_peak)
        s_peak = s_cur;
    if (s_cur > s_peak_all)
        s_peak_all = s_cur;
    return h + 1;
}

static void a_free(void *p)
{
    if (p == NULL)
        return;
    hdr_t *h = (hdr_t *)p - 1;
    s_cur -= h->n;
    free(h);
}

static void *a_realloc(void *p, size_t n, const char *file, int line)
{
    if (p == NULL)
        return a_malloc(n, file, line);
    big(n, file, line);
    hdr_t *h = (hdr_t *)p - 1;
    size_t old = h->n;
    hdr_t *nh = realloc(h, sizeof(hdr_t) + n);
    if (nh == NULL)
        return NULL;
    nh->n = n;
    s_cur = s_cur - old + n;
    if (s_cur > s_peak)
        s_peak = s_cur;
    if (s_cur > s_peak_all)
        s_peak_all = s_cur;
    return nh + 1;
}

/* ---- conf ---- */

static struct {
    char dir[64], name[64], email[64];
    char http[160], git[160], ssh[160], https[160], big[160];
} C;

static void conf_read(void)
{
    FILE *f = fopen(CONF, "r");
    char line[200];
    if (f == NULL)
        return;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (eq == NULL)
            continue;
        *eq++ = 0;
#define KEY(k) if (!strcmp(line, #k)) snprintf(C.k, sizeof(C.k), "%s", eq)
        KEY(dir); KEY(name); KEY(email);
        KEY(http); KEY(git); KEY(ssh); KEY(https); KEY(big);
#undef KEY
    }
    fclose(f);
}

/* ---- reporting ---- */

static int64_t s_t0;
static char s_detail[256];
static int s_failures;

static void step_begin(void)
{
    s_detail[0] = 0;
    s_peak = s_cur;
    s_t0 = esp_timer_get_time();
}

static void step_end(const char *step, int rc)
{
    const git_error *e = rc < 0 ? git_error_last() : NULL;
    if (rc < 0)
        s_failures++;
    printf("GIT-ST %s %s ms=%lld peak=%u %s%s%s\n", step, rc < 0 ? "FAIL" : "ok",
           (long long)((esp_timer_get_time() - s_t0) / 1000), (unsigned)s_peak, s_detail,
           e ? " err=" : "", e ? e->message : "");
}

#define DETAIL(...) snprintf(s_detail + strlen(s_detail), sizeof(s_detail) - strlen(s_detail), __VA_ARGS__)
#define TRY(x) do { if ((rc = (x)) < 0) goto out; } while (0)

/* ---- remote callbacks ---- */

typedef struct {
    int cred_tries;
    int cert_valid;             /* -1: no check made */
    unsigned objects, bytes;
    char rejected[96];
} cb_state_t;

static int cred_cb(git_credential **out, const char *url, const char *user,
                   unsigned int allowed, void *payload)
{
    cb_state_t *st = payload;
    (void)url;
    if (st->cred_tries++ > 0)
        return GIT_PASSTHROUGH;             /* give up after one try */
    if (allowed & GIT_CREDENTIAL_SSH_KEY)
        /* The public half too: libssh2's mbedTLS backend can't derive it. */
        return git_credential_ssh_key_new(out, user ? user : "git", KEYFILE ".pub", KEYFILE, NULL);
    return GIT_PASSTHROUGH;
}

/* Trust whatever the test server shows; report whether libgit2 found it valid. */
static int cert_cb(git_cert *cert, int valid, const char *host, void *payload)
{
    cb_state_t *st = payload;
    (void)cert; (void)host;
    st->cert_valid = valid;
    return 0;
}

static int progress_cb(const git_indexer_progress *p, void *payload)
{
    cb_state_t *st = payload;
    st->objects = p->total_objects;
    st->bytes = (unsigned)p->received_bytes;
    return 0;
}

static int push_ref_cb(const char *ref, const char *status, void *payload)
{
    cb_state_t *st = payload;
    if (status != NULL)
        snprintf(st->rejected, sizeof(st->rejected), "%s:%s", ref, status);
    return 0;
}

static void callbacks(git_remote_callbacks *cb, cb_state_t *st)
{
    memset(st, 0, sizeof(*st));
    st->cert_valid = -1;
    cb->credentials = cred_cb;
    cb->certificate_check = cert_cb;
    cb->transfer_progress = progress_cb;
    cb->push_update_reference = push_ref_cb;
    cb->payload = st;
}

/* ---- steps ---- */

static int write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        git_error_set_str(GIT_ERROR_OS, "cannot write the test file");
        return -1;
    }
    fputs(text, f);
    fclose(f);
    return 0;
}

/* Commit the index's {file} (already written) on top of HEAD, if any. */
static int commit_file(git_repository *repo, const char *file, const char *msg, git_oid *out)
{
    git_index *index = NULL;
    git_tree *tree = NULL;
    git_signature *sig = NULL;
    git_commit *parent = NULL;
    git_oid tree_id, head;
    int rc;

    TRY(git_repository_index(&index, repo));
    TRY(git_index_add_bypath(index, file));
    TRY(git_index_write(index));
    TRY(git_index_write_tree(&tree_id, index));
    TRY(git_tree_lookup(&tree, repo, &tree_id));
    TRY(git_signature_now(&sig, C.name, C.email));
    if (git_reference_name_to_id(&head, repo, "HEAD") == 0)
        TRY(git_commit_lookup(&parent, repo, &head));
    const git_commit *parents[1] = { parent };
    TRY(git_commit_create(out, repo, "HEAD", sig, sig, NULL, msg, tree,
                          parent ? 1 : 0, parents));
out:
    git_commit_free(parent);
    git_signature_free(sig);
    git_tree_free(tree);
    git_index_free(index);
    return rc;
}

static void local_steps(void)
{
    git_repository *repo = NULL;
    git_config *cfg = NULL;
    git_status_list *status = NULL;
    git_reference *branch = NULL;
    git_commit *commit = NULL;
    git_oid oid;
    char path[96], file[128];
    int rc, filemode = -1, symlinks = -1;

    snprintf(path, sizeof(path), "%s/local", C.dir);
    step_begin();
    rc = git_repository_init(&repo, path, 0);
    if (rc == 0 && git_repository_config_snapshot(&cfg, repo) == 0) {
        git_config_get_bool(&filemode, cfg, "core.filemode");
        git_config_get_bool(&symlinks, cfg, "core.symlinks");
    }
    DETAIL("filemode=%d symlinks=%d", filemode, symlinks);
    step_end("init", rc);
    if (rc < 0)
        return;

    step_begin();
    snprintf(file, sizeof(file), "%s/hello.txt", path);
    rc = write_file(file, "hello from the device\n");
    if (rc == 0)
        rc = commit_file(repo, "hello.txt", "First commit on the device\n", &oid);
    if (rc == 0 && (rc = write_file(file, "hello again\n")) == 0)
        rc = commit_file(repo, "hello.txt", "Second commit\n", &oid);
    if (rc == 0)
        DETAIL("head=%s", git_oid_tostr_s(&oid));
    step_end("commit", rc);

    step_begin();
    rc = write_file(file, "changed, not committed\n");
    git_status_options so = GIT_STATUS_OPTIONS_INIT;
    if (rc == 0 && (rc = git_status_list_new(&status, repo, &so)) == 0) {
        const git_status_entry *e = git_status_list_entrycount(status) ? git_status_byindex(status, 0) : NULL;
        DETAIL("entries=%u modified=%d", (unsigned)git_status_list_entrycount(status),
               e && (e->status & GIT_STATUS_WT_MODIFIED) ? 1 : 0);
    }
    step_end("status", rc);

    /* A branch at the first commit, checked out: the file goes back. */
    step_begin();
    git_checkout_options co = GIT_CHECKOUT_OPTIONS_INIT;
    co.checkout_strategy = GIT_CHECKOUT_FORCE;
    rc = git_revparse_single((git_object **)&commit, repo, "HEAD~1");
    if (rc == 0)
        rc = git_branch_create(&branch, repo, "first", commit, 0);
    if (rc == 0)
        rc = git_checkout_tree(repo, (git_object *)commit, &co);
    if (rc == 0)
        rc = git_repository_set_head(repo, "refs/heads/first");
    if (rc == 0) {
        FILE *f = fopen(file, "r");
        char text[64] = "";
        if (f) {
            fgets(text, sizeof(text), f);
            fclose(f);
        }
        text[strcspn(text, "\n")] = 0;
        DETAIL("file='%s'", text);
    }
    step_end("checkout", rc);

    git_commit_free(commit);
    git_reference_free(branch);
    git_status_list_free(status);
    git_config_free(cfg);
    git_repository_free(repo);
}

static unsigned count_commits(git_repository *repo)
{
    git_revwalk *walk = NULL;
    git_oid oid;
    unsigned n = 0;
    if (git_revwalk_new(&walk, repo) == 0 && git_revwalk_push_head(walk) == 0)
        while (git_revwalk_next(&oid, walk) == 0)
            n++;
    git_revwalk_free(walk);
    return n;
}

static void remote_steps(const char *kind, const char *url, bool push)
{
    git_repository *repo = NULL;
    git_remote *remote = NULL;
    git_reference *head = NULL;
    cb_state_t st;
    git_oid oid;
    char path[96], file[128], step[24];
    int rc;

    snprintf(path, sizeof(path), "%s/%s", C.dir, kind);
    snprintf(step, sizeof(step), "clone-%s", kind);
    step_begin();
    git_clone_options opts = GIT_CLONE_OPTIONS_INIT;
    callbacks(&opts.fetch_opts.callbacks, &st);
    rc = git_clone(&repo, url, path, &opts);
    if (rc == 0 && git_repository_head(&head, repo) == 0)
        DETAIL("head=%s commits=%u objects=%u bytes=%u cert=%d",
               git_oid_tostr_s(git_reference_target(head)), count_commits(repo),
               st.objects, st.bytes, st.cert_valid);
    step_end(step, rc);
    if (rc < 0 || !push)
        goto out;

    snprintf(step, sizeof(step), "push-%s", kind);
    step_begin();
    snprintf(file, sizeof(file), "%s/device-%s.txt", path, kind);
    char name[32];
    snprintf(name, sizeof(name), "device-%s.txt", kind);
    rc = write_file(file, "pushed from the device\n");
    if (rc == 0)
        rc = commit_file(repo, name, "Pushed from the device\n", &oid);
    if (rc == 0)
        rc = git_remote_lookup(&remote, repo, "origin");
    if (rc == 0) {
        char spec[128];
        snprintf(spec, sizeof(spec), "%s:%s", git_reference_name(head), git_reference_name(head));
        char *specs[1] = { spec };
        git_strarray refs = { specs, 1 };
        git_push_options po = GIT_PUSH_OPTIONS_INIT;
        callbacks(&po.callbacks, &st);
        rc = git_remote_push(remote, &refs, &po);
        if (rc == 0 && st.rejected[0]) {
            git_error_set_str(GIT_ERROR_REFERENCE, st.rejected);
            rc = -1;
        }
        if (rc == 0)
            DETAIL("head=%s", git_oid_tostr_s(&oid));
    }
    step_end(step, rc);
    if (rc < 0)
        goto out;

    snprintf(step, sizeof(step), "fetch-%s", kind);
    step_begin();
    git_fetch_options fo = GIT_FETCH_OPTIONS_INIT;
    callbacks(&fo.callbacks, &st);
    rc = git_remote_fetch(remote, NULL, &fo, NULL);
    if (rc == 0)
        DETAIL("objects=%u", st.objects);
    step_end(step, rc);
out:
    git_remote_free(remote);
    git_reference_free(head);
    git_repository_free(repo);
}

static void selftest_task(void *arg)
{
    (void)arg;
    struct stat sb;
    while (stat(CONF, &sb) != 0)
        vTaskDelay(pdMS_TO_TICKS(1000));

    memset(&C, 0, sizeof(C));
    snprintf(C.dir, sizeof(C.dir), "/fat/gt");
    snprintf(C.name, sizeof(C.name), "esp-vim");
    snprintf(C.email, sizeof(C.email), "esp-vim@localhost");
    conf_read();
    rename(CONF, CONF ".done");

    esp_fs_err_t ferr;
    esp_fs_delete(C.dir, NULL, NULL, &ferr);            /* fine if absent */
    mkdir(C.dir, 0777);

    printf("GIT-ST-BEGIN libgit2=%s\n", LIBGIT2_VERSION);
    static git_allocator alloc = { a_malloc, a_realloc, a_free };
    git_libgit2_opts(GIT_OPT_SET_ALLOCATOR, &alloc);
    step_begin();
    int rc = git_libgit2_init();
    /*
     * Memory limits for a device. Without mmap a pack "window" is a malloc'd
     * copy, and the 32-bit default is 32 MB of one: a large clone would read
     * its whole pack into RAM. 256 KB windows, at most ~1 MB of them, and a
     * 1 MB object cache (the default is 256 MB).
     */
    if (rc >= 0)
        rc = git_libgit2_opts(GIT_OPT_SET_MWINDOW_SIZE, (size_t)256 * 1024);
    if (rc >= 0)
        rc = git_libgit2_opts(GIT_OPT_SET_MWINDOW_MAPPED_LIMIT, (size_t)1024 * 1024);
    if (rc >= 0)
        rc = git_libgit2_opts(GIT_OPT_SET_CACHE_MAX_SIZE, (ssize_t)1024 * 1024);
    step_end("libinit", rc);

    local_steps();
    if (C.http[0])
        remote_steps("http", C.http, true);
    if (C.git[0])
        remote_steps("git", C.git, true);
    if (C.ssh[0])
        remote_steps("ssh", C.ssh, true);
    if (C.https[0])
        remote_steps("https", C.https, false);
    if (C.big[0]) {
        char path[96];
        esp_fs_err_t e;
        remote_steps("big", C.big, false);
        snprintf(path, sizeof(path), "%s/big", C.dir);
        esp_fs_delete(path, NULL, NULL, &e);
    }

    git_libgit2_shutdown();
    printf("GIT-ST-END failures=%d peak=%u stack=%u leaked=%u int_min=%u\n", s_failures,
           (unsigned)s_peak_all,
           (unsigned)(CONFIG_ESP_VIM_GIT_SELFTEST_STACK - uxTaskGetStackHighWaterMark(NULL)),
           (unsigned)s_cur, (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    vTaskDelete(NULL);
}

void esp_git_selftest_start(void)
{
    /* Core 0, as Vim: select() across cores asserted under the emulator. */
    xTaskCreatePinnedToCore(selftest_task, "gittest", CONFIG_ESP_VIM_GIT_SELFTEST_STACK,
                            NULL, 4, NULL, 0);
}
