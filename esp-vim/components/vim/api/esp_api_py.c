/*
 * esp_py_exec(), esp_py_eval(), esp_py_reset(), esp_py_heap(): MicroPython,
 * behind :EspPy, :EspPyRun and :EspPyReset (docs/PLAN.md, Phase 7).
 *
 * The interpreter (components/micropython) runs here, on the Vim task, and
 * reaches Vim only through s_host below: Vim values as opaque typval_T
 * pointers, and commands, expressions and calls run the way Vim's own Python
 * interface runs them (if_py_both.h: VimTryStart/VimTryEnd), so a Vim error
 * becomes a Python exception (vim.error) rather than a message.
 *
 * print() goes to a sink: Vim's messages, or the end of a buffer (the
 * [Python] window of :EspPyRun), shown as it arrives.
 */

#include "vim.h"
#include "esp_vim_api.h"
#include "esp_py.h"

#include "esp_timer.h"

/* ------------------------------------------------------------ output -- */

typedef struct {
    int         buf;            /* buffer number, or 0: messages */
    garray_T    line;           /* the unfinished line */
    int64_t     last_show;      /* when the screen last caught up */
} sink_T;

static sink_T *s_sink;          /* where print() goes now */

/* Windows showing buf whose cursor was on its last line (before) follow the
 * new end, as channel output into a buffer does (channel.c). */
static void follow_end(buf_T *buf, linenr_T before)
{
    win_T *wp;

    FOR_ALL_WINDOWS(wp)
    {
	if (wp->w_buffer != buf || wp->w_cursor.lnum != before)
	    continue;
	win_T *save_curwin = curwin;
	wp->w_cursor.lnum = buf->b_ml.ml_line_count;
	wp->w_cursor.col = 0;
	curwin = wp;
	curbuf = curwin->w_buffer;
	scroll_cursor_bot(0, FALSE);
	curwin = save_curwin;
	curbuf = curwin->w_buffer;
    }
    redraw_buf_later(buf, UPD_VALID);
}

/* Put the screen up to date, at most every 200 ms (and when forced). */
static void sink_show(sink_T *sink, int force)
{
    int64_t now = esp_timer_get_time();

    if (!force && now - sink->last_show < 200 * 1000)
	return;
    sink->last_show = now;
    if (sink->buf != 0)
	update_screen(0);
    out_flush();
}

static void sink_line(sink_T *sink, char_u *line)
{
    buf_T *buf = sink->buf != 0 ? buflist_findnr(sink->buf) : NULL;

    if (buf == NULL || buf->b_ml.ml_mfp == NULL)
    {
	msg((char *)line);
	return;
    }

    linenr_T before = buf->b_ml.ml_line_count;
    int empty = before == 1 && *ml_get_buf(buf, 1, FALSE) == NUL;
    typval_T argv[4];
    typval_T rettv;

    /* setbufline(buf, 1, line) into an empty buffer, else
     * appendbufline(buf, '$', line): both leave curbuf alone. */
    argv[0].v_type = VAR_NUMBER;
    argv[0].vval.v_number = sink->buf;
    argv[1].v_type = VAR_STRING;
    argv[1].vval.v_string = (char_u *)(empty ? "1" : "$");
    argv[2].v_type = VAR_STRING;
    argv[2].vval.v_string = line;
    argv[3].v_type = VAR_UNKNOWN;
    rettv.v_type = VAR_NUMBER;
    rettv.vval.v_number = 0;
    if (empty)
	f_setbufline(argv, &rettv);
    else
	f_appendbufline(argv, &rettv);
    clear_tv(&rettv);
    follow_end(buf, before);
}

static void host_write(const char *s, size_t len)
{
    sink_T *sink = s_sink;
    sink_T fallback;

    if (sink == NULL)
    {
	CLEAR_FIELD(fallback);
	ga_init2(&fallback.line, 1, 80);
	sink = &fallback;
    }
    for (size_t i = 0; i < len; i++)
    {
	if (s[i] != '\n')
	{
	    if (s[i] != '\r')
		ga_append(&sink->line, s[i]);
	    continue;
	}
	ga_append(&sink->line, NUL);
	sink_line(sink, sink->line.ga_data != NULL
				? (char_u *)sink->line.ga_data : (char_u *)"");
	sink->line.ga_len = 0;
    }
    if (sink == &fallback)
    {
	if (fallback.line.ga_len > 0)
	{
	    ga_append(&fallback.line, NUL);
	    sink_line(&fallback, (char_u *)fallback.line.ga_data);
	}
	ga_clear(&fallback.line);
    }
    sink_show(sink, FALSE);
}

static void sink_begin(sink_T *sink, sink_T **prev, int buf)
{
    CLEAR_POINTER(sink);
    sink->buf = buf;
    ga_init2(&sink->line, 1, 80);
    *prev = s_sink;
    s_sink = sink;
}

static void sink_end(sink_T *sink, sink_T *prev)
{
    if (sink->line.ga_len > 0)
    {
	ga_append(&sink->line, NUL);
	sink_line(sink, (char_u *)sink->line.ga_data);
    }
    ga_clear(&sink->line);
    s_sink = prev;
    if (sink->buf != 0)
	sink_show(sink, TRUE);
}

/* ------------------------------------------------------ Vim values -- */

static esp_py_kind_t host_kind(const void *v)
{
    const typval_T *tv = v;

    switch (tv->v_type)
    {
	case VAR_NUMBER:  return ESP_PY_V_NUMBER;
	case VAR_BOOL:    return ESP_PY_V_BOOL;
	case VAR_SPECIAL:
	case VAR_UNKNOWN:
	case VAR_VOID:    return ESP_PY_V_NONE;
	case VAR_FLOAT:   return ESP_PY_V_FLOAT;
	case VAR_STRING:  return ESP_PY_V_STRING;
	case VAR_BLOB:    return ESP_PY_V_BLOB;
	case VAR_LIST:    return ESP_PY_V_LIST;
	case VAR_TUPLE:   return ESP_PY_V_TUPLE;
	case VAR_DICT:    return ESP_PY_V_DICT;
	default:          return ESP_PY_V_OTHER;
    }
}

static int64_t host_number(const void *v)
{
    return (int64_t)((const typval_T *)v)->vval.v_number;
}

static double host_fnum(const void *v)
{
    return (double)((const typval_T *)v)->vval.v_float;
}

static char_u *s_other;         /* string() of the last OTHER value read */

static const char *host_bytes(const void *v, size_t *len)
{
    typval_T *tv = (typval_T *)v;
    char_u numbuf[NUMBUFLEN];
    char_u *tofree = NULL;

    if (tv->v_type == VAR_STRING)
    {
	char_u *s = tv->vval.v_string != NULL ? tv->vval.v_string : (char_u *)"";
	*len = STRLEN(s);
	return (const char *)s;
    }
    if (tv->v_type == VAR_BLOB)
    {
	blob_T *b = tv->vval.v_blob;
	*len = b != NULL ? (size_t)b->bv_ga.ga_len : 0;
	return b != NULL && b->bv_ga.ga_data != NULL ? b->bv_ga.ga_data : "";
    }
    /* A Funcref, a Job...: its string(). */
    VIM_CLEAR(s_other);
    char_u *s = tv2string(tv, &tofree, numbuf, 0);
    s_other = vim_strsave(s != NULL ? s : (char_u *)"");
    vim_free(tofree);
    *len = s_other != NULL ? STRLEN(s_other) : 0;
    return s_other != NULL ? (const char *)s_other : "";
}

static int host_each(const void *v, esp_py_each_fn fn, void *ctx)
{
    const typval_T *tv = v;

    if (tv->v_type == VAR_LIST)
    {
	list_T *l = tv->vval.v_list;
	listitem_T *li;

	if (l == NULL)
	    return 0;
	CHECK_LIST_MATERIALIZE(l);
	FOR_ALL_LIST_ITEMS(l, li)
	    if (fn(ctx, NULL, &li->li_tv))
		return 1;
    }
    else if (tv->v_type == VAR_TUPLE)
    {
	tuple_T *t = tv->vval.v_tuple;

	for (int i = 0; t != NULL && i < TUPLE_LEN(t); i++)
	    if (fn(ctx, NULL, TUPLE_ITEM(t, i)))
		return 1;
    }
    else if (tv->v_type == VAR_DICT && tv->vval.v_dict != NULL)
    {
	hashtab_T *ht = &tv->vval.v_dict->dv_hashtab;
	long todo = (long)ht->ht_used;
	hashitem_T *hi;

	FOR_ALL_HASHTAB_ITEMS(ht, hi, todo)
	{
	    if (HASHITEM_EMPTY(hi))
		continue;
	    --todo;
	    dictitem_T *di = HI2DI(hi);
	    if (fn(ctx, (const char *)di->di_key, &di->di_tv))
		return 1;
	}
    }
    return 0;
}

static void *host_new_value(void)
{
    typval_T *tv = ALLOC_CLEAR_ONE(typval_T);

    if (tv != NULL)
	tv->v_type = VAR_UNKNOWN;
    return tv;
}

static void host_free_value(void *v)
{
    if (v != NULL)
	free_tv((typval_T *)v);
}

static void host_set_none(void *v)
{
    typval_T *tv = v;
    tv->v_type = VAR_SPECIAL;
    tv->vval.v_number = VVAL_NONE;
}

static void host_set_bool(void *v, int b)
{
    typval_T *tv = v;
    tv->v_type = VAR_BOOL;
    tv->vval.v_number = b ? VVAL_TRUE : VVAL_FALSE;
}

static void host_set_number(void *v, int64_t n)
{
    typval_T *tv = v;
    tv->v_type = VAR_NUMBER;
    tv->vval.v_number = (varnumber_T)n;
}

static void host_set_float(void *v, double f)
{
    typval_T *tv = v;
    tv->v_type = VAR_FLOAT;
    tv->vval.v_float = (float_T)f;
}

/* A NUL ends a Vim String: what follows one is dropped. */
static int host_set_string(void *v, const char *s, size_t len)
{
    typval_T *tv = v;
    tv->v_type = VAR_STRING;
    tv->vval.v_string = vim_strnsave((char_u *)s, len);
    return tv->vval.v_string == NULL ? -1 : 0;
}

static int host_set_blob(void *v, const void *p, size_t len)
{
    blob_T *b = blob_alloc();

    if (b == NULL)
	return -1;
    if (len > 0)
    {
	if (ga_grow(&b->bv_ga, (int)len) == FAIL)
	{
	    blob_free(b);
	    return -1;
	}
	mch_memmove(b->bv_ga.ga_data, p, len);
	b->bv_ga.ga_len = (int)len;
    }
    rettv_blob_set((typval_T *)v, b);
    return 0;
}

static int host_set_list(void *v)
{
    return rettv_list_alloc((typval_T *)v) == OK ? 0 : -1;
}

static void *host_list_append(void *v)
{
    listitem_T *li = listitem_alloc();

    if (li == NULL)
	return NULL;
    li->li_tv.v_type = VAR_UNKNOWN;
    li->li_tv.v_lock = 0;
    list_append(((typval_T *)v)->vval.v_list, li);
    return &li->li_tv;
}

static int host_set_tuple(void *v, size_t n)
{
    return rettv_tuple_set_with_items((typval_T *)v, (int)n) == OK ? 0 : -1;
}

/* The item's value moves into the tuple, whose room set_tuple() made. */
static int host_tuple_add(void *v, void *item)
{
    tuple_T *t = ((typval_T *)v)->vval.v_tuple;

    tuple_set_item(t, TUPLE_LEN(t), (typval_T *)item);
    vim_free(item);
    return 0;
}

static int host_set_dict(void *v)
{
    return rettv_dict_alloc((typval_T *)v) == OK ? 0 : -1;
}

static void *host_dict_add(void *v, const char *key)
{
    dictitem_T *di = dictitem_alloc((char_u *)key);

    if (di == NULL)
	return NULL;
    di->di_tv.v_type = VAR_UNKNOWN;
    di->di_tv.v_lock = 0;
    if (dict_add(((typval_T *)v)->vval.v_dict, di) == FAIL)
    {
	dictitem_free(di);
	return NULL;
    }
    return &di->di_tv;
}

/* ----------------------------------------------------- Vim itself -- */

/* As Vim's Python interface does it (VimTryStart/VimTryEnd, if_py_both.h):
 * errors inside become an exception instead of a message. */
static void try_start(void)
{
    ++trylevel;
}

static int try_end(char *err, size_t errlen)
{
    --trylevel;
    did_emsg = FALSE;
    if (got_int)
    {
	if (did_throw)
	    discard_current_exception();
	got_int = FALSE;
	return -2;
    }
    if (msg_list != NULL && *msg_list != NULL)
    {
	int should_free = FALSE;
	char *m = get_exception_string(*msg_list, ET_ERROR, NULL, &should_free);

	vim_strncpy((char_u *)err, (char_u *)(m != NULL ? m : "error"), errlen - 1);
	free_global_msglist();
	if (should_free)
	    vim_free(m);
	return -1;
    }
    if (did_throw)
    {
	vim_strncpy((char_u *)err, (char_u *)current_exception->value, errlen - 1);
	discard_current_exception();
	return -1;
    }
    return 0;
}

static int host_command(const char *cmd, char *err, size_t errlen)
{
    try_start();
    do_cmdline_cmd((char_u *)cmd);
    return try_end(err, errlen);
}

static int host_eval(const char *expr, void *out, char *err, size_t errlen)
{
    try_start();
    typval_T *tv = eval_expr((char_u *)expr, NULL);
    int rc = try_end(err, errlen);

    if (rc != 0)
    {
	if (tv != NULL)
	    free_tv(tv);
	return rc;
    }
    if (tv == NULL)
    {
	vim_snprintf(err, errlen, "invalid expression: \"%s\"", expr);
	return -1;
    }
    *(typval_T *)out = *tv;         /* the value moves; its shell is freed */
    vim_free(tv);
    return 0;
}

static int host_call(const char *func, const void *args, void *out, char *err, size_t errlen)
{
    try_start();
    int ok = func_call((char_u *)func, (typval_T *)args, NULL, NULL, (typval_T *)out);
    int rc = try_end(err, errlen);

    if (rc == 0 && ok == FAIL)
    {
	vim_snprintf(err, errlen, "calling %s() failed", func);
	rc = -1;
    }
    return rc;
}

static int host_interrupted(void)
{
    ui_breakcheck();
    return got_int;
}

static const esp_py_host_t s_host = {
    .kind = host_kind,
    .number = host_number,
    .fnum = host_fnum,
    .bytes = host_bytes,
    .each = host_each,
    .new_value = host_new_value,
    .free_value = host_free_value,
    .set_none = host_set_none,
    .set_bool = host_set_bool,
    .set_number = host_set_number,
    .set_float = host_set_float,
    .set_string = host_set_string,
    .set_blob = host_set_blob,
    .set_list = host_set_list,
    .list_append = host_list_append,
    .set_tuple = host_set_tuple,
    .tuple_add = host_tuple_add,
    .set_dict = host_set_dict,
    .dict_add = host_dict_add,
    .command = host_command,
    .eval = host_eval,
    .call = host_call,
    .interrupted = host_interrupted,
    .write = host_write,
};

/* ------------------------------------------------------- builtins -- */

/* Zeroed with the rest of Vim's state at every session start (linker.lf). */
static int s_session_known;

/* The first esp_py_*() of a session: the interpreter is the last session's. */
static void session_check(void)
{
    if (!s_session_known)
    {
	esp_py_new_session();
	s_session_known = TRUE;
    }
}

static int start(const char *fn)
{
    char err[128];

    session_check();

    if (esp_py_start(&s_host, err, sizeof(err)) == 0)
	return OK;
    semsg("%s(): %s", fn, err);
    return FAIL;
}

/* The traceback's last line: "ZeroDivisionError: divide by zero". */
static char_u *last_line(const char *tb)
{
    const char *end = tb + strlen(tb);

    while (end > tb && (end[-1] == '\n' || end[-1] == '\r'))
	--end;
    const char *start = end;
    while (start > tb && start[-1] != '\n')
	--start;
    return vim_strnsave((char_u *)start, end - start);
}

/* A String, or a List of lines joined with "\n": allocated. */
static char_u *source_arg(typval_T *tv)
{
    if (tv->v_type != VAR_LIST)
	return vim_strsave(tv_get_string(tv));

    garray_T ga;
    listitem_T *li;
    ga_init2(&ga, 1, 256);
    if (tv->vval.v_list != NULL)
    {
	CHECK_LIST_MATERIALIZE(tv->vval.v_list);
	FOR_ALL_LIST_ITEMS(tv->vval.v_list, li)
	{
	    ga_concat(&ga, tv_get_string(&li->li_tv));
	    ga_append(&ga, '\n');
	}
    }
    ga_append(&ga, NUL);
    return (char_u *)ga.ga_data;
}

/*
 * esp_py_exec({source} [, {opts}]): run Python, in __main__. {source} is a
 * String or a List of lines. {opts}:
 *   name   the file name tracebacks give (default "<string>")
 *   buf    a buffer number: print() output goes to its end (default: messages)
 *   mode   "file" (default) or "single": one statement as typed at a prompt,
 *          which prints an expression's value
 * Returns {ok: 0/1, error: the traceback's last line, traceback: all of it}.
 */
void f_esp_py_exec(typval_T *argvars, typval_T *rettv)
{
    if (rettv_dict_alloc(rettv) == FAIL)
	return;
    if (check_for_string_or_list_arg(argvars, 0) == FAIL
	    || check_for_opt_dict_arg(argvars, 1) == FAIL)
	return;

    dict_T *opts = argvars[1].v_type == VAR_DICT ? argvars[1].vval.v_dict : NULL;
    char_u *name = opts != NULL ? dict_get_string(opts, "name", TRUE) : NULL;
    int buf = opts != NULL ? (int)dict_get_number(opts, "buf") : 0;
    char_u *mode = opts != NULL ? dict_get_string(opts, "mode", FALSE) : NULL;
    esp_py_mode_t m = mode != NULL && STRCMP(mode, "single") == 0 ? ESP_PY_SINGLE : ESP_PY_FILE;
    char_u *src = source_arg(&argvars[0]);
    char *tb = NULL;
    int rc = -1;

    if (src != NULL && start("esp_py_exec") == OK)
    {
	sink_T sink, *prev;

	sink_begin(&sink, &prev, buf);
	rc = esp_py_exec((char *)src, STRLEN(src),
			 name != NULL ? (char *)name : "<string>", m, &tb);
	sink_end(&sink, prev);
	got_int = FALSE;            /* a CTRL-C stopped Python, not the caller */
    }

    dict_add_number(rettv->vval.v_dict, "ok", rc == 0);
    dict_add_string(rettv->vval.v_dict, "traceback", (char_u *)(tb != NULL ? tb : ""));
    char_u *err = last_line(tb != NULL ? tb : "");
    dict_add_string(rettv->vval.v_dict, "error", err);
    vim_free(err);
    free(tb);
    vim_free(src);
    vim_free(name);
}

/* esp_py_eval({expr}): a Python expression's value, as a Vim value. */
void f_esp_py_eval(typval_T *argvars, typval_T *rettv)
{
    if (check_for_string_arg(argvars, 0) == FAIL || start("esp_py_eval") == FAIL)
	return;

    char_u *expr = tv_get_string(&argvars[0]);
    char *tb = NULL;
    sink_T sink, *prev;

    sink_begin(&sink, &prev, 0);
    int rc = esp_py_eval((char *)expr, STRLEN(expr), rettv, &tb);
    sink_end(&sink, prev);
    got_int = FALSE;
    if (rc != 0)
    {
	clear_tv(rettv);
	rettv->v_type = VAR_NUMBER;
	rettv->vval.v_number = 0;
	char_u *err = last_line(tb != NULL ? tb : "Python failed");
	semsg("esp_py_eval(): %s", err);
	vim_free(err);
    }
    free(tb);
}

/* esp_py_reset(): discard every Python variable, module and open file. */
void f_esp_py_reset(typval_T *argvars UNUSED, typval_T *rettv)
{
    session_check();
    rettv->vval.v_number = esp_py_reset() == 0;
    if (!rettv->vval.v_number)
	emsg("esp_py_reset(): not from inside Python");
}

/* esp_py_heap(): {total, used, free, max_free, running} (bytes). */
void f_esp_py_heap(typval_T *argvars UNUSED, typval_T *rettv)
{
    esp_py_heap_t h;

    if (rettv_dict_alloc(rettv) == FAIL)
	return;
    session_check();
    esp_py_heap(&h);
    dict_add_number(rettv->vval.v_dict, "total", (varnumber_T)h.total);
    dict_add_number(rettv->vval.v_dict, "used", (varnumber_T)h.used);
    dict_add_number(rettv->vval.v_dict, "free", (varnumber_T)h.free);
    dict_add_number(rettv->vval.v_dict, "max_free", (varnumber_T)h.max_free);
    dict_add_number(rettv->vval.v_dict, "running", h.running);
}
