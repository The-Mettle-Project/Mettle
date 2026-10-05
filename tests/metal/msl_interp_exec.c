#include "msl_interp_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MSL_DECODE_OK, MSL_DECODE_UNINIT, MSL_DECODE_BADBOOL };

int msl_run_fail(MslRun *run, const MslThread *thread, int line, const char *fmt, ...) {
  char message[600];
  va_list ap;
  if (run->failed) {
    return 0;
  }
  va_start(ap, fmt);
  vsnprintf(message, sizeof message, fmt, ap);
  va_end(ap);
  if (thread != NULL) {
    msl_format_error(run->error, run->error_size, "kernel '%s' line %d, threadgroup (%u,%u,%u) thread (%u,%u,%u): %s",
                     run->kernel->name, line, run->group[0], run->group[1], run->group[2], thread->pos[0], thread->pos[1],
                     thread->pos[2], message);
  } else {
    msl_format_error(run->error, run->error_size, "kernel '%s': %s", run->kernel != NULL ? run->kernel->name : "?", message);
  }
  run->failed = 1;
  return 0;
}

uint32_t msl_object_new(MslRun *run, int space, size_t size, size_t align, int32_t owner) {
  uint32_t index;
  MslObject *object;
  if (run->free_count > 0) {
    index = run->free_objects[--run->free_count];
  } else {
    if (run->object_count >= (1u << 24)) {
      return 0;
    }
    if (run->object_count == run->object_cap) {
      size_t cap = run->object_cap == 0 ? 256 : run->object_cap * 2;
      MslObject *grown = (MslObject *)realloc(run->objects, cap * sizeof(MslObject));
      if (grown == NULL) {
        return 0;
      }
      run->objects = grown;
      run->object_cap = cap;
    }
    index = (uint32_t)run->object_count++;
    memset(&run->objects[index], 0, sizeof(MslObject));
  }
  object = &run->objects[index];
  object->bytes = NULL;
  object->init = NULL;
  object->size = size;
  object->align = align == 0 ? 1 : align;
  object->base = 0;
  object->owner = owner;
  object->space = (uint8_t)space;
  object->alive = 1;
  object->readonly = 0;
  object->heap = 0;
  return index;
}

void msl_object_release(MslRun *run, uint32_t index) {
  MslObject *object = &run->objects[index];
  if (!object->alive) {
    return;
  }
  if (object->heap) {
    free(object->bytes);
  }
  object->bytes = NULL;
  object->init = NULL;
  object->alive = 0;
  object->heap = 0;
  object->gen++;
  if (run->free_count == run->free_cap) {
    size_t cap = run->free_cap == 0 ? 256 : run->free_cap * 2;
    uint32_t *grown = (uint32_t *)realloc(run->free_objects, cap * sizeof(uint32_t));
    if (grown == NULL) {
      return;
    }
    run->free_objects = grown;
    run->free_cap = cap;
  }
  run->free_objects[run->free_count++] = index;
}

MslPtr msl_ptr_unpack(const uint64_t *slots) {
  MslPtr ptr;
  ptr.space = (int)(slots[0] & 0xffu);
  ptr.object = (uint32_t)((slots[0] >> 8) & 0xffffffu);
  ptr.gen = (uint32_t)(slots[0] >> 32);
  ptr.offset = (int64_t)slots[1];
  return ptr;
}

void msl_ptr_pack(MslPtr ptr, uint64_t *slots) {
  slots[0] = (uint64_t)(uint8_t)ptr.space | ((uint64_t)ptr.object << 8) | ((uint64_t)ptr.gen << 32);
  slots[1] = (uint64_t)ptr.offset;
}

uint64_t msl_ptr_encode(const MslRun *run, MslPtr ptr) {
  int64_t offset = ptr.offset;
  if (ptr.object == 0) {
    return (uint64_t)ptr.offset;
  }
  if (ptr.space == MSL_SP_DEVICE && ptr.object < run->object_count) {
    return run->objects[ptr.object].base + (uint64_t)ptr.offset;
  }
  if (offset < INT32_MIN || offset > INT32_MAX) {
    offset = INT32_MIN;
  }
  return (1ull << 63) | ((uint64_t)(ptr.space & 0x7f) << 56) | ((uint64_t)ptr.object << 32) | (uint64_t)(uint32_t)offset;
}

MslPtr msl_ptr_decode(const MslRun *run, uint64_t bits) {
  MslPtr ptr;
  size_t i;
  memset(&ptr, 0, sizeof ptr);
  if (bits >> 63) {
    ptr.space = (int)((bits >> 56) & 0x7fu);
    ptr.object = (uint32_t)((bits >> 32) & 0xffffffu);
    ptr.offset = (int64_t)(int32_t)(uint32_t)bits;
    ptr.gen = ptr.object < run->object_count ? run->objects[ptr.object].gen : 0;
    return ptr;
  }
  ptr.space = MSL_SP_DEVICE;
  for (i = 0; i < run->prog->buffer_count; i++) {
    const MslBuffer *buffer = &run->prog->buffers[i];
    if (bits >= buffer->address && bits <= buffer->address + buffer->size) {
      ptr.object = (uint32_t)(i + 1);
      ptr.gen = run->objects[ptr.object].gen;
      ptr.offset = (int64_t)(bits - buffer->address);
      return ptr;
    }
  }
  ptr.offset = (int64_t)bits;
  return ptr;
}

static int access_object(MslRun *run, const MslThread *thread, int line, MslPtr ptr, int space, MslObject **out) {
  MslObject *object;
  if (ptr.object == 0) {
    if (ptr.offset == 0) {
      return msl_run_fail(run, thread, line, "null pointer dereference");
    }
    return msl_run_fail(run, thread, line, "access through address 0x%llx, which is not inside any registered buffer",
                        (unsigned long long)ptr.offset);
  }
  if (ptr.space != space) {
    return msl_run_fail(run, thread, line, "pointer to %s memory used as a %s pointer", msl_space_name(ptr.space), msl_space_name(space));
  }
  if (ptr.object >= run->object_count) {
    return msl_run_fail(run, thread, line, "invalid pointer");
  }
  object = &run->objects[ptr.object];
  if (!object->alive || object->gen != ptr.gen || object->space != space) {
    return msl_run_fail(run, thread, line, "dangling %s pointer: the object it pointed to no longer exists", msl_space_name(space));
  }
  if (space == MSL_SP_THREAD && thread != NULL && object->owner != (int32_t)thread->lid) {
    return msl_run_fail(run, thread, line, "access to thread memory of another thread");
  }
  *out = object;
  return 1;
}

int msl_mem_access(MslRun *run, const MslThread *thread, int line, MslPtr ptr, int space, size_t size, size_t align, int write, MslObject **out) {
  MslObject *object;
  uint64_t address;
  if (!access_object(run, thread, line, ptr, space, &object)) {
    return 0;
  }
  if (ptr.offset < 0 || (uint64_t)ptr.offset > object->size || size > object->size - (size_t)ptr.offset) {
    return msl_run_fail(run, thread, line, "out-of-bounds access: %u bytes at offset %lld of a %llu-byte %s object", (unsigned)size,
                        (long long)ptr.offset, (unsigned long long)object->size, msl_space_name(space));
  }
  address = space == MSL_SP_DEVICE ? object->base + (uint64_t)ptr.offset : (uint64_t)ptr.offset;
  if (align > 1 && (address % align != 0 || object->align % align != 0)) {
    return msl_run_fail(run, thread, line, "misaligned %u-byte access at offset %lld (needs %u-byte alignment, object is %u-aligned)",
                        (unsigned)size, (long long)ptr.offset, (unsigned)align, (unsigned)object->align);
  }
  if (write && object->readonly) {
    return msl_run_fail(run, thread, line, "write to constant memory");
  }
  *out = object;
  return 1;
}

static uint64_t load_le(const uint8_t *bytes, size_t n) {
  uint64_t value = 0;
  size_t i;
  for (i = 0; i < n; i++) {
    value |= (uint64_t)bytes[i] << (8 * i);
  }
  return value;
}

static void store_le(uint8_t *bytes, size_t n, uint64_t value) {
  size_t i;
  for (i = 0; i < n; i++) {
    bytes[i] = (uint8_t)(value >> (8 * i));
  }
}

static int initialized(const uint8_t *init, size_t n) {
  size_t i;
  if (init == NULL) {
    return 1;
  }
  for (i = 0; i < n; i++) {
    if (!init[i]) {
      return 0;
    }
  }
  return 1;
}

static void mark_init(uint8_t *init, size_t n) {
  if (init != NULL) {
    memset(init, 1, n);
  }
}

static int decode_scalar(int scalar, const uint8_t *bytes, const uint8_t *init, uint64_t *slot) {
  size_t n = msl_scalars[scalar].size;
  uint64_t raw;
  if (!initialized(init, n)) {
    return MSL_DECODE_UNINIT;
  }
  raw = load_le(bytes, n);
  if (scalar == MSL_S_BOOL && raw > 1) {
    return MSL_DECODE_BADBOOL;
  }
  *slot = msl_canonical(scalar, raw);
  return MSL_DECODE_OK;
}

static int decode_value(const MslRun *run, const MslType *type, const uint8_t *bytes, const uint8_t *init, uint64_t *slots) {
  int i;
  size_t half;
  switch (type->kind) {
    case MSL_K_SCALAR:
      return decode_scalar(type->scalar, bytes, init, slots);
    case MSL_K_VECTOR:
      for (i = 0; i < type->count; i++) {
        size_t step = msl_scalars[type->scalar].size * (size_t)i;
        int code = decode_scalar(type->scalar, bytes + step, init != NULL ? init + step : NULL, &slots[i]);
        if (code != MSL_DECODE_OK) {
          return code;
        }
      }
      return MSL_DECODE_OK;
    case MSL_K_POINTER:
      if (!initialized(init, 8)) {
        return MSL_DECODE_UNINIT;
      }
      msl_ptr_pack(msl_ptr_decode(run, load_le(bytes, 8)), slots);
      return MSL_DECODE_OK;
    case MSL_K_VOTE:
      if (!initialized(init, 8)) {
        return MSL_DECODE_UNINIT;
      }
      slots[0] = load_le(bytes, 8);
      return MSL_DECODE_OK;
    default:
      half = (size_t)type->slots / 2 * 8;
      memset(slots, 0, (size_t)type->slots * sizeof(uint64_t));
      memcpy(slots, bytes, type->size);
      if (init != NULL) {
        memcpy((uint8_t *)slots + half, init, type->size);
      } else {
        memset((uint8_t *)slots + half, 1, type->size);
      }
      return MSL_DECODE_OK;
  }
}

static void encode_value(const MslRun *run, const MslType *type, const uint64_t *slots, uint8_t *bytes, uint8_t *init) {
  int i;
  size_t half;
  switch (type->kind) {
    case MSL_K_SCALAR:
      store_le(bytes, type->size, slots[0]);
      mark_init(init, type->size);
      return;
    case MSL_K_VECTOR:
      for (i = 0; i < type->count; i++) {
        size_t n = msl_scalars[type->scalar].size;
        store_le(bytes + n * (size_t)i, n, slots[i]);
        mark_init(init != NULL ? init + n * (size_t)i : NULL, n);
      }
      return;
    case MSL_K_POINTER:
      store_le(bytes, 8, msl_ptr_encode(run, msl_ptr_unpack(slots)));
      mark_init(init, 8);
      return;
    case MSL_K_VOTE:
      store_le(bytes, 8, slots[0]);
      mark_init(init, 8);
      return;
    default:
      half = (size_t)type->slots / 2 * 8;
      memcpy(bytes, slots, type->size);
      if (init != NULL) {
        memcpy(init, (const uint8_t *)slots + half, type->size);
      }
      return;
  }
}

static int decode_failure(MslRun *run, const MslThread *thread, int line, int code, const char *what) {
  if (code == MSL_DECODE_BADBOOL) {
    return msl_run_fail(run, thread, line, "%s holds a bool that is neither 0 nor 1", what);
  }
  return msl_run_fail(run, thread, line, "%s", what);
}

int msl_read_value(MslRun *run, const MslThread *thread, int line, MslPtr ptr, int space, const MslType *type, uint64_t *slots) {
  MslObject *object;
  int code;
  char what[160];
  if (!msl_mem_access(run, thread, line, ptr, space, type->size, type->align, 0, &object)) {
    return 0;
  }
  code = decode_value(run, type, object->bytes + ptr.offset, object->init != NULL ? object->init + ptr.offset : NULL, slots);
  if (code == MSL_DECODE_OK) {
    return 1;
  }
  snprintf(what, sizeof what, "read of uninitialized %s memory at offset %lld", msl_space_name(space), (long long)ptr.offset);
  return decode_failure(run, thread, line, code, code == MSL_DECODE_UNINIT ? what : "memory");
}

int msl_write_value(MslRun *run, const MslThread *thread, int line, MslPtr ptr, int space, const MslType *type, const uint64_t *slots) {
  MslObject *object;
  if (!msl_mem_access(run, thread, line, ptr, space, type->size, type->align, 1, &object)) {
    return 0;
  }
  encode_value(run, type, slots, object->bytes + ptr.offset, object->init != NULL ? object->init + ptr.offset : NULL);
  return 1;
}

int msl_stack_reserve(MslRun *run, MslThread *thread, size_t slots) {
  if (thread->sp + slots <= thread->cap) {
    return 1;
  }
  {
    size_t cap = thread->cap == 0 ? 64 : thread->cap;
    uint64_t *grown;
    while (cap < thread->sp + slots) {
      cap *= 2;
    }
    grown = (uint64_t *)realloc(thread->stack, cap * sizeof(uint64_t));
    if (grown == NULL) {
      return msl_run_fail(run, thread, 0, "out of memory");
    }
    thread->stack = grown;
    thread->cap = cap;
  }
  return 1;
}

static int frame_enter(MslRun *run, MslThread *t, const MslFunction *fn) {
  MslFrame *frame;
  size_t i;
  if (t->depth >= 64) {
    return msl_run_fail(run, t, fn->line, "call depth exceeds 64");
  }
  if (t->depth == t->frame_cap) {
    size_t cap = t->frame_cap == 0 ? 8 : t->frame_cap * 2;
    MslFrame *grown = (MslFrame *)realloc(t->frames, cap * sizeof(MslFrame));
    if (grown == NULL) {
      return msl_run_fail(run, t, fn->line, "out of memory");
    }
    t->frames = grown;
    t->frame_cap = cap;
  }
  frame = &t->frames[t->depth];
  memset(frame, 0, sizeof *frame);
  frame->fn = fn;
  frame->stack_base = t->sp;
  frame->objects = (uint32_t *)calloc(fn->local_count + 1, sizeof(uint32_t));
  frame->storage = (uint8_t *)calloc(fn->frame_bytes * 2 + 1, 1);
  t->depth++;
  if (frame->objects == NULL || frame->storage == NULL) {
    return msl_run_fail(run, t, fn->line, "out of memory");
  }
  for (i = 0; i < fn->local_count; i++) {
    const MslLocal *local = &fn->locals[i];
    uint32_t index;
    if (local->storage == MSL_VAR_GROUP) {
      t->frames[t->depth - 1].objects[i] = run->group_objects[i];
      continue;
    }
    if (local->storage == MSL_VAR_ARGS) {
      t->frames[t->depth - 1].objects[i] = run->args_object;
      continue;
    }
    index = msl_object_new(run, MSL_SP_THREAD, local->type->size, local->align, (int32_t)t->lid);
    if (index == 0) {
      return msl_run_fail(run, t, fn->line, "out of memory");
    }
    frame = &t->frames[t->depth - 1];
    run->objects[index].bytes = frame->storage + fn->local_offsets[i];
    run->objects[index].init = frame->storage + fn->frame_bytes + fn->local_offsets[i];
    frame->objects[i] = index;
  }
  return 1;
}

static void frame_leave(MslRun *run, MslThread *t) {
  MslFrame *frame = &t->frames[t->depth - 1];
  size_t i;
  if (frame->objects != NULL) {
    for (i = 0; i < frame->fn->local_count; i++) {
      if (frame->fn->locals[i].storage == MSL_VAR_THREAD && frame->objects[i] != 0) {
        msl_object_release(run, frame->objects[i]);
      }
    }
  }
  free(frame->objects);
  free(frame->storage);
  t->depth--;
}

int msl_thread_start(MslRun *run, MslThread *t, const MslFunction *fn);
void msl_thread_unwind(MslRun *run, MslThread *t);
int msl_write_local(MslRun *run, MslThread *t, int local, const uint64_t *slots);

int msl_thread_start(MslRun *run, MslThread *t, const MslFunction *fn) {
  return frame_enter(run, t, fn);
}

void msl_thread_unwind(MslRun *run, MslThread *t) {
  while (t->depth > 0) {
    frame_leave(run, t);
  }
}

int msl_write_local(MslRun *run, MslThread *t, int local, const uint64_t *slots) {
  MslFrame *frame = &t->frames[t->depth - 1];
  MslObject *object = &run->objects[frame->objects[local]];
  encode_value(run, frame->fn->locals[local].type, slots, object->bytes, object->init);
  return 1;
}

static MslObject *local_object(MslRun *run, MslThread *t, int64_t local) {
  return &run->objects[t->frames[t->depth - 1].objects[local]];
}

static int op_load_local(MslRun *run, MslThread *t, const MslInsn *in) {
  MslObject *object = local_object(run, t, in->a);
  int code;
  char what[160];
  if (!msl_stack_reserve(run, t, (size_t)in->t1->slots)) {
    return 0;
  }
  code = decode_value(run, in->t1, object->bytes, object->init, &t->stack[t->sp]);
  if (code != MSL_DECODE_OK) {
    snprintf(what, sizeof what, "variable '%s' is read before it is assigned", t->frames[t->depth - 1].fn->locals[in->a].name);
    return decode_failure(run, t, in->line, code, what);
  }
  t->sp += (size_t)in->t1->slots;
  return 1;
}

static int op_store_local(MslRun *run, MslThread *t, const MslInsn *in) {
  MslObject *object = local_object(run, t, in->a);
  t->sp -= (size_t)in->t1->slots;
  encode_value(run, in->t1, &t->stack[t->sp], object->bytes, object->init);
  return 1;
}

static int op_addr_local(MslRun *run, MslThread *t, const MslInsn *in) {
  MslPtr ptr;
  uint32_t index = t->frames[t->depth - 1].objects[in->a];
  if (!msl_stack_reserve(run, t, 2)) {
    return 0;
  }
  ptr.space = (int)in->b;
  ptr.object = index;
  ptr.gen = run->objects[index].gen;
  ptr.offset = 0;
  msl_ptr_pack(ptr, &t->stack[t->sp]);
  t->sp += 2;
  return 1;
}

static int op_load(MslRun *run, MslThread *t, const MslInsn *in) {
  MslPtr ptr = msl_ptr_unpack(&t->stack[t->sp - 2]);
  t->sp -= 2;
  if (!msl_stack_reserve(run, t, (size_t)in->t1->slots)) {
    return 0;
  }
  if (!msl_read_value(run, t, in->line, ptr, (int)in->a, in->t1, &t->stack[t->sp])) {
    return 0;
  }
  t->sp += (size_t)in->t1->slots;
  return 1;
}

static int op_store(MslRun *run, MslThread *t, const MslInsn *in) {
  size_t slots = (size_t)in->t1->slots;
  MslPtr ptr = msl_ptr_unpack(&t->stack[t->sp - slots - 2]);
  if (!msl_write_value(run, t, in->line, ptr, (int)in->a, in->t1, &t->stack[t->sp - slots])) {
    return 0;
  }
  t->sp -= slots + 2;
  return 1;
}

static int64_t index_value(const MslType *type, uint64_t slot, int *huge) {
  *huge = 0;
  if (msl_scalars[type->scalar].is_signed) {
    return (int64_t)slot;
  }
  if (slot > (uint64_t)INT64_MAX) {
    *huge = 1;
    return 0;
  }
  return (int64_t)slot;
}

static int op_ptr_add(MslRun *run, MslThread *t, const MslInsn *in) {
  int huge;
  int64_t index = index_value(in->t1, t->stack[--t->sp], &huge);
  MslPtr ptr = msl_ptr_unpack(&t->stack[t->sp - 2]);
  (void)run;
  if (huge || index > ((int64_t)1 << 40) || index < -((int64_t)1 << 40)) {
    ptr.offset = (int64_t)1 << 60;
  } else {
    ptr.offset += in->b * index * in->a;
  }
  if (ptr.offset > ((int64_t)1 << 60) || ptr.offset < -((int64_t)1 << 60)) {
    ptr.offset = (int64_t)1 << 60;
  }
  msl_ptr_pack(ptr, &t->stack[t->sp - 2]);
  return 1;
}

static int op_index(MslRun *run, MslThread *t, const MslInsn *in) {
  int huge;
  int64_t index = index_value(in->t1, t->stack[--t->sp], &huge);
  MslPtr ptr = msl_ptr_unpack(&t->stack[t->sp - 2]);
  if (huge || index < 0 || index >= in->b) {
    return msl_run_fail(run, t, in->line, "array index %lld is out of bounds [0, %lld)", huge ? -1LL : (long long)index, (long long)in->b);
  }
  ptr.offset += index * in->a;
  msl_ptr_pack(ptr, &t->stack[t->sp - 2]);
  return 1;
}

static int scalar_binary(MslRun *run, MslThread *t, int line, int op, int scalar, int rscalar, uint64_t a, uint64_t b, uint64_t *out) {
  char msg[256];
  int ok;
  if (op == MSL_BIN_SHL || op == MSL_BIN_SHR) {
    ok = msl_int_shift(scalar, op == MSL_BIN_SHL, a, rscalar, b, out, msg, sizeof msg);
  } else if (msl_scalars[scalar].is_float) {
    ok = msl_float_binary(scalar, op, a, b, out, msg, sizeof msg);
  } else {
    ok = msl_int_binary(scalar, op, a, b, out, msg, sizeof msg);
  }
  return ok ? 1 : msl_run_fail(run, t, line, "%s", msg);
}

static int op_binary(MslRun *run, MslThread *t, const MslInsn *in) {
  const MslType *lt = in->t1;
  int op = (int)in->a;
  if (lt->kind == MSL_K_NULLPTR) {
    uint64_t r = msl_ptr_encode(run, msl_ptr_unpack(&t->stack[t->sp - 2]));
    uint64_t l = msl_ptr_encode(run, msl_ptr_unpack(&t->stack[t->sp - 4]));
    t->sp -= 4;
    t->stack[t->sp++] = op == MSL_BIN_EQ ? l == r : l != r;
    return 1;
  }
  if (lt->kind == MSL_K_VECTOR) {
    size_t n = (size_t)lt->count;
    size_t i;
    uint64_t *l = &t->stack[t->sp - 2 * n];
    uint64_t *r = &t->stack[t->sp - n];
    for (i = 0; i < n; i++) {
      if (!scalar_binary(run, t, in->line, op, lt->scalar, lt->scalar, l[i], r[i], &l[i])) {
        return 0;
      }
    }
    t->sp -= n;
    return 1;
  }
  t->sp--;
  return scalar_binary(run, t, in->line, op, lt->scalar, in->t2->scalar, t->stack[t->sp - 1], t->stack[t->sp], &t->stack[t->sp - 1]);
}

static int op_unary(MslRun *run, MslThread *t, const MslInsn *in) {
  const MslType *type = in->t1;
  size_t n = type->kind == MSL_K_VECTOR ? (size_t)type->count : 1;
  size_t i;
  char msg[256];
  for (i = 0; i < n; i++) {
    uint64_t *slot = &t->stack[t->sp - n + i];
    if (!msl_unary(type->scalar, (int)in->a, *slot, slot, msg, sizeof msg)) {
      return msl_run_fail(run, t, in->line, "%s", msg);
    }
  }
  return 1;
}

static int op_cast(MslRun *run, MslThread *t, const MslInsn *in) {
  char msg[256];
  uint64_t *slot = &t->stack[t->sp - 1];
  if (!msl_convert(in->t1->scalar, in->t2->scalar, *slot, slot, msg, sizeof msg)) {
    return msl_run_fail(run, t, in->line, "%s", msg);
  }
  return 1;
}

static int op_astype(MslRun *run, MslThread *t, const MslInsn *in) {
  uint8_t bytes[16];
  size_t from = (size_t)in->t1->slots;
  memset(bytes, 0, sizeof bytes);
  encode_value(run, in->t1, &t->stack[t->sp - from], bytes, NULL);
  t->sp -= from;
  if (!msl_stack_reserve(run, t, (size_t)in->t2->slots)) {
    return 0;
  }
  decode_value(run, in->t2, bytes, NULL, &t->stack[t->sp]);
  t->sp += (size_t)in->t2->slots;
  return 1;
}

static int op_ptr_int(MslRun *run, MslThread *t, const MslInsn *in) {
  if (in->op == MSL_OP_PTR_TO_INT) {
    uint64_t bits = msl_ptr_encode(run, msl_ptr_unpack(&t->stack[t->sp - 2]));
    t->sp -= 2;
    t->stack[t->sp++] = bits;
    return 1;
  }
  if (!msl_stack_reserve(run, t, 1)) {
    return 0;
  }
  {
    uint64_t bits = t->stack[t->sp - 1];
    msl_ptr_pack(msl_ptr_decode(run, bits), &t->stack[t->sp - 1]);
    t->sp++;
  }
  return 1;
}

static int op_extract(MslThread *t, const MslInsn *in) {
  size_t n = (size_t)in->t1->count;
  uint64_t lane = t->stack[t->sp - n + (size_t)in->a];
  t->sp -= n;
  t->stack[t->sp++] = lane;
  return 1;
}

static int op_zero(MslRun *run, MslThread *t, const MslInsn *in) {
  size_t slots = (size_t)in->t1->slots;
  if (!msl_stack_reserve(run, t, slots)) {
    return 0;
  }
  memset(&t->stack[t->sp], 0, slots * sizeof(uint64_t));
  memset((uint8_t *)&t->stack[t->sp] + slots / 2 * 8, 1, in->t1->size);
  t->sp += slots;
  return 1;
}

static int op_call(MslRun *run, MslThread *t, const MslInsn *in) {
  const MslFunction *fn = (const MslFunction *)in->p;
  size_t base = t->sp - fn->param_slots;
  size_t offset = base;
  size_t i;
  t->sp = base;
  if (!frame_enter(run, t, fn)) {
    return 0;
  }
  for (i = 0; i < fn->param_count; i++) {
    MslObject *object = local_object(run, t, (int64_t)i);
    encode_value(run, fn->locals[i].type, &t->stack[offset], object->bytes, object->init);
    offset += (size_t)fn->locals[i].type->slots;
  }
  return 1;
}

static int op_return(MslRun *run, MslThread *t, const MslInsn *in) {
  size_t slots = in->op == MSL_OP_RETURN_VALUE ? (size_t)in->t1->slots : 0;
  size_t base = t->frames[t->depth - 1].stack_base;
  memmove(&t->stack[base], &t->stack[t->sp - slots], slots * sizeof(uint64_t));
  t->sp = base + slots;
  frame_leave(run, t);
  if (t->depth == 0) {
    t->state = MSL_TS_DONE;
  }
  return 1;
}

static int op_push(MslRun *run, MslThread *t, const MslInsn *in) {
  if (!msl_stack_reserve(run, t, in->op == MSL_OP_DUP ? (size_t)in->a : 2)) {
    return 0;
  }
  if (in->op == MSL_OP_CONST) {
    t->stack[t->sp++] = (uint64_t)in->a;
  } else if (in->op == MSL_OP_NULLPTR) {
    t->stack[t->sp++] = 0;
    t->stack[t->sp++] = 0;
  } else {
    memcpy(&t->stack[t->sp], &t->stack[t->sp - (size_t)in->a], (size_t)in->a * sizeof(uint64_t));
    t->sp += (size_t)in->a;
  }
  return 1;
}

static int op_uninit(MslRun *run, MslThread *t, const MslInsn *in) {
  MslObject *object = local_object(run, t, in->a);
  if (object->init != NULL) {
    memset(object->init, 0, object->size);
  }
  return 1;
}

static int step(MslRun *run, MslThread *t, const MslInsn *in) {
  switch (in->op) {
    case MSL_OP_CONST:
    case MSL_OP_NULLPTR:
    case MSL_OP_DUP: return op_push(run, t, in);
    case MSL_OP_ZERO: return op_zero(run, t, in);
    case MSL_OP_LOAD_LOCAL: return op_load_local(run, t, in);
    case MSL_OP_STORE_LOCAL: return op_store_local(run, t, in);
    case MSL_OP_ADDR_LOCAL: return op_addr_local(run, t, in);
    case MSL_OP_UNINIT_LOCAL: return op_uninit(run, t, in);
    case MSL_OP_LOAD: return op_load(run, t, in);
    case MSL_OP_STORE: return op_store(run, t, in);
    case MSL_OP_PTR_ADD: return op_ptr_add(run, t, in);
    case MSL_OP_FIELD: t->stack[t->sp - 1] += (uint64_t)in->a; return 1;
    case MSL_OP_LANE_ELEM:
      if (run->simd_width != 32) {
        return msl_run_fail(run, t, in->line, "thread_elements() follows the 32-lane matrix layout; this dispatch has %u lanes",
                            run->simd_width);
      }
      t->stack[t->sp - 1] += (uint64_t)((msl_matrix_lane_element(t->lane) + (uint32_t)in->a) * (uint64_t)in->b);
      return 1;
    case MSL_OP_INDEX: return op_index(run, t, in);
    case MSL_OP_BINARY: return op_binary(run, t, in);
    case MSL_OP_UNARY: return op_unary(run, t, in);
    case MSL_OP_CAST: return op_cast(run, t, in);
    case MSL_OP_ASTYPE: return op_astype(run, t, in);
    case MSL_OP_PTR_TO_INT:
    case MSL_OP_INT_TO_PTR: return op_ptr_int(run, t, in);
    case MSL_OP_EXTRACT: return op_extract(t, in);
    case MSL_OP_JUMP: t->frames[t->depth - 1].pc = (size_t)in->a; return 1;
    case MSL_OP_JUMP_FALSE:
      if (t->stack[--t->sp] == 0) {
        t->frames[t->depth - 1].pc = (size_t)in->a;
      }
      return 1;
    case MSL_OP_POP: t->sp -= (size_t)in->a; return 1;
    case MSL_OP_CALL: return op_call(run, t, in);
    case MSL_OP_RETURN:
    case MSL_OP_RETURN_VALUE: return op_return(run, t, in);
    case MSL_OP_BUILTIN: return msl_exec_builtin(run, t, in);
    case MSL_OP_LOG: return msl_exec_log(run, t, in);
    case MSL_OP_SYNC:
      t->state = in->a == MSL_SY_BARRIER ? MSL_TS_BARRIER : MSL_TS_SIMD;
      t->wait = in;
      t->wait_line = in->line;
      return 1;
    default:
      return msl_run_fail(run, t, in->line, "function '%s' reached its end without returning a value", t->frames[t->depth - 1].fn->name);
  }
}

int msl_exec_thread(MslRun *run, MslThread *t) {
  while (t->state == MSL_TS_READY) {
    MslFrame *frame = &t->frames[t->depth - 1];
    const MslInsn *in = &frame->fn->code[frame->pc++];
    if (in->statement && ++run->steps > run->budget) {
      return msl_run_fail(run, t, in->line, "step budget of %llu statements exhausted (infinite loop?)", (unsigned long long)run->budget);
    }
    if (!step(run, t, in)) {
      return 0;
    }
  }
  return 1;
}
