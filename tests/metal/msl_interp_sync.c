#include "msl_interp_internal.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

size_t msl_sync_arg_slots(const MslInsn *insn) {
  switch (insn->a) {
    case MSL_SY_SUM:
    case MSL_SY_MIN:
    case MSL_SY_MAX:
    case MSL_SY_PREFIX_INC:
    case MSL_SY_PREFIX_EXC:
    case MSL_SY_BALLOT:
    case MSL_SY_ANY:
    case MSL_SY_ALL: return 1;
    case MSL_SY_BROADCAST:
    case MSL_SY_SHUFFLE: return 2;
    case MSL_SY_MAT_LOAD:
    case MSL_SY_MAT_STORE:
    case MSL_SY_MAT_MAC: return 8;
    default: return 0;
  }
}

static size_t sync_result_slots(const MslInsn *insn) {
  switch (insn->a) {
    case MSL_SY_BARRIER:
    case MSL_SY_SIMD_BARRIER:
    case MSL_SY_MAT_LOAD:
    case MSL_SY_MAT_STORE:
    case MSL_SY_MAT_MAC: return 0;
    default: return 1;
  }
}

static uint64_t float_result(double value) {
  return msl_flush_subnormal(MSL_S_FLOAT, msl_float_to_bits(msl_round_float(value)));
}

static double float_value(uint64_t bits) {
  return (double)msl_bits_to_float((uint32_t)msl_flush_subnormal(MSL_S_FLOAT, bits));
}

static int exec_math(MslThread *t, const MslInsn *in) {
  uint64_t *top = &t->stack[t->sp - 1];
  if (in->a == MSL_BI_FABS) {
    *top &= 0x7fffffffu;
    return 1;
  }
  *top = float_result(msl_math_unary((int)in->a, float_value(*top)));
  return 1;
}

static int exec_fmod(MslThread *t) {
  double y = float_value(t->stack[t->sp - 1]);
  double x = float_value(t->stack[t->sp - 2]);
  t->sp--;
  t->stack[t->sp - 1] = float_result(fmod(x, y));
  return 1;
}

static int exec_fma(MslThread *t, const MslInsn *in) {
  uint64_t *a;
  const uint64_t *b;
  const uint64_t *c;
  int lane;
  if (in->t1 && in->t1->kind == MSL_K_SCALAR) {
    a = &t->stack[t->sp - 3];
    a[0] = msl_fma_float((uint32_t)a[0], (uint32_t)a[1], (uint32_t)a[2]);
    t->sp -= 2;
    return 1;
  }
  a = &t->stack[t->sp - 6];
  b = &t->stack[t->sp - 4];
  c = &t->stack[t->sp - 2];
  for (lane = 0; lane < 2; lane++) {
    a[lane] = msl_fma_half((uint16_t)a[lane], (uint16_t)b[lane], (uint16_t)c[lane]);
  }
  t->sp -= 4;
  return 1;
}

static void fill_matrix_bytes(uint8_t *bytes, size_t elem, uint64_t value) {
  size_t i;
  size_t k;
  for (i = 0; i < 64; i++) {
    for (k = 0; k < elem; k++) {
      bytes[i * elem + k] = (uint8_t)(value >> (8 * k));
    }
  }
}

static int exec_fill(MslRun *run, MslThread *t, const MslInsn *in) {
  const MslType *matrix = in->t1;
  size_t slots = (size_t)matrix->slots;
  uint64_t value = t->stack[--t->sp];
  uint8_t *bytes;
  if (!msl_stack_reserve(run, t, slots)) {
    return 0;
  }
  memset(&t->stack[t->sp], 0, slots * sizeof(uint64_t));
  bytes = (uint8_t *)&t->stack[t->sp];
  fill_matrix_bytes(bytes, msl_scalars[matrix->scalar].size, value);
  memset(bytes + slots / 2 * 8, 1, matrix->size);
  t->sp += slots;
  return 1;
}

static uint64_t next_random(MslRun *run) {
  uint64_t x = run->cas_state;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  run->cas_state = x;
  return x;
}

static uint64_t atomic_update(int id, uint64_t old, uint64_t operand) {
  switch (id) {
    case MSL_BI_ATOMIC_STORE:
    case MSL_BI_ATOMIC_XCHG: return operand;
    case MSL_BI_ATOMIC_ADD: return (old + operand) & 0xffffffffu;
    case MSL_BI_ATOMIC_SUB: return (old - operand) & 0xffffffffu;
    case MSL_BI_ATOMIC_MIN: return operand < old ? operand : old;
    case MSL_BI_ATOMIC_MAX: return operand > old ? operand : old;
    case MSL_BI_ATOMIC_AND: return old & operand;
    case MSL_BI_ATOMIC_OR: return old | operand;
    case MSL_BI_ATOMIC_XOR: return old ^ operand;
    default: return old;
  }
}

static int exec_atomic(MslRun *run, MslThread *t, const MslInsn *in) {
  const MslType *uint_type = run->prog->scalar_types[MSL_S_UINT];
  int id = (int)in->a;
  int space = in->t1->space;
  size_t operands = id == MSL_BI_ATOMIC_LOAD ? 0 : 1;
  size_t base = t->sp - 2 - operands;
  MslPtr ptr = msl_ptr_unpack(&t->stack[base]);
  uint64_t operand = operands ? t->stack[t->sp - 1] : 0;
  uint64_t old = 0;
  if (id != MSL_BI_ATOMIC_STORE && !msl_read_value(run, t, in->line, ptr, space, uint_type, &old)) {
    return 0;
  }
  if (id != MSL_BI_ATOMIC_LOAD) {
    uint64_t fresh = atomic_update(id, old, operand);
    if (!msl_write_value(run, t, in->line, ptr, space, uint_type, &fresh)) {
      return 0;
    }
  }
  t->sp = base;
  if (id != MSL_BI_ATOMIC_STORE) {
    t->stack[t->sp++] = old;
  }
  return 1;
}

static int exec_cas(MslRun *run, MslThread *t, const MslInsn *in) {
  const MslType *uint_type = run->prog->scalar_types[MSL_S_UINT];
  int space = in->t1->space;
  MslPtr target = msl_ptr_unpack(&t->stack[t->sp - 5]);
  MslPtr expected_ptr = msl_ptr_unpack(&t->stack[t->sp - 3]);
  uint64_t desired = t->stack[t->sp - 1];
  uint64_t current = 0;
  uint64_t expected = 0;
  int spurious;
  int swapped = 0;
  if (!msl_read_value(run, t, in->line, target, space, uint_type, &current) ||
      !msl_read_value(run, t, in->line, expected_ptr, MSL_SP_THREAD, uint_type, &expected)) {
    return 0;
  }
  spurious = run->dispatch->spurious_cas != 0 && (next_random(run) >> 33) % 2u == 0u;
  if (!spurious && current == expected) {
    if (!msl_write_value(run, t, in->line, target, space, uint_type, &desired)) {
      return 0;
    }
    swapped = 1;
  } else if (!msl_write_value(run, t, in->line, expected_ptr, MSL_SP_THREAD, uint_type, &current)) {
    return 0;
  }
  t->sp -= 5;
  t->stack[t->sp++] = (uint64_t)swapped;
  return 1;
}

int msl_exec_builtin(MslRun *run, MslThread *thread, const MslInsn *insn) {
  switch (insn->a) {
    case MSL_BI_SQRT:
    case MSL_BI_RSQRT:
    case MSL_BI_SIN:
    case MSL_BI_COS:
    case MSL_BI_LOG:
    case MSL_BI_EXP:
    case MSL_BI_FABS: return exec_math(thread, insn);
    case MSL_BI_FMOD: return exec_fmod(thread);
    case MSL_BI_FMA: return exec_fma(thread, insn);
    case MSL_BI_FILL: return exec_fill(run, thread, insn);
    case MSL_BI_ATOMIC_CAS: return exec_cas(run, thread, insn);
    default: return exec_atomic(run, thread, insn);
  }
}

typedef struct {
  char *text;
  size_t size;
  size_t used;
} MslTextBuffer;

static void text_append(MslTextBuffer *out, const char *text, size_t len) {
  size_t room = out->size - 1 - out->used;
  if (len > room) {
    len = room;
  }
  memcpy(out->text + out->used, text, len);
  out->used += len;
  out->text[out->used] = 0;
}

static int is_wide(int scalar) {
  return scalar == MSL_S_LONG || scalar == MSL_S_ULONG;
}

static void format_one(MslTextBuffer *out, const char *spec, size_t spec_len, int conv, const MslType *type, uint64_t value) {
  char fmt[48];
  char piece[512];
  int n;
  int scalar = type->scalar;
  if (spec_len > 24) {
    spec_len = 24;
  }
  snprintf(fmt, sizeof fmt, "%%%.*s%s%c", (int)spec_len, spec, is_wide(scalar) ? "ll" : "", conv);
  if (msl_scalars[scalar].is_float) {
    n = snprintf(piece, sizeof piece, fmt, msl_float_kind_to_double(scalar, value));
  } else if (is_wide(scalar)) {
    n = snprintf(piece, sizeof piece, fmt, scalar == MSL_S_LONG ? (long long)(int64_t)value : (long long)value);
  } else if (msl_scalars[scalar].is_signed) {
    n = snprintf(piece, sizeof piece, fmt, (int)(int64_t)value);
  } else {
    n = snprintf(piece, sizeof piece, fmt, (unsigned)value);
  }
  if (n > 0) {
    text_append(out, piece, strlen(piece));
  }
}

static const char *skip_spec(const char *p) {
  while (*p != 0 && strchr("-+ #0", *p) != NULL) {
    p++;
  }
  while (*p >= '0' && *p <= '9') {
    p++;
  }
  if (*p == '.') {
    p++;
    while (*p >= '0' && *p <= '9') {
      p++;
    }
  }
  return p;
}

static void format_log(MslTextBuffer *out, const MslLogInfo *info, const uint64_t *args) {
  const char *p = info->format;
  size_t used = 0;
  while (*p != 0) {
    const char *spec;
    const char *end;
    if (*p != '%') {
      text_append(out, p, 1);
      p++;
      continue;
    }
    p++;
    if (*p == '%') {
      text_append(out, "%", 1);
      p++;
      continue;
    }
    spec = p;
    end = skip_spec(p);
    p = end;
    while (*p == 'l') {
      p++;
    }
    format_one(out, spec, (size_t)(end - spec), *p, info->arg_types[used], args[used]);
    used++;
    p++;
  }
}

int msl_exec_log(MslRun *run, MslThread *thread, const MslInsn *insn) {
  const MslLogInfo *info = (const MslLogInfo *)insn->p;
  char text[1024];
  MslTextBuffer out;
  out.text = text;
  out.size = sizeof text;
  out.used = 0;
  text[0] = 0;
  format_log(&out, info, &thread->stack[thread->sp - info->arg_count]);
  thread->sp -= info->arg_count;
  if (run->dispatch->log != NULL) {
    run->dispatch->log(run->dispatch->log_user, text);
  } else {
    printf("%s\n", text);
  }
  return 1;
}

static uint64_t lane_arg(const MslThread *t, size_t from_top) {
  return t->stack[t->sp - from_top];
}

static int is_float_insn(const MslInsn *in) {
  return in->t1 != NULL && in->t1->kind == MSL_K_SCALAR && in->t1->scalar == MSL_S_FLOAT;
}

static uint64_t add_values(int is_float, uint64_t a, uint64_t b) {
  uint64_t out = 0;
  char msg[8];
  if (is_float) {
    msl_float_binary(MSL_S_FLOAT, MSL_BIN_ADD, a, b, &out, msg, sizeof msg);
    return out;
  }
  return (a + b) & 0xffffffffu;
}

static int less_value(int is_float, uint64_t a, uint64_t b) {
  if (is_float) {
    return float_value(a) < float_value(b);
  }
  return a < b;
}

static int is_nan_bits(int is_float, uint64_t value) {
  return is_float && (value & 0x7f800000u) == 0x7f800000u && (value & 0x007fffffu) != 0u;
}

static uint64_t pick_extreme(int is_float, int want_max, uint64_t acc, uint64_t v) {
  if (is_nan_bits(is_float, acc)) {
    return v;
  }
  if (is_nan_bits(is_float, v)) {
    return acc;
  }
  if (want_max) {
    return less_value(is_float, acc, v) ? v : acc;
  }
  return less_value(is_float, v, acc) ? v : acc;
}

static void simd_reduce(MslThread **lanes, uint32_t count, const MslInsn *in, uint64_t *results) {
  int is_float = is_float_insn(in);
  uint64_t acc = lane_arg(lanes[0], 1);
  uint64_t running = is_float ? 0x80000000u : 0;
  uint32_t i;
  for (i = 1; i < count; i++) {
    uint64_t v = lane_arg(lanes[i], 1);
    if (in->a == MSL_SY_SUM) {
      acc = add_values(is_float, acc, v);
    } else if (in->a == MSL_SY_MIN || in->a == MSL_SY_MAX) {
      acc = pick_extreme(is_float, in->a == MSL_SY_MAX, acc, v);
    }
  }
  for (i = 0; i < count; i++) {
    uint64_t v = lane_arg(lanes[i], 1);
    if (in->a == MSL_SY_PREFIX_INC) {
      running = i == 0 ? v : add_values(is_float, running, v);
      results[i] = running;
    } else if (in->a == MSL_SY_PREFIX_EXC) {
      results[i] = running;
      running = i == 0 ? v : add_values(is_float, running, v);
    } else {
      results[i] = acc;
    }
  }
}

static MslThread *lane_by_index(MslThread **lanes, uint32_t count, uint64_t lane) {
  uint32_t i;
  for (i = 0; i < count; i++) {
    if (lanes[i]->lane == lane) {
      return lanes[i];
    }
  }
  return NULL;
}

static int simd_exchange(MslRun *run, MslThread **lanes, uint32_t count, const MslInsn *in, uint64_t *results) {
  const char *name = in->a == MSL_SY_BROADCAST ? "simd_broadcast" : "simd_shuffle";
  uint32_t i;
  for (i = 0; i < count; i++) {
    uint64_t source = lane_arg(lanes[i], 1);
    const MslThread *from;
    if (in->a == MSL_SY_BROADCAST && source != lane_arg(lanes[0], 1)) {
      return msl_run_fail(run, lanes[i], in->line, "simd_broadcast lane %llu differs from lane %u's choice of %llu",
                          (unsigned long long)source, lanes[0]->lane, (unsigned long long)lane_arg(lanes[0], 1));
    }
    if (source >= run->simd_width) {
      return msl_run_fail(run, lanes[i], in->line, "%s names lane %llu, outside the %u-wide SIMD-group", name,
                          (unsigned long long)source, run->simd_width);
    }
    from = lane_by_index(lanes, count, source);
    if (from == NULL) {
      return msl_run_fail(run, lanes[i], in->line, "%s names lane %llu, which is not active", name, (unsigned long long)source);
    }
    results[i] = lane_arg(from, 2);
  }
  return 1;
}

static void simd_vote(MslThread **lanes, uint32_t count, const MslInsn *in, uint64_t *results) {
  uint64_t mask = 0;
  uint64_t active = 0;
  uint32_t i;
  for (i = 0; i < count; i++) {
    uint64_t bit = 1ull << lanes[i]->lane;
    active |= bit;
    if (in->a != MSL_SY_ACTIVE && lane_arg(lanes[i], 1) != 0) {
      mask |= bit;
    }
  }
  for (i = 0; i < count; i++) {
    switch (in->a) {
      case MSL_SY_BALLOT: results[i] = mask; break;
      case MSL_SY_ACTIVE: results[i] = active; break;
      case MSL_SY_ANY: results[i] = mask != 0; break;
      default: results[i] = mask == active; break;
    }
  }
}

typedef struct {
  MslPtr matrix;
  MslPtr memory;
  uint64_t stride;
  uint64_t origin_x;
  uint64_t origin_y;
  uint64_t transpose;
} MslMatrixIo;

static MslMatrixIo matrix_io_args(const MslThread *t) {
  MslMatrixIo io;
  const uint64_t *args = &t->stack[t->sp - 8];
  io.matrix = msl_ptr_unpack(args);
  io.memory = msl_ptr_unpack(args + 2);
  io.stride = args[4];
  io.origin_x = args[5];
  io.origin_y = args[6];
  io.transpose = args[7];
  return io;
}

static int same_io(const MslMatrixIo *a, const MslMatrixIo *b) {
  return a->memory.space == b->memory.space && a->memory.object == b->memory.object && a->memory.offset == b->memory.offset &&
         a->stride == b->stride && a->origin_x == b->origin_x && a->origin_y == b->origin_y && a->transpose == b->transpose;
}

static int check_lanes_agree(MslRun *run, MslThread **lanes, uint32_t count, const MslInsn *in, MslMatrixIo *first) {
  const char *name = in->a == MSL_SY_MAT_LOAD ? "simdgroup_load" : "simdgroup_store";
  uint32_t i;
  *first = matrix_io_args(lanes[0]);
  for (i = 1; i < count; i++) {
    MslMatrixIo other = matrix_io_args(lanes[i]);
    if (!same_io(first, &other)) {
      return msl_run_fail(run, lanes[i], in->line, "%s: lane %u passes a different pointer, stride, origin or transpose than lane %u",
                          name, lanes[i]->lane, lanes[0]->lane);
    }
  }
  if (first->stride > 0xffffffu || first->origin_x > 0xffffffu || first->origin_y > 0xffffffu) {
    return msl_run_fail(run, lanes[0], in->line, "%s: stride or origin is out of range", name);
  }
  return 1;
}

static MslPtr element_pointer(const MslMatrixIo *io, int row, int col, size_t elem) {
  MslPtr ptr = io->memory;
  uint64_t r = io->transpose ? (uint64_t)col : (uint64_t)row;
  uint64_t c = io->transpose ? (uint64_t)row : (uint64_t)col;
  uint64_t index = (io->origin_y + r) * io->stride + io->origin_x + c;
  ptr.offset += (int64_t)(index * elem);
  return ptr;
}

static void matrix_slots(const MslType *matrix, const uint8_t *bytes, uint64_t *slots) {
  size_t count = (size_t)matrix->slots;
  memset(slots, 0, count * sizeof(uint64_t));
  memcpy(slots, bytes, matrix->size);
  memset((uint8_t *)slots + count / 2 * 8, 1, matrix->size);
}

static int matrix_initialized(const MslType *matrix, const uint64_t *slots) {
  const uint8_t *init = (const uint8_t *)slots + (size_t)matrix->slots / 2 * 8;
  size_t i;
  for (i = 0; i < matrix->size; i++) {
    if (!init[i]) {
      return 0;
    }
  }
  return 1;
}

static int read_matrix(MslRun *run, MslThread *t, const MslInsn *in, MslPtr ptr, const MslType *matrix, uint64_t *slots) {
  if (!msl_read_value(run, t, in->line, ptr, MSL_SP_THREAD, matrix, slots)) {
    return 0;
  }
  if (!matrix_initialized(matrix, slots)) {
    return msl_run_fail(run, t, in->line, "simdgroup matrix is read before every element is assigned");
  }
  return 1;
}

static int matrix_load(MslRun *run, MslThread **lanes, uint32_t count, const MslInsn *in) {
  const MslType *matrix = in->t1;
  const MslType *elem = matrix->elem;
  size_t size = elem->size;
  uint8_t bytes[256];
  uint64_t slots[64];
  MslMatrixIo io;
  uint32_t i;
  int r;
  int c;
  if (!check_lanes_agree(run, lanes, count, in, &io)) {
    return 0;
  }
  for (r = 0; r < 8; r++) {
    for (c = 0; c < 8; c++) {
      uint64_t value = 0;
      size_t k;
      if (!msl_read_value(run, lanes[0], in->line, element_pointer(&io, r, c, size), in->t2->space, elem, &value)) {
        return 0;
      }
      for (k = 0; k < size; k++) {
        bytes[(size_t)(r * 8 + c) * size + k] = (uint8_t)(value >> (8 * k));
      }
    }
  }
  matrix_slots(matrix, bytes, slots);
  for (i = 0; i < count; i++) {
    MslMatrixIo own = matrix_io_args(lanes[i]);
    if (!msl_write_value(run, lanes[i], in->line, own.matrix, MSL_SP_THREAD, matrix, slots)) {
      return 0;
    }
  }
  return 1;
}

static uint64_t element_bits(const uint8_t *bytes, size_t index, size_t size) {
  uint64_t value = 0;
  size_t k;
  for (k = 0; k < size; k++) {
    value |= (uint64_t)bytes[index * size + k] << (8 * k);
  }
  return value;
}

uint32_t msl_matrix_lane_element(uint32_t lane) {
  uint32_t q = lane / 4u;
  uint32_t row = (q & 4u) + (lane / 2u) % 4u;
  uint32_t col = (q & 2u) * 2u + (lane % 2u) * 2u;
  return row * 8u + col;
}

static int assemble_matrix(MslRun *run, MslThread **lanes, uint32_t count, const MslInsn *in, const MslPtr *ptrs,
                           const MslType *matrix, const char *name, uint64_t *out) {
  uint64_t copy[64];
  size_t size = matrix->elem->size;
  int full = count == 32 && run->simd_width == 32;
  uint32_t i;
  if (!read_matrix(run, lanes[0], in, ptrs[0], matrix, out)) {
    return 0;
  }
  for (i = 1; i < count; i++) {
    uint32_t e;
    if (!read_matrix(run, lanes[i], in, ptrs[i], matrix, copy)) {
      return 0;
    }
    if (memcmp(out, copy, matrix->size) == 0) {
      continue;
    }
    if (!full) {
      return msl_run_fail(run, lanes[i], in->line, "%s: lane %u holds a different matrix than lane %u", name, lanes[i]->lane,
                          lanes[0]->lane);
    }
    e = msl_matrix_lane_element(lanes[i]->lane);
    memcpy((uint8_t *)out + e * size, (const uint8_t *)copy + e * size, 2 * size);
  }
  return 1;
}

static int matrix_store(MslRun *run, MslThread **lanes, uint32_t count, const MslInsn *in) {
  const MslType *matrix = in->t1;
  const MslType *elem = matrix->elem;
  uint64_t first[64];
  MslPtr ptrs[64];
  MslMatrixIo io;
  uint32_t i;
  int r;
  int c;
  if (!check_lanes_agree(run, lanes, count, in, &io)) {
    return 0;
  }
  for (i = 0; i < count; i++) {
    ptrs[i] = matrix_io_args(lanes[i]).matrix;
  }
  if (!assemble_matrix(run, lanes, count, in, ptrs, matrix, "simdgroup_store", first)) {
    return 0;
  }
  for (r = 0; r < 8; r++) {
    for (c = 0; c < 8; c++) {
      uint64_t value = element_bits((const uint8_t *)first, (size_t)(r * 8 + c), elem->size);
      if (!msl_write_value(run, lanes[0], in->line, element_pointer(&io, r, c, elem->size), in->t2->space, elem, &value)) {
        return 0;
      }
    }
  }
  return 1;
}

static double matrix_element(const MslType *matrix, const uint64_t *slots, int row, int col) {
  uint64_t bits = element_bits((const uint8_t *)slots, (size_t)(row * 8 + col), matrix->elem->size);
  return msl_float_kind_to_double(matrix->scalar, bits);
}

static uint32_t float_chain(const uint64_t *a, const uint64_t *b, const uint64_t *c, int row, int col) {
  uint32_t sum = (uint32_t)element_bits((const uint8_t *)c, (size_t)(row * 8 + col), 4);
  int i;
  for (i = 0; i < 8; i++) {
    uint32_t left = (uint32_t)element_bits((const uint8_t *)a, (size_t)(row * 8 + i), 4);
    uint32_t right = (uint32_t)element_bits((const uint8_t *)b, (size_t)(i * 8 + col), 4);
    sum = msl_fma_float(left, right, sum);
  }
  return sum;
}

static void multiply_accumulate(const MslType *const *types, const uint64_t *a, const uint64_t *b, const uint64_t *c, uint8_t *out) {
  const MslType *d = types[0];
  size_t size = d->elem->size;
  int all_float = d->scalar == MSL_S_FLOAT && types[1]->scalar == MSL_S_FLOAT && types[2]->scalar == MSL_S_FLOAT &&
                  types[3]->scalar == MSL_S_FLOAT;
  int row;
  int col;
  for (row = 0; row < 8; row++) {
    for (col = 0; col < 8; col++) {
      double sum = matrix_element(types[3], c, row, col);
      uint64_t bits;
      size_t k;
      int i;
      for (i = 0; i < 8; i++) {
        sum += matrix_element(types[1], a, row, i) * matrix_element(types[2], b, i, col);
      }
      bits = all_float ? float_chain(a, b, c, row, col) : msl_double_to_float_kind(d->scalar, sum, 0);
      for (k = 0; k < size; k++) {
        out[(size_t)(row * 8 + col) * size + k] = (uint8_t)(bits >> (8 * k));
      }
    }
  }
}

static int matrix_mac(MslRun *run, MslThread **lanes, uint32_t count, const MslInsn *in) {
  const MslType *const *types = (const MslType *const *)in->p;
  uint64_t inputs[3][64];
  uint64_t slots[64];
  uint8_t bytes[256];
  MslPtr ptrs[64];
  uint32_t i;
  int k;
  for (k = 0; k < 3; k++) {
    for (i = 0; i < count; i++) {
      ptrs[i] = msl_ptr_unpack(&lanes[i]->stack[lanes[i]->sp - 8] + 2 * (k + 1));
    }
    if (!assemble_matrix(run, lanes, count, in, ptrs, types[k + 1], "simdgroup_multiply_accumulate", inputs[k])) {
      return 0;
    }
  }
  multiply_accumulate(types, inputs[0], inputs[1], inputs[2], bytes);
  matrix_slots(types[0], bytes, slots);
  for (i = 0; i < count; i++) {
    MslThread *t = lanes[i];
    if (!msl_write_value(run, t, in->line, msl_ptr_unpack(&t->stack[t->sp - 8]), MSL_SP_THREAD, types[0], slots)) {
      return 0;
    }
  }
  return 1;
}

static int simd_values(MslRun *run, MslThread **lanes, uint32_t count, const MslInsn *in, uint64_t *results) {
  switch (in->a) {
    case MSL_SY_SUM:
    case MSL_SY_MIN:
    case MSL_SY_MAX:
    case MSL_SY_PREFIX_INC:
    case MSL_SY_PREFIX_EXC: simd_reduce(lanes, count, in, results); return 1;
    case MSL_SY_BROADCAST:
    case MSL_SY_SHUFFLE: return simd_exchange(run, lanes, count, in, results);
    case MSL_SY_BALLOT:
    case MSL_SY_ACTIVE:
    case MSL_SY_ANY:
    case MSL_SY_ALL: simd_vote(lanes, count, in, results); return 1;
    case MSL_SY_MAT_LOAD: return matrix_load(run, lanes, count, in);
    case MSL_SY_MAT_STORE: return matrix_store(run, lanes, count, in);
    case MSL_SY_MAT_MAC: return matrix_mac(run, lanes, count, in);
    default: return 1;
  }
}

int msl_resolve_simd(MslRun *run, MslThread **lanes, uint32_t count, const MslInsn *insn) {
  uint64_t results[64];
  size_t args = msl_sync_arg_slots(insn);
  size_t outs = sync_result_slots(insn);
  uint32_t i;
  memset(results, 0, sizeof results);
  if (!simd_values(run, lanes, count, insn, results)) {
    return 0;
  }
  for (i = 0; i < count; i++) {
    MslThread *t = lanes[i];
    t->sp -= args;
    if (outs > 0) {
      if (!msl_stack_reserve(run, t, outs)) {
        return 0;
      }
      t->stack[t->sp++] = results[i];
    }
  }
  return 1;
}
