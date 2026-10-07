/*
 * The esp_*() builtins of features left out of this build (main/Kconfig.projbuild,
 * "esp-vim features"): git, MicroPython, SSH, the web interface.
 *
 * Vim's builtin table (patches/vim/0008) names every one of them in every
 * build, so each still needs a function. Here each reports that its feature is
 * missing and returns nothing; the :Esp* command calling it then just fails
 * with that message. The two that run without being asked -- esp_settings()
 * at every start, esp_py_heap() in :EspHeap -- answer with defaults instead.
 * The real ones are in esp_api_<feature>.c, built instead.
 */

#include "vim.h"
#include "esp_vim_api.h"

#define ABSENT(fn, feature)                                                 \
    void f_##fn(typval_T *argvars UNUSED, typval_T *rettv UNUSED)          \
    {                                                                       \
        semsg("%s(): %s is not in this build", #fn, feature);               \
    }

#if !CONFIG_ESP_VIM_GIT
ABSENT(esp_git_init, "git")
ABSENT(esp_git_status, "git")
ABSENT(esp_git_add, "git")
ABSENT(esp_git_reset, "git")
ABSENT(esp_git_restore, "git")
ABSENT(esp_git_commit, "git")
ABSENT(esp_git_log, "git")
ABSENT(esp_git_diff, "git")
ABSENT(esp_git_show, "git")
ABSENT(esp_git_branch, "git")
ABSENT(esp_git_checkout, "git")
ABSENT(esp_git_config, "git")
ABSENT(esp_git_remote, "git")
ABSENT(esp_git_clone, "git")
ABSENT(esp_git_fetch, "git")
ABSENT(esp_git_pull, "git")
ABSENT(esp_git_push, "git")
ABSENT(esp_git_gc, "git")
#endif

#if !CONFIG_ESP_VIM_PYTHON
ABSENT(esp_py_exec, "Python")
ABSENT(esp_py_eval, "Python")
ABSENT(esp_py_more, "Python")
ABSENT(esp_py_complete, "Python")
ABSENT(esp_py_reset, "Python")

/* :EspHeap asks every build: no interpreter, so nothing running. */
void f_esp_py_heap(typval_T *argvars UNUSED, typval_T *rettv)
{
    if (rettv_dict_alloc(rettv) == FAIL)
        return;
    dict_add_number(rettv->vval.v_dict, "total", 0);
    dict_add_number(rettv->vval.v_dict, "used", 0);
    dict_add_number(rettv->vval.v_dict, "free", 0);
    dict_add_number(rettv->vval.v_dict, "max_free", 0);
    dict_add_number(rettv->vval.v_dict, "running", 0);
}
#endif

#if !CONFIG_ESP_VIM_SSH
ABSENT(esp_ssh_hostkey, "SSH")
ABSENT(esp_ssh_trust, "SSH")
ABSENT(esp_ssh_get, "SSH")
ABSENT(esp_ssh_put, "SSH")
ABSENT(esp_ssh_list, "SSH")
ABSENT(esp_ssh_mkdir, "SSH")
ABSENT(esp_ssh_remove, "SSH")
ABSENT(esp_ssh_rename, "SSH")
ABSENT(esp_ssh_keygen, "SSH")
#endif

#if !CONFIG_ESP_VIM_WEB
ABSENT(esp_web_start, "the web interface")
ABSENT(esp_web_stop, "the web interface")
ABSENT(esp_web_info, "the web interface")
ABSENT(esp_web_passwd, "the web interface")
ABSENT(esp_web_publish, "the web interface")
ABSENT(esp_web_settings_changed, "the web interface")

/* plugin/esp.vim reads these at every start: with no web interface to change
 * them, they are always Vim's own defaults. */
void f_esp_settings(typval_T *argvars UNUSED, typval_T *rettv)
{
    if (rettv_dict_alloc(rettv) == FAIL)
        return;
    dict_T *d = rettv->vval.v_dict;
    dict_add_number(d, "tabstop", 8);
    dict_add_number(d, "shiftwidth", 8);
    dict_add_bool(d, "expandtab", FALSE);
    dict_add_bool(d, "number", FALSE);
    dict_add_bool(d, "relativenumber", FALSE);
    dict_add_bool(d, "wrap", TRUE);
    dict_add_string(d, "colorscheme", (char_u *)"");
    dict_add_string(d, "background", (char_u *)"");
}
#endif
