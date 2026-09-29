# libgit2 as an ESP-IDF component.
#
# Upstream's own CMake is not used: it probes the host. The source lists below
# follow its globs (src/libgit2, src/util, and the bundled deps), with the
# platform choices made once in port/git2_features.h, as Vim's are in its
# config.h:
#   - no threads (git_libgit2_init once, from one task at a time);
#   - no mmap: NO_MMAP selects upstream's read-into-memory fallback;
#   - SHA-1, SHA-256 and HTTPS from ESP-IDF's mbedTLS;
#   - SSH from libssh2 (skuodi/libssh2_esp, as esp_ssh uses);
#   - regcomp/regexec from newlib, the bundled zlib and llhttp.
#
# Source is NOT in this repo: it is extracted and patched into build-deps/ by
# scripts/prepare-deps.sh. See docs/PLAN.md, stage 6z.

cmake_minimum_required(VERSION 3.16)

set(GIT2_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../build-deps/libgit2")
if(NOT EXISTS "${GIT2_ROOT}/src/libgit2/repository.c")
    message(FATAL_ERROR
        "libgit2 source not found at ${GIT2_ROOT}.\n"
        "Run: pixi run deps")
endif()

file(GLOB GIT2_SRC
    "${GIT2_ROOT}/src/libgit2/*.c"
    "${GIT2_ROOT}/src/libgit2/streams/*.c"
    "${GIT2_ROOT}/src/libgit2/transports/*.c"
    "${GIT2_ROOT}/src/util/*.c"
    "${GIT2_ROOT}/src/util/allocators/*.c"
    "${GIT2_ROOT}/src/util/hash/mbedtls.c"
    "${GIT2_ROOT}/src/util/unix/realpath.c"
    "${GIT2_ROOT}/deps/llhttp/*.c"
    "${GIT2_ROOT}/deps/xdiff/*.c"
    "${GIT2_ROOT}/deps/zlib/*.c")
# Windows, macOS and backends this build does not select. (Each also guards
# itself with #ifdef; leaving them out keeps the list honest. streams/openssl.c
# stays: without OpenSSL it is the stub libgit2.c's init table calls.)
list(FILTER GIT2_SRC EXCLUDE REGEX
    "/(winhttp|auth_sspi|auth_gssapi|auth_ntlmclient|ssh_exec|openssl_[a-z]*|schannel|stransport)\\.c$")
list(FILTER GIT2_SRC EXCLUDE REGEX "/allocators/win32_leakcheck\\.c$")
list(SORT GIT2_SRC)
list(APPEND GIT2_SRC "port/esp_port.c")

idf_component_register(
    SRCS ${GIT2_SRC}
    INCLUDE_DIRS "${GIT2_ROOT}/include"
    PRIV_INCLUDE_DIRS "port"
                      "${GIT2_ROOT}/src/libgit2"
                      "${GIT2_ROOT}/src/util"
                      "${GIT2_ROOT}/deps/llhttp"
                      "${GIT2_ROOT}/deps/xdiff"
                      "${GIT2_ROOT}/deps/zlib"
    REQUIRES mbedtls
    PRIV_REQUIRES skuodi__libssh2_esp lwip esp_hw_support newlib
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    _GNU_SOURCE
    NO_MMAP
    NO_READDIR_R
    GIT_DEPRECATE_HARD)
target_compile_options(${COMPONENT_LIB} PRIVATE
    -include "${CMAKE_CURRENT_LIST_DIR}/port/esp_port.h"
    -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable
    -Wno-implicit-fallthrough -Wno-sign-compare -Wno-format
    -Wno-deprecated-declarations -Wno-char-subscripts
    -Wno-maybe-uninitialized -Wno-missing-field-initializers
    # int32_t is long on this toolchain: libgit2 passes int * for int32_t *
    # (the same 32 bits).
    -Wno-incompatible-pointer-types)
