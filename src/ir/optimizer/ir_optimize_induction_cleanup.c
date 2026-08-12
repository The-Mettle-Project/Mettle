#include "ir_optimize_internal.h"

/* ---- congruent induction-variable elimination ----------------------------
 *
 * Several hand-unrolled kernels carry a fan of induction variables that are all
 * constant offsets of one another and step in lockstep, e.g. matmul's inner
 * loop:
 *
 *     b1_idx = b0_idx + 1; b2_idx = b0_idx + 2; b3_idx = b0_idx + 3;  // pre-loop
 *     while (k < N) { ... use b{0..3}_idx ...; b0_idx += N; b1_idx += N; ... }
 *
 * Throughout the loop b1_idx == b0_idx + 1 etc., so the b1..b3 variables are
 * redundant: each costs a load/add/store every iteration plus a live stack slot
 * (which, under the 7-register promotion budget, is what forces the hot
 * accumulators to spill). This pass replaces every in-loop read of a derived IV
 * with a fresh `base + C` recompute and deletes the derived IV's init and
 * increment. The recompute lands as `(base + C) << k; ptr + ...; load/store`,
 * which the backend's offset-scaled-address fold collapses into a single
 * displacement memory op, so the net effect is: 3 induction variables and
 * their per-iteration maintenance vanish, addresses gain a constant disp, and
 * register pressure drops by the eliminated IVs.
 *
 * Only runs under -O/--release (the whole optimizer is gated there), so the
 * differential fuzzer's unoptimized debug build is an independent oracle. */

#define IR_CIV_MAX_GROUP 32

static int ir_civ_operand_is_sym(const IROperand *op, const char *name) {
  return op && op->kind == IR_OPERAND_SYMBOL && op->name && name &&
         strcmp(op->name, name) == 0;
}

static int ir_civ_instruction_mentions_sym(const IRInstruction *in,
                                            const char *name) {
  if (!in) {
    return 0;
  }
  if (ir_civ_operand_is_sym(&in->dest, name) ||
      ir_civ_operand_is_sym(&in->lhs, name) ||
      ir_civ_operand_is_sym(&in->rhs, name)) {
    return 1;
  }
  for (size_t a = 0; a < in->argument_count; a++) {
    if (ir_civ_operand_is_sym(&in->arguments[a], name)) {
      return 1;
    }
  }
  return 0;
}

static int ir_civ_steps_match(const IROperand *a, const IROperand *b) {
  if (!a || !b || a->kind != b->kind) {
    return 0;
  }
  if (a->kind == IR_OPERAND_INT) {
    return a->int_value == b->int_value;
  }
  if (a->kind == IR_OPERAND_SYMBOL || a->kind == IR_OPERAND_TEMP) {
    return a->name && b->name && strcmp(a->name, b->name) == 0;
  }
  return 0;
}

/* Recognize `v = v + S` (or `v = S + v`): the canonical induction step. Returns
 * the stepped symbol name and a pointer to the step operand S. */
static int ir_civ_is_self_add(const IRInstruction *in, const char **name_out,
                              const IROperand **step_out) {
  if (!in || in->op != IR_OP_BINARY || in->is_float || !in->text ||
      strcmp(in->text, "+") != 0 || in->dest.kind != IR_OPERAND_SYMBOL ||
      !in->dest.name) {
    return 0;
  }
  if (ir_civ_operand_is_sym(&in->lhs, in->dest.name) &&
      in->rhs.kind != IR_OPERAND_NONE) {
    *name_out = in->dest.name;
    *step_out = &in->rhs;
    return 1;
  }
  if (ir_civ_operand_is_sym(&in->rhs, in->dest.name) &&
      in->lhs.kind != IR_OPERAND_NONE) {
    *name_out = in->dest.name;
    *step_out = &in->lhs;
    return 1;
  }
  return 0;
}

/* Find a pre-loop init `v = base + C` (base a symbol, C an int constant) in
 * [0, label_idx). Fills base/offset and the init's index. */
static int ir_civ_find_offset_init(const IRFunction *function, size_t label_idx,
                                   const char *v, const char **base_out,
                                   long long *offset_out, size_t *init_idx_out) {
  for (size_t i = 0; i < label_idx; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (in->op != IR_OP_BINARY || in->is_float || !in->text ||
        strcmp(in->text, "+") != 0 ||
        !ir_civ_operand_is_sym(&in->dest, v)) {
      continue;
    }
    if (in->lhs.kind == IR_OPERAND_SYMBOL && in->lhs.name &&
        in->rhs.kind == IR_OPERAND_INT) {
      *base_out = in->lhs.name;
      *offset_out = in->rhs.int_value;
      *init_idx_out = i;
      return 1;
    }
    if (in->rhs.kind == IR_OPERAND_SYMBOL && in->rhs.name &&
        in->lhs.kind == IR_OPERAND_INT) {
      *base_out = in->rhs.name;
      *offset_out = in->lhs.int_value;
      *init_idx_out = i;
      return 1;
    }
  }
  return 0;
}

/* Count writes (BINARY/ASSIGN with dest == name) to a symbol across the whole
 * function; used to confirm a derived IV is written only by its init+step. */
static size_t ir_civ_symbol_write_count(const IRFunction *function,
                                        const char *name) {
  size_t count = 0;
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (in->op == IR_OP_DECLARE_LOCAL) {
      continue;
    }
    if (ir_civ_operand_is_sym(&in->dest, name)) {
      count++;
    }
  }
  return count;
}

int ir_eliminate_congruent_ivs_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }

  char derived_names[IR_CIV_MAX_GROUP][256];
  char derived_base[IR_CIV_MAX_GROUP][256];
  long long derived_off[IR_CIV_MAX_GROUP];
  size_t derived_count = 0;

  for (size_t jump_idx = 0; jump_idx < function->instruction_count; jump_idx++) {
    const IRInstruction *jump = &function->instructions[jump_idx];
    if (!jump || jump->op != IR_OP_JUMP || !jump->text) {
      continue;
    }

    /* Locate the loop header label preceding this back-edge. */
    size_t label_idx = jump_idx;
    int have_label = 0;
    for (size_t li = 0; li < jump_idx; li++) {
      const IRInstruction *lab = &function->instructions[li];
      if (lab->op == IR_OP_LABEL && lab->text &&
          strcmp(lab->text, jump->text) == 0) {
        label_idx = li;
        have_label = 1;
        break;
      }
    }
    if (!have_label) {
      continue;
    }

    /* Collect induction steps (v = v + S) in the loop body. */
    const char *iv_name[IR_CIV_MAX_GROUP];
    const IROperand *iv_step[IR_CIV_MAX_GROUP];
    size_t iv_count = 0;
    for (size_t i = label_idx + 1; i < jump_idx; i++) {
      const char *nm = NULL;
      const IROperand *step = NULL;
      if (ir_civ_is_self_add(&function->instructions[i], &nm, &step)) {
        if (iv_count < IR_CIV_MAX_GROUP) {
          iv_name[iv_count] = nm;
          iv_step[iv_count] = step;
          iv_count++;
        }
      }
    }
    if (iv_count < 2) {
      continue;
    }

    /* For each IV, classify as derived (pre-loop init `v = base + C`, base a
     * same-step IV, written only by init+step, dead outside the loop, used
     * inside only as a plain lhs/rhs operand). */
    for (size_t d = 0; d < iv_count; d++) {
      const char *v = iv_name[d];
      const char *base = NULL;
      long long offset = 0;
      size_t init_idx = 0;

      if (derived_count >= IR_CIV_MAX_GROUP) {
        break;
      }
      if (!ir_civ_find_offset_init(function, label_idx, v, &base, &offset,
                                   &init_idx)) {
        continue;
      }
      /* base must be a DIFFERENT, same-step IV in this loop and itself NOT
       * derived from another (avoid chained rewrites referencing a base we are
       * about to delete). */
      int base_is_group_member = 0;
      int base_is_derived = 0;
      for (size_t g = 0; g < iv_count; g++) {
        if (strcmp(iv_name[g], base) == 0) {
          base_is_group_member =
              ir_civ_steps_match(iv_step[g], iv_step[d]);
        }
      }
      if (!base_is_group_member || strcmp(base, v) == 0) {
        continue;
      }
      /* base must not itself have a `base = other + C` pre-loop init. */
      {
        const char *bb = NULL;
        long long bo = 0;
        size_t bi = 0;
        if (ir_civ_find_offset_init(function, label_idx, base, &bb, &bo, &bi)) {
          base_is_derived = 1;
        }
      }
      if (base_is_derived) {
        continue;
      }
      /* v written only by its init + its loop step. */
      if (ir_civ_symbol_write_count(function, v) != 2) {
        continue;
      }
      /* No mention of v outside [label_idx, jump_idx] except its DECLARE_LOCAL
       * and the init we will delete. */
      int ok = 1;
      for (size_t i = 0; i < function->instruction_count && ok; i++) {
        if (i >= label_idx && i <= jump_idx) {
          continue;
        }
        if (i == init_idx) {
          continue;
        }
        const IRInstruction *in = &function->instructions[i];
        if (in->op == IR_OP_DECLARE_LOCAL) {
          continue;
        }
        if (ir_civ_instruction_mentions_sym(in, v)) {
          ok = 0;
        }
      }
      if (!ok) {
        continue;
      }
      /* Inside the loop, every mention of v outside its own step must be a
       * plain lhs/rhs read (no dest write, no argument use). */
      for (size_t i = label_idx + 1; i < jump_idx && ok; i++) {
        const IRInstruction *in = &function->instructions[i];
        const char *snm = NULL;
        const IROperand *sstep = NULL;
        if (ir_civ_is_self_add(in, &snm, &sstep) && strcmp(snm, v) == 0) {
          continue; /* the step itself */
        }
        if (ir_civ_operand_is_sym(&in->dest, v)) {
          ok = 0;
          break;
        }
        for (size_t a = 0; a < in->argument_count; a++) {
          if (ir_civ_operand_is_sym(&in->arguments[a], v)) {
            ok = 0;
            break;
          }
        }
      }
      if (!ok) {
        continue;
      }

      /* Accept this derived IV. */
      if (strlen(v) >= sizeof(derived_names[0]) ||
          strlen(base) >= sizeof(derived_base[0])) {
        continue;
      }
      strcpy(derived_names[derived_count], v);
      strcpy(derived_base[derived_count], base);
      derived_off[derived_count] = offset;
      derived_count++;
    }
  }

  if (derived_count == 0) {
    return 1;
  }

  /* Mutation pass: NOP every write to a derived IV (init + step), and replace
   * each read with a freshly recomputed `base + C` temp. Names are matched
   * (not indices), so the in-place insertions below can shift the array
   * without invalidating the plan. */
  static unsigned long long civ_tmp_serial = 0;
  size_t i = 0;
  while (i < function->instruction_count) {
    IRInstruction *insn = &function->instructions[i];

    /* Delete writes to derived IVs (their init and increment). */
    if ((insn->op == IR_OP_BINARY) && insn->dest.kind == IR_OPERAND_SYMBOL &&
        insn->dest.name) {
      int is_derived_dest = 0;
      for (size_t d = 0; d < derived_count; d++) {
        if (strcmp(insn->dest.name, derived_names[d]) == 0) {
          is_derived_dest = 1;
          break;
        }
      }
      if (is_derived_dest) {
        ir_instruction_make_nop(insn);
        if (changed) {
          *changed = 1;
        }
        i++;
        continue;
      }
    }

    /* Replace a read of a derived IV (lhs/rhs) with a recomputed base+C. */
    int hit = -1;
    if (insn->lhs.kind == IR_OPERAND_SYMBOL && insn->lhs.name) {
      for (size_t d = 0; d < derived_count; d++) {
        if (strcmp(insn->lhs.name, derived_names[d]) == 0) {
          hit = (int)d;
          break;
        }
      }
    }
    int hit_rhs = -1;
    if (hit < 0 && insn->rhs.kind == IR_OPERAND_SYMBOL && insn->rhs.name) {
      for (size_t d = 0; d < derived_count; d++) {
        if (strcmp(insn->rhs.name, derived_names[d]) == 0) {
          hit_rhs = (int)d;
          break;
        }
      }
    }

    if (hit >= 0 || hit_rhs >= 0) {
      int d = hit >= 0 ? hit : hit_rhs;
      char tmpname[64];
      snprintf(tmpname, sizeof(tmpname), "__civ%llu", civ_tmp_serial++);

      IRInstruction recompute;
      memset(&recompute, 0, sizeof(recompute));
      recompute.op = IR_OP_BINARY;
      recompute.location = insn->location;
      recompute.text = (char *)"+";
      recompute.dest.kind = IR_OPERAND_TEMP;
      recompute.dest.name = tmpname;
      recompute.lhs.kind = IR_OPERAND_SYMBOL;
      recompute.lhs.name = derived_base[d];
      recompute.rhs.kind = IR_OPERAND_INT;
      recompute.rhs.int_value = derived_off[d];

      if (!ir_function_insert_instruction(function, i, &recompute)) {
        return 0;
      }
      /* insn shifted to i+1; repoint the operand(s) that named the derived IV
       * to the new temp. */
      IRInstruction *moved = &function->instructions[i + 1];
      if (ir_civ_operand_is_sym(&moved->lhs, derived_names[d])) {
        ir_operand_destroy(&moved->lhs);
        moved->lhs.kind = IR_OPERAND_TEMP;
        moved->lhs.name = mettle_strdup(tmpname);
      }
      if (ir_civ_operand_is_sym(&moved->rhs, derived_names[d])) {
        ir_operand_destroy(&moved->rhs);
        moved->rhs.kind = IR_OPERAND_TEMP;
        moved->rhs.name = mettle_strdup(tmpname);
      }
      if (changed) {
        *changed = 1;
      }
      /* Re-examine the moved instruction (it may still read another derived
       * IV); the inserted recompute reads only `base`, never a derived IV. */
      i++;
      continue;
    }

    i++;
  }

  return 1;
}
