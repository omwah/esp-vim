/*
 * auto/config.h replacement for the ESP-IDF build.
 *
 * Vim's autoconf cannot cross-compile: its probes are AC_TRY_RUN tests that must
 * execute on the target. So `configure` is run once on the HOST as a generator
 * (`pixi run configure-vim`) and its output is curated by hand into this file.
 *
 * Every deviation from the host's answer is deliberate and commented. The
 * evidence is Phase 1 (docs/PHASE1.md), which probed the real target under the
 * emulator, plus direct probes of the riscv32-esp-elf sysroot.
 *
 * Do not regenerate this file. Re-run configure only to diff against it when
 * moving to a newer Vim.
 */

#ifndef ESP_VIM_CONFIG_H
#define ESP_VIM_CONFIG_H

/* ------------------------------------------------------------------ target -- */

#define UNIX                    /* Vim's closest-fitting platform family */
#define FEAT_NORMAL             /* +eval +syntax +quickfix (docs/PLAN.md) */

/*
 * Probed with riscv32-esp-elf-gcc, not assumed. Note the combination that
 * catches people out: a 32-bit target with a 64-bit time_t and a 32-bit off_t.
 */
#define VIM_SIZEOF_INT      4
#define VIM_SIZEOF_LONG     4
#define SIZEOF_OFF_T        4
#define SIZEOF_TIME_T       8

#define HAVE_ATTRIBUTE_UNUSED
#define USEMEMMOVE
#define RETSIGTYPE void
#define SIGRETURN return

/* ----------------------------------------------------------------- termcap -- */

/*
 * The single most consequential setting here. With HAVE_TGETENT undefined,
 * term.c uses only its compiled-in termcaps (builtin_xterm, builtin_ansi,
 * builtin_dumb) and Vim needs no terminal library at all -- so there is no
 * ncurses/terminfo port to do. TERM=xterm resolves entirely in-binary.
 */
/* #undef HAVE_TGETENT */
/* #undef TERMINFO */
/* #undef HAVE_TERMCAP_H */
/* #undef HAVE_OSPEED */
/* #undef HAVE_UP_BC_PC */
/* #undef HAVE_DEL_CURTERM */
/* #undef TGETENT_ZERO_ERR */

#define HAVE_TERMIOS_H          /* via IDF's newlib overlay; CONFIG_VFS_SUPPORT_TERMIOS=y */
/* #undef HAVE_SGTTY_H */

/* ------------------------------------------------------------------- procs -- */

/*
 * No fork() on ESP-IDF -- confirmed absent in Phase 1. USE_SYSTEM routes
 * mch_call_shell() through system(), which the port stubs to report E371.
 * Everything pty- and job-related follows from this.
 */
#define USE_SYSTEM
/* #undef HAVE_FORK */
/* #undef HAVE_SETPGID */
/* #undef HAVE_SETSID */
/* #undef HAVE_GETPGID */
/* #undef HAVE_UNION_WAIT */
/* #undef HAVE_SVR4_PTYS */
/* #undef HAVE_POSIX_OPENPT */
/* #undef HAVE_SYS_PTMS_H */

/*
 * signal() does not link (Phase 1). CTRL-C therefore arrives as byte 0x03 in
 * the input stream and the reader must set got_int -- there is no other path.
 */
/* #undef HAVE_SIGACTION */
/* #undef HAVE_SIGSET */
/* #undef HAVE_SIGPROCMASK */
/* #undef HAVE_SIGALTSTACK */
/* #undef HAVE_SIGSTACK */
/* #undef HAVE_SIGCONTEXT */
/* #undef HAVE_SYSCONF_SIGSTKSZ */

/* ------------------------------------------------------------------ users -- */

/*
 * All absent (Phase 1). port/esp_shims.c supplies stand-ins so '~' expands and
 * mch_get_user_name() answers something.
 */
/* #undef HAVE_GETPWENT */
/* #undef HAVE_GETPWNAM */
/* #undef HAVE_GETPWUID */
#define HAVE_PWD_H              /* the header exists; the functions do not */

/* --------------------------------------------------------------- file I/O -- */

#define HAVE_DIRENT_H
#define HAVE_OPENDIR
#define HAVE_RENAME
#define HAVE_FSYNC
#define HAVE_FTRUNCATE
#define HAVE_FSEEKO
#define HAVE_ST_BLKSIZE         /* probed: struct stat has st_blksize */
#define ST_MTIM_NSEC st_mtim.tv_nsec    /* probed: present */

/*
 * Absent (Phase 1): there are no symlinks on FATFS, and lstat is not even
 * DECLARED in IDF's headers -- a compile error before it is a link error.
 */
/* #undef HAVE_LSTAT */
/* #undef HAVE_READLINK */
/* #undef HAVE_LINK */
/* #undef HAVE_FCHOWN */
/* #undef HAVE_FCHMOD */
/* #undef HAVE_FLOCK */
/* #undef HAVE_XATTR */
/* #undef HAVE_SYNC */
/* #undef HAVE_MKDTEMP */
/* #undef HAVE_DIRFD */

/*
 * ESP-IDF has NO working directory: chdir() is newlib's ENOSYS stub and
 * getcwd() always answers "/". There is no implementation in IDF v5.5.5 to
 * enable. The port supplies a userspace CWD and redefines mch_open/mch_fopen
 * (see the bottom of this file) so relative paths are resolved before they
 * reach the VFS. HAVE_GETCWD stays defined because mch_dirname() uses it --
 * it resolves to the port's getcwd, not newlib's.
 */
#define HAVE_GETCWD
/* #undef HAVE_FCHDIR */

/* ---------------------------------------------------------------- strings -- */

#define HAVE_MEMSET
#define HAVE_STRCASECMP
#define HAVE_STRNCASECMP
#define HAVE_STRCOLL
#define HAVE_STRERROR
#define HAVE_STRFTIME
#define HAVE_STRPBRK
#define HAVE_STRTOL
#define HAVE_QSORT
#define HAVE_MBLEN
#define HAVE_TOWLOWER
#define HAVE_TOWUPPER
#define HAVE_ISWUPPER

/* No gettext on a microcontroller. */
/* #undef HAVE_LIBINTL_H */
/* #undef HAVE_GETTEXT */
/* #undef HAVE_BIND_TEXTDOMAIN_CODESET */
/* #undef HAVE_NL_LANGINFO_CODESET */
/* #undef HAVE_LANGINFO_H */

/*
 * No iconv. +multi_byte is mandatory in modern Vim (feature.h makes it
 * unconditional), so mbyte.c compiles regardless -- we simply run as UTF-8 and
 * never convert. 'encoding' is forced to utf-8 by the port's defaults.
 */
/* #undef HAVE_ICONV */
/* #undef HAVE_ICONV_H */
/* #undef DYNAMIC_ICONV */

/* -------------------------------------------------------------------- time -- */

#define HAVE_GETTIMEOFDAY
#define HAVE_LOCALTIME_R
#define HAVE_NANOSLEEP
#define HAVE_USLEEP
#define HAVE_TZSET
#define HAVE_CLOCK_GETTIME
/* #undef HAVE_STRPTIME */
/* #undef HAVE_TIMER_CREATE */
/* #undef HAVE_UTIME */
/* #undef HAVE_UTIME_H */

/* -------------------------------------------------------------------- misc -- */

#define HAVE_SELECT
#define SELECT_TYPE_ARG234 (fd_set *)
#define SYS_SELECT_WITH_SYS_TIME
#define HAVE_SETENV
#define HAVE_PUTENV
#define HAVE_UNSETENV
#define HAVE_ISINF
#define HAVE_ISNAN
#define HAVE_FD_CLOEXEC

/* poll() exists via IDF but select() is the better-supported VFS path. */
/* #undef HAVE_POLL */
/* #undef HAVE_POLL_H */
/* #undef HAVE_SYS_POLL_H */

/* No dynamic loading, no /proc, no resource limits, no sysinfo. */
/* #undef HAVE_DLFCN_H */
/* #undef HAVE_DLOPEN */
/* #undef HAVE_DLSYM */
/* #undef PROC_EXE_LINK */
/* #undef HAVE_GETRLIMIT */
/* #undef HAVE_SYS_RESOURCE_H */
/* #undef HAVE_SYSINFO */
/* #undef HAVE_SYSINFO_MEM_UNIT */
/* #undef HAVE_SYSINFO_UPTIME */
/* #undef HAVE_SYS_SYSINFO_H */
/* #undef HAVE_SYS_STATFS_H */
/* #undef HAVE_SYS_UTSNAME_H */
/* #undef HAVE_SYSCONF */
/* #undef ENABLE_CSCOPE */
/* #undef HAVE_SELINUX */
/* #undef USEMAN_S */
/* #undef HAVE_DATE_TIME */

/* ----------------------------------------------------------------- headers -- */

#define HAVE_ERRNO_H
#define HAVE_FCNTL_H
#define HAVE_INTTYPES_H
#define HAVE_LOCALE_H
#define HAVE_MATH_H
#define HAVE_SETJMP_H
#define HAVE_STDINT_H
#define HAVE_STDLIB_H
#define HAVE_STRINGS_H
#define HAVE_STRING_H
#define HAVE_SYS_IOCTL_H        /* IDF newlib overlay */
#define HAVE_SYS_PARAM_H
#define HAVE_SYS_SELECT_H
#define HAVE_SYS_TIME_H
#define HAVE_SYS_TYPES_H
#define HAVE_SYS_WAIT_H
#define HAVE_UNISTD_H
#define HAVE_WCHAR_H
#define HAVE_WCTYPE_H
/* #undef HAVE_LIBGEN_H */

#endif /* ESP_VIM_CONFIG_H */
