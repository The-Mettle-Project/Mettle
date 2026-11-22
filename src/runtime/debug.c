
#if defined(_WIN32) || defined(_WIN64)
#define __USE_MINGW_ANSI_STDIO 0
#endif

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) || defined(_WIN64)

#include <windows.h>

extern int dbg_owned_vsnprintf(char *buffer, size_t cap, const char *format,
                               va_list args) __asm__("vsnprintf");

static int dbg_snprintf_impl(char *buffer, size_t cap, const char *format,
                             ...) {
  va_list args;
  int written;
  if (!buffer || cap == 0) return 0;
  va_start(args, format);
  written = dbg_owned_vsnprintf(buffer, cap, format, args);
  va_end(args);
  buffer[cap - 1] = '\0';
  return written < 0 ? (int)(cap - 1) : written;
}

static int dbg_vsnprintf_impl(char *buffer, size_t cap, const char *format,
                              va_list args) {
  int written;
  if (!buffer || cap == 0) return 0;
  written = dbg_owned_vsnprintf(buffer, cap, format, args);
  buffer[cap - 1] = '\0';
  return written < 0 ? (int)(cap - 1) : written;
}

#define snprintf dbg_snprintf_impl
#define vsnprintf dbg_vsnprintf_impl

extern double dbg_owned_strtod(const char *str, char **end) __asm__("strtod");
#undef strtod
#define strtod dbg_owned_strtod

extern int64_t dbg_owned_strtoi64(const char *str, char **end, int base)
    __asm__("_strtoi64");
extern uint64_t dbg_owned_strtoui64(const char *str, char **end, int base)
    __asm__("_strtoui64");
#undef _strtoi64
#undef _strtoui64
#define _strtoi64 dbg_owned_strtoi64
#define _strtoui64 dbg_owned_strtoui64

#else

typedef int HANDLE;
typedef long LONG;
typedef unsigned long DWORD;
#define INVALID_HANDLE_VALUE (-1)
#define INFINITE 0xFFFFFFFFul

extern long read(int fd, void *buffer, unsigned long count);
extern long write(int fd, const void *buffer, unsigned long count);
extern int close(int fd);
#if defined(MT_HOSTED)
extern int ioctl(int fd, unsigned long request, ...);
#else
extern int ioctl(int fd, unsigned long request, void *argument);
#endif
extern void usleep(unsigned long microseconds);
extern unsigned int mettle_thread_current_id(void);
extern int32_t mettle_atomic_exchange_i32(int32_t *target, int32_t value);
extern int32_t mettle_atomic_compare_exchange_i32(int32_t *target,
                                                  int32_t exchange,
                                                  int32_t comparand);
extern int pthread_create(void *thread, const void *attr,
                          void *(*start)(void *), void *argument);
extern int pthread_mutex_init(void *mutex, const void *attr);
extern int pthread_mutex_lock(void *mutex);
extern int pthread_mutex_unlock(void *mutex);
extern int pthread_cond_init(void *cond, const void *attr);
extern int pthread_cond_wait(void *cond, void *mutex);
extern int pthread_cond_broadcast(void *cond);

#if defined(MT_HOSTED)
typedef struct { unsigned char opaque[64]; } __attribute__((aligned(16)))
CRITICAL_SECTION;
typedef struct { unsigned char opaque[64]; } __attribute__((aligned(16)))
CONDITION_VARIABLE;
#else
typedef struct { unsigned char opaque[64]; } CRITICAL_SECTION;
typedef struct { unsigned char opaque[64]; } CONDITION_VARIABLE;
#endif

#define InitializeCriticalSection(cs) pthread_mutex_init((cs), 0)
#define EnterCriticalSection(cs) pthread_mutex_lock((cs))
#define LeaveCriticalSection(cs) pthread_mutex_unlock((cs))
#define InitializeConditionVariable(cv) pthread_cond_init((cv), 0)
#define WakeAllConditionVariable(cv) pthread_cond_broadcast((cv))
#define SleepConditionVariableCS(cv, cs, ms) pthread_cond_wait((cv), (cs))
#define GetCurrentThreadId() ((DWORD)mettle_thread_current_id())
#define Sleep(ms) usleep((unsigned long)(ms) * 1000ul)
#define InterlockedExchange(target, value)                                     \
  ((LONG)mettle_atomic_exchange_i32((int32_t *)(target), (int32_t)(value)))
#define InterlockedCompareExchange(target, exchange, comparand)                \
  ((LONG)mettle_atomic_compare_exchange_i32(                                   \
      (int32_t *)(target), (int32_t)(exchange), (int32_t)(comparand)))

static int dbg_posix_write(HANDLE fd, const void *buffer, DWORD count,
                           DWORD *written, void *unused) {
  const char *at = (const char *)buffer;
  unsigned long remaining = (unsigned long)count;
  int retries = 0;
  (void)unused;
  while (remaining > 0) {
    long n = write(fd, at, remaining);
    if (n > 0) {
      at += n;
      remaining -= (unsigned long)n;
      retries = 0;
      continue;
    }
    if (n == 0 || ++retries > 64) {
      break;
    }
  }
  if (written) *written = (DWORD)(count - remaining);
  return remaining == 0;
}

static int dbg_posix_read(HANDLE fd, void *buffer, DWORD count, DWORD *got,
                          void *unused) {
  long n;
  (void)unused;
  n = read(fd, buffer, (unsigned long)count);
  if (got) *got = n > 0 ? (DWORD)n : 0;
  return n > 0;
}

#if defined(MT_HOSTED) && defined(__APPLE__)
#define DBG_FIONREAD 0x4004667Ful
#else
#define DBG_FIONREAD 0x541Bul
#endif

static int dbg_posix_peek(HANDLE fd, void *a, DWORD b, void *c, DWORD *avail,
                          void *d) {
  int pending = 0;
  (void)a; (void)b; (void)c; (void)d;
  if (ioctl(fd, DBG_FIONREAD, &pending) != 0) {
    return 0;
  }
  if (avail) *avail = pending > 0 ? (DWORD)pending : 0;
  return 1;
}

static HANDLE dbg_posix_open(const char *path) {
  FILE *stream = fopen(path, "r+b");
  return stream ? fileno(stream) : INVALID_HANDLE_VALUE;
}

extern unsigned long dbg_owned_strtoul(const char *str, char **end, int base)
    __asm__("strtoul");
extern long long dbg_owned_strtoll(const char *str, char **end, int base)
    __asm__("strtoll");
extern unsigned long long dbg_owned_strtoull(const char *str, char **end,
                                             int base) __asm__("strtoull");
extern double dbg_owned_strtod_posix(const char *str, char **end)
    __asm__("strtod");
extern int dbg_owned_sscanf(const char *input, const char *format, ...)
    __asm__("sscanf");
#undef sscanf
#define sscanf dbg_owned_sscanf
#undef strtoul
#undef strtoll
#undef strtoull
#undef strtod
#define strtoul dbg_owned_strtoul
#define strtoll dbg_owned_strtoll
#define strtoull dbg_owned_strtoull
#define strtod dbg_owned_strtod_posix

#define _strtoi64 dbg_owned_strtoll
#define _strtoui64 dbg_owned_strtoull

extern int mettle_address_is_readable(const void *address,
                                      unsigned long long length);
extern int mettle_install_signal_handler(int signal_number,
                                         void (*handler)(int, void *, void *));
#define GetCurrentProcessId() ((DWORD)mettle_thread_current_id())

static DWORD dbg_posix_getenv(const char *name, char *buffer, DWORD cap) {
  const char *value = getenv(name);
  DWORD i = 0;
  if (!value || !buffer || cap == 0) return 0;
  while (value[i] && i + 1 < cap) {
    buffer[i] = value[i];
    i++;
  }
  buffer[i] = '\0';
  return i;
}

typedef DWORD (*DbgThreadStart)(void *);
static DbgThreadStart g_dbg_thread_start = 0;

static void *dbg_posix_thread_trampoline(void *argument) {
  if (g_dbg_thread_start) {
    (void)g_dbg_thread_start(argument);
  }
  return 0;
}

static HANDLE dbg_posix_spawn(DbgThreadStart start) {
  unsigned long thread = 0;
  g_dbg_thread_start = start;
  if (pthread_create(&thread, 0, dbg_posix_thread_trampoline, 0) != 0) {
    return 0;
  }
  return 1;
}

#define CreateThread(sec, stack, start, arg, flags, id)                        \
  dbg_posix_spawn((start))

#define GENERIC_READ 0
#define GENERIC_WRITE 0
#define OPEN_EXISTING 0
#define CreateFileA(path, access, share, sec, disp, flags, tmpl)               \
  dbg_posix_open((path))

#define WriteFile(h, buf, n, wr, ov) dbg_posix_write((h), (buf), (n), (wr), (ov))
#define ReadFile(h, buf, n, got, ov) dbg_posix_read((h), (buf), (n), (got), (ov))
#define PeekNamedPipe(h, a, b, c, avail, d)                                    \
  dbg_posix_peek((h), (a), (b), (c), (avail), (d))
#define CloseHandle(h) close((h))
#define GetEnvironmentVariableA(name, buf, cap)                                \
  dbg_posix_getenv((name), (buf), (cap))

#define WINAPI
typedef void *LPVOID;

#endif

extern uint64_t mettle_profile_name_count;
extern const char *mettle_profile_names[];
extern const char *mettle_profile_files[];
extern uint64_t mettle_profile_lines[];
extern uint64_t mettle_dbg_local_count;
extern const char *mettle_dbg_local_names[];
extern const char *mettle_dbg_local_types[];
extern uint64_t mettle_dbg_struct_count;
extern const char *mettle_dbg_struct_names[];
extern uint64_t mettle_dbg_struct_sizes[];
extern uint64_t mettle_dbg_struct_field_start[];
extern uint64_t mettle_dbg_struct_field_count[];
extern const char *mettle_dbg_field_names[];
extern const char *mettle_dbg_field_types[];
extern uint64_t mettle_dbg_field_offsets[];

typedef enum {
  DBG_K_I8, DBG_K_U8, DBG_K_I16, DBG_K_U16, DBG_K_I32, DBG_K_U32,
  DBG_K_I64, DBG_K_U64, DBG_K_F32, DBG_K_F64, DBG_K_BOOL,
  DBG_K_PTR,
  DBG_K_STRING,
  DBG_K_OTHER
} DbgKind;

static DbgKind dbg_classify_type(const char *type_name) {
  size_t len;
  if (!type_name) return DBG_K_OTHER;
  len = strlen(type_name);
  if (len > 0 && type_name[len - 1] == '*') return DBG_K_PTR;
  if (strchr(type_name, '[')) return DBG_K_OTHER;
  if (strcmp(type_name, "cstring") == 0) return DBG_K_PTR;
  if (strcmp(type_name, "rawptr") == 0) return DBG_K_PTR;
  if (strncmp(type_name, "fn", 2) == 0 && (len == 2 || type_name[2] == '(')) return DBG_K_PTR;
  if (strcmp(type_name, "string") == 0) return DBG_K_STRING;
  if (strcmp(type_name, "bool") == 0) return DBG_K_BOOL;
  if (strcmp(type_name, "int8") == 0) return DBG_K_I8;
  if (strcmp(type_name, "uint8") == 0) return DBG_K_U8;
  if (strcmp(type_name, "int16") == 0) return DBG_K_I16;
  if (strcmp(type_name, "uint16") == 0) return DBG_K_U16;
  if (strcmp(type_name, "int32") == 0) return DBG_K_I32;
  if (strcmp(type_name, "uint32") == 0) return DBG_K_U32;
  if (strcmp(type_name, "int64") == 0) return DBG_K_I64;
  if (strcmp(type_name, "uint64") == 0) return DBG_K_U64;
  if (strcmp(type_name, "float32") == 0) return DBG_K_F32;
  if (strcmp(type_name, "float64") == 0) return DBG_K_F64;
  return DBG_K_OTHER;
}

static int64_t dbg_struct_index(const char *name, size_t name_len) {
  for (uint64_t i = 0; i < mettle_dbg_struct_count; i++) {
    const char *candidate = mettle_dbg_struct_names[i];
    if (candidate && strlen(candidate) == name_len &&
        strncmp(candidate, name, name_len) == 0) {
      return (int64_t)i;
    }
  }
  return -1;
}

static int dbg_parse_array_type(const char *type_name, char *elem,
                                size_t elem_cap, uint64_t *count_out) {
  const char *bracket = type_name ? strrchr(type_name, '[') : NULL;
  if (!bracket || bracket == type_name) return 0;
  uint64_t n = _strtoui64(bracket + 1, NULL, 10);
  size_t base_len = (size_t)(bracket - type_name);
  if (base_len + 1 > elem_cap) return 0;
  memcpy(elem, type_name, base_len);
  elem[base_len] = '\0';
  *count_out = n;
  return 1;
}

static uint64_t dbg_type_size(const char *type_name) {
  switch (dbg_classify_type(type_name)) {
  case DBG_K_I8: case DBG_K_U8: case DBG_K_BOOL: return 1;
  case DBG_K_I16: case DBG_K_U16: return 2;
  case DBG_K_I32: case DBG_K_U32: case DBG_K_F32: return 4;
  case DBG_K_I64: case DBG_K_U64: case DBG_K_F64: case DBG_K_PTR: return 8;
  case DBG_K_STRING: return 16;
  case DBG_K_OTHER:
  default: {
    char elem[96];
    uint64_t n = 0;
    if (dbg_parse_array_type(type_name, elem, sizeof(elem), &n)) {
      return n * dbg_type_size(elem);
    }
    int64_t s = dbg_struct_index(type_name, type_name ? strlen(type_name) : 0);
    return s >= 0 ? mettle_dbg_struct_sizes[s] : 0;
  }
  }
}

static int dbg_type_has_kids(const char *type_name) {
  if (!type_name) return 0;
  size_t len = strlen(type_name);
  if (len > 0 && type_name[len - 1] == '*') {
    char base[96];
    if (len - 1 >= sizeof(base)) return 0;
    memcpy(base, type_name, len - 1);
    base[len - 1] = '\0';
    size_t blen = strlen(base);
    while (blen > 0 && base[blen - 1] == '*') base[--blen] = '\0';
    return dbg_struct_index(base, blen) >= 0 || strchr(base, '[') != NULL;
  }
  if (strchr(type_name, '[')) return 1;
  return dbg_struct_index(type_name, len) >= 0;
}

#define DBG_MAX_STACK 1024
#define DBG_MAX_LOCALS 8192
#define DBG_MAX_BREAKPOINTS 512
#define DBG_MAX_FILES 256
#define DBG_LINE_MAX 1024

typedef struct {
  uint32_t fn_id;
  uint32_t line;
  uint32_t locals_base;
} DbgFrame;

typedef struct {
  const char *name;
  const char *type_name;
  void *ptr;
  DbgKind kind;
  uint8_t is_param;
} DbgLocal;

typedef struct {
  uint32_t file_id;
  uint32_t line;
  char cond[160];
} DbgBreakpoint;

typedef enum {
  DBG_RUN = 0,
  DBG_STEP_IN,
  DBG_STEP_OVER,
  DBG_STEP_OUT,
  DBG_PAUSE_REQ
} DbgRunMode;

static volatile LONG g_active = 0;
static DWORD g_main_thread = 0;
static HANDLE g_pipe = INVALID_HANDLE_VALUE;
static HANDLE g_pipe_in = INVALID_HANDLE_VALUE;
static HANDLE g_reader_thread = 0;

static DbgFrame *g_stack = NULL;
static uint32_t g_depth = 0;
static DbgLocal *g_locals = NULL;
static uint32_t g_local_count = 0;

static const char **g_file_paths = NULL;
static uint32_t g_file_count = 0;
static uint32_t *g_fn_file = NULL;

static CRITICAL_SECTION g_lock;
static CONDITION_VARIABLE g_wake;
static volatile LONG g_paused = 0;
static DbgRunMode g_mode = DBG_RUN;
static uint32_t g_step_depth = 0;
static DbgBreakpoint *g_breakpoints = NULL;
static volatile LONG g_bp_count = 0;

#define DBG_CMD_QUEUE 32
static char (*g_cmd_queue)[DBG_LINE_MAX] = NULL;
static uint32_t g_cmd_head = 0, g_cmd_tail = 0;

static void dbg_send(const char *line) {
  char framed[DBG_LINE_MAX + 1];
  DWORD written = 0;
  size_t len;
  if (g_pipe == INVALID_HANDLE_VALUE || !line) return;
  len = strlen(line);
  if (len > DBG_LINE_MAX) len = DBG_LINE_MAX;
  memcpy(framed, line, len);
  framed[len] = '\n';
  WriteFile(g_pipe, framed, (DWORD)(len + 1), &written, NULL);
}

static void dbg_sendf(const char *format, ...) {
  char buffer[DBG_LINE_MAX];
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  dbg_send(buffer);
}

static int dbg_mem_readable(const void *ptr, size_t size) {
#if defined(_WIN32) || defined(_WIN64)
  MEMORY_BASIC_INFORMATION info;
  if (!ptr) return 0;
  if (VirtualQuery(ptr, &info, sizeof(info)) == 0) return 0;
  if (info.State != MEM_COMMIT) return 0;
  if (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
  return (const char *)ptr + size <=
         (const char *)info.BaseAddress + info.RegionSize;
#else
  if (!ptr) return 0;
  return mettle_address_is_readable(ptr, (unsigned long long)size);
#endif
}

static void dbg_escape_into(char *out, size_t out_cap, const char *bytes,
                            size_t count) {
  size_t o = 0;
  for (size_t i = 0; i < count && o + 4 < out_cap; i++) {
    unsigned char c = (unsigned char)bytes[i];
    if (c == '\t' || c == '\n' || c == '\r' || c < 0x20 || c > 0x7e) {
      o += (size_t)snprintf(out + o, out_cap - o, "\\x%02x", c);
    } else {
      out[o++] = (char)c;
    }
  }
  out[o] = '\0';
}

static void dbg_format_value(void *p, const char *type_name, DbgKind kind,
                             char *out, size_t cap) {
  if (!dbg_mem_readable(p, 1)) {
    snprintf(out, cap, "<unreadable>");
    return;
  }
  switch (kind) {
  case DBG_K_I8:  snprintf(out, cap, "%d", (int)*(int8_t *)p); break;
  case DBG_K_U8:  snprintf(out, cap, "%u", (unsigned)*(uint8_t *)p); break;
  case DBG_K_I16: snprintf(out, cap, "%d", (int)*(int16_t *)p); break;
  case DBG_K_U16: snprintf(out, cap, "%u", (unsigned)*(uint16_t *)p); break;
  case DBG_K_I32: snprintf(out, cap, "%d", *(int32_t *)p); break;
  case DBG_K_U32: snprintf(out, cap, "%u", *(uint32_t *)p); break;
  case DBG_K_I64: snprintf(out, cap, "%lld", (long long)*(int64_t *)p); break;
  case DBG_K_U64: snprintf(out, cap, "%llu", (unsigned long long)*(uint64_t *)p); break;
  case DBG_K_F32: snprintf(out, cap, "%g", (double)*(float *)p); break;
  case DBG_K_F64: snprintf(out, cap, "%g", *(double *)p); break;
  case DBG_K_BOOL:
    snprintf(out, cap, "%s", *(uint8_t *)p ? "true" : "false");
    break;
  case DBG_K_PTR: {
    const char *target = *(const char **)p;
    if (!target) {
      snprintf(out, cap, "null");
    } else if (dbg_mem_readable(target, 1)) {
      char preview[80];
      size_t n = 0;
      while (n < 24 && dbg_mem_readable(target + n, 1) && target[n] != '\0' &&
             target[n] >= 0x20 && target[n] <= 0x7e) {
        n++;
      }
      if (n > 0 && (target[n] == '\0' || n == 24)) {
        dbg_escape_into(preview, sizeof(preview), target, n);
        snprintf(out, cap, "0x%llx \"%s%s\"", (unsigned long long)(uintptr_t)target,
                 preview, n == 24 ? "..." : "");
      } else {
        snprintf(out, cap, "0x%llx", (unsigned long long)(uintptr_t)target);
      }
    } else {
      snprintf(out, cap, "0x%llx", (unsigned long long)(uintptr_t)target);
    }
    break;
  }
  case DBG_K_STRING: {
    const char *chars = *(const char **)p;
    uint64_t length = *(uint64_t *)((char *)p + 8);
    if (!chars || !dbg_mem_readable(chars, 1)) {
      snprintf(out, cap, "string(len=%llu)", (unsigned long long)length);
    } else {
      char preview[160];
      size_t take = length < 48 ? (size_t)length : 48;
      if (!dbg_mem_readable(chars, take)) take = 0;
      dbg_escape_into(preview, sizeof(preview), chars, take);
      snprintf(out, cap, "\"%s%s\" (len=%llu)", preview,
               (uint64_t)take < length ? "..." : "",
               (unsigned long long)length);
    }
    break;
  }
  case DBG_K_OTHER:
  default:
    snprintf(out, cap, "%s @0x%llx", type_name ? type_name : "?",
             (unsigned long long)(uintptr_t)p);
    break;
  }
}

static int dbg_write_value(void *p, DbgKind kind, const char *text) {
  if (!dbg_mem_readable(p, 1) || !text) return 0;
  switch (kind) {
  case DBG_K_I8:  *(int8_t *)p = (int8_t)_strtoi64(text, NULL, 0); return 1;
  case DBG_K_U8:  *(uint8_t *)p = (uint8_t)_strtoui64(text, NULL, 0); return 1;
  case DBG_K_I16: *(int16_t *)p = (int16_t)_strtoi64(text, NULL, 0); return 1;
  case DBG_K_U16: *(uint16_t *)p = (uint16_t)_strtoui64(text, NULL, 0); return 1;
  case DBG_K_I32: *(int32_t *)p = (int32_t)_strtoi64(text, NULL, 0); return 1;
  case DBG_K_U32: *(uint32_t *)p = (uint32_t)_strtoui64(text, NULL, 0); return 1;
  case DBG_K_I64: *(int64_t *)p = (int64_t)_strtoi64(text, NULL, 0); return 1;
  case DBG_K_U64: *(uint64_t *)p = (uint64_t)_strtoui64(text, NULL, 0); return 1;
  case DBG_K_F32: *(float *)p = (float)strtod(text, NULL); return 1;
  case DBG_K_F64: *(double *)p = strtod(text, NULL); return 1;
  case DBG_K_BOOL:
    *(uint8_t *)p = (strcmp(text, "true") == 0 || _strtoi64(text, NULL, 0) != 0)
                        ? 1 : 0;
    return 1;
  case DBG_K_PTR: *(uint64_t *)p = _strtoui64(text, NULL, 0); return 1;
  default: return 0;
  }
}

static DbgLocal *dbg_find_local_in_frame(uint32_t frame_index,
                                         const char *name, size_t name_len);

static int dbg_resolve_path(uint32_t frame_index, const char *path,
                            void **addr_out, char *type_buf, size_t type_cap) {
  const char *cursor = path;
  void *addr = NULL;

  const char *start = cursor;
  while (*cursor && (*cursor == '_' || (*cursor >= 'a' && *cursor <= 'z') ||
                     (*cursor >= 'A' && *cursor <= 'Z') ||
                     (*cursor >= '0' && *cursor <= '9'))) {
    cursor++;
  }
  if (cursor == start) return 0;
  {
    DbgLocal *local = dbg_find_local_in_frame(frame_index, start,
                                              (size_t)(cursor - start));
    if (!local) return 0;
    addr = local->ptr;
    snprintf(type_buf, type_cap, "%s", local->type_name ? local->type_name : "?");
  }

  while (*cursor) {
    if (cursor[0] == '.' || (cursor[0] == '-' && cursor[1] == '>')) {
      cursor += cursor[0] == '.' ? 1 : 2;
      size_t tlen = strlen(type_buf);
      while (tlen > 0 && type_buf[tlen - 1] == '*') {
        if (!dbg_mem_readable(addr, 8)) return 0;
        addr = *(void **)addr;
        type_buf[--tlen] = '\0';
        if (!addr) return 0;
      }
      const char *fstart = cursor;
      while (*cursor && (*cursor == '_' || (*cursor >= 'a' && *cursor <= 'z') ||
                         (*cursor >= 'A' && *cursor <= 'Z') ||
                         (*cursor >= '0' && *cursor <= '9'))) {
        cursor++;
      }
      if (cursor == fstart) return 0;
      int64_t s = dbg_struct_index(type_buf, tlen);
      if (s < 0) return 0;
      uint64_t fbase = mettle_dbg_struct_field_start[s];
      uint64_t fcount = mettle_dbg_struct_field_count[s];
      int found = 0;
      for (uint64_t f = 0; f < fcount; f++) {
        const char *fname = mettle_dbg_field_names[fbase + f];
        if (fname && strlen(fname) == (size_t)(cursor - fstart) &&
            strncmp(fname, fstart, (size_t)(cursor - fstart)) == 0) {
          addr = (char *)addr + mettle_dbg_field_offsets[fbase + f];
          snprintf(type_buf, type_cap, "%s", mettle_dbg_field_types[fbase + f]);
          found = 1;
          break;
        }
      }
      if (!found) return 0;
      continue;
    }
    if (cursor[0] == '[') {
      char *end = NULL;
      uint64_t index = _strtoui64(cursor + 1, &end, 10);
      if (!end || *end != ']') return 0;
      cursor = end + 1;
      char elem[96];
      uint64_t n = 0;
      size_t tlen = strlen(type_buf);
      if (dbg_parse_array_type(type_buf, elem, sizeof(elem), &n)) {
        if (index >= n) return 0;
        uint64_t esize = dbg_type_size(elem);
        if (esize == 0) return 0;
        addr = (char *)addr + index * esize;
        snprintf(type_buf, type_cap, "%s", elem);
        continue;
      }
      if (tlen > 0 && type_buf[tlen - 1] == '*') {
        if (!dbg_mem_readable(addr, 8)) return 0;
        void *base = *(void **)addr;
        if (!base) return 0;
        type_buf[tlen - 1] = '\0';
        uint64_t esize = dbg_type_size(type_buf);
        if (esize == 0) return 0;
        addr = (char *)base + index * esize;
        continue;
      }
      return 0;
    }
    return 0;
  }

  *addr_out = addr;
  return 1;
}

static int dbg_condition_true(uint32_t frame_index, const char *cond) {
  char path[128];
  char op[3] = {0};
  char lit[64];
  if (sscanf(cond, "%127s %2s %63s", path, op, lit) != 3) {
    return 1;
  }
  void *addr = NULL;
  char type_buf[96];
  if (!dbg_resolve_path(frame_index, path, &addr, type_buf, sizeof(type_buf))) {
    return 1;
  }
  DbgKind kind = dbg_classify_type(type_buf);
  double actual;
  if (!dbg_mem_readable(addr, dbg_type_size(type_buf) ? dbg_type_size(type_buf) : 1)) {
    return 1;
  }
  switch (kind) {
  case DBG_K_I8: actual = (double)*(int8_t *)addr; break;
  case DBG_K_U8: case DBG_K_BOOL: actual = (double)*(uint8_t *)addr; break;
  case DBG_K_I16: actual = (double)*(int16_t *)addr; break;
  case DBG_K_U16: actual = (double)*(uint16_t *)addr; break;
  case DBG_K_I32: actual = (double)*(int32_t *)addr; break;
  case DBG_K_U32: actual = (double)*(uint32_t *)addr; break;
  case DBG_K_I64: actual = (double)*(int64_t *)addr; break;
  case DBG_K_U64: actual = (double)*(uint64_t *)addr; break;
  case DBG_K_F32: actual = (double)*(float *)addr; break;
  case DBG_K_F64: actual = *(double *)addr; break;
  case DBG_K_PTR: actual = (double)(uintptr_t)*(void **)addr; break;
  default: return 1;
  }
  double expected;
  if (strcmp(lit, "true") == 0) expected = 1;
  else if (strcmp(lit, "false") == 0) expected = 0;
  else if (strncmp(lit, "0x", 2) == 0 || strncmp(lit, "0X", 2) == 0) {
    expected = (double)_strtoui64(lit, NULL, 16);
  } else expected = strtod(lit, NULL);

  if (strcmp(op, "==") == 0) return actual == expected;
  if (strcmp(op, "!=") == 0) return actual != expected;
  if (strcmp(op, "<") == 0) return actual < expected;
  if (strcmp(op, "<=") == 0) return actual <= expected;
  if (strcmp(op, ">") == 0) return actual > expected;
  if (strcmp(op, ">=") == 0) return actual >= expected;
  return 1;
}

static uint32_t dbg_intern_file(const char *path) {
  for (uint32_t i = 0; i < g_file_count; i++) {
    if (strcmp(g_file_paths[i], path) == 0) return i;
  }
  if (g_file_count >= DBG_MAX_FILES) return 0;
  g_file_paths[g_file_count] = path;
  return g_file_count++;
}

static void dbg_build_and_send_tables(void) {
  uint64_t count = mettle_profile_name_count;
  g_file_paths = calloc(DBG_MAX_FILES, sizeof(char *));
  g_fn_file = calloc(count ? (size_t)count : 1u, sizeof(uint32_t));
  if (!g_file_paths || !g_fn_file) return;

  dbg_sendf("hello\t%lu", (unsigned long)GetCurrentProcessId());
  for (uint64_t i = 0; i < count; i++) {
    const char *file = mettle_profile_files[i] ? mettle_profile_files[i] : "?";
    g_fn_file[i] = dbg_intern_file(file);
  }
  for (uint32_t i = 0; i < g_file_count; i++) {
    dbg_sendf("file\t%u\t%s", i, g_file_paths[i]);
  }
  for (uint64_t i = 0; i < count; i++) {
    dbg_sendf("fn\t%llu\t%u\t%s", (unsigned long long)i, g_fn_file[i],
              mettle_profile_names[i] ? mettle_profile_names[i] : "?");
  }
  dbg_send("tablesdone");
}

static void dbg_reply_stack(void) {
  for (uint32_t i = 0; i < g_depth; i++) {
    const DbgFrame *frame = &g_stack[g_depth - 1 - i];
    dbg_sendf("frame\t%u\t%u\t%u", i, frame->fn_id, frame->line);
  }
  dbg_send("framesdone");
}

static DbgFrame *dbg_frame_at(uint32_t top_relative_index) {
  if (top_relative_index >= g_depth) return NULL;
  return &g_stack[g_depth - 1 - top_relative_index];
}

static void dbg_frame_local_range(const DbgFrame *frame, uint32_t *begin,
                                  uint32_t *end) {
  uint32_t frame_pos = (uint32_t)(frame - g_stack);
  *begin = frame->locals_base;
  *end = (frame_pos + 1 < g_depth) ? g_stack[frame_pos + 1].locals_base
                                   : g_local_count;
}

static void dbg_reply_vars(uint32_t frame_index) {
  DbgFrame *frame = dbg_frame_at(frame_index);
  if (frame) {
    uint32_t begin = 0, end = 0;
    char value[256];
    dbg_frame_local_range(frame, &begin, &end);
    for (uint32_t i = begin; i < end; i++) {
      dbg_format_value(g_locals[i].ptr, g_locals[i].type_name,
                       g_locals[i].kind, value, sizeof(value));
      dbg_sendf("var\t%s\t%s\t%d\t%d\t%s", g_locals[i].name,
                g_locals[i].type_name ? g_locals[i].type_name : "?",
                (int)g_locals[i].is_param,
                dbg_type_has_kids(g_locals[i].type_name), value);
    }
  }
  dbg_send("varsdone");
}

static DbgLocal *dbg_find_local_in_frame(uint32_t frame_index,
                                         const char *name, size_t name_len) {
  DbgFrame *frame = dbg_frame_at(frame_index);
  uint32_t begin = 0, end = 0;
  if (!frame || !name) return NULL;
  dbg_frame_local_range(frame, &begin, &end);
  for (uint32_t i = end; i > begin; i--) {
    if (g_locals[i - 1].name && strlen(g_locals[i - 1].name) == name_len &&
        strncmp(g_locals[i - 1].name, name, name_len) == 0) {
      return &g_locals[i - 1];
    }
  }
  return NULL;
}

static void dbg_reply_expand(uint32_t frame_index, const char *path) {
  void *addr = NULL;
  char type_buf[96];
  if (!dbg_resolve_path(frame_index, path, &addr, type_buf, sizeof(type_buf))) {
    dbg_send("varsdone");
    return;
  }
  size_t tlen = strlen(type_buf);
  int guard = 0;
  while (tlen > 0 && type_buf[tlen - 1] == '*' && guard++ < 4) {
    if (!dbg_mem_readable(addr, 8)) { dbg_send("varsdone"); return; }
    addr = *(void **)addr;
    type_buf[--tlen] = '\0';
    if (!addr) { dbg_send("varsdone"); return; }
  }

  char value[256];
  char elem[96];
  uint64_t n = 0;
  if (dbg_parse_array_type(type_buf, elem, sizeof(elem), &n)) {
    uint64_t esize = dbg_type_size(elem);
    DbgKind ekind = dbg_classify_type(elem);
    int ekids = dbg_type_has_kids(elem);
    uint64_t shown = n < 128 ? n : 128;
    for (uint64_t i = 0; esize > 0 && i < shown; i++) {
      void *eaddr = (char *)addr + i * esize;
      dbg_format_value(eaddr, elem, ekind, value, sizeof(value));
      dbg_sendf("var\t[%llu]\t%s\t0\t%d\t%s", (unsigned long long)i, elem,
                ekids, value);
    }
    if (shown < n) {
      dbg_sendf("var\t...\t%s\t0\t0\t(%llu more elements)", elem,
                (unsigned long long)(n - shown));
    }
    dbg_send("varsdone");
    return;
  }

  int64_t s = dbg_struct_index(type_buf, tlen);
  if (s >= 0) {
    uint64_t fbase = mettle_dbg_struct_field_start[s];
    uint64_t fcount = mettle_dbg_struct_field_count[s];
    for (uint64_t f = 0; f < fcount; f++) {
      const char *fname = mettle_dbg_field_names[fbase + f];
      const char *ftype = mettle_dbg_field_types[fbase + f];
      void *faddr = (char *)addr + mettle_dbg_field_offsets[fbase + f];
      dbg_format_value(faddr, ftype, dbg_classify_type(ftype), value,
                       sizeof(value));
      dbg_sendf("var\t%s\t%s\t0\t%d\t%s", fname ? fname : "?", ftype,
                dbg_type_has_kids(ftype), value);
    }
  }
  dbg_send("varsdone");
}

static void dbg_reply_eval(uint32_t frame_index, const char *path) {
  void *addr = NULL;
  char type_buf[96];
  if (!dbg_resolve_path(frame_index, path, &addr, type_buf, sizeof(type_buf))) {
    dbg_send("evalr\t0\tnot a variable in this frame");
    return;
  }
  char value[256];
  dbg_format_value(addr, type_buf, dbg_classify_type(type_buf), value,
                   sizeof(value));
  dbg_sendf("evalr\t1\t%s\t%d\t%s", type_buf, dbg_type_has_kids(type_buf),
            value);
}

static void dbg_reply_set(uint32_t frame_index, const char *path,
                          const char *text) {
  void *addr = NULL;
  char type_buf[96];
  if (!dbg_resolve_path(frame_index, path, &addr, type_buf, sizeof(type_buf)) ||
      !dbg_write_value(addr, dbg_classify_type(type_buf), text)) {
    dbg_send("setr\t0\t");
    return;
  }
  char value[256];
  dbg_format_value(addr, type_buf, dbg_classify_type(type_buf), value,
                   sizeof(value));
  dbg_sendf("setr\t1\t%s", value);
}

static int dbg_split(char *line, char *fields[], int max_fields) {
  int count = 0;
  char *cursor = line;
  while (count < max_fields) {
    fields[count++] = cursor;
    cursor = strchr(cursor, '\t');
    if (!cursor) break;
    *cursor++ = '\0';
  }
  return count;
}

static int dbg_apply_control(char *fields[], int field_count) {
  const char *verb = fields[0];
  if (strcmp(verb, "go") == 0) {
    g_mode = DBG_RUN;
    return 1;
  }
  if (strcmp(verb, "stepin") == 0) {
    g_mode = DBG_STEP_IN;
    return 1;
  }
  if (strcmp(verb, "next") == 0) {
    g_mode = DBG_STEP_OVER;
    g_step_depth = g_depth;
    return 1;
  }
  if (strcmp(verb, "stepout") == 0) {
    g_mode = DBG_STEP_OUT;
    g_step_depth = g_depth;
    return 1;
  }
  if (strcmp(verb, "pause") == 0) {
    g_mode = DBG_PAUSE_REQ;
    return 0;
  }
  if (strcmp(verb, "detach") == 0) {
    InterlockedExchange(&g_active, 0);
    g_mode = DBG_RUN;
    return 1;
  }
  if (strcmp(verb, "setbp") == 0 && field_count >= 3) {
    uint32_t file_id = (uint32_t)strtoul(fields[1], NULL, 10);
    LONG kept = 0;
    for (LONG i = 0; i < g_bp_count; i++) {
      if (g_breakpoints[i].file_id != file_id) {
        g_breakpoints[kept++] = g_breakpoints[i];
      }
    }
    g_bp_count = kept;
    {
      char *cursor = fields[2];
      while (cursor && *cursor && g_bp_count < DBG_MAX_BREAKPOINTS) {
        uint32_t line = (uint32_t)strtoul(cursor, &cursor, 10);
        if (line > 0) {
          g_breakpoints[g_bp_count].file_id = file_id;
          g_breakpoints[g_bp_count].line = line;
          g_breakpoints[g_bp_count].cond[0] = '\0';
          g_bp_count++;
        }
        if (*cursor == ',') cursor++;
        else break;
      }
    }
    return 0;
  }
  if (strcmp(verb, "bpadd") == 0 && field_count >= 4) {
    if (g_bp_count < DBG_MAX_BREAKPOINTS) {
      g_breakpoints[g_bp_count].file_id =
          (uint32_t)strtoul(fields[1], NULL, 10);
      g_breakpoints[g_bp_count].line = (uint32_t)strtoul(fields[2], NULL, 10);
      strncpy(g_breakpoints[g_bp_count].cond, fields[3],
              sizeof(g_breakpoints[g_bp_count].cond) - 1);
      g_breakpoints[g_bp_count].cond[sizeof(g_breakpoints[0].cond) - 1] = '\0';
      g_bp_count++;
    }
    return 0;
  }
  if (strcmp(verb, "clearall") == 0) {
    g_bp_count = 0;
    return 0;
  }
  return 0;
}

static void dbg_apply_query(char *fields[], int field_count) {
  const char *verb = fields[0];
  if (strcmp(verb, "stack") == 0) {
    dbg_reply_stack();
  } else if (strcmp(verb, "vars") == 0 && field_count >= 2) {
    dbg_reply_vars((uint32_t)strtoul(fields[1], NULL, 10));
  } else if (strcmp(verb, "expand") == 0 && field_count >= 3) {
    dbg_reply_expand((uint32_t)strtoul(fields[1], NULL, 10), fields[2]);
  } else if (strcmp(verb, "eval") == 0 && field_count >= 3) {
    dbg_reply_eval((uint32_t)strtoul(fields[1], NULL, 10), fields[2]);
  } else if (strcmp(verb, "set") == 0 && field_count >= 4) {
    dbg_reply_set((uint32_t)strtoul(fields[1], NULL, 10), fields[2], fields[3]);
  }
}

static int dbg_is_query(const char *verb) {
  return strcmp(verb, "stack") == 0 || strcmp(verb, "vars") == 0 ||
         strcmp(verb, "expand") == 0 || strcmp(verb, "eval") == 0 ||
         strcmp(verb, "set") == 0;
}

static DWORD WINAPI dbg_reader_main(LPVOID unused) {
  char buffer[DBG_LINE_MAX];
  size_t buffered = 0;
  (void)unused;

  for (;;) {
    DWORD avail = 0;
    DWORD bytes_read = 0;
    if (!PeekNamedPipe(g_pipe_in, NULL, 0, NULL, &avail, NULL)) {
      EnterCriticalSection(&g_lock);
      InterlockedExchange(&g_active, 0);
      g_mode = DBG_RUN;
      WakeAllConditionVariable(&g_wake);
      LeaveCriticalSection(&g_lock);
      return 0;
    }
    if (avail == 0) {
      Sleep(5);
      continue;
    }
    if (avail > (DWORD)(sizeof(buffer) - buffered - 1)) {
      avail = (DWORD)(sizeof(buffer) - buffered - 1);
    }
    if (!ReadFile(g_pipe_in, buffer + buffered, avail, &bytes_read, NULL) ||
        bytes_read == 0) {
      EnterCriticalSection(&g_lock);
      InterlockedExchange(&g_active, 0);
      g_mode = DBG_RUN;
      WakeAllConditionVariable(&g_wake);
      LeaveCriticalSection(&g_lock);
      return 0;
    }
    buffered += bytes_read;
    buffer[buffered] = '\0';

    char *line_start = buffer;
    for (;;) {
      char *newline = strchr(line_start, '\n');
      if (!newline) break;
      *newline = '\0';
      if (newline > line_start && newline[-1] == '\r') newline[-1] = '\0';

      EnterCriticalSection(&g_lock);
      {
        char working[DBG_LINE_MAX];
        char *fields[8];
        int field_count;
        strncpy(working, line_start, sizeof(working) - 1);
        working[sizeof(working) - 1] = '\0';
        field_count = dbg_split(working, fields, 8);
        if (field_count > 0) {
          if (dbg_is_query(fields[0]) ||
              (g_paused && !dbg_is_query(fields[0]))) {
            uint32_t next_tail = (g_cmd_tail + 1) % DBG_CMD_QUEUE;
            if (next_tail != g_cmd_head) {
              strncpy(g_cmd_queue[g_cmd_tail], line_start, DBG_LINE_MAX - 1);
              g_cmd_queue[g_cmd_tail][DBG_LINE_MAX - 1] = '\0';
              g_cmd_tail = next_tail;
            }
            WakeAllConditionVariable(&g_wake);
          } else {
            dbg_apply_control(fields, field_count);
          }
        }
      }
      LeaveCriticalSection(&g_lock);
      line_start = newline + 1;
    }
    buffered = strlen(line_start);
    memmove(buffer, line_start, buffered + 1);
  }
}

static DWORD g_exc_code = 0;
static uint64_t g_exc_addr = 0;

static void dbg_pause_here(const char *reason) {
  DbgFrame *top = g_depth > 0 ? &g_stack[g_depth - 1] : NULL;
  uint32_t fn_id = top ? top->fn_id : 0;
  uint32_t line = top ? top->line : 0;
  uint32_t file_id =
      (g_fn_file && fn_id < mettle_profile_name_count) ? g_fn_file[fn_id] : 0;

  EnterCriticalSection(&g_lock);
  g_paused = 1;
  LeaveCriticalSection(&g_lock);

  if (strcmp(reason, "exception") == 0) {
    dbg_sendf("stopped\t%s\t%u\t%u\t%u\t%u\t0x%lx\t0x%llx", reason, file_id,
              line, g_depth, fn_id, (unsigned long)g_exc_code,
              (unsigned long long)g_exc_addr);
  } else {
    dbg_sendf("stopped\t%s\t%u\t%u\t%u\t%u", reason, file_id, line, g_depth,
              fn_id);
  }

  EnterCriticalSection(&g_lock);
  for (;;) {
    int resumed = 0;
    while (g_cmd_head == g_cmd_tail && g_active) {
      SleepConditionVariableCS(&g_wake, &g_lock, INFINITE);
    }
    if (!g_active) break;
    while (g_cmd_head != g_cmd_tail) {
      char working[DBG_LINE_MAX];
      char *fields[8];
      int field_count;
      strncpy(working, g_cmd_queue[g_cmd_head], sizeof(working) - 1);
      working[sizeof(working) - 1] = '\0';
      g_cmd_head = (g_cmd_head + 1) % DBG_CMD_QUEUE;
      field_count = dbg_split(working, fields, 8);
      if (field_count == 0) continue;
      if (dbg_is_query(fields[0])) {
        LeaveCriticalSection(&g_lock);
        dbg_apply_query(fields, field_count);
        EnterCriticalSection(&g_lock);
      } else if (dbg_apply_control(fields, field_count)) {
        resumed = 1;
        break;
      }
    }
    if (resumed) break;
  }
  g_paused = 0;
  LeaveCriticalSection(&g_lock);
}

#if !defined(_WIN32) && !defined(_WIN64)

static void dbg_signal_handler(int signal_number, void *address, void *context) {
  static LONG in_handler = 0;
  (void)context;
  if (!g_active || GetCurrentThreadId() != g_main_thread) {
    return;
  }
  if (InterlockedCompareExchange(&in_handler, 1, 0) != 0) {
    return;
  }
  g_exc_code = (DWORD)signal_number;
  g_exc_addr = (uint64_t)(uintptr_t)address;
  EnterCriticalSection(&g_lock);
  g_mode = DBG_RUN;
  LeaveCriticalSection(&g_lock);
  dbg_pause_here("exception");
  InterlockedExchange(&in_handler, 0);
}

static void dbg_install_fault_handlers(void) {
  static const int signals[] = {11, 8, 4, 7};
  for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
    (void)mettle_install_signal_handler(signals[i], dbg_signal_handler);
  }
}

#else

static LONG WINAPI dbg_vectored_handler(EXCEPTION_POINTERS *info) {
  static LONG in_handler = 0;
  DWORD code;
  if (!g_active || GetCurrentThreadId() != g_main_thread || !info ||
      !info->ExceptionRecord) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  code = info->ExceptionRecord->ExceptionCode;
  if (code != EXCEPTION_ACCESS_VIOLATION &&
      code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
      code != EXCEPTION_ILLEGAL_INSTRUCTION &&
      code != EXCEPTION_PRIV_INSTRUCTION &&
      code != EXCEPTION_ARRAY_BOUNDS_EXCEEDED &&
      code != EXCEPTION_FLT_DIVIDE_BY_ZERO) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  if (InterlockedCompareExchange(&in_handler, 1, 0) != 0) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  g_exc_code = code;
  g_exc_addr = (uint64_t)(uintptr_t)info->ExceptionRecord->ExceptionAddress;
  EnterCriticalSection(&g_lock);
  g_mode = DBG_RUN;
  LeaveCriticalSection(&g_lock);
  dbg_pause_here("exception");
  InterlockedExchange(&in_handler, 0);
  return EXCEPTION_CONTINUE_SEARCH;
}

#endif

static void dbg_close_pipes(void) {
  if (g_pipe_in != INVALID_HANDLE_VALUE && g_pipe_in != g_pipe) {
    CloseHandle(g_pipe_in);
  }
  if (g_pipe != INVALID_HANDLE_VALUE) {
    CloseHandle(g_pipe);
  }
  g_pipe_in = INVALID_HANDLE_VALUE;
  g_pipe = INVALID_HANDLE_VALUE;
}

static void dbg_try_init(void) {
  char pipe_name[512];
  DWORD got = GetEnvironmentVariableA("METTLE_DBG_PIPE", pipe_name,
                                      sizeof(pipe_name));
  if (got == 0 || got >= sizeof(pipe_name)) {
    return;
  }

  g_pipe = CreateFileA(pipe_name, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                       OPEN_EXISTING, 0, NULL);
  if (g_pipe == INVALID_HANDLE_VALUE) {
    return;
  }
#if defined(_WIN32) || defined(_WIN64)
  g_pipe_in = g_pipe;
#else
  if (got + 4 > sizeof(pipe_name)) {
    dbg_close_pipes();
    return;
  }
  memcpy(pipe_name + got, ".in", 4);
  g_pipe_in = CreateFileA(pipe_name, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                          OPEN_EXISTING, 0, NULL);
  if (g_pipe_in == INVALID_HANDLE_VALUE) {
    dbg_close_pipes();
    return;
  }
#endif

  g_stack = calloc(DBG_MAX_STACK, sizeof(DbgFrame));
  g_locals = calloc(DBG_MAX_LOCALS, sizeof(DbgLocal));
  g_cmd_queue = calloc(DBG_CMD_QUEUE, DBG_LINE_MAX);
  g_breakpoints = calloc(DBG_MAX_BREAKPOINTS, sizeof(DbgBreakpoint));
  if (!g_stack || !g_locals || !g_cmd_queue || !g_breakpoints) {
    dbg_close_pipes();
    return;
  }

  g_main_thread = GetCurrentThreadId();
  InitializeCriticalSection(&g_lock);
  InitializeConditionVariable(&g_wake);
  dbg_build_and_send_tables();

  g_reader_thread = CreateThread(NULL, 0, dbg_reader_main, NULL, 0, NULL);
  if (!g_reader_thread) {
    dbg_close_pipes();
    return;
  }
#if defined(_WIN32) || defined(_WIN64)
  AddVectoredExceptionHandler(1, dbg_vectored_handler);
#else
  dbg_install_fault_handlers();
#endif
  InterlockedExchange(&g_active, 1);
}

void mettle_dbg_enter(uint32_t fn_id) {
  static LONG initialized = 0;
  if (InterlockedCompareExchange(&initialized, 1, 0) == 0) {
    dbg_try_init();
    if (g_active) {
      if (g_depth < DBG_MAX_STACK) {
        g_stack[g_depth].fn_id = fn_id;
        g_stack[g_depth].line = 0;
        g_stack[g_depth].locals_base = g_local_count;
        g_depth++;
      }
      dbg_pause_here("entry");
      return;
    }
  }
  if (!g_active || GetCurrentThreadId() != g_main_thread) return;
  if (g_depth < DBG_MAX_STACK) {
    g_stack[g_depth].fn_id = fn_id;
    g_stack[g_depth].line = 0;
    g_stack[g_depth].locals_base = g_local_count;
    g_depth++;
  }
}

void mettle_dbg_exit(void) {
  if (!g_active || GetCurrentThreadId() != g_main_thread) return;
  if (g_depth > 0) {
    g_depth--;
    g_local_count = g_stack[g_depth].locals_base;
  }
}

void mettle_dbg_local(int64_t local_id, void *ptr, int64_t is_param) {
  if (!g_active || GetCurrentThreadId() != g_main_thread) return;
  if (g_depth == 0) return;
  if (local_id < 0 || (uint64_t)local_id >= mettle_dbg_local_count) return;
  {
    const char *name = mettle_dbg_local_names[local_id];
    const char *type_name = mettle_dbg_local_types[local_id];
    DbgFrame *top = &g_stack[g_depth - 1];
    for (uint32_t i = g_local_count; i > top->locals_base; i--) {
      if (g_locals[i - 1].ptr == ptr &&
          strcmp(g_locals[i - 1].name, name) == 0) {
        g_locals[i - 1].type_name = type_name;
        g_locals[i - 1].kind = dbg_classify_type(type_name);
        return;
      }
    }
    if (g_local_count >= DBG_MAX_LOCALS) return;
    g_locals[g_local_count].name = name;
    g_locals[g_local_count].type_name = type_name;
    g_locals[g_local_count].ptr = ptr;
    g_locals[g_local_count].kind = dbg_classify_type(type_name);
    g_locals[g_local_count].is_param = is_param ? 1 : 0;
    g_local_count++;
  }
}

void mettle_dbg_line(uint32_t line) {
  if (!g_active || GetCurrentThreadId() != g_main_thread) return;
  if (g_depth == 0) return;

  {
    DbgFrame *top = &g_stack[g_depth - 1];
    top->line = line;

    DbgRunMode mode = g_mode;
    const char *reason = NULL;

    if (mode == DBG_STEP_IN) {
      reason = "step";
    } else if (mode == DBG_STEP_OVER && g_depth <= g_step_depth) {
      reason = "step";
    } else if (mode == DBG_STEP_OUT && g_depth < g_step_depth) {
      reason = "step";
    } else if (mode == DBG_PAUSE_REQ) {
      reason = "pause";
    }

    if (!reason && g_bp_count > 0) {
      uint32_t file_id = (g_fn_file && top->fn_id < mettle_profile_name_count)
                             ? g_fn_file[top->fn_id]
                             : 0;
      char cond[160];
      int hit = 0;
      cond[0] = '\0';
      EnterCriticalSection(&g_lock);
      for (LONG i = 0; i < g_bp_count; i++) {
        if (g_breakpoints[i].line == line &&
            g_breakpoints[i].file_id == file_id) {
          hit = 1;
          strncpy(cond, g_breakpoints[i].cond, sizeof(cond) - 1);
          cond[sizeof(cond) - 1] = '\0';
          break;
        }
      }
      LeaveCriticalSection(&g_lock);
      if (hit && (cond[0] == '\0' || dbg_condition_true(0, cond))) {
        reason = "breakpoint";
      }
    }

    if (reason) {
      EnterCriticalSection(&g_lock);
      g_mode = DBG_RUN;
      LeaveCriticalSection(&g_lock);
      dbg_pause_here(reason);
    }
  }
}

