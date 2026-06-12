/* Mettle interactive debug runtime (opt-in: --debug-hooks).
 *
 * The compiler instruments the program with calls to the mettle_dbg_*
 * symbols below; the internal linker pulls this object in when it sees them
 * undefined (mirroring crash_handler/profile auto-linking).
 *
 * With no debugger attached (METTLE_DBG_PIPE unset) every hook is a single
 * predictable-branch early-out. With one attached, the runtime connects to
 * the named pipe the editor's debug adapter owns and speaks a line-based
 * tab-separated protocol:
 *
 *   runtime -> adapter: hello, file/fn tables, stopped events, frame/var
 *                       listings, eval/set replies
 *   adapter -> runtime: breakpoint sets, continue/pause/step, stack/variable
 *                       queries, variable writes, detach
 *
 * Variable values are read (and written) through LIVE POINTERS the
 * instrumentation registered via mettle_dbg_local -- the IR pass takes the
 * address of every local and parameter, which forces a memory home, so the
 * pointer always sees the current value.
 *
 * Single-threaded model: only the thread that performs the first
 * mettle_dbg_enter (main) is debugged; hooks from other threads return
 * immediately. Windows-complete; on other platforms the hooks are no-ops
 * for now. */

/* Use MSVCRT's printf/strtod family directly: MinGW's C99-conformance
 * wrappers (__mingw_snprintf, __mingw_strtod, ...) are static-library code
 * the internal PE linker has no runtime for. Must precede every include. */
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

/* Route the printf family through MSVCRT's _vsnprintf (declared by the
 * headers), with the explicit NUL termination MS's variant does not
 * guarantee. */
static int dbg_snprintf_impl(char *buffer, size_t cap, const char *format,
                             ...) {
  va_list args;
  int written;
  if (!buffer || cap == 0) return 0;
  va_start(args, format);
  written = _vsnprintf(buffer, cap - 1, format, args);
  va_end(args);
  buffer[cap - 1] = '\0';
  return written < 0 ? (int)(cap - 1) : written;
}

static int dbg_vsnprintf_impl(char *buffer, size_t cap, const char *format,
                              va_list args) {
  int written;
  if (!buffer || cap == 0) return 0;
  written = _vsnprintf(buffer, cap - 1, format, args);
  buffer[cap - 1] = '\0';
  return written < 0 ? (int)(cap - 1) : written;
}

#define snprintf dbg_snprintf_impl
#define vsnprintf dbg_vsnprintf_impl

/* MinGW's stdlib.h redirects strtod to its __strtod helper; alias straight
 * to the MSVCRT export instead. */
extern double dbg_msvcrt_strtod(const char *str, char **end) __asm__("strtod");
#undef strtod
#define strtod dbg_msvcrt_strtod

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

/* --- value classification ------------------------------------------------- */

typedef enum {
  DBG_K_I8, DBG_K_U8, DBG_K_I16, DBG_K_U16, DBG_K_I32, DBG_K_U32,
  DBG_K_I64, DBG_K_U64, DBG_K_F32, DBG_K_F64, DBG_K_BOOL,
  DBG_K_PTR,    /* any T*, cstring, fn pointers: 8-byte address */
  DBG_K_STRING, /* the 16-byte { chars, length } struct */
  DBG_K_OTHER   /* structs, arrays: shown as their address */
} DbgKind;

static DbgKind dbg_classify_type(const char *type_name) {
  size_t len;
  if (!type_name) return DBG_K_OTHER;
  len = strlen(type_name);
  if (len > 0 && type_name[len - 1] == '*') return DBG_K_PTR;
  if (strchr(type_name, '[')) return DBG_K_OTHER;
  if (strcmp(type_name, "cstring") == 0) return DBG_K_PTR;
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

/* --- type metadata (the embedded struct layout tables) ----------------------- */

/* Index of `name` in the struct table, or -1. */
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

/* Parse `T[N]` into the element type (written to elem/elem_cap) and N.
 * Returns 1 on match. */
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

/* Byte size of a value of this type (0 = unknown). */
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

/* Whether a value of this type expands to children in the variables tree:
 * structs and arrays do; pointers do when they point at a struct or array. */
static int dbg_type_has_kids(const char *type_name) {
  if (!type_name) return 0;
  size_t len = strlen(type_name);
  if (len > 0 && type_name[len - 1] == '*') {
    char base[96];
    if (len - 1 >= sizeof(base)) return 0;
    memcpy(base, type_name, len - 1);
    base[len - 1] = '\0';
    /* strip any remaining pointer levels for the struct check */
    size_t blen = strlen(base);
    while (blen > 0 && base[blen - 1] == '*') base[--blen] = '\0';
    return dbg_struct_index(base, blen) >= 0 || strchr(base, '[') != NULL;
  }
  if (strchr(type_name, '[')) return 1;
  return dbg_struct_index(type_name, len) >= 0;
}

/* --- state ----------------------------------------------------------------- */

#define DBG_MAX_STACK 1024
#define DBG_MAX_LOCALS 8192
#define DBG_MAX_BREAKPOINTS 512
#define DBG_MAX_FILES 256
/* Keep line buffers (and so stack frames) under the 4KB Windows stack-probe
 * threshold: gcc emits ___chkstk_ms calls for larger frames, which the
 * internal PE linker has no runtime for. Protocol lines are short. */
#define DBG_LINE_MAX 1024

typedef struct {
  uint32_t fn_id;
  uint32_t line;
  uint32_t locals_base;
} DbgFrame;

typedef struct {
  const char *name;      /* points into the embedded .rdata string literal */
  const char *type_name;
  void *ptr;
  DbgKind kind;
  uint8_t is_param;
} DbgLocal;

typedef struct {
  uint32_t file_id;
  uint32_t line;
  char cond[160]; /* empty = unconditional */
} DbgBreakpoint;

typedef enum {
  DBG_RUN = 0,
  DBG_STEP_IN,
  DBG_STEP_OVER,
  DBG_STEP_OUT,
  DBG_PAUSE_REQ
} DbgRunMode;

static volatile LONG g_active = 0;     /* hooks early-out when 0 */
static DWORD g_main_thread = 0;
static HANDLE g_pipe = INVALID_HANDLE_VALUE;
static HANDLE g_reader_thread = NULL;

/* The big tables live on the heap (allocated in dbg_try_init): the internal
 * PE linker rejects runtime objects with very large .bss sections, and with
 * no debugger attached none of this memory is needed anyway. */
static DbgFrame *g_stack = NULL;
static uint32_t g_depth = 0;
static DbgLocal *g_locals = NULL;
static uint32_t g_local_count = 0;

static const char **g_file_paths = NULL; /* file_id -> path */
static uint32_t g_file_count = 0;
static uint32_t *g_fn_file = NULL;       /* fn_id -> file_id */

static CRITICAL_SECTION g_lock;
static CONDITION_VARIABLE g_wake;
static volatile LONG g_paused = 0;
static DbgRunMode g_mode = DBG_RUN;
static uint32_t g_step_depth = 0;
static DbgBreakpoint *g_breakpoints = NULL; /* heap: see the .bss note above */
static volatile LONG g_bp_count = 0;

/* Command queue: the reader thread enqueues lines for the paused program
 * thread to execute (queries must run on the thread that owns the frames). */
#define DBG_CMD_QUEUE 32
static char (*g_cmd_queue)[DBG_LINE_MAX] = NULL;
static uint32_t g_cmd_head = 0, g_cmd_tail = 0;

/* --- pipe I/O ----------------------------------------------------------------- */

static void dbg_send(const char *line) {
  DWORD written = 0;
  size_t len;
  if (g_pipe == INVALID_HANDLE_VALUE || !line) return;
  len = strlen(line);
  WriteFile(g_pipe, line, (DWORD)len, &written, NULL);
  WriteFile(g_pipe, "\n", 1, &written, NULL);
}

static void dbg_sendf(const char *format, ...) {
  char buffer[DBG_LINE_MAX];
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  dbg_send(buffer);
}

/* --- memory probing -------------------------------------------------------------- */

static int dbg_mem_readable(const void *ptr, size_t size) {
  MEMORY_BASIC_INFORMATION info;
  if (!ptr) return 0;
  if (VirtualQuery(ptr, &info, sizeof(info)) == 0) return 0;
  if (info.State != MEM_COMMIT) return 0;
  if (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
  /* conservative: require the whole range inside this region */
  return (const char *)ptr + size <=
         (const char *)info.BaseAddress + info.RegionSize;
}

/* --- value rendering ----------------------------------------------------------------- */

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
      /* show the address plus a short byte preview for cstring-ish data */
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

/* --- path resolution ----------------------------------------------------------------
 * `name(.field | ->field | [index])*` resolves to an address + type using
 * the embedded struct layout tables. Pointers auto-dereference for field
 * access, so `p.x` works whether p is `Point` or `Point*`. */

static DbgLocal *dbg_find_local_in_frame(uint32_t frame_index,
                                         const char *name, size_t name_len);

static int dbg_resolve_path(uint32_t frame_index, const char *path,
                            void **addr_out, char *type_buf, size_t type_cap) {
  const char *cursor = path;
  void *addr = NULL;

  /* leading identifier: a local/param in the frame */
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
      /* auto-deref any pointer levels before field access */
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
    return 0; /* unexpected character */
  }

  *addr_out = addr;
  return 1;
}

/* --- conditional breakpoints --------------------------------------------------------
 * Condition grammar: `<path> <op> <literal>` with op in == != < <= > >= and a
 * numeric/true/false literal. Evaluated on the program thread at hit time. */

static int dbg_condition_true(uint32_t frame_index, const char *cond) {
  char path[128];
  char op[3] = {0};
  char lit[64];
  if (sscanf(cond, "%127s %2s %63s", path, op, lit) != 3) {
    return 1; /* unparseable condition: stop rather than silently skip */
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

/* --- tables ----------------------------------------------------------------------- */

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

/* --- queries (run on the paused program thread) -------------------------------------- */

static void dbg_reply_stack(void) {
  for (uint32_t i = 0; i < g_depth; i++) {
    const DbgFrame *frame = &g_stack[g_depth - 1 - i]; /* top first */
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
  /* latest registration wins (block-scoped shadowing) */
  for (uint32_t i = end; i > begin; i--) {
    if (g_locals[i - 1].name && strlen(g_locals[i - 1].name) == name_len &&
        strncmp(g_locals[i - 1].name, name, name_len) == 0) {
      return &g_locals[i - 1];
    }
  }
  return NULL;
}

/* List the children of the value at `path`: struct fields, array elements
 * (capped), with pointers auto-dereferenced first. Same line shape as vars. */
static void dbg_reply_expand(uint32_t frame_index, const char *path) {
  void *addr = NULL;
  char type_buf[96];
  if (!dbg_resolve_path(frame_index, path, &addr, type_buf, sizeof(type_buf))) {
    dbg_send("varsdone");
    return;
  }
  /* auto-deref pointer levels so expanding `p: Point*` shows the fields */
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

/* --- command handling --------------------------------------------------------------------- */

/* Split a command line on tabs in place. Returns the field count. */
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

/* Apply a control command. Returns 1 when it resumes execution (the paused
 * loop should exit), 0 otherwise. Caller holds g_lock. */
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
    /* setbp <file_id> <comma-separated lines>: replace that file's set
     * (conditional breakpoints are re-added afterwards via bpadd) */
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
    /* bpadd <file_id> <line> <condition>: one conditional breakpoint */
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

/* Execute one query command (paused program thread only). */
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

/* Reader thread: parses lines off the pipe. Control commands apply
 * immediately; queries are queued for the paused program thread. */
static DWORD WINAPI dbg_reader_main(LPVOID unused) {
  char buffer[DBG_LINE_MAX];
  size_t buffered = 0;
  (void)unused;

  for (;;) {
    /* NEVER hold a blocking ReadFile on the (synchronous) pipe handle: Win32
     * serializes operations per handle, so a pending read would block the
     * program thread's WriteFile of the next `stopped` event -- a deadlock
     * until the adapter happens to send something. Peek, then read only
     * what is already there. */
    DWORD avail = 0;
    DWORD bytes_read = 0;
    if (!PeekNamedPipe(g_pipe, NULL, 0, NULL, &avail, NULL)) {
      /* adapter went away: keep running at full speed */
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
    if (!ReadFile(g_pipe, buffer + buffered, avail, &bytes_read, NULL) ||
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
            /* paused: everything runs on the program thread, in order */
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

/* --- the pause loop (program thread) --------------------------------------------------- */

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
        /* release the lock while reading program memory / writing the pipe */
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

/* --- crash interception --------------------------------------------------------------------
 * A vectored exception handler (registered first) turns a hardware fault
 * into a debugger stop at the faulting source line: the shadow stack and
 * variable registry are intact, so the full stack and every variable are
 * inspectable at the moment of the crash. `continue` returns
 * EXCEPTION_CONTINUE_SEARCH, handing the fault to the default handling
 * (the crash trace, then process death) -- a fault is not resumable. */

static LONG WINAPI dbg_vectored_handler(EXCEPTION_POINTERS *info) {
  static LONG in_handler = 0;
  DWORD code;
  if (!g_active || GetCurrentThreadId() != g_main_thread || !info ||
      !info->ExceptionRecord) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  code = info->ExceptionRecord->ExceptionCode;
  /* genuine faults only; stack overflow excluded (no stack left to pause on) */
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

/* --- initialization ----------------------------------------------------------------------- */

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
    return; /* adapter not listening: run normally */
  }

  g_stack = calloc(DBG_MAX_STACK, sizeof(DbgFrame));
  g_locals = calloc(DBG_MAX_LOCALS, sizeof(DbgLocal));
  g_cmd_queue = calloc(DBG_CMD_QUEUE, DBG_LINE_MAX);
  g_breakpoints = calloc(DBG_MAX_BREAKPOINTS, sizeof(DbgBreakpoint));
  if (!g_stack || !g_locals || !g_cmd_queue || !g_breakpoints) {
    CloseHandle(g_pipe);
    g_pipe = INVALID_HANDLE_VALUE;
    return;
  }

  g_main_thread = GetCurrentThreadId();
  InitializeCriticalSection(&g_lock);
  InitializeConditionVariable(&g_wake);
  dbg_build_and_send_tables();

  g_reader_thread = CreateThread(NULL, 0, dbg_reader_main, NULL, 0, NULL);
  if (!g_reader_thread) {
    CloseHandle(g_pipe);
    g_pipe = INVALID_HANDLE_VALUE;
    return;
  }
  AddVectoredExceptionHandler(1, dbg_vectored_handler);
  InterlockedExchange(&g_active, 1);
}

/* --- the instrumentation hooks -------------------------------------------------------------- */

void mettle_dbg_enter(uint32_t fn_id) {
  static LONG initialized = 0;
  if (InterlockedCompareExchange(&initialized, 1, 0) == 0) {
    dbg_try_init();
    if (g_active) {
      /* hold at the very first function until the adapter configures
       * breakpoints and resumes (or requests a stop-on-entry) */
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
    /* re-registration in the same frame (loop-scoped declarations) updates
     * in place; shadowing in nested blocks appends and lookup prefers the
     * latest entry */
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
      /* condition evaluated OUTSIDE the lock: it reads program memory via
       * the path resolver, which only this (the program) thread touches */
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

#else /* non-Windows: hooks are no-ops until the POSIX transport lands */

void mettle_dbg_enter(uint32_t fn_id) { (void)fn_id; }
void mettle_dbg_exit(void) {}
void mettle_dbg_local(int64_t local_id, void *ptr, int64_t is_param) {
  (void)local_id; (void)ptr; (void)is_param;
}
void mettle_dbg_line(uint32_t line) { (void)line; }

#endif
