#include "codegen/binary/mir.h"
#include "codegen/binary/mir_machine.h"
#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MIR_VERIFY_MAX_WORK 4000000u
#define MIR_VERIFY_MAX_SUCC 8

extern const char *g_mir_ra_trace_name;
const char *mir_external_allocator_name(void);

typedef struct {
  const MirFunction *fn;
  size_t n;
  size_t words;
  unsigned long long *live_in;
  unsigned long long *defd_in;
  unsigned long long *scratch;
  int *pred_head;
  int *pred_next;
  int *pred_from;
  size_t pred_count;
  size_t pred_capacity;
  unsigned char *has_def;
  unsigned char *mentioned;
  const char **label_sym;
  int *label_idx;
  size_t label_count;
  int *table_succ;
  size_t table_succ_count;
  size_t table_succ_capacity;
} MirVerify;

int mir_regalloc_verify_enabled(void) {
  static int cached = -1;
  if (cached < 0) {
    cached = getenv("METTLE_REGALLOC_VERIFY") ? 1 : 0;
  }
  return cached;
}

int mir_regalloc_verify_sabotage_enabled(void) {
  static int cached = -1;
  if (cached < 0) {
    cached = getenv("METTLE_REGALLOC_VERIFY_BREAK") ? 1 : 0;
  }
  return cached;
}

static const char *mir_verify_fn_name(const MirFunction *fn) {
  if (fn->context && fn->context->function_name) {
    return fn->context->function_name;
  }
  return g_mir_ra_trace_name ? g_mir_ra_trace_name : "?";
}

static int mir_verify_fail(MirFunction *fn, const char *what, size_t at,
                           MirVregId a, MirVregId b) {
  const char *name = mir_verify_fn_name(fn);
  fprintf(stderr, "RA-VERIFY\t%s\tFAIL\t%s\tinsn=%zu\tv%d\tv%d\n", name, what,
          at, (int)a, (int)b);
  if (fn->generator && !fn->generator->has_error) {
    const char *allocator = mir_external_allocator_name();
    if (allocator) {
      code_generator_set_error(fn->generator,
                               "'@%s' wrote a wrong assignment: %s at "
                               "instruction %zu (v%d, v%d) in function '%s'",
                               allocator, what, at, (int)a, (int)b, name);
      fn->generator->has_user_error = 1;
    } else {
      code_generator_set_error(fn->generator,
                               "register allocation verifier: %s at instruction "
                               "%zu (v%d, v%d) in function '%s'",
                               what, at, (int)a, (int)b, name);
    }
  }
  fn->has_error = 1;
  return 0;
}

static void mir_verify_bit_set(unsigned long long *set, size_t v) {
  set[v >> 6] |= 1ull << (v & 63);
}

static void mir_verify_bit_clear(unsigned long long *set, size_t v) {
  set[v >> 6] &= ~(1ull << (v & 63));
}

static int mir_verify_bit_get(const unsigned long long *set, size_t v) {
  return (set[v >> 6] >> (v & 63)) & 1ull;
}

static int mir_verify_dst_is_read(const MirInst *in) {
  return in->op == MIR_CMOV || in->op == MIR_CMOVCC;
}

static int mir_verify_operand_vregs(const MirOperand *op, MirVregId *out) {
  int n = 0;
  if (op->kind == MIR_OPK_VREG) {
    out[n++] = op->vreg;
  } else if (op->kind == MIR_OPK_MEM) {
    out[n++] = op->mem.base;
    out[n++] = op->mem.index;
  }
  return n;
}

static int mir_verify_uses(const MirInst *in, MirVregId *out) {
  int n = 0;
  MirVregId tmp[2];
  int k;
  if (in->dst.kind == MIR_OPK_MEM || mir_verify_dst_is_read(in)) {
    k = mir_verify_operand_vregs(&in->dst, tmp);
    for (int j = 0; j < k; j++) {
      out[n++] = tmp[j];
    }
  }
  k = mir_verify_operand_vregs(&in->a, tmp);
  for (int j = 0; j < k; j++) {
    out[n++] = tmp[j];
  }
  k = mir_verify_operand_vregs(&in->b, tmp);
  for (int j = 0; j < k; j++) {
    out[n++] = tmp[j];
  }
  return n;
}

static MirVregId mir_verify_def(const MirInst *in) {
  if (in->dst.kind == MIR_OPK_VREG) {
    return in->dst.vreg;
  }
  return MIR_VREG_NONE;
}

static int mir_verify_valid(const MirVerify *st, MirVregId v) {
  return v >= 0 && (size_t)v < st->n;
}

static int mir_verify_find_label(const MirVerify *st, const char *sym) {
  if (!sym) {
    return -1;
  }
  for (size_t k = 0; k < st->label_count; k++) {
    if (strcmp(st->label_sym[k], sym) == 0) {
      return st->label_idx[k];
    }
  }
  return -1;
}

static int mir_verify_push_table_succ(MirVerify *st, int target) {
  if (st->table_succ_count == st->table_succ_capacity) {
    size_t cap = st->table_succ_capacity ? st->table_succ_capacity * 2 : 64;
    int *grown = (int *)realloc(st->table_succ, cap * sizeof(int));
    if (!grown) {
      return 0;
    }
    st->table_succ = grown;
    st->table_succ_capacity = cap;
  }
  st->table_succ[st->table_succ_count++] = target;
  return 1;
}

static int mir_verify_successors(MirVerify *st, size_t i, int *out,
                                 int *count, const int **table, int *table_n) {
  const MirInst *in = &st->fn->insns[i];
  int n = 0;
  int fall = 1;
  *table = NULL;
  *table_n = 0;
  if (in->op == MIR_JMP) {
    fall = 0;
    out[n++] = mir_verify_find_label(st, in->dst.sym);
  } else if (mir_op_has(in->op, MIR_OPF_CONDITIONAL_BRANCH)) {
    out[n++] = mir_verify_find_label(st, in->dst.sym);
  } else if (in->op == MIR_JMP_TABLE) {
    const MirJumpTable *jt = (const MirJumpTable *)in->aux;
    fall = 0;
    if (!jt) {
      return 0;
    }
    st->table_succ_count = 0;
    for (size_t t = 0; t < jt->count; t++) {
      int target = mir_verify_find_label(st, jt->labels[t]);
      if (target < 0 || !mir_verify_push_table_succ(st, target)) {
        return 0;
      }
    }
    *table = st->table_succ;
    *table_n = (int)st->table_succ_count;
  } else if (in->op == MIR_RET || in->op == MIR_TRAP) {
    fall = 0;
  }
  for (int k = 0; k < n; k++) {
    if (out[k] < 0) {
      return 0;
    }
  }
  if (fall && i + 1 < st->fn->insn_count) {
    out[n++] = (int)(i + 1);
  }
  *count = n;
  return 1;
}

static int mir_verify_live_out(MirVerify *st, size_t i,
                               unsigned long long *out) {
  int succ[MIR_VERIFY_MAX_SUCC];
  int succ_count = 0;
  const int *table = NULL;
  int table_n = 0;
  memset(out, 0, st->words * sizeof(*out));
  if (!mir_verify_successors(st, i, succ, &succ_count, &table, &table_n)) {
    return 0;
  }
  for (int k = 0; k < succ_count; k++) {
    const unsigned long long *s = st->live_in + (size_t)succ[k] * st->words;
    for (size_t w = 0; w < st->words; w++) {
      out[w] |= s[w];
    }
  }
  for (int k = 0; k < table_n; k++) {
    const unsigned long long *s = st->live_in + (size_t)table[k] * st->words;
    for (size_t w = 0; w < st->words; w++) {
      out[w] |= s[w];
    }
  }
  return 1;
}

static int mir_verify_add_pred(MirVerify *st, int to, int from) {
  if (st->pred_count == st->pred_capacity) {
    size_t cap = st->pred_capacity ? st->pred_capacity * 2 : 256;
    int *n1 = (int *)realloc(st->pred_next, cap * sizeof(int));
    int *n2;
    if (!n1) {
      return 0;
    }
    st->pred_next = n1;
    n2 = (int *)realloc(st->pred_from, cap * sizeof(int));
    if (!n2) {
      return 0;
    }
    st->pred_from = n2;
    st->pred_capacity = cap;
  }
  st->pred_from[st->pred_count] = from;
  st->pred_next[st->pred_count] = st->pred_head[to];
  st->pred_head[to] = (int)st->pred_count;
  st->pred_count++;
  return 1;
}

static int mir_verify_build_preds(MirVerify *st) {
  const MirFunction *fn = st->fn;
  st->pred_head = (int *)malloc(fn->insn_count * sizeof(int));
  if (!st->pred_head) {
    return 0;
  }
  for (size_t i = 0; i < fn->insn_count; i++) {
    st->pred_head[i] = -1;
  }
  for (size_t i = 0; i < fn->insn_count; i++) {
    int succ[MIR_VERIFY_MAX_SUCC];
    int succ_count = 0;
    const int *table = NULL;
    int table_n = 0;
    if (fn->insns[i].op == MIR_NOP) {
      if (i + 1 < fn->insn_count && !mir_verify_add_pred(st, (int)(i + 1), (int)i)) {
        return 0;
      }
      continue;
    }
    if (!mir_verify_successors(st, i, succ, &succ_count, &table, &table_n)) {
      return 0;
    }
    for (int k = 0; k < succ_count; k++) {
      if (!mir_verify_add_pred(st, succ[k], (int)i)) {
        return 0;
      }
    }
    for (int k = 0; k < table_n; k++) {
      if (!mir_verify_add_pred(st, table[k], (int)i)) {
        return 0;
      }
    }
  }
  return 1;
}

static void mir_verify_defd_out(const MirVerify *st, size_t i,
                                unsigned long long *out) {
  const MirInst *in = &st->fn->insns[i];
  MirVregId def;
  memcpy(out, st->defd_in + i * st->words, st->words * sizeof(*out));
  if (in->op == MIR_NOP) {
    return;
  }
  def = mir_verify_def(in);
  if (mir_verify_valid(st, def)) {
    mir_verify_bit_set(out, (size_t)def);
  }
}

static int mir_verify_compute_defined(MirVerify *st) {
  const MirFunction *fn = st->fn;
  int changed = 1;
  int rounds = 0;
  unsigned long long *entry = st->defd_in;
  for (size_t p = 0; p < fn->param_count; p++) {
    if (mir_verify_valid(st, fn->params[p].vreg)) {
      mir_verify_bit_set(entry, (size_t)fn->params[p].vreg);
    }
    if (mir_verify_valid(st, fn->params[p].sysv_storage)) {
      mir_verify_bit_set(entry, (size_t)fn->params[p].sysv_storage);
    }
  }
  if (fn->returns_indirect && mir_verify_valid(st, fn->indirect_return_vreg)) {
    mir_verify_bit_set(entry, (size_t)fn->indirect_return_vreg);
  }
  while (changed) {
    changed = 0;
    rounds++;
    if (rounds > 10000) {
      return 0;
    }
    for (size_t i = 1; i < fn->insn_count; i++) {
      unsigned long long *cur = st->defd_in + i * st->words;
      for (int e = st->pred_head[i]; e >= 0; e = st->pred_next[e]) {
        mir_verify_defd_out(st, (size_t)st->pred_from[e], st->scratch);
        for (size_t w = 0; w < st->words; w++) {
          unsigned long long next = cur[w] | st->scratch[w];
          if (next != cur[w]) {
            cur[w] = next;
            changed = 1;
          }
        }
      }
    }
  }
  return 1;
}

static int mir_verify_compute_liveness(MirVerify *st) {
  const MirFunction *fn = st->fn;
  int changed = 1;
  int rounds = 0;
  while (changed) {
    changed = 0;
    rounds++;
    if (rounds > 10000) {
      return 0;
    }
    for (size_t bi = fn->insn_count; bi > 0; bi--) {
      size_t i = bi - 1;
      const MirInst *in = &fn->insns[i];
      unsigned long long *cur = st->live_in + i * st->words;
      MirVregId uses[6];
      int use_count;
      MirVregId def;
      if (in->op == MIR_NOP) {
        if (i + 1 < fn->insn_count) {
          const unsigned long long *next = st->live_in + (i + 1) * st->words;
          for (size_t w = 0; w < st->words; w++) {
            if (cur[w] != next[w]) {
              cur[w] = next[w];
              changed = 1;
            }
          }
        }
        continue;
      }
      if (!mir_verify_live_out(st, i, st->scratch)) {
        return 0;
      }
      def = mir_verify_def(in);
      if (mir_verify_valid(st, def)) {
        mir_verify_bit_clear(st->scratch, (size_t)def);
      }
      use_count = mir_verify_uses(in, uses);
      for (int k = 0; k < use_count; k++) {
        if (mir_verify_valid(st, uses[k])) {
          mir_verify_bit_set(st->scratch, (size_t)uses[k]);
        }
      }
      for (size_t w = 0; w < st->words; w++) {
        if (cur[w] != st->scratch[w]) {
          cur[w] = st->scratch[w];
          changed = 1;
        }
      }
    }
  }
  return 1;
}

static int mir_verify_gp_survives_barrier(const MirInst *in, int phys) {
  const BinaryAbi *abi = code_generator_binary_active_abi();
  int sysv = abi && abi->counts_classes_separately;
  switch (phys) {
  case BINARY_GP_RBX:
  case BINARY_GP_RBP:
  case BINARY_GP_R12:
  case BINARY_GP_R13:
  case BINARY_GP_R14:
  case BINARY_GP_R15:
    return 1;
  case BINARY_GP_RSI:
  case BINARY_GP_RDI:
    return !sysv;
  case BINARY_GP_RAX:
    return in->op == MIR_CALL && in->preserves_rax;
  default:
    return 0;
  }
}

static int mir_verify_xmm_survives_barrier(const MirInst *in, int phys) {
  if (in->op != MIR_CALL || !in->preserves_xmm) {
    return 0;
  }
  for (size_t k = 0; k < MIR_XMM_POOL_COUNT; k++) {
    if ((int)MIR_XMM_POOL[k] == phys) {
      return 1;
    }
  }
  return 0;
}

static unsigned mir_verify_gp_clobbers(const MirInst *in) {
  unsigned m = mir_inst_fixed_gp(in);
  if (in->dst.kind == MIR_OPK_PHYS && in->dst.rclass == MIR_RC_GP &&
      in->dst.phys >= 0 && in->dst.phys < 16) {
    m |= 1u << (unsigned)in->dst.phys;
  }
  if (mir_op_has(in->op, MIR_OPF_CLOBBERS_LISTED_BY_KERNEL)) {
    const MirKernelAux *ka = (const MirKernelAux *)in->aux;
    const MirIrKernel *kern = ka ? mir_ir_kernel_at(ka->kernel_index) : NULL;
    if (kern) {
      m |= kern->gp_clobbers;
    }
  }
  if (mir_op_has(in->op, MIR_OPF_CLOBBERS_EVERY_GP)) {
    m |= 0xFFFFu;
  }
  return m;
}

static int mir_verify_check_insn(MirVerify *st, size_t i) {
  MirFunction *fn = (MirFunction *)st->fn;
  const MirInst *in = &fn->insns[i];
  unsigned long long *out = st->scratch;
  const unsigned long long *defd = st->defd_in + i * st->words;
  MirVregId def = mir_verify_def(in);
  const MirVreg *dv = mir_verify_valid(st, def) ? &fn->vregs[def] : NULL;
  unsigned gp_clobbers = mir_verify_gp_clobbers(in);
  int xmm_clobber = (in->dst.kind == MIR_OPK_PHYS &&
                     in->dst.rclass == MIR_RC_XMM)
                        ? in->dst.phys
                        : -1;
  int barrier = mir_op_has(in->op, MIR_OPF_CALL_BARRIER);

  if (in->op == MIR_NOP) {
    return 1;
  }
  if (!mir_verify_live_out(st, i, out)) {
    return mir_verify_fail(fn, "branch target unresolved", i, MIR_VREG_NONE,
                           MIR_VREG_NONE);
  }
  for (size_t w = 0; w < st->words; w++) {
    unsigned long long bits = out[w] & defd[w];
    while (bits) {
      size_t v = w * 64 + (size_t)__builtin_ctzll(bits);
      const MirVreg *vr;
      bits &= bits - 1;
      if (v >= st->n || !st->has_def[v]) {
        continue;
      }
      vr = &fn->vregs[v];
      if (!vr->in_register) {
        continue;
      }
      if ((MirVregId)v == def) {
        continue;
      }
      if (dv && dv->in_register && st->has_def[def] &&
          dv->rclass == vr->rclass && dv->phys == vr->phys) {
        return mir_verify_fail(fn, "two live values share a register", i, def,
                               (MirVregId)v);
      }
      if (vr->rclass == MIR_RC_GP && vr->phys >= 0 && vr->phys < 16 &&
          (gp_clobbers & (1u << (unsigned)vr->phys))) {
        return mir_verify_fail(fn, "live value in a clobbered register", i,
                               (MirVregId)v, def);
      }
      if (vr->rclass == MIR_RC_XMM && vr->phys == xmm_clobber) {
        return mir_verify_fail(fn, "live value in a clobbered xmm register", i,
                               (MirVregId)v, def);
      }
      if (barrier) {
        int ok = vr->rclass == MIR_RC_GP
                     ? mir_verify_gp_survives_barrier(in, vr->phys)
                     : mir_verify_xmm_survives_barrier(in, vr->phys);
        if (!ok) {
          return mir_verify_fail(fn, "live value in a register the call "
                                     "does not preserve",
                                 i, (MirVregId)v, def);
        }
      }
    }
  }
  return 1;
}

static int mir_verify_check_entry(MirVerify *st) {
  MirFunction *fn = (MirFunction *)st->fn;
  const unsigned long long *entry = st->live_in;
  const unsigned long long *defd = st->defd_in;
  for (size_t a = 0; a < st->n; a++) {
    const MirVreg *av;
    if (!st->has_def[a] || !mir_verify_bit_get(entry, a) ||
        !mir_verify_bit_get(defd, a)) {
      continue;
    }
    av = &fn->vregs[a];
    if (!av->in_register) {
      continue;
    }
    for (size_t b = a + 1; b < st->n; b++) {
      const MirVreg *bv;
      if (!st->has_def[b] || !mir_verify_bit_get(entry, b) ||
          !mir_verify_bit_get(defd, b)) {
        continue;
      }
      bv = &fn->vregs[b];
      if (bv->in_register && bv->rclass == av->rclass && bv->phys == av->phys) {
        return mir_verify_fail(fn, "two entry-live values share a register", 0,
                               (MirVregId)a, (MirVregId)b);
      }
    }
  }
  return 1;
}

static int mir_verify_check_assignments(MirVerify *st) {
  MirFunction *fn = (MirFunction *)st->fn;
  for (size_t v = 0; v < st->n; v++) {
    const MirVreg *vr = &fn->vregs[v];
    if (!st->mentioned[v]) {
      continue;
    }
    if (!vr->assigned) {
      return mir_verify_fail(fn, "a mentioned value was never assigned", 0,
                             (MirVregId)v, MIR_VREG_NONE);
    }
    if (vr->in_register) {
      if (vr->phys < 0 || vr->phys >= 16) {
        return mir_verify_fail(fn, "register number out of range", 0,
                               (MirVregId)v, MIR_VREG_NONE);
      }
    } else if (vr->spill_offset <= 0) {
      return mir_verify_fail(fn, "a spilled value has no frame slot", 0,
                             (MirVregId)v, MIR_VREG_NONE);
    }
  }
  return 1;
}

static void mir_verify_free(MirVerify *st) {
  free(st->live_in);
  free(st->defd_in);
  free(st->scratch);
  free(st->pred_head);
  free(st->pred_next);
  free(st->pred_from);
  free(st->has_def);
  free(st->mentioned);
  free(st->label_sym);
  free(st->label_idx);
  free(st->table_succ);
}

static int mir_verify_init(MirVerify *st, MirFunction *fn) {
  size_t labels = 0;
  memset(st, 0, sizeof(*st));
  st->fn = fn;
  st->n = fn->vreg_count;
  st->words = (st->n + 63) / 64;
  if ((unsigned long long)fn->insn_count * (unsigned long long)st->words >
      MIR_VERIFY_MAX_WORK) {
    return -1;
  }
  st->live_in = (unsigned long long *)calloc(fn->insn_count * st->words,
                                             sizeof(unsigned long long));
  st->defd_in = (unsigned long long *)calloc(fn->insn_count * st->words,
                                             sizeof(unsigned long long));
  st->scratch =
      (unsigned long long *)calloc(st->words, sizeof(unsigned long long));
  st->has_def = (unsigned char *)calloc(st->n, 1);
  st->mentioned = (unsigned char *)calloc(st->n, 1);
  for (size_t i = 0; i < fn->insn_count; i++) {
    const MirInst *in = &fn->insns[i];
    if (in->op == MIR_LABEL && in->dst.kind == MIR_OPK_LABEL && in->dst.sym) {
      labels++;
    }
  }
  st->label_sym = (const char **)calloc(labels + 1, sizeof(const char *));
  st->label_idx = (int *)calloc(labels + 1, sizeof(int));
  if (!st->live_in || !st->defd_in || !st->scratch || !st->has_def ||
      !st->mentioned || !st->label_sym || !st->label_idx) {
    mir_verify_free(st);
    return 0;
  }
  for (size_t i = 0; i < fn->insn_count; i++) {
    const MirInst *in = &fn->insns[i];
    MirVregId ids[6];
    int k;
    MirVregId def;
    if (in->op == MIR_LABEL && in->dst.kind == MIR_OPK_LABEL && in->dst.sym) {
      st->label_sym[st->label_count] = in->dst.sym;
      st->label_idx[st->label_count] = (int)i;
      st->label_count++;
      continue;
    }
    if (in->op == MIR_NOP) {
      continue;
    }
    def = mir_verify_def(in);
    if (mir_verify_valid(st, def)) {
      st->has_def[def] = 1;
      st->mentioned[def] = 1;
    }
    k = mir_verify_uses(in, ids);
    for (int j = 0; j < k; j++) {
      if (mir_verify_valid(st, ids[j])) {
        st->mentioned[ids[j]] = 1;
      }
    }
  }
  for (size_t p = 0; p < fn->param_count; p++) {
    if (mir_verify_valid(st, fn->params[p].vreg)) {
      st->has_def[fn->params[p].vreg] = 1;
    }
    if (mir_verify_valid(st, fn->params[p].sysv_storage)) {
      st->has_def[fn->params[p].sysv_storage] = 1;
    }
  }
  if (fn->returns_indirect && mir_verify_valid(st, fn->indirect_return_vreg)) {
    st->has_def[fn->indirect_return_vreg] = 1;
  }
  if (!mir_verify_build_preds(st)) {
    mir_verify_free(st);
    return 0;
  }
  return 1;
}

static int mir_verify_analyse(MirVerify *st) {
  return mir_verify_compute_liveness(st) && mir_verify_compute_defined(st);
}

void mir_regalloc_verify_sabotage(MirFunction *fn) {
  MirVerify st;
  int init = mir_verify_init(&st, fn);
  if (init <= 0) {
    return;
  }
  if (!mir_verify_analyse(&st)) {
    mir_verify_free(&st);
    return;
  }
  for (size_t i = 0; i < fn->insn_count; i++) {
    const MirInst *in = &fn->insns[i];
    MirVregId def = mir_verify_def(in);
    MirVreg *dv;
    const unsigned long long *defd = st.defd_in + i * st.words;
    if (in->op == MIR_NOP || !mir_verify_valid(&st, def)) {
      continue;
    }
    dv = &fn->vregs[def];
    if (!dv->in_register || !mir_verify_live_out(&st, i, st.scratch)) {
      continue;
    }
    for (size_t v = 0; v < st.n; v++) {
      const MirVreg *vr = &fn->vregs[v];
      if ((MirVregId)v == def || !st.has_def[v] || !vr->in_register ||
          vr->rclass != dv->rclass ||
          !mir_verify_bit_get(st.scratch, v) ||
          !mir_verify_bit_get(defd, v)) {
        continue;
      }
      fprintf(stderr, "RA-SABOTAGE\t%s\tv%d takes v%zu's register at insn %zu\n",
              mir_verify_fn_name(fn), (int)def, v, i);
      dv->phys = vr->phys;
      mir_verify_free(&st);
      return;
    }
  }
  mir_verify_free(&st);
}

int mir_regalloc_verify(MirFunction *fn) {
  MirVerify st;
  int init;
  int ok = 1;
  if (!fn || fn->vreg_count == 0 || fn->insn_count == 0) {
    return 1;
  }
  init = mir_verify_init(&st, fn);
  if (init < 0) {
    fprintf(stderr, "RA-VERIFY\t%s\tSKIP\tbudget\n", mir_verify_fn_name(fn));
    return 1;
  }
  if (init == 0) {
    return 1;
  }
  if (!mir_verify_analyse(&st)) {
    mir_verify_free(&st);
    return mir_verify_fail(fn, "liveness did not converge", 0, MIR_VREG_NONE,
                           MIR_VREG_NONE);
  }
  ok = mir_verify_check_assignments(&st) && mir_verify_check_entry(&st);
  for (size_t i = 0; ok && i < fn->insn_count; i++) {
    ok = mir_verify_check_insn(&st, i);
  }
  if (ok && getenv("METTLE_REGALLOC_TRACE")) {
    fprintf(stderr, "RA-VERIFY\t%s\tOK\n", mir_verify_fn_name(fn));
  }
  mir_verify_free(&st);
  return ok;
}
