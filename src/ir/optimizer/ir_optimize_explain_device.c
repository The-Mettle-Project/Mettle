#include "ir_optimize_internal.h"
#include "ir_optimize_explain_internal.h"
#include "common.h"
#include "../ir_explain_device.h"
#include "../../error/diag_style.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IR_DEVICE_MAX_QUANTITIES 4
#define IR_DEVICE_CODE_LIMIT 60000

typedef struct {
  char *name;
  long long value;
} IRDeviceQuantity;

typedef struct {
  size_t function;
  size_t line;
  size_t order;
  size_t sites;
  char *kind;
  char *subject;
  IRDeviceVerdict verdict;
  char *headline;
  char *reason;
  char *fix;
  char *code;
  IRDeviceQuantity quantities[IR_DEVICE_MAX_QUANTITIES];
  size_t quantity_count;
} IRDeviceNote;

typedef struct {
  size_t function;
  size_t line;
  char *text;
} IRDeviceCodeLine;

typedef struct {
  char *name;
  size_t line;
  int is_kernel;
  int block[3];
  long long counters[IR_DEVICE_COUNTER_COUNT];
  unsigned char known[IR_DEVICE_COUNTER_COUNT];
  char *limiter;
} IRDeviceFunction;

typedef struct {
  int begun;
  char *target;
  char *profile;
  char *measured_by;
  IRDeviceFunction *functions;
  size_t function_count;
  size_t function_capacity;
  IRDeviceNote *notes;
  size_t note_count;
  size_t note_capacity;
  int last_note_recorded;
  IRDeviceCodeLine *code;
  size_t code_count;
  size_t code_capacity;
  size_t code_total;
} IRDeviceStore;

static MTLC_THREAD_LOCAL IRDeviceStore g_device;

static const char *const IR_DEVICE_COUNTER_NAMES[IR_DEVICE_COUNTER_COUNT] = {
    "valuesInRegisters", "valuesInMemory",   "memoryBytes",
    "workgroupBytes",    "workgroupAtLaunch", "accessesNamed",
    "accessesGeneric",   "vectorGroups",     "loadsSaved",
    "callsKept",         "registers",        "spillStoreBytes",
    "spillLoadBytes",    "stackFrame",       "sharedBytes",
    "warps",             "warpLimit",        "threadWidth",
    "maxThreads",        "valuesPromoted"};

static char *device_dup(const char *text) {
  size_t length;
  char *copy;
  if (!text) {
    return NULL;
  }
  length = strlen(text) + 1;
  copy = malloc(length);
  if (copy) {
    memcpy(copy, text, length);
  }
  return copy;
}

static void device_note_free(IRDeviceNote *note) {
  free(note->kind);
  free(note->subject);
  free(note->headline);
  free(note->reason);
  free(note->fix);
  free(note->code);
  for (size_t q = 0; q < note->quantity_count; q++) {
    free(note->quantities[q].name);
  }
}

static void device_release(void) {
  for (size_t i = 0; i < g_device.function_count; i++) {
    free(g_device.functions[i].name);
    free(g_device.functions[i].limiter);
  }
  for (size_t i = 0; i < g_device.note_count; i++) {
    device_note_free(&g_device.notes[i]);
  }
  for (size_t i = 0; i < g_device.code_count; i++) {
    free(g_device.code[i].text);
  }
  free(g_device.functions);
  free(g_device.notes);
  free(g_device.code);
  free(g_device.target);
  free(g_device.profile);
  free(g_device.measured_by);
  memset(&g_device, 0, sizeof(g_device));
}

int ir_explain_device_enabled(void) {
  return ir_explain_enabled() && g_device.begun;
}

int ir_explain_device_has_report(void) { return g_device.begun; }

void ir_explain_device_begin(const char *target, const char *profile) {
  device_release();
  if (!ir_explain_enabled()) {
    return;
  }
  g_device.begun = 1;
  g_device.target = device_dup(target ? target : "device");
  g_device.profile = device_dup(profile ? profile : "");
}

static IRDeviceFunction *device_find(const char *function, size_t *index) {
  if (!g_device.begun || !function) {
    return NULL;
  }
  for (size_t i = 0; i < g_device.function_count; i++) {
    if (strcmp(g_device.functions[i].name, function) == 0) {
      if (index) {
        *index = i;
      }
      return &g_device.functions[i];
    }
  }
  return NULL;
}

int ir_explain_device_function(const char *function, const char *filename,
                               size_t line, int is_kernel, const int *block) {
  IRDeviceFunction *entry;
  if (!ir_explain_device_enabled() || !function ||
      !ir_explain_file_enabled(filename)) {
    return 0;
  }
  if (device_find(function, NULL)) {
    return 1;
  }
  if (g_device.function_count == g_device.function_capacity) {
    size_t capacity =
        g_device.function_capacity ? g_device.function_capacity * 2 : 8;
    IRDeviceFunction *grown =
        realloc(g_device.functions, capacity * sizeof(*grown));
    if (!grown) {
      return 0;
    }
    g_device.functions = grown;
    g_device.function_capacity = capacity;
  }
  entry = &g_device.functions[g_device.function_count++];
  memset(entry, 0, sizeof(*entry));
  entry->name = device_dup(function);
  entry->line = line;
  entry->is_kernel = is_kernel;
  for (int i = 0; i < 3; i++) {
    entry->block[i] = block ? block[i] : 0;
  }
  return 1;
}

size_t ir_explain_device_function_count(void) {
  return g_device.function_count;
}

const char *ir_explain_device_function_name(size_t index, int *is_kernel) {
  if (index >= g_device.function_count) {
    return NULL;
  }
  if (is_kernel) {
    *is_kernel = g_device.functions[index].is_kernel;
  }
  return g_device.functions[index].name;
}

static int device_same(const char *a, const char *b) {
  return (!a && !b) || (a && b && strcmp(a, b) == 0);
}

static IRDeviceNote *device_note_seen(size_t function, size_t line,
                                      int any_line, const char *kind,
                                      const char *subject,
                                      const char *headline) {
  char *shown_subject = ir_explain_text_dup(subject);
  char *shown_headline = ir_explain_text_dup(headline);
  IRDeviceNote *found = NULL;
  for (size_t i = 0; i < g_device.note_count && !found; i++) {
    IRDeviceNote *seen = &g_device.notes[i];
    if (seen->function == function && (any_line || seen->line == line) &&
        device_same(seen->kind, kind) &&
        device_same(seen->subject, shown_subject) &&
        device_same(seen->headline, shown_headline)) {
      found = seen;
    }
  }
  free(shown_subject);
  free(shown_headline);
  return found;
}

static void device_note_add(const char *function, size_t line, int merge_sites,
                            const char *kind, const char *subject,
                            IRDeviceVerdict verdict, const char *headline,
                            const char *reason, const char *fix,
                            const char *code) {
  size_t index = 0;
  IRDeviceNote *note;
  g_device.last_note_recorded = 0;
  if (!headline || !device_find(function, &index)) {
    return;
  }
  note = device_note_seen(index, line, merge_sites, kind ? kind : "other",
                          subject, headline);
  if (note) {
    note->sites += merge_sites ? 1 : 0;
    return;
  }
  if (g_device.note_count == g_device.note_capacity) {
    size_t capacity = g_device.note_capacity ? g_device.note_capacity * 2 : 16;
    IRDeviceNote *grown = realloc(g_device.notes, capacity * sizeof(*grown));
    if (!grown) {
      return;
    }
    g_device.notes = grown;
    g_device.note_capacity = capacity;
  }
  note = &g_device.notes[g_device.note_count];
  memset(note, 0, sizeof(*note));
  note->function = index;
  note->line = line;
  note->order = g_device.note_count++;
  note->kind = device_dup(kind ? kind : "other");
  note->subject = ir_explain_text_dup(subject);
  note->verdict = verdict;
  note->headline = ir_explain_text_dup(headline);
  note->reason = ir_explain_text_dup(reason);
  note->fix = ir_explain_text_dup(fix);
  note->code = device_dup(code);
  note->sites = 1;
  g_device.last_note_recorded = 1;
}

void ir_explain_device_note(const char *function, size_t line,
                            const char *kind, const char *subject,
                            IRDeviceVerdict verdict, const char *headline,
                            const char *reason, const char *fix,
                            const char *code) {
  device_note_add(function, line, 0, kind, subject, verdict, headline, reason,
                  fix, code);
}

void ir_explain_device_note_site(const char *function, size_t line,
                                 const char *kind, const char *subject,
                                 IRDeviceVerdict verdict, const char *headline,
                                 const char *reason, const char *fix,
                                 const char *code) {
  device_note_add(function, line, 1, kind, subject, verdict, headline, reason,
                  fix, code);
}

void ir_explain_device_note_quantity(const char *name, long long value) {
  IRDeviceNote *note;
  if (!g_device.last_note_recorded || g_device.note_count == 0 || !name) {
    return;
  }
  note = &g_device.notes[g_device.note_count - 1];
  if (note->quantity_count >= IR_DEVICE_MAX_QUANTITIES) {
    return;
  }
  note->quantities[note->quantity_count].name = device_dup(name);
  note->quantities[note->quantity_count].value = value;
  note->quantity_count++;
}

void ir_explain_device_add(const char *function, IRDeviceCounter counter,
                           long long amount) {
  IRDeviceFunction *entry = device_find(function, NULL);
  if (!entry || counter < 0 || counter >= IR_DEVICE_COUNTER_COUNT) {
    return;
  }
  entry->counters[counter] += amount;
  entry->known[counter] = 1;
}

void ir_explain_device_set(const char *function, IRDeviceCounter counter,
                           long long value) {
  IRDeviceFunction *entry = device_find(function, NULL);
  if (!entry || counter < 0 || counter >= IR_DEVICE_COUNTER_COUNT) {
    return;
  }
  entry->counters[counter] = value;
  entry->known[counter] = 1;
}

static long long device_note_bytes(const IRDeviceNote *note) {
  for (size_t q = 0; q < note->quantity_count; q++) {
    if (strcmp(note->quantities[q].name, "bytes") == 0) {
      return note->quantities[q].value;
    }
  }
  return 0;
}

static void device_promote_storage(IRDeviceFunction *entry, size_t index) {
  size_t kept = 0;
  long long promoted = entry->counters[IR_DEVICE_VALUES_IN_MEMORY];
  entry->counters[IR_DEVICE_VALUES_PROMOTED] = promoted;
  entry->known[IR_DEVICE_VALUES_PROMOTED] = promoted > 0;
  entry->counters[IR_DEVICE_VALUES_IN_REGISTERS] += promoted;
  entry->known[IR_DEVICE_VALUES_IN_REGISTERS] = 1;
  entry->counters[IR_DEVICE_VALUES_IN_MEMORY] = 0;
  entry->counters[IR_DEVICE_MEMORY_BYTES] = 0;
  for (size_t i = 0; i < g_device.note_count; i++) {
    IRDeviceNote *note = &g_device.notes[i];
    if (note->function == index && strcmp(note->kind, "storage") == 0) {
      device_note_free(note);
      continue;
    }
    g_device.notes[kept++] = *note;
  }
  g_device.note_count = kept;
  g_device.last_note_recorded = 0;
}

void ir_explain_device_resolve_storage(const char *function) {
  size_t index = 0;
  IRDeviceFunction *entry = device_find(function, &index);
  long long frame;
  long long emitted;
  if (!entry || !entry->known[IR_DEVICE_MEASURED_STACK_FRAME] ||
      !entry->known[IR_DEVICE_MEMORY_BYTES]) {
    return;
  }
  frame = entry->counters[IR_DEVICE_MEASURED_STACK_FRAME];
  emitted = entry->counters[IR_DEVICE_MEMORY_BYTES];
  if (frame == 0) {
    device_promote_storage(entry, index);
    return;
  }
  if (frame < emitted) {
    return;
  }
  for (size_t i = 0; i < g_device.note_count; i++) {
    IRDeviceNote *note = &g_device.notes[i];
    char headline[160];
    if (note->function != index || strcmp(note->kind, "storage") != 0) {
      continue;
    }
    snprintf(headline, sizeof(headline), "in local memory, %lld bytes",
             device_note_bytes(note));
    free(note->headline);
    note->headline = device_dup(headline);
  }
}

void ir_explain_device_limiter(const char *function, const char *limiter) {
  IRDeviceFunction *entry = device_find(function, NULL);
  if (!entry) {
    return;
  }
  free(entry->limiter);
  entry->limiter = device_dup(limiter);
}

void ir_explain_device_measured_by(const char *tool) {
  if (!g_device.begun) {
    return;
  }
  free(g_device.measured_by);
  g_device.measured_by = device_dup(tool);
}

void ir_explain_device_code(const char *function, size_t line,
                            const char *text) {
  size_t index = 0;
  if (!text || !device_find(function, &index)) {
    return;
  }
  g_device.code_total++;
  if (g_device.code_count >= IR_DEVICE_CODE_LIMIT) {
    return;
  }
  if (g_device.code_count == g_device.code_capacity) {
    size_t capacity = g_device.code_capacity ? g_device.code_capacity * 2 : 256;
    IRDeviceCodeLine *grown =
        realloc(g_device.code, capacity * sizeof(*grown));
    if (!grown) {
      return;
    }
    g_device.code = grown;
    g_device.code_capacity = capacity;
  }
  g_device.code[g_device.code_count].function = index;
  g_device.code[g_device.code_count].line = line;
  g_device.code[g_device.code_count].text = device_dup(text);
  g_device.code_count++;
}

static int device_note_compare(const void *a, const void *b) {
  const IRDeviceNote *left = a;
  const IRDeviceNote *right = b;
  if (left->function != right->function) {
    return left->function < right->function ? -1 : 1;
  }
  if (left->line != right->line) {
    return left->line < right->line ? -1 : 1;
  }
  return left->order < right->order ? -1 : left->order > right->order;
}

static int device_is_metal(void) {
  return g_device.target && strcmp(g_device.target, "metal") == 0;
}

static void device_target_label(char *out, size_t size) {
  if (device_is_metal()) {
    snprintf(out, size, "Metal %s", g_device.profile);
  } else if (g_device.target && strcmp(g_device.target, "ptx") == 0) {
    snprintf(out, size, "PTX %s", g_device.profile);
  } else {
    snprintf(out, size, "%s %s", g_device.target ? g_device.target : "device",
             g_device.profile ? g_device.profile : "");
  }
}

#define IR_DEVICE_ROWS 8
#define IR_DEVICE_ROW_SIZE 440

static MTLC_THREAD_LOCAL char g_device_rows[IR_DEVICE_ROWS][IR_DEVICE_ROW_SIZE];
static MTLC_THREAD_LOCAL size_t g_device_row_count = 0;

static void device_row(const char *text) {
  if (g_device_row_count < IR_DEVICE_ROWS) {
    snprintf(g_device_rows[g_device_row_count++], IR_DEVICE_ROW_SIZE, "%s",
             text);
  }
}

static long long device_value(const IRDeviceFunction *entry,
                              IRDeviceCounter counter) {
  return entry->known[counter] ? entry->counters[counter] : 0;
}

static void device_values_row(const IRDeviceFunction *entry) {
  char row[256];
  long long memory = device_value(entry, IR_DEVICE_VALUES_IN_MEMORY);
  long long bytes = device_value(entry, IR_DEVICE_MEMORY_BYTES);
  long long frame = device_value(entry, IR_DEVICE_MEASURED_STACK_FRAME);
  int measured = entry->known[IR_DEVICE_MEASURED_STACK_FRAME];
  const char *where = measured && frame >= bytes ? "in local memory"
                                                 : "emitted as local memory";
  if (memory == 0) {
    return;
  }
  if (device_is_metal()) {
    snprintf(row, sizeof(row),
             "values: %lld %s kept whole (%lld bytes)", memory,
             memory == 1 ? "array or record" : "arrays or records", bytes);
  } else {
    snprintf(row, sizeof(row), "values: %lld %s (%lld bytes)", memory, where,
             bytes);
  }
  device_row(row);
}

static void device_memory_row(const IRDeviceFunction *entry) {
  char row[320];
  size_t used;
  long long named = device_value(entry, IR_DEVICE_ACCESSES_NAMED);
  long long generic = device_value(entry, IR_DEVICE_ACCESSES_GENERIC);
  long long groups = device_value(entry, IR_DEVICE_VECTOR_GROUPS);
  long long saved = device_value(entry, IR_DEVICE_LOADS_SAVED);
  long long total = named + generic;
  if (generic == 0 && groups == 0) {
    return;
  }
  used = (size_t)snprintf(row, sizeof(row), "memory: ");
  if (generic > 0) {
    used += (size_t)snprintf(row + used, sizeof(row) - used,
                             "%lld of %lld %s through the generic path%s",
                             generic, total,
                             total == 1 ? "access" : "accesses",
                             groups > 0 ? ", " : "");
  }
  if (groups > 0 && used < sizeof(row)) {
    snprintf(row + used, sizeof(row) - used,
             "%lld vector %s in place of %lld separate loads", groups,
             groups == 1 ? "load" : "loads", groups + saved);
  }
  device_row(row);
}

static void device_workgroup_row(const IRDeviceFunction *entry) {
  char row[200];
  long long bytes = device_value(entry, IR_DEVICE_WORKGROUP_BYTES);
  long long at_launch = device_value(entry, IR_DEVICE_WORKGROUP_LAUNCH_SIZED);
  long long measured = device_value(entry, IR_DEVICE_MEASURED_SHARED_BYTES);
  if (at_launch == 0 && (bytes == 0 || bytes == measured)) {
    return;
  }
  if (bytes > 0 && at_launch > 0) {
    snprintf(row, sizeof(row),
             "workgroup memory: %lld bytes a block, and an arena sized at "
             "launch",
             bytes);
  } else if (bytes > 0) {
    snprintf(row, sizeof(row), "workgroup memory: %lld bytes a block", bytes);
  } else {
    snprintf(row, sizeof(row), "workgroup memory: an arena sized at launch");
  }
  device_row(row);
}

static void device_calls_row(const IRDeviceFunction *entry) {
  char row[120];
  long long kept = device_value(entry, IR_DEVICE_CALLS_KEPT);
  if (kept == 0) {
    return;
  }
  snprintf(row, sizeof(row), "calls: %lld kept as %s", kept,
           kept == 1 ? "a device call" : "device calls");
  device_row(row);
}

static void device_measured_ptx(const IRDeviceFunction *entry, char *row,
                                size_t size) {
  size_t used;
  long long stores = device_value(entry, IR_DEVICE_MEASURED_SPILL_STORES);
  long long loads = device_value(entry, IR_DEVICE_MEASURED_SPILL_LOADS);
  long long frame = device_value(entry, IR_DEVICE_MEASURED_STACK_FRAME);
  long long shared = device_value(entry, IR_DEVICE_MEASURED_SHARED_BYTES);
  used = (size_t)snprintf(row, size, "%s: %lld registers a thread",
                          g_device.measured_by ? g_device.measured_by : "ptxas",
                          device_value(entry, IR_DEVICE_MEASURED_REGISTERS));
  if (stores > 0 || loads > 0) {
    used += (size_t)snprintf(row + used, used < size ? size - used : 0,
                             ", spills of %lld bytes stored and %lld loaded",
                             stores, loads);
  }
  if (frame > 0) {
    used += (size_t)snprintf(row + used, used < size ? size - used : 0,
                             ", a %lld byte stack frame", frame);
  }
  if (shared > 0 && used < size) {
    snprintf(row + used, size - used, ", %lld bytes of shared memory a block",
             shared);
  }
}

static void device_occupancy_row(const IRDeviceFunction *entry) {
  char row[200];
  long long warps = device_value(entry, IR_DEVICE_MEASURED_WARPS);
  long long limit = device_value(entry, IR_DEVICE_MEASURED_WARP_LIMIT);
  if (!entry->known[IR_DEVICE_MEASURED_WARPS]) {
    return;
  }
  if (warps >= limit) {
    snprintf(row, sizeof(row), "occupancy: all %lld warps fit", limit);
  } else {
    snprintf(row, sizeof(row), "occupancy: %lld of %lld warps fit%s%s", warps,
             limit, entry->limiter ? ", limited by " : "",
             entry->limiter ? entry->limiter : "");
  }
  device_row(row);
}

static void device_measured_metal(const IRDeviceFunction *entry, char *row,
                                  size_t size) {
  size_t used;
  long long shared = device_value(entry, IR_DEVICE_MEASURED_SHARED_BYTES);
  used = (size_t)snprintf(
      row, size, "%s: %lld threads a SIMD group, up to %lld threads a group",
      g_device.measured_by ? g_device.measured_by : "Metal",
      device_value(entry, IR_DEVICE_MEASURED_THREAD_WIDTH),
      device_value(entry, IR_DEVICE_MEASURED_MAX_THREADS));
  if (shared > 0 && used < size) {
    snprintf(row + used, size - used,
             ", %lld bytes of static threadgroup memory", shared);
  }
}

static void device_measured_row(const IRDeviceFunction *entry) {
  char row[420];
  if (entry->known[IR_DEVICE_MEASURED_REGISTERS]) {
    device_measured_ptx(entry, row, sizeof(row));
  } else if (entry->known[IR_DEVICE_MEASURED_THREAD_WIDTH]) {
    device_measured_metal(entry, row, sizeof(row));
  } else {
    return;
  }
  device_row(row);
  device_occupancy_row(entry);
}

static size_t device_collect_rows(const IRDeviceFunction *entry) {
  size_t counted;
  g_device_row_count = 0;
  device_values_row(entry);
  device_memory_row(entry);
  device_workgroup_row(entry);
  device_calls_row(entry);
  counted = g_device_row_count;
  device_measured_row(entry);
  return counted;
}

static void device_join_rows(size_t from, size_t to, int labeled,
                             char *out, size_t size) {
  size_t used = 0;
  out[0] = '\0';
  for (size_t i = from; i < to && used < size; i++) {
    const char *colon = labeled ? NULL : strstr(g_device_rows[i], ": ");
    used += (size_t)snprintf(out + used, size - used, "%s%s",
                             i == from ? "" : labeled ? "; " : ", ",
                             colon ? colon + 2 : g_device_rows[i]);
  }
}

static void device_function_head(const IRDeviceFunction *entry) {
  char block[64] = "";
  char measured[1024];
  char counted[1024];
  size_t plain = device_collect_rows(entry);
  if (entry->is_kernel && entry->block[0] > 0) {
    size_t used = (size_t)snprintf(block, sizeof(block), ", block %d",
                                   entry->block[0]);
    int dimensions = entry->block[2] > 1 ? 3 : entry->block[1] > 1 ? 2 : 1;
    for (int i = 1; i < dimensions; i++) {
      used += (size_t)snprintf(block + used,
                               used < sizeof(block) ? sizeof(block) - used : 0,
                               "x%d", entry->block[i]);
    }
  }
  device_join_rows(plain, g_device_row_count, 0, measured, sizeof(measured));
  device_join_rows(0, plain, 1, counted, sizeof(counted));
  ir_explain_emit("  %s%s%s (%s @ line %zu%s)%s%s\n",
                  ir_explain_style(IR_EXPLAIN_STYLE_BOLD), entry->name,
                  ir_explain_style(IR_EXPLAIN_STYLE_RESET),
                  entry->is_kernel ? "kernel" : "device function", entry->line,
                  block, measured[0] ? ": " : "", measured);
  if (counted[0]) {
    ir_explain_emit("      %s%s %s%s\n", ir_explain_style(IR_EXPLAIN_STYLE_DIM),
                    diag_glyphs()->elbow, counted,
                    ir_explain_style(IR_EXPLAIN_STYLE_RESET));
  }
}

static IRExplainStyle device_verdict_style(IRDeviceVerdict verdict) {
  switch (verdict) {
  case IR_DEVICE_GOOD:
    return IR_EXPLAIN_STYLE_GREEN;
  case IR_DEVICE_MISS:
  case IR_DEVICE_REFUSED:
    return IR_EXPLAIN_STYLE_RED;
  default:
    return IR_EXPLAIN_STYLE_RESET;
  }
}

static const char *device_verdict_name(IRDeviceVerdict verdict) {
  switch (verdict) {
  case IR_DEVICE_GOOD:
    return "good";
  case IR_DEVICE_MISS:
    return "miss";
  case IR_DEVICE_REFUSED:
    return "refused";
  default:
    return "fact";
  }
}

static void device_note_text(const IRDeviceNote *note) {
  const IRDeviceFunction *entry = &g_device.functions[note->function];
  char tag[96] = "";
  char sites[48] = "";
  if (note->code && (note->verdict == IR_DEVICE_MISS ||
                     note->verdict == IR_DEVICE_REFUSED)) {
    snprintf(tag, sizeof(tag), "  %s[%s]%s",
             ir_explain_style(IR_EXPLAIN_STYLE_DIM), note->code,
             ir_explain_style(IR_EXPLAIN_STYLE_RESET));
  }
  if (note->sites > 1) {
    snprintf(sites, sizeof(sites), ", %zu sites", note->sites);
  }
  ir_explain_emit("  %s%s%s (%s @ line %zu%s): %s%s%s%s\n",
                  ir_explain_style(IR_EXPLAIN_STYLE_BOLD), entry->name,
                  ir_explain_style(IR_EXPLAIN_STYLE_RESET),
                  note->subject ? note->subject : note->kind, note->line,
                  sites,
                  ir_explain_style(device_verdict_style(note->verdict)),
                  note->headline, ir_explain_style(IR_EXPLAIN_STYLE_RESET),
                  tag);
  if (note->verdict == IR_DEVICE_MISS || note->verdict == IR_DEVICE_REFUSED) {
    ir_explain_echo_source_range(note->line, 0);
  }
  if (note->reason) {
    ir_explain_emit("      %s%s reason: %s%s\n",
                    ir_explain_style(IR_EXPLAIN_STYLE_DIM),
                    diag_glyphs()->elbow, note->reason,
                    ir_explain_style(IR_EXPLAIN_STYLE_RESET));
  }
  if (note->fix) {
    ir_explain_emit("      %s%s fix: %s%s\n",
                    ir_explain_style(IR_EXPLAIN_STYLE_DIM),
                    diag_glyphs()->elbow, note->fix,
                    ir_explain_style(IR_EXPLAIN_STYLE_RESET));
  }
}

static int device_function_quiet(size_t function) {
  for (size_t n = 0; n < g_device.note_count; n++) {
    if (g_device.notes[n].function == function) {
      return 0;
    }
  }
  device_collect_rows(&g_device.functions[function]);
  return g_device_row_count == 0;
}

static void device_quiet_line(size_t count) {
  char names[512];
  size_t used = 0;
  size_t shown = 0;
  if (count == 0) {
    return;
  }
  names[0] = '\0';
  for (size_t f = 0; f < g_device.function_count && shown < 8; f++) {
    if (device_function_quiet(f)) {
      used += (size_t)snprintf(names + used,
                               used < sizeof(names) ? sizeof(names) - used : 0,
                               "%s%s", shown ? ", " : "",
                               g_device.functions[f].name);
      shown++;
    }
  }
  if (shown < count && used < sizeof(names)) {
    snprintf(names + used, sizeof(names) - used, " and %zu more",
             count - shown);
  }
  ir_explain_emit("  %snothing to report for %s%s\n",
                  ir_explain_style(IR_EXPLAIN_STYLE_DIM), names,
                  ir_explain_style(IR_EXPLAIN_STYLE_RESET));
}

static void device_text(void) {
  char label[160];
  char target[96];
  size_t next = 0;
  size_t quiet = 0;
  device_target_label(target, sizeof(target));
  snprintf(label, sizeof(label), "device report, %s", target);
  ir_explain_print_header(label);
  if (g_device.function_count == 0) {
    ir_explain_emit("  (no kernels in this file)\n\n");
    return;
  }
  if (g_device.measured_by) {
    ir_explain_emit("  %smeasured by %s%s%s\n",
                    ir_explain_style(IR_EXPLAIN_STYLE_DIM),
                    g_device.measured_by,
                    device_is_metal()
                        ? ""
                        : "; warps are the most that fit on one multiprocessor",
                    ir_explain_style(IR_EXPLAIN_STYLE_RESET));
  }
  for (size_t f = 0; f < g_device.function_count; f++) {
    if (device_function_quiet(f)) {
      quiet++;
      continue;
    }
    device_function_head(&g_device.functions[f]);
    while (next < g_device.note_count && g_device.notes[next].function == f) {
      device_note_text(&g_device.notes[next++]);
    }
  }
  device_quiet_line(quiet);
  ir_explain_emit("\n");
}

static void device_function_json(size_t index) {
  const IRDeviceFunction *entry = &g_device.functions[index];
  size_t emitted = 0;
  ir_explain_json_raw("%s{\"fn\":", index ? "," : "");
  ir_explain_json_str(entry->name);
  ir_explain_json_raw(",\"line\":%zu,\"kernel\":%s,\"block\":[%d,%d,%d]",
                      entry->line, entry->is_kernel ? "true" : "false",
                      entry->block[0], entry->block[1], entry->block[2]);
  ir_explain_json_raw(",\"counters\":{");
  for (int c = 0; c < IR_DEVICE_COUNTER_COUNT; c++) {
    if (!entry->known[c]) {
      continue;
    }
    ir_explain_json_raw("%s\"%s\":%lld", emitted++ ? "," : "",
                        IR_DEVICE_COUNTER_NAMES[c], entry->counters[c]);
  }
  ir_explain_json_raw("},\"limiter\":");
  ir_explain_json_str(entry->limiter);
  ir_explain_json_raw(",\"rows\":[");
  device_collect_rows(entry);
  for (size_t i = 0; i < g_device_row_count; i++) {
    ir_explain_json_raw("%s", i ? "," : "");
    ir_explain_json_str(g_device_rows[i]);
  }
  ir_explain_json_raw("],\"code\":[");
  emitted = 0;
  for (size_t i = 0; i < g_device.code_count; i++) {
    if (g_device.code[i].function != index) {
      continue;
    }
    ir_explain_json_raw("%s[%zu,", emitted++ ? "," : "",
                        g_device.code[i].line);
    ir_explain_json_str(g_device.code[i].text);
    ir_explain_json_raw("]");
  }
  ir_explain_json_raw("]}");
}

static void device_note_json(size_t index) {
  const IRDeviceNote *note = &g_device.notes[index];
  ir_explain_json_raw("%s{\"fn\":", index ? "," : "");
  ir_explain_json_str(g_device.functions[note->function].name);
  ir_explain_json_raw(",\"line\":%zu,\"kind\":", note->line);
  ir_explain_json_str(note->kind);
  ir_explain_json_raw(",\"subject\":");
  ir_explain_json_str(note->subject);
  ir_explain_json_raw(",\"verdict\":\"%s\",\"headline\":",
                      device_verdict_name(note->verdict));
  ir_explain_json_str(note->headline);
  ir_explain_json_raw(",\"reason\":");
  ir_explain_json_str(note->reason);
  ir_explain_json_raw(",\"fix\":");
  ir_explain_json_str(note->fix);
  ir_explain_json_raw(",\"code\":");
  ir_explain_json_str(note->code);
  ir_explain_json_raw(",\"sites\":%zu", note->sites);
  if (note->quantity_count > 0) {
    ir_explain_json_raw(",\"quantities\":{");
    for (size_t q = 0; q < note->quantity_count; q++) {
      ir_explain_json_raw("%s", q ? "," : "");
      ir_explain_json_str(note->quantities[q].name);
      ir_explain_json_raw(":%lld", note->quantities[q].value);
    }
    ir_explain_json_raw("}");
  }
  ir_explain_json_raw("}");
}

static void device_json(void) {
  ir_explain_json_raw("\"device\":{\"target\":");
  ir_explain_json_str(g_device.target);
  ir_explain_json_raw(",\"profile\":");
  ir_explain_json_str(g_device.profile);
  ir_explain_json_raw(",\"measuredBy\":");
  ir_explain_json_str(g_device.measured_by);
  ir_explain_json_raw(",\"kernels\":[");
  for (size_t f = 0; f < g_device.function_count; f++) {
    device_function_json(f);
  }
  ir_explain_json_raw("],\"notes\":[");
  for (size_t n = 0; n < g_device.note_count; n++) {
    device_note_json(n);
  }
  ir_explain_json_raw("],\"codeTotal\":%zu},", g_device.code_total);
}

void ir_explain_device_render(void) {
  if (!g_device.begun) {
    return;
  }
  if (g_device.note_count > 1) {
    qsort(g_device.notes, g_device.note_count, sizeof(*g_device.notes),
          device_note_compare);
  }
  device_text();
  device_json();
  device_release();
}
