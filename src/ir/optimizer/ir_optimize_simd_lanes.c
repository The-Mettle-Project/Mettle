#include "ir_optimize_internal.h"
#include "../ir_lanes.h"

enum { LK_F32 = 1, LK_INT = 2, LK_MASK = 3 };
enum { LX_S = 1, LX_U = 2 };

#define LANE_NONE (-2)
#define LANE_POISON (-1)

typedef struct {
  int op;
  int a;
  int b;
  int c;
  int kind;
  int ext;
} LaneNode;

typedef struct {
  const char *name;
  int node;
} LaneBinding;

typedef struct {
  LaneBinding *items;
  int count;
  int capacity;
} LaneEnv;

typedef struct {
  IRFunction *fn;
  const char *iv;
  size_t header;
  size_t cond_at;
  size_t tag;
  size_t lo;
  size_t hi;
  size_t exit_index;
  LaneNode nodes[LANES_MAX_OPS];
  int n;
  const char *bases[LANES_MAX_BASES];
  int n_bases;
  unsigned stored;
  unsigned plain_access;
  int path;
  int forward[LANES_MAX_BASES];
  const char *scalars[LANES_MAX_SCALARS];
  int scalar_is_temp[LANES_MAX_SCALARS];
  int n_scalars;
  uint32_t consts[LANES_MAX_CONSTS];
  int n_consts;
  const char *carried[LANES_MAX_REDUCE];
  int n_carried;
  int depth;
  int epoch;
  const char **written;
  size_t written_count;
  const char **address_temps;
  size_t address_temp_count;
  int keep_last;
  int why;
  char why_reason[320];
  char why_fix[240];
} LaneBuild;

typedef struct {
  char *fn;
  size_t line;
  int why;
  char reason[320];
  char fix[240];
} LaneVerdict;

static LaneVerdict *g_lane_verdicts;
static size_t g_lane_verdict_count;
static size_t g_lane_verdict_capacity;

static void lane_why(LaneBuild *B, int why, const char *reason,
                     const char *fix) {
  if (B->why != LANE_WHY_NONE) {
    return;
  }
  B->why = why;
  snprintf(B->why_reason, sizeof(B->why_reason), "%s", reason);
  snprintf(B->why_fix, sizeof(B->why_fix), "%s", fix);
}

static LaneVerdict *lane_verdict_slot(const char *fn, size_t line) {
  for (size_t i = 0; i < g_lane_verdict_count; i++) {
    if (g_lane_verdicts[i].line == line && strcmp(g_lane_verdicts[i].fn, fn) == 0) {
      return &g_lane_verdicts[i];
    }
  }
  if (g_lane_verdict_count == g_lane_verdict_capacity) {
    size_t grown = g_lane_verdict_capacity ? 2 * g_lane_verdict_capacity : 16;
    LaneVerdict *more = realloc(g_lane_verdicts, grown * sizeof(LaneVerdict));
    if (!more) {
      return NULL;
    }
    g_lane_verdicts = more;
    g_lane_verdict_capacity = grown;
  }
  LaneVerdict *v = &g_lane_verdicts[g_lane_verdict_count];
  v->fn = mettle_strdup(fn);
  if (!v->fn) {
    return NULL;
  }
  v->line = line;
  g_lane_verdict_count++;
  return v;
}

static void lane_record_verdict(const LaneBuild *B, size_t header) {
  const IRFunction *fn = B->fn;
  if (!fn->name) {
    return;
  }
  LaneVerdict *v =
      lane_verdict_slot(fn->name, fn->instructions[header].location.line);
  if (!v) {
    return;
  }
  v->why = B->why;
  snprintf(v->reason, sizeof(v->reason), "%s", B->why_reason);
  snprintf(v->fix, sizeof(v->fix), "%s", B->why_fix);
}

int ir_simd_lanes_verdict(const char *fn, size_t line, char *reason,
                          size_t reason_cap, char *fix, size_t fix_cap) {
  for (size_t i = 0; fn && i < g_lane_verdict_count; i++) {
    const LaneVerdict *v = &g_lane_verdicts[i];
    if (v->line == line && strcmp(v->fn, fn) == 0 && v->why != LANE_WHY_NONE) {
      snprintf(reason, reason_cap, "%s", v->reason);
      snprintf(fix, fix_cap, "%s", v->fix);
      return v->why;
    }
  }
  return LANE_WHY_NONE;
}

static int lane_names_contain(const char **names, size_t count,
                              const char *name) {
  for (size_t i = 0; i < count; i++) {
    if (strcmp(names[i], name) == 0) {
      return 1;
    }
  }
  return 0;
}

static int lane_env_get(const LaneEnv *env, const char *name) {
  for (int i = env->count; i-- > 0;) {
    if (strcmp(env->items[i].name, name) == 0) {
      return env->items[i].node;
    }
  }
  return LANE_NONE;
}

static int lane_env_set(LaneEnv *env, const char *name, int node) {
  for (int i = env->count; i-- > 0;) {
    if (strcmp(env->items[i].name, name) == 0) {
      env->items[i].node = node;
      return 1;
    }
  }
  if (env->count == env->capacity) {
    int grown = env->capacity ? env->capacity * 2 : 64;
    LaneBinding *items = realloc(env->items, (size_t)grown * sizeof(*items));
    if (!items) {
      return 0;
    }
    env->items = items;
    env->capacity = grown;
  }
  env->items[env->count].name = name;
  env->items[env->count].node = node;
  env->count++;
  return 1;
}

static int lane_env_copy(LaneEnv *dst, const LaneEnv *src) {
  dst->count = 0;
  for (int i = 0; i < src->count; i++) {
    if (!lane_env_set(dst, src->items[i].name, src->items[i].node)) {
      return 0;
    }
  }
  return 1;
}

static int lane_add(LaneBuild *B, int op, int a, int b, int c, int kind,
                    int ext) {
  int unique = op == LANE_STORE || op == LANE_REDUCE || op == LANE_MSTORE;
  if (op == LANE_LOAD || op == LANE_MLOAD) {
    c = B->epoch;
  }
  for (int i = 0; !unique && i < B->n; i++) {
    const LaneNode *n = &B->nodes[i];
    if (n->op == op && n->a == a && n->b == b && n->c == c &&
        n->kind == kind && n->ext == ext) {
      return i;
    }
  }
  if (B->n == LANES_MAX_OPS) {
    return -1;
  }
  LaneNode *n = &B->nodes[B->n];
  n->op = op;
  n->a = a;
  n->b = b;
  n->c = c;
  n->kind = kind;
  n->ext = ext;
  return B->n++;
}

static int lane_const_node(LaneBuild *B, uint32_t bits, int kind, int ext) {
  int at = -1;
  for (int i = 0; i < B->n_consts; i++) {
    if (B->consts[i] == bits) {
      at = i;
      break;
    }
  }
  if (at < 0) {
    if (B->n_consts == LANES_MAX_CONSTS) {
      return -1;
    }
    at = B->n_consts;
    B->consts[B->n_consts++] = bits;
  }
  return lane_add(B, LANE_CONST, at, 0, 0, kind, ext);
}

static int lane_float_const(LaneBuild *B, double value) {
  float f = (float)value;
  uint32_t bits;
  memcpy(&bits, &f, sizeof(bits));
  return lane_const_node(B, bits, LK_F32, 0);
}

static int lane_int_const(LaneBuild *B, long long value) {
  int ext = 0;
  if (value >= 0 && value <= 0x7FFFFFFFLL) {
    ext = LX_S | LX_U;
  } else if (value < 0 && value >= -0x80000000LL) {
    ext = LX_S;
  } else if (value > 0x7FFFFFFFLL && value <= 0xFFFFFFFFLL) {
    ext = LX_U;
  } else {
    return -1;
  }
  return lane_const_node(B, (uint32_t)value, LK_INT, ext);
}

static const char *lane_param_type(const IRFunction *fn, const char *name) {
  for (size_t i = 0; fn->parameter_names && i < fn->parameter_count; i++) {
    if (fn->parameter_names[i] && strcmp(fn->parameter_names[i], name) == 0) {
      return fn->parameter_types ? fn->parameter_types[i] : NULL;
    }
  }
  return NULL;
}

static const char *lane_symbol_type(const IRFunction *fn, const char *name) {
  const char *type = ir_function_local_declared_type(fn, name);
  return type ? type : lane_param_type(fn, name);
}

static int lane_type_kind(const char *type, int *kind, int *ext) {
  if (!type) {
    return 0;
  }
  if (strcmp(type, "float32") == 0) {
    *kind = LK_F32;
    *ext = 0;
    return 1;
  }
  *kind = LK_INT;
  if (strcmp(type, "int32") == 0 || strcmp(type, "int16") == 0 ||
      strcmp(type, "int8") == 0) {
    *ext = LX_S;
    return 1;
  }
  if (strcmp(type, "uint32") == 0) {
    *ext = LX_U;
    return 1;
  }
  if (strcmp(type, "uint16") == 0 || strcmp(type, "uint8") == 0 ||
      strcmp(type, "char") == 0 || strcmp(type, "bool") == 0) {
    *ext = LX_S | LX_U;
    return 1;
  }
  if (strcmp(type, "int64") == 0 || strcmp(type, "uint64") == 0) {
    *ext = 0;
    return 1;
  }
  return 0;
}

static int lane_temp_kind_at(const IRFunction *fn, size_t before,
                             const char *name, int *kind, int *ext, int depth);
static int lane_is_comparison(const char *op);

static int lane_operand_kind(const IRFunction *fn, size_t before,
                             const IROperand *op, int *kind, int *ext,
                             int depth) {
  switch (op->kind) {
  case IR_OPERAND_INT:
    *kind = LK_INT;
    *ext = op->int_value >= 0 && op->int_value <= 0x7FFFFFFFLL ? (LX_S | LX_U)
           : op->int_value < 0 && op->int_value >= -0x80000000LL ? LX_S
                                                                 : 0;
    return 1;
  case IR_OPERAND_SYMBOL:
    return op->name &&
           lane_type_kind(lane_symbol_type(fn, op->name), kind, ext);
  case IR_OPERAND_TEMP:
    return op->name &&
           lane_temp_kind_at(fn, before, op->name, kind, ext, depth + 1);
  default:
    return 0;
  }
}

static int lane_int_binary_kind(const IRFunction *fn, size_t before,
                                const IRInstruction *def, int *kind, int *ext,
                                int depth) {
  int kl = 0, el = 0, kr = 0, er = 0;
  if (!def->text || !lane_operand_kind(fn, before, &def->lhs, &kl, &el, depth) ||
      !lane_operand_kind(fn, before, &def->rhs, &kr, &er, depth) ||
      kl != LK_INT || kr != LK_INT) {
    return 0;
  }
  *kind = LK_INT;
  if (lane_is_comparison(def->text)) {
    *ext = LX_S | LX_U;
  } else if (el == 0 || er == 0) {
    *ext = 0;
  } else {
    *ext = (el == LX_U || er == LX_U) ? LX_U : LX_S;
  }
  return 1;
}

static int lane_temp_kind(const IRFunction *fn, size_t before,
                          const char *name, int *kind, int *ext) {
  return lane_temp_kind_at(fn, before, name, kind, ext, 0);
}

static const IRInstruction *lane_temp_def(const IRFunction *fn, size_t before,
                                          const char *name) {
  const IRInstruction *found = NULL;
  for (size_t i = 0; i < fn->instruction_count; i++) {
    const IRInstruction *ins = &fn->instructions[i];
    if (ins->op == IR_OP_NOP || !ir_operand_is_temp(&ins->dest) ||
        strcmp(ins->dest.name, name) != 0) {
      continue;
    }
    if (found || i >= before) {
      return NULL;
    }
    found = ins;
  }
  return found;
}

static int lane_temp_kind_at(const IRFunction *fn, size_t before,
                             const char *name, int *kind, int *ext,
                             int depth) {
  const IRInstruction *def = lane_temp_def(fn, before, name);
  if (!def || depth > 8) {
    return 0;
  }
  if (def->op == IR_OP_BINARY && !def->is_float) {
    return lane_int_binary_kind(fn, (size_t)(def - fn->instructions), def,
                                kind, ext, depth);
  }
  if (def->op == IR_OP_ASSIGN) {
    return lane_operand_kind(fn, (size_t)(def - fn->instructions), &def->lhs,
                             kind, ext, depth);
  }
  if (def->is_float && def->float_bits == 32 && def->op != IR_OP_BINARY) {
    *kind = LK_F32;
    *ext = 0;
    return 1;
  }
  if (def->op == IR_OP_BINARY && def->is_float && def->float_bits == 32 &&
      def->text && strchr("+-*/", def->text[0]) && def->text[1] == '\0') {
    *kind = LK_F32;
    *ext = 0;
    return 1;
  }
  if (def->op == IR_OP_CAST && def->text) {
    return lane_type_kind(def->text, kind, ext);
  }
  if (def->op == IR_OP_CALL && def->text) {
    IRFunction *callee =
        ir_program_find_function(ir_optimize_get_program(), def->text);
    return callee && lane_type_kind(callee->return_type_name, kind, ext);
  }
  if (def->op == IR_OP_LOAD && !def->is_float &&
      def->rhs.kind == IR_OPERAND_INT) {
    long long size = def->rhs.int_value;
    *kind = LK_INT;
    *ext = size == 8   ? 0
           : size == 4 ? (def->is_unsigned ? LX_U : LX_S)
           : def->is_unsigned ? (LX_S | LX_U)
                              : LX_S;
    return size == 1 || size == 2 || size == 4 || size == 8;
  }
  return 0;
}

static int lane_scalar(LaneBuild *B, const IROperand *op) {
  int kind = 0;
  int ext = 0;
  int is_temp = op->kind == IR_OPERAND_TEMP;
  if (is_temp ? !lane_temp_kind(B->fn, B->cond_at, op->name, &kind, &ext)
              : !lane_type_kind(lane_symbol_type(B->fn, op->name), &kind,
                                &ext)) {
    return -1;
  }
  int at = -1;
  for (int i = 0; i < B->n_scalars; i++) {
    if (strcmp(B->scalars[i], op->name) == 0) {
      at = i;
    }
  }
  if (at < 0) {
    if (B->n_scalars == LANES_MAX_SCALARS) {
      return -1;
    }
    at = B->n_scalars++;
    B->scalars[at] = op->name;
    B->scalar_is_temp[at] = is_temp;
  }
  return lane_add(B, LANE_SCALAR, at, 0, 0, kind, ext);
}

static int lane_carry(LaneBuild *B, const char *name) {
  int at = -1;
  for (int i = 0; i < B->n_carried; i++) {
    if (strcmp(B->carried[i], name) == 0) {
      at = i;
    }
  }
  if (at < 0) {
    if (B->n_carried == LANES_MAX_REDUCE) {
      return -1;
    }
    at = B->n_carried++;
    B->carried[at] = name;
  }
  int kind = 0;
  int ext = 0;
  if (!lane_type_kind(lane_symbol_type(B->fn, name), &kind, &ext)) {
    return -1;
  }
  return lane_add(B, LANE_CARRY, at, 0, 0, kind, ext);
}

static int lane_value(LaneBuild *B, const LaneEnv *env, const IROperand *op) {
  switch (op->kind) {
  case IR_OPERAND_INT:
    return lane_int_const(B, op->int_value);
  case IR_OPERAND_FLOAT:
    return lane_float_const(B, op->float_value);
  case IR_OPERAND_TEMP:
  case IR_OPERAND_SYMBOL: {
    if (!op->name) {
      return -1;
    }
    int bound = lane_env_get(env, op->name);
    if (bound != LANE_NONE) {
      return bound;
    }
    if (op->kind == IR_OPERAND_SYMBOL && strcmp(op->name, B->iv) == 0) {
      return lane_add(B, LANE_IOTA, 0, 0, 0, LK_INT, LX_S | LX_U);
    }
    if (op->kind == IR_OPERAND_SYMBOL &&
        lane_names_contain(B->written, B->written_count, op->name)) {
      return lane_carry(B, op->name);
    }
    return lane_scalar(B, op);
  }
  default:
    return -1;
  }
}

static int lane_as_int(LaneBuild *B, int node) {
  if (node < 0) {
    return -1;
  }
  if (B->nodes[node].kind == LK_MASK) {
    return lane_add(B, LANE_MASK01, node, 0, 0, LK_INT, LX_S | LX_U);
  }
  return B->nodes[node].kind == LK_INT ? node : -1;
}

static int lane_not(LaneBuild *B, int mask) {
  int ones = lane_const_node(B, 0xFFFFFFFFu, LK_INT, LX_S);
  return ones < 0 ? -1 : lane_add(B, LANE_XOR, mask, ones, 0, LK_MASK, 0);
}

static int lane_condition(LaneBuild *B, int node, int *inverted) {
  if (node < 0) {
    return -1;
  }
  *inverted = 0;
  if (B->nodes[node].kind == LK_MASK) {
    return node;
  }
  *inverted = 1;
  if (B->nodes[node].kind == LK_INT) {
    int zero = lane_int_const(B, 0);
    return lane_add(B, LANE_ICMPEQ, node, zero, 0, LK_MASK, 0);
  }
  int zero = lane_float_const(B, 0.0);
  return lane_add(B, LANE_FCMP, node, zero, LANE_CMP_EQ, LK_MASK, 0);
}

static int lane_select(LaneBuild *B, int mask, int on, int off) {
  if (on == off) {
    return on;
  }
  if (on < 0 || off < 0 || mask < 0) {
    return -1;
  }
  int kind = B->nodes[on].kind;
  if (kind != B->nodes[off].kind) {
    on = lane_as_int(B, on);
    off = lane_as_int(B, off);
    if (on < 0 || off < 0) {
      return -1;
    }
    kind = LK_INT;
  }
  const LaneNode *m = &B->nodes[mask];
  if (kind == LK_F32 && m->op == LANE_FCMP && m->c == LANE_CMP_LT) {
    if (m->a == on && m->b == off) {
      return lane_add(B, LANE_FMIN, on, off, 0, LK_F32, 0);
    }
    if (m->a == off && m->b == on) {
      return lane_add(B, LANE_FMAX, on, off, 0, LK_F32, 0);
    }
  }
  int ext = B->nodes[on].ext & B->nodes[off].ext;
  return lane_add(B, LANE_SELECT, mask, on, off, kind, ext);
}

static int lane_float_binary(LaneBuild *B, const char *op, int a, int b) {
  if (B->nodes[a].kind != LK_F32 || B->nodes[b].kind != LK_F32) {
    return -1;
  }
  int code = op[1] ? 0
             : op[0] == '+' ? LANE_FADD
             : op[0] == '-' ? LANE_FSUB
             : op[0] == '*' ? LANE_FMUL
             : op[0] == '/' ? LANE_FDIV
                            : 0;
  if (code) {
    return lane_add(B, code, a, b, 0, LK_F32, 0);
  }
  if (strcmp(op, "<") == 0) return lane_add(B, LANE_FCMP, a, b, LANE_CMP_LT, LK_MASK, 0);
  if (strcmp(op, ">") == 0) return lane_add(B, LANE_FCMP, b, a, LANE_CMP_LT, LK_MASK, 0);
  if (strcmp(op, "<=") == 0) return lane_add(B, LANE_FCMP, a, b, LANE_CMP_LE, LK_MASK, 0);
  if (strcmp(op, ">=") == 0) return lane_add(B, LANE_FCMP, b, a, LANE_CMP_LE, LK_MASK, 0);
  if (strcmp(op, "==") == 0) return lane_add(B, LANE_FCMP, a, b, LANE_CMP_EQ, LK_MASK, 0);
  if (strcmp(op, "!=") == 0) return lane_add(B, LANE_FCMP, a, b, LANE_CMP_NE, LK_MASK, 0);
  return -1;
}

static int lane_clamp_against_iota(LaneBuild *B, int value, int other,
                                   int is_unsigned) {
  const LaneNode *n = &B->nodes[value];
  if (B->nodes[other].op != LANE_IOTA || n->op != LANE_SCALAR || n->ext != 0 ||
      n->kind != LK_INT || n->c != 0) {
    return value;
  }
  return lane_add(B, LANE_SCALAR, n->a, 0, is_unsigned ? 2 : 1, LK_INT,
                  is_unsigned ? (LX_S | LX_U) : LX_S);
}

static int lane_int_compare(LaneBuild *B, const char *op, int a, int b,
                            int is_unsigned) {
  int clamped_a = lane_clamp_against_iota(B, a, b, is_unsigned);
  b = lane_clamp_against_iota(B, b, a, is_unsigned);
  a = clamped_a;
  if (a < 0 || b < 0) {
    return -1;
  }
  int need = is_unsigned ? LX_U : LX_S;
  if ((B->nodes[a].ext & need) == 0 || (B->nodes[b].ext & need) == 0) {
    return -1;
  }
  if (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0) {
    int eq = lane_add(B, LANE_ICMPEQ, a, b, 0, LK_MASK, 0);
    return op[0] == '=' ? eq : lane_not(B, eq);
  }
  if (is_unsigned) {
    int bias = lane_const_node(B, 0x80000000u, LK_INT, LX_U);
    a = lane_add(B, LANE_XOR, a, bias, 0, LK_INT, LX_S);
    b = lane_add(B, LANE_XOR, b, bias, 0, LK_INT, LX_S);
    if (a < 0 || b < 0) {
      return -1;
    }
  }
  if (strcmp(op, ">") == 0) return lane_add(B, LANE_ICMPGT, a, b, 0, LK_MASK, 0);
  if (strcmp(op, "<") == 0) return lane_add(B, LANE_ICMPGT, b, a, 0, LK_MASK, 0);
  if (strcmp(op, "<=") == 0) return lane_not(B, lane_add(B, LANE_ICMPGT, a, b, 0, LK_MASK, 0));
  if (strcmp(op, ">=") == 0) return lane_not(B, lane_add(B, LANE_ICMPGT, b, a, 0, LK_MASK, 0));
  return -1;
}

static int lane_is_comparison(const char *op) {
  return strcmp(op, "<") == 0 || strcmp(op, "<=") == 0 ||
         strcmp(op, ">") == 0 || strcmp(op, ">=") == 0 ||
         strcmp(op, "==") == 0 || strcmp(op, "!=") == 0;
}

static int lane_int_binary(LaneBuild *B, const IRInstruction *ins, int a,
                           int b) {
  const char *op = ins->text;
  if (lane_is_comparison(op)) {
    return lane_int_compare(B, op, a, b, ins->is_unsigned);
  }
  int ea = B->nodes[a].ext;
  int eb = B->nodes[b].ext;
  if (strcmp(op, "+") == 0) return lane_add(B, LANE_IADD, a, b, 0, LK_INT, 0);
  if (strcmp(op, "-") == 0) return lane_add(B, LANE_ISUB, a, b, 0, LK_INT, 0);
  if (strcmp(op, "*") == 0) return lane_add(B, LANE_IMUL, a, b, 0, LK_INT, 0);
  if (strcmp(op, "&") == 0) {
    int both = (ea & eb) | ((ea == (LX_S | LX_U) || eb == (LX_S | LX_U))
                                ? (LX_S | LX_U)
                                : 0);
    return lane_add(B, LANE_AND, a, b, 0, LK_INT, both);
  }
  if (strcmp(op, "|") == 0) return lane_add(B, LANE_OR, a, b, 0, LK_INT, ea & eb);
  if (strcmp(op, "^") == 0) return lane_add(B, LANE_XOR, a, b, 0, LK_INT, ea & eb);
  if (strcmp(op, "<<") == 0 || strcmp(op, ">>") == 0) {
    const LaneNode *count = &B->nodes[b];
    int left = op[0] == '<';
    int logical = !left && (ins->is_unsigned || (ea & LX_S) == 0);
    if (!left && (ea & (logical ? LX_U : LX_S)) == 0) {
      return -1;
    }
    if (count->op == LANE_CONST) {
      uint32_t k = B->consts[count->a];
      if (k > 31) {
        return -1;
      }
      int code = left ? LANE_SHLI : logical ? LANE_SHRI : LANE_SARI;
      int ext = left ? 0 : (k > 0 && logical ? (LX_S | LX_U) : ea);
      return lane_add(B, code, a, (int)k, 0, LK_INT, ext);
    }
    if ((eb & LX_U) == 0) {
      return -1;
    }
    int code = left ? LANE_SHLV : logical ? LANE_SHRV : LANE_SARV;
    return lane_add(B, code, a, b, 0, LK_INT, left ? 0 : ea);
  }
  return -1;
}

static int lane_binary(LaneBuild *B, const LaneEnv *env,
                       const IRInstruction *ins) {
  if (!ins->text) {
    return -1;
  }
  int a = lane_value(B, env, &ins->lhs);
  int b = lane_value(B, env, &ins->rhs);
  if (a < 0 || b < 0) {
    return -1;
  }
  if (ins->is_float) {
    return ins->float_bits == 32 ? lane_float_binary(B, ins->text, a, b) : -1;
  }
  a = lane_as_int(B, a);
  b = lane_as_int(B, b);
  if (a < 0 || b < 0) {
    return -1;
  }
  return lane_int_binary(B, ins, a, b);
}

static int lane_unary(LaneBuild *B, const LaneEnv *env,
                      const IRInstruction *ins) {
  int a = lane_value(B, env, &ins->lhs);
  const char *op = ins->text ? ins->text : "";
  if (a < 0) {
    return -1;
  }
  int kind = B->nodes[a].kind;
  if (strcmp(op, "-") == 0 && kind == LK_F32) {
    int sign = lane_const_node(B, 0x80000000u, LK_F32, 0);
    return lane_add(B, LANE_XOR, a, sign, 0, LK_F32, 0);
  }
  if (strcmp(op, "sqrt") == 0 && kind == LK_F32) {
    return lane_add(B, LANE_FSQRT, a, 0, 0, LK_F32, 0);
  }
  if (strcmp(op, "f32bits") == 0 && kind == LK_F32) {
    return lane_add(B, LANE_COPY, a, 0, 0, LK_INT, LX_U);
  }
  if (strcmp(op, "bitsf") == 0 && ins->float_bits == 32 && kind == LK_INT) {
    return lane_add(B, LANE_COPY, a, 0, 0, LK_F32, 0);
  }
  if (strcmp(op, "+") == 0) {
    return a;
  }
  if (strcmp(op, "!") == 0) {
    int inverted = 0;
    int m = lane_condition(B, a, &inverted);
    return inverted ? m : lane_not(B, m);
  }
  a = lane_as_int(B, a);
  if (a < 0) {
    return -1;
  }
  if (strcmp(op, "-") == 0) {
    return lane_add(B, LANE_ISUB, lane_int_const(B, 0), a, 0, LK_INT, 0);
  }
  if (strcmp(op, "~") == 0) {
    int ones = lane_const_node(B, 0xFFFFFFFFu, LK_INT, LX_S);
    return lane_add(B, LANE_XOR, a, ones, 0, LK_INT, B->nodes[a].ext);
  }
  return -1;
}

static int lane_narrow(LaneBuild *B, int a, int bits, int is_signed) {
  if (bits == 32) {
    return lane_add(B, LANE_COPY, a, 0, 0, LK_INT, is_signed ? LX_S : LX_U);
  }
  if (!is_signed) {
    int mask = lane_int_const(B, (1LL << bits) - 1);
    return lane_add(B, LANE_AND, a, mask, 0, LK_INT, LX_S | LX_U);
  }
  int up = lane_add(B, LANE_SHLI, a, 32 - bits, 0, LK_INT, 0);
  return up < 0 ? -1 : lane_add(B, LANE_SARI, up, 32 - bits, 0, LK_INT, LX_S);
}

static int lane_int_op(LaneBuild *B, int op, int a, int b) {
  return a < 0 || b < 0 ? -1 : lane_add(B, op, a, b, 0, LK_INT, 0);
}

static int lane_float_to_int_bits(LaneBuild *B, int f) {
  int bits = lane_add(B, LANE_COPY, f, 0, 0, LK_INT, 0);
  int fast = lane_add(B, LANE_CVTF2I, f, 0, 0, LK_INT, 0);
  int exp = lane_int_op(B, LANE_AND, lane_add(B, LANE_SHRI, bits, 23, 0, LK_INT, 0),
                        lane_int_const(B, 0xFF));
  int mant = lane_int_op(
      B, LANE_OR, lane_int_op(B, LANE_AND, bits, lane_int_const(B, 0x7FFFFF)),
      lane_int_const(B, 0x800000));
  int mag = lane_int_op(B, LANE_SHLV, mant,
                        lane_int_op(B, LANE_ISUB, exp, lane_int_const(B, 150)));
  int sign = lane_add(B, LANE_SARI, bits, 31, 0, LK_INT, 0);
  int wrapped = lane_int_op(B, LANE_ISUB, lane_int_op(B, LANE_XOR, mag, sign),
                            sign);
  int bad = lane_int_op(B, LANE_ICMPEQ, fast, lane_int_const(B, 0x80000000LL));
  if (bad < 0 || wrapped < 0 || fast < 0) {
    return -1;
  }
  return lane_add(B, LANE_SELECT, bad, wrapped, fast, LK_INT, 0);
}

static int lane_cast(LaneBuild *B, const LaneEnv *env,
                     const IRInstruction *ins) {
  int a = lane_value(B, env, &ins->lhs);
  const char *to = ins->text;
  if (a < 0 || !to) {
    return -1;
  }
  int kind = B->nodes[a].kind;
  int ext = B->nodes[a].ext;
  if (strcmp(to, "float32") == 0) {
    if (kind == LK_F32) {
      return a;
    }
    a = lane_as_int(B, a);
    if (a < 0 || (B->nodes[a].ext & LX_S) == 0) {
      return -1;
    }
    return lane_add(B, LANE_CVTI2F, a, 0, 0, LK_F32, 0);
  }
  a = kind == LK_F32 ? lane_float_to_int_bits(B, a) : lane_as_int(B, a);
  if (a < 0) {
    return -1;
  }
  ext = B->nodes[a].ext;
  if (strcmp(to, "int64") == 0 || strcmp(to, "uint64") == 0) {
    return ext || kind == LK_F32 ? a : -1;
  }
  if (strcmp(to, "int32") == 0) return lane_narrow(B, a, 32, 1);
  if (strcmp(to, "uint32") == 0) return lane_narrow(B, a, 32, 0);
  if (strcmp(to, "int16") == 0) return lane_narrow(B, a, 16, 1);
  if (strcmp(to, "uint16") == 0) return lane_narrow(B, a, 16, 0);
  if (strcmp(to, "int8") == 0) return lane_narrow(B, a, 8, 1);
  if (strcmp(to, "uint8") == 0 || strcmp(to, "char") == 0) {
    return lane_narrow(B, a, 8, 0);
  }
  return -1;
}

static int lane_decode_address(LaneBuild *B, size_t before,
                               const IROperand *addr, const char **base_out) {
  if (addr->kind != IR_OPERAND_TEMP || !addr->name) {
    return 0;
  }
  const IRInstruction *add =
      ir_find_temp_producer_before(B->fn, before, addr->name);
  if (!add || add->op != IR_OP_BINARY || add->is_float || !add->text ||
      strcmp(add->text, "+") != 0) {
    return 0;
  }
  const IROperand *base = &add->lhs;
  const IROperand *index = &add->rhs;
  if (!ir_operand_is_symbol(base)) {
    base = &add->rhs;
    index = &add->lhs;
  }
  if (!ir_operand_is_symbol(base) || index->kind != IR_OPERAND_TEMP ||
      !index->name) {
    return 0;
  }
  const IRInstruction *shl =
      ir_find_temp_producer_before(B->fn, before, index->name);
  if (!shl || shl->op != IR_OP_BINARY || shl->is_float || !shl->text ||
      strcmp(shl->text, "<<") != 0 ||
      !ir_operand_is_symbol_named(&shl->lhs, B->iv) ||
      shl->rhs.kind != IR_OPERAND_INT || shl->rhs.int_value != 2) {
    return 0;
  }
  const char *type = lane_symbol_type(B->fn, base->name);
  size_t len = type ? strlen(type) : 0;
  if (len < 2 || type[len - 1] != '*' ||
      lane_names_contain(B->written, B->written_count, base->name)) {
    return 0;
  }
  *base_out = base->name;
  return 1;
}

static int lane_base_index(LaneBuild *B, const char *base) {
  for (int i = 0; i < B->n_bases; i++) {
    if (strcmp(B->bases[i], base) == 0) {
      return i;
    }
  }
  if (B->n_bases == LANES_MAX_BASES) {
    return -1;
  }
  B->forward[B->n_bases] = -1;
  B->bases[B->n_bases] = base;
  return B->n_bases++;
}

static void lane_refuse_width(LaneBuild *B, const IRInstruction *ins,
                              const char *verb) {
  char reason[320];
  if (ins->rhs.kind != IR_OPERAND_INT || ins->rhs.int_value == 4) {
    return;
  }
  snprintf(reason, sizeof(reason),
           "line %zu %s %lld-byte elements, and the lanes are 32 bits wide",
           (size_t)ins->location.line, verb, ins->rhs.int_value);
  lane_why(B, LANE_WHY_WIDTH, reason,
           "store the elements as float32, int32 or uint32 when their range "
           "fits");
}

static int lane_load(LaneBuild *B, const IRInstruction *ins, size_t at) {
  const char *base = NULL;
  lane_refuse_width(B, ins, "reads");
  if (ins->rhs.kind != IR_OPERAND_INT || ins->rhs.int_value != 4 ||
      !lane_decode_address(B, at, &ins->lhs, &base)) {
    return -1;
  }
  int bi = lane_base_index(B, base);
  if (bi < 0) {
    return -1;
  }
  if (B->forward[bi] >= 0) {
    return B->forward[bi];
  }
  int kind = ins->is_float ? LK_F32 : LK_INT;
  int ext = ins->is_float ? 0 : (ins->is_unsigned ? LX_U : LX_S);
  if (ins->is_float && ins->float_bits != 32 && ins->float_bits != 0) {
    return -1;
  }
  if (B->path >= 0 && !(B->plain_access & (1u << bi))) {
    return lane_add(B, LANE_MLOAD, bi, B->path, 0, kind, ext);
  }
  B->plain_access |= 1u << bi;
  return lane_add(B, LANE_LOAD, bi, 0, 0, kind, ext);
}

static int lane_store(LaneBuild *B, const LaneEnv *env,
                      const IRInstruction *ins, size_t at) {
  const char *base = NULL;
  lane_refuse_width(B, ins, "writes");
  if ((B->depth && B->path < 0) || ins->rhs.kind != IR_OPERAND_INT ||
      ins->rhs.int_value != 4 ||
      !lane_decode_address(B, at, &ins->dest, &base)) {
    return 0;
  }
  int value = lane_value(B, env, &ins->lhs);
  int bi = lane_base_index(B, base);
  if (value < 0 || bi < 0) {
    return 0;
  }
  if (B->nodes[value].kind == LK_MASK) {
    value = lane_as_int(B, value);
  }
  int kind = value < 0 ? 0 : B->nodes[value].kind;
  int stored = value < 0         ? -1
               : B->path >= 0 ? lane_add(B, LANE_MSTORE, bi, value, B->path,
                                         kind, 0)
                              : lane_add(B, LANE_STORE, bi, value, 0, kind, 0);
  if (stored < 0) {
    return 0;
  }
  B->stored |= 1u << bi;
  if (B->path < 0) {
    B->plain_access |= 1u << bi;
  }
  B->forward[bi] = B->path < 0 ? value : -1;
  B->epoch++;
  return 1;
}

static int lane_is_address_temp(const LaneBuild *B, const IROperand *op) {
  return op->kind == IR_OPERAND_TEMP && op->name &&
         lane_names_contain(B->address_temps, B->address_temp_count,
                            op->name);
}

static int lane_eval_range(LaneBuild *B, LaneEnv *env, size_t lo, size_t hi,
                           const char *exit);

static size_t lane_label_index(const IRFunction *fn, size_t lo, size_t hi,
                               const char *label) {
  for (size_t i = lo; i < hi; i++) {
    const IRInstruction *ins = &fn->instructions[i];
    if (ins->op == IR_OP_LABEL && ins->text && strcmp(ins->text, label) == 0) {
      return i;
    }
  }
  return (size_t)-1;
}

static int lane_label_uses(const IRFunction *fn, const char *label) {
  int uses = 0;
  for (size_t i = 0; i < fn->instruction_count; i++) {
    const IRInstruction *ins = &fn->instructions[i];
    if ((ins->op == IR_OP_JUMP || ins->op == IR_OP_BRANCH_ZERO ||
         ins->op == IR_OP_BRANCH_EQ) &&
        ins->text && strcmp(ins->text, label) == 0) {
      uses++;
    }
  }
  return uses;
}

static int lane_merge(LaneBuild *B, LaneEnv *env, const LaneEnv *on,
                      const LaneEnv *off, int mask, int inverted) {
  const LaneEnv *sides[2] = {on, off};
  for (int s = 0; s < 2; s++) {
    for (int i = 0; i < sides[s]->count; i++) {
      const char *name = sides[s]->items[i].name;
      int a = lane_env_get(on, name);
      int b = lane_env_get(off, name);
      if ((a == LANE_NONE || b == LANE_NONE) &&
          lane_names_contain(B->written, B->written_count, name)) {
        int carried = lane_carry(B, name);
        a = a == LANE_NONE ? carried : a;
        b = b == LANE_NONE ? carried : b;
      }
      int merged;
      if (a == b) {
        merged = a;
      } else if (a < 0 || b < 0) {
        merged = LANE_POISON;
      } else {
        merged = inverted ? lane_select(B, mask, b, a)
                          : lane_select(B, mask, a, b);
        if (merged < 0) {
          return 0;
        }
      }
      if (!lane_env_set(env, name, merged)) {
        return 0;
      }
    }
  }
  return 1;
}

static int lane_label_used_only_in(const IRFunction *fn, const char *label,
                                   size_t lo, size_t hi) {
  int inside = 0;
  for (size_t i = lo; i < hi; i++) {
    const IRInstruction *ins = &fn->instructions[i];
    inside += (ins->op == IR_OP_JUMP || ins->op == IR_OP_BRANCH_ZERO ||
               ins->op == IR_OP_BRANCH_EQ) &&
              ins->text && strcmp(ins->text, label) == 0;
  }
  return inside > 0 && inside == lane_label_uses(fn, label);
}

typedef struct {
  size_t then_lo;
  size_t then_hi;
  size_t else_lo;
  size_t else_hi;
  const char *then_exit;
  const char *else_exit;
} LaneArms;

static int lane_diamond_shape(const IRFunction *fn, size_t at, size_t hi,
                              LaneArms *arms, size_t *next) {
  const IRInstruction *br = &fn->instructions[at];
  size_t else_at =
      br->text ? lane_label_index(fn, at + 1, hi, br->text) : (size_t)-1;
  if (else_at == (size_t)-1 ||
      !lane_label_used_only_in(fn, br->text, at, else_at)) {
    return 0;
  }
  size_t jump_at = else_at;
  while (jump_at > at + 1 && fn->instructions[jump_at - 1].op == IR_OP_NOP) {
    jump_at--;
  }
  *arms = (LaneArms){at + 1, else_at, else_at + 1, else_at + 1, br->text, NULL};
  *next = else_at + 1;
  if (jump_at > at + 1 && fn->instructions[jump_at - 1].op == IR_OP_JUMP) {
    const char *end_label = fn->instructions[jump_at - 1].text;
    size_t end_at = end_label ? lane_label_index(fn, else_at + 1, hi, end_label)
                              : (size_t)-1;
    if (end_at == (size_t)-1 ||
        !lane_label_used_only_in(fn, end_label, at, end_at)) {
      return 0;
    }
    arms->then_hi = jump_at - 1;
    arms->else_hi = end_at;
    arms->then_exit = end_label;
    arms->else_exit = end_label;
    *next = end_at + 1;
  }
  return 1;
}

static int lane_branch_masks(LaneBuild *B, const LaneEnv *env,
                             const IRInstruction *br, int *mask,
                             int *inverted, int *then_mask, int *else_mask) {
  *mask = lane_condition(B, lane_value(B, env, &br->lhs), inverted);
  int ones = lane_int_const(B, 0xFFFFFFFFLL);
  if (*mask < 0 || ones < 0) {
    return 0;
  }
  int flipped = lane_add(B, LANE_XOR, *mask, ones, 0, LK_MASK, 0);
  *then_mask = *inverted ? flipped : *mask;
  *else_mask = *inverted ? *mask : flipped;
  if (B->path >= 0 && flipped >= 0) {
    *then_mask = lane_add(B, LANE_AND, B->path, *then_mask, 0, LK_MASK, 0);
    *else_mask = lane_add(B, LANE_AND, B->path, *else_mask, 0, LK_MASK, 0);
  }
  return flipped >= 0 && *then_mask >= 0 && *else_mask >= 0;
}

static int lane_eval_arm(LaneBuild *B, LaneEnv *arm, const LaneEnv *env,
                         int path, size_t lo, size_t hi, const char *exit) {
  int outer = B->path;
  B->depth++;
  B->path = path;
  int ok = lane_env_copy(arm, env) && lane_eval_range(B, arm, lo, hi, exit);
  B->path = outer;
  B->depth--;
  return ok;
}

static int lane_diamond(LaneBuild *B, LaneEnv *env, size_t at, size_t hi,
                        size_t *next) {
  LaneArms arms;
  int mask = -1;
  int inverted = 0;
  int then_mask = -1;
  int else_mask = -1;
  if (!lane_diamond_shape(B->fn, at, hi, &arms, next)) {
    char reason[320];
    snprintf(reason, sizeof(reason),
             "the branch on line %zu does not nest as an if/else whose arms "
             "rejoin in order, the only branch shape the lanes can merge",
             (size_t)B->fn->instructions[at].location.line);
    lane_why(B, LANE_WHY_SHAPE, reason,
             "write the condition as nested if/else blocks, and move a "
             "`break`, `continue` or early `return` into a loop of its own");
    return 0;
  }
  if (!lane_branch_masks(B, env, &B->fn->instructions[at], &mask, &inverted,
                         &then_mask, &else_mask)) {
    return 0;
  }
  LaneEnv on = {0};
  LaneEnv off = {0};
  int ok = lane_eval_arm(B, &on, env, then_mask, arms.then_lo, arms.then_hi,
                         arms.then_exit) &&
           lane_eval_arm(B, &off, env, else_mask, arms.else_lo, arms.else_hi,
                         arms.else_exit) &&
           lane_merge(B, env, &on, &off, mask, inverted);
  free(on.items);
  free(off.items);
  return ok;
}

static int lane_guarded_tail(LaneBuild *B, LaneEnv *env, size_t at,
                             size_t hi, const char *exit) {
  int mask = -1;
  int inverted = 0;
  int then_mask = -1;
  int else_mask = -1;
  if (!lane_branch_masks(B, env, &B->fn->instructions[at], &mask, &inverted,
                         &then_mask, &else_mask)) {
    return 0;
  }
  LaneEnv on = {0};
  LaneEnv off = {0};
  int ok = lane_eval_arm(B, &on, env, then_mask, at + 1, hi, exit) &&
           lane_env_copy(&off, env) &&
           lane_merge(B, env, &on, &off, mask, inverted);
  free(on.items);
  free(off.items);
  return ok;
}

static int lane_declare(LaneBuild *B, LaneEnv *env, const IRInstruction *ins) {
  int kind = 0;
  int ext = 0;
  if (!ins->dest.name || !lane_type_kind(ins->text, &kind, &ext)) {
    return 0;
  }
  int zero = kind == LK_F32 ? lane_float_const(B, 0.0) : lane_int_const(B, 0);
  return zero >= 0 && lane_env_set(env, ins->dest.name, zero);
}

static int lane_assign(LaneBuild *B, LaneEnv *env, const IRInstruction *ins) {
  int value;
  if (ins->lhs.kind == IR_OPERAND_FLOAT && ins->dest.kind == IR_OPERAND_SYMBOL) {
    int kind = 0;
    int ext = 0;
    if (!lane_type_kind(lane_symbol_type(B->fn, ins->dest.name), &kind,
                        &ext) ||
        kind != LK_F32) {
      return 0;
    }
    value = lane_float_const(B, ins->lhs.float_value);
  } else {
    value = lane_value(B, env, &ins->lhs);
  }
  return value >= 0 && lane_env_set(env, ins->dest.name, value);
}

static void lane_source_name(const char *raw, char *out, size_t cap) {
  ir_display_name(raw, out, cap);
  char *ssa = strstr(out, "__ssa");
  if (ssa) {
    *ssa = '\0';
  }
}

static const IROperand *lane_carried_operand(const LaneBuild *B,
                                             const LaneEnv *env,
                                             const IRInstruction *ins) {
  const IROperand *sides[2] = {&ins->lhs, &ins->rhs};
  for (int s = 0; s < 2; s++) {
    const IROperand *op = sides[s];
    if (op->kind == IR_OPERAND_SYMBOL && op->name &&
        lane_names_contain(B->written, B->written_count, op->name) &&
        lane_env_get(env, op->name) == LANE_NONE) {
      return op;
    }
  }
  return NULL;
}

static int lane_refuse_carried_op(LaneBuild *B, const LaneEnv *env,
                                  const IRInstruction *ins) {
  const IROperand *carried = lane_carried_operand(B, env, ins);
  if (!carried || !ins->text || strcmp(ins->text, "+") == 0 ||
      strcmp(ins->text, "-") == 0) {
    return 0;
  }
  char name[96];
  char reason[320];
  lane_source_name(carried->name, name, sizeof(name));
  snprintf(reason, sizeof(reason),
           "`%s` carries its value from one iteration into the next through "
           "`%s` on line %zu, and only a running sum regroups into lanes",
           name, ins->text, (size_t)ins->location.line);
  lane_why(B, LANE_WHY_CARRIED, reason,
           "when this chain is the algorithm, as in a hash or a filter, the "
           "loop is already at its scalar floor");
  return 1;
}

static void lane_refuse_op(LaneBuild *B, const LaneEnv *env,
                           const IRInstruction *ins) {
  char reason[320];
  char fix[240];
  size_t line = ins->location.line;
  const char *text = ins->text ? ins->text : ir_opcode_name(ins->op);
  if (lane_refuse_carried_op(B, env, ins)) {
    return;
  }
  int divides = ins->op == IR_OP_BINARY && !ins->is_float && ins->text &&
                (strcmp(ins->text, "/") == 0 || strcmp(ins->text, "%") == 0);
  if (ins->is_float && ins->float_bits == 64) {
    snprintf(reason, sizeof(reason),
             "line %zu computes `%s` in float64, and the lane vectorizer runs "
             "eight 32-bit lanes",
             line, text);
    snprintf(fix, sizeof(fix),
             "keep the arithmetic in float32; a float64 sum of float32 "
             "products is the one float64 shape with a kernel of its own");
  } else if (divides) {
    snprintf(reason, sizeof(reason),
             "line %zu divides integers, and AVX2 has no integer divide",
             line);
    snprintf(fix, sizeof(fix),
             "divide by a power of two with a shift, or move the division "
             "out of the loop");
  } else if (ins->op == IR_OP_CAST && strcmp(text, "float64") == 0) {
    snprintf(reason, sizeof(reason),
             "line %zu widens a value to float64, and the lanes are 32 bits "
             "wide",
             line);
    snprintf(fix, sizeof(fix),
             "keep the arithmetic in float32; a float64 sum of float32 "
             "products is the one float64 shape with a kernel of its own");
  } else if (ins->op == IR_OP_CAST) {
    snprintf(reason, sizeof(reason),
             "line %zu converts to `%s`, and the 32-bit lanes cannot hold "
             "that value exactly",
             line, text);
    snprintf(fix, sizeof(fix),
             "convert between float32 and 32-bit integers inside the loop, "
             "and widen after it");
  } else {
    snprintf(reason, sizeof(reason),
             "line %zu needs `%s` on values wider than the 32-bit lanes hold "
             "exactly",
             line, text);
    snprintf(fix, sizeof(fix),
             "declare the values int32, uint32 or float32 when their range "
             "fits");
  }
  lane_why(B, LANE_WHY_OP, reason, fix);
}

static int lane_eval_one(LaneBuild *B, LaneEnv *env, size_t i) {
  const IRInstruction *ins = &B->fn->instructions[i];
  int value = -1;
  switch (ins->op) {
  case IR_OP_NOP:
    return 1;
  case IR_OP_LABEL:
    return ins->text && lane_label_uses(B->fn, ins->text) == 0;
  case IR_OP_DECLARE_LOCAL:
    return lane_declare(B, env, ins);
  case IR_OP_ASSIGN:
    return ins->dest.name && lane_assign(B, env, ins);
  case IR_OP_STORE:
    return lane_store(B, env, ins, i);
  case IR_OP_LOAD:
    value = lane_load(B, ins, i);
    break;
  case IR_OP_BINARY:
    if (lane_is_address_temp(B, &ins->dest)) {
      return 1;
    }
    value = lane_binary(B, env, ins);
    break;
  case IR_OP_UNARY:
    value = lane_unary(B, env, ins);
    break;
  case IR_OP_CAST:
    value = lane_cast(B, env, ins);
    break;
  case IR_OP_SELECT: {
    int inverted = 0;
    int mask = lane_condition(B, lane_value(B, env, &ins->lhs), &inverted);
    int on = lane_value(B, env, &ins->rhs);
    int off = ins->argument_count == 1
                  ? lane_value(B, env, &ins->arguments[0])
                  : -1;
    value = inverted ? lane_select(B, mask, off, on)
                     : lane_select(B, mask, on, off);
    break;
  }
  default:
    return 0;
  }
  if (value < 0 && ins->op != IR_OP_LOAD) {
    lane_refuse_op(B, env, ins);
  }
  if (value < 0 || !ins->dest.name ||
      (ins->dest.kind != IR_OPERAND_TEMP &&
       ins->dest.kind != IR_OPERAND_SYMBOL)) {
    return 0;
  }
  return lane_env_set(env, ins->dest.name, value);
}

static int lane_eval_range(LaneBuild *B, LaneEnv *env, size_t lo, size_t hi,
                           const char *exit) {
  for (size_t i = lo; i < hi; i++) {
    const IRInstruction *ins = &B->fn->instructions[i];
    if (ins->op == IR_OP_BRANCH_ZERO && exit && ins->text &&
        strcmp(ins->text, exit) == 0) {
      return lane_guarded_tail(B, env, i, hi, exit);
    }
    if (ins->op == IR_OP_BRANCH_ZERO) {
      size_t next = i;
      if (!lane_diamond(B, env, i, hi, &next)) {
        return 0;
      }
      i = next - 1;
      continue;
    }
    if (!lane_eval_one(B, env, i)) {
      return 0;
    }
  }
  return 1;
}

static int lane_collect_names(LaneBuild *B) {
  size_t cap = B->hi - B->lo + 1;
  B->written = calloc(cap, sizeof(char *));
  B->address_temps = calloc(cap, sizeof(char *));
  if (!B->written || !B->address_temps) {
    return 0;
  }
  for (size_t i = B->lo; i < B->hi; i++) {
    const IRInstruction *ins = &B->fn->instructions[i];
    if (ins->op == IR_OP_INLINE_ASM || ins->op == IR_OP_CALL ||
        ins->op == IR_OP_CALL_INDIRECT) {
      return 0;
    }
    if (ir_instruction_writes_destination(ins) &&
        ins->dest.kind == IR_OPERAND_SYMBOL && ins->dest.name &&
        !lane_names_contain(B->written, B->written_count, ins->dest.name)) {
      if (strcmp(ins->dest.name, B->iv) == 0 ||
          ir_symbol_address_taken(B->fn, ins->dest.name)) {
        return 0;
      }
      B->written[B->written_count++] = ins->dest.name;
    }
    const IROperand *addr = ins->op == IR_OP_LOAD    ? &ins->lhs
                            : ins->op == IR_OP_STORE ? &ins->dest
                                                     : NULL;
    if (addr && addr->kind == IR_OPERAND_TEMP && addr->name &&
        !lane_names_contain(B->address_temps, B->address_temp_count,
                            addr->name)) {
      B->address_temps[B->address_temp_count++] = addr->name;
    }
  }
  for (size_t i = B->lo; i < B->hi; i++) {
    const IRInstruction *ins = &B->fn->instructions[i];
    const IROperand *reads[2] = {&ins->lhs, &ins->rhs};
    int as_address = ins->op == IR_OP_LOAD;
    for (int r = 0; r < 2; r++) {
      if (lane_is_address_temp(B, reads[r]) && !(as_address && r == 0)) {
        return 0;
      }
    }
    for (size_t a = 0; a < ins->argument_count; a++) {
      if (lane_is_address_temp(B, &ins->arguments[a])) {
        return 0;
      }
    }
  }
  return 1;
}

static int lane_value_operands(const LaneNode *n, int out[3]) {
  LaneInsn insn = {n->op, n->a, n->b, n->c};
  return ir_lanes_operands(&insn, out);
}

static int lane_is_root(int op) {
  return op == LANE_STORE || op == LANE_REDUCE || op == LANE_MSTORE;
}

static void lane_mark_live(const LaneBuild *B, unsigned char *live) {
  memset(live, 0, (size_t)B->n);
  for (int i = B->n; i-- > 0;) {
    const LaneNode *n = &B->nodes[i];
    if (lane_is_root(n->op)) {
      live[i] = 1;
    }
    if (!live[i]) {
      continue;
    }
    int ops[3];
    int count = lane_value_operands(n, ops);
    for (int k = 0; k < count; k++) {
      live[ops[k]] = 1;
    }
  }
}

static int lane_depends_on(const LaneBuild *B, int node, int target) {
  if (node < 0 || target < 0 || node < target) {
    return 0;
  }
  unsigned char reach[LANES_MAX_OPS];
  memset(reach, 0, (size_t)(node + 1));
  reach[node] = 1;
  for (int i = node; i > target; i--) {
    if (!reach[i]) {
      continue;
    }
    int ops[3];
    int count = lane_value_operands(&B->nodes[i], ops);
    for (int k = 0; k < count; k++) {
      if (ops[k] >= 0) {
        reach[ops[k]] = 1;
      }
    }
  }
  return reach[target];
}

static int lane_negate(LaneBuild *B, int value, int is_float) {
  if (is_float) {
    int sign = lane_const_node(B, 0x80000000u, LK_INT, LX_U);
    return sign < 0 ? -1 : lane_add(B, LANE_XOR, value, sign, 0, LK_F32, 0);
  }
  int zero = lane_int_const(B, 0);
  return zero < 0 ? -1 : lane_add(B, LANE_ISUB, zero, value, 0, LK_INT, 0);
}

static int lane_addend(LaneBuild *B, int node, int carry, int *op) {
  while (node >= 0 && B->nodes[node].op == LANE_COPY &&
         B->nodes[node].kind == LK_INT) {
    node = B->nodes[node].a;
  }
  if (node < 0 || node == carry) {
    return -1;
  }
  LaneNode n = B->nodes[node];
  int is_float = n.op == LANE_FADD || n.op == LANE_FSUB;
  int adds = n.op == LANE_FADD || n.op == LANE_IADD;
  if (!adds && n.op != LANE_FSUB && n.op != LANE_ISUB) {
    return -1;
  }
  int sum_op = is_float ? LANE_FADD : LANE_IADD;
  if (*op && *op != sum_op) {
    return -1;
  }
  *op = sum_op;
  int left = lane_depends_on(B, n.a, carry);
  int right = lane_depends_on(B, n.b, carry);
  if (left == right || (!adds && right)) {
    return -1;
  }
  int inner = left ? n.a : n.b;
  int other = left ? n.b : n.a;
  int rest = inner == carry ? -1 : lane_addend(B, inner, carry, op);
  if (inner != carry && rest < 0) {
    return -1;
  }
  int kind = is_float ? LK_F32 : LK_INT;
  if (!adds) {
    return rest < 0 ? lane_negate(B, other, is_float)
                    : lane_add(B, n.op, rest, other, 0, kind, 0);
  }
  return rest < 0 ? other : lane_add(B, n.op, rest, other, 0, kind, 0);
}

static const char *lane_op_name(int op) {
  switch (op) {
  case LANE_FADD:
  case LANE_IADD:
    return "+";
  case LANE_FSUB:
  case LANE_ISUB:
    return "-";
  case LANE_FMUL:
  case LANE_IMUL:
    return "*";
  case LANE_FDIV:
    return "/";
  case LANE_FMIN:
  case LANE_IMIN:
  case LANE_UMIN:
    return "min";
  case LANE_FMAX:
  case LANE_IMAX:
  case LANE_UMAX:
    return "max";
  case LANE_SELECT:
    return "if";
  case LANE_AND:
    return "&";
  case LANE_OR:
    return "|";
  case LANE_XOR:
    return "^";
  case LANE_SHLI:
  case LANE_SHLV:
    return "<<";
  case LANE_SHRI:
  case LANE_SHRV:
  case LANE_SARI:
  case LANE_SARV:
    return ">>";
  default:
    return "a copy";
  }
}

enum { LANE_CARRY_CHAIN, LANE_CARRY_WIDE_SUM, LANE_CARRY_LAST_MATCH,
       LANE_CARRY_PREVIOUS };

static int lane_depends_on(const LaneBuild *B, int node, int target);

static int lane_chain_op(const LaneBuild *B, int node, int carry) {
  while (node >= 0 && node != carry) {
    const LaneNode *n = &B->nodes[node];
    int sum = n->op == LANE_FADD || n->op == LANE_IADD || n->op == LANE_FSUB ||
              n->op == LANE_ISUB || n->op == LANE_COPY;
    if (!sum) {
      return n->op;
    }
    node = n->op != LANE_COPY && lane_depends_on(B, n->b, carry) ? n->b : n->a;
  }
  return 0;
}

static void lane_refuse_carried(LaneBuild *B, int r, int final, int shape,
                                const char *type) {
  char reason[320];
  char fix[240];
  char name[96];
  lane_source_name(B->carried[r], name, sizeof(name));
  int op = final >= 0 ? B->nodes[final].op : 0;
  if (shape == LANE_CARRY_WIDE_SUM) {
    snprintf(reason, sizeof(reason),
             "`%s` is a running sum in %s, and the lanes add 32-bit values",
             name, type ? type : "a wider type");
    snprintf(fix, sizeof(fix),
             "accumulate in float32 or int32 when the total fits, and widen "
             "once after the loop");
  } else if (shape == LANE_CARRY_LAST_MATCH) {
    snprintf(reason, sizeof(reason),
             "`%s` keeps the value from the last iteration whose condition "
             "held, and the lanes cannot tell which lane came last",
             name);
    snprintf(fix, sizeof(fix),
             "find the index of the last match in a loop of its own, then "
             "read the value at that index");
  } else if (shape == LANE_CARRY_PREVIOUS) {
    snprintf(reason, sizeof(reason),
             "each iteration reads `%s` as the iteration before left it, so "
             "no two iterations can run side by side",
             name);
    snprintf(fix, sizeof(fix),
             "read the earlier element straight from its array (`x[i - 1]`) "
             "so each iteration stands alone");
  } else {
    int carry = -1;
    for (int i = 0; i < B->n; i++) {
      carry = B->nodes[i].op == LANE_CARRY && B->nodes[i].a == r ? i : carry;
    }
    int chain = lane_chain_op(B, final, carry);
    snprintf(reason, sizeof(reason),
             "`%s` carries its value from one iteration into the next "
             "through `%s`, and only a running sum regroups into lanes",
             name, lane_op_name(chain ? chain : op));
    snprintf(fix, sizeof(fix),
             "when this chain is the algorithm, as in a hash or a filter, "
             "the loop is already at its scalar floor");
  }
  lane_why(B, LANE_WHY_CARRIED, reason, fix);
}

static int lane_reduction(LaneBuild *B, int r, int final, int *kind_out) {
  int carry = -1;
  for (int i = 0; i < B->n; i++) {
    if (B->nodes[i].op == LANE_CARRY && B->nodes[i].a == r) {
      carry = i;
    }
  }
  if (carry < 0 || final < 0) {
    lane_refuse_carried(B, r, -1, LANE_CARRY_LAST_MATCH, NULL);
    return -1;
  }
  int op = 0;
  int value = lane_addend(B, final, carry, &op);
  if (value < 0 && B->nodes[final].op == LANE_SELECT) {
    const LaneNode s = B->nodes[final];
    op = 0;
    int on = s.b == carry ? -1 : lane_addend(B, s.b, carry, &op);
    int off = s.c == carry ? -1 : lane_addend(B, s.c, carry, &op);
    int identity = op == LANE_FADD ? lane_const_node(B, 0x80000000u, LK_F32, 0)
                                   : lane_int_const(B, 0);
    if (on >= 0 && s.c == carry) {
      value = lane_select(B, s.a, on, identity);
    } else if (off >= 0 && s.b == carry) {
      value = lane_select(B, s.a, identity, off);
    }
  }
  const char *type = lane_symbol_type(B->fn, B->carried[r]);
  int fsum = op == LANE_FADD && type && strcmp(type, "float32") == 0;
  int isum = op == LANE_IADD && type &&
             (strcmp(type, "int32") == 0 || strcmp(type, "uint32") == 0);
  if (value < 0 || (!fsum && !isum) || lane_depends_on(B, value, carry)) {
    const LaneNode *f = &B->nodes[final];
    int shape = value >= 0 && !lane_depends_on(B, value, carry)
                    ? LANE_CARRY_WIDE_SUM
                : f->op == LANE_SELECT && (f->b == carry || f->c == carry)
                    ? LANE_CARRY_LAST_MATCH
                    : LANE_CARRY_CHAIN;
    lane_refuse_carried(B, r, final, shape, type);
    return -1;
  }
  *kind_out = fsum ? LANE_REDUCE_FSUM : LANE_REDUCE_ISUM;
  return lane_add(B, LANE_REDUCE, r, value, 0, B->nodes[value].kind, 0);
}

typedef struct {
  int reduce_kind[LANES_MAX_REDUCE];
  int n_reduce;
} LanePlan;

static int lane_finish(LaneBuild *B, const LaneEnv *env, LanePlan *plan) {
  for (int r = 0; r < B->n_carried; r++) {
    int kind = 0;
    if (lane_reduction(B, r, lane_env_get(env, B->carried[r]), &kind) < 0) {
      return 0;
    }
    plan->reduce_kind[r] = kind;
  }
  plan->n_reduce = B->n_carried;
  for (size_t w = 0; w < B->written_count; w++) {
    B->keep_last |=
        !lane_names_contain(B->carried, (size_t)B->n_carried, B->written[w]) &&
        ir_symbol_live_after_loop(B->fn, B->exit_index, B->written[w]);
  }
  unsigned char live[LANES_MAX_OPS];
  lane_mark_live(B, live);
  int roots = 0;
  for (int i = 0; i < B->n; i++) {
    if (live[i] && B->nodes[i].op == LANE_CARRY) {
      lane_refuse_carried(B, B->nodes[i].a, i, LANE_CARRY_PREVIOUS, NULL);
      return 0;
    }
    roots += live[i] && lane_is_root(B->nodes[i].op);
  }
  return roots > 0;
}

static int lane_serialize(const LaneBuild *B, const LanePlan *plan,
                          IRInstruction *fused) {
  unsigned char live[LANES_MAX_OPS];
  int remap[LANES_MAX_OPS];
  int const_map[LANES_MAX_CONSTS];
  int scalar_map[LANES_MAX_SCALARS];
  uint32_t consts[LANES_MAX_CONSTS];
  int scalars[LANES_MAX_SCALARS];
  int n_ops = 0;
  int n_consts = 0;
  int n_scalars = 0;
  int uses_iota = 0;
  lane_mark_live(B, live);
  for (int c = 0; c < B->n_consts; c++) {
    const_map[c] = -1;
  }
  for (int s = 0; s < B->n_scalars; s++) {
    scalar_map[s] = -1;
  }
  for (int i = 0; i < B->n; i++) {
    remap[i] = -1;
    if (!live[i]) {
      continue;
    }
    const LaneNode *n = &B->nodes[i];
    if (n->op == LANE_CONST && const_map[n->a] < 0) {
      const_map[n->a] = n_consts;
      consts[n_consts++] = B->consts[n->a];
    }
    if (n->op == LANE_SCALAR && scalar_map[n->a] < 0) {
      scalar_map[n->a] = n_scalars;
      scalars[n_scalars++] = n->a;
    }
    uses_iota |= n->op == LANE_IOTA;
    remap[i] = n_ops++;
  }
  size_t argc = LANES_HEADER_ARGS + (size_t)B->n_bases + (size_t)n_scalars +
                2 * (size_t)plan->n_reduce + (size_t)n_consts +
                4 * (size_t)n_ops;
  fused->arguments = calloc(argc, sizeof(IROperand));
  if (!fused->arguments) {
    return 0;
  }
  fused->argument_count = argc;
  IROperand *a = fused->arguments;
  size_t k = 0;
  a[k++] = ir_operand_int(B->n_bases);
  a[k++] = ir_operand_int(n_scalars);
  a[k++] = ir_operand_int(n_consts);
  a[k++] = ir_operand_int(n_ops);
  a[k++] = ir_operand_int(plan->n_reduce);
  a[k++] = ir_operand_int(B->stored);
  a[k++] = ir_operand_int((uses_iota ? LANES_FLAG_IOTA : 0) |
                          (B->keep_last ? LANES_FLAG_KEEP_LAST : 0));
  for (int i = 0; i < B->n_bases; i++) {
    a[k++] = ir_operand_symbol(B->bases[i]);
  }
  for (int s = 0; s < n_scalars; s++) {
    int from = scalars[s];
    a[k++] = B->scalar_is_temp[from] ? ir_operand_temp(B->scalars[from])
                                     : ir_operand_symbol(B->scalars[from]);
  }
  for (int r = 0; r < plan->n_reduce; r++) {
    char name[96];
    snprintf(name, sizeof(name), "lanes_%zu_p%d", B->tag, r);
    a[k++] = ir_operand_temp(name);
  }
  for (int c = 0; c < n_consts; c++) {
    a[k++] = ir_operand_int((long long)consts[c]);
  }
  for (int r = 0; r < plan->n_reduce; r++) {
    a[k++] = ir_operand_int(plan->reduce_kind[r]);
  }
  for (int i = 0; i < B->n; i++) {
    if (!live[i]) {
      continue;
    }
    LaneNode n = B->nodes[i];
    int fields[3];
    int count = ir_lanes_operand_fields(n.op, fields);
    int *slots[3] = {&n.a, &n.b, &n.c};
    for (int q = 0; q < count; q++) {
      *slots[fields[q]] = remap[*slots[fields[q]]];
    }
    if (n.op == LANE_CONST) {
      n.a = const_map[n.a];
    } else if (n.op == LANE_SCALAR) {
      n.a = scalar_map[n.a];
    } else if (n.op == LANE_LOAD || n.op == LANE_MLOAD) {
      n.c = 0;
    }
    if (n.op == LANE_CONST || n.op == LANE_SCALAR || n.op == LANE_LOAD ||
        n.op == LANE_IOTA) {
      n.b = n.kind == LK_F32;
    }
    a[k++] = ir_operand_int(n.op);
    a[k++] = ir_operand_int(n.a);
    a[k++] = ir_operand_int(n.b);
    a[k++] = ir_operand_int(n.c);
  }
  return k == argc;
}

static int lane_insert(IRFunction *fn, size_t *at, IRInstruction *ins) {
  int ok = ir_function_insert_instruction(fn, *at, ins);
  ir_instruction_destroy_storage(ins);
  if (ok) {
    (*at)++;
  }
  return ok;
}

static int lane_install_partial(LaneBuild *B, size_t *at, int r, int fsum) {
  SourceLocation where = B->fn->instructions[*at].location;
  char name[96];
  IRInstruction decl = {0};
  decl.op = IR_OP_DECLARE_LOCAL;
  decl.location = where;
  snprintf(name, sizeof(name), "__lanes_%zu_s%d", B->tag, r);
  decl.dest = ir_operand_symbol(name);
  decl.text = mettle_strdup(fsum ? "float32" : "int32");
  IRInstruction take = {0};
  take.op = IR_OP_ADDRESS_OF;
  take.location = where;
  take.lhs = ir_operand_symbol(name);
  snprintf(name, sizeof(name), "lanes_%zu_p%d", B->tag, r);
  take.dest = ir_operand_temp(name);
  if (!decl.text) {
    ir_instruction_destroy_storage(&take);
    return 0;
  }
  return lane_insert(B->fn, at, &decl) && lane_insert(B->fn, at, &take);
}

static int lane_install_fold(LaneBuild *B, size_t *at, int r, int fsum) {
  SourceLocation where = B->fn->instructions[*at].location;
  const char *acc = B->carried[r];
  char addr[96];
  char value[96];
  char wide[96];
  snprintf(addr, sizeof(addr), "lanes_%zu_p%d", B->tag, r);
  snprintf(value, sizeof(value), "lanes_%zu_v%d", B->tag, r);
  snprintf(wide, sizeof(wide), "lanes_%zu_w%d", B->tag, r);
  IRInstruction load = {0};
  load.op = IR_OP_LOAD;
  load.location = where;
  load.lhs = ir_operand_temp(addr);
  load.dest = ir_operand_temp(value);
  load.rhs = ir_operand_int(4);
  load.is_float = fsum;
  load.float_bits = fsum ? 32 : 0;
  load.dest.float_bits = load.float_bits;
  IRInstruction add = {0};
  add.op = IR_OP_BINARY;
  add.location = where;
  add.text = mettle_strdup("+");
  add.dest = fsum ? ir_operand_symbol(acc) : ir_operand_temp(wide);
  add.lhs = ir_operand_symbol(acc);
  add.rhs = ir_operand_temp(value);
  add.is_float = fsum;
  add.float_bits = load.float_bits;
  add.dest.float_bits = load.float_bits;
  if (!add.text || !lane_insert(B->fn, at, &load)) {
    ir_instruction_destroy_storage(&add);
    return 0;
  }
  if (!lane_insert(B->fn, at, &add)) {
    return 0;
  }
  if (fsum) {
    return 1;
  }
  IRInstruction narrow = {0};
  narrow.op = IR_OP_CAST;
  narrow.location = where;
  narrow.dest = ir_operand_symbol(acc);
  narrow.lhs = ir_operand_temp(wide);
  narrow.text = mettle_strdup(lane_symbol_type(B->fn, acc));
  if (!narrow.text) {
    ir_instruction_destroy_storage(&narrow);
    return 0;
  }
  return lane_insert(B->fn, at, &narrow);
}

static int lane_install(LaneBuild *B, const LanePlan *plan,
                        const IROperand *bound, size_t *inserted) {
  size_t at = B->header;
  for (int r = 0; r < plan->n_reduce; r++) {
    if (!lane_install_partial(B, &at, r,
                              plan->reduce_kind[r] == LANE_REDUCE_FSUM)) {
      return 0;
    }
  }
  IRInstruction fused = {0};
  fused.op = IR_OP_SIMD_LANES;
  fused.location = B->fn->instructions[at].location;
  fused.dest = ir_operand_symbol(B->iv);
  fused.float_bits = 32;
  if (!ir_operand_clone(bound, &fused.lhs) ||
      !lane_serialize(B, plan, &fused)) {
    ir_instruction_destroy_storage(&fused);
    return 0;
  }
  if (!lane_insert(B->fn, &at, &fused)) {
    return 0;
  }
  for (int r = 0; r < plan->n_reduce; r++) {
    if (!lane_install_fold(B, &at, r,
                           plan->reduce_kind[r] == LANE_REDUCE_FSUM)) {
      return 0;
    }
  }
  *inserted = at - B->header;
  return 1;
}

static int lane_is_loop_test(const IRFunction *fn, size_t cmp_at,
                             size_t *br_out) {
  const IRInstruction *cmp = &fn->instructions[cmp_at];
  if (cmp->op != IR_OP_BINARY || cmp->is_float || cmp->is_unsigned ||
      !cmp->text || strcmp(cmp->text, "<") != 0 ||
      cmp->dest.kind != IR_OPERAND_TEMP || !ir_operand_is_symbol(&cmp->lhs) ||
      (cmp->rhs.kind != IR_OPERAND_INT && cmp->rhs.kind != IR_OPERAND_SYMBOL &&
       cmp->rhs.kind != IR_OPERAND_TEMP) ||
      !ir_find_next_non_nop(fn, cmp_at + 1, br_out)) {
    return 0;
  }
  const IRInstruction *br = &fn->instructions[*br_out];
  return br->op == IR_OP_BRANCH_ZERO && br->text &&
         ir_operand_is_temp_named(&br->lhs, cmp->dest.name);
}

static int lane_prefix_op(const IRInstruction *ins) {
  switch (ins->op) {
  case IR_OP_BINARY:
  case IR_OP_UNARY:
  case IR_OP_CAST:
  case IR_OP_ASSIGN:
    return ins->dest.kind == IR_OPERAND_TEMP && ins->dest.name != NULL;
  default:
    return 0;
  }
}

static int lane_match_compare(const IRFunction *fn, size_t header,
                              size_t *cmp_out, size_t *br_out) {
  const IRInstruction *head = &fn->instructions[header];
  if (head->op != IR_OP_LABEL || !ir_label_is_while_header(head->text)) {
    return 0;
  }
  for (size_t k = header + 1; k < fn->instruction_count; k++) {
    const IRInstruction *ins = &fn->instructions[k];
    if (ins->op == IR_OP_NOP) {
      continue;
    }
    if (lane_is_loop_test(fn, k, br_out)) {
      *cmp_out = k;
      return 1;
    }
    if (!lane_prefix_op(ins)) {
      return 0;
    }
  }
  return 0;
}

static int lane_written_in(const IRFunction *fn, size_t lo, size_t hi,
                           size_t skip, const char *name) {
  for (size_t i = lo; i <= hi; i++) {
    const IRInstruction *ins = &fn->instructions[i];
    if (i != skip && ir_instruction_writes_destination(ins) &&
        ins->dest.name && strcmp(ins->dest.name, name) == 0) {
      return 1;
    }
  }
  return 0;
}

static int lane_prefix_operand(const LaneBuild *B, size_t at, size_t end,
                               const IROperand *op) {
  switch (op->kind) {
  case IR_OPERAND_NONE:
  case IR_OPERAND_INT:
  case IR_OPERAND_FLOAT:
    return 1;
  case IR_OPERAND_SYMBOL:
    return op->name && !ir_symbol_address_taken(B->fn, op->name) &&
           !lane_written_in(B->fn, B->header, end, (size_t)-1, op->name);
  case IR_OPERAND_TEMP:
    return op->name && lane_temp_def(B->fn, at, op->name) &&
           !lane_written_in(B->fn, at, end, (size_t)-1, op->name);
  default:
    return 0;
  }
}

static int lane_prefix_is_invariant(const LaneBuild *B, size_t end) {
  for (size_t k = B->header + 1; k < B->cond_at; k++) {
    const IRInstruction *ins = &B->fn->instructions[k];
    if (ins->op != IR_OP_NOP &&
        (lane_written_in(B->fn, B->header, end, k, ins->dest.name) ||
         !lane_prefix_operand(B, k, end, &ins->lhs) ||
         !lane_prefix_operand(B, k, end, &ins->rhs))) {
      return 0;
    }
  }
  return 1;
}

static void lane_hoist_prefix(LaneBuild *B) {
  IRInstruction *ins = B->fn->instructions;
  IRInstruction label = ins[B->header];
  memmove(&ins[B->header], &ins[B->header + 1],
          (B->cond_at - B->header - 1) * sizeof(IRInstruction));
  ins[B->cond_at - 1] = label;
  B->header = B->cond_at - 1;
}

static size_t lane_pick_tag(const LaneBuild *B) {
  size_t tag = B->header;
  char name[96];
  for (;;) {
    snprintf(name, sizeof(name), "__lanes_%zu_s0", tag);
    if (!ir_function_local_declared_type(B->fn, name)) {
      return tag;
    }
    tag++;
  }
}

static int lane_bound_is_invariant(const LaneBuild *B, const IROperand *rhs) {
  if (rhs->kind == IR_OPERAND_INT) {
    return 1;
  }
  if (!rhs->name) {
    return 0;
  }
  for (size_t i = B->lo; i < B->hi; i++) {
    const IRInstruction *ins = &B->fn->instructions[i];
    if (ir_instruction_writes_destination(ins) && ins->dest.name &&
        strcmp(ins->dest.name, rhs->name) == 0) {
      return 0;
    }
  }
  return rhs->kind != IR_OPERAND_TEMP ||
         lane_temp_def(B->fn, B->cond_at, rhs->name) != NULL;
}

static int lane_is_unit_step(const IRInstruction *w, const char *iv) {
  return w->op == IR_OP_BINARY && !w->is_float && w->text &&
         strcmp(w->text, "+") == 0 &&
         ((ir_operand_is_symbol_named(&w->lhs, iv) &&
           ir_operand_is_int_value(&w->rhs, 1)) ||
          (ir_operand_is_symbol_named(&w->rhs, iv) &&
           ir_operand_is_int_value(&w->lhs, 1)));
}

static int lane_is_increment(const IRFunction *fn, size_t lo, size_t at,
                             const char *iv) {
  const IRInstruction *ins = &fn->instructions[at];
  if (ir_try_parse_direct_unit_increment(ins, iv)) {
    return 1;
  }
  if (ins->op != IR_OP_ASSIGN || !ir_operand_is_symbol_named(&ins->dest, iv) ||
      (ins->lhs.kind != IR_OPERAND_SYMBOL && ins->lhs.kind != IR_OPERAND_TEMP) ||
      !ins->lhs.name) {
    return 0;
  }
  int found = 0;
  for (size_t k = lo; k < at; k++) {
    const IRInstruction *w = &fn->instructions[k];
    if (!ir_instruction_writes_destination(w) || !w->dest.name ||
        strcmp(w->dest.name, ins->lhs.name) != 0) {
      continue;
    }
    if (found || !lane_is_unit_step(w, iv)) {
      return 0;
    }
    found = 1;
  }
  return found;
}

static int lane_frame(LaneBuild *B, size_t header, IROperand *bound) {
  IRFunction *fn = B->fn;
  size_t cmp_at = 0;
  size_t br_at = 0;
  if (!lane_match_compare(fn, header, &cmp_at, &br_at)) {
    return 0;
  }
  const IRInstruction *head = &fn->instructions[header];
  const IRInstruction *br = &fn->instructions[br_at];
  const IRInstruction *cmp = &fn->instructions[cmp_at];
  size_t jump_at = (size_t)-1;
  for (size_t i = br_at + 1; i < fn->instruction_count; i++) {
    const IRInstruction *ins = &fn->instructions[i];
    if (ins->op == IR_OP_JUMP && ins->text &&
        strcmp(ins->text, head->text) == 0) {
      jump_at = i;
      break;
    }
  }
  if (jump_at == (size_t)-1 ||
      !ir_fused_loop_exit_is_adjacent(fn, jump_at, br->text)) {
    return 0;
  }
  size_t inc_at = jump_at;
  while (inc_at > br_at + 1 && fn->instructions[inc_at - 1].op == IR_OP_NOP) {
    inc_at--;
  }
  if (inc_at <= br_at + 1 ||
      !lane_is_increment(fn, br_at + 1, inc_at - 1, cmp->lhs.name)) {
    return 0;
  }
  B->iv = cmp->lhs.name;
  B->header = header;
  B->cond_at = cmp_at;
  B->lo = br_at + 1;
  B->hi = inc_at - 1;
  B->path = -1;
  B->exit_index = jump_at + 1;
  const char *iv_type = lane_symbol_type(fn, B->iv);
  if (!iv_type ||
      (strcmp(iv_type, "int64") != 0 && strcmp(iv_type, "int32") != 0) ||
      ir_symbol_address_taken(fn, B->iv) ||
      !lane_prefix_is_invariant(B, jump_at) ||
      !lane_bound_is_invariant(B, &cmp->rhs)) {
    return 0;
  }
  for (size_t i = header; i-- > 0;) {
    const IRInstruction *ins = &fn->instructions[i];
    if (ins->op == IR_OP_LABEL) {
      break;
    }
    if (ins->op == IR_OP_SIMD_LANES || ins->op == IR_OP_SIMD_FIND) {
      return 0;
    }
  }
  return ir_operand_clone(&cmp->rhs, bound);
}

static int lane_within_budget(LaneBuild *B, const LanePlan *plan,
                              const IROperand *bound) {
  int operands = B->n_bases + B->n_scalars + plan->n_reduce +
                 (bound->kind == IR_OPERAND_INT ? 1 : 2);
  if (operands <= 16) {
    return 1;
  }
  char reason[320];
  snprintf(reason, sizeof(reason),
           "the loop touches %d arrays and %d loop-invariant values, and the "
           "lane kernel takes at most 16 operands",
           B->n_bases, B->n_scalars);
  lane_why(B, LANE_WHY_BUDGET, reason,
           "split the body into two loops over the same range, each with "
           "fewer arrays");
  return 0;
}

static int lane_plan_loop(LaneBuild *B, LaneEnv *env, LanePlan *plan,
                          size_t header, IROperand *bound) {
  return lane_frame(B, header, bound) && lane_collect_names(B) &&
         lane_eval_range(B, env, B->lo, B->hi, NULL) &&
         lane_finish(B, env, plan) && lane_within_budget(B, plan, bound);
}

static int lane_computes(const LaneBuild *B) {
  unsigned char live[LANES_MAX_OPS];
  lane_mark_live(B, live);
  for (int i = 0; i < B->n; i++) {
    int op = B->nodes[i].op;
    if (live[i] && op != LANE_LOAD && op != LANE_STORE && op != LANE_CONST &&
        op != LANE_SCALAR && op != LANE_MLOAD && op != LANE_MSTORE &&
        op != LANE_COPY) {
      return 1;
    }
  }
  return 0;
}

static int lane_try_at(IRFunction *fn, size_t header, int install,
                       size_t *inserted) {
  LaneBuild *B = calloc(1, sizeof(LaneBuild));
  LaneEnv env = {0};
  LanePlan plan = {{0}, 0};
  IROperand bound = ir_operand_none();
  int ok = 1;
  *inserted = 0;
  if (!B) {
    return 0;
  }
  B->fn = fn;
  int planned = lane_plan_loop(B, &env, &plan, header, &bound);
  if ((install && planned) || (!planned && B->why != LANE_WHY_NONE)) {
    lane_record_verdict(B, header);
  }
  if (planned && install) {
    lane_hoist_prefix(B);
    B->tag = lane_pick_tag(B);
    ok = lane_install(B, &plan, &bound, inserted);
    *inserted += B->header - header;
  }
  int owned = planned && lane_computes(B);
  ir_operand_destroy(&bound);
  free(env.items);
  free(B->written);
  free(B->address_temps);
  free(B);
  return install ? ok : owned;
}

int ir_simd_lanes_accepts(IRFunction *function, size_t header) {
  size_t unused = 0;
  return function && header < function->instruction_count &&
         function->instructions[header].op == IR_OP_LABEL &&
         ir_label_is_while_header(function->instructions[header].text) &&
         lane_try_at(function, header, 0, &unused);
}

int ir_simd_lanes_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op != IR_OP_LABEL || !ir_label_is_while_header(ins->text)) {
      continue;
    }
    size_t inserted = 0;
    if (!lane_try_at(function, i, 1, &inserted)) {
      return 0;
    }
    if (inserted) {
      i += inserted;
      if (changed) {
        *changed = 1;
      }
    }
  }
  return 1;
}
