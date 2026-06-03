/*
 * posix_helpers.c – POSIX-specific runtime helpers (Linux/macOS only).
 *
 * Tiny C shims for things Mettle can't express directly:
 *   posix_get_errno()  – read errno (a thread-local macro on POSIX)
 *   posix_yield()      – sched_yield() for spin-lock contention paths
 *
 * Atomic CAS / exchange / inc / dec are provided as cross-platform symbols by
 * src/runtime/atomics.c (`mettle_atomic_*`); std/thread (Linux variant) uses
 * those directly, so no atomic helpers live here.
 *
 * Auto-linked into Linux ELF executables when the program references either of
 * these symbols (see object_needs_posix_helpers in src/main.c), mirroring how
 * Windows auto-links Win32 runtime DLLs without the user passing link flags.
 */

#if !defined(_WIN32) && !defined(_WIN64)

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

int posix_get_errno(void) {
    return errno;
}

void posix_yield(void) {
    sched_yield();
}

/* ─── Win32-style thread API emulated over pthreads ─────────────────────────
 * std/thread (Linux variant) wraps these to expose CreateThread/CloseHandle/
 * WaitForSingleObject/Sleep/GetCurrentThreadId/CreateMutexA/ReleaseMutex with
 * the same signatures and contract as the Windows module, so a single
 * `import "std/thread"` works identically on both OSes. */

/* The Mettle-side start routine has the Win32 signature `fn(void*) -> uint32`
 * but pthreads expects `void* (*)(void*)`. A small C trampoline adapts. We
 * stuff the user function pointer + arg into a heap cell so the trampoline
 * can recover both inside the worker thread. */
typedef uint32_t (*mettle_thread_proc)(void *);
typedef struct {
    mettle_thread_proc fn;
    void *arg;
} mettle_thread_ctx;

static void *mettle_pthread_trampoline(void *raw) {
    mettle_thread_ctx ctx = *(mettle_thread_ctx *)raw;
    free(raw);
    uint32_t result = ctx.fn(ctx.arg);
    return (void *)(uintptr_t)result;
}

/* CreateThread(attr, stackSize, startRoutine, arg, flags, &threadId)
 *   Returns the thread "handle" (a malloc'd pthread_t pointer cast to int64),
 *   or 0 on failure (matching Win32 INVALID_HANDLE behaviour for std/thread).
 *   `attr`, `stackSize`, and `flags` are accepted for ABI parity and ignored.
 *   `threadIdOut`, if non-NULL, receives a 32-bit thread id (low 32 bits of
 *   the pthread_t value, sufficient for logging). */
int64_t mettle_thread_create(void *attr, uint64_t stack_size,
                             mettle_thread_proc fn, void *arg,
                             uint32_t flags, uint32_t *thread_id_out) {
    (void)attr;
    (void)stack_size;
    (void)flags;
    pthread_t *handle = (pthread_t *)malloc(sizeof(pthread_t));
    if (!handle) {
        return 0;
    }
    mettle_thread_ctx *ctx = (mettle_thread_ctx *)malloc(sizeof(*ctx));
    if (!ctx) {
        free(handle);
        return 0;
    }
    ctx->fn = fn;
    ctx->arg = arg;
    if (pthread_create(handle, NULL, mettle_pthread_trampoline, ctx) != 0) {
        free(ctx);
        free(handle);
        return 0;
    }
    if (thread_id_out) {
        *thread_id_out = (uint32_t)(uintptr_t)*handle;
    }
    return (int64_t)(uintptr_t)handle;
}

/* CloseHandle(handle) — Win32 releases the kernel handle. On POSIX we detach
 * the thread (if still joinable) and free the heap cell. Returns 1 on
 * success, 0 on failure, matching the Win32 BOOL contract. */
int32_t mettle_thread_close(int64_t handle) {
    if (handle == 0) {
        return 0;
    }
    pthread_t *p = (pthread_t *)(uintptr_t)handle;
    pthread_detach(*p);  /* harmless if already joined */
    free(p);
    return 1;
}

/* WaitForSingleObject — pthreads timed-join. Returns:
 *   WAIT_OBJECT_0 (0)       on successful join,
 *   WAIT_TIMEOUT  (258)     if the timeout expired,
 *   WAIT_FAILED   (0xFFFFFFFF) on error.
 * timeout_ms == 0xFFFFFFFF (INFINITE) uses pthread_join (block forever). */
#define MT_WAIT_OBJECT_0 0u
#define MT_WAIT_TIMEOUT  258u
#define MT_WAIT_FAILED   0xFFFFFFFFu

uint32_t mettle_thread_wait(int64_t handle, uint32_t timeout_ms) {
    if (handle == 0) {
        return MT_WAIT_FAILED;
    }
    pthread_t *p = (pthread_t *)(uintptr_t)handle;
    if (timeout_ms == 0xFFFFFFFFu) {
        return pthread_join(*p, NULL) == 0 ? MT_WAIT_OBJECT_0 : MT_WAIT_FAILED;
    }
    /* pthread_timedjoin_np is a glibc extension; absolute timespec. */
    struct timespec abs_ts;
    if (clock_gettime(CLOCK_REALTIME, &abs_ts) != 0) {
        return MT_WAIT_FAILED;
    }
    abs_ts.tv_sec  += timeout_ms / 1000u;
    abs_ts.tv_nsec += (long)(timeout_ms % 1000u) * 1000000L;
    if (abs_ts.tv_nsec >= 1000000000L) {
        abs_ts.tv_sec += 1;
        abs_ts.tv_nsec -= 1000000000L;
    }
    int rc = pthread_timedjoin_np(*p, NULL, &abs_ts);
    if (rc == 0) return MT_WAIT_OBJECT_0;
    if (rc == ETIMEDOUT) return MT_WAIT_TIMEOUT;
    return MT_WAIT_FAILED;
}

/* Sleep(ms) — Win32 millisecond sleep. usleep takes microseconds. */
void mettle_thread_sleep_ms(uint32_t milliseconds) {
    if (milliseconds == 0) {
        sched_yield();
        return;
    }
    usleep((useconds_t)milliseconds * 1000u);
}

/* GetCurrentThreadId — Win32 returns a DWORD. We return the low 32 bits of
 * pthread_self(), sufficient for logging/identification. */
uint32_t mettle_thread_current_id(void) {
    return (uint32_t)(uintptr_t)pthread_self();
}

/* CreateMutexA(attr, initialOwner, name)
 *   Allocates a pthread_mutex_t on the heap, initialises it, and returns the
 *   pointer cast to int64. If `initial_owner` is non-zero, the mutex is
 *   locked once before returning (matching Win32 semantics). Returns 0 on
 *   allocation failure. */
int64_t mettle_mutex_create(void *attr, int32_t initial_owner,
                            const char *name) {
    (void)attr;
    (void)name;
    pthread_mutex_t *m = (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t));
    if (!m) {
        return 0;
    }
    if (pthread_mutex_init(m, NULL) != 0) {
        free(m);
        return 0;
    }
    if (initial_owner) {
        pthread_mutex_lock(m);
    }
    return (int64_t)(uintptr_t)m;
}

/* ReleaseMutex — Win32 unlock. Returns 1 on success, 0 on failure. */
int32_t mettle_mutex_release(int64_t handle) {
    if (handle == 0) {
        return 0;
    }
    pthread_mutex_t *m = (pthread_mutex_t *)(uintptr_t)handle;
    return pthread_mutex_unlock(m) == 0 ? 1 : 0;
}

/* Used by std/thread Linux variant's mutex_lock(handle, timeout_ms). Returns
 * the same Win32 WAIT_* code as mettle_thread_wait. */
uint32_t mettle_mutex_wait(int64_t handle, uint32_t timeout_ms) {
    if (handle == 0) {
        return MT_WAIT_FAILED;
    }
    pthread_mutex_t *m = (pthread_mutex_t *)(uintptr_t)handle;
    if (timeout_ms == 0xFFFFFFFFu) {
        return pthread_mutex_lock(m) == 0 ? MT_WAIT_OBJECT_0 : MT_WAIT_FAILED;
    }
    struct timespec abs_ts;
    if (clock_gettime(CLOCK_REALTIME, &abs_ts) != 0) {
        return MT_WAIT_FAILED;
    }
    abs_ts.tv_sec  += timeout_ms / 1000u;
    abs_ts.tv_nsec += (long)(timeout_ms % 1000u) * 1000000L;
    if (abs_ts.tv_nsec >= 1000000000L) {
        abs_ts.tv_sec += 1;
        abs_ts.tv_nsec -= 1000000000L;
    }
    int rc = pthread_mutex_timedlock(m, &abs_ts);
    if (rc == 0) return MT_WAIT_OBJECT_0;
    if (rc == ETIMEDOUT) return MT_WAIT_TIMEOUT;
    return MT_WAIT_FAILED;
}

/* CloseHandle for a mutex: destroy + free. Returns 1 on success. */
int32_t mettle_mutex_close(int64_t handle) {
    if (handle == 0) {
        return 0;
    }
    pthread_mutex_t *m = (pthread_mutex_t *)(uintptr_t)handle;
    pthread_mutex_destroy(m);
    free(m);
    return 1;
}

#endif /* !_WIN32 */
