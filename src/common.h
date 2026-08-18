#ifndef METTLE_COMMON_H
#define METTLE_COMMON_H

#include <stddef.h>
#include <stdarg.h>

/* Some Windows C compilers omit the POSIX strcasecmp declaration. */
#if defined(_WIN32) && !defined(__MINGW32__)
#include <string.h>
#ifndef strcasecmp
#define strcasecmp _stricmp
#endif
#ifndef strncasecmp
#define strncasecmp _strnicmp
#endif
#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef ptrdiff_t ssize_t;
#endif
#endif

/* Thread-local storage qualifier. The backend keeps its mutable per-compile
 * diagnostic state (the --explain report/remarks and the --annotate-asm capture)
 * thread-local so two frontends can drive libmtlc concurrently on separate
 * threads without clobbering each other -- i.e. the backend has no shared
 * mutable global state. MettleCompilerContext uses the owned TLS layer too.
 * The freestanding host supplies __emutls_get_address on Windows and native
 * thread pointer setup on Linux, so this does not pull in a thread library. */
#if defined(_MSC_VER)
#define MTLC_THREAD_LOCAL __declspec(thread)
#elif defined(_WIN32) && defined(__GNUC__) && !defined(__clang__) && \
    __GNUC__ >= 16
/* MSYS2's gcc 16 emits native PE TLS (_tls_index + a loader TLS directory)
 * with no emutls fallback left. The freestanding host supplies only
 * __emutls_get_address, and the internal linker builds no TLS directory, so
 * native TLS cannot link self-contained. Degrade to plain statics: one
 * compile per process on this toolchain. */
#define MTLC_THREAD_LOCAL
#else
#define MTLC_THREAD_LOCAL __thread
#endif

#define METTLE_FNV1A_OFFSET_BASIS ((size_t)1469598103934665603ULL)
#define METTLE_FNV1A_PRIME        ((size_t)1099511628211ULL)

char *mettle_strdup(const char *text);
size_t mettle_fnv1a_hash(const char *str);
void mettle_set_error(char **dest, const char *fmt, ...);
void mettle_free_string(char *str);
void mettle_free_string_array(char **values, size_t count);

#endif
