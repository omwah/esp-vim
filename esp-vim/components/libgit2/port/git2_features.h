/*
 * libgit2's build configuration for ESP-IDF, written by hand in place of the
 * one upstream's CMake generates from git2_features.h.in (which probes the
 * host). See components/libgit2/CMakeLists.txt.
 */
#ifndef INCLUDE_features_h__
#define INCLUDE_features_h__

#define GIT_ARCH_32 1

/* newlib's regcomp/regexec, rather than the bundled PCRE2 (~200 KB). */
#define GIT_REGEX_REGCOMP 1

/* newlib's qsort_r takes the GNU argument order under _GNU_SOURCE. */
#define GIT_QSORT_GNU 1

#define GIT_SSH 1
#define GIT_SSH_LIBSSH2 1
#define GIT_SSH_LIBSSH2_MEMORY_CREDENTIALS 1

#define GIT_HTTPS 1
#define GIT_MBEDTLS 1
#define GIT_HTTPPARSER_BUILTIN 1

#define GIT_SHA1_MBEDTLS 1
#define GIT_SHA256_MBEDTLS 1

#define GIT_COMPRESSION_BUILTIN 1

#define GIT_RAND_GETENTROPY 1

/* ESP-IDF's poll() (newlib/src/poll.c, over select()). */
#define GIT_IO_POLL 1

#endif
