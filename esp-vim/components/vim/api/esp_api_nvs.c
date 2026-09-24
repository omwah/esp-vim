/*
 * esp_nvs_get(), esp_nvs_set(), esp_nvs_erase(), esp_nvs_list(): the "nvs"
 * partition, behind :EspNvs.
 *
 * Values are stored as strings or 64-bit integers, following the Vim type.
 * Reading also understands the other integer types ESP-IDF components write,
 * so :EspNvs can show everything on the partition; blobs are listed by size.
 */

#include "vim.h"
#include "esp_vim_api.h"

#include "nvs.h"
#include "nvs_flash.h"

static void nvs_fail(const char *fn, const char *ns, const char *key, esp_err_t err)
{
    semsg("%s(\"%s\", \"%s\"): %s", fn, ns, key, esp_err_to_name(err));
}

/* Store an entry's value in *tv. Strings and integers only; FAIL for blobs. */
static int get_value(nvs_handle_t h, const char *key, nvs_type_t type, typval_T *tv)
{
    esp_err_t err = ESP_OK;
    varnumber_T n = 0;
    switch (type) {
    case NVS_TYPE_STR: {
        size_t len = 0;
        if ((err = nvs_get_str(h, key, NULL, &len)) != ESP_OK)
            return FAIL;
        char_u *s = alloc(len);
        if (s == NULL || (err = nvs_get_str(h, key, (char *)s, &len)) != ESP_OK) {
            vim_free(s);
            return FAIL;
        }
        tv->v_type = VAR_STRING;
        tv->vval.v_string = s;
        return OK;
    }
    case NVS_TYPE_I8:  { int8_t v;   err = nvs_get_i8(h, key, &v);  n = v; break; }
    case NVS_TYPE_U8:  { uint8_t v;  err = nvs_get_u8(h, key, &v);  n = v; break; }
    case NVS_TYPE_I16: { int16_t v;  err = nvs_get_i16(h, key, &v); n = v; break; }
    case NVS_TYPE_U16: { uint16_t v; err = nvs_get_u16(h, key, &v); n = v; break; }
    case NVS_TYPE_I32: { int32_t v;  err = nvs_get_i32(h, key, &v); n = v; break; }
    case NVS_TYPE_U32: { uint32_t v; err = nvs_get_u32(h, key, &v); n = v; break; }
    case NVS_TYPE_I64: { int64_t v;  err = nvs_get_i64(h, key, &v); n = v; break; }
    case NVS_TYPE_U64: { uint64_t v; err = nvs_get_u64(h, key, &v); n = (varnumber_T)v; break; }
    default:
        return FAIL;
    }
    if (err != ESP_OK)
        return FAIL;
    tv->v_type = VAR_NUMBER;
    tv->vval.v_number = n;
    return OK;
}

/* The type of an existing key, or NVS_TYPE_ANY when there is none. */
static nvs_type_t key_type(const char *ns, const char *key)
{
    nvs_entry_info_t info;
    nvs_iterator_t it = NULL;
    nvs_type_t t = NVS_TYPE_ANY;
    esp_err_t err = nvs_entry_find(NVS_DEFAULT_PART_NAME, ns, NVS_TYPE_ANY, &it);
    while (err == ESP_OK) {
        nvs_entry_info(it, &info);
        if (strcmp(info.key, key) == 0) {
            t = info.type;
            break;
        }
        err = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    return t;
}

/* esp_nvs_get({namespace}, {key} [, {default}]): the value, or {default}
 * (else an error) when the key does not exist. */
void f_esp_nvs_get(typval_T *argvars, typval_T *rettv)
{
    char_u nsbuf[NUMBUFLEN];
    const char *ns = (const char *)tv_get_string_buf(&argvars[0], nsbuf);
    const char *key = (const char *)tv_get_string(&argvars[1]);
    bool has_default = argvars[2].v_type != VAR_UNKNOWN;

    nvs_type_t type = key_type(ns, key);
    nvs_handle_t h;
    esp_err_t err = ESP_ERR_NVS_NOT_FOUND;
    if (type != NVS_TYPE_ANY && (err = nvs_open(ns, NVS_READONLY, &h)) == ESP_OK) {
        int ok = get_value(h, key, type, rettv);
        nvs_close(h);
        if (ok == OK)
            return;
        err = type == NVS_TYPE_BLOB ? ESP_ERR_NVS_TYPE_MISMATCH : ESP_FAIL;
    }
    if (has_default && err == ESP_ERR_NVS_NOT_FOUND)
        copy_tv(&argvars[2], rettv);
    else
        nvs_fail("esp_nvs_get", ns, key, err);
}

/* esp_nvs_set({namespace}, {key}, {value}): a Number is stored as a 64-bit
 * integer, anything else as a string. Replaces a key of another type. */
void f_esp_nvs_set(typval_T *argvars, typval_T *rettv)
{
    char_u nsbuf[NUMBUFLEN], keybuf[NUMBUFLEN];
    const char *ns = (const char *)tv_get_string_buf(&argvars[0], nsbuf);
    const char *key = (const char *)tv_get_string_buf(&argvars[1], keybuf);

    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        bool is_num = argvars[2].v_type == VAR_NUMBER || argvars[2].v_type == VAR_BOOL;
        nvs_type_t old = key_type(ns, key);
        if (old != NVS_TYPE_ANY && old != (is_num ? NVS_TYPE_I64 : NVS_TYPE_STR))
            nvs_erase_key(h, key);      /* NVS refuses a set that changes the type */
        if (is_num)
            err = nvs_set_i64(h, key, tv_get_number(&argvars[2]));
        else
            err = nvs_set_str(h, key, (const char *)tv_get_string(&argvars[2]));
        if (err == ESP_OK)
            err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK)
        nvs_fail("esp_nvs_set", ns, key, err);
    rettv->vval.v_number = err == ESP_OK;
}

/* esp_nvs_erase({namespace}, {key}): TRUE if it existed. */
void f_esp_nvs_erase(typval_T *argvars, typval_T *rettv)
{
    char_u nsbuf[NUMBUFLEN];
    const char *ns = (const char *)tv_get_string_buf(&argvars[0], nsbuf);
    const char *key = (const char *)tv_get_string(&argvars[1]);

    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND)
        return;                         /* no such namespace: nothing to erase */
    if (err == ESP_OK) {
        err = nvs_erase_key(h, key);
        if (err == ESP_OK)
            err = nvs_commit(h);
        nvs_close(h);
    }
    if (err == ESP_ERR_NVS_NOT_FOUND)
        return;
    if (err != ESP_OK)
        nvs_fail("esp_nvs_erase", ns, key, err);
    rettv->vval.v_number = err == ESP_OK;
}

static const char *nvs_type_str(nvs_type_t t)
{
    switch (t) {
    case NVS_TYPE_STR:  return "string";
    case NVS_TYPE_BLOB: return "blob";
    case NVS_TYPE_I8:   return "i8";
    case NVS_TYPE_U8:   return "u8";
    case NVS_TYPE_I16:  return "i16";
    case NVS_TYPE_U16:  return "u16";
    case NVS_TYPE_I32:  return "i32";
    case NVS_TYPE_U32:  return "u32";
    case NVS_TYPE_I64:  return "i64";
    case NVS_TYPE_U64:  return "u64";
    default:            return "?";
    }
}

/* esp_nvs_list([{namespace}]) -> List of Dicts: namespace, key, type, and
 * value (for blobs, size instead). */
void f_esp_nvs_list(typval_T *argvars, typval_T *rettv)
{
    if (rettv_list_alloc(rettv) == FAIL)
        return;
    const char *only = argvars[0].v_type == VAR_UNKNOWN ? NULL
                     : (const char *)tv_get_string(&argvars[0]);

    nvs_iterator_t it = NULL;
    esp_err_t err = nvs_entry_find(NVS_DEFAULT_PART_NAME, only, NVS_TYPE_ANY, &it);
    while (err == ESP_OK) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        dict_T *e = dict_alloc();
        if (e == NULL)
            break;
        dict_add_string(e, "namespace", (char_u *)info.namespace_name);
        dict_add_string(e, "key", (char_u *)info.key);
        dict_add_string(e, "type", (char_u *)nvs_type_str(info.type));
        nvs_handle_t h;
        if (nvs_open(info.namespace_name, NVS_READONLY, &h) == ESP_OK) {
            typval_T tv = {0};
            size_t size = 0;
            if (info.type == NVS_TYPE_BLOB) {
                if (nvs_get_blob(h, info.key, NULL, &size) == ESP_OK)
                    dict_add_number(e, "size", size);
            } else if (get_value(h, info.key, info.type, &tv) == OK) {
                dict_add_tv(e, "value", &tv);
                clear_tv(&tv);
            }
            nvs_close(h);
        }
        list_append_dict(rettv->vval.v_list, e);
        err = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
}
