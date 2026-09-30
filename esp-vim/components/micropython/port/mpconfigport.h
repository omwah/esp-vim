/*
 * MicroPython's configuration for esp-vim: an interpreter embedded in Vim.
 *
 * It runs on the Vim task, is entered only through esp_py_*() (esp_py.c), and
 * talks to the device through Vim: there is no REPL on the console, no
 * `machine`, no sockets, no threads. Files are the ordinary POSIX calls
 * (VfsPosix mounted at "/"), which the firmware reroutes at link time so that
 * relative paths follow Vim's working directory (components/vim/port/esp_shims.c).
 *
 * This file is also read by the qstr preprocessing pass (py/mkrules.cmake),
 * which runs with MicroPython's include paths only: nothing here may include
 * an ESP-IDF header.
 */

#include <stdint.h>
#include <alloca.h>

#define MICROPY_CONFIG_ROM_LEVEL            (MICROPY_CONFIG_ROM_LEVEL_EXTRA_FEATURES)

/* Core. */
#define MICROPY_ENABLE_COMPILER             (1)
#define MICROPY_ENABLE_GC                   (1)
#define MICROPY_ENABLE_FINALISER            (1)     /* VfsPosix files close when collected */
#define MICROPY_HELPER_REPL                 (1)     /* :EspPy prints an expression's value */
#define MICROPY_ENABLE_SCHEDULER            (1)
#define MICROPY_KBD_EXCEPTION               (1)     /* CTRL-C raises KeyboardInterrupt */
#define MICROPY_STACK_CHECK                 (1)
#define MICROPY_STACK_CHECK_MARGIN          (12 * 1024) /* room for Vim under vim.command() */
#define MICROPY_LONGINT_IMPL                (MICROPY_LONGINT_IMPL_MPZ)
/* Double, as Vim's Float is: a value passed through Python keeps its digits. */
#define MICROPY_FLOAT_IMPL                  (MICROPY_FLOAT_IMPL_DOUBLE)
#define MICROPY_ERROR_REPORTING             (MICROPY_ERROR_REPORTING_DETAILED)
#define MICROPY_WARNINGS                    (1)
#define MICROPY_USE_INTERNAL_PRINTF         (0)
#define MICROPY_USE_INTERNAL_ERRNO          (0)
#define MICROPY_PY_THREAD                   (0)
/* Xtensa's windowed ABI: setjmp/longjmp, as MicroPython's ESP32 port uses
 * (py/nlrxtensa.c is for the call0 ABI). RISC-V has its own nlr_push. */
#if defined(__XTENSA__)
#define MICROPY_NLR_SETJMP                  (1)
#endif
#define MICROPY_PERSISTENT_CODE_LOAD        (1)     /* import a .mpy */
/* vim and esp are frozen into the firmware (manifest.py): bytecode and its
 * qstrs in flash, found through ".frozen" in sys.path. */
#define MICROPY_MODULE_FROZEN_MPY           (1)
#define MICROPY_QSTR_EXTRA_POOL             mp_qstr_frozen_const_pool
#define MICROPY_MODULE_GETATTR              (1)     /* esp.<anything> */
#define MICROPY_CAN_OVERRIDE_BUILTINS       (1)
#define MICROPY_ENABLE_EXTERNAL_IMPORT      (1)
#define MICROPY_READER_VFS                  (1)
#define MICROPY_ALLOC_PATH_MAX              (256)

/* Every 1000 jumps and returns, esp_py_poll() looks for CTRL-C (at most every
 * 20 ms: it checks the clock first). */
#define MICROPY_VM_HOOK_COUNT               (1000)
#define MICROPY_VM_HOOK_INIT                static unsigned int vm_hook_divisor = MICROPY_VM_HOOK_COUNT;
#define MICROPY_VM_HOOK_POLL                if (--vm_hook_divisor == 0) { \
        vm_hook_divisor = MICROPY_VM_HOOK_COUNT; \
        extern void esp_py_poll(void); \
        esp_py_poll(); \
}
#define MICROPY_VM_HOOK_LOOP                MICROPY_VM_HOOK_POLL
#define MICROPY_VM_HOOK_RETURN              MICROPY_VM_HOOK_POLL

/* Files: the POSIX calls, through a VfsPosix mounted at "/". */
#define MICROPY_VFS                         (1)
#define MICROPY_VFS_POSIX                   (1)
#define MICROPY_VFS_POSIX_NO_SYS_STDFILES   (1)     /* sys.stdout is ours (patch 0001) */
#define MICROPY_VFS_FAT                     (0)
#define MICROPY_VFS_LFS1                    (0)
#define MICROPY_VFS_LFS2                    (0)
#define MICROPY_VFS_ROM                     (0)
#define MICROPY_PY_OS                       (1)
#define MICROPY_PY_OS_STATVFS               (0)
#define MICROPY_PY_OS_UNAME                 (0)
#define MICROPY_PY_OS_URANDOM               (1)
#define MICROPY_PY_OS_GETENV_PUTENV_UNSETENV (0)
#define MICROPY_PY_OS_SYSTEM                (0)
#define MICROPY_PY_OS_DUPTERM               (0)
#define MICROPY_PY_OS_SYNC                  (0)
#define MICROPY_PY_OS_ERRNO                 (0)
#define MICROPY_PY_IO                       (1)

/* sys: print() and sys.stdout go to Vim; there is no stdin. */
#define MICROPY_PY_SYS                      (1)
#define MICROPY_PY_SYS_PATH                 (1)
#define MICROPY_PY_SYS_ARGV                 (1)
#define MICROPY_PY_SYS_EXIT                 (1)
#define MICROPY_PY_SYS_STDFILES             (1)
#define MICROPY_PY_SYS_STDIO_BUFFER         (0)
#define MICROPY_PY_SYS_PLATFORM             "esp-vim"
#define MICROPY_PY_BUILTINS_INPUT           (0)     /* ours, in the builtins below */
#define MICROPY_PY_BUILTINS_HELP            (1)
#define MICROPY_PY_MICROPYTHON_MEM_INFO     (1)

/* Modules that are pure computation. */
#define MICROPY_PY_MATH                     (1)
#define MICROPY_PY_CMATH                    (1)
#define MICROPY_PY_JSON                     (1)
#define MICROPY_PY_RE                       (1)
#define MICROPY_PY_RANDOM                   (1)
#define MICROPY_PY_RANDOM_EXTRA_FUNCS       (1)
#define MICROPY_PY_RANDOM_SEED_INIT_FUNC    (esp_py_random_seed())
#define MICROPY_PY_BINASCII                 (1)
#define MICROPY_PY_HEAPQ                    (1)
#define MICROPY_PY_HASHLIB                  (1)
#define MICROPY_PY_HASHLIB_SHA1             (1)
#define MICROPY_PY_HASHLIB_SHA256           (1)
#define MICROPY_PY_HASHLIB_MD5              (1)
#define MICROPY_SSL_MBEDTLS                 (1)     /* hashlib on ESP-IDF's mbedTLS */
#define MICROPY_PY_DEFLATE                  (1)
#define MICROPY_PY_DEFLATE_COMPRESS         (1)
#define MICROPY_PY_STRUCT                   (1)
#define MICROPY_PY_COLLECTIONS              (1)
#define MICROPY_PY_ERRNO                    (1)
#define MICROPY_PY_UCTYPES                  (0)
#define MICROPY_PY_ARRAY                    (1)

/* time: sleep() slices itself so CTRL-C gets through (mphal_esp.c). */
#define MICROPY_PY_TIME                     (1)
#define MICROPY_PY_TIME_TIME_TIME_NS        (1)
#define MICROPY_PY_TIME_GMTIME_LOCALTIME_MKTIME (1)
#define MICROPY_PY_TIME_INCLUDEFILE         "port/modtime_esp.h"
#define MICROPY_EPOCH_IS_1970               (1)

/* What this port does not have. */
#define MICROPY_PY_MACHINE                  (0)
#define MICROPY_PY_NETWORK                  (0)
#define MICROPY_PY_SOCKET                   (0)
#define MICROPY_PY_SSL                      (0)
#define MICROPY_PY_SELECT                   (0)
#define MICROPY_PY_ASYNCIO                  (0)
#define MICROPY_PY_BLUETOOTH                (0)
#define MICROPY_PY_FRAMEBUF                 (0)
#define MICROPY_PY_BTREE                    (0)
#define MICROPY_PY_ONEWIRE                  (0)
#define MICROPY_PY_WEBSOCKET                (0)
#define MICROPY_PY_WEBREPL                  (0)
#define MICROPY_PY_CRYPTOLIB                (0)
#define MICROPY_PY_PLATFORM                 (0)
#define MICROPY_PY_VFS                      (1)

/* Builtins: input() asks through Vim's input(). */
extern const struct _mp_obj_fun_builtin_var_t esp_py_input_obj;
#define MICROPY_PORT_BUILTINS \
    { MP_ROM_QSTR(MP_QSTR_input), MP_ROM_PTR(&esp_py_input_obj) },

/* Machine. */
#define MICROPY_MAKE_POINTER_CALLABLE(p)    ((void *)((mp_uint_t)(p)))
#define MP_SSIZE_MAX                        (0x7fffffff)
typedef int mp_int_t;
typedef unsigned int mp_uint_t;
typedef long mp_off_t;

#define MICROPY_HW_BOARD_NAME               "esp-vim"
#define MICROPY_HW_MCU_NAME                 "ESP32"

uint32_t esp_py_random_seed(void);

#define MICROPY_MPHALPORT_H                 "port/mphalport.h"
