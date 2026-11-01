#if defined(__APPLE__)
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#endif
#else
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#endif

#include "hosted_posix.h"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <crt_externs.h>
#include <mach-o/dyld.h>
#include <sys/syslimits.h>
#else
extern char **environ;
#endif

#define MT_HOSTED_DEFAULT_PATH "/usr/local/bin:/usr/bin:/bin"
#define MT_HOSTED_THREAD_STACK_BYTES (1024u * 1024u)
#define MT_HOSTED_THREAD_STACK_FLOOR 65536u
#define MT_HOSTED_WAIT_BUCKET_BITS 6u
#define MT_HOSTED_WAIT_BUCKETS (1u << MT_HOSTED_WAIT_BUCKET_BITS)

typedef struct {
  pthread_mutex_t lock;
  pthread_cond_t wake;
} MtHostedWaitBucket;

typedef struct {
  volatile int running;
  int detached;
  unsigned id;
  pthread_t handle;
  unsigned (*start)(void *);
  void *argument;
} MtHostedThread;

typedef struct {
  volatile int state;
} MtHostedMutex;

static const char *mt_hosted_program;
static pthread_t mt_hosted_main_thread;
static int mt_hosted_main_thread_known;
static volatile unsigned mt_hosted_thread_counter;
static __thread void *mt_hosted_stack_top;
static __thread unsigned mt_hosted_thread_number;
static char mt_hosted_signal_stack[64 * 1024];
static int mt_hosted_signal_stack_installed;
static MtHostedWaitBucket mt_hosted_wait_buckets[MT_HOSTED_WAIT_BUCKETS];
static pthread_once_t mt_hosted_wait_once = PTHREAD_ONCE_INIT;

void mt_hosted_startup(int argc, char **argv) {
  mt_hosted_program = argc > 0 && argv ? argv[0] : NULL;
  mt_hosted_stack_top = (void *)argv;
  mt_hosted_main_thread = pthread_self();
  mt_hosted_main_thread_known = 1;
}

int *mt_hosted_errno_location(void) { return &errno; }

char **mt_hosted_environ(void) {
#if defined(__APPLE__)
  return *_NSGetEnviron();
#else
  return environ;
#endif
}

void *mt_hosted_thread_stack_high(void) { return mt_hosted_stack_top; }

typedef char *(*MtHostedGetcwd)(char *, size_t);

char *mt_hosted_getcwd(char *buffer, unsigned long long size) {
  static MtHostedGetcwd resolved;
  MtHostedGetcwd function = __atomic_load_n(&resolved, __ATOMIC_ACQUIRE);
  if (!function) {
    function = (MtHostedGetcwd)dlsym(RTLD_NEXT, "getcwd");
    if (!function) {
      function = getcwd;
    }
    __atomic_store_n(&resolved, function, __ATOMIC_RELEASE);
  }
  return function(buffer, (size_t)size);
}

int mt_hosted_open(const char *path, int mt_flags, int mode) {
  int flags = O_RDONLY;
  if ((mt_flags & MT_HOSTED_O_ACCMODE) == MT_HOSTED_O_WRONLY) {
    flags = O_WRONLY;
  } else if ((mt_flags & MT_HOSTED_O_ACCMODE) == MT_HOSTED_O_RDWR) {
    flags = O_RDWR;
  }
  if (mt_flags & MT_HOSTED_O_CREAT) {
    flags |= O_CREAT;
  }
  if (mt_flags & MT_HOSTED_O_TRUNC) {
    flags |= O_TRUNC;
  }
  if (mt_flags & MT_HOSTED_O_APPEND) {
    flags |= O_APPEND;
  }
  if (mt_flags & MT_HOSTED_O_DIRECTORY) {
    flags |= O_DIRECTORY;
  }
  return open(path, flags, (mode_t)mode);
}

int mt_hosted_is_directory(const char *path) {
  int descriptor = open(path, O_RDONLY | O_DIRECTORY);
  if (descriptor < 0) {
    return 0;
  }
  (void)close(descriptor);
  return 1;
}

void *mt_hosted_map(unsigned long long bytes) {
  void *mapping = mmap(NULL, (size_t)bytes, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANON, -1, 0);
  return mapping == MAP_FAILED ? NULL : mapping;
}

int mt_hosted_unmap(void *address, unsigned long long bytes) {
  return munmap(address, (size_t)bytes);
}

static long long mt_hosted_copy_path(char *buffer, unsigned long long size,
                                     const char *text) {
  size_t length = strlen(text);
  if (length > size) {
    length = (size_t)size;
  }
  memcpy(buffer, text, length);
  return (long long)length;
}

static long long mt_hosted_search_program(char *buffer,
                                          unsigned long long size,
                                          const char *program) {
  const char *path = getenv("PATH");
  size_t program_length = strlen(program);
  if (!path) {
    path = MT_HOSTED_DEFAULT_PATH;
  }
  while (*path) {
    const char *end = path;
    size_t directory_length;
    char candidate[PATH_MAX];
    while (*end && *end != ':') {
      end++;
    }
    directory_length = (size_t)(end - path);
    if (directory_length + program_length + 2 < sizeof(candidate)) {
      memcpy(candidate, path, directory_length);
      candidate[directory_length] = '/';
      memcpy(candidate + directory_length + 1, program, program_length + 1);
      if (access(candidate, X_OK) == 0) {
        return mt_hosted_copy_path(buffer, size, candidate);
      }
    }
    path = *end ? end + 1 : end;
  }
  errno = ENOENT;
  return -1;
}

static long long mt_hosted_program_path(char *buffer,
                                        unsigned long long size) {
  char joined[PATH_MAX];
  size_t used;
  size_t program_length;
  if (!mt_hosted_program || !mt_hosted_program[0]) {
    errno = ENOENT;
    return -1;
  }
  if (!strchr(mt_hosted_program, '/')) {
    return mt_hosted_search_program(buffer, size, mt_hosted_program);
  }
  if (mt_hosted_program[0] == '/') {
    return mt_hosted_copy_path(buffer, size, mt_hosted_program);
  }
  if (!mt_hosted_getcwd(joined, sizeof(joined))) {
    return -1;
  }
  used = strlen(joined);
  program_length = strlen(mt_hosted_program);
  if (used + program_length + 2 > sizeof(joined)) {
    errno = ENAMETOOLONG;
    return -1;
  }
  if (used == 0 || joined[used - 1] != '/') {
    joined[used++] = '/';
  }
  memcpy(joined + used, mt_hosted_program, program_length + 1);
  return mt_hosted_copy_path(buffer, size, joined);
}

long long mt_hosted_executable_path(char *buffer, unsigned long long size) {
  if (!buffer || size == 0) {
    errno = EINVAL;
    return -1;
  }
#if defined(__APPLE__)
  {
    char path[PATH_MAX];
    uint32_t capacity = (uint32_t)sizeof(path);
    if (_NSGetExecutablePath(path, &capacity) == 0) {
      return mt_hosted_copy_path(buffer, size, path);
    }
  }
#else
  {
    ssize_t length = readlink("/proc/self/exe", buffer, (size_t)size);
    if (length >= 0) {
      return (long long)length;
    }
  }
#endif
  return mt_hosted_program_path(buffer, size);
}

int mt_hosted_monotonic(long long *timespec_value) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
    return -1;
  }
  timespec_value[0] = (long long)now.tv_sec;
  timespec_value[1] = (long long)now.tv_nsec;
  return 0;
}

static long long mt_hosted_now_ms(void) {
  long long now[2] = {0, 0};
  (void)mt_hosted_monotonic(now);
  return now[0] * 1000 + now[1] / 1000000;
}

void *mt_hosted_dir_open(const char *path) { return opendir(path); }

const char *mt_hosted_dir_next(void *directory, int *entry_type) {
  struct dirent *entry = readdir((DIR *)directory);
  if (!entry) {
    return NULL;
  }
#if defined(DT_UNKNOWN)
  *entry_type = (int)entry->d_type;
#else
  *entry_type = 0;
#endif
  return entry->d_name;
}

void mt_hosted_dir_close(void *directory) { (void)closedir((DIR *)directory); }

int mt_hosted_install_signal_handler(int signal_number,
                                     void (*handler)(int, void *, void *)) {
  struct sigaction action;
  if (!mt_hosted_signal_stack_installed) {
    stack_t stack;
    memset(&stack, 0, sizeof(stack));
    stack.ss_sp = mt_hosted_signal_stack;
    stack.ss_size = sizeof(mt_hosted_signal_stack);
    stack.ss_flags = 0;
    if (sigaltstack(&stack, NULL) == 0) {
      mt_hosted_signal_stack_installed = 1;
    }
  }
  memset(&action, 0, sizeof(action));
  action.sa_sigaction = (void (*)(int, siginfo_t *, void *))handler;
  action.sa_flags = SA_SIGINFO;
  if (mt_hosted_signal_stack_installed) {
    action.sa_flags |= SA_ONSTACK;
  }
  (void)sigemptyset(&action.sa_mask);
  return sigaction(signal_number, &action, NULL);
}

int mt_hosted_address_is_readable(const void *address,
                                  unsigned long long length) {
  int descriptors[2] = {-1, -1};
  char sink[64];
  ssize_t written;
  if (!address || length == 0) {
    return 0;
  }
  if (pipe(descriptors) != 0) {
    return 0;
  }
  written = write(descriptors[1], address, (size_t)length);
  if (written > 0) {
    ssize_t remaining = written;
    while (remaining > 0) {
      size_t amount = remaining < (ssize_t)sizeof(sink) ? (size_t)remaining
                                                        : sizeof(sink);
      ssize_t got = read(descriptors[0], sink, amount);
      if (got <= 0) {
        break;
      }
      remaining -= got;
    }
  }
  (void)close(descriptors[0]);
  (void)close(descriptors[1]);
  return written == (ssize_t)length;
}

static char *const *mt_hosted_spawn_environment(char *const *environment) {
  return environment ? environment : mt_hosted_environ();
}

int mt_hosted_wait_process(long long pid, int *status) {
  int raw = 0;
  for (;;) {
    pid_t result = waitpid((pid_t)pid, &raw, 0);
    if (result >= 0) {
      if (status) {
        *status = raw;
      }
      return 0;
    }
    if (errno != EINTR) {
      return -1;
    }
  }
}

int mt_hosted_run_process(const char *program, const char *const *arguments,
                          char *const *environment, const char *search_path) {
  char *const *argv = (char *const *)arguments;
  char *const *envp = mt_hosted_spawn_environment(environment);
  pid_t pid = -1;
  int spawned = ENOENT;
  int status = 0;
  if (!program || !arguments) {
    return 127;
  }
  if (strchr(program, '/')) {
    spawned = posix_spawn(&pid, program, NULL, NULL, argv, envp);
  } else {
    const char *path = search_path ? search_path : MT_HOSTED_DEFAULT_PATH;
    size_t program_length = strlen(program);
    while (*path && spawned != 0) {
      const char *end = path;
      size_t directory_length;
      char candidate[1024];
      while (*end && *end != ':') {
        end++;
      }
      directory_length = (size_t)(end - path);
      if (directory_length + program_length + 2 < sizeof(candidate)) {
        memcpy(candidate, path, directory_length);
        candidate[directory_length] = '/';
        memcpy(candidate + directory_length + 1, program, program_length + 1);
        spawned = posix_spawn(&pid, candidate, NULL, NULL, argv, envp);
      }
      path = *end ? end + 1 : end;
    }
  }
  if (spawned != 0) {
    return 127;
  }
  if (mt_hosted_wait_process((long long)pid, &status) != 0) {
    return 127;
  }
  if ((status & 0x7f) != 0) {
    return 128 + (status & 0x7f);
  }
  return (status >> 8) & 0xff;
}

long long mt_hosted_spawn_process(const char *program,
                                  const char *const *arguments) {
  pid_t pid = -1;
  if (!program || !arguments ||
      posix_spawn(&pid, program, NULL, NULL, (char *const *)arguments,
                  mt_hosted_environ()) != 0) {
    return -1;
  }
  return (long long)pid;
}

int mt_hosted_poll_process(long long pid, int *exit_code) {
  int raw = 0;
  pid_t result = waitpid((pid_t)pid, &raw, WNOHANG);
  if (result == 0) {
    return 0;
  }
  if (result < 0) {
    return -1;
  }
  if (exit_code) {
    *exit_code = (raw & 0x7f) != 0 ? 128 + (raw & 0x7f) : (raw >> 8) & 0xff;
  }
  return 1;
}

void mt_hosted_sleep_ms(unsigned milliseconds) {
  struct timespec request;
  request.tv_sec = (time_t)(milliseconds / 1000u);
  request.tv_nsec = (long)(milliseconds % 1000u) * 1000000L;
  (void)nanosleep(&request, NULL);
}

int mt_hosted_file_mtime(const char *path, long long *seconds,
                         long long *nanoseconds) {
  struct stat info;
  if (!path || stat(path, &info) != 0) {
    return 0;
  }
  *seconds = (long long)info.st_mtime;
#if defined(__APPLE__)
  *nanoseconds = (long long)info.st_mtimespec.tv_nsec;
#else
  *nanoseconds = (long long)info.st_mtim.tv_nsec;
#endif
  return 1;
}

long long mt_hosted_spawn_shell_reader(const char *command,
                                       char *const *environment,
                                       int *read_descriptor) {
  int descriptors[2] = {-1, -1};
  posix_spawn_file_actions_t actions;
  char *arguments[4];
  pid_t pid = -1;
  int result;
  if (!command || !read_descriptor) {
    errno = EINVAL;
    return -1;
  }
  if (pipe(descriptors) != 0) {
    return -1;
  }
  (void)fcntl(descriptors[0], F_SETFD, FD_CLOEXEC);
  if (descriptors[1] != 1) {
    (void)fcntl(descriptors[1], F_SETFD, FD_CLOEXEC);
  }
  if (posix_spawn_file_actions_init(&actions) != 0) {
    (void)close(descriptors[0]);
    (void)close(descriptors[1]);
    return -1;
  }
  if (descriptors[1] != 1) {
    (void)posix_spawn_file_actions_adddup2(&actions, descriptors[1], 1);
  }
  arguments[0] = (char *)"sh";
  arguments[1] = (char *)"-c";
  arguments[2] = (char *)command;
  arguments[3] = NULL;
  result = posix_spawn(&pid, "/bin/sh", &actions, NULL, arguments,
                       mt_hosted_spawn_environment(environment));
  (void)posix_spawn_file_actions_destroy(&actions);
  (void)close(descriptors[1]);
  if (result != 0) {
    (void)close(descriptors[0]);
    errno = result;
    return -1;
  }
  *read_descriptor = descriptors[0];
  return (long long)pid;
}

void mt_hosted_yield(void) { (void)sched_yield(); }

unsigned mt_hosted_hardware_threads(void) {
  long count = sysconf(_SC_NPROCESSORS_ONLN);
  return count > 0 ? (unsigned)count : 1u;
}

static void mt_hosted_set_stack_size(pthread_attr_t *attributes,
                                     unsigned long long size) {
  long page = sysconf(_SC_PAGESIZE);
  unsigned long long granule = page > 0 ? (unsigned long long)page : 4096u;
  if (size < MT_HOSTED_THREAD_STACK_FLOOR) {
    size = MT_HOSTED_THREAD_STACK_FLOOR;
  }
  size = (size + granule - 1u) / granule * granule;
  (void)pthread_attr_setstacksize(attributes, (size_t)size);
}

int mt_hosted_spawn_detached(void *(*start)(void *), void *argument,
                             unsigned long long stack_size) {
  pthread_attr_t attributes;
  pthread_t thread;
  int result;
  if (pthread_attr_init(&attributes) != 0) {
    return 0;
  }
  mt_hosted_set_stack_size(&attributes, stack_size ? stack_size
                                                   : MT_HOSTED_THREAD_STACK_BYTES);
  (void)pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
  result = pthread_create(&thread, &attributes, start, argument);
  (void)pthread_attr_destroy(&attributes);
  return result == 0;
}

static void mt_hosted_wait_init(void) {
  unsigned i;
  for (i = 0; i < MT_HOSTED_WAIT_BUCKETS; i++) {
    (void)pthread_mutex_init(&mt_hosted_wait_buckets[i].lock, NULL);
    (void)pthread_cond_init(&mt_hosted_wait_buckets[i].wake, NULL);
  }
}

static MtHostedWaitBucket *mt_hosted_wait_bucket(volatile int *address) {
  unsigned long long key = (unsigned long long)(uintptr_t)address;
  (void)pthread_once(&mt_hosted_wait_once, mt_hosted_wait_init);
  key = (key >> 2) * 0x9E3779B97F4A7C15ull;
  return &mt_hosted_wait_buckets[key >> (64u - MT_HOSTED_WAIT_BUCKET_BITS)];
}

static void mt_hosted_deadline(struct timespec *deadline,
                               unsigned milliseconds) {
  (void)clock_gettime(CLOCK_REALTIME, deadline);
  deadline->tv_sec += (time_t)(milliseconds / 1000u);
  deadline->tv_nsec += (long)(milliseconds % 1000u) * 1000000L;
  if (deadline->tv_nsec >= 1000000000L) {
    deadline->tv_sec += 1;
    deadline->tv_nsec -= 1000000000L;
  }
}

int mt_hosted_wait_on(volatile int *address, int expected,
                      unsigned milliseconds) {
  MtHostedWaitBucket *bucket = mt_hosted_wait_bucket(address);
  struct timespec deadline;
  int timed_out = 0;
  if (milliseconds != MT_HOSTED_INFINITE) {
    mt_hosted_deadline(&deadline, milliseconds);
  }
  (void)pthread_mutex_lock(&bucket->lock);
  if (__atomic_load_n(address, __ATOMIC_SEQ_CST) == expected) {
    if (milliseconds == MT_HOSTED_INFINITE) {
      (void)pthread_cond_wait(&bucket->wake, &bucket->lock);
    } else if (pthread_cond_timedwait(&bucket->wake, &bucket->lock,
                                      &deadline) == ETIMEDOUT) {
      timed_out = 1;
    }
  }
  (void)pthread_mutex_unlock(&bucket->lock);
  return timed_out;
}

void mt_hosted_wake(volatile int *address) {
  MtHostedWaitBucket *bucket = mt_hosted_wait_bucket(address);
  (void)pthread_mutex_lock(&bucket->lock);
  (void)pthread_cond_broadcast(&bucket->wake);
  (void)pthread_mutex_unlock(&bucket->lock);
}

static unsigned mt_hosted_next_thread_id(void) {
  return (unsigned)getpid() +
         __atomic_add_fetch(&mt_hosted_thread_counter, 1u, __ATOMIC_SEQ_CST);
}

unsigned mt_hosted_thread_current_id(void) {
  if (!mt_hosted_thread_number) {
    if (mt_hosted_main_thread_known &&
        pthread_equal(pthread_self(), mt_hosted_main_thread)) {
      mt_hosted_thread_number = (unsigned)getpid();
    } else {
      mt_hosted_thread_number = mt_hosted_next_thread_id();
    }
  }
  return mt_hosted_thread_number;
}

static void *mt_hosted_thread_entry(void *raw) {
  MtHostedThread *thread = (MtHostedThread *)raw;
  mt_hosted_stack_top = __builtin_frame_address(0);
  mt_hosted_thread_number = thread->id;
  (void)thread->start(thread->argument);
  __atomic_store_n(&thread->running, 0, __ATOMIC_RELEASE);
  mt_hosted_wake(&thread->running);
  return NULL;
}

long long mt_hosted_thread_create(unsigned long long stack_size,
                                  unsigned (*start)(void *), void *argument,
                                  unsigned *thread_id) {
  MtHostedThread *thread;
  pthread_attr_t attributes;
  unsigned long long size =
      stack_size ? stack_size : (unsigned long long)MT_HOSTED_THREAD_STACK_BYTES;
  int created;
  if (!start) {
    return 0;
  }
  if (size < MT_HOSTED_THREAD_STACK_FLOOR) {
    size = MT_HOSTED_THREAD_STACK_FLOOR;
  }
  thread = (MtHostedThread *)calloc(1, sizeof(MtHostedThread));
  if (!thread) {
    return 0;
  }
  thread->running = 1;
  thread->id = mt_hosted_next_thread_id();
  thread->start = start;
  thread->argument = argument;
  if (pthread_attr_init(&attributes) != 0) {
    free(thread);
    return 0;
  }
  mt_hosted_set_stack_size(&attributes, size);
  created = pthread_create(&thread->handle, &attributes,
                           mt_hosted_thread_entry, thread);
  (void)pthread_attr_destroy(&attributes);
  if (created != 0) {
    free(thread);
    return 0;
  }
  if (thread_id) {
    *thread_id = thread->id;
  }
  return (long long)(uintptr_t)thread;
}

unsigned mt_hosted_thread_wait(long long handle, unsigned milliseconds) {
  MtHostedThread *thread = (MtHostedThread *)(uintptr_t)handle;
  long long deadline = 0;
  if (!thread) {
    return MT_HOSTED_WAIT_FAILED;
  }
  if (milliseconds != MT_HOSTED_INFINITE) {
    deadline = mt_hosted_now_ms() + (long long)milliseconds;
  }
  for (;;) {
    unsigned remaining = MT_HOSTED_INFINITE;
    if (__atomic_load_n(&thread->running, __ATOMIC_ACQUIRE) == 0) {
      return MT_HOSTED_WAIT_OBJECT_0;
    }
    if (milliseconds != MT_HOSTED_INFINITE) {
      long long now = mt_hosted_now_ms();
      if (now >= deadline) {
        return MT_HOSTED_WAIT_TIMEOUT;
      }
      remaining = (unsigned)(deadline - now);
    }
    (void)mt_hosted_wait_on(&thread->running, 1, remaining);
  }
}

int mt_hosted_thread_close(long long handle) {
  MtHostedThread *thread = (MtHostedThread *)(uintptr_t)handle;
  if (!thread) {
    return 0;
  }
  if (__atomic_load_n(&thread->running, __ATOMIC_ACQUIRE) != 0) {
    if (!thread->detached) {
      thread->detached = 1;
      (void)pthread_detach(thread->handle);
    }
    return 1;
  }
  if (!thread->detached) {
    (void)pthread_join(thread->handle, NULL);
  }
  free(thread);
  return 1;
}

void mt_hosted_thread_sleep_ms(unsigned milliseconds) {
  struct timespec request;
  request.tv_sec = (time_t)(milliseconds / 1000u);
  request.tv_nsec = (long)(milliseconds % 1000u) * 1000000L;
  (void)nanosleep(&request, NULL);
}

long long mt_hosted_mutex_create(int initial_owner) {
  MtHostedMutex *mutex = (MtHostedMutex *)calloc(1, sizeof(MtHostedMutex));
  if (mutex && initial_owner) {
    mutex->state = 1;
  }
  return (long long)(uintptr_t)mutex;
}

unsigned mt_hosted_mutex_wait(long long handle, unsigned milliseconds) {
  MtHostedMutex *mutex = (MtHostedMutex *)(uintptr_t)handle;
  long long deadline = 0;
  if (!mutex) {
    return MT_HOSTED_WAIT_FAILED;
  }
  if (milliseconds != MT_HOSTED_INFINITE) {
    deadline = mt_hosted_now_ms() + (long long)milliseconds;
  }
  for (;;) {
    int expected = 0;
    unsigned remaining = MT_HOSTED_INFINITE;
    if (__atomic_compare_exchange_n(&mutex->state, &expected, 1, 0,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
      return MT_HOSTED_WAIT_OBJECT_0;
    }
    if (milliseconds != MT_HOSTED_INFINITE) {
      long long now = mt_hosted_now_ms();
      if (now >= deadline) {
        return MT_HOSTED_WAIT_TIMEOUT;
      }
      remaining = (unsigned)(deadline - now);
    }
    (void)mt_hosted_wait_on(&mutex->state, 1, remaining);
  }
}

int mt_hosted_mutex_release(long long handle) {
  MtHostedMutex *mutex = (MtHostedMutex *)(uintptr_t)handle;
  if (!mutex) {
    return 0;
  }
  __atomic_store_n(&mutex->state, 0, __ATOMIC_RELEASE);
  mt_hosted_wake(&mutex->state);
  return 1;
}

int mt_hosted_mutex_close(long long handle) {
  MtHostedMutex *mutex = (MtHostedMutex *)(uintptr_t)handle;
  if (!mutex || __atomic_load_n(&mutex->state, __ATOMIC_ACQUIRE) != 0) {
    return 0;
  }
  free(mutex);
  return 1;
}
