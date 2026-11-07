#include "ir_lanes.h"

typedef struct {
  uint32_t lane[8];
} IILanes;

static float ii_lane_f(uint32_t bits) {
  float f;
  memcpy(&f, &bits, 4);
  return f;
}

static uint32_t ii_lane_bits(float f) {
  uint32_t bits;
  memcpy(&bits, &f, 4);
  return bits;
}

static uint32_t ii_lane_mask(int truth) { return truth ? 0xFFFFFFFFu : 0u; }

static uint32_t ii_lane_float_op(int op, int predicate, uint32_t x,
                                 uint32_t y) {
  float a = ii_lane_f(x);
  float b = ii_lane_f(y);
  switch (op) {
  case LANE_FADD:
    return ii_lane_bits(a + b);
  case LANE_FSUB:
    return ii_lane_bits(a - b);
  case LANE_FMUL:
    return ii_lane_bits(a * b);
  case LANE_FDIV:
    return ii_lane_bits(a / b);
  case LANE_FMIN:
    return a < b ? x : y;
  case LANE_FMAX:
    return a > b ? x : y;
  default:
    switch (predicate) {
    case LANE_CMP_EQ:
      return ii_lane_mask(a == b);
    case LANE_CMP_LT:
      return ii_lane_mask(a < b);
    case LANE_CMP_LE:
      return ii_lane_mask(a <= b);
    default:
      return ii_lane_mask(!(a == b));
    }
  }
}

static uint32_t ii_lane_shift(int op, uint32_t x, uint32_t count) {
  if (count > 31) {
    return op == LANE_SARI || op == LANE_SARV
               ? ((x & 0x80000000u) ? 0xFFFFFFFFu : 0u)
               : 0u;
  }
  switch (op) {
  case LANE_SHLI:
  case LANE_SHLV:
    return x << count;
  case LANE_SHRI:
  case LANE_SHRV:
    return x >> count;
  default:
    return (uint32_t)((int32_t)x >> count);
  }
}

static uint32_t ii_lane_int_op(int op, uint32_t x, uint32_t y) {
  int32_t a = (int32_t)x;
  int32_t b = (int32_t)y;
  switch (op) {
  case LANE_IADD:
    return x + y;
  case LANE_ISUB:
    return x - y;
  case LANE_IMUL:
    return x * y;
  case LANE_AND:
    return x & y;
  case LANE_OR:
    return x | y;
  case LANE_XOR:
    return x ^ y;
  case LANE_ICMPEQ:
    return ii_lane_mask(x == y);
  case LANE_ICMPGT:
    return ii_lane_mask(a > b);
  case LANE_IMIN:
    return a < b ? x : y;
  case LANE_IMAX:
    return a > b ? x : y;
  case LANE_UMIN:
    return x < y ? x : y;
  case LANE_UMAX:
    return x > y ? x : y;
  default:
    return ii_lane_shift(op, x, y);
  }
}

static uint32_t ii_lane_truncate(uint32_t x) {
  float f = ii_lane_f(x);
  if (!(f >= -2147483648.0f && f < 2147483648.0f)) {
    return 0x80000000u;
  }
  return (uint32_t)(int32_t)f;
}

static uint32_t ii_lane_clamp(long long v, int is_unsigned) {
  if (is_unsigned) {
    return (unsigned long long)v > 0x7FFFFFFFull ? 0x7FFFFFFFu : (uint32_t)v;
  }
  return v > INT32_MAX   ? (uint32_t)INT32_MAX
         : v < INT32_MIN ? (uint32_t)INT32_MIN
                         : (uint32_t)(int32_t)v;
}

static uint32_t ii_lane_unary(int op, int imm, uint32_t x) {
  switch (op) {
  case LANE_FSQRT:
    return ii_lane_bits(sqrtf(ii_lane_f(x)));
  case LANE_MASK01:
    return x >> 31;
  case LANE_COPY:
    return x;
  case LANE_CVTI2F:
    return ii_lane_bits((float)(int32_t)x);
  case LANE_CVTF2I:
    return ii_lane_truncate(x);
  default:
    return ii_lane_shift(op, x, (uint32_t)imm);
  }
}

typedef struct {
  const IRInstruction *insn;
  LaneLayout L;
  unsigned long long bases[LANES_MAX_BASES];
  uint32_t scalars[LANES_MAX_SCALARS];
  long long raw[LANES_MAX_SCALARS];
  uint32_t acc[LANES_MAX_REDUCE][8];
  IILanes *v;
  long long iv;
} IILaneRun;

static int ii_lane_step(IRInterpMachine *machine, IILaneRun *run, int i) {
  LaneInsn n = ir_lanes_insn(run->insn, &run->L, i);
  IILanes *out = &run->v[i];
  const IROperand *args = run->insn->arguments;
  for (int l = 0; l < 8; l++) {
    int memory = n.op == LANE_LOAD || n.op == LANE_STORE ||
                 n.op == LANE_MLOAD || n.op == LANE_MSTORE;
    unsigned long long at = run->bases[memory ? n.a : 0] +
                            (unsigned long long)(run->iv + l) * 4u;
    uint32_t x = n.op >= LANE_FADD && n.a >= 0 && n.a < i ? run->v[n.a].lane[l]
                                                          : 0;
    uint32_t y = n.b >= 0 && n.b < i ? run->v[n.b].lane[l] : 0;
    uint32_t z = n.c >= 0 && n.c < i ? run->v[n.c].lane[l] : 0;
    unsigned long long raw = 0;
    switch (n.op) {
    case LANE_LOAD:
      if (!ii_mem_read(machine, at, 4, &raw)) {
        return 0;
      }
      out->lane[l] = (uint32_t)raw;
      break;
    case LANE_CONST:
      out->lane[l] = (uint32_t)args[run->L.consts_at + n.a].int_value;
      break;
    case LANE_SCALAR:
      out->lane[l] = n.c == 0 ? run->scalars[n.a]
                              : ii_lane_clamp(run->raw[n.a], n.c == 2);
      break;
    case LANE_IOTA:
      out->lane[l] = (uint32_t)(run->iv + l);
      break;
    case LANE_FADD:
    case LANE_FSUB:
    case LANE_FMUL:
    case LANE_FDIV:
    case LANE_FMIN:
    case LANE_FMAX:
    case LANE_FCMP:
      out->lane[l] = ii_lane_float_op(n.op, n.c, x, y);
      break;
    case LANE_SELECT:
      out->lane[l] = (x & 0x80000000u) ? y : z;
      break;
    case LANE_FSQRT:
    case LANE_MASK01:
    case LANE_COPY:
    case LANE_CVTI2F:
    case LANE_CVTF2I:
    case LANE_SHLI:
    case LANE_SHRI:
    case LANE_SARI:
      out->lane[l] = ii_lane_unary(n.op, n.b, x);
      break;
    case LANE_STORE:
      if (!ii_mem_write(machine, at, 4, y)) {
        return 0;
      }
      break;
    case LANE_MLOAD:
      out->lane[l] = 0;
      if ((y & 0x80000000u) && !ii_mem_read(machine, at, 4, &raw)) {
        return 0;
      }
      out->lane[l] = (uint32_t)raw;
      break;
    case LANE_MSTORE:
      if ((z & 0x80000000u) && !ii_mem_write(machine, at, 4, y)) {
        return 0;
      }
      break;
    case LANE_REDUCE:
      run->acc[n.a][l] =
          args[run->L.kinds_at + n.a].int_value == LANE_REDUCE_FSUM
              ? ii_lane_bits(ii_lane_f(run->acc[n.a][l]) + ii_lane_f(y))
              : run->acc[n.a][l] + y;
      break;
    default:
      out->lane[l] = ii_lane_int_op(n.op, x, y);
      break;
    }
  }
  return 1;
}

static uint32_t ii_lane_horizontal(const uint32_t *acc, int fsum) {
  uint32_t x[4];
  for (int j = 0; j < 4; j++) {
    x[j] = fsum ? ii_lane_bits(ii_lane_f(acc[j]) + ii_lane_f(acc[j + 4]))
                : acc[j] + acc[j + 4];
  }
  uint32_t y0 = fsum ? ii_lane_bits(ii_lane_f(x[0]) + ii_lane_f(x[2]))
                     : x[0] + x[2];
  uint32_t y1 = fsum ? ii_lane_bits(ii_lane_f(x[1]) + ii_lane_f(x[3]))
                     : x[1] + x[3];
  return fsum ? ii_lane_bits(ii_lane_f(y0) + ii_lane_f(y1)) : y0 + y1;
}

static long long ii_lane_count(const IILaneRun *run, long long bound) {
  if (bound <= run->iv) {
    return 0;
  }
  long long count = (bound - run->iv - run->L.keep_last) & ~7ll;
  for (int s = 0; s < run->L.n_bases; s++) {
    if (!(run->L.store_mask & (1u << s))) {
      continue;
    }
    for (int t = 0; t < run->L.n_bases; t++) {
      long long d = (long long)(run->bases[s] - run->bases[t]);
      unsigned long long span = (unsigned long long)(d < 0 ? -d : d) >> 2;
      if (d != 0 && span < (unsigned long long)count) {
        return 0;
      }
    }
  }
  if (run->L.uses_iota && (run->iv < 0 || run->iv + count > INT32_MAX)) {
    return 0;
  }
  return count;
}

static int ii_lane_fetch(IRInterpMachine *machine, IIFrame *frame,
                         IILaneRun *run) {
  const IROperand *args = run->insn->arguments;
  for (int b = 0; b < run->L.n_bases; b++) {
    if (!ii_fetch_addr(machine, frame, &args[run->L.bases_at + b],
                       &run->bases[b])) {
      return 0;
    }
  }
  for (int s = 0; s < run->L.n_scalars; s++) {
    IRInterpValue value;
    if (!ii_fetch(machine, frame, &args[run->L.scalars_at + s], &value)) {
      return 0;
    }
    run->scalars[s] = value.is_float ? ii_lane_bits((float)value.f)
                                     : (uint32_t)value.i;
    run->raw[s] = value.is_float ? 0 : value.i;
  }
  for (int r = 0; r < run->L.n_reduce; r++) {
    uint32_t identity =
        args[run->L.kinds_at + r].int_value == LANE_REDUCE_FSUM ? 0x80000000u
                                                                : 0u;
    for (int l = 0; l < 8; l++) {
      run->acc[r][l] = identity;
    }
  }
  return 1;
}

static int ii_exec_lanes(IRInterpMachine *machine, IIFrame *frame,
                         const IRInstruction *insn) {
  IILaneRun run;
  memset(&run, 0, sizeof(run));
  run.insn = insn;
  long long bound = 0;
  if (!ir_lanes_layout(insn, &run.L)) {
    ii_fail(machine, IR_INTERP_UNSUPPORTED, "malformed simd_lanes");
    return 0;
  }
  if (!ii_fetch_int(machine, frame, &insn->dest, &run.iv) ||
      !ii_fetch_int(machine, frame, &insn->lhs, &bound) ||
      !ii_lane_fetch(machine, frame, &run)) {
    return 0;
  }
  long long count = ii_lane_count(&run, bound);
  long long end = run.iv + count;
  run.v = (IILanes *)calloc((size_t)run.L.n_ops, sizeof(IILanes));
  if (!run.v) {
    ii_fail(machine, IR_INTERP_TRAP, "out of memory");
    return 0;
  }
  int ok = 1;
  for (; ok && run.iv < end; run.iv += 8) {
    for (int i = 0; ok && i < run.L.n_ops; i++) {
      ok = ii_lane_step(machine, &run, i);
    }
  }
  free(run.v);
  machine->fuel -= count;
  const IROperand *args = insn->arguments;
  for (int r = 0; ok && r < run.L.n_reduce; r++) {
    unsigned long long partial = 0;
    int fsum = args[run.L.kinds_at + r].int_value == LANE_REDUCE_FSUM;
    ok = ii_fetch_addr(machine, frame, &args[run.L.partials_at + r],
                       &partial) &&
         ii_mem_write(machine, partial, 4,
                      ii_lane_horizontal(run.acc[r], fsum));
  }
  IRInterpValue next = ii_int_value(end);
  return ok && ii_store_dest(machine, frame, &insn->dest, &next);
}
