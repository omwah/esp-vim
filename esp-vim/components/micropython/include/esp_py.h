/*
 * MicroPython, embedded in Vim (docs/PLAN.md, Phase 7).
 *
 * The interpreter runs on the Vim task and is entered only through the calls
 * below, from the esp_py_*() builtins (components/vim/api/esp_api_py.c). It
 * reaches Vim only through the esp_py_host_t table those builtins hand it,
 * which is also how this component stays free of Vim's headers: a Vim value
 * is an opaque pointer (a typval_T *) that the host reads and builds for it.
 *
 * State: one interpreter, with its heap in one fixed PSRAM block (Kconfig
 * ESP_VIM_PY_HEAP_KB) allocated on first use and kept. A new Vim session (the
 * restart after :q) says so with esp_py_new_session(), and the next use starts
 * a new interpreter in the same block.
 */

#ifndef ESP_PY_H
#define ESP_PY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ESP_PY_V_OTHER,     /* a Funcref, a Job...: read as its string() */
    ESP_PY_V_NONE,      /* v:none, v:null */
    ESP_PY_V_BOOL,      /* v:true, v:false */
    ESP_PY_V_NUMBER,
    ESP_PY_V_FLOAT,
    ESP_PY_V_STRING,
    ESP_PY_V_BLOB,
    ESP_PY_V_LIST,
    ESP_PY_V_TUPLE,
    ESP_PY_V_DICT,
} esp_py_kind_t;

/* Called for each item of a List or Tuple (key NULL) or Dict; nonzero stops. */
typedef int (*esp_py_each_fn)(void *ctx, const char *key, const void *item);

typedef struct {
    /* Reading a Vim value. */
    esp_py_kind_t (*kind)(const void *v);
    int64_t     (*number)(const void *v);           /* NUMBER, BOOL */
    double      (*fnum)(const void *v);             /* FLOAT */
    /* STRING, BLOB and OTHER: the bytes, valid until the value is freed. */
    const char *(*bytes)(const void *v, size_t *len);
    int         (*each)(const void *v, esp_py_each_fn fn, void *ctx);

    /* Building one. new_value() is empty; each set_*() fills it once. */
    void       *(*new_value)(void);
    void        (*free_value)(void *v);
    void        (*set_none)(void *v);
    void        (*set_bool)(void *v, int b);
    void        (*set_number)(void *v, int64_t n);
    void        (*set_float)(void *v, double f);
    int         (*set_string)(void *v, const char *s, size_t len);
    int         (*set_blob)(void *v, const void *p, size_t len);
    int         (*set_list)(void *v);
    void       *(*list_append)(void *list);         /* an empty item to fill */
    int         (*set_tuple)(void *v, size_t n);
    int         (*tuple_add)(void *tuple, void *item); /* takes item (a new_value()) */
    int         (*set_dict)(void *v);
    void       *(*dict_add)(void *dict, const char *key); /* NULL: duplicate */

    /* Vim itself. Each returns 0, or -1 with a message in err (a Vim error
     * or exception), or -2 if CTRL-C interrupted it. */
    int (*command)(const char *cmd, char *err, size_t errlen);
    int (*eval)(const char *expr, void *out, char *err, size_t errlen);
    int (*call)(const char *func, const void *args, void *out, char *err, size_t errlen);

    int  (*interrupted)(void);      /* looks for CTRL-C; nonzero if typed */
    void (*write)(const char *s, size_t len);   /* print() and sys.stdout */
} esp_py_host_t;

/* How esp_py_exec() compiles the source. */
typedef enum {
    ESP_PY_FILE,        /* a module: statements */
    ESP_PY_SINGLE,      /* one statement, as typed at the prompt: an
                           expression's value is printed */
} esp_py_mode_t;

/*
 * Make sure the interpreter is running, starting it if this is the first use
 * in this session. The host table must outlive it. Returns 0, or -1 with a
 * reason in err (no memory for the heap).
 */
int  esp_py_start(const esp_py_host_t *host, char *err, size_t errlen);

/* A new Vim session has begun: forget the interpreter without running it
 * down (the old session's files are already closed). */
void esp_py_new_session(void);

/* Discard all Python state; the next use starts again. -1 while Python is
 * running (it was called from Python, through Vim). */
int  esp_py_reset(void);

/*
 * Run source. name is the file name tracebacks show. On an uncaught exception
 * returns -1 and, if tb is given, sets *tb to the traceback text (malloc'd:
 * free() it); SystemExit is not an error. -2: not started.
 */
int  esp_py_exec(const char *src, size_t len, const char *name, esp_py_mode_t mode,
                 char **tb);

/* Evaluate an expression into out (a new_value() from the host). */
int  esp_py_eval(const char *expr, size_t len, void *out, char **tb);

typedef struct {
    size_t total, used, free, max_free;     /* bytes of the Python heap */
    int running;                            /* started in this session */
} esp_py_heap_t;
void esp_py_heap(esp_py_heap_t *out);

#ifdef __cplusplus
}
#endif

#endif
