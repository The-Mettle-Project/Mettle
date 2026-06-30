#include "profile.h"
#include "crash_handler.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#else
#include <fcntl.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>
#endif

extern uint64_t mettle_profile_name_count;
extern const char *mettle_profile_names[];
extern const char *mettle_profile_files[];
extern uint64_t mettle_profile_lines[];

typedef struct {
  uint64_t call_count;
  uint64_t total_ns;
  uint64_t self_ns;
  uint64_t max_ns;
} MettleProfileStats;

typedef struct {
  uint64_t counts[METTLE_PROFILE_OP_CLASS_COUNT];
} MettleProfileOpStats;

typedef struct {
  uint32_t fn_id;
  uint64_t start_ns;
  uint64_t child_ns;
} MettleProfileFrame;

typedef struct {
  uint32_t caller_id;
  uint32_t callee_id;
  uint64_t call_count;
  uint64_t total_ns;
} MettleProfileEdge;

#define METTLE_PROFILE_MAX_STACK 1024u
#define METTLE_PROFILE_INITIAL_CAPACITY 16u
#define NS_PER_US 1000u

static MettleProfileStats *g_stats = NULL;
static MettleProfileOpStats *g_op_stats = NULL;
static size_t g_stats_capacity = 0;
static size_t g_op_stats_capacity = 0;
static MettleProfileFrame g_stack[METTLE_PROFILE_MAX_STACK];
static size_t g_stack_depth = 0;
static size_t g_stack_overflow_exits = 0;
static MettleProfileEdge *g_edges = NULL;
static size_t g_edge_count = 0;
static size_t g_edge_capacity = 0;
static uint64_t *g_block_counts = NULL;
static size_t g_block_capacity = 0;
static size_t g_block_max_id = 0; /* highest block id ever seen, + 1 = count */
#if defined(_WIN32) || defined(_WIN64)
static int g_qpc_initialized = 0;
static LARGE_INTEGER g_qpc_frequency = {0};
#endif

static void mettle_profile_write_padded_field(const char *text, size_t width) {
  size_t length = text ? strlen(text) : 0;
  mettle_crash_write_stderr(text ? text : "");
  while (length < width) {
    mettle_crash_write_stderr(" ");
    length++;
  }
}

static int uint64_to_decimal(uint64_t value, char *buf, size_t buf_size) {
  size_t index = buf_size;
  int count = 0;

  buf[--index] = '\0';
  if (value == 0) {
    buf[--index] = '0';
    count = 1;
  } else {
    while (value != 0 && index > 0) {
      buf[--index] = (char)('0' + (value % 10u));
      value /= 10u;
      count++;
    }
  }

  if (index > 0) {
    memmove(buf, buf + index, (size_t)count + 1);
  }
  return count;
}

static void mettle_profile_write_padded_uint64(uint64_t value, size_t width) {
  char buffer[32];
  size_t length = (size_t)uint64_to_decimal(value, buffer, sizeof(buffer));

  mettle_crash_write_stderr(buffer);
  while (length < width) {
    mettle_crash_write_stderr(" ");
    length++;
  }
}

static void mettle_profile_write_pct(double pct) {
  char buffer[16];
  size_t index = 0;
  int whole = 0;
  int tenths = 0;
  int digits[4];
  int digit_count = 0;
  int value = 0;

  if (pct < 0.0) {
    pct = 0.0;
  }
  if (pct > 100.0) {
    pct = 100.0;
  }

  whole = (int)pct;
  tenths = (int)((pct - (double)whole) * 10.0 + 0.5);
  if (tenths >= 10) {
    whole++;
    tenths = 0;
  }

  value = whole;
  if (value == 0) {
    digits[digit_count++] = 0;
  } else {
    while (value > 0 && digit_count < 4) {
      digits[digit_count++] = value % 10;
      value /= 10;
    }
  }

  while (digit_count > 0) {
    buffer[index++] = (char)('0' + digits[--digit_count]);
  }
  buffer[index++] = '.';
  buffer[index++] = (char)('0' + tenths);
  buffer[index++] = '%';
  buffer[index] = '\0';
  mettle_crash_write_stderr(buffer);
}

static void mettle_profile_write_location(uint32_t fn_id, size_t width) {
  char buffer[160];
  const char *file = "?";
  uint64_t line = 0;
  size_t index = 0;

  if (fn_id < (uint32_t)mettle_profile_name_count) {
    if (mettle_profile_files && mettle_profile_files[fn_id]) {
      file = mettle_profile_files[fn_id];
    }
    if (mettle_profile_lines) {
      line = mettle_profile_lines[fn_id];
    }
  }

  while (file[index] != '\0' && index + 1 < sizeof(buffer)) {
    buffer[index] = file[index];
    index++;
  }

  if (line > 0 && index + 2 < sizeof(buffer)) {
    uint64_t remaining = line;
    char digits[24];
    size_t digit_count = 0;

    buffer[index++] = ':';
    while (remaining > 0 && digit_count < sizeof(digits)) {
      digits[digit_count++] = (char)('0' + (remaining % 10u));
      remaining /= 10u;
    }
    while (digit_count > 0 && index + 1 < sizeof(buffer)) {
      buffer[index++] = digits[--digit_count];
    }
  }

  buffer[index] = '\0';
  mettle_profile_write_padded_field(buffer, width);
}

#if defined(_WIN32) || defined(_WIN64)
static void mettle_profile_init_qpc(void) {
  if (g_qpc_initialized) {
    return;
  }
  QueryPerformanceFrequency(&g_qpc_frequency);
  g_qpc_initialized = 1;
}
#endif

static uint64_t mettle_profile_now_ns(void) {
#if defined(_WIN32) || defined(_WIN64)
  LARGE_INTEGER counter = {0};

  mettle_profile_init_qpc();
  if (g_qpc_frequency.QuadPart == 0) {
    return 0;
  }

  QueryPerformanceCounter(&counter);
  return (uint64_t)((counter.QuadPart * 1000000000ULL) /
                    (uint64_t)g_qpc_frequency.QuadPart);
#else
#if defined(CLOCK_MONOTONIC)
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
  }
#endif
  {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000000ULL +
           (uint64_t)tv.tv_usec * 1000ULL;
  }
#endif
}

#define METTLE_PROFILE_ENSURE_CAPACITY(ptr, count, capacity, type, needed) do { \
    if ((needed) > (capacity)) { \
        size_t new_cap = (capacity) ? (capacity) : METTLE_PROFILE_INITIAL_CAPACITY; \
        while (new_cap < (needed)) new_cap *= 2; \
        type *new_buf = realloc((ptr), new_cap * sizeof(type)); \
        if (!new_buf) break; \
        memset(new_buf + (capacity), 0, (new_cap - (capacity)) * sizeof(type)); \
        (ptr) = new_buf; \
        (capacity) = new_cap; \
    } \
} while (0)

static int mettle_profile_ensure_stats(uint32_t fn_id) {
  size_t needed = (size_t)fn_id + 1u;

  if (needed <= g_stats_capacity) {
    return 1;
  }

  METTLE_PROFILE_ENSURE_CAPACITY(g_stats, g_stats_capacity, g_stats_capacity,
                                 MettleProfileStats, needed);
  return needed <= g_stats_capacity;
}

static int mettle_profile_ensure_op_stats(uint32_t fn_id) {
  size_t needed = (size_t)fn_id + 1u;

  if (needed <= g_op_stats_capacity) {
    return 1;
  }

  METTLE_PROFILE_ENSURE_CAPACITY(g_op_stats, g_op_stats_capacity,
                                 g_op_stats_capacity, MettleProfileOpStats,
                                 needed);
  return needed <= g_op_stats_capacity;
}

static MettleProfileEdge *mettle_profile_find_edge(uint32_t caller_id,
                                                   uint32_t callee_id) {
  for (size_t i = 0; i < g_edge_count; i++) {
    if (g_edges[i].caller_id == caller_id && g_edges[i].callee_id == callee_id) {
      return &g_edges[i];
    }
  }
  return NULL;
}

static int mettle_profile_ensure_edge(uint32_t caller_id, uint32_t callee_id,
                                      MettleProfileEdge **edge_out) {
  MettleProfileEdge *edge = mettle_profile_find_edge(caller_id, callee_id);

  if (edge) {
    if (edge_out) {
      *edge_out = edge;
    }
    return 1;
  }

  if (g_edge_count >= g_edge_capacity) {
    size_t new_capacity = g_edge_capacity == 0 ? 32u : g_edge_capacity * 2u;
    MettleProfileEdge *edges =
        realloc(g_edges, new_capacity * sizeof(MettleProfileEdge));
    if (!edges) {
      return 0;
    }
    g_edges = edges;
    g_edge_capacity = new_capacity;
  }

  edge = &g_edges[g_edge_count++];
  edge->caller_id = caller_id;
  edge->callee_id = callee_id;
  edge->call_count = 0;
  edge->total_ns = 0;
  if (edge_out) {
    *edge_out = edge;
  }
  return 1;
}

static void mettle_profile_note_edge_call(uint32_t caller_id,
                                          uint32_t callee_id) {
  MettleProfileEdge *edge = NULL;

  if (!mettle_profile_ensure_edge(caller_id, callee_id, &edge) || !edge) {
    return;
  }
  edge->call_count++;
}

static void mettle_profile_note_edge_time(uint32_t caller_id, uint32_t callee_id,
                                          uint64_t elapsed_ns) {
  MettleProfileEdge *edge = NULL;

  if (!mettle_profile_ensure_edge(caller_id, callee_id, &edge) || !edge) {
    return;
  }
  edge->total_ns += elapsed_ns;
}

void mettle_profile_enter(uint32_t fn_id) {
  uint64_t now = 0;

  if (!mettle_profile_ensure_stats(fn_id) || !mettle_profile_ensure_op_stats(fn_id)) {
    return;
  }

  if (g_stack_depth > 0 && g_stack_depth <= METTLE_PROFILE_MAX_STACK) {
    mettle_profile_note_edge_call(g_stack[g_stack_depth - 1].fn_id, fn_id);
  }

  g_stats[fn_id].call_count++;

  if (g_stack_depth >= METTLE_PROFILE_MAX_STACK) {
    g_stack_overflow_exits++;
    return;
  }

  now = mettle_profile_now_ns();
  g_stack[g_stack_depth].fn_id = fn_id;
  g_stack[g_stack_depth].start_ns = now;
  g_stack[g_stack_depth].child_ns = 0;
  g_stack_depth++;
}

void mettle_profile_op(uint32_t op_class, uint64_t amount) {
  uint32_t fn_id = 0;

  if (g_stack_depth == 0 || op_class >= METTLE_PROFILE_OP_CLASS_COUNT) {
    return;
  }

  fn_id = g_stack[g_stack_depth - 1].fn_id;
  if (!mettle_profile_ensure_op_stats(fn_id)) {
    return;
  }

  g_op_stats[fn_id].counts[op_class] += amount;
}

void mettle_profile_block(uint32_t block_id) {
  size_t needed = (size_t)block_id + 1u;

  if (needed > g_block_capacity) {
    METTLE_PROFILE_ENSURE_CAPACITY(g_block_counts, g_block_capacity,
                                   g_block_capacity, uint64_t, needed);
    if (needed > g_block_capacity) {
      return;
    }
  }

  g_block_counts[block_id]++;
  if (needed > g_block_max_id) {
    g_block_max_id = needed;
  }
}

void mettle_profile_exit(void) {
  uint64_t now = 0;
  uint64_t elapsed = 0;
  MettleProfileFrame frame = {0};
  MettleProfileStats *stats = NULL;
  uint32_t caller_id = UINT32_MAX;

  if (g_stack_depth == 0) {
    return;
  }

  if (g_stack_overflow_exits > 0) {
    g_stack_overflow_exits--;
    return;
  }

  g_stack_depth--;
  frame = g_stack[g_stack_depth];
  if (g_stack_depth > 0) {
    caller_id = g_stack[g_stack_depth - 1].fn_id;
  }

  now = mettle_profile_now_ns();
  elapsed = now - frame.start_ns;

  if (frame.fn_id >= g_stats_capacity) {
    if (!mettle_profile_ensure_stats(frame.fn_id)) {
      return;
    }
  }

  stats = &g_stats[frame.fn_id];
  stats->total_ns += elapsed;
  stats->self_ns += elapsed - frame.child_ns;
  if (elapsed > stats->max_ns) {
    stats->max_ns = elapsed;
  }

  if (caller_id != UINT32_MAX) {
    mettle_profile_note_edge_time(caller_id, frame.fn_id, elapsed);
    g_stack[g_stack_depth - 1].child_ns += elapsed;
  }
}

typedef struct {
  uint32_t fn_id;
  uint64_t total_ns;
} MettleProfileSortEntry;

static int mettle_profile_sort_desc(const void *left, const void *right) {
  const MettleProfileSortEntry *a = (const MettleProfileSortEntry *)left;
  const MettleProfileSortEntry *b = (const MettleProfileSortEntry *)right;

  if (a->total_ns < b->total_ns) {
    return 1;
  }
  if (a->total_ns > b->total_ns) {
    return -1;
  }
  if (a->fn_id < b->fn_id) {
    return -1;
  }
  if (a->fn_id > b->fn_id) {
    return 1;
  }
  return 0;
}

typedef struct {
  uint32_t callee_id;
  uint64_t call_count;
  uint64_t total_ns;
} MettleProfileChildEntry;

static int mettle_profile_child_sort_desc(const void *left, const void *right) {
  const MettleProfileChildEntry *a = (const MettleProfileChildEntry *)left;
  const MettleProfileChildEntry *b = (const MettleProfileChildEntry *)right;

  if (a->total_ns < b->total_ns) {
    return 1;
  }
  if (a->total_ns > b->total_ns) {
    return -1;
  }
  return 0;
}

static const char *mettle_profile_function_name(uint32_t fn_id) {
  if (fn_id < (uint32_t)mettle_profile_name_count && mettle_profile_names &&
      mettle_profile_names[fn_id]) {
    return mettle_profile_names[fn_id];
  }
  return "?";
}

static uint32_t mettle_profile_find_root_id(size_t function_count) {
  uint32_t root_id = 0;
  uint64_t best_total = 0;

  for (size_t i = 0; i < function_count; i++) {
    const char *name = mettle_profile_function_name((uint32_t)i);
    if (name && strcmp(name, "main") == 0 && g_stats[i].call_count > 0) {
      return (uint32_t)i;
    }
  }

  for (size_t i = 0; i < function_count; i++) {
    if (g_stats[i].call_count == 0) {
      continue;
    }
    if (g_stats[i].total_ns >= best_total) {
      best_total = g_stats[i].total_ns;
      root_id = (uint32_t)i;
    }
  }

  return root_id;
}

static void mettle_profile_write_uint64_plain(uint64_t value) {
  char buffer[32];
  uint64_to_decimal(value, buffer, sizeof(buffer));
  mettle_crash_write_stderr(buffer);
}

static void mettle_profile_write_uint64_grouped(uint64_t value) {
  char out[48];
  char digits[32];
  size_t count = 0;
  size_t out_len = 0;

  if (value == 0) {
    mettle_crash_write_stderr("0");
    return;
  }

  while (value > 0 && count < sizeof(digits)) {
    digits[count++] = (char)('0' + (value % 10u));
    value /= 10u;
  }

  for (size_t i = 0; i < count && out_len + 1 < sizeof(out); i++) {
    size_t rev = count - 1 - i;
    out[out_len++] = digits[rev];
    if (rev > 0 && (rev % 3) == 0 && out_len + 1 < sizeof(out)) {
      out[out_len++] = ',';
    }
  }
  out[out_len] = '\0';
  mettle_crash_write_stderr(out);
}

static void mettle_profile_write_indent(size_t depth) {
  for (size_t i = 0; i < depth; i++) {
    mettle_crash_write_stderr("  ");
  }
}

static void mettle_profile_report_callgraph_node(uint32_t fn_id, size_t depth,
                                                 int *visited,
                                                 size_t function_count) {
  MettleProfileChildEntry *children = NULL;
  size_t child_count = 0;

  if (!visited || fn_id >= function_count || visited[fn_id]) {
    return;
  }
  visited[fn_id] = 1;

  for (size_t i = 0; i < g_edge_count; i++) {
    if (g_edges[i].caller_id == fn_id && g_edges[i].call_count > 0) {
      child_count++;
    }
  }

  if (child_count == 0) {
    return;
  }

  children = calloc(child_count, sizeof(MettleProfileChildEntry));
  if (!children) {
    return;
  }

  child_count = 0;
  for (size_t i = 0; i < g_edge_count; i++) {
    if (g_edges[i].caller_id != fn_id || g_edges[i].call_count == 0) {
      continue;
    }
    children[child_count].callee_id = g_edges[i].callee_id;
    children[child_count].call_count = g_edges[i].call_count;
    children[child_count].total_ns = g_edges[i].total_ns;
    child_count++;
  }

  qsort(children, child_count, sizeof(MettleProfileChildEntry),
        mettle_profile_child_sort_desc);

  for (size_t i = 0; i < child_count; i++) {
    uint32_t child_id = children[i].callee_id;
    uint64_t edge_us = children[i].total_ns / NS_PER_US;
    uint64_t calls = children[i].call_count;

    mettle_profile_write_indent(depth);
    mettle_crash_write_stderr(mettle_profile_function_name(child_id));
    mettle_crash_write_stderr("  ");
    mettle_profile_write_uint64_plain(calls);
    mettle_crash_write_stderr(" calls  ");
    mettle_profile_write_uint64_plain(edge_us);
    mettle_crash_write_stderr(" us\n");

    mettle_profile_report_callgraph_node(child_id, depth + 1, visited,
                                         function_count);
  }

  free(children);
}

static void mettle_profile_report_callgraph(size_t function_count) {
  uint32_t root_id = mettle_profile_find_root_id(function_count);
  int *visited = NULL;

  if (!g_stats || function_count == 0) {
    return;
  }

  visited = calloc(function_count, sizeof(int));
  if (!visited) {
    return;
  }

  mettle_crash_write_stderr("\nRuntime profile (call graph):\n");
  mettle_crash_write_stderr(mettle_profile_function_name(root_id));
  mettle_crash_write_stderr("\n");
  mettle_profile_report_callgraph_node(root_id, 1, visited, function_count);
  free(visited);
}

static const char *mettle_profile_op_class_name(uint32_t op_class) {
  static const char *kNames[METTLE_PROFILE_OP_CLASS_COUNT] = {
      "load.u8", "store", "branch", "call", "add.i64", "mul", "div",
      "mod",     "shift", "bitwise", "mem.primitive", "simd", "popcnt"};
  if (op_class >= METTLE_PROFILE_OP_CLASS_COUNT) {
    return "?";
  }
  return kNames[op_class];
}

static void mettle_profile_report_operations(
    const MettleProfileSortEntry *entries, size_t active_count) {
  int has_ops = 0;

  if (!entries || active_count == 0 || !g_op_stats) {
    return;
  }

  for (size_t i = 0; i < active_count && !has_ops; i++) {
    uint32_t fn_id = entries[i].fn_id;
    for (uint32_t op = 0; op < METTLE_PROFILE_OP_CLASS_COUNT; op++) {
      if (g_op_stats[fn_id].counts[op] != 0) {
        has_ops = 1;
        break;
      }
    }
  }

  if (!has_ops) {
    return;
  }

  mettle_crash_write_stderr("\nOperation profile:\n");
  mettle_crash_write_stderr(
      "function             op_class               count\n");

  for (size_t i = 0; i < active_count; i++) {
    uint32_t fn_id = entries[i].fn_id;
    const char *fn_name = mettle_profile_function_name(fn_id);
    for (uint32_t op = 0; op < METTLE_PROFILE_OP_CLASS_COUNT; op++) {
      uint64_t count = g_op_stats[fn_id].counts[op];
      if (count == 0 && op != METTLE_PROFILE_OP_POPCNT &&
          op != METTLE_PROFILE_OP_SIMD) {
        continue;
      }
      mettle_profile_write_padded_field(fn_name, 20);
      mettle_crash_write_stderr(" ");
      mettle_profile_write_padded_field(mettle_profile_op_class_name(op), 22);
      mettle_crash_write_stderr(" ");
      mettle_profile_write_uint64_grouped(count);
      mettle_crash_write_stderr("\n");
    }
  }
}

/* The runtime deliberately avoids libc stdio (the from-scratch internal linker
 * for --build resolves no __mingw_fprintf), so the sidecar is built in a
 * growable byte buffer and written with one OS call. */
typedef struct {
  char *data;
  size_t len;
  size_t cap;
  int failed;
} MProfBuf;

static void mb_putc(MProfBuf *b, char c) {
  if (b->failed) {
    return;
  }
  if (b->len + 1 > b->cap) {
    size_t nc = b->cap ? b->cap * 2 : 8192;
    char *grown = realloc(b->data, nc);
    if (!grown) {
      b->failed = 1;
      return;
    }
    b->data = grown;
    b->cap = nc;
  }
  b->data[b->len++] = c;
}

static void mb_puts(MProfBuf *b, const char *s) {
  if (!s) {
    return;
  }
  while (*s) {
    mb_putc(b, *s++);
  }
}

static void mb_putu(MProfBuf *b, uint64_t value) {
  char tmp[32];
  uint64_to_decimal(value, tmp, sizeof(tmp));
  mb_puts(b, tmp);
}

static void mb_escape(MProfBuf *b, const char *s) {
  if (!s) {
    return;
  }
  for (; *s; s++) {
    unsigned char c = (unsigned char)*s;
    switch (c) {
    case '"':
      mb_puts(b, "\\\"");
      break;
    case '\\':
      mb_puts(b, "\\\\");
      break;
    case '\n':
      mb_puts(b, "\\n");
      break;
    case '\r':
      mb_puts(b, "\\r");
      break;
    case '\t':
      mb_puts(b, "\\t");
      break;
    default:
      if (c < 0x20) {
        static const char hexd[] = "0123456789abcdef";
        mb_puts(b, "\\u00");
        mb_putc(b, hexd[(c >> 4) & 0xF]);
        mb_putc(b, hexd[c & 0xF]);
      } else {
        mb_putc(b, (char)c);
      }
    }
  }
}

static int mprof_write_file(const char *path, const char *data, size_t length) {
#if defined(_WIN32) || defined(_WIN64)
  HANDLE handle = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, NULL);
  size_t written_total = 0;
  if (handle == INVALID_HANDLE_VALUE) {
    return 0;
  }
  while (written_total < length) {
    DWORD chunk = (DWORD)((length - written_total > 0x40000000u)
                              ? 0x40000000u
                              : (length - written_total));
    DWORD written = 0;
    if (!WriteFile(handle, data + written_total, chunk, &written, NULL)) {
      break;
    }
    written_total += written;
  }
  CloseHandle(handle);
  return written_total == length;
#else
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  size_t written_total = 0;
  if (fd < 0) {
    return 0;
  }
  while (written_total < length) {
    ssize_t written = write(fd, data + written_total, length - written_total);
    if (written <= 0) {
      break;
    }
    written_total += (size_t)written;
  }
  close(fd);
  return written_total == length;
#endif
}

/* Machine-readable companion to the stderr report: the measured side of the
 * VTune-style codegen view. The extension joins blocks[] with the static
 * <stem>.annot.json (per-instruction block ids + port cost) to attribute
 * real execution frequency onto the microarchitecture model. */
static void mettle_profile_write_sidecar(void) {
  const char *path = getenv("METTLE_PROFILE_OUT");
  size_t function_count = (size_t)mettle_profile_name_count;
  MProfBuf b = {0};
  int first = 1;

  if (!path || !path[0]) {
    path = "mettle.mprof";
  }

  mb_puts(&b, "{\"version\":1,\"functions\":[");
  for (size_t i = 0; i < function_count; i++) {
    MettleProfileStats *stats = (i < g_stats_capacity) ? &g_stats[i] : NULL;
    const char *fname = NULL;

    if (!stats || stats->call_count == 0) {
      continue;
    }
    fname = (mettle_profile_files && mettle_profile_files[i])
                ? mettle_profile_files[i]
                : "?";

    if (!first) {
      mb_putc(&b, ',');
    }
    first = 0;
    mb_puts(&b, "{\"id\":");
    mb_putu(&b, (uint64_t)i);
    mb_puts(&b, ",\"name\":\"");
    mb_escape(&b, mettle_profile_function_name((uint32_t)i));
    mb_puts(&b, "\",\"file\":\"");
    mb_escape(&b, fname);
    mb_puts(&b, "\",\"line\":");
    mb_putu(&b, mettle_profile_lines ? mettle_profile_lines[i] : 0);
    mb_puts(&b, ",\"calls\":");
    mb_putu(&b, stats->call_count);
    mb_puts(&b, ",\"total_ns\":");
    mb_putu(&b, stats->total_ns);
    mb_puts(&b, ",\"self_ns\":");
    mb_putu(&b, stats->self_ns);
    mb_puts(&b, ",\"max_ns\":");
    mb_putu(&b, stats->max_ns);
    mb_putc(&b, '}');
  }

  mb_puts(&b, "],\"edges\":[");
  for (size_t i = 0; i < g_edge_count; i++) {
    if (i) {
      mb_putc(&b, ',');
    }
    mb_puts(&b, "{\"caller\":");
    mb_putu(&b, g_edges[i].caller_id);
    mb_puts(&b, ",\"callee\":");
    mb_putu(&b, g_edges[i].callee_id);
    mb_puts(&b, ",\"calls\":");
    mb_putu(&b, g_edges[i].call_count);
    mb_puts(&b, ",\"total_ns\":");
    mb_putu(&b, g_edges[i].total_ns);
    mb_putc(&b, '}');
  }

  mb_puts(&b, "],\"ops\":[");
  first = 1;
  for (size_t i = 0; g_op_stats && i < function_count && i < g_op_stats_capacity;
       i++) {
    int any = 0;
    for (uint32_t op = 0; op < METTLE_PROFILE_OP_CLASS_COUNT; op++) {
      if (g_op_stats[i].counts[op]) {
        any = 1;
        break;
      }
    }
    if (!any) {
      continue;
    }
    if (!first) {
      mb_putc(&b, ',');
    }
    first = 0;
    mb_puts(&b, "{\"fn\":");
    mb_putu(&b, (uint64_t)i);
    mb_puts(&b, ",\"counts\":[");
    for (uint32_t op = 0; op < METTLE_PROFILE_OP_CLASS_COUNT; op++) {
      if (op) {
        mb_putc(&b, ',');
      }
      mb_putu(&b, g_op_stats[i].counts[op]);
    }
    mb_puts(&b, "]}");
  }

  mb_puts(&b, "],\"blocks\":[");
  for (size_t i = 0; i < g_block_max_id; i++) {
    if (i) {
      mb_putc(&b, ',');
    }
    mb_putu(&b, i < g_block_capacity ? g_block_counts[i] : 0);
  }
  mb_puts(&b, "]}\n");

  if (!b.failed && b.data) {
    mprof_write_file(path, b.data, b.len);
  }
  free(b.data);
}

void mettle_profile_report(void) {
  size_t function_count = 0;
  size_t active_count = 0;
  MettleProfileSortEntry *entries = NULL;
  uint64_t root_total_ns = 0;
  size_t i = 0;

  mettle_profile_write_sidecar();

  function_count = (size_t)mettle_profile_name_count;
  if (function_count == 0 || !g_stats) {
    return;
  }

  entries = calloc(function_count, sizeof(MettleProfileSortEntry));
  if (!entries) {
    return;
  }

  for (i = 0; i < function_count; i++) {
    if (g_stats[i].call_count == 0) {
      continue;
    }
    entries[active_count].fn_id = (uint32_t)i;
    entries[active_count].total_ns = g_stats[i].total_ns;
    if (g_stats[i].total_ns > root_total_ns) {
      root_total_ns = g_stats[i].total_ns;
    }
    active_count++;
  }

  if (active_count == 0 || root_total_ns == 0) {
    free(entries);
    return;
  }

  qsort(entries, active_count, sizeof(MettleProfileSortEntry),
        mettle_profile_sort_desc);

  mettle_crash_write_stderr("\nRuntime profile:\n");
  mettle_crash_write_stderr(
      "function             location                             calls    "
      "total_us    avg_ns    self_us     pct\n");

  for (i = 0; i < active_count; i++) {
    uint32_t fn_id = entries[i].fn_id;
    MettleProfileStats *stats = &g_stats[fn_id];
    const char *name = mettle_profile_function_name(fn_id);
    uint64_t total_us = stats->total_ns / NS_PER_US;
    uint64_t self_us = stats->self_ns / NS_PER_US;
    uint64_t avg_ns =
        stats->call_count == 0 ? 0 : stats->total_ns / stats->call_count;
    double pct = 100.0 * ((double)stats->total_ns / (double)root_total_ns);

    mettle_profile_write_padded_field(name, 20);
    mettle_crash_write_stderr(" ");
    mettle_profile_write_location(fn_id, 36);
    mettle_crash_write_stderr(" ");
    mettle_profile_write_padded_uint64(stats->call_count, 8);
    mettle_crash_write_stderr("  ");
    mettle_profile_write_padded_uint64(total_us, 8);
    mettle_crash_write_stderr("  ");
    mettle_profile_write_padded_uint64(avg_ns, 8);
    mettle_crash_write_stderr("  ");
    mettle_profile_write_padded_uint64(self_us, 8);
    mettle_crash_write_stderr("  ");
    mettle_profile_write_pct(pct);
    mettle_crash_write_stderr("\n");
  }

  mettle_profile_report_callgraph(function_count);
  mettle_profile_report_operations(entries, active_count);
  free(entries);
}
