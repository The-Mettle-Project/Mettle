static NumTerm ii_sym_read_bytes(IRInterpMachine *machine, IIBuffer *buf,
                                 long long offset, int size);
static int ii_sym_write_bytes(IRInterpMachine *machine, IIBuffer *buf,
                              long long offset, int size, NumTerm term);
static int ii_sym_device_call(IRInterpMachine *machine,
                              MtlcIntrinsic intrinsic,
                              const IRInterpValue *args, size_t count,
                              IRInterpValue *out, int *handled);

static int ii_gpu_is_collective(const IRInstruction *insn) {
  if (insn->op == IR_OP_TENSOR_MMA || insn->op == IR_OP_TENSOR_SCALE_ROWS) {
    return 1;
  }
  if (insn->op == IR_OP_BARRIER &&
      insn->memory_scope == MTLC_MEMORY_SCOPE_SUBGROUP) {
    return 1;
  }
  return insn->op == IR_OP_CALL && ir_intrinsic_is_subgroup(insn->intrinsic) &&
         insn->intrinsic != MTLC_INTRINSIC_GPU_SUBGROUP_LOCAL_ID &&
         insn->intrinsic != MTLC_INTRINSIC_GPU_SUBGROUP_SIZE;
}

static int ii_pending_check(IRInterpMachine *machine, IIBuffer *buf,
                            long long offset, long long length) {
  if (!buf || !buf->pending) {
    return 1;
  }
  for (long long i = 0; i < length; i++) {
    if (buf->pending[offset + i]) {
      snprintf(machine->detail, sizeof(machine->detail),
               "a read of workgroup bytes that the asynchronous copy at line "
               "%u has not delivered yet; wait for its group first",
               buf->pending[offset + i]);
      machine->status = IR_INTERP_TRAP;
      return 0;
    }
  }
  return 1;
}

static int ii_pending_mark(IIBuffer *buf, long long offset, long long length,
                           uint32_t line) {
  if (!buf->pending) {
    if (!line) {
      return 1;
    }
    buf->pending = (uint32_t *)calloc((size_t)buf->size, sizeof(uint32_t));
    if (!buf->pending) {
      return 0;
    }
  }
  for (long long i = 0; i < length; i++) {
    buf->pending[offset + i] = line;
  }
  return 1;
}

static int ii_gpu_read(IRInterpMachine *machine, unsigned long long addr,
                       long long length, unsigned char *out) {
  long long offset = 0;
  IIBuffer *buf = ii_addr_to_buffer(machine, addr, length, &offset);
  if (!buf) {
    ii_fail(machine, IR_INTERP_TRAP,
            "a device operation read outside its buffer");
    return 0;
  }
  if (!ii_pending_check(machine, buf, offset, length)) {
    return 0;
  }
  if (g_ii_symbolic && buf->shadow && ii_sym_range(buf, offset, length)) {
    ii_misuse();
  }
  memcpy(out, buf->data + offset, (size_t)length);
  return 1;
}

static int ii_gpu_write(IRInterpMachine *machine, unsigned long long addr,
                        long long length, const unsigned char *in) {
  long long offset = 0;
  IIBuffer *buf = ii_addr_to_buffer(machine, addr, length, &offset);
  if (!buf) {
    ii_fail(machine, IR_INTERP_TRAP,
            "a device operation wrote outside its buffer");
    return 0;
  }
  memcpy(buf->data + offset, in, (size_t)length);
  ii_mark_bytes(buf, offset, length, 1);
  ii_sym_clear(buf, offset, length);
  return 1;
}

static int ii_gpu_copy(IRInterpMachine *machine, unsigned long long to,
                       unsigned long long from, long long length) {
  unsigned char stack[256];
  unsigned char *scratch =
      length <= (long long)sizeof(stack) ? stack : (unsigned char *)malloc(
                                                       (size_t)length);
  int ok;
  if (!scratch) {
    ii_fail(machine, IR_INTERP_TRAP, "out of memory copying device bytes");
    return 0;
  }
  {
    int symbolic = g_ii_symbolic;
    g_ii_symbolic = 0;
    ok = ii_gpu_read(machine, from, length, scratch) &&
         ii_gpu_write(machine, to, length, scratch);
    g_ii_symbolic = symbolic;
  }
  if (ok && machine->num) {
    ok = ii_sym_copy(machine, to, from, length);
  }
  if (scratch != stack) {
    free(scratch);
  }
  return ok;
}

static int ii_gpu_async_copy(IRInterpMachine *machine, IIFrame *frame,
                             const IRInstruction *insn) {
  IIGpuThread *thread = machine->gpu_current;
  unsigned long long to = 0, from = 0;
  long long element = insn->argument_types && insn->argument_types[0] &&
                              insn->argument_types[0]->base_type
                          ? (long long)insn->argument_types[0]->base_type->size
                          : 0;
  long long bytes = element * (long long)insn->async_copy_element_count;
  long long offset = 0;
  IIBuffer *buf;
  if (!machine->gpu.active || !thread) {
    ii_fail(machine, IR_INTERP_UNSUPPORTED,
            "an asynchronous copy outside a dispatched grid");
    return 0;
  }
  if (insn->argument_count < 2 || bytes <= 0 ||
      !ii_fetch_addr(machine, frame, &insn->arguments[0], &to) ||
      !ii_fetch_addr(machine, frame, &insn->arguments[1], &from)) {
    if (machine->status == IR_INTERP_OK) {
      ii_fail(machine, IR_INTERP_UNSUPPORTED, "malformed asynchronous copy");
    }
    return 0;
  }
  if (!ii_addr_to_buffer(machine, from, bytes, &offset)) {
    ii_fail(machine, IR_INTERP_TRAP,
            "an asynchronous copy reads outside its source buffer");
    return 0;
  }
  buf = ii_addr_to_buffer(machine, to, bytes, &offset);
  if (!buf) {
    ii_fail(machine, IR_INTERP_TRAP,
            "an asynchronous copy writes outside its workgroup buffer");
    return 0;
  }
  if (thread->copy_count == thread->copy_capacity) {
    size_t grown = thread->copy_capacity ? thread->copy_capacity * 2 : 16;
    IIAsyncCopy *table = (IIAsyncCopy *)realloc(
        thread->copies, grown * sizeof(IIAsyncCopy));
    if (!table) {
      ii_fail(machine, IR_INTERP_TRAP, "out of memory queueing a copy");
      return 0;
    }
    thread->copies = table;
    thread->copy_capacity = grown;
  }
  thread->copies[thread->copy_count].destination = to;
  thread->copies[thread->copy_count].source = from;
  thread->copies[thread->copy_count].bytes = bytes;
  thread->copies[thread->copy_count].group = thread->committed;
  thread->copies[thread->copy_count].line = insn->location.line;
  thread->copy_count++;
  if (!ii_pending_mark(buf, offset, bytes,
                       insn->location.line ? (uint32_t)insn->location.line
                                           : 1u)) {
    ii_fail(machine, IR_INTERP_TRAP, "out of memory tracking a copy");
    return 0;
  }
  return 1;
}

static int ii_gpu_async_wait(IRInterpMachine *machine, unsigned pending) {
  IIGpuThread *thread = machine->gpu_current;
  size_t kept = 0;
  if (!machine->gpu.active || !thread) {
    ii_fail(machine, IR_INTERP_UNSUPPORTED,
            "an asynchronous wait outside a dispatched grid");
    return 0;
  }
  for (size_t i = 0; i < thread->copy_count; i++) {
    IIAsyncCopy *copy = &thread->copies[i];
    if ((unsigned long long)copy->group + pending <
        (unsigned long long)thread->committed) {
      long long offset = 0;
      IIBuffer *buf =
          ii_addr_to_buffer(machine, copy->destination, copy->bytes, &offset);
      if (!buf) {
        ii_fail(machine, IR_INTERP_TRAP,
                "an asynchronous copy's destination went away");
        return 0;
      }
      ii_pending_mark(buf, offset, copy->bytes, 0);
      if (!ii_gpu_copy(machine, copy->destination, copy->source,
                       copy->bytes)) {
        return 0;
      }
      continue;
    }
    thread->copies[kept++] = *copy;
  }
  thread->copy_count = kept;
  return 1;
}

static IIMbarrier *ii_mbarrier_find(IRInterpMachine *machine,
                                    unsigned long long address, int create) {
  IIGpuGrid *gpu = &machine->gpu;
  for (size_t i = 0; i < gpu->mbarrier_count; i++) {
    if (gpu->mbarriers[i].address == address) {
      return &gpu->mbarriers[i];
    }
  }
  if (!create || gpu->mbarrier_count >= II_GPU_MAX_MBARRIERS) {
    return NULL;
  }
  memset(&gpu->mbarriers[gpu->mbarrier_count], 0, sizeof(IIMbarrier));
  gpu->mbarriers[gpu->mbarrier_count].address = address;
  return &gpu->mbarriers[gpu->mbarrier_count++];
}

static void ii_mbarrier_settle(IIMbarrier *bar) {
  if (bar->expected > 0 && bar->arrived >= bar->expected &&
      bar->transaction == 0) {
    bar->phase++;
    bar->arrived = 0;
  }
}

#define II_TENSOR_MAP_MAGIC 0x4D544D41u

typedef struct {
  unsigned magic;
  unsigned element_bytes;
  unsigned long long base;
  unsigned long long width;
  unsigned long long height;
  unsigned long long row_bytes;
  unsigned box_width;
  unsigned box_height;
  unsigned swizzle;
} IITensorMap;

static unsigned long long ii_swizzled(unsigned long long address,
                                      unsigned swizzle) {
  unsigned long long mask;
  if (!swizzle) {
    return address;
  }
  mask = (unsigned long long)(swizzle / 16u - 1u);
  return address ^ (((address >> 7) & mask) << 4);
}

static int ii_gpu_tma_load(IRInterpMachine *machine, const IRInterpValue *args) {
  unsigned long long to = (unsigned long long)ii_as_int(&args[0]);
  unsigned long long map_address = (unsigned long long)ii_as_int(&args[1]);
  long long x = ii_as_int(&args[2]);
  long long y = ii_as_int(&args[3]);
  unsigned long long bar_address = (unsigned long long)ii_as_int(&args[4]);
  IITensorMap map;
  IIMbarrier *bar;
  unsigned char element[16];
  if (!ii_gpu_read(machine, map_address, (long long)sizeof(map),
                   (unsigned char *)&map)) {
    return 0;
  }
  if (map.magic != II_TENSOR_MAP_MAGIC || map.element_bytes == 0 ||
      map.element_bytes > sizeof(element)) {
    ii_fail(machine, IR_INTERP_UNSUPPORTED,
            "a tensor-map load through a map the CPU grid runner did not "
            "build; describe it with numerics_tensor_map_2d");
    return 0;
  }
  for (unsigned row = 0; row < map.box_height; row++) {
    for (unsigned column = 0; column < map.box_width; column++) {
      long long gx = x + (long long)column;
      long long gy = y + (long long)row;
      unsigned long long local =
          ((unsigned long long)row * map.box_width + column) *
          map.element_bytes;
      unsigned long long target = ii_swizzled(to + local, map.swizzle);
      if (gx < 0 || gy < 0 || (unsigned long long)gx >= map.width ||
          (unsigned long long)gy >= map.height) {
        memset(element, 0, map.element_bytes);
        if (!ii_gpu_write(machine, target, map.element_bytes, element)) {
          return 0;
        }
        continue;
      }
      if (!ii_gpu_copy(machine, target,
                       map.base + (unsigned long long)gy * map.row_bytes +
                           (unsigned long long)gx * map.element_bytes,
                       map.element_bytes)) {
        return 0;
      }
    }
  }
  bar = ii_mbarrier_find(machine, bar_address, 1);
  if (!bar) {
    ii_fail(machine, IR_INTERP_UNSUPPORTED, "too many transaction barriers");
    return 0;
  }
  bar->transaction -= (long long)map.box_width * map.box_height *
                      (long long)map.element_bytes;
  ii_mbarrier_settle(bar);
  return 1;
}

static int ii_gpu_wait_ready(IRInterpMachine *machine, IIFrame *frame,
                             const IRInstruction *insn, int *ready) {
  IRInterpValue bar_value, parity;
  IIMbarrier *bar;
  *ready = 1;
  if (insn->argument_count < 2 ||
      !ii_fetch(machine, frame, &insn->arguments[0], &bar_value) ||
      !ii_fetch(machine, frame, &insn->arguments[1], &parity)) {
    if (machine->status == IR_INTERP_OK) {
      ii_fail(machine, IR_INTERP_UNSUPPORTED, "malformed barrier wait");
    }
    return 0;
  }
  bar = ii_mbarrier_find(machine, (unsigned long long)ii_as_int(&bar_value), 0);
  if (!bar) {
    ii_fail(machine, IR_INTERP_TRAP,
            "a wait on a transaction barrier nobody initialized");
    return 0;
  }
  *ready = (bar->phase & 1u) != ((unsigned)ii_as_int(&parity) & 1u);
  return 1;
}

static int ii_gpu_device_call(IRInterpMachine *machine,
                              MtlcIntrinsic intrinsic,
                              const IRInterpValue *args, size_t count,
                              IRInterpValue *out, int *handled) {
  IIMbarrier *bar;
  *handled = 1;
  *out = ii_int_value(0);
  if (machine->num) {
    int symbolic = 0;
    if (!ii_sym_device_call(machine, intrinsic, args, count, out,
                            &symbolic)) {
      return 0;
    }
    if (symbolic) {
      return 1;
    }
  }
  switch (intrinsic) {
  case MTLC_INTRINSIC_GPU_MBARRIER_INIT:
    if (count < 2) break;
    bar = ii_mbarrier_find(machine, (unsigned long long)ii_as_int(&args[0]), 1);
    if (!bar) {
      ii_fail(machine, IR_INTERP_UNSUPPORTED, "too many transaction barriers");
      return 0;
    }
    memset(bar, 0, sizeof(*bar));
    bar->address = (unsigned long long)ii_as_int(&args[0]);
    bar->expected = ii_as_int(&args[1]);
    return 1;
  case MTLC_INTRINSIC_GPU_MBARRIER_ARRIVE_EXPECT_TX:
    if (count < 2) break;
    bar = ii_mbarrier_find(machine, (unsigned long long)ii_as_int(&args[0]), 0);
    if (!bar) {
      ii_fail(machine, IR_INTERP_TRAP,
              "an arrival at a transaction barrier nobody initialized");
      return 0;
    }
    bar->transaction += ii_as_int(&args[1]);
    bar->arrived++;
    ii_mbarrier_settle(bar);
    return 1;
  case MTLC_INTRINSIC_GPU_MBARRIER_WAIT_PARITY:
  case MTLC_INTRINSIC_GPU_FENCE_MBARRIER_INIT:
  case MTLC_INTRINSIC_GPU_FENCE_PROXY_ASYNC:
  case MTLC_INTRINSIC_GPU_TENSORMAP_ACQUIRE:
    return 1;
  case MTLC_INTRINSIC_GPU_TMA_LOAD_2D:
    if (count < 5) break;
    return ii_gpu_tma_load(machine, args);
  case MTLC_INTRINSIC_GPU_F16_BITS_TO_F32:
  case MTLC_INTRINSIC_GPU_H2F_LO:
  case MTLC_INTRINSIC_GPU_H2F_HI: {
    unsigned long long bits;
    if (count < 1) break;
    bits = (unsigned long long)ii_as_int(&args[0]);
    if (intrinsic == MTLC_INTRINSIC_GPU_H2F_HI) bits >>= 16;
    *out = ii_float_value(
        (double)mettle_f16bits_to_f32((uint16_t)(bits & 0xFFFFu)));
    return 1;
  }
  case MTLC_INTRINSIC_GPU_F32_TO_F16_BITS: {
    float value;
    uint32_t bits;
    if (count < 1) break;
    value = (float)ii_as_float(&args[0]);
    memcpy(&bits, &value, 4);
    *out = ii_int_value((long long)mettle_f32bits_to_f16bits(bits));
    return 1;
  }
  case MTLC_INTRINSIC_GPU_F2H2: {
    float lo, hi;
    uint32_t lo_bits, hi_bits;
    if (count < 2) break;
    lo = (float)ii_as_float(&args[0]);
    hi = (float)ii_as_float(&args[1]);
    memcpy(&lo_bits, &lo, 4);
    memcpy(&hi_bits, &hi, 4);
    *out = ii_int_value(
        (long long)((uint32_t)mettle_f32bits_to_f16bits(lo_bits) |
                    ((uint32_t)mettle_f32bits_to_f16bits(hi_bits) << 16)));
    return 1;
  }
  case MTLC_INTRINSIC_GPU_F32_FROM_BITS: {
    uint32_t bits;
    float value;
    if (count < 1) break;
    bits = (uint32_t)ii_as_int(&args[0]);
    memcpy(&value, &bits, 4);
    *out = ii_float_value((double)value);
    return 1;
  }
  case MTLC_INTRINSIC_GPU_WORKGROUP_UNIFORM_U32:
    if (count < 1) break;
    *out = ii_int_value((long long)(uint32_t)ii_as_int(&args[0]));
    return 1;
  case MTLC_INTRINSIC_GPU_F32_TO_BITS: {
    float value;
    uint32_t bits;
    if (count < 1) break;
    value = (float)ii_as_float(&args[0]);
    memcpy(&bits, &value, 4);
    *out = ii_int_value((long long)bits);
    return 1;
  }
  case MTLC_INTRINSIC_GPU_LOAD4_F32:
  case MTLC_INTRINSIC_GPU_LOAD4_U32:
  case MTLC_INTRINSIC_GPU_STORE4_F32:
  case MTLC_INTRINSIC_GPU_STORE4_U32: {
    unsigned long long vector, scalar;
    int load = intrinsic == MTLC_INTRINSIC_GPU_LOAD4_F32 ||
               intrinsic == MTLC_INTRINSIC_GPU_LOAD4_U32;
    if (count < 2) break;
    vector = (unsigned long long)ii_as_int(&args[0]);
    scalar = (unsigned long long)ii_as_int(&args[1]);
    return load ? ii_gpu_copy(machine, scalar, vector, 16)
                : ii_gpu_copy(machine, vector, scalar, 16);
  }
  default:
    *handled = 0;
    return 1;
  }
  ii_fail(machine, IR_INTERP_UNSUPPORTED, "malformed device intrinsic call");
  return 0;
}

static double ii_gpu_element(const unsigned char *bytes,
                             MtlcTensorElement element) {
  uint16_t half;
  float single;
  switch (element) {
  case MTLC_TENSOR_ELEMENT_FLOAT16:
    memcpy(&half, bytes, 2);
    return (double)mettle_f16bits_to_f32(half);
  case MTLC_TENSOR_ELEMENT_BFLOAT16:
    memcpy(&half, bytes, 2);
    return (double)mettle_bf16bits_to_f32(half);
  case MTLC_TENSOR_ELEMENT_FLOAT32:
    memcpy(&single, bytes, 4);
    return (double)single;
  case MTLC_TENSOR_ELEMENT_INT8:
    return (double)(signed char)bytes[0];
  case MTLC_TENSOR_ELEMENT_UINT8:
    return (double)bytes[0];
  default:
    return 0.0;
  }
}

static int ii_gpu_element_bytes(MtlcTensorElement element) {
  switch (element) {
  case MTLC_TENSOR_ELEMENT_FLOAT16:
  case MTLC_TENSOR_ELEMENT_BFLOAT16:
    return 2;
  case MTLC_TENSOR_ELEMENT_FLOAT32:
  case MTLC_TENSOR_ELEMENT_INT32:
    return 4;
  case MTLC_TENSOR_ELEMENT_INT8:
  case MTLC_TENSOR_ELEMENT_UINT8:
    return 1;
  default:
    return 0;
  }
}

typedef struct {
  unsigned long long base;
  unsigned long long stride;
  MtlcTensorLayout layout;
  MtlcTensorElement element;
  MtlcTensorPacking packing;
  unsigned zero_point;
  unsigned swizzle;
  int transpose;
} IIMmaOperand;

static int ii_mma_operand_value(IRInterpMachine *machine,
                                const IIMmaOperand *operand,
                                unsigned long long row,
                                unsigned long long column, double *out) {
  int row_major =
      (operand->layout == MTLC_TENSOR_LAYOUT_ROW_MAJOR) != (operand->transpose != 0);
  unsigned long long position = row_major ? row * operand->stride + column
                                          : column * operand->stride + row;
  unsigned char bytes[4];
  if (operand->element == MTLC_TENSOR_ELEMENT_INT4 ||
      operand->element == MTLC_TENSOR_ELEMENT_UINT4) {
    unsigned long long byte;
    int high;
    unsigned nibble;
    if (operand->packing == MTLC_TENSOR_PACKING_HALVES) {
      byte = 16u * (position / 32u) + position % 16u;
      high = position % 32u >= 16u;
    } else {
      byte = position / 2u;
      high = (int)(position & 1u);
    }
    if (!ii_gpu_read(machine, ii_swizzled(operand->base + byte,
                                          operand->swizzle),
                     1, bytes)) {
      return 0;
    }
    nibble = high ? (unsigned)(bytes[0] >> 4) : (unsigned)(bytes[0] & 15u);
    if (operand->element == MTLC_TENSOR_ELEMENT_INT4) {
      *out = (double)((int)nibble - (nibble >= 8u ? 16 : 0));
    } else {
      *out = (double)((int)nibble - (int)operand->zero_point);
    }
    return 1;
  }
  {
    int size = ii_gpu_element_bytes(operand->element);
    unsigned long long address =
        operand->base + position * (unsigned long long)size;
    if (size <= 0) {
      ii_fail(machine, IR_INTERP_UNSUPPORTED,
              "a tensor operand format the CPU grid runner does not read");
      return 0;
    }
    if (!ii_gpu_read(machine, ii_swizzled(address, operand->swizzle), size,
                     bytes)) {
      return 0;
    }
    *out = ii_gpu_element(bytes, operand->element);
    if (operand->element == MTLC_TENSOR_ELEMENT_UINT8) {
      *out -= (double)operand->zero_point;
    }
    return 1;
  }
}

static int ii_mma_scale(IRInterpMachine *machine, unsigned long long base,
                        unsigned long long index, MtlcTensorElement element,
                        double *out) {
  unsigned char bytes[4];
  int size = ii_gpu_element_bytes(element);
  if (size <= 0) {
    ii_fail(machine, IR_INTERP_UNSUPPORTED,
            "a block scale format the CPU grid runner does not read");
    return 0;
  }
  if (!ii_gpu_read(machine, base + index * (unsigned long long)size, size,
                   bytes)) {
    return 0;
  }
  *out = ii_gpu_element(bytes, element);
  return 1;
}

static float ii_fma_f32(float a, float b, float c) {
  return mettle_fmaf_exact(a, b, c);
}

typedef struct {
  unsigned long long pointer[4];
  unsigned long long stride[4];
  unsigned long long a_scale;
  unsigned long long b_scale;
  unsigned long long c_scale;
  int c_zero;
  IIMmaOperand a;
  IIMmaOperand b;
} IIMmaTile;

static int ii_mma_fetch_tile(IRInterpMachine *machine, IIFrame *frame,
                             const IRInstruction *insn, size_t base,
                             size_t per_tile, IIMmaTile *tile) {
  const MtlcTensorMmaDesc *desc = &IR_TENSOR_MMA(insn);
  size_t index = 4;
  unsigned mask = ir_tensor_mma_runtime_stride_mask(desc);
  memset(tile, 0, sizeof(*tile));
  tile->stride[0] = desc->a_leading_dimension;
  tile->stride[1] = desc->b_leading_dimension;
  tile->stride[2] = desc->c_leading_dimension;
  tile->stride[3] = desc->d_leading_dimension;
  tile->c_zero = ir_tensor_c_is_zero(&insn->arguments[base + 2]);
  for (int i = 0; i < 4; i++) {
    if (i == 2 && tile->c_zero) continue;
    if (!ii_fetch_addr(machine, frame, &insn->arguments[base + i],
                       &tile->pointer[i])) {
      return 0;
    }
  }
  if (desc->sparsity != MTLC_TENSOR_SPARSITY_DENSE) index++;
  if (desc->a_scale_mode != MTLC_TENSOR_SCALE_NONE &&
      !ii_fetch_addr(machine, frame, &insn->arguments[base + index++],
                     &tile->a_scale)) {
    return 0;
  }
  if (desc->b_scale_mode != MTLC_TENSOR_SCALE_NONE &&
      !ii_fetch_addr(machine, frame, &insn->arguments[base + index++],
                     &tile->b_scale)) {
    return 0;
  }
  for (int i = 0; i < 4; i++) {
    long long value = 0;
    if (!(mask & (1u << i))) continue;
    if (!ii_fetch_int(machine, frame, &insn->arguments[base + index++],
                      &value)) {
      return 0;
    }
    tile->stride[i] = (unsigned long long)value;
  }
  if (desc->c_scale_mode == MTLC_TENSOR_SCALE_PER_ROW &&
      !ii_fetch_addr(machine, frame, &insn->arguments[base + per_tile - 1],
                     &tile->c_scale)) {
    return 0;
  }
  tile->a.base = tile->pointer[0];
  tile->a.stride = tile->stride[0];
  tile->a.layout = desc->a_layout;
  tile->a.element = desc->a_element;
  tile->a.packing = desc->a_packing;
  tile->a.zero_point = desc->a_zero_point;
  tile->a.swizzle = desc->a_swizzle;
  tile->a.transpose = desc->transpose_a;
  tile->b.base = tile->pointer[1];
  tile->b.stride = tile->stride[1];
  tile->b.layout = desc->b_layout;
  tile->b.element = desc->b_element;
  tile->b.packing = desc->b_packing;
  tile->b.zero_point = desc->b_zero_point;
  tile->b.swizzle = desc->b_swizzle;
  tile->b.transpose = desc->transpose_b;
  return 1;
}

static int ii_sym_read_at(IRInterpMachine *machine, unsigned long long addr,
                          int size, NumTerm *out) {
  long long offset = 0;
  IIBuffer *buf = ii_addr_to_buffer(machine, addr, size, &offset);
  if (!buf) {
    ii_fail(machine, IR_INTERP_TRAP,
            "a device operation read outside its buffer");
    return 0;
  }
  if (!ii_pending_check(machine, buf, offset, size)) {
    return 0;
  }
  *out = ii_sym_read_bytes(machine, buf, offset, size);
  return *out != 0;
}

static int ii_mma_operand_term(IRInterpMachine *machine,
                               const IIMmaOperand *operand,
                               unsigned long long row,
                               unsigned long long column, NumTerm *out) {
  int row_major =
      (operand->layout == MTLC_TENSOR_LAYOUT_ROW_MAJOR) != (operand->transpose != 0);
  unsigned long long position = row_major ? row * operand->stride + column
                                          : column * operand->stride + row;
  if (operand->element == MTLC_TENSOR_ELEMENT_INT4 ||
      operand->element == MTLC_TENSOR_ELEMENT_UINT4) {
    unsigned long long byte;
    int high;
    NumTerm raw;
    uint64_t imm;
    if (operand->packing == MTLC_TENSOR_PACKING_HALVES) {
      byte = 16u * (position / 32u) + position % 16u;
      high = position % 32u >= 16u;
    } else {
      byte = position / 2u;
      high = (int)(position & 1u);
    }
    if (!ii_sym_read_at(machine,
                        ii_swizzled(operand->base + byte, operand->swizzle), 1,
                        &raw)) {
      return 0;
    }
    imm = (uint64_t)high |
          (operand->element == MTLC_TENSOR_ELEMENT_INT4 ? 2u : 0u) |
          ((uint64_t)operand->zero_point << 8);
    *out = num_op(machine->num, NUM_NIBBLE, 8, imm, &raw, 1);
    return *out != 0;
  }
  if (operand->element == MTLC_TENSOR_ELEMENT_UINT8) {
    ii_fail(machine, IR_INTERP_UNSUPPORTED,
            "an unsigned 8-bit tensor operand under the numerics check");
    return 0;
  }
  {
    int size = ii_gpu_element_bytes(operand->element);
    unsigned long long address =
        operand->base + position * (unsigned long long)size;
    if (size <= 0) {
      ii_fail(machine, IR_INTERP_UNSUPPORTED,
              "a tensor operand format the numerics check does not read");
      return 0;
    }
    return ii_sym_read_at(machine, ii_swizzled(address, operand->swizzle),
                          size, out);
  }
}

static int ii_mma_scale_term(IRInterpMachine *machine, unsigned long long base,
                             unsigned long long index,
                             MtlcTensorElement element, NumTerm *out) {
  int size = ii_gpu_element_bytes(element);
  NumTerm raw;
  if (size != 2 && size != 4) {
    ii_fail(machine, IR_INTERP_UNSUPPORTED,
            "a block scale format the numerics check does not read");
    return 0;
  }
  if (!ii_sym_read_at(machine, base + index * (unsigned long long)size, size,
                      &raw)) {
    return 0;
  }
  if (size == 2) {
    raw = num_op(machine->num, NUM_CVT, 32,
                 element == MTLC_TENSOR_ELEMENT_BFLOAT16 ? NUM_CVT_BF16_TO_F32
                                                         : NUM_CVT_F16_TO_F32,
                 &raw, 1);
  }
  *out = raw;
  return raw != 0;
}

static int ii_gpu_tensor_mma_symbolic(IRInterpMachine *machine,
                                      IIFrame *frame,
                                      const IRInstruction *insn, int f16) {
  const MtlcTensorMmaDesc *desc = &IR_TENSOR_MMA(insn);
  size_t per_tile = ir_tensor_mma_operand_count(desc);
  size_t tiles = ir_tensor_mma_instruction_count(insn);
  NumTerm *result = (NumTerm *)malloc((size_t)desc->m * desc->n *
                                      sizeof(NumTerm));
  NumTerm args[65];
  if (!result) {
    ii_fail(machine, IR_INTERP_TRAP, "out of memory running a tensor_mma");
    return 0;
  }
  for (size_t tile_index = 0; tile_index < tiles; tile_index++) {
    IIMmaTile t;
    unsigned blocks = desc->k / 32u;
    unsigned a_ld = desc->a_scale_leading_dimension
                        ? desc->a_scale_leading_dimension
                        : blocks;
    unsigned b_ld = desc->b_scale_leading_dimension
                        ? desc->b_scale_leading_dimension
                        : blocks;
    IRBlockScaleOrder order = ir_tensor_block_scale_order(desc);
    if (!ii_mma_fetch_tile(machine, frame, insn, tile_index * per_tile,
                           per_tile, &t)) {
      free(result);
      return 0;
    }
    for (unsigned r = 0; r < desc->m; r++) {
      NumTerm row_scale = 0;
      if (t.c_scale && !ii_sym_read_at(machine, t.c_scale + 4ull * r, 4,
                                       &row_scale)) {
        free(result);
        return 0;
      }
      for (unsigned c = 0; c < desc->n; c++) {
        NumTerm acc = num_const(machine->num, 32, 0);
        if (!t.c_zero) {
          unsigned long long position =
              desc->c_layout == MTLC_TENSOR_LAYOUT_ROW_MAJOR
                  ? (unsigned long long)r * t.stride[2] + c
                  : (unsigned long long)c * t.stride[2] + r;
          if (!ii_sym_read_at(machine, t.pointer[2] + position * 4u, 4,
                              &acc)) {
            free(result);
            return 0;
          }
        }
        if (row_scale) {
          NumTerm pair[2];
          pair[0] = acc;
          pair[1] = row_scale;
          acc = num_op(machine->num, NUM_FMUL, 32, 0, pair, 2);
        }
        if (f16) {
          for (unsigned ks = 0; ks < desc->k; ks += 16) {
            args[0] = acc;
            for (unsigned j = 0; j < 16; j++) {
              if (!ii_mma_operand_term(machine, &t.a, r, ks + j,
                                       &args[1 + j]) ||
                  !ii_mma_operand_term(machine, &t.b, ks + j, c,
                                       &args[17 + j])) {
                free(result);
                return 0;
              }
            }
            acc = num_op(machine->num, NUM_MMA, 32,
                         (uint64_t)desc->a_element, args, 33);
          }
        } else {
          for (unsigned kb = 0; kb < blocks; kb++) {
            NumTerm dot, sa, sb, scaled, triple[3];
            for (unsigned j = 0; j < 32; j++) {
              if (!ii_mma_operand_term(machine, &t.a, r, kb * 32u + j,
                                       &args[j]) ||
                  !ii_mma_operand_term(machine, &t.b, kb * 32u + j, c,
                                       &args[32 + j])) {
                free(result);
                return 0;
              }
            }
            dot = num_op(machine->num, NUM_IDOT, 32, 0, args, 64);
            if (!ii_mma_scale_term(machine, t.a_scale,
                                   (unsigned long long)r * a_ld + kb,
                                   desc->a_scale_element, &sa) ||
                !ii_mma_scale_term(machine, t.b_scale,
                                   (unsigned long long)c * b_ld + kb,
                                   desc->b_scale_element, &sb)) {
              free(result);
              return 0;
            }
            if (order == IR_BLOCK_SCALE_PRODUCT) {
              NumTerm pair[2], wide;
              wide = num_resize(machine->num, dot, 64, 1);
              triple[0] = num_op(machine->num, NUM_CVT, 32, NUM_CVT_S64_TO_F32,
                                 &wide, 1);
              pair[0] = sa;
              pair[1] = sb;
              triple[1] = num_op(machine->num, NUM_FMUL, 32, 0, pair, 2);
            } else {
              NumTerm pair[2];
              pair[0] = dot;
              pair[1] = order == IR_BLOCK_SCALE_B_FIRST ? sb : sa;
              scaled = num_op(machine->num, NUM_BSCALE, 32, 0, pair, 2);
              triple[0] = scaled;
              triple[1] = order == IR_BLOCK_SCALE_B_FIRST ? sa : sb;
            }
            triple[2] = acc;
            acc = num_op(machine->num, NUM_FFMA, 32, 0, triple, 3);
          }
        }
        if (!acc) {
          free(result);
          ii_fail(machine, IR_INTERP_TRAP, "the numerics check ran out of memory");
          return 0;
        }
        result[(size_t)r * desc->n + c] = acc;
      }
    }
    for (unsigned r = 0; r < desc->m; r++) {
      for (unsigned c = 0; c < desc->n; c++) {
        unsigned long long position =
            desc->d_layout == MTLC_TENSOR_LAYOUT_ROW_MAJOR
                ? (unsigned long long)r * t.stride[3] + c
                : (unsigned long long)c * t.stride[3] + r;
        long long offset = 0;
        IIBuffer *buf = ii_addr_to_buffer(machine, t.pointer[3] + position * 4u,
                                          4, &offset);
        if (!buf) {
          free(result);
          ii_fail(machine, IR_INTERP_TRAP,
                  "a tensor_mma wrote outside its D buffer");
          return 0;
        }
        if (!ii_sym_write_bytes(machine, buf, offset, 4,
                                result[(size_t)r * desc->n + c])) {
          free(result);
          return 0;
        }
      }
    }
  }
  free(result);
  return 1;
}

static int ii_gpu_tensor_scale_rows(IRInterpMachine *machine,
                                    IIFrame *frame,
                                    const IRInstruction *insn) {
  const MtlcTensorMmaDesc *desc = &IR_TENSOR_MMA(insn);
  unsigned long long d = 0, scale = 0;
  unsigned long long ld = desc->d_leading_dimension;
  int row_major = desc->d_layout == MTLC_TENSOR_LAYOUT_ROW_MAJOR;
  if (machine->num || desc->accumulator_element != MTLC_TENSOR_ELEMENT_FLOAT32 ||
      insn->argument_count != ir_tensor_scale_rows_operand_count(desc)) {
    ii_fail(machine, IR_INTERP_UNSUPPORTED,
            "a tensor_scale_rows the CPU grid runner does not model: it "
            "scales f32 accumulators");
    return 0;
  }
  if (!ii_fetch_addr(machine, frame, &insn->arguments[0], &d) ||
      !ii_fetch_addr(machine, frame, &insn->arguments[1], &scale)) {
    return 0;
  }
  if (insn->argument_count > 2) {
    long long value = 0;
    if (!ii_fetch_int(machine, frame, &insn->arguments[2], &value)) {
      return 0;
    }
    ld = (unsigned long long)value;
  }
  for (unsigned r = 0; r < desc->m; r++) {
    double factor = 1.0;
    if (!ii_mma_scale(machine, scale, r, MTLC_TENSOR_ELEMENT_FLOAT32,
                      &factor)) {
      return 0;
    }
    for (unsigned c = 0; c < desc->n; c++) {
      unsigned long long at =
          d + 4u * (row_major ? (unsigned long long)r * ld + c
                              : (unsigned long long)c * ld + r);
      unsigned char bytes[4];
      float value;
      if (!ii_gpu_read(machine, at, 4, bytes)) {
        return 0;
      }
      memcpy(&value, bytes, 4);
      value = value * (float)factor;
      memcpy(bytes, &value, 4);
      if (!ii_gpu_write(machine, at, 4, bytes)) {
        return 0;
      }
    }
  }
  return 1;
}

static int ii_gpu_tensor_mma(IRInterpMachine *machine, IIFrame *frame,
                             const IRInstruction *insn) {
  const MtlcTensorMmaDesc *desc = &IR_TENSOR_MMA(insn);
  size_t per_tile = ir_tensor_mma_operand_count(desc);
  size_t tiles = ir_tensor_mma_instruction_count(insn);
  int f16 = (desc->a_element == MTLC_TENSOR_ELEMENT_FLOAT16 ||
             desc->a_element == MTLC_TENSOR_ELEMENT_BFLOAT16) &&
            desc->b_element == desc->a_element &&
            desc->a_scale_mode == MTLC_TENSOR_SCALE_NONE &&
            desc->b_scale_mode == MTLC_TENSOR_SCALE_NONE;
  int scaled = (desc->a_element == MTLC_TENSOR_ELEMENT_INT8 ||
                desc->a_element == MTLC_TENSOR_ELEMENT_UINT8) &&
               (desc->b_element == MTLC_TENSOR_ELEMENT_INT8 ||
                desc->b_element == MTLC_TENSOR_ELEMENT_UINT8 ||
                desc->b_element == MTLC_TENSOR_ELEMENT_INT4 ||
                desc->b_element == MTLC_TENSOR_ELEMENT_UINT4) &&
               desc->a_scale_mode == MTLC_TENSOR_SCALE_BLOCK_32 &&
               desc->b_scale_mode == MTLC_TENSOR_SCALE_BLOCK_32;
  float *result = NULL;
  if (machine->num) {
    return ii_gpu_tensor_mma_symbolic(machine, frame, insn, f16);
  }
  if (!per_tile || insn->argument_count != per_tile * tiles ||
      desc->sparsity != MTLC_TENSOR_SPARSITY_DENSE ||
      desc->math_mode != MTLC_TENSOR_MATH_MULTIPLY_ADD ||
      desc->accumulator_element != MTLC_TENSOR_ELEMENT_FLOAT32 ||
      desc->result_element != MTLC_TENSOR_ELEMENT_FLOAT32 ||
      (!f16 && !scaled) || (f16 && desc->k % 16) || (scaled && desc->k % 32)) {
    ii_fail(machine, IR_INTERP_UNSUPPORTED,
            "a tensor_mma the CPU grid runner does not model: it runs dense "
            "f16/bf16 with f32 accumulators and block-scaled int8");
    return 0;
  }
  result = (float *)malloc((size_t)desc->m * desc->n * sizeof(float));
  if (!result) {
    ii_fail(machine, IR_INTERP_TRAP, "out of memory running a tensor_mma");
    return 0;
  }
  for (size_t tile = 0; tile < tiles; tile++) {
    size_t base = tile * per_tile;
    IIMmaTile t;
    if (!ii_mma_fetch_tile(machine, frame, insn, base, per_tile, &t)) {
      free(result);
      return 0;
    }
    for (unsigned r = 0; r < desc->m; r++) {
      double row_scale = 1.0;
      if (t.c_scale && !ii_mma_scale(machine, t.c_scale, r,
                                   MTLC_TENSOR_ELEMENT_FLOAT32, &row_scale)) {
        free(result);
        return 0;
      }
      for (unsigned c = 0; c < desc->n; c++) {
        float acc = 0.0f;
        if (!t.c_zero) {
          unsigned char bytes[4];
          unsigned long long position =
              desc->c_layout == MTLC_TENSOR_LAYOUT_ROW_MAJOR
                  ? (unsigned long long)r * t.stride[2] + c
                  : (unsigned long long)c * t.stride[2] + r;
          if (!ii_gpu_read(machine, t.pointer[2] + position * 4u, 4, bytes)) {
            free(result);
            return 0;
          }
          memcpy(&acc, bytes, 4);
        }
        if (t.c_scale) {
          acc = acc * (float)row_scale;
        }
        if (f16) {
          for (unsigned ks = 0; ks < desc->k; ks += 16) {
            double sum = (double)acc;
            for (unsigned j = 0; j < 16; j++) {
              double x = 0.0, y = 0.0;
              if (!ii_mma_operand_value(machine, &t.a, r, ks + j, &x) ||
                  !ii_mma_operand_value(machine, &t.b, ks + j, c, &y)) {
                free(result);
                return 0;
              }
              sum += x * y;
            }
            acc = (float)sum;
          }
        } else {
          IRBlockScaleOrder order = ir_tensor_block_scale_order(desc);
          unsigned blocks = desc->k / 32u;
          unsigned a_ld = desc->a_scale_leading_dimension
                              ? desc->a_scale_leading_dimension
                              : blocks;
          unsigned b_ld = desc->b_scale_leading_dimension
                              ? desc->b_scale_leading_dimension
                              : blocks;
          for (unsigned kb = 0; kb < blocks; kb++) {
            long long dot = 0;
            double sa = 0.0, sb = 0.0;
            float f;
            for (unsigned j = 0; j < 32; j++) {
              double x = 0.0, y = 0.0;
              if (!ii_mma_operand_value(machine, &t.a, r, kb * 32u + j, &x) ||
                  !ii_mma_operand_value(machine, &t.b, kb * 32u + j, c, &y)) {
                free(result);
                return 0;
              }
              dot += (long long)x * (long long)y;
            }
            if (!ii_mma_scale(machine, t.a_scale,
                              (unsigned long long)r * a_ld + kb,
                              desc->a_scale_element, &sa) ||
                !ii_mma_scale(machine, t.b_scale,
                              (unsigned long long)c * b_ld + kb,
                              desc->b_scale_element, &sb)) {
              free(result);
              return 0;
            }
            f = 12582912.0f + (float)dot;
            if (order == IR_BLOCK_SCALE_B_FIRST) {
              float t = ii_fma_f32(f, (float)sb, -(12582912.0f * (float)sb));
              acc = ii_fma_f32(t, (float)sa, acc);
            } else if (order == IR_BLOCK_SCALE_A_FIRST) {
              float t = ii_fma_f32(f, (float)sa, -(12582912.0f * (float)sa));
              acc = ii_fma_f32(t, (float)sb, acc);
            } else {
              acc = ii_fma_f32(f - 12582912.0f, (float)sa * (float)sb, acc);
            }
          }
        }
        result[(size_t)r * desc->n + c] = acc;
      }
    }
    for (unsigned r = 0; r < desc->m; r++) {
      for (unsigned c = 0; c < desc->n; c++) {
        unsigned long long position =
            desc->d_layout == MTLC_TENSOR_LAYOUT_ROW_MAJOR
                ? (unsigned long long)r * t.stride[3] + c
                : (unsigned long long)c * t.stride[3] + r;
        if (!ii_gpu_write(machine, t.pointer[3] + position * 4u, 4,
                          (const unsigned char *)&result[(size_t)r * desc->n +
                                                         c])) {
          free(result);
          return 0;
        }
      }
    }
  }
  free(result);
  return 1;
}

static IRInterpValue ii_subgroup_combine(IRInterpMachine *machine,
                                         MtlcIntrinsic intrinsic,
                                         const IRInterpValue *a,
                                         const IRInterpValue *b) {
  if (machine->num && (a->term || b->term)) {
    NumTerm args[2];
    NumOp op;
    int is_float = 1;
    switch (intrinsic) {
    case MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_ADD_F32:
    case MTLC_INTRINSIC_GPU_SUBGROUP_SCAN_INCLUSIVE_ADD_F32:
    case MTLC_INTRINSIC_GPU_SUBGROUP_SCAN_EXCLUSIVE_ADD_F32:
      op = NUM_FADD;
      break;
    case MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_MIN_F32:
      op = NUM_FMIN;
      break;
    case MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_MAX_F32:
      op = NUM_FMAX;
      break;
    default:
      is_float = 0;
      op = NUM_ADD;
      break;
    }
    if (!is_float && intrinsic != MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_ADD_U32 &&
        intrinsic != MTLC_INTRINSIC_GPU_SUBGROUP_SCAN_INCLUSIVE_ADD_U32 &&
        intrinsic != MTLC_INTRINSIC_GPU_SUBGROUP_SCAN_EXCLUSIVE_ADD_U32) {
      ii_misuse();
      return ii_int_value(0);
    }
    if (is_float) {
      args[0] = ii_sym_term(machine, a, 32, 1);
      args[1] = ii_sym_term(machine, b, 32, 1);
      return ii_sym_value(machine, num_op(machine->num, op, 32, 0, args, 2), 1);
    }
    args[0] = num_resize(machine->num, ii_sym_term(machine, a, 64, 0), 32, 0);
    args[1] = num_resize(machine->num, ii_sym_term(machine, b, 64, 0), 32, 0);
    return ii_sym_value(
        machine,
        num_resize(machine->num, num_op(machine->num, NUM_ADD, 32, 0, args, 2),
                   64, 0),
        0);
  }
  switch (intrinsic) {
  case MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_ADD_F32:
  case MTLC_INTRINSIC_GPU_SUBGROUP_SCAN_INCLUSIVE_ADD_F32:
  case MTLC_INTRINSIC_GPU_SUBGROUP_SCAN_EXCLUSIVE_ADD_F32:
    return ii_float_value(
        (double)((float)ii_as_float(a) + (float)ii_as_float(b)));
  case MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_MIN_F32:
  case MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_MAX_F32: {
    float x = (float)ii_as_float(a), y = (float)ii_as_float(b);
    int want_max = intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_MAX_F32;
    if (x != x) return ii_float_value((double)y);
    if (y != y) return ii_float_value((double)x);
    if (x == y) {
      int x_negative = signbit(x) != 0;
      return ii_float_value((double)((x_negative == want_max) ? y : x));
    }
    return ii_float_value((double)((x > y) == want_max ? x : y));
  }
  case MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_MIN_U32:
  case MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_MAX_U32: {
    uint32_t x = (uint32_t)ii_as_int(a), y = (uint32_t)ii_as_int(b);
    int want_max = intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_MAX_U32;
    return ii_int_value((long long)((x > y) == want_max ? x : y));
  }
  default:
    return ii_int_value(
        (long long)(uint32_t)((uint32_t)ii_as_int(a) + (uint32_t)ii_as_int(b)));
  }
}

static int ii_gpu_subgroup_collective(IRInterpMachine *machine,
                                      IIGpuThread *threads, long long first,
                                      long long lanes,
                                      const IRInstruction *insn) {
  IRInterpValue values[II_GPU_SUBGROUP];
  IRInterpValue sources[II_GPU_SUBGROUP];
  IRInterpValue results[II_GPU_SUBGROUP];
  IRInterpValue round[II_GPU_SUBGROUP];
  MtlcIntrinsic op = insn->intrinsic;
  int is_float = ir_intrinsic_subgroup_result_kind(op) == MTLC_TYPE_FLOAT32;
  for (long long l = 0; l < lanes; l++) {
    IIFrame *frame = (IIFrame *)threads[first + l].frame;
    machine->gpu.lane = first + l;
    if (insn->argument_count < 1 ||
        !ii_fetch(machine, frame, &insn->arguments[0], &values[l])) {
      if (machine->status == IR_INTERP_OK) {
        ii_fail(machine, IR_INTERP_UNSUPPORTED, "malformed subgroup operation");
      }
      return 0;
    }
    if (is_float && !values[l].is_float) {
      values[l] = ii_float_value((double)values[l].i);
    }
    sources[l] = ii_int_value(0);
    if (insn->argument_count > 1 &&
        !ii_fetch(machine, frame, &insn->arguments[1], &sources[l])) {
      return 0;
    }
  }
  switch (op) {
  case MTLC_INTRINSIC_GPU_SUBGROUP_SHUFFLE_U32:
  case MTLC_INTRINSIC_GPU_SUBGROUP_SHUFFLE_F32:
  case MTLC_INTRINSIC_GPU_SUBGROUP_BROADCAST_U32:
  case MTLC_INTRINSIC_GPU_SUBGROUP_BROADCAST_F32:
    for (long long l = 0; l < lanes; l++) {
      long long source = ii_as_int(&sources[l]) & (II_GPU_SUBGROUP - 1);
      results[l] = source < lanes ? values[source] : values[l];
    }
    break;
  case MTLC_INTRINSIC_GPU_SUBGROUP_ANY:
  case MTLC_INTRINSIC_GPU_SUBGROUP_ALL:
  case MTLC_INTRINSIC_GPU_SUBGROUP_BALLOT_WORD: {
    int any = 0, all = 1;
    unsigned long long word = 0;
    for (long long o = 0; o < lanes; o++) {
      int set = values[o].is_float ? values[o].f != 0.0 : values[o].i != 0;
      if (values[o].term && machine->num) {
        ii_misuse();
      }
      any |= set;
      all &= set;
      if (set) word |= 1ULL << o;
    }
    for (long long l = 0; l < lanes; l++) {
      results[l] = ii_int_value(
          op == MTLC_INTRINSIC_GPU_SUBGROUP_ANY   ? any
          : op == MTLC_INTRINSIC_GPU_SUBGROUP_ALL ? all
                                                  : (long long)word);
    }
    break;
  }
  case MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_ADD_U32:
  case MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_ADD_F32:
  case MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_MIN_U32:
  case MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_MIN_F32:
  case MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_MAX_U32:
  case MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_MAX_F32:
    for (long long l = 0; l < lanes; l++) results[l] = values[l];
    for (long long offset = 16; offset >= 1; offset /= 2) {
      for (long long l = 0; l < lanes; l++) round[l] = results[l];
      for (long long l = 0; l < lanes; l++) {
        if (l + offset < lanes) {
          results[l] = ii_subgroup_combine(machine, op, &round[l], &round[l + offset]);
        }
      }
    }
    for (long long l = 1; l < lanes; l++) results[l] = results[0];
    break;
  case MTLC_INTRINSIC_GPU_SUBGROUP_SCAN_INCLUSIVE_ADD_U32:
  case MTLC_INTRINSIC_GPU_SUBGROUP_SCAN_INCLUSIVE_ADD_F32:
  case MTLC_INTRINSIC_GPU_SUBGROUP_SCAN_EXCLUSIVE_ADD_U32:
  case MTLC_INTRINSIC_GPU_SUBGROUP_SCAN_EXCLUSIVE_ADD_F32:
    for (long long l = 0; l < lanes; l++) results[l] = values[l];
    for (long long offset = 1; offset <= 16; offset *= 2) {
      for (long long l = 0; l < lanes; l++) round[l] = results[l];
      for (long long l = 0; l < lanes; l++) {
        if (l >= offset) {
          results[l] = ii_subgroup_combine(machine, op, &round[l], &round[l - offset]);
        }
      }
    }
    if (op == MTLC_INTRINSIC_GPU_SUBGROUP_SCAN_EXCLUSIVE_ADD_U32 ||
        op == MTLC_INTRINSIC_GPU_SUBGROUP_SCAN_EXCLUSIVE_ADD_F32) {
      for (long long l = 0; l < lanes; l++) round[l] = results[l];
      for (long long l = 0; l < lanes; l++) {
        results[l] = l >= 1 ? round[l - 1]
                     : is_float ? ii_float_value(0.0)
                                : ii_int_value(0);
      }
    }
    break;
  default:
    ii_fail(machine, IR_INTERP_UNSUPPORTED,
            "a subgroup operation the CPU grid runner does not model");
    return 0;
  }
  for (long long l = 0; l < lanes; l++) {
    IIFrame *frame = (IIFrame *)threads[first + l].frame;
    IRInterpValue value = results[l];
    if (is_float && !value.is_float) {
      value = ii_float_value((double)value.i);
    }
    machine->gpu.lane = first + l;
    if (!ii_store_dest(machine, frame, &insn->dest, &value)) {
      return 0;
    }
  }
  return 1;
}

static void ii_gpu_set_thread(IRInterpMachine *machine, long long t) {
  IIGpuGrid *gpu = &machine->gpu;
  gpu->tid[0] = t % gpu->block[0];
  gpu->tid[1] = (t / gpu->block[0]) % gpu->block[1];
  gpu->tid[2] = t / (gpu->block[0] * gpu->block[1]);
  gpu->lane = t;
}

static int ii_gpu_run_collective(IRInterpMachine *machine,
                                 IIGpuThread *threads, long long first,
                                 long long lanes) {
  const IRInstruction *insn = threads[first].site;
  int ok = 1;
  if (insn->op == IR_OP_TENSOR_MMA || insn->op == IR_OP_TENSOR_SCALE_ROWS) {
    ii_gpu_set_thread(machine, first);
    machine->gpu_current = &threads[first];
    ok = insn->op == IR_OP_TENSOR_MMA
             ? ii_gpu_tensor_mma(machine, (IIFrame *)threads[first].frame,
                                 insn)
             : ii_gpu_tensor_scale_rows(
                   machine, (IIFrame *)threads[first].frame, insn);
    machine->gpu_current = NULL;
  } else if (insn->op == IR_OP_CALL) {
    ok = ii_gpu_subgroup_collective(machine, threads, first, lanes, insn);
  }
  for (long long l = 0; l < lanes; l++) {
    threads[first + l].stop = II_STOP_NONE;
  }
  return ok;
}

static void ii_lane_save(IRInterpMachine *machine, IIGpuThread *thread) {
  IIGpuGrid *gpu = &machine->gpu;
  size_t extra = machine->effect_frame_count > gpu->base_effects
                     ? machine->effect_frame_count - gpu->base_effects
                     : 0;
  thread->depth = machine->depth;
  thread->pure_depth = machine->pure_depth;
  thread->pure_fn = machine->pure_fn;
  thread->pure_declared = machine->pure_declared;
  free(thread->effects);
  thread->effects = NULL;
  thread->effect_count = 0;
  if (extra) {
    size_t bytes = extra * sizeof(*machine->effect_frames);
    thread->effects = malloc(bytes);
    if (thread->effects) {
      memcpy(thread->effects, &machine->effect_frames[gpu->base_effects],
             bytes);
      thread->effect_count = extra;
    }
  }
  machine->depth = gpu->base_depth;
  machine->pure_depth = gpu->base_pure_depth;
  machine->pure_fn = gpu->base_pure_fn;
  machine->pure_declared = gpu->base_pure_declared;
  machine->effect_frame_count = gpu->base_effects;
}

static void ii_lane_restore(IRInterpMachine *machine, IIGpuThread *thread) {
  IIGpuGrid *gpu = &machine->gpu;
  machine->depth = thread->depth;
  machine->pure_depth = thread->pure_depth;
  machine->pure_fn = thread->pure_fn;
  machine->pure_declared = thread->pure_declared;
  if (thread->effect_count) {
    size_t need = gpu->base_effects + thread->effect_count;
    if (need > machine->effect_frame_capacity) {
      void *grown = realloc(machine->effect_frames,
                            need * sizeof(*machine->effect_frames));
      if (!grown) {
        ii_fail(machine, IR_INTERP_TRAP, "out of memory resuming a work item");
        return;
      }
      machine->effect_frames = grown;
      machine->effect_frame_capacity = need;
    }
    memcpy(&machine->effect_frames[gpu->base_effects], thread->effects,
           thread->effect_count * sizeof(*machine->effect_frames));
    machine->effect_frame_count = need;
  }
  free(thread->effects);
  thread->effects = NULL;
  thread->effect_count = 0;
}

static int ii_gpu_stop(IRInterpMachine *machine, IIFrame *frame, size_t pc,
                       const IRInstruction *insn, IIStopKind kind) {
  IIGpuThread *thread = machine->gpu_current;
  if (!thread || !thread->fiber) {
    ii_fail(machine, IR_INTERP_UNSUPPORTED,
            "a barrier or subgroup operation outside a work item");
    return 0;
  }
  thread->stop = kind;
  thread->frame = frame;
  thread->pc = pc;
  thread->site = insn;
  thread->barrier_line = insn->location.line;
  thread->suspended = 1;
  ii_lane_save(machine, thread);
  ir_fiber_yield(thread->fiber);
  ii_lane_restore(machine, thread);
  thread->suspended = 0;
  return machine->status == IR_INTERP_OK;
}

static void ii_lane_entry(void *argument) {
  IIGpuThread *thread = (IIGpuThread *)argument;
  IRInterpMachine *machine = thread->machine;
  IRInterpValue result = ii_int_value(0);
  thread->ok = ii_exec_function(machine, machine->gpu.kernel,
                                machine->gpu.args, machine->gpu.nargs,
                                &result);
  thread->finished = 1;
  thread->stop = II_STOP_NONE;
  ii_lane_save(machine, thread);
  for (;;) {
    ir_fiber_yield(thread->fiber);
  }
}

static int ii_gpu_resume(IRInterpMachine *machine, IIGpuThread *thread,
                         long long t) {
  ii_gpu_set_thread(machine, t);
  thread->stop = II_STOP_NONE;
  if (!thread->fiber) {
    thread->machine = machine;
    thread->fiber = ir_fiber_create(machine->gpu.scheduler, 1u << 20,
                                    ii_lane_entry, thread);
    if (!thread->fiber) {
      ii_fail(machine, IR_INTERP_TRAP, "could not start a work item");
      return 0;
    }
  }
  machine->gpu_current = thread;
  ir_fiber_run(thread->fiber);
  machine->gpu_current = NULL;
  if (thread->finished && !thread->ok) {
    if (machine->status == IR_INTERP_OK) {
      ii_fail(machine, IR_INTERP_TRAP, "a work item failed");
    }
    return 0;
  }
  return machine->status == IR_INTERP_OK;
}

static void ii_gpu_new_phase(IIGpuGrid *gpu, IIGpuThread *threads,
                             long long count) {
  for (size_t w = 0; gpu->tile_traces && w < gpu->tile_trace_warps; w++) {
    gpu->tile_traces[w].count = 0;
  }
  for (long long t = 0; t < count; t++) {
    threads[t].tile_cursor = 0;
  }
}

static int ii_gpu_run_warp(IRInterpMachine *machine, IIGpuThread *threads,
                           long long first, long long lanes, int *progress) {
  int ran = 1;
  while (ran) {
    long long gathered = 0;
    const IRInstruction *site = NULL;
    int same = 1, all_here = 1;
    ran = 0;
    for (long long l = 0; l < lanes; l++) {
      IIGpuThread *thread = &threads[first + l];
      if (thread->finished || thread->stop == II_STOP_BARRIER ||
          thread->stop == II_STOP_COLLECTIVE) {
        continue;
      }
      if (thread->stop == II_STOP_WAIT) {
        int ready = 0;
        ii_gpu_set_thread(machine, first + l);
        if (!ii_gpu_wait_ready(machine, (IIFrame *)thread->frame,
                               thread->site, &ready)) {
          return 0;
        }
        if (!ready) {
          continue;
        }
      }
      if (!ii_gpu_resume(machine, thread, first + l)) {
        return 0;
      }
      ran = 1;
      *progress = 1;
    }
    for (long long l = 0; l < lanes; l++) {
      IIGpuThread *thread = &threads[first + l];
      if (thread->stop == II_STOP_COLLECTIVE && !thread->finished) {
        if (!gathered) {
          site = thread->site;
        } else if (thread->site != site) {
          same = 0;
        }
        gathered++;
      } else {
        all_here = 0;
      }
    }
    if (gathered && same && all_here) {
      if (!ii_gpu_run_collective(machine, threads, first, lanes)) {
        return 0;
      }
      ran = 1;
      *progress = 1;
    }
  }
  return 1;
}

static int ii_gpu_run_block(IRInterpMachine *machine, IRFunction *kernel,
                            const IRInterpValue *args, size_t nargs) {
  IIGpuGrid *gpu = &machine->gpu;
  long long count = gpu->threads_per_block;
  IIGpuThread *threads = NULL;
  int ok = 1;

  if (count <= 0) {
    return 1;
  }
  threads = (IIGpuThread *)calloc((size_t)count, sizeof(IIGpuThread));
  if (!threads) {
    ii_fail(machine, IR_INTERP_TRAP, "out of memory running a workgroup");
    return 0;
  }
  gpu->scheduler = ir_fiber_scheduler_begin();
  if (!gpu->scheduler) {
    free(threads);
    ii_fail(machine, IR_INTERP_TRAP, "could not schedule a workgroup");
    return 0;
  }
  gpu->kernel = kernel;
  gpu->args = args;
  gpu->nargs = nargs;
  gpu->base_depth = machine->depth;
  gpu->base_pure_depth = machine->pure_depth;
  gpu->base_pure_fn = machine->pure_fn;
  gpu->base_pure_declared = machine->pure_declared;
  gpu->base_effects = machine->effect_frame_count;
  gpu->tile_trace_warps = (size_t)((count + II_GPU_SUBGROUP - 1) /
                                   II_GPU_SUBGROUP);
  gpu->tile_traces = (IITileTrace *)calloc(gpu->tile_trace_warps,
                                           sizeof(IITileTrace));
  ii_gpu_new_phase(gpu, threads, count);

  while (ok) {
    int progress = 0;
    long long live = 0, at_barrier = 0, at_collective = 0, waiting = 0;
    const IRInstruction *barrier_site = NULL;
    size_t barrier_line = 0, other_line = 0;
    int mixed_barriers = 0;
    for (long long first = 0; first < count && ok; first += II_GPU_SUBGROUP) {
      long long lanes = count - first < II_GPU_SUBGROUP ? count - first
                                                        : II_GPU_SUBGROUP;
      ok = ii_gpu_run_warp(machine, threads, first, lanes, &progress);
    }
    if (!ok) {
      break;
    }
    if (progress) {
      continue;
    }
    for (long long t = 0; t < count; t++) {
      IIGpuThread *thread = &threads[t];
      if (thread->finished) {
        continue;
      }
      live++;
      if (thread->stop == II_STOP_BARRIER) {
        at_barrier++;
        if (!barrier_site) {
          barrier_site = thread->site;
          barrier_line = thread->barrier_line;
        } else if (barrier_site != thread->site) {
          mixed_barriers = 1;
          other_line = thread->barrier_line;
        }
      } else if (thread->stop == II_STOP_COLLECTIVE) {
        at_collective++;
        other_line = thread->barrier_line;
      } else if (thread->stop == II_STOP_WAIT) {
        waiting++;
        other_line = thread->barrier_line;
      }
    }
    if (live == 0) {
      break;
    }
    if (at_collective) {
      snprintf(machine->detail, sizeof(machine->detail),
               "the subgroup operation at line %llu was reached by part of its "
               "subgroup; the rest stopped elsewhere or returned",
               (unsigned long long)other_line);
      machine->status = IR_INTERP_TRAP;
      ok = 0;
      break;
    }
    if (waiting) {
      snprintf(machine->detail, sizeof(machine->detail),
               "the transaction-barrier wait at line %llu never completes: no "
               "work item left can arrive or deliver its bytes",
               (unsigned long long)other_line);
      machine->status = IR_INTERP_TRAP;
      ok = 0;
      break;
    }
    if (mixed_barriers) {
      snprintf(machine->detail, sizeof(machine->detail),
               "two work items of one block stopped at different barriers, "
               "one at line %llu and one at line %llu",
               (unsigned long long)barrier_line,
               (unsigned long long)other_line);
      machine->status = IR_INTERP_TRAP;
      ok = 0;
      break;
    }
    if (live < count) {
      snprintf(machine->detail, sizeof(machine->detail),
               "the barrier at line %llu was reached by %lld of the %lld "
               "work items still running in this workgroup",
               (unsigned long long)barrier_line, at_barrier, count);
      machine->status = IR_INTERP_TRAP;
      ok = 0;
      break;
    }
    for (long long t = 0; t < count; t++) {
      if (threads[t].stop == II_STOP_BARRIER) {
        threads[t].stop = II_STOP_NONE;
      }
    }
    gpu->threads_run += at_barrier;
    ii_gpu_new_phase(gpu, threads, count);
  }

  gpu->threads_run += count;
  machine->gpu_current = NULL;
  machine->depth = gpu->base_depth;
  machine->pure_depth = gpu->base_pure_depth;
  machine->pure_fn = gpu->base_pure_fn;
  machine->pure_declared = gpu->base_pure_declared;
  machine->effect_frame_count = gpu->base_effects;
  for (size_t w = 0; gpu->tile_traces && w < gpu->tile_trace_warps; w++) {
    free(gpu->tile_traces[w].marks);
  }
  free(gpu->tile_traces);
  gpu->tile_traces = NULL;
  gpu->tile_trace_warps = 0;
  for (long long t = 0; t < count; t++) {
    ir_fiber_destroy(threads[t].fiber);
    free(threads[t].copies);
    free(threads[t].effects);
  }
  ir_fiber_scheduler_end(gpu->scheduler);
  gpu->scheduler = NULL;
  free(threads);
  return ok;
}

static int ii_extern_numerics_harness(IRInterpMachine *machine,
                                      const char *name,
                                      const IRInterpValue *args,
                                      size_t arg_count,
                                      IRInterpValue *result);

static int ii_extern_numerics(IRInterpMachine *machine, const char *name,
                              const IRInterpValue *args, size_t arg_count,
                              IRInterpValue *result) {
  if (strncmp(name, "numerics_", 9) != 0) {
    return II_EXTERN_UNHANDLED;
  }
  if (strcmp(name, "numerics_tensor_map_2d") != 0) {
    return ii_extern_numerics_harness(machine, name, args, arg_count, result);
  }
  {
    IITensorMap map;
    if (arg_count < 9) {
      return -1;
    }
    memset(&map, 0, sizeof(map));
    map.magic = II_TENSOR_MAP_MAGIC;
    map.base = (unsigned long long)ii_as_int(&args[1]);
    map.element_bytes = (unsigned)ii_as_int(&args[2]);
    map.width = (unsigned long long)ii_as_int(&args[3]);
    map.height = (unsigned long long)ii_as_int(&args[4]);
    map.row_bytes = (unsigned long long)ii_as_int(&args[5]);
    map.box_width = (unsigned)ii_as_int(&args[6]);
    map.box_height = (unsigned)ii_as_int(&args[7]);
    map.swizzle = (unsigned)ii_as_int(&args[8]);
    if (map.swizzle != 0 && map.swizzle != 32 && map.swizzle != 64 &&
        map.swizzle != 128) {
      ii_fail(machine, IR_INTERP_TRAP,
              "numerics_tensor_map_2d: swizzle is 0, 32, 64 or 128 bytes");
      return -1;
    }
    if (!ii_gpu_write(machine, (unsigned long long)ii_as_int(&args[0]),
                      (long long)sizeof(map), (const unsigned char *)&map)) {
      return -1;
    }
    *result = ii_int_value(0);
    return 1;
  }
}
