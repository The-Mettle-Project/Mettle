#if defined(__APPLE__)
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#endif
#else
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif
#endif

#include "hosted_posix.h"
#include "owned.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define MTLC_HOST_STACK_BYTES ((rlim_t)64 * 1024 * 1024)

FILE *mtlc_host_stdin;
FILE *mtlc_host_stdout;
FILE *mtlc_host_stderr;
long long (*mtlc_host_crash_heap_classifier)(void *address);

static void mtlc_host_raise_stack_limit(void) {
  struct rlimit current;
  struct rlimit next;
  rlim_t want = MTLC_HOST_STACK_BYTES;
  if (getrlimit(RLIMIT_STACK, &current) != 0) {
    return;
  }
  if (current.rlim_max != RLIM_INFINITY && want > current.rlim_max) {
    want = current.rlim_max;
  }
  if (current.rlim_cur == RLIM_INFINITY || want <= current.rlim_cur) {
    return;
  }
  next.rlim_cur = want;
  next.rlim_max = current.rlim_max;
  (void)setrlimit(RLIMIT_STACK, &next);
}

__attribute__((constructor(101))) static void
mtlc_host_libc_startup(int argc, char **argv) {
  mtlc_host_stdin = stdin;
  mtlc_host_stdout = stdout;
  mtlc_host_stderr = stderr;
  (void)setvbuf(stdout, NULL, _IONBF, 0);
  mt_hosted_startup(argc, argv);
  mtlc_host_raise_stack_limit();
}

void *mtlc_host_memset(void *d, int v, size_t n) { return memset(d, v, n); }
void *mtlc_host_memcpy(void *d, const void *s, size_t n) {
  return memcpy(d, s, n);
}
void *mtlc_host_memmove(void *d, const void *s, size_t n) {
  return memmove(d, s, n);
}
void *mtlc_host_memchr(const void *m, int c, size_t n) {
  return memchr(m, c, n);
}
int mtlc_host_memcmp(const void *a, const void *b, size_t n) {
  return memcmp(a, b, n);
}
size_t mtlc_host_strlen(const char *s) { return strlen(s); }
int mtlc_host_strcmp(const char *a, const char *b) { return strcmp(a, b); }
int mtlc_host_strncmp(const char *a, const char *b, size_t n) {
  return strncmp(a, b, n);
}
char *mtlc_host_strchr(const char *s, int c) { return strchr(s, c); }
char *mtlc_host_strrchr(const char *s, int c) { return strrchr(s, c); }
char *mtlc_host_strncpy(char *d, const char *s, size_t n) {
  return strncpy(d, s, n);
}
char *mtlc_host_strcpy(char *d, const char *s) { return strcpy(d, s); }
char *mtlc_host_strcat(char *d, const char *s) { return strcat(d, s); }
char *mtlc_host_strstr(const char *s, const char *p) { return strstr(s, p); }
char *mtlc_host_strpbrk(const char *s, const char *a) {
  return strpbrk(s, a);
}
int mtlc_host_strcasecmp(const char *a, const char *b) {
  return strcasecmp(a, b);
}
char *mtlc_host_strtok(char *s, const char *d) { return strtok(s, d); }
char *mtlc_host_strdup(const char *s) { return strdup(s); }
char *mtlc_host_strerror(int e) { return strerror(e); }

void mtlc_host_alloc_report(void) {
  (void)fputs("Owned runtime heap: not used by the hosted build\n", stderr);
}
void *mtlc_host_malloc(size_t n) { return malloc(n); }
void *mtlc_host_calloc(size_t c, size_t n) { return calloc(c, n); }
void *mtlc_host_realloc(void *p, size_t n) { return realloc(p, n); }
void mtlc_host_free(void *p) { free(p); }

FILE *mtlc_host_fopen(const char *p, const char *m) { return fopen(p, m); }
int mtlc_host_fclose(FILE *f) { return fclose(f); }
size_t mtlc_host_fread(void *b, size_t s, size_t c, FILE *f) {
  return fread(b, s, c, f);
}
size_t mtlc_host_fwrite(const void *b, size_t s, size_t c, FILE *f) {
  return fwrite(b, s, c, f);
}
int mtlc_host_fputs(const char *s, FILE *f) { return fputs(s, f); }
int mtlc_host_puts(const char *s) { return puts(s); }
int mtlc_host_fputc(int c, FILE *f) { return fputc(c, f); }
int mtlc_host_putchar(int c) { return putchar(c); }
int mtlc_host_getchar(void) { return getchar(); }
char *mtlc_host_fgets(char *b, int n, FILE *f) { return fgets(b, n, f); }
int mtlc_host_fflush(FILE *f) { return fflush(f); }
int mtlc_host_ferror(FILE *f) { return ferror(f); }
int mtlc_host_fseek(FILE *f, long o, int w) { return fseek(f, o, w); }
long mtlc_host_ftell(FILE *f) { return ftell(f); }
int mtlc_host_fseeki64(FILE *f, long long o, int w) {
  return fseeko(f, (off_t)o, w);
}
long long mtlc_host_ftelli64(FILE *f) { return (long long)ftello(f); }
void mtlc_host_rewind(FILE *f) { rewind(f); }
int mtlc_host_setvbuf(FILE *f, char *b, int m, size_t n) {
  return setvbuf(f, b, m, n);
}
int mtlc_host_fileno(FILE *f) { return fileno(f); }
int mtlc_host_isatty(int d) { return isatty(d); }
int mtlc_host_remove(const char *p) { return remove(p); }
int mtlc_host_access(const char *p, int m) { return access(p, m); }
char *mtlc_host_getcwd(char *b, size_t n) { return getcwd(b, n); }
int mtlc_host_mkdir(const char *p, mode_t m) { return mkdir(p, m); }
int mtlc_host_gettimeofday(struct timeval *t, void *z) {
  return gettimeofday(t, z);
}
ssize_t mtlc_host_readlink(const char *p, char *b, size_t n) {
  return readlink(p, b, n);
}
char *mtlc_host_realpath(const char *p, char *r) { return realpath(p, r); }
int mtlc_host_stat(const char *p, struct stat *s) { return stat(p, s); }
int mtlc_host_unlink(const char *p) { return unlink(p); }
int mtlc_host_rename(const char *f, const char *t) { return rename(f, t); }
int mtlc_host_putenv(char *s) { return putenv(s); }
int mtlc_host_system(const char *c) { return system(c); }
FILE *mtlc_host_popen(const char *c, const char *m) { return popen(c, m); }
int mtlc_host_pclose(FILE *f) { return pclose(f); }

int mtlc_host_printf(const char *format, ...) {
  va_list arguments;
  int result;
  va_start(arguments, format);
  result = vprintf(format, arguments);
  va_end(arguments);
  return result;
}

int mtlc_host_fprintf(FILE *f, const char *format, ...) {
  va_list arguments;
  int result;
  va_start(arguments, format);
  result = vfprintf(f, format, arguments);
  va_end(arguments);
  return result;
}

int mtlc_host_vfprintf(FILE *f, const char *format, va_list arguments) {
  return vfprintf(f, format, arguments);
}

int mtlc_host_sprintf(char *b, const char *format, ...) {
  va_list arguments;
  int result;
  va_start(arguments, format);
  result = vsprintf(b, format, arguments);
  va_end(arguments);
  return result;
}

int mtlc_host_snprintf(char *b, size_t n, const char *format, ...) {
  va_list arguments;
  int result;
  va_start(arguments, format);
  result = vsnprintf(b, n, format, arguments);
  va_end(arguments);
  return result;
}

int mtlc_host_vsnprintf(char *b, size_t n, const char *format,
                        va_list arguments) {
  return vsnprintf(b, n, format, arguments);
}

int mtlc_host_sscanf(const char *input, const char *format, ...) {
  va_list arguments;
  int result;
  va_start(arguments, format);
  result = vsscanf(input, format, arguments);
  va_end(arguments);
  return result;
}

int mtlc_host_vsscanf(const char *input, const char *format,
                      va_list arguments) {
  return vsscanf(input, format, arguments);
}

int mtlc_host_atoi(const char *s) { return atoi(s); }
long mtlc_host_atol(const char *s) { return atol(s); }
long long mtlc_host_atoll(const char *s) { return atoll(s); }
double mtlc_host_atof(const char *s) { return atof(s); }
double mtlc_host_strtod(const char *s, char **e) { return strtod(s, e); }
unsigned long mtlc_host_strtoul(const char *s, char **e, int b) {
  return strtoul(s, e, b);
}
unsigned long long mtlc_host_strtoull(const char *s, char **e, int b) {
  return strtoull(s, e, b);
}
long long mtlc_host_strtoll(const char *s, char **e, int b) {
  return strtoll(s, e, b);
}
long long mtlc_host_strtoi64(const char *s, char **e, int b) {
  return strtoll(s, e, b);
}
unsigned long long mtlc_host_strtoui64(const char *s, char **e, int b) {
  return strtoull(s, e, b);
}
void mtlc_host_qsort(void *b, size_t n, size_t w,
                     int (*compare)(const void *, const void *)) {
  qsort(b, n, w, compare);
}
void *mtlc_host_bsearch(const void *k, const void *b, size_t n, size_t w,
                        int (*compare)(const void *, const void *)) {
  return bsearch(k, b, n, w, compare);
}
void mtlc_host_srand(unsigned int seed) { srand(seed); }
int mtlc_host_rand(void) { return rand(); }
char *mtlc_host_getenv(const char *n) { return getenv(n); }
clock_t mtlc_host_clock(void) { return clock(); }
int mtlc_host_clock_gettime(int id, void *t) {
  return clock_gettime((clockid_t)id, (struct timespec *)t);
}
void mtlc_host_abort(void) { abort(); }
void mtlc_host_exit(int status) { exit(status); }
void mtlc_host_immediate_exit(int status) { _exit(status); }

int mtlc_host_isspace(int c) { return isspace(c); }
int mtlc_host_isalpha(int c) { return isalpha(c); }
int mtlc_host_isalnum(int c) { return isalnum(c); }
int mtlc_host_isdigit(int c) { return isdigit(c); }
int mtlc_host_isxdigit(int c) { return isxdigit(c); }
int mtlc_host_isupper(int c) { return isupper(c); }
int mtlc_host_islower(int c) { return islower(c); }
int mtlc_host_isprint(int c) { return isprint(c); }
int mtlc_host_isgraph(int c) { return isgraph(c); }
int mtlc_host_ispunct(int c) { return ispunct(c); }
int mtlc_host_iscntrl(int c) { return iscntrl(c); }
int mtlc_host_tolower(int c) { return tolower(c); }
int mtlc_host_toupper(int c) { return toupper(c); }
float mtlc_host_sqrtf(float x) { return sqrtf(x); }
double mtlc_host_sqrt(double x) { return sqrt(x); }
double mtlc_host_fabs(double x) { return fabs(x); }
double mtlc_host_exp(double x) { return exp(x); }
float mtlc_host_expf(float x) { return expf(x); }
double mtlc_host_tanh(double x) { return tanh(x); }
float mtlc_host_tanhf(float x) { return tanhf(x); }
int *mtlc_host_errno_location(void) { return &errno; }

int mtlc_host_close(int d) { return close(d); }

int mtlc_host_ioctl(int d, unsigned long request, ...) {
  va_list arguments;
  void *value;
  va_start(arguments, request);
  value = va_arg(arguments, void *);
  va_end(arguments);
  return ioctl(d, request, value);
}

void *mtlc_host_mmap(void *a, size_t n, int p, int f, int d, off_t o) {
  return mmap(a, n, p, f, d, o);
}
int mtlc_host_munmap(void *a, size_t n) { return munmap(a, n); }
int mtlc_host_mprotect(void *a, size_t n, int p) { return mprotect(a, n, p); }
int mtlc_host_socket(int d, int t, int p) { return socket(d, t, p); }
int mtlc_host_connect(int s, const struct sockaddr *a, socklen_t n) {
  return connect(s, a, n);
}
int mtlc_host_bind(int s, const struct sockaddr *a, socklen_t n) {
  return bind(s, a, n);
}
int mtlc_host_listen(int s, int b) { return listen(s, b); }
int mtlc_host_accept(int s, struct sockaddr *a, socklen_t *n) {
  return accept(s, a, n);
}
int mtlc_host_setsockopt(int s, int l, int o, const void *v, socklen_t n) {
  return setsockopt(s, l, o, v, n);
}
ssize_t mtlc_host_send(int s, const void *b, size_t n, int f) {
  return send(s, b, n, f);
}
ssize_t mtlc_host_recv(int s, void *b, size_t n, int f) {
  return recv(s, b, n, f);
}
int mtlc_host_shutdown(int s, int h) { return shutdown(s, h); }
int mtlc_host_nanosleep(const struct timespec *r, struct timespec *m) {
  return nanosleep(r, m);
}
int mtlc_host_usleep(useconds_t u) { return usleep(u); }
uint16_t mtlc_host_htons(uint16_t v) { return htons(v); }
uint16_t mtlc_host_ntohs(uint16_t v) { return ntohs(v); }
uint32_t mtlc_host_htonl(uint32_t v) { return htonl(v); }
uint32_t mtlc_host_ntohl(uint32_t v) { return ntohl(v); }
in_addr_t mtlc_host_inet_addr(const char *c) { return inet_addr(c); }

void mtlc_host_rt_startup(long long argc, char **argv) {
  mt_hosted_startup((int)argc, argv);
}

int mtlc_host_run_process(const char *program, const char *const *arguments) {
  const char *path =
      program && !strchr(program, '/') ? getenv("PATH") : NULL;
  return mt_hosted_run_process(program, arguments, mt_hosted_environ(), path);
}

long mettle_spawn_process(const char *program, const char *const *arguments) {
  return (long)mt_hosted_spawn_process(program, arguments);
}

int mettle_poll_process(long pid, int *exit_code) {
  return mt_hosted_poll_process((long long)pid, exit_code);
}

void mettle_sleep_ms(unsigned milliseconds) { mt_hosted_sleep_ms(milliseconds); }

int mettle_file_mtime(const char *path, long long *seconds,
                      long long *nanoseconds) {
  return mt_hosted_file_mtime(path, seconds, nanoseconds);
}

int mtlc_host_find_executable(const char *program) {
  const char *path;
  size_t program_length;
  if (!program || !program[0]) {
    return 0;
  }
  if (strchr(program, '/')) {
    return access(program, F_OK) == 0;
  }
  path = getenv("PATH");
  if (!path) {
    path = "/usr/local/bin:/usr/bin:/bin";
  }
  program_length = strlen(program);
  while (*path) {
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
      if (access(candidate, F_OK) == 0) {
        return 1;
      }
    }
    path = *end ? end + 1 : end;
  }
  return 0;
}

int mtlc_host_install_signal_handler(int signal_number,
                                     void (*handler)(int, void *, void *)) {
  return mt_hosted_install_signal_handler(signal_number, handler);
}

int mtlc_host_address_is_readable(const void *address,
                                  unsigned long long length) {
  return mt_hosted_address_is_readable(address, length);
}

void mtlc_host_crash_write_stderr_bytes(const char *text,
                                        unsigned long long length) {
  while (text && length > 0) {
    ssize_t written = write(2, text, (size_t)length);
    if (written <= 0) {
      if (written < 0 && errno == EINTR) {
        continue;
      }
      return;
    }
    text += written;
    length -= (unsigned long long)written;
  }
}

void mtlc_host_crash_write_stderr(const char *text) {
  if (text) {
    mtlc_host_crash_write_stderr_bytes(text, strlen(text));
  }
}

void mtlc_host_crash_set_heap_classifier(long long (*classifier)(void *)) {
  mtlc_host_crash_heap_classifier = classifier;
}

long long mtlc_host_thread_create(void *attributes, unsigned long long stack,
                                  unsigned (*start)(void *), void *argument,
                                  unsigned flags, unsigned *thread_id) {
  (void)attributes;
  (void)flags;
  return mt_hosted_thread_create(stack, start, argument, thread_id);
}

unsigned mtlc_host_thread_wait(long long handle, unsigned milliseconds) {
  return mt_hosted_thread_wait(handle, milliseconds);
}

int mtlc_host_thread_close(long long handle) {
  return mt_hosted_thread_close(handle);
}

unsigned mtlc_host_thread_current_id(void) {
  return mt_hosted_thread_current_id();
}

void mettle_thread_sleep_ms(unsigned milliseconds) {
  mt_hosted_thread_sleep_ms(milliseconds);
}

void *mettle_thread_stack_high(void) { return mt_hosted_thread_stack_high(); }

long long mtlc_host_mutex_create(void *attributes, int initial_owner,
                                 const char *name) {
  (void)attributes;
  (void)name;
  return mt_hosted_mutex_create(initial_owner);
}

unsigned mtlc_host_mutex_wait(long long handle, unsigned milliseconds) {
  return mt_hosted_mutex_wait(handle, milliseconds);
}

int mtlc_host_mutex_release(long long handle) {
  return mt_hosted_mutex_release(handle);
}

int mtlc_host_mutex_close(long long handle) {
  return mt_hosted_mutex_close(handle);
}

int mtlc_host_atomic_compare_exchange_i32(volatile int *target, int exchange,
                                          int comparand) {
  __atomic_compare_exchange_n(target, &comparand, exchange, 0,
                              __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
  return comparand;
}

int mtlc_host_atomic_exchange_i32(volatile int *target, int value) {
  return __atomic_exchange_n(target, value, __ATOMIC_SEQ_CST);
}

int mtlc_host_atomic_inc_i32(volatile int *target) {
  return __atomic_add_fetch(target, 1, __ATOMIC_SEQ_CST);
}

int mtlc_host_atomic_dec_i32(volatile int *target) {
  return __atomic_sub_fetch(target, 1, __ATOMIC_SEQ_CST);
}

int mtlc_host_posix_get_errno(void) { return errno; }

void mtlc_host_posix_yield(void) { mt_hosted_yield(); }

int mtlc_host_posix_cas_i32(volatile int *target, int expected, int desired) {
  return __atomic_compare_exchange_n(target, &expected, desired, 0,
                                     __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

int mtlc_host_posix_atomic_exchange_i32(volatile int *target, int value) {
  return __atomic_exchange_n(target, value, __ATOMIC_SEQ_CST);
}

int mtlc_host_posix_atomic_add_i32(volatile int *target, int value) {
  return __atomic_fetch_add(target, value, __ATOMIC_SEQ_CST);
}

int mettle_path_exists(const char *path) {
  return path && access(path, F_OK) == 0;
}

int mettle_path_is_directory(const char *path) {
  return path && mt_hosted_is_directory(path);
}

int mettle_make_directory(const char *path) { return mkdir(path, 0777); }

int mettle_remove_directory(const char *path) { return rmdir(path); }

int mettle_move_path(const char *from, const char *to) {
  return rename(from, to);
}

int mettle_dir_list(const char *path, char *names, int capacity) {
  void *directory = path ? mt_hosted_dir_open(path) : NULL;
  const char *name;
  int type = 0;
  int used = 0;
  int count = 0;
  if (!directory || !names || capacity <= 0) {
    return -1;
  }
  while ((name = mt_hosted_dir_next(directory, &type))) {
    size_t name_length = strlen(name);
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
      continue;
    }
    if ((size_t)used + name_length + 2 > (size_t)capacity) {
      count = -1;
      break;
    }
    names[used++] = type == 4 ? 'd' : type == 8 ? 'f' : 'l';
    memcpy(names + used, name, name_length + 1);
    used += (int)name_length + 1;
    count++;
  }
  mt_hosted_dir_close(directory);
  return count;
}

int mettle_dir_exists(const char *path) {
  return mettle_path_is_directory(path);
}

int mettle_dir_create(const char *path) { return mkdir(path, 0755); }

int mettle_file_exists(const char *path) {
  return mettle_path_exists(path) && !mettle_path_is_directory(path);
}

long long mettle_executable_path(char *buffer, unsigned long long size) {
  return mt_hosted_executable_path(buffer, size);
}

char *mettle_realpath(const char *path, char *resolved) {
  return realpath(path, resolved);
}

int mettle_getcwd(char *buffer, int size) {
  return buffer && size > 0 && getcwd(buffer, (size_t)size) ? 0 : -1;
}

long long mettle_readlink(const char *path, char *buffer,
                          unsigned long long size) {
  return (long long)readlink(path, buffer, (size_t)size);
}

unsigned long long mettle_environment_write(char *buffer,
                                            unsigned long long size,
                                            const char *prefix) {
  char **item = mt_hosted_environ();
  size_t prefix_length = prefix ? strlen(prefix) : 0;
  unsigned long long used = 0;
  if (!buffer || size == 0) {
    return 0;
  }
  buffer[0] = '\0';
  for (; item && *item; item++) {
    const char *equals = strchr(*item, '=');
    size_t name_length;
    size_t value_length;
    unsigned long long need;
    if (!equals ||
        (prefix_length && strncmp(*item, prefix, prefix_length) != 0)) {
      continue;
    }
    name_length = (size_t)(equals - *item);
    value_length = strlen(equals + 1);
    need = (unsigned long long)name_length + value_length + 2;
    if (used + need >= size) {
      continue;
    }
    memcpy(buffer + used, *item, name_length);
    used += name_length;
    buffer[used++] = '=';
    memcpy(buffer + used, equals + 1, value_length);
    used += value_length;
    buffer[used++] = '\n';
    buffer[used] = '\0';
  }
  return used;
}
