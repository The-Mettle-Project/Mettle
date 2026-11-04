#include "codegen/binary/mir.h"
#include "codegen/binary/mir_cfg.h"
#include "codegen/binary/mir_machine.h"
#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const char *g_mir_ra_trace_name;

typedef struct {
  MirFunction *fn;
  const char *stage;
  const char **labels;
  size_t label_count;
  unsigned char *has_def;
} MirShape;

int mir_verify_enabled(void) {
  static int cached = -1;
  if (cached < 0) {
    cached = getenv("METTLE_MIR_VERIFY") ? 1 : 0;
  }
  return cached;
}

int mir_verify_sabotage_enabled(void) {
  static int cached = -1;
  if (cached < 0) {
    cached = getenv("METTLE_MIR_VERIFY_BREAK") ? 1 : 0;
  }
  return cached;
}

static void mir_verify_sabotage_def(MirFunction *fn) {
  unsigned *defs = (unsigned *)calloc(fn->vreg_count ? fn->vreg_count : 1,
                                      sizeof(unsigned));
  unsigned char *read = (unsigned char *)calloc(
      fn->vreg_count ? fn->vreg_count : 1, 1);
  MirVregId uses[6];
  if (!defs || !read) {
    free(defs);
    free(read);
    return;
  }
  for (size_t i = 0; i < fn->insn_count; i++) {
    MirVregId d = mir_cfg_insn_def(&fn->insns[i]);
    int n = mir_cfg_insn_uses(&fn->insns[i], uses);
    if (d >= 0 && (size_t)d < fn->vreg_count) {
      defs[d]++;
    }
    for (int k = 0; k < n; k++) {
      if (uses[k] >= 0 && (size_t)uses[k] < fn->vreg_count) {
        read[uses[k]] = 1;
      }
    }
  }
  for (size_t i = 0; i < fn->insn_count; i++) {
    MirVregId d = mir_cfg_insn_def(&fn->insns[i]);
    if (d >= 0 && (size_t)d < fn->vreg_count && defs[d] == 1 && read[d] &&
        !fn->vregs[d].may_read_undefined && !fn->vregs[d].address_taken) {
      fn->insns[i].op = MIR_NOP;
      break;
    }
  }
  free(defs);
  free(read);
}

void mir_verify_sabotage(MirFunction *fn) {
  const char *mode = getenv("METTLE_MIR_VERIFY_BREAK");
  if (mode && strcmp(mode, "def") == 0) {
    mir_verify_sabotage_def(fn);
    return;
  }
  for (size_t i = 0; i < fn->insn_count; i++) {
    MirInst *in = &fn->insns[i];
    if (in->op == MIR_JMP || in->op == MIR_JCC || in->op == MIR_CMPBR) {
      in->dst.sym = "__mir_verify_sabotage";
      return;
    }
  }
}

static const char *mir_shape_fn_name(const MirFunction *fn) {
  if (fn->ir_function && fn->ir_function->name) {
    return fn->ir_function->name;
  }
  if (fn->context && fn->context->function_name) {
    return fn->context->function_name;
  }
  return g_mir_ra_trace_name ? g_mir_ra_trace_name : "?";
}

static int mir_shape_fail(MirShape *st, size_t at, const char *what,
                          long long detail) {
  MirFunction *fn = st->fn;
  const char *name = mir_shape_fn_name(fn);
  const char *op_name =
      at < fn->insn_count ? mir_opcode_name(fn->insns[at].op) : "-";
  fprintf(stderr, "MIR-VERIFY\t%s\tFAIL\t%s\t%s\tinsn=%zu\t%s\t%lld\n", name,
          st->stage, what, at, op_name, detail);
  if (fn->generator && !fn->generator->has_error) {
    code_generator_set_error(fn->generator,
                             "MIR verifier: %s (%lld) at instruction %zu (%s) "
                             "after %s in function '%s'",
                             what, detail, at, op_name, st->stage, name);
  }
  fn->has_error = 1;
  return 0;
}

static int mir_shape_vreg_ok(const MirFunction *fn, MirVregId v) {
  return v >= 0 && (size_t)v < fn->vreg_count;
}

static int mir_shape_label_cmp(const void *a, const void *b) {
  const char *const *x = (const char *const *)a;
  const char *const *y = (const char *const *)b;
  return strcmp(*x, *y);
}

static int mir_shape_label_defined(const MirShape *st, const char *sym) {
  if (!sym || st->label_count == 0) {
    return 0;
  }
  return bsearch(&sym, st->labels, st->label_count, sizeof(*st->labels),
                 mir_shape_label_cmp) != NULL;
}

static int mir_shape_collect_labels(MirShape *st) {
  MirFunction *fn = st->fn;
  size_t n = 0;
  for (size_t i = 0; i < fn->insn_count; i++) {
    if (fn->insns[i].op == MIR_LABEL) {
      n++;
    }
  }
  st->labels = (const char **)malloc((n ? n : 1) * sizeof(*st->labels));
  if (!st->labels) {
    return 0;
  }
  for (size_t i = 0; i < fn->insn_count; i++) {
    const MirInst *in = &fn->insns[i];
    if (in->op != MIR_LABEL) {
      continue;
    }
    if (in->dst.kind != MIR_OPK_LABEL || !in->dst.sym) {
      return mir_shape_fail(st, i, "label without a name", in->dst.kind);
    }
    st->labels[st->label_count++] = in->dst.sym;
  }
  qsort(st->labels, st->label_count, sizeof(*st->labels), mir_shape_label_cmp);
  for (size_t k = 1; k < st->label_count; k++) {
    if (strcmp(st->labels[k - 1], st->labels[k]) == 0) {
      for (size_t i = 0; i < fn->insn_count; i++) {
        const MirInst *in = &fn->insns[i];
        if (in->op == MIR_LABEL && strcmp(in->dst.sym, st->labels[k]) == 0) {
          return mir_shape_fail(st, i, "label defined twice", (long long)k);
        }
      }
      return mir_shape_fail(st, 0, "label defined twice", (long long)k);
    }
  }
  return 1;
}

static int mir_shape_operand(MirShape *st, size_t at, const MirOperand *op,
                             long long which) {
  MirFunction *fn = st->fn;
  switch (op->kind) {
  case MIR_OPK_NONE:
  case MIR_OPK_IMM:
  case MIR_OPK_FIMM:
  case MIR_OPK_STACKHOME:
    return 1;
  case MIR_OPK_VREG:
    if (!mir_shape_vreg_ok(fn, op->vreg)) {
      return mir_shape_fail(st, at, "vreg out of range", which);
    }
    return 1;
  case MIR_OPK_PHYS:
    if (op->phys < 0 || op->phys > 15) {
      return mir_shape_fail(st, at, "physical register out of range", which);
    }
    return 1;
  case MIR_OPK_MEM:
    if (op->mem.base != MIR_VREG_NONE && !mir_shape_vreg_ok(fn, op->mem.base)) {
      return mir_shape_fail(st, at, "memory base vreg out of range", which);
    }
    if (op->mem.index != MIR_VREG_NONE &&
        !mir_shape_vreg_ok(fn, op->mem.index)) {
      return mir_shape_fail(st, at, "memory index vreg out of range", which);
    }
    if (op->mem.scale != 0 && op->mem.scale != 1 && op->mem.scale != 2 &&
        op->mem.scale != 4 && op->mem.scale != 8) {
      return mir_shape_fail(st, at, "memory scale not 1, 2, 4 or 8", which);
    }
    if (op->mem.phys_base_valid && (op->mem.phys_base < 0 ||
                                    op->mem.phys_base > 15)) {
      return mir_shape_fail(st, at, "memory physical base out of range", which);
    }
    if (op->mem.frame_home_valid && !mir_shape_vreg_ok(fn, op->mem.frame_home)) {
      return mir_shape_fail(st, at, "memory frame home out of range", which);
    }
    return 1;
  case MIR_OPK_LABEL:
  case MIR_OPK_SYMBOL:
    if (!op->sym) {
      return mir_shape_fail(st, at, "label or symbol operand without a name",
                            which);
    }
    return 1;
  default:
    return mir_shape_fail(st, at, "unknown operand kind", (long long)op->kind);
  }
}

static int mir_shape_branch_target(MirShape *st, size_t at) {
  const MirInst *in = &st->fn->insns[at];
  if (mir_op_has(in->op, MIR_OPF_LABEL_BRANCH)) {
    if (in->dst.kind != MIR_OPK_LABEL) {
      return mir_shape_fail(st, at, "branch target is not a label",
                            in->dst.kind);
    }
    if (!mir_shape_label_defined(st, in->dst.sym)) {
      return mir_shape_fail(st, at, "branch to a label the function lacks", 0);
    }
    return 1;
  }
  if (in->op == MIR_JMP_TABLE) {
    const MirJumpTable *jt = (const MirJumpTable *)in->aux;
    if (!jt || jt->count == 0) {
      return mir_shape_fail(st, at, "jump table without entries", 0);
    }
    for (size_t t = 0; t < jt->count; t++) {
      if (!mir_shape_label_defined(st, jt->labels[t])) {
        return mir_shape_fail(st, at, "jump table entry names a missing label",
                              (long long)t);
      }
    }
  }
  return 1;
}

static void mir_shape_note_def(MirShape *st, MirVregId v) {
  if (mir_shape_vreg_ok(st->fn, v)) {
    st->has_def[v] = 1;
  }
}

static int mir_shape_collect_defs(MirShape *st) {
  MirFunction *fn = st->fn;
  st->has_def = (unsigned char *)calloc(fn->vreg_count ? fn->vreg_count : 1, 1);
  if (!st->has_def) {
    return 0;
  }
  for (size_t p = 0; p < fn->param_count; p++) {
    mir_shape_note_def(st, fn->params[p].vreg);
    mir_shape_note_def(st, fn->params[p].sysv_storage);
  }
  if (fn->returns_indirect) {
    mir_shape_note_def(st, fn->indirect_return_vreg);
  }
  for (size_t i = 0; i < fn->insn_count; i++) {
    mir_shape_note_def(st, mir_cfg_insn_def(&fn->insns[i]));
  }
  return 1;
}

static int mir_shape_reads_defined(MirShape *st, size_t at) {
  MirFunction *fn = st->fn;
  MirVregId uses[6];
  int n = mir_cfg_insn_uses(&fn->insns[at], uses);
  for (int k = 0; k < n; k++) {
    MirVregId v = uses[k];
    if (v == MIR_VREG_NONE || !mir_shape_vreg_ok(fn, v)) {
      continue;
    }
    if (!st->has_def[v] && !fn->vregs[v].address_taken &&
        !fn->vregs[v].may_read_undefined) {
      return mir_shape_fail(st, at, "reads a vreg nothing defines", v);
    }
  }
  return 1;
}

static int mir_shape_assigned(MirShape *st, size_t at, MirVregId v) {
  const MirVreg *vr;
  if (v == MIR_VREG_NONE) {
    return 1;
  }
  vr = &st->fn->vregs[v];
  if (vr->in_register) {
    if (vr->phys < 0 || vr->phys > 15) {
      return mir_shape_fail(st, at, "vreg assigned an out-of-range register", v);
    }
    if (vr->rclass == MIR_RC_GP && vr->phys == BINARY_GP_RSP) {
      return mir_shape_fail(st, at, "vreg assigned the stack pointer", v);
    }
    if (vr->rclass == MIR_RC_GP &&
        (vr->phys == BINARY_GP_R10 || vr->phys == BINARY_GP_R11)) {
      return mir_shape_fail(st, at, "vreg assigned an encoder scratch register",
                            v);
    }
    if (vr->rclass == MIR_RC_XMM &&
        mir_xmm_is_encoder_scratch((BinaryXmmRegister)vr->phys)) {
      return mir_shape_fail(st, at,
                            "vreg assigned an encoder float scratch register",
                            v);
    }
    return 1;
  }
  if (vr->spill_offset <= 0) {
    return mir_shape_fail(st, at, "vreg used with neither a register nor a slot",
                          v);
  }
  return 1;
}

static int mir_shape_operand_assigned(MirShape *st, size_t at,
                                      const MirOperand *op) {
  if (op->kind == MIR_OPK_VREG) {
    return mir_shape_assigned(st, at, op->vreg);
  }
  if (op->kind == MIR_OPK_MEM) {
    return mir_shape_assigned(st, at, op->mem.base) &&
           mir_shape_assigned(st, at, op->mem.index);
  }
  return 1;
}

static int mir_shape_position_covers(MirShape *st, size_t at, MirVregId v) {
  const MirVreg *vr;
  if (v == MIR_VREG_NONE || !mir_shape_vreg_ok(st->fn, v)) {
    return 1;
  }
  vr = &st->fn->vregs[v];
  if (at < (size_t)(vr->live_start < 0 ? 0 : vr->live_start) ||
      vr->live_end < 0 || at > (size_t)vr->live_end) {
    return mir_shape_fail(st, at, "code moved outside its vreg's live positions",
                          v);
  }
  return 1;
}

static int mir_shape_moved_positions(MirShape *st, size_t at) {
  const MirInst *in = &st->fn->insns[at];
  MirVregId uses[6];
  int n;
  if (!st->fn->code_moved_after_allocation) {
    return 1;
  }
  n = mir_cfg_insn_uses(in, uses);
  for (int k = 0; k < n; k++) {
    if (!mir_shape_position_covers(st, at, uses[k])) {
      return 0;
    }
  }
  return mir_shape_position_covers(st, at, mir_cfg_insn_def(in));
}

int mir_verify_structure(MirFunction *fn, const char *stage, int allocated) {
  MirShape st;
  int ok = 1;
  if (!fn || fn->has_error) {
    return fn ? 0 : 1;
  }
  memset(&st, 0, sizeof(st));
  st.fn = fn;
  st.stage = stage ? stage : "?";
  if (!mir_shape_collect_labels(&st) || !mir_shape_collect_defs(&st)) {
    if (!fn->has_error) {
      mir_shape_fail(&st, 0, "out of memory", 0);
    }
    free(st.labels);
    free(st.has_def);
    return 0;
  }
  for (size_t i = 0; ok && i < fn->insn_count; i++) {
    const MirInst *in = &fn->insns[i];
    if ((int)in->op < 0 || in->op >= MIR_OPCODE_COUNT) {
      ok = mir_shape_fail(&st, i, "opcode out of range", (long long)in->op);
      break;
    }
    if (in->op == MIR_NOP) {
      continue;
    }
    if (in->width < 0 || in->width > 64) {
      ok = mir_shape_fail(&st, i, "width out of range", in->width);
      break;
    }
    ok = mir_shape_operand(&st, i, &in->dst, 0) &&
         mir_shape_operand(&st, i, &in->a, 1) &&
         mir_shape_operand(&st, i, &in->b, 2) &&
         mir_shape_branch_target(&st, i) && mir_shape_reads_defined(&st, i);
    if (ok && allocated) {
      ok = mir_shape_operand_assigned(&st, i, &in->dst) &&
           mir_shape_operand_assigned(&st, i, &in->a) &&
           mir_shape_operand_assigned(&st, i, &in->b) &&
           mir_shape_moved_positions(&st, i);
    }
  }
  free(st.labels);
  free(st.has_def);
  return ok;
}
