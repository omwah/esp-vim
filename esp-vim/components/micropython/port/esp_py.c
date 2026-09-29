/*
 * MicroPython in Vim: the interpreter's lifetime, running code, and the _vim
 * module (see include/esp_py.h).
 *
 * Everything here runs on the Vim task. Vim is reached only through the host
 * table, so this file needs MicroPython's headers and no Vim or ESP-IDF ones:
 * it is read by the qstr preprocessing pass, which has MicroPython's include
 * paths only. What needs ESP-IDF (the task, its stack, the heap block, the
 * clock) is in mphal_esp.c.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "py/builtin.h"
#include "py/compile.h"
#include "py/cstack.h"
#include "py/gc.h"
#include "py/lexer.h"
#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/objexcept.h"
#include "py/objstr.h"
#include "py/parse.h"
#include "py/runtime.h"
#include "py/unicode.h"
#include "extmod/vfs.h"
#include "extmod/vfs_posix.h"

#include "esp_py.h"
#include "esp_py_port.h"

#define DEPTH_MAX 100               /* nesting of a converted value */
#define ERR_MAX   512

static const esp_py_host_t *s_host;
static void   *s_heap;
static size_t  s_heap_size;
static int     s_started;
static int     s_depth;             /* esp_py_exec()s in progress */
static uint32_t s_last_poll;

/* ---------------------------------------------------------------------- */
/*  Output, input and CTRL-C                                              */
/* ---------------------------------------------------------------------- */

mp_uint_t mp_hal_stdout_tx_strn(const char *str, size_t len) {
    if (s_host != NULL && len > 0) {
        s_host->write(str, len);
    }
    return len;
}

void mp_hal_stdout_tx_strn_cooked(const char *str, size_t len) {
    mp_hal_stdout_tx_strn(str, len);
}

int mp_hal_stdin_rx_chr(void) {
    /* No console belongs to Python: Vim has it. input() asks through Vim. */
    mp_raise_OSError(MP_EIO);
}

uintptr_t mp_hal_stdio_poll(uintptr_t poll_flags) {
    (void)poll_flags;
    return 0;
}

/* Raise KeyboardInterrupt at the next opportunity. */
static void schedule_interrupt(void) {
    mp_obj_exception_clear_traceback(MP_OBJ_FROM_PTR(&MP_STATE_VM(mp_kbd_exception)));
    mp_sched_keyboard_interrupt();
}

/* From the VM (MICROPY_VM_HOOK_POLL) and sleep: at most every 20 ms, ask Vim
 * whether CTRL-C was typed. */
void esp_py_poll(void) {
    uint32_t now = mp_hal_ticks_ms();
    if (s_host == NULL || now - s_last_poll < 20) {
        return;
    }
    s_last_poll = now;
    if (s_host->interrupted()) {
        schedule_interrupt();
    }
}

void nlr_jump_fail(void *val) {
    (void)val;
    abort();
}

/* ---------------------------------------------------------------------- */
/*  Vim values <-> Python objects                                          */
/* ---------------------------------------------------------------------- */

static mp_obj_t to_py(const void *v, int depth);

typedef struct {
    mp_obj_t container;
    int depth;
} each_ctx_t;

static int add_item(void *ctx_, const char *key, const void *item) {
    each_ctx_t *ctx = ctx_;
    mp_obj_t o = to_py(item, ctx->depth + 1);
    if (key == NULL) {
        mp_obj_list_append(ctx->container, o);
    } else {
        mp_obj_dict_store(ctx->container, mp_obj_new_str(key, strlen(key)), o);
    }
    return 0;
}

static mp_obj_t new_str_or_bytes(const char *p, size_t len) {
    if (unicode_encoding_check(MP_ENCODING_UTF8, (const byte *)p, len)) {
        return mp_obj_new_str(p, len);
    }
    return mp_obj_new_bytes((const byte *)p, len);     /* not UTF-8: as it is */
}

static mp_obj_t to_py(const void *v, int depth) {
    if (depth > DEPTH_MAX) {
        mp_raise_ValueError(MP_ERROR_TEXT("Vim value nested too deeply"));
    }
    size_t len;
    const char *p;
    each_ctx_t ctx = { MP_OBJ_NULL, depth };

    switch (s_host->kind(v)) {
        case ESP_PY_V_NONE:
            return mp_const_none;
        case ESP_PY_V_BOOL:
            return mp_obj_new_bool(s_host->number(v) != 0);
        case ESP_PY_V_NUMBER:
            return mp_obj_new_int_from_ll(s_host->number(v));
        case ESP_PY_V_FLOAT:
            return mp_obj_new_float(s_host->fnum(v));
        case ESP_PY_V_BLOB:
            p = s_host->bytes(v, &len);
            return mp_obj_new_bytes((const byte *)p, len);
        case ESP_PY_V_LIST:
        case ESP_PY_V_TUPLE:
            ctx.container = mp_obj_new_list(0, NULL);
            s_host->each(v, add_item, &ctx);
            if (s_host->kind(v) == ESP_PY_V_TUPLE) {
                size_t n;
                mp_obj_t *items;
                mp_obj_list_get(ctx.container, &n, &items);
                return mp_obj_new_tuple(n, items);
            }
            return ctx.container;
        case ESP_PY_V_DICT:
            ctx.container = mp_obj_new_dict(0);
            s_host->each(v, add_item, &ctx);
            return ctx.container;
        case ESP_PY_V_STRING:
        default:
            p = s_host->bytes(v, &len);
            return new_str_or_bytes(p, len);
    }
}

static void from_py(mp_obj_t o, void *v, int depth);

static void no_memory(void) {
    mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("out of Vim memory"));
}

static void from_seq(mp_obj_t o, void *v, int depth) {
    size_t n;
    mp_obj_t *items;
    mp_obj_get_array(o, &n, &items);
    if (s_host->set_list(v) != 0) {
        no_memory();
    }
    for (size_t i = 0; i < n; i++) {
        void *item = s_host->list_append(v);
        if (item == NULL) {
            no_memory();
        }
        from_py(items[i], item, depth + 1);
    }
}

static void from_dict(mp_obj_t o, void *v, int depth) {
    mp_map_t *map = mp_obj_dict_get_map(o);
    if (s_host->set_dict(v) != 0) {
        no_memory();
    }
    for (size_t i = 0; i < map->alloc; i++) {
        if (!mp_map_slot_is_filled(map, i)) {
            continue;
        }
        mp_obj_t k = map->table[i].key;
        const char *key = mp_obj_is_str(k) ? mp_obj_str_get_str(k)
                                           : mp_obj_str_get_str(mp_obj_str_make_new(
                                                 &mp_type_str, 1, 0, &k));
        void *item = s_host->dict_add(v, key);
        if (item == NULL) {
            mp_raise_msg_varg(&mp_type_ValueError,
                MP_ERROR_TEXT("duplicate key in Dict: %s"), key);
        }
        from_py(map->table[i].value, item, depth + 1);
    }
}

static void *to_vim(mp_obj_t o, int depth);

static void from_tuple(mp_obj_t o, void *v, int depth) {
    size_t n;
    mp_obj_t *items;
    mp_obj_tuple_get(o, &n, &items);
    if (s_host->set_tuple(v, n) != 0) {
        no_memory();
    }
    for (size_t i = 0; i < n; i++) {
        if (s_host->tuple_add(v, to_vim(items[i], depth + 1)) != 0) {
            no_memory();
        }
    }
}

/* Fill v (a new_value()) from o. Raises on what Vim cannot hold; v is then
 * partly filled, and the caller frees it. */
static void from_py(mp_obj_t o, void *v, int depth) {
    if (depth > DEPTH_MAX) {
        mp_raise_ValueError(MP_ERROR_TEXT("value nested too deeply (or recursive) for Vim"));
    }
    if (o == mp_const_none) {
        s_host->set_none(v);
    } else if (mp_obj_is_bool(o)) {
        s_host->set_bool(v, o == mp_const_true);
    } else if (mp_obj_is_int(o)) {
        s_host->set_number(v, mp_obj_get_ll(o));
    } else if (mp_obj_is_float(o)) {
        s_host->set_float(v, mp_obj_get_float(o));
    } else if (mp_obj_is_str(o)) {
        size_t len;
        const char *s = mp_obj_str_get_data(o, &len);
        if (s_host->set_string(v, s, len) != 0) {
            no_memory();
        }
    } else if (mp_obj_is_type(o, &mp_type_list)) {
        from_seq(o, v, depth);
    } else if (mp_obj_is_type(o, &mp_type_tuple)) {
        from_tuple(o, v, depth);
    } else if (mp_obj_is_dict_or_ordereddict(o)) {
        from_dict(o, v, depth);
    } else {
        mp_buffer_info_t buf;
        if (mp_get_buffer(o, &buf, MP_BUFFER_READ)) {       /* bytes, bytearray... */
            if (s_host->set_blob(v, buf.buf, buf.len) != 0) {
                no_memory();
            }
            return;
        }
        mp_raise_msg_varg(&mp_type_TypeError,
            MP_ERROR_TEXT("can't convert %s to a Vim value"), mp_obj_get_type_str(o));
    }
}

/* from_py() into a new host value, freeing it if the conversion raises. */
static void *to_vim(mp_obj_t o, int depth) {
    void *v = s_host->new_value();
    if (v == NULL) {
        no_memory();
    }
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        from_py(o, v, depth);
        nlr_pop();
        return v;
    }
    s_host->free_value(v);
    nlr_jump(nlr.ret_val);
}

/* to_py() of a host value, which is freed either way. */
static mp_obj_t take_py(void *v) {
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_obj_t o = to_py(v, 0);
        nlr_pop();
        s_host->free_value(v);
        return o;
    }
    s_host->free_value(v);
    nlr_jump(nlr.ret_val);
}

/* ---------------------------------------------------------------------- */
/*  The _vim module (runtime-image/python/vim.py builds `vim` on it)        */
/* ---------------------------------------------------------------------- */

MP_DEFINE_CONST_OBJ_TYPE(mp_type_vim_error, MP_QSTR_error, MP_TYPE_FLAG_NONE,
    make_new, mp_obj_exception_make_new,
    print, mp_obj_exception_print,
    attr, mp_obj_exception_attr,
    parent, &mp_type_Exception);

/* A host call's result: raise vim.error or KeyboardInterrupt on failure. */
static void check(int rc, const char *err) {
    if (rc == -2) {
        mp_raise_type(&mp_type_KeyboardInterrupt);
    }
    if (rc != 0) {
        mp_obj_t msg = new_str_or_bytes(err, strlen(err));
        nlr_raise(mp_obj_new_exception_arg1(&mp_type_vim_error, msg));
    }
}

/* vim.command(cmd) */
static mp_obj_t vim_command(mp_obj_t cmd) {
    char err[ERR_MAX] = "";
    check(s_host->command(mp_obj_str_get_str(cmd), err, sizeof(err)), err);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(vim_command_obj, vim_command);

/* vim.eval(expr) */
static mp_obj_t vim_eval(mp_obj_t expr) {
    char err[ERR_MAX] = "";
    void *out = s_host->new_value();
    if (out == NULL) {
        no_memory();
    }
    int rc = s_host->eval(mp_obj_str_get_str(expr), out, err, sizeof(err));
    if (rc != 0) {
        s_host->free_value(out);
        check(rc, err);
    }
    return take_py(out);
}
static MP_DEFINE_CONST_FUN_OBJ_1(vim_eval_obj, vim_eval);

/* vim.call(func, *args) */
static mp_obj_t vim_call(size_t n_args, const mp_obj_t *args) {
    char err[ERR_MAX] = "";
    const char *func = mp_obj_str_get_str(args[0]);
    void *argv = to_vim(mp_obj_new_list(n_args - 1, (mp_obj_t *)args + 1), 0);
    void *out = s_host->new_value();
    if (out == NULL) {
        s_host->free_value(argv);
        no_memory();
    }
    int rc = s_host->call(func, argv, out, err, sizeof(err));
    s_host->free_value(argv);
    if (rc != 0) {
        s_host->free_value(out);
        check(rc, err);
    }
    return take_py(out);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(vim_call_obj, 1, 21, vim_call);

static const mp_rom_map_elem_t vim_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR__vim) },
    { MP_ROM_QSTR(MP_QSTR_command), MP_ROM_PTR(&vim_command_obj) },
    { MP_ROM_QSTR(MP_QSTR_eval), MP_ROM_PTR(&vim_eval_obj) },
    { MP_ROM_QSTR(MP_QSTR_call), MP_ROM_PTR(&vim_call_obj) },
    { MP_ROM_QSTR(MP_QSTR_error), MP_ROM_PTR(&mp_type_vim_error) },
};
static MP_DEFINE_CONST_DICT(vim_module_globals, vim_module_globals_table);

const mp_obj_module_t mp_module_vim = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&vim_module_globals,
};
MP_REGISTER_MODULE(MP_QSTR__vim, mp_module_vim);

/* input([prompt]): Vim's input(), so it works with the editor on screen. */
static mp_obj_t esp_py_input(size_t n_args, const mp_obj_t *args) {
    mp_obj_t prompt = n_args > 0 ? mp_obj_str_make_new(&mp_type_str, 1, 0, args)
                                 : MP_OBJ_NEW_QSTR(MP_QSTR_);
    mp_obj_t call_args[2] = { MP_OBJ_NEW_QSTR(MP_QSTR_input), prompt };
    return vim_call(2, call_args);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(esp_py_input_obj, 0, 1, esp_py_input);

/* ---------------------------------------------------------------------- */
/*  Lifetime                                                               */
/* ---------------------------------------------------------------------- */

/* Tell MicroPython where the C stack is: from here down to the task's end. */
static int set_stack(void *here) {
    size_t avail = esp_py_port_stack_avail(here);
    if (avail <= MICROPY_STACK_CHECK_MARGIN + 4096) {
        return -1;
    }
    mp_cstack_init_with_top(here, avail);
    return 0;
}

static void add_path(const char *dir) {
    mp_obj_list_append(mp_sys_path, mp_obj_new_str(dir, strlen(dir)));
}

/* Import vim and esp into __main__, so :EspPy vim.eval('&ts') just works.
 * They live in /vimrt/python; a runtime without them still has _vim. */
static const char s_prelude[] =
    "try:\n"
    " import vim, esp\n"
    "except ImportError:\n"
    " pass\n";

int esp_py_start(const esp_py_host_t *host, char *err, size_t errlen) {
    int here;

    if (s_started) {
        return 0;
    }
    /* First use, a reset, or a new session (esp_py_new_session()). */
    s_host = host;
    s_started = 0;
    s_depth = 0;
    if (s_heap == NULL) {
        s_heap = esp_py_port_alloc_heap(&s_heap_size);
        if (s_heap == NULL) {
            snprintf(err, errlen, "no memory for the Python heap (%u KB)",
                (unsigned)(s_heap_size / 1024));
            return -1;
        }
    }
    if (set_stack(&here) != 0) {
        snprintf(err, errlen, "not enough stack left to start Python");
        return -1;
    }
    gc_init(s_heap, (uint8_t *)s_heap + s_heap_size);
    mp_init();

    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_obj_list_append(mp_sys_path, MP_OBJ_NEW_QSTR(MP_QSTR_));
        add_path("/fat/python");
        add_path("/vimrt/python");
        mp_obj_t args[2] = {
            MP_OBJ_TYPE_GET_SLOT(&mp_type_vfs_posix, make_new)(&mp_type_vfs_posix, 0, 0, NULL),
            MP_OBJ_NEW_QSTR(MP_QSTR__slash_),
        };
        mp_vfs_mount(2, args, (mp_map_t *)&mp_const_empty_map);
        MP_STATE_VM(vfs_cur) = MP_STATE_VM(vfs_mount_table);
        nlr_pop();
    } else {
        snprintf(err, errlen, "Python failed to start");
        mp_deinit();
        return -1;
    }
    s_started = 1;

    char *tb = NULL;
    esp_py_exec(s_prelude, sizeof(s_prelude) - 1, "<prelude>", ESP_PY_FILE, &tb);
    free(tb);
    return 0;
}

/* The old session's files were closed with it (esp_vim_session_begin), so its
 * interpreter is dropped, not deinitialised: its finalisers would close those
 * descriptors again, and they may be someone else's by now. */
void esp_py_new_session(void) {
    s_started = 0;
    s_depth = 0;
}

int esp_py_reset(void) {
    if (s_depth > 0) {
        return -1;          /* from inside Python (vim.command) */
    }
    if (s_started) {
        int here;
        set_stack(&here);
        mp_deinit();        /* runs finalisers: open files are closed */
    }
    s_started = 0;
    return 0;
}

/* The exception's traceback as text, malloc'd. */
static char *traceback(mp_obj_t exc) {
    vstr_t vstr;
    vstr_init(&vstr, 256);
    mp_print_t pr = { &vstr, (mp_print_strn_t)vstr_add_strn };
    mp_obj_print_exception(&pr, exc);
    char *s = malloc(vstr.len + 1);
    if (s != NULL) {
        memcpy(s, vstr.buf, vstr.len);
        s[vstr.len] = '\0';
    }
    vstr_clear(&vstr);
    return s;
}

static int run(const char *src, size_t len, const char *name,
               mp_parse_input_kind_t kind, void *out, char **tb) {
    int here;
    int rc = 0;

    if (tb != NULL) {
        *tb = NULL;
    }
    if (!s_started) {
        return -2;
    }
    if (s_depth == 0 && set_stack(&here) != 0) {
        if (tb != NULL) {
            *tb = strdup("RuntimeError: not enough stack left to run Python\n");
        }
        return -1;
    }
    s_depth++;

    /* Code from Vim runs in __main__, whatever module called into Vim. */
    mp_obj_dict_t *globals = mp_globals_get();
    mp_obj_dict_t *locals = mp_locals_get();
    mp_globals_set(&MP_STATE_VM(dict_main));
    mp_locals_set(&MP_STATE_VM(dict_main));

    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        qstr qname = qstr_from_str(name != NULL ? name : "<string>");
        mp_lexer_t *lex = mp_lexer_new_from_str_len(qname, src, len, 0);
        mp_parse_tree_t tree = mp_parse(lex, kind);
        mp_obj_t fun = mp_compile(&tree, qname, kind == MP_PARSE_SINGLE_INPUT);
        mp_obj_t result = mp_call_function_0(fun);
        mp_handle_pending(true);        /* a CTRL-C just at the end */
        if (out != NULL) {
            from_py(result, out, 0);
        }
        nlr_pop();
    } else {
        mp_obj_t exc = MP_OBJ_FROM_PTR(nlr.ret_val);
        if (!mp_obj_is_subclass_fast(MP_OBJ_FROM_PTR(mp_obj_get_type(exc)),
                                     MP_OBJ_FROM_PTR(&mp_type_SystemExit))) {
            rc = -1;
            if (tb != NULL) {
                *tb = traceback(exc);
            }
        }
    }
    /* A CTRL-C that came too late to raise is not kept for the next run. */
    MP_STATE_MAIN_THREAD(mp_pending_exception) = MP_OBJ_NULL;

    mp_globals_set(globals);
    mp_locals_set(locals);
    s_depth--;
    return rc;
}

int esp_py_exec(const char *src, size_t len, const char *name, esp_py_mode_t mode,
                char **tb) {
    return run(src, len, name,
        mode == ESP_PY_SINGLE ? MP_PARSE_SINGLE_INPUT : MP_PARSE_FILE_INPUT, NULL, tb);
}

int esp_py_eval(const char *expr, size_t len, void *out, char **tb) {
    return run(expr, len, "<eval>", MP_PARSE_EVAL_INPUT, out, tb);
}

void esp_py_heap(esp_py_heap_t *out) {
    memset(out, 0, sizeof(*out));
    out->total = s_heap_size;
    out->running = s_started;
    if (out->running) {
        gc_info_t info;
        gc_info(&info);
        out->total = info.total;
        out->used = info.used;
        out->free = info.free;
        out->max_free = info.max_free * MICROPY_BYTES_PER_GC_BLOCK;
    }
}
