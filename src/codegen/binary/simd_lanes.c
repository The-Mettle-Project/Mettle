#include "codegen/binary/simd_internal.h"
#include "codegen/code_generator_internal.h"
#include "ir/ir_lanes.h"

#include <string.h>

enum { LOC_NONE = 0, LOC_REG, LOC_SLOT, LOC_FOLD, LOC_SPILL, LOC_IOTA };
enum { SCRATCH_A = 14, SCRATCH_B = 15, LANE_POOL = 14 };
enum { LANE_SKIP_MAX = 72, LANE_PAGE = 4096 };

static const BinaryGpRegister kLaneBases[LANES_MAX_BASES] = {
    BINARY_GP_RCX, BINARY_GP_RDX, BINARY_GP_R8,  BINARY_GP_R9,
    BINARY_GP_R10, BINARY_GP_R11, BINARY_GP_RSI, BINARY_GP_RDI};

typedef struct {
  int mem;
  int reg;
  int base;
  int index;
  int scale;
  int disp;
} LaneRm;

typedef struct {
  CodeGenerator *gen;
  BinaryFunctionContext *ctx;
  const IRInstruction *in;
  BinaryCodeBuffer *b;
  LaneLayout L;
  LaneInsn ops[LANES_MAX_OPS];
  int emit;
  int frame;
  int seq_slot;
  int step_slot;
  int spill_base;
  int spills;
  int iota_reg;
  int acc_reg[LANES_MAX_REDUCE];
  int last_use[LANES_MAX_OPS];
  int uses[LANES_MAX_OPS];
  int fold[LANES_MAX_OPS];
  int loc_kind[LANES_MAX_OPS];
  int loc[LANES_MAX_OPS];
  int sat_slot[LANES_MAX_OPS];
  int holder[16];
  int reserved[16];
  size_t skip[LANE_SKIP_MAX];
  int n_skip;
} LaneGen;

static int lane_vex(BinaryCodeBuffer *b, int map, int pp, int w, int l,
                    unsigned char opcode, int reg, int vvvv, LaneRm rm) {
  int x = rm.mem && rm.index >= 0 ? (rm.index >> 3) & 1 : 0;
  int bb = rm.mem ? (rm.base >> 3) & 1 : (rm.reg >> 3) & 1;
  int r = (reg >> 3) & 1;
  unsigned char b1 = (unsigned char)(((~r & 1) << 7) | ((~x & 1) << 6) |
                                     ((~bb & 1) << 5) | (map & 0x1F));
  unsigned char b2 = (unsigned char)(((w & 1) << 7) | (((~vvvv) & 15) << 3) |
                                     ((l & 1) << 2) | (pp & 3));
  if (!binary_code_buffer_append_u8(b, 0xC4) ||
      !binary_code_buffer_append_u8(b, b1) ||
      !binary_code_buffer_append_u8(b, b2) ||
      !binary_code_buffer_append_u8(b, opcode)) {
    return 0;
  }
  if (!rm.mem) {
    return binary_code_buffer_append_u8(
        b, (unsigned char)(0xC0 | ((reg & 7) << 3) | (rm.reg & 7)));
  }
  int base_low = rm.base & 7;
  int mod = (rm.disp == 0 && base_low != 5) ? 0
            : (rm.disp >= -128 && rm.disp <= 127) ? 1
                                                  : 2;
  int sib = rm.index >= 0 || base_low == 4;
  if (!binary_code_buffer_append_u8(
          b, (unsigned char)((mod << 6) | ((reg & 7) << 3) |
                             (sib ? 4 : base_low)))) {
    return 0;
  }
  if (sib) {
    int scale = rm.index < 0     ? 0
                : rm.scale == 8 ? 3
                : rm.scale == 4 ? 2
                : rm.scale == 2 ? 1
                                : 0;
    int index = rm.index >= 0 ? (rm.index & 7) : 4;
    if (!binary_code_buffer_append_u8(
            b, (unsigned char)((scale << 6) | (index << 3) | base_low))) {
      return 0;
    }
  }
  if (mod == 1) {
    return binary_code_buffer_append_u8(b, (unsigned char)(signed char)rm.disp);
  }
  return mod != 2 || binary_code_buffer_append_u32(b, (uint32_t)rm.disp);
}

static LaneRm lane_reg_rm(int reg) {
  LaneRm rm = {0, reg, 0, -1, 1, 0};
  return rm;
}

static LaneRm lane_mem(int base, int index, int disp) {
  LaneRm rm = {1, 0, base, index, 4, disp};
  return rm;
}

static int lane_slot_disp(int slot) { return 32 * slot; }

static int lane_op1(LaneGen *k, int map, int pp, unsigned char op, int dst,
                    int src1, LaneRm src2) {
  return !k->emit || lane_vex(k->b, map, pp, 0, 1, op, dst, src1, src2);
}

static int lane_op1_imm(LaneGen *k, int map, int pp, unsigned char op,
                        int dst, int src1, LaneRm src2, unsigned char imm) {
  return !k->emit || (lane_vex(k->b, map, pp, 0, 1, op, dst, src1, src2) &&
                      binary_code_buffer_append_u8(k->b, imm));
}

static int lane_movups_load(LaneGen *k, int dst, LaneRm src) {
  return lane_op1(k, 1, 0, 0x10, dst, 0, src);
}

static int lane_movups_store(LaneGen *k, LaneRm dst, int src) {
  return lane_op1(k, 1, 0, 0x11, src, 0, dst);
}

static int lane_value_operands(const LaneInsn *n, int out[3]) {
  return ir_lanes_operands(n, out);
}

static int lane_writes_memory(int op) {
  return op == LANE_STORE || op == LANE_MSTORE;
}

static int lane_plain_const(const LaneGen *k, int v) {
  if (k->ops[v].op != LANE_CONST) {
    return 0;
  }
  uint32_t bits = (uint32_t)k->in->arguments[k->L.consts_at + k->ops[v].a]
                      .int_value;
  return (bits & 0x7F800000u) != 0x7F800000u || (bits & 0x007FFFFFu) == 0;
}

static int lane_commutes(const LaneGen *k, const LaneInsn *n) {
  switch (n->op) {
  case LANE_FADD:
  case LANE_FMUL:
    return lane_plain_const(k, n->a) || lane_plain_const(k, n->b);
  case LANE_IADD:
  case LANE_IMUL:
  case LANE_AND:
  case LANE_OR:
  case LANE_XOR:
  case LANE_ICMPEQ:
  case LANE_IMIN:
  case LANE_IMAX:
  case LANE_UMIN:
  case LANE_UMAX:
    return 1;
  case LANE_FCMP:
    return n->c == LANE_CMP_EQ || n->c == LANE_CMP_NE;
  default:
    return 0;
  }
}

static int lane_rm_operand(const LaneGen *k, const LaneInsn *n, int value) {
  int ops[3];
  int count = lane_value_operands(n, ops);
  switch (n->op) {
  case LANE_STORE:
  case LANE_MLOAD:
  case LANE_MSTORE:
  case LANE_COPY:
  case LANE_MASK01:
  case LANE_SHLI:
  case LANE_SHRI:
  case LANE_SARI:
    return 0;
  case LANE_SELECT:
    return ops[1] == value && ops[0] != value && ops[2] != value;
  default:
    break;
  }
  if (count < 2) {
    return count == 1;
  }
  if (ops[0] == ops[1]) {
    return 0;
  }
  return ops[1] == value || (ops[0] == value && lane_commutes(k, n));
}

static void lane_forward_copies(LaneGen *k) {
  int alias[LANES_MAX_OPS];
  for (int i = 0; i < k->L.n_ops; i++) {
    LaneInsn *n = &k->ops[i];
    int *slots[3] = {&n->a, &n->b, &n->c};
    int fields[3];
    int count = ir_lanes_operand_fields(n->op, fields);
    for (int q = 0; q < count; q++) {
      *slots[fields[q]] = alias[*slots[fields[q]]];
    }
    alias[i] = n->op == LANE_COPY ? n->a : i;
  }
}

static void lane_analyze(LaneGen *k) {
  int last_store = -1;
  int store_before[LANES_MAX_OPS];
  lane_forward_copies(k);
  for (int i = 0; i < k->L.n_ops; i++) {
    k->last_use[i] = -1;
    k->uses[i] = 0;
    k->fold[i] = 0;
    store_before[i] = last_store;
    if (lane_writes_memory(k->ops[i].op)) {
      last_store = i;
    }
  }
  for (int i = 0; i < k->L.n_ops; i++) {
    int ops[3];
    int count = lane_value_operands(&k->ops[i], ops);
    for (int q = 0; q < count; q++) {
      k->last_use[ops[q]] = i;
      k->uses[ops[q]]++;
    }
  }
  for (int v = 0; v < k->L.n_ops; v++) {
    int u = k->last_use[v];
    if (k->ops[v].op != LANE_LOAD || k->uses[v] != 1 || u < 0 ||
        store_before[u] > v || !lane_rm_operand(k, &k->ops[u], v)) {
      continue;
    }
    k->fold[v] = 1;
  }
}

static LaneRm lane_location(LaneGen *k, int v) {
  const LaneInsn *n = &k->ops[v];
  switch (k->loc_kind[v]) {
  case LOC_REG:
  case LOC_IOTA:
    return lane_reg_rm(k->loc[v]);
  case LOC_SLOT:
    return lane_mem(BINARY_GP_RSP, -1, lane_slot_disp(k->loc[v]));
  case LOC_FOLD:
    return lane_mem(kLaneBases[n->a], BINARY_GP_R12, 0);
  default:
    return lane_mem(BINARY_GP_RSP, -1,
                    lane_slot_disp(k->spill_base + k->loc[v]));
  }
}

static int lane_in_reg(const LaneGen *k, int v) {
  return k->loc_kind[v] == LOC_REG || k->loc_kind[v] == LOC_IOTA;
}

static int lane_as_reg(LaneGen *k, int v, int scratch, int *reg) {
  if (lane_in_reg(k, v)) {
    *reg = k->loc[v];
    return 1;
  }
  *reg = scratch;
  return lane_movups_load(k, scratch, lane_location(k, v));
}

static int lane_spill_one(LaneGen *k, int now, const int *keep, int nkeep) {
  int victim = -1;
  int farthest = -1;
  for (int r = 0; r < LANE_POOL; r++) {
    int v = k->holder[r];
    int kept = 0;
    for (int q = 0; q < nkeep; q++) {
      kept |= keep[q] == v;
    }
    if (v < 0 || k->reserved[r] || kept) {
      continue;
    }
    int next = -1;
    for (int i = now; i < k->L.n_ops && next < 0; i++) {
      int ops[3];
      int count = lane_value_operands(&k->ops[i], ops);
      for (int q = 0; q < count; q++) {
        if (ops[q] == v) {
          next = i;
        }
      }
    }
    if (next < 0) {
      next = k->L.n_ops + 1;
    }
    if (next > farthest) {
      farthest = next;
      victim = r;
    }
  }
  if (victim < 0) {
    return -1;
  }
  int v = k->holder[victim];
  int slot = k->spills++;
  k->loc_kind[v] = LOC_SPILL;
  k->loc[v] = slot;
  k->holder[victim] = -1;
  if (!lane_movups_store(k, lane_mem(BINARY_GP_RSP, -1,
                                     lane_slot_disp(k->spill_base + slot)),
                         victim)) {
    return -2;
  }
  return victim;
}

static int lane_take_reg(LaneGen *k, int now, int v, const int *keep,
                         int nkeep) {
  for (int r = 0; r < LANE_POOL; r++) {
    if (!k->reserved[r] && k->holder[r] < 0) {
      k->holder[r] = v;
      return r;
    }
  }
  int r = lane_spill_one(k, now, keep, nkeep);
  if (r >= 0) {
    k->holder[r] = v;
  }
  return r;
}

static void lane_release(LaneGen *k, int i) {
  int ops[3];
  int count = lane_value_operands(&k->ops[i], ops);
  for (int q = 0; q < count; q++) {
    int v = ops[q];
    if (k->last_use[v] == i && k->loc_kind[v] == LOC_REG &&
        k->holder[k->loc[v]] == v) {
      k->holder[k->loc[v]] = -1;
    }
  }
}

static int lane_binary_code(int op, int *map, int *pp, unsigned char *code) {
  static const struct {
    int op;
    int map;
    int pp;
    unsigned char code;
  } table[] = {
      {LANE_FADD, 1, 0, 0x58},  {LANE_FSUB, 1, 0, 0x5C},
      {LANE_FMUL, 1, 0, 0x59},  {LANE_FDIV, 1, 0, 0x5E},
      {LANE_FMIN, 1, 0, 0x5D},  {LANE_FMAX, 1, 0, 0x5F},
      {LANE_IADD, 1, 1, 0xFE},  {LANE_ISUB, 1, 1, 0xFA},
      {LANE_IMUL, 2, 1, 0x40},  {LANE_AND, 1, 1, 0xDB},
      {LANE_OR, 1, 1, 0xEB},    {LANE_XOR, 1, 1, 0xEF},
      {LANE_SHLV, 2, 1, 0x47},  {LANE_SHRV, 2, 1, 0x45},
      {LANE_SARV, 2, 1, 0x46},  {LANE_ICMPEQ, 1, 1, 0x76},
      {LANE_ICMPGT, 1, 1, 0x66}, {LANE_IMIN, 2, 1, 0x39},
      {LANE_IMAX, 2, 1, 0x3D},  {LANE_UMIN, 2, 1, 0x3B},
      {LANE_UMAX, 2, 1, 0x3F},  {LANE_FCMP, 1, 0, 0xC2},
  };
  for (size_t t = 0; t < sizeof(table) / sizeof(table[0]); t++) {
    if (table[t].op == op) {
      *map = table[t].map;
      *pp = table[t].pp;
      *code = table[t].code;
      return 1;
    }
  }
  return 0;
}

static int lane_dest(LaneGen *k, int i, const int *keep, int nkeep) {
  lane_release(k, i);
  int r = lane_take_reg(k, i, i, keep, nkeep);
  if (r < 0) {
    return -1;
  }
  k->loc_kind[i] = LOC_REG;
  k->loc[i] = r;
  return r;
}

static int lane_emit_binary(LaneGen *k, int i) {
  LaneInsn n = k->ops[i];
  int map = 0, pp = 0;
  unsigned char code = 0;
  if (!lane_binary_code(n.op, &map, &pp, &code)) {
    return 0;
  }
  int s1 = n.a, s2 = n.b;
  if (lane_commutes(k, &n) && !lane_in_reg(k, s1) && lane_in_reg(k, s2)) {
    s1 = n.b;
    s2 = n.a;
  }
  int keep[2] = {s1, s2};
  int r1 = 0;
  if (!lane_as_reg(k, s1, SCRATCH_A, &r1)) {
    return 0;
  }
  LaneRm m2 = lane_location(k, s2);
  int d = lane_dest(k, i, keep, 2);
  if (d < 0) {
    return 0;
  }
  if (n.op == LANE_FCMP) {
    return lane_op1_imm(k, map, pp, code, d, r1, m2, (unsigned char)n.c);
  }
  return lane_op1(k, map, pp, code, d, r1, m2);
}

static int lane_emit_unary(LaneGen *k, int i) {
  LaneInsn n = k->ops[i];
  int keep[1] = {n.a};
  LaneRm src = lane_location(k, n.a);
  int shift = n.op == LANE_MASK01 || n.op == LANE_SHLI ||
              n.op == LANE_SHRI || n.op == LANE_SARI;
  int r = 0;
  if (shift) {
    if (!lane_as_reg(k, n.a, SCRATCH_A, &r)) {
      return 0;
    }
    src = lane_reg_rm(r);
  }
  int d = lane_dest(k, i, keep, 1);
  if (d < 0) {
    return 0;
  }
  switch (n.op) {
  case LANE_FSQRT:
    return lane_op1(k, 1, 0, 0x51, d, 0, src);
  case LANE_CVTI2F:
    return lane_op1(k, 1, 0, 0x5B, d, 0, src);
  case LANE_CVTF2I:
    return lane_op1(k, 1, 2, 0x5B, d, 0, src);
  case LANE_MASK01:
    return lane_op1_imm(k, 1, 1, 0x72, 2, d, src, 31);
  case LANE_SHLI:
    return lane_op1_imm(k, 1, 1, 0x72, 6, d, src, (unsigned char)n.b);
  case LANE_SHRI:
    return lane_op1_imm(k, 1, 1, 0x72, 2, d, src, (unsigned char)n.b);
  case LANE_SARI:
    return lane_op1_imm(k, 1, 1, 0x72, 4, d, src, (unsigned char)n.b);
  default:
    return 0;
  }
}

static int lane_emit_select(LaneGen *k, int i) {
  LaneInsn n = k->ops[i];
  int keep[3] = {n.a, n.b, n.c};
  int rmask = 0, roff = 0;
  if (!lane_as_reg(k, n.a, SCRATCH_A, &rmask) ||
      !lane_as_reg(k, n.c, SCRATCH_B, &roff)) {
    return 0;
  }
  LaneRm on = lane_location(k, n.b);
  int d = lane_dest(k, i, keep, 3);
  if (d < 0) {
    return 0;
  }
  return lane_op1_imm(k, 3, 1, 0x4A, d, roff, on,
                      (unsigned char)((rmask & 15) << 4));
}

static int lane_emit_op(LaneGen *k, int i) {
  LaneInsn n = k->ops[i];
  switch (n.op) {
  case LANE_CONST:
  case LANE_SCALAR:
    k->loc_kind[i] = LOC_SLOT;
    k->loc[i] = n.op == LANE_CONST ? n.a
                : n.c != 0         ? k->sat_slot[i]
                                   : k->L.n_consts + n.a;
    return 1;
  case LANE_IOTA:
    k->loc_kind[i] = LOC_IOTA;
    k->loc[i] = k->iota_reg;
    return 1;
  case LANE_LOAD: {
    if (k->fold[i]) {
      k->loc_kind[i] = LOC_FOLD;
      return 1;
    }
    int d = lane_dest(k, i, NULL, 0);
    return d >= 0 &&
           lane_movups_load(k, d, lane_mem(kLaneBases[n.a], BINARY_GP_R12, 0));
  }
  case LANE_STORE: {
    int r = 0;
    if (!lane_as_reg(k, n.b, SCRATCH_A, &r)) {
      return 0;
    }
    lane_release(k, i);
    return lane_movups_store(k, lane_mem(kLaneBases[n.a], BINARY_GP_R12, 0),
                             r);
  }
  case LANE_REDUCE: {
    int acc = k->acc_reg[n.a];
    LaneRm v = lane_location(k, n.b);
    int is_float = k->in->arguments[k->L.kinds_at + n.a].int_value ==
                   LANE_REDUCE_FSUM;
    lane_release(k, i);
    return is_float ? lane_op1(k, 1, 0, 0x58, acc, acc, v)
                    : lane_op1(k, 1, 1, 0xFE, acc, acc, v);
  }
  case LANE_MLOAD: {
    int mask = 0;
    int keep[1] = {n.b};
    if (!lane_as_reg(k, n.b, SCRATCH_A, &mask)) {
      return 0;
    }
    int d = lane_dest(k, i, keep, 1);
    return d >= 0 &&
           lane_op1(k, 2, 1, 0x2C, d, mask,
                    lane_mem(kLaneBases[n.a], BINARY_GP_R12, 0));
  }
  case LANE_MSTORE: {
    int value = 0;
    int mask = 0;
    if (!lane_as_reg(k, n.b, SCRATCH_A, &value) ||
        !lane_as_reg(k, n.c, SCRATCH_B, &mask)) {
      return 0;
    }
    lane_release(k, i);
    return lane_op1(k, 2, 1, 0x2E, value, mask,
                    lane_mem(kLaneBases[n.a], BINARY_GP_R12, 0));
  }
  case LANE_SELECT:
    return lane_emit_select(k, i);
  case LANE_COPY:
    return 1;
  case LANE_FSQRT:
  case LANE_CVTI2F:
  case LANE_CVTF2I:
  case LANE_MASK01:
  case LANE_SHLI:
  case LANE_SHRI:
  case LANE_SARI:
    return lane_emit_unary(k, i);
  default:
    return lane_emit_binary(k, i);
  }
}

static int lane_emit_body(LaneGen *k) {
  for (int r = 0; r < 16; r++) {
    k->holder[r] = -1;
  }
  k->spills = 0;
  for (int i = 0; i < k->L.n_ops; i++) {
    k->loc_kind[i] = LOC_NONE;
  }
  for (int i = 0; i < k->L.n_ops; i++) {
    if (!lane_emit_op(k, i)) {
      return 0;
    }
  }
  return 1;
}

static int lane_check_operands(const LaneGen *k) {
  for (int i = 0; i < k->L.n_ops; i++) {
    const LaneInsn *n = &k->ops[i];
    int ops[3];
    int count = lane_value_operands(n, ops);
    if (n->op < LANE_LOAD || n->op >= LANE_OP_COUNT || n->op == LANE_CARRY) {
      return 0;
    }
    for (int q = 0; q < count; q++) {
      if (ops[q] < 0 || ops[q] >= i) {
        return 0;
      }
    }
    int memory = n->op == LANE_LOAD || n->op == LANE_MLOAD ||
                 lane_writes_memory(n->op);
    int limit = memory                  ? k->L.n_bases
                : n->op == LANE_CONST   ? k->L.n_consts
                : n->op == LANE_SCALAR  ? k->L.n_scalars
                : n->op == LANE_REDUCE  ? k->L.n_reduce
                                        : -1;
    if ((limit >= 0 && (n->a < 0 || n->a >= limit)) ||
        (n->op == LANE_SCALAR && (n->c < 0 || n->c > 2))) {
      return 0;
    }
  }
  return 1;
}

static int lane_skip_if(LaneGen *k, unsigned char cc) {
  return k->n_skip < LANE_SKIP_MAX && wcs_jcc(k->b, cc, &k->skip[k->n_skip++]);
}

static int lane_movsxd(BinaryCodeBuffer *b, int reg) {
  return binary_emit_rex(b, 1, reg >> 3, 0, reg >> 3) &&
         binary_code_buffer_append_u8(b, 0x63) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((reg & 7) << 3) | (reg & 7)));
}

static int lane_compare_width(LaneGen *k, const IROperand *op, int *is_unsigned) {
  if (op->kind == IR_OPERAND_INT) {
    return 0;
  }
  const MtlcType *t =
      code_generator_binary_get_operand_type_in_context(k->gen, k->ctx, op);
  if (!t || code_generator_type_is_aggregate(t) ||
      code_generator_binary_resolved_type_float_bits(t) != 0) {
    return 8;
  }
  *is_unsigned |= !code_generator_binary_resolved_type_is_signed_integer(t);
  int s = code_generator_binary_resolved_type_scalar_size(t);
  return (s == 1 || s == 2 || s == 4) ? s : 8;
}

static int lane_extend_counters(LaneGen *k) {
  int is_unsigned = 0;
  int wi = lane_compare_width(k, &k->in->dest, &is_unsigned);
  int wb = lane_compare_width(k, &k->in->lhs, &is_unsigned);
  if ((wi > wb ? wi : wb) != 4) {
    return 1;
  }
  BinaryCodeBuffer *b = k->b;
  if (!is_unsigned) {
    return lane_movsxd(b, BINARY_GP_R12) && lane_movsxd(b, BINARY_GP_R13);
  }
  return wcs_mov_reg_reg32(b, BINARY_GP_R12, BINARY_GP_R12) &&
         wcs_mov_reg_reg32(b, BINARY_GP_R13, BINARY_GP_R13) &&
         binary_emit_cmp_reg_imm32(b, BINARY_GP_R12, 0x7FFFFFFFu) &&
         lane_skip_if(k, 0x8F);
}

static int lane_overlap_guard(LaneGen *k, int s, int t) {
  BinaryCodeBuffer *b = k->b;
  size_t same = 0;
  return binary_emit_mov_reg_reg(b, BINARY_GP_RAX, kLaneBases[s]) &&
         binary_emit_alu_reg_reg(b, 0x29, BINARY_GP_RAX, kLaneBases[t]) &&
         wcs_jcc(b, 0x84, &same) &&
         binary_emit_mov_reg_reg(b, BINARY_GP_RBX, BINARY_GP_RAX) &&
         binary_emit_shift_reg_imm8(b, 7, BINARY_GP_RBX, 63) &&
         binary_emit_alu_reg_reg(b, 0x31, BINARY_GP_RAX, BINARY_GP_RBX) &&
         binary_emit_alu_reg_reg(b, 0x29, BINARY_GP_RAX, BINARY_GP_RBX) &&
         binary_emit_shift_reg_imm8(b, 5, BINARY_GP_RAX, 2) &&
         binary_emit_cmp_reg_reg(b, BINARY_GP_RAX, BINARY_GP_R13) &&
         lane_skip_if(k, 0x82) && wcs_patch_here(b, same);
}

static int lane_emit_guards(LaneGen *k) {
  BinaryCodeBuffer *b = k->b;
  const IROperand *args = k->in->arguments;
  for (int i = 0; i < k->L.n_bases; i++) {
    if (!code_generator_binary_emit_operand_load(
            k->gen, k->ctx, &args[k->L.bases_at + i], kLaneBases[i])) {
      return 0;
    }
  }
  if (!code_generator_binary_emit_operand_load(k->gen, k->ctx, &k->in->dest,
                                               BINARY_GP_R12) ||
      !code_generator_binary_emit_operand_load(k->gen, k->ctx, &k->in->lhs,
                                               BINARY_GP_R13) ||
      !lane_extend_counters(k) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_RAX, BINARY_GP_R13) ||
      !binary_emit_alu_reg_reg(b, 0x29, BINARY_GP_RAX, BINARY_GP_R12) ||
      !lane_skip_if(k, 0x8E) ||
      (k->L.keep_last && !binary_emit_alu_reg_imm32(b, 5, BINARY_GP_RAX, 1)) ||
      !binary_emit_alu_reg_imm32(b, 4, BINARY_GP_RAX, 0xFFFFFFF8u) ||
      !lane_skip_if(k, 0x84) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_R13, BINARY_GP_RAX)) {
    return 0;
  }
  for (int s = 0; s < k->L.n_bases; s++) {
    if (!(k->L.store_mask & (1u << s))) {
      continue;
    }
    for (int t = 0; t < k->L.n_bases; t++) {
      int checked = (k->L.store_mask & (1u << t)) && t < s;
      if (t != s && !checked && !lane_overlap_guard(k, s, t)) {
        return 0;
      }
    }
  }
  if (k->L.uses_iota &&
      (!binary_emit_test_reg_reg(b, BINARY_GP_R12) || !lane_skip_if(k, 0x88) ||
       !binary_emit_lea_reg_base_index_scale_disp(b, BINARY_GP_RAX,
                                                  BINARY_GP_R12, BINARY_GP_R13,
                                                  1, 0) ||
       !binary_emit_cmp_reg_imm32(b, BINARY_GP_RAX, 0x7FFFFFFFu) ||
       !lane_skip_if(k, 0x8F))) {
    return 0;
  }
  return binary_emit_alu_reg_reg(b, 0x01, BINARY_GP_R13, BINARY_GP_R12);
}

static int lane_rsp_adjust(LaneGen *k, int grow) {
  BinaryCodeBuffer *b = k->b;
  if (!grow) {
    return binary_emit_alu_reg_imm32(b, 0, BINARY_GP_RSP, (uint32_t)k->frame);
  }
  int left = k->frame;
  while (left > LANE_PAGE) {
    if (!binary_emit_alu_reg_imm32(b, 5, BINARY_GP_RSP, LANE_PAGE) ||
        !binary_emit_mov_mem_imm32(b, BINARY_GP_RSP, 0, 0)) {
      return 0;
    }
    left -= LANE_PAGE;
  }
  return binary_emit_alu_reg_imm32(b, 5, BINARY_GP_RSP, (uint32_t)left);
}

static int lane_dword_store(BinaryCodeBuffer *b, int base, int disp,
                            uint32_t value) {
  return binary_emit_memory_access_ex(b, 0, 0, 0xC7, 0, 0,
                                      (BinaryGpRegister)0,
                                      (BinaryGpRegister)base, disp) &&
         binary_code_buffer_append_u32(b, value);
}

static int lane_broadcast_slot(LaneGen *k, int slot, LaneRm from) {
  LaneRm to = lane_mem(BINARY_GP_RSP, -1, lane_slot_disp(slot));
  return lane_op1(k, 2, 1, 0x18, SCRATCH_A, 0, from) &&
         lane_movups_store(k, to, SCRATCH_A);
}

static int lane_scalar_slot(LaneGen *k, int s, LaneRm *at) {
  const IROperand *op = &k->in->arguments[k->L.scalars_at + s];
  for (size_t i = 0; i < k->ctx->marshaled_operand_count; i++) {
    if (k->ctx->marshaled_operands[i].operand == op) {
      int base = k->ctx->marshaled_operands[i].base_register;
      int disp = k->ctx->marshaled_operands[i].displacement;
      *at = lane_mem(base, -1, base == BINARY_GP_RSP ? disp + k->frame : disp);
      return 1;
    }
  }
  return 0;
}

static int lane_emit_clamped_scalars(LaneGen *k);

static int lane_emit_setup(LaneGen *k) {
  BinaryCodeBuffer *b = k->b;
  const IROperand *args = k->in->arguments;
  for (int c = 0; c < k->L.n_consts; c++) {
    uint32_t bits = (uint32_t)args[k->L.consts_at + c].int_value;
    if (!lane_dword_store(b, BINARY_GP_RSP, lane_slot_disp(c), bits) ||
        !lane_broadcast_slot(
            k, c, lane_mem(BINARY_GP_RSP, -1, lane_slot_disp(c)))) {
      return 0;
    }
  }
  for (int s = 0; s < k->L.n_scalars; s++) {
    LaneRm at;
    if (!lane_scalar_slot(k, s, &at)) {
      code_generator_set_error(k->gen, "simd_lanes scalar is not staged");
      return 0;
    }
    if (!lane_broadcast_slot(k, k->L.n_consts + s, at)) {
      return 0;
    }
  }
  if (!lane_emit_clamped_scalars(k)) {
    return 0;
  }
  if (k->L.uses_iota) {
    int seq = lane_slot_disp(k->seq_slot);
    int step = lane_slot_disp(k->step_slot);
    for (int j = 0; j < 8; j++) {
      if (!lane_dword_store(b, BINARY_GP_RSP, seq + 4 * j, (uint32_t)j)) {
        return 0;
      }
    }
    if (!lane_dword_store(b, BINARY_GP_RSP, step, 8) ||
        !lane_broadcast_slot(k, k->step_slot,
                             lane_mem(BINARY_GP_RSP, -1, step)) ||
        !lane_vex(b, 1, 1, 0, 0, 0x6E, k->iota_reg, 0,
                  lane_reg_rm(BINARY_GP_R12)) ||
        !lane_vex(b, 2, 1, 0, 1, 0x58, k->iota_reg, 0,
                  lane_reg_rm(k->iota_reg)) ||
        !lane_op1(k, 1, 1, 0xFE, k->iota_reg, k->iota_reg,
                  lane_mem(BINARY_GP_RSP, -1, seq))) {
      return 0;
    }
  }
  for (int r = 0; r < k->L.n_reduce; r++) {
    int acc = k->acc_reg[r];
    int fsum = args[k->L.kinds_at + r].int_value == LANE_REDUCE_FSUM;
    if (!lane_op1(k, 1, 1, fsum ? 0x76 : 0xEF, acc, acc, lane_reg_rm(acc)) ||
        (fsum && !lane_op1_imm(k, 1, 1, 0x72, 6, acc, lane_reg_rm(acc), 31))) {
      return 0;
    }
  }
  return 1;
}

static int lane_emit_loop(LaneGen *k) {
  BinaryCodeBuffer *b = k->b;
  size_t top = b->size;
  size_t back = 0;
  return lane_emit_body(k) &&
         wcs_addsub_reg_imm8(b, BINARY_GP_R12, 0, 8) &&
         (!k->L.uses_iota ||
          lane_op1(k, 1, 1, 0xFE, k->iota_reg, k->iota_reg,
                   lane_mem(BINARY_GP_RSP, -1,
                            lane_slot_disp(k->step_slot)))) &&
         binary_emit_cmp_reg_reg(b, BINARY_GP_R12, BINARY_GP_R13) &&
         wcs_jcc(b, 0x8C, &back) && wcs_patch_to(b, back, top);
}

static int lane_reduce_acc(LaneGen *k, int r) {
  BinaryCodeBuffer *b = k->b;
  int acc = k->acc_reg[r];
  int fsum = k->in->arguments[k->L.kinds_at + r].int_value == LANE_REDUCE_FSUM;
  int pp = fsum ? 0 : 1;
  unsigned char add = fsum ? 0x58 : 0xFE;
  return lane_vex(b, 3, 1, 0, 1, 0x19, acc, 0, lane_reg_rm(SCRATCH_B)) &&
         binary_code_buffer_append_u8(b, 1) &&
         lane_vex(b, 1, pp, 0, 0, add, acc, acc, lane_reg_rm(SCRATCH_B)) &&
         lane_vex(b, 1, 0, 0, 0, 0x12, SCRATCH_B, SCRATCH_B,
                  lane_reg_rm(acc)) &&
         lane_vex(b, 1, pp, 0, 0, add, acc, acc, lane_reg_rm(SCRATCH_B)) &&
         lane_vex(b, 1, 2, 0, 0, 0x16, SCRATCH_B, 0, lane_reg_rm(acc)) &&
         (fsum ? lane_vex(b, 1, 2, 0, 0, 0x58, acc, acc,
                          lane_reg_rm(SCRATCH_B))
               : lane_vex(b, 1, 1, 0, 0, 0xFE, acc, acc,
                          lane_reg_rm(SCRATCH_B)));
}

static int lane_store_partials(LaneGen *k, int identity) {
  BinaryCodeBuffer *b = k->b;
  const IROperand *args = k->in->arguments;
  for (int r = 0; r < k->L.n_reduce; r++) {
    int fsum = args[k->L.kinds_at + r].int_value == LANE_REDUCE_FSUM;
    if (!code_generator_binary_emit_operand_load(
            k->gen, k->ctx, &args[k->L.partials_at + r], BINARY_GP_RAX)) {
      return 0;
    }
    int ok = identity
                 ? lane_dword_store(b, BINARY_GP_RAX, 0,
                                    fsum ? 0x80000000u : 0u)
                 : lane_vex(b, 1, 2, 0, 0, 0x11, k->acc_reg[r], 0,
                            lane_mem(BINARY_GP_RAX, -1, 0));
    if (!ok) {
      return 0;
    }
  }
  return 1;
}

static void lane_reserve(LaneGen *k) {
  int next = LANE_POOL - 1;
  if (k->L.uses_iota) {
    k->iota_reg = next;
    k->reserved[next--] = 1;
  }
  for (int r = 0; r < k->L.n_reduce; r++) {
    k->acc_reg[r] = next;
    k->reserved[next--] = 1;
  }
  k->seq_slot = k->L.n_consts + k->L.n_scalars;
  k->step_slot = k->seq_slot + 1;
  k->spill_base = k->step_slot + 1;
  for (int i = 0; i < k->L.n_ops; i++) {
    if (k->ops[i].op == LANE_SCALAR && k->ops[i].c != 0) {
      k->sat_slot[i] = k->spill_base++;
    }
  }
}

static int lane_emit_clamped_scalars(LaneGen *k) {
  BinaryCodeBuffer *b = k->b;
  for (int i = 0; i < k->L.n_ops; i++) {
    const LaneInsn *n = &k->ops[i];
    LaneRm at;
    if (n->op != LANE_SCALAR || n->c == 0) {
      continue;
    }
    int disp = lane_slot_disp(k->sat_slot[i]);
    int ok = lane_scalar_slot(k, n->a, &at) &&
             binary_emit_mov_reg_mem(b, BINARY_GP_RAX,
                                     (BinaryGpRegister)at.base, at.disp) &&
             binary_emit_mov_reg_imm64(b, BINARY_GP_RBX, 0x7FFFFFFFu) &&
             binary_emit_cmp_reg_reg(b, BINARY_GP_RAX, BINARY_GP_RBX) &&
             binary_emit_cmovcc_reg_reg(b, n->c == 2 ? 0x47 : 0x4F,
                                        BINARY_GP_RAX, BINARY_GP_RBX);
    if (ok && n->c == 1) {
      ok = binary_emit_mov_reg_imm64(b, BINARY_GP_RBX, 0xFFFFFFFF80000000ull) &&
           binary_emit_cmp_reg_reg(b, BINARY_GP_RAX, BINARY_GP_RBX) &&
           binary_emit_cmovcc_reg_reg(b, 0x4C, BINARY_GP_RAX, BINARY_GP_RBX);
    }
    if (!ok ||
        !binary_emit_memory_access_ex(b, 0, 0, 0x89, 0, 0, BINARY_GP_RAX,
                                      BINARY_GP_RSP, disp) ||
        !lane_broadcast_slot(k, k->sat_slot[i],
                             lane_mem(BINARY_GP_RSP, -1, disp))) {
      return 0;
    }
  }
  return 1;
}

static int lane_emit_kernel(LaneGen *k) {
  BinaryCodeBuffer *b = k->b;
  size_t out = 0;
  k->emit = 0;
  if (!lane_emit_body(k)) {
    return 0;
  }
  k->frame = lane_slot_disp(k->spill_base + k->spills);
  k->emit = 1;
  if (!lane_emit_guards(k) || !lane_rsp_adjust(k, 1) || !lane_emit_setup(k) ||
      !lane_emit_loop(k)) {
    return 0;
  }
  for (int r = 0; r < k->L.n_reduce; r++) {
    if (!lane_reduce_acc(k, r)) {
      return 0;
    }
  }
  if (!lane_rsp_adjust(k, 0) || !lane_store_partials(k, 0) ||
      !wcs_jcc(b, 0, &out)) {
    return 0;
  }
  for (int j = 0; j < k->n_skip; j++) {
    if (!wcs_patch_here(b, k->skip[j])) {
      return 0;
    }
  }
  return lane_store_partials(k, 1) && wcs_patch_here(b, out) &&
         wcs_avx_vzeroupper(b) &&
         code_generator_binary_emit_destination_store(k->gen, k->ctx,
                                                      &k->in->dest,
                                                      BINARY_GP_R12);
}

int code_generator_binary_emit_simd_lanes(CodeGenerator *generator,
                                          BinaryFunctionContext *context,
                                          const IRInstruction *instruction) {
  if (!generator || !context || !instruction) {
    return 0;
  }
  LaneGen *k = (LaneGen *)calloc(1, sizeof(LaneGen));
  if (!k) {
    code_generator_set_error(generator, "Out of memory in simd_lanes");
    return 0;
  }
  k->gen = generator;
  k->ctx = context;
  k->in = instruction;
  k->b = &context->code;
  int ok = ir_lanes_layout(instruction, &k->L) &&
           instruction->dest.kind == IR_OPERAND_SYMBOL;
  for (int i = 0; ok && i < k->L.n_ops; i++) {
    k->ops[i] = ir_lanes_insn(instruction, &k->L, i);
  }
  if (!ok || !lane_check_operands(k)) {
    free(k);
    code_generator_set_error(generator, "Malformed simd_lanes");
    return 0;
  }
  lane_analyze(k);
  lane_reserve(k);
  ok = lane_emit_kernel(k);
  if (!ok) {
    code_generator_set_error(generator, "simd_lanes encoding failed");
  }
  free(k);
  return ok;
}
