#include "ir_optimize_internal.h"

/* Reads of `sym` in one instruction: lhs/rhs/arguments, plus dest on a STORE
 * (a store's dest is the address operand, which is read, not written). */
static size_t ir_load_copy_count_symbol_reads(const IRInstruction *ins,
                                              const char *sym) {
  size_t count = 0;
  if (ir_operand_is_symbol_named(&ins->lhs, sym)) {
    count++;
  }
  if (ir_operand_is_symbol_named(&ins->rhs, sym)) {
    count++;
  }
  if (ins->op == IR_OP_STORE && ir_operand_is_symbol_named(&ins->dest, sym)) {
    count++;
  }
  for (size_t a = 0; a < ins->argument_count; a++) {
    if (ir_operand_is_symbol_named(&ins->arguments[a], sym)) {
      count++;
    }
  }
  return count;
}

static void ir_load_copy_replace_operand(IROperand *operand, const char *sym,
                                         const char *temp) {
  if (!ir_operand_is_symbol_named(operand, sym)) {
    return;
  }
  int float_bits = operand->float_bits;
  ir_operand_destroy(operand);
  *operand = ir_operand_temp(temp);
  /* Keep the IEEE-754 width tag the symbol operand carried; consumers use it
   * to tell float32 values from the default double width. */
  operand->float_bits = float_bits;
}

/* A loop's entry label, as opposed to the exit label that carries it as a
 * prefix (`ir_while_9` vs `ir_while_end_9`). */
static int ir_cleanup_label_is_loop_header(const char *label) {
  if (!label) {
    return 0;
  }
  if (strstr(label, "ir_for_cond_") != NULL) {
    return 1;
  }
  return strstr(label, "ir_while_") != NULL &&
         strstr(label, "ir_while_end_") == NULL;
}

/* The latch: the last jump back to `label`. Everything between the header and
 * it is the loop, body and nested loops alike. */
static size_t ir_cleanup_loop_latch(const IRFunction *function, size_t header,
                                    const char *label) {
  size_t latch = 0;
  for (size_t i = header + 1; i < function->instruction_count; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_JUMP && ins->text && strcmp(ins->text, label) == 0) {
      latch = i;
    }
  }
  return latch;
}

/* Move a loop body's declarations above its header.
 *
 * `var v: int32 = a[i] * 2;` in a loop body lowers to a DECLARE_LOCAL followed
 * by a separate store; the declaration names storage and carries no value, so
 * where it sits is free. It is not free to the recognizers: each walks a body
 * expecting load->compute->store and stops at the first instruction it does not
 * model, so one declaration between a load and the arithmetic costs the loop
 * its kernel. That is why `--explain` has been telling writers to "declare `v`
 * before the loop" -- advice for an edit the compiler can make itself. Doing it
 * here means idiomatic code (a named intermediate per iteration) vectorizes as
 * readily as the same loop written as one expression. */
int ir_hoist_body_locals_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }
  for (size_t header = 0; header < function->instruction_count; header++) {
    const IRInstruction *label = &function->instructions[header];
    size_t latch = 0;
    size_t insert = header;

    if (label->op != IR_OP_LABEL ||
        !ir_cleanup_label_is_loop_header(label->text)) {
      continue;
    }
    latch = ir_cleanup_loop_latch(function, header, label->text);
    if (!latch) {
      continue;
    }

    for (size_t i = header + 1; i < latch; i++) {
      IRInstruction saved;
      if (function->instructions[i].op != IR_OP_DECLARE_LOCAL ||
          function->instructions[i].dest.kind != IR_OPERAND_SYMBOL) {
        continue;
      }
      saved = function->instructions[i];
      memmove(&function->instructions[insert + 1],
              &function->instructions[insert],
              (i - insert) * sizeof(IRInstruction));
      function->instructions[insert] = saved;
      insert++;
      if (changed) {
        *changed = 1;
      }
    }
    /* The declarations landed before the header, so the header moved down by
     * as many; resume the outer scan from it rather than re-reading them. */
    header = insert;
  }
  return 1;
}

/* Does the loop canonical form actually hold?
 *
 * The stage driver proves the canonicalizers CONVERGED, which is a statement
 * about the passes rather than about the IR. It is satisfied by a matcher that
 * has quietly stopped recognizing its shape: the pass reports clean, the stage
 * settles, and every recognizer downstream then fails to match for a reason
 * nothing reports. That is exactly how a recognizer rots.
 *
 * So this states the property independently of the pass that establishes it.
 * The redundancy is the point: two separate statements of one invariant, and a
 * disagreement means the hoister stopped doing its job. It asserts only what
 * ir_hoist_body_locals_pass guarantees unconditionally -- no declaration
 * survives inside a loop body -- because the global-base hoister legitimately
 * declines (a base whose pointer type is unknown, a temp that escapes), and a
 * verifier that reports those would be crying wolf about correct output.
 *
 * Returns 1 when the form holds. On failure fills `detail` with the loop and
 * the symbol, which is the whole diagnostic value: it names what to look at. */
int ir_verify_loop_canonical_form(const IRFunction *function, char *detail,
                                  size_t detail_size) {
  if (!function || function->instruction_count == 0) {
    return 1;
  }

  /* Three linear sweeps rather than a latch search per header.
   *
   * The obvious formulation -- for every loop header, scan forward for its
   * back edge -- costs a full tail walk per loop, which is quadratic in the
   * number of loops one function holds. That is invisible on ordinary code
   * and very visible on generated code: a single function with 400 sequential
   * loops paid 35% more compile time for this check alone, and the check runs
   * on every build. So: collect the headers, resolve every back edge in one
   * pass over the jumps, then sweep once carrying the furthest back edge of
   * any loop opened so far, which answers "is this instruction inside a
   * loop" in constant time per instruction. */
  size_t count = 0;
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_LABEL && ir_cleanup_label_is_loop_header(ins->text)) {
      count++;
    }
  }
  if (count == 0) {
    return 1;
  }

  size_t *header_idx = (size_t *)malloc(count * sizeof(size_t));
  size_t *latch = (size_t *)calloc(count, sizeof(size_t));
  const char **header_label =
      (const char **)malloc(count * sizeof(const char *));
  if (!header_idx || !latch || !header_label) {
    /* Out of memory checking an invariant is not a reason to fail the build;
     * the passes themselves still ran. */
    free(header_idx);
    free(latch);
    free(header_label);
    return 1;
  }

  size_t h = 0;
  for (size_t i = 0; i < function->instruction_count && h < count; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_LABEL && ir_cleanup_label_is_loop_header(ins->text)) {
      header_idx[h] = i;
      header_label[h] = ins->text;
      h++;
    }
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op != IR_OP_JUMP || !ins->text) {
      continue;
    }
    for (size_t k = 0; k < count; k++) {
      if (header_idx[k] < i && header_label[k] &&
          strcmp(header_label[k], ins->text) == 0) {
        if (i > latch[k]) {
          latch[k] = i;
        }
        break;
      }
    }
  }

  int ok = 1;
  size_t next_header = 0;
  size_t reach = 0; /* furthest back edge of any loop opened at or before i */
  for (size_t i = 0; i < function->instruction_count && ok; i++) {
    while (next_header < count && header_idx[next_header] <= i) {
      if (latch[next_header] > reach) {
        reach = latch[next_header];
      }
      next_header++;
    }
    if (reach <= i) {
      continue; /* outside every loop */
    }
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op != IR_OP_DECLARE_LOCAL ||
        ins->dest.kind != IR_OPERAND_SYMBOL) {
      continue;
    }
    if (detail && detail_size) {
      /* Name the innermost enclosing loop. Only the failure path pays for
       * this search, so it can be the simple one. */
      const char *loop = "?";
      for (size_t k = 0; k < count; k++) {
        if (header_idx[k] < i && latch[k] > i) {
          loop = header_label[k] ? header_label[k] : "?";
        }
      }
      snprintf(detail, detail_size,
               "declaration of '%s' still inside loop '%s'",
               ins->dest.name ? ins->dest.name : "?", loop);
    }
    ok = 0;
  }

  free(header_idx);
  free(latch);
  free(header_label);
  return ok;
}

static const char *ir_hoist_element_pointer_type(const IRInstruction *mem) {
  if (mem->rhs.kind != IR_OPERAND_INT) {
    return NULL;
  }
  if (mem->is_float) {
    return mem->rhs.int_value == 4 ? "float32*" : "float64*";
  }
  switch (mem->rhs.int_value) {
  case 1: return mem->is_unsigned ? "uint8*" : "int8*";
  case 2: return mem->is_unsigned ? "uint16*" : "int16*";
  case 8: return mem->is_unsigned ? "uint64*" : "int64*";
  default: return mem->is_unsigned ? "uint32*" : "int32*";
  }
}

/* The address operand of a memory op, or NULL if it is not one. A store's dest
 * is the address it writes through. */
static const IROperand *ir_hoist_memory_address(const IRInstruction *ins) {
  if (ins->op == IR_OP_LOAD) {
    return &ins->lhs;
  }
  if (ins->op == IR_OP_STORE) {
    return &ins->dest;
  }
  return NULL;
}

/* The element type reached through `addr_temp`, as a pointer type for the
 * hoisted base's declaration. An index lands one `+` past the base, so follow
 * that step as well as reading straight through. Returning NULL means nothing
 * in the loop indexes off this address, and there is no base worth naming. */
static const char *ir_hoist_base_pointer_type(const IRFunction *function,
                                              size_t lo, size_t hi,
                                              const char *addr_temp) {
  for (size_t i = lo; i < hi; i++) {
    const IRInstruction *ins = &function->instructions[i];
    const IROperand *addr = ir_hoist_memory_address(ins);
    const char *derived = NULL;
    if (addr && ir_operand_is_temp_named(addr, addr_temp)) {
      return ir_hoist_element_pointer_type(ins);
    }
    if (ins->op != IR_OP_BINARY || ins->is_float || !ins->text ||
        strcmp(ins->text, "+") != 0 || ins->dest.kind != IR_OPERAND_TEMP ||
        !ins->dest.name ||
        !(ir_operand_is_temp_named(&ins->lhs, addr_temp) ||
          ir_operand_is_temp_named(&ins->rhs, addr_temp))) {
      continue;
    }
    derived = ins->dest.name;
    for (size_t j = i + 1; j < hi; j++) {
      const IRInstruction *mem = &function->instructions[j];
      const IROperand *maddr = ir_hoist_memory_address(mem);
      if (maddr && ir_operand_is_temp_named(maddr, derived)) {
        return ir_hoist_element_pointer_type(mem);
      }
    }
  }
  return NULL;
}

/* True if `sym` names a global rather than anything this function declares. */
static int ir_hoist_symbol_is_global(const IRFunction *function,
                                     const char *sym) {
  return sym && !ir_function_symbol_is_parameter(function, sym) &&
         ir_function_local_declared_type(function, sym) == NULL;
}

static void ir_hoist_rename_temp_reads(IRFunction *function, size_t lo,
                                       size_t hi, const char *temp,
                                       const char *sym) {
  for (size_t i = lo; i < hi; i++) {
    IRInstruction *ins = &function->instructions[i];
    IROperand *slots[3];
    slots[0] = &ins->lhs;
    slots[1] = &ins->rhs;
    slots[2] = (ins->op == IR_OP_STORE) ? &ins->dest : NULL;
    for (int k = 0; k < 3; k++) {
      if (!slots[k] || !ir_operand_is_temp_named(slots[k], temp)) {
        continue;
      }
      {
        int float_bits = slots[k]->float_bits;
        ir_operand_destroy(slots[k]);
        *slots[k] = ir_operand_symbol(sym);
        slots[k]->float_bits = float_bits;
      }
    }
    for (size_t a = 0; a < ins->argument_count; a++) {
      if (ir_operand_is_temp_named(&ins->arguments[a], temp)) {
        int float_bits = ins->arguments[a].float_bits;
        ir_operand_destroy(&ins->arguments[a]);
        ins->arguments[a] = ir_operand_symbol(sym);
        ins->arguments[a].float_bits = float_bits;
      }
    }
  }
}

/* Reads of a temp anywhere outside [lo,hi). */
static int ir_hoist_temp_escapes(const IRFunction *function, size_t lo,
                                 size_t hi, const char *temp) {
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (i >= lo && i < hi) {
      continue;
    }
    if (ir_operand_is_temp_named(&ins->lhs, temp) ||
        ir_operand_is_temp_named(&ins->rhs, temp) ||
        ir_operand_is_temp_named(&ins->dest, temp)) {
      return 1;
    }
    for (size_t a = 0; a < ins->argument_count; a++) {
      if (ir_operand_is_temp_named(&ins->arguments[a], temp)) {
        return 1;
      }
    }
  }
  return 0;
}

/* Give a loop-invariant `&global` a name above the loop.
 *
 * `G[i]`, where G is a global array, lowers to `%t <- &@G` INSIDE the body and
 * then indexes off the temp. Every recognizer reads a base as a symbol, so the
 * temp hid the array from all of them at once: a program that keeps its buffers
 * at file scope, which is how most C-shaped code is written, vectorized
 * nowhere. `--explain` has been printing "hoist the invariant part of the index
 * into a base pointer before the loop" for exactly this, which is advice for an
 * edit the compiler can make itself.
 *
 * A global's address is a link-time constant, so the hoist is unconditional and
 * needs no invariance proof. Doing it here rather than in each recognizer means
 * every kernel gains global arrays at once, and none of them had to learn a
 * second spelling of a base. */
int ir_hoist_global_bases_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }
  for (size_t header = 0; header < function->instruction_count; header++) {
    char loop_label[128];
    size_t latch = 0;

    {
      const IRInstruction *label = &function->instructions[header];
      if (label->op != IR_OP_LABEL ||
          !ir_cleanup_label_is_loop_header(label->text) ||
          snprintf(loop_label, sizeof(loop_label), "%s", label->text) >=
              (int)sizeof(loop_label)) {
        continue;
      }
    }
    latch = ir_cleanup_loop_latch(function, header, loop_label);
    if (!latch) {
      continue;
    }

    /* Every insertion below can realloc the instruction array, so nothing here
     * holds a pointer into it across one. */
    for (size_t i = header + 1; i < latch; i++) {
      char base_name[128];
      char global[128];
      char temp[128];
      const char *ptr_type = NULL;
      IRInstruction decl = {0};
      IRInstruction init = {0};
      int already = 0;

      {
        const IRInstruction *addr = &function->instructions[i];
        if (addr->op != IR_OP_ADDRESS_OF ||
            addr->dest.kind != IR_OPERAND_TEMP || !addr->dest.name ||
            addr->lhs.kind != IR_OPERAND_SYMBOL || !addr->lhs.name ||
            snprintf(global, sizeof(global), "%s", addr->lhs.name) >=
                (int)sizeof(global) ||
            snprintf(temp, sizeof(temp), "%s", addr->dest.name) >=
                (int)sizeof(temp)) {
          continue;
        }
      }
      if (!ir_hoist_symbol_is_global(function, global)) {
        continue;
      }
      ptr_type = ir_hoist_base_pointer_type(function, i, latch, temp);
      if (!ptr_type || ir_hoist_temp_escapes(function, header, latch, temp)) {
        continue;
      }
      /* Named per loop, so one body's several `&@G` share a base while a
       * sibling loop gets its own and stays independent of this one's region. */
      if (snprintf(base_name, sizeof(base_name), "__gbase_%s_%s", loop_label,
                   global) >= (int)sizeof(base_name)) {
        continue;
      }
      already = ir_function_local_declared_type(function, base_name) != NULL;

      ir_hoist_rename_temp_reads(function, i, latch, temp, base_name);
      ir_instruction_make_nop(&function->instructions[i]);
      if (changed) {
        *changed = 1;
      }
      if (already) {
        continue;
      }
      decl.op = IR_OP_DECLARE_LOCAL;
      decl.dest = ir_operand_symbol(base_name);
      decl.text = mettle_strdup(ptr_type);
      init.op = IR_OP_ADDRESS_OF;
      init.dest = ir_operand_symbol(base_name);
      init.lhs = ir_operand_symbol(global);
      if (!decl.dest.name || !decl.text || !init.dest.name || !init.lhs.name ||
          !ir_function_insert_instruction(function, header, &decl) ||
          !ir_function_insert_instruction(function, header + 1, &init)) {
        ir_instruction_destroy_storage(&decl);
        ir_instruction_destroy_storage(&init);
        return 0;
      }
      ir_instruction_destroy_storage(&decl);
      ir_instruction_destroy_storage(&init);
      /* The pair landed before the label, so the label, this instruction and
       * the latch all sit two further along. */
      header += 2;
      i += 2;
      latch += 2;
    }
  }
  return 1;
}

/* Is `sym` written by any instruction in [lo, hi)? */
static int ir_row_symbol_written(const IRFunction *function, size_t lo,
                                 size_t hi, const char *sym) {
  for (size_t i = lo; i < hi && i < function->instruction_count; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ir_instruction_writes_destination(ins) &&
        ins->dest.kind == IR_OPERAND_SYMBOL && ins->dest.name &&
        strcmp(ins->dest.name, sym) == 0) {
      return 1;
    }
  }
  return 0;
}

/* The pointer type `sym` carries in this function: a local's declared type or
 * a parameter's. NULL unless it actually is a pointer. */
static const char *ir_row_symbol_pointer_type(const IRFunction *function,
                                              const char *sym) {
  const char *t = ir_function_local_declared_type(function, sym);
  if (!t) {
    for (size_t p = 0; p < function->parameter_count; p++) {
      if (function->parameter_names[p] &&
          strcmp(function->parameter_names[p], sym) == 0) {
        t = function->parameter_types[p];
        break;
      }
    }
  }
  if (!t) {
    return NULL;
  }
  size_t len = strlen(t);
  if ((len > 0 && t[len - 1] == '*') || strcmp(t, "cstring") == 0 ||
      strcmp(t, "rawptr") == 0) {
    return t;
  }
  return NULL;
}

/* Does `ins` read the temp named `name` anywhere: lhs/rhs, a store's address,
 * or a call/SIMD argument? */
static int ir_row_instruction_reads_temp(const IRInstruction *ins,
                                         const char *name) {
  if (ir_operand_is_temp_named(&ins->lhs, name) ||
      ir_operand_is_temp_named(&ins->rhs, name)) {
    return 1;
  }
  if (ins->op == IR_OP_STORE && ir_operand_is_temp_named(&ins->dest, name)) {
    return 1;
  }
  for (size_t a = 0; a < ins->argument_count; a++) {
    if (ir_operand_is_temp_named(&ins->arguments[a], name)) {
      return 1;
    }
  }
  return 0;
}

/* One side of an index add that the loop never changes: a symbol neither
 * written inside [lo, hi) nor aliasable through a taken address. A literal
 * deliberately does NOT count: `a[i + 3]` is the straight-line shape the SLP
 * matcher and the backend's constant-displacement fold already consume, and
 * splitting its base away breaks their view of adjacent accesses. */
static int ir_row_operand_invariant(const IRFunction *function, size_t lo,
                                    size_t hi, const IROperand *op) {
  return op->kind == IR_OPERAND_SYMBOL && op->name &&
         !ir_row_symbol_written(function, lo, hi, op->name) &&
         !ir_symbol_address_taken(function, op->name);
}

#define IR_ROW_MAX_CONSUMERS 8

/* Hoist the invariant half of an indexed access out of the loop.
 *
 * `m[r + i]` with `r` fixed across the loop lowers to
 *     %idx = r + i ; %sh = %idx << k ; %addr = m + %sh
 * and every recognizer reads a base indexed by the counter alone, so the sum
 * hid the access from all of them: the row-major inner loop, a windowed dot,
 * any offset slice. Rewriting it as
 *     row = m + (r << k)              (before the loop)
 *     %sh = i << k ; %addr = row + %sh
 * is the edit `--explain` has been prescribing ("bind the row to a pointer
 * before the loop"), made by the compiler. The addresses agree (both orders
 * are equal modulo 2^64), the loop saves an add per access, and the result is
 * the one address form every kernel reads.
 *
 * Soundness gates: the hoisted half and each pointer base must be
 * loop-invariant (never written in the loop, address never taken), the
 * counter half must not be rewritten between the index add and the shift, and
 * EVERY reader of the shifted index must be a `base + %sh` add this rewrite
 * also retargets. One consumer left behind would compute an address missing
 * the hoisted term. */
int ir_hoist_row_pointers_pass(IRFunction *function, int *changed) {
  static int g_row_counter;
  if (!function) {
    return 0;
  }
  for (size_t header = 0; header < function->instruction_count; header++) {
    size_t latch = 0;
    {
      const IRInstruction *label = &function->instructions[header];
      if (label->op != IR_OP_LABEL ||
          !ir_cleanup_label_is_loop_header(label->text)) {
        continue;
      }
      latch = ir_cleanup_loop_latch(function, header, label->text);
    }
    if (!latch) {
      continue;
    }

    for (size_t s = header + 1; s < latch; s++) {
      long long k = 0;
      size_t idx_pos = 0;
      size_t consumers[IR_ROW_MAX_CONSUMERS];
      size_t consumer_count = 0;
      IROperand inv = {0};
      IROperand var = {0};
      char sh_name[128];
      int bad = 0;

      {
        const IRInstruction *shl = &function->instructions[s];
        if (!(shl->op == IR_OP_BINARY && !shl->is_float && shl->text &&
              strcmp(shl->text, "<<") == 0 &&
              shl->rhs.kind == IR_OPERAND_INT && shl->rhs.int_value >= 1 &&
              shl->rhs.int_value <= 3 && shl->lhs.kind == IR_OPERAND_TEMP &&
              shl->lhs.name && shl->dest.kind == IR_OPERAND_TEMP &&
              shl->dest.name &&
              snprintf(sh_name, sizeof(sh_name), "%s", shl->dest.name) <
                  (int)sizeof(sh_name))) {
          continue;
        }
        k = shl->rhs.int_value;

        /* The shifted value must be `invariant + counter`, defined in-loop. */
        const IRInstruction *idx = NULL;
        for (size_t j = s; j-- > header + 1;) {
          const IRInstruction *cand = &function->instructions[j];
          if (ir_instruction_writes_destination(cand) &&
              cand->dest.kind == IR_OPERAND_TEMP && cand->dest.name &&
              strcmp(cand->dest.name, shl->lhs.name) == 0) {
            idx = cand;
            idx_pos = j;
            break;
          }
        }
        if (!idx || idx->op != IR_OP_BINARY || idx->is_float || !idx->text ||
            strcmp(idx->text, "+") != 0) {
          continue;
        }
        int lhs_inv =
            ir_row_operand_invariant(function, header + 1, latch, &idx->lhs);
        int rhs_inv =
            ir_row_operand_invariant(function, header + 1, latch, &idx->rhs);
        if (lhs_inv == rhs_inv) {
          continue; /* both fixed is plain LICM; both moving has no hoist. */
        }
        const IROperand *inv_side = lhs_inv ? &idx->lhs : &idx->rhs;
        const IROperand *var_side = lhs_inv ? &idx->rhs : &idx->lhs;
        if ((var_side->kind != IR_OPERAND_SYMBOL &&
             var_side->kind != IR_OPERAND_TEMP) ||
            !var_side->name) {
          continue;
        }
        if (inv_side->kind == IR_OPERAND_INT && inv_side->int_value == 0) {
          continue;
        }
        /* The moving half must still hold the index add's value at the
         * shift: nothing may redefine it in between. */
        int redefined = 0;
        for (size_t j = idx_pos + 1; j < s; j++) {
          const IRInstruction *mid = &function->instructions[j];
          if (ir_instruction_writes_destination(mid) &&
              mid->dest.kind == var_side->kind && mid->dest.name &&
              strcmp(mid->dest.name, var_side->name) == 0) {
            redefined = 1;
            break;
          }
        }
        if (redefined) {
          continue;
        }
        inv = ir_operand_copy(inv_side);
        var = ir_operand_copy(var_side);
      }

      /* Every reader of the shifted index, in or out of the loop, must be a
       * rewritable `base + %sh` add inside this loop; and nothing else may
       * define the same temp. */
      for (size_t j = 0; j < function->instruction_count && !bad; j++) {
        const IRInstruction *ins = &function->instructions[j];
        if (j != s && ir_instruction_writes_destination(ins) &&
            ins->dest.kind == IR_OPERAND_TEMP && ins->dest.name &&
            strcmp(ins->dest.name, sh_name) == 0) {
          bad = 1;
          break;
        }
        if (j == s || !ir_row_instruction_reads_temp(ins, sh_name)) {
          continue;
        }
        if (j <= s || j >= latch || consumer_count >= IR_ROW_MAX_CONSUMERS) {
          bad = 1;
          break;
        }
        if (!(ins->op == IR_OP_BINARY && !ins->is_float && ins->text &&
              strcmp(ins->text, "+") == 0 &&
              ins->lhs.kind == IR_OPERAND_SYMBOL && ins->lhs.name &&
              ir_operand_is_temp_named(&ins->rhs, sh_name) &&
              ins->dest.kind == IR_OPERAND_TEMP)) {
          bad = 1;
          break;
        }
        if (!ir_row_symbol_pointer_type(function, ins->lhs.name) ||
            ir_row_symbol_written(function, header + 1, latch, ins->lhs.name) ||
            ir_symbol_address_taken(function, ins->lhs.name)) {
          bad = 1;
          break;
        }
        consumers[consumer_count++] = j;
      }
      if (bad || consumer_count == 0) {
        ir_operand_destroy(&inv);
        ir_operand_destroy(&var);
        continue;
      }

      /* Capture each consumer's base and its pointer type before touching
       * anything, then rewrite in place (inserting reallocates the array). */
      char base_names[IR_ROW_MAX_CONSUMERS][128];
      char ptr_types[IR_ROW_MAX_CONSUMERS][64];
      char row_names[IR_ROW_MAX_CONSUMERS][48];
      char off_name[48];
      int need_off_temp = inv.kind != IR_OPERAND_INT;
      for (size_t c = 0; c < consumer_count && !bad; c++) {
        const IRInstruction *addr = &function->instructions[consumers[c]];
        const char *pt = ir_row_symbol_pointer_type(function, addr->lhs.name);
        if (!pt ||
            snprintf(base_names[c], sizeof(base_names[c]), "%s",
                     addr->lhs.name) >= (int)sizeof(base_names[c]) ||
            snprintf(ptr_types[c], sizeof(ptr_types[c]), "%s", pt) >=
                (int)sizeof(ptr_types[c])) {
          bad = 1;
        }
      }
      if (bad) {
        ir_operand_destroy(&inv);
        ir_operand_destroy(&var);
        continue;
      }

      snprintf(off_name, sizeof(off_name), "__rowoff_%d", g_row_counter);
      for (size_t c = 0; c < consumer_count; c++) {
        snprintf(row_names[c], sizeof(row_names[c]), "__rowp_%d_%zu",
                 g_row_counter, c);
        IRInstruction *addr = &function->instructions[consumers[c]];
        ir_operand_destroy(&addr->lhs);
        addr->lhs = ir_operand_symbol(row_names[c]);
      }
      g_row_counter++;
      {
        IRInstruction *shl = &function->instructions[s];
        ir_operand_destroy(&shl->lhs);
        shl->lhs = var;
        var = ir_operand_none();
      }

      /* The shift no longer reads the index add, and the row pointer carries
       * what it contributed. Retire it when nothing else reads it: left in
       * place it is dead code that still describes an offset, and a later
       * recognizer can charge that offset on top of the row pointer. */
      {
        IRInstruction *idx_ins = &function->instructions[idx_pos];
        if (idx_ins->dest.kind == IR_OPERAND_TEMP && idx_ins->dest.name) {
          int read_elsewhere = 0;
          for (size_t j = 0; j < function->instruction_count; j++) {
            if (j == idx_pos) {
              continue;
            }
            if (ir_row_instruction_reads_temp(&function->instructions[j],
                                              idx_ins->dest.name)) {
              read_elsewhere = 1;
              break;
            }
          }
          if (!read_elsewhere) {
            ir_instruction_make_nop(idx_ins);
          }
        }
      }

      /* Preheader, inserted at the header label: declarations, the scaled
       * offset when the hoisted half is a runtime symbol, then one add per
       * row. Build each instruction with owned storage; insertion copies. */
      size_t at = header;
      size_t inserted = 0;
      int failed = 0;
      for (size_t c = 0; c < consumer_count && !failed; c++) {
        IRInstruction decl = {0};
        decl.op = IR_OP_DECLARE_LOCAL;
        decl.dest = ir_operand_symbol(row_names[c]);
        decl.text = mettle_strdup(ptr_types[c]);
        if (!decl.dest.name || !decl.text ||
            !ir_function_insert_instruction(function, at, &decl)) {
          failed = 1;
        }
        ir_instruction_destroy_storage(&decl);
        if (!failed) {
          at++;
          inserted++;
        }
      }
      if (!failed && need_off_temp) {
        IRInstruction off = {0};
        off.op = IR_OP_BINARY;
        off.text = mettle_strdup("<<");
        off.dest = ir_operand_temp(off_name);
        off.lhs = ir_operand_copy(&inv);
        off.rhs = ir_operand_int(k);
        if (!off.text || !off.dest.name ||
            !ir_function_insert_instruction(function, at, &off)) {
          failed = 1;
        }
        ir_instruction_destroy_storage(&off);
        if (!failed) {
          at++;
          inserted++;
        }
      }
      for (size_t c = 0; c < consumer_count && !failed; c++) {
        IRInstruction add = {0};
        add.op = IR_OP_BINARY;
        add.text = mettle_strdup("+");
        add.dest = ir_operand_symbol(row_names[c]);
        add.lhs = ir_operand_symbol(base_names[c]);
        add.rhs = need_off_temp ? ir_operand_temp(off_name)
                                : ir_operand_int(inv.int_value << k);
        if (!add.text || !add.dest.name || !add.lhs.name ||
            !ir_function_insert_instruction(function, at, &add)) {
          failed = 1;
        }
        ir_instruction_destroy_storage(&add);
        if (!failed) {
          at++;
          inserted++;
        }
      }
      ir_operand_destroy(&inv);
      if (failed) {
        return 0;
      }
      header += inserted;
      s += inserted;
      latch += inserted;
      if (changed) {
        *changed = 1;
      }
    }
  }
  return 1;
}

/* `base + (iv << k)`, the address of `base[iv]`. Fills the base symbol and the
 * element width the shift implies. */
static int ir_scan_decode_indexed(const IRFunction *function, size_t before,
                                  const char *addr_temp, const char *iv,
                                  const char **base_out, long long *width_out) {
  const IRInstruction *addr =
      ir_find_temp_producer_before(function, before, addr_temp);
  const IRInstruction *shl = NULL;
  if (!addr || addr->op != IR_OP_BINARY || addr->is_float || !addr->text ||
      strcmp(addr->text, "+") != 0 || addr->lhs.kind != IR_OPERAND_SYMBOL ||
      !addr->lhs.name || addr->rhs.kind != IR_OPERAND_TEMP || !addr->rhs.name) {
    return 0;
  }
  shl = ir_find_temp_producer_before(function, before, addr->rhs.name);
  if (!shl || shl->op != IR_OP_BINARY || shl->is_float || !shl->text ||
      strcmp(shl->text, "<<") != 0 ||
      !ir_operand_is_symbol_named(&shl->lhs, iv) ||
      shl->rhs.kind != IR_OPERAND_INT || shl->rhs.int_value < 0 ||
      shl->rhs.int_value > 3) {
    return 0;
  }
  *base_out = addr->lhs.name;
  *width_out = 1LL << shl->rhs.int_value;
  return 1;
}

/* The element a body instruction assigns to `sym`, when it assigns exactly
 * `base[iv]` and nothing derived from it. */
static int ir_scan_assigns_element(const IRFunction *function, size_t at,
                                   const char *sym, const char *iv,
                                   const char **base_out, long long *width_out) {
  const IRInstruction *ins = &function->instructions[at];
  const IRInstruction *load = ins;
  if (!ir_operand_is_symbol_named(&ins->dest, sym)) {
    return 0;
  }
  if (ins->op == IR_OP_ASSIGN && ins->lhs.kind == IR_OPERAND_TEMP &&
      ins->lhs.name) {
    load = ir_find_temp_producer_before(function, at, ins->lhs.name);
  }
  if (!load || load->op != IR_OP_LOAD || load->lhs.kind != IR_OPERAND_TEMP ||
      !load->lhs.name || load->rhs.kind != IR_OPERAND_INT) {
    return 0;
  }
  {
    size_t li = (size_t)(load - function->instructions);
    const char *base = NULL;
    long long width = 0;
    if (!ir_scan_decode_indexed(function, li, load->lhs.name, iv, &base,
                                &width) ||
        width != load->rhs.int_value) {
      return 0;
    }
    *base_out = base;
    *width_out = width;
    return 1;
  }
}

/* Start a scan seeded from the first element at 0 rather than 1.
 *
 * `var m = a[0]; var i = 1; while (i < n) { if (a[i] > m) { m = a[i]; } }` is
 * how a maximum is usually written, and every kernel walks its arrays from
 * element 0 and reads the compare bound as a count, so the recognizers refuse
 * a counter that starts anywhere else. The awkward spelling, seeding from a
 * sentinel and counting from 0, vectorized; the ordinary one did not.
 *
 * Iteration 0 is a no-op here: the body's only effect is to assign `m` an
 * element of the same array at the same index, and at i == 0 that element is
 * the seed `m` already holds. So the counter can start at 0 and the loop runs
 * one extra iteration that cannot change anything. */
int ir_normalize_scan_from_first_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }
  for (size_t header = 0; header < function->instruction_count; header++) {
    const IRInstruction *label = &function->instructions[header];
    size_t latch = 0;
    size_t init_index = 0;
    const char *iv = NULL;
    const char *acc = NULL;
    const char *base = NULL;
    long long width = 0;
    int found_init = 0;
    int ok = 1;

    if (label->op != IR_OP_LABEL ||
        !ir_cleanup_label_is_loop_header(label->text)) {
      continue;
    }
    latch = ir_cleanup_loop_latch(function, header, label->text);
    if (!latch) {
      continue;
    }
    {
      size_t compare_index = 0;
      const IRInstruction *cmp = NULL;
      if (!ir_find_next_non_nop(function, header + 1, &compare_index) ||
          compare_index >= latch) {
        continue;
      }
      cmp = &function->instructions[compare_index];
      if (cmp->op != IR_OP_BINARY || cmp->is_float || !cmp->text ||
          strcmp(cmp->text, "<") != 0 || cmp->lhs.kind != IR_OPERAND_SYMBOL ||
          !cmp->lhs.name) {
        continue;
      }
      iv = cmp->lhs.name;
    }
    /* The counter's last setting before the loop must be the literal 1. */
    for (size_t k = 0; k < header; k++) {
      const IRInstruction *ins = &function->instructions[k];
      if (ir_instruction_writes_destination(ins) &&
          ir_operand_is_symbol_named(&ins->dest, iv)) {
        init_index = k;
        found_init = (ins->op == IR_OP_ASSIGN &&
                      ins->lhs.kind == IR_OPERAND_INT && ins->lhs.int_value == 1);
      }
    }
    if (!found_init) {
      continue;
    }
    /* Every symbol the body writes, other than the counter, is one accumulator
     * assigned exactly `base[iv]`. */
    for (size_t k = header + 1; k < latch && ok; k++) {
      const IRInstruction *ins = &function->instructions[k];
      const char *b = NULL;
      long long w = 0;
      if (ins->op == IR_OP_STORE || ins->op == IR_OP_CALL ||
          ins->op == IR_OP_CALL_INDIRECT || ins->op == IR_OP_INLINE_ASM ||
          ins->op == IR_OP_ADDRESS_OF || ins->op == IR_OP_NEW) {
        ok = 0;
        break;
      }
      if (!ir_instruction_writes_destination(ins) ||
          ins->dest.kind != IR_OPERAND_SYMBOL || !ins->dest.name ||
          strcmp(ins->dest.name, iv) == 0) {
        continue;
      }
      if (acc && strcmp(acc, ins->dest.name) != 0) {
        ok = 0;
        break;
      }
      if (!ir_scan_assigns_element(function, k, ins->dest.name, iv, &b, &w) ||
          (base && strcmp(base, b) != 0) || (width && w != width)) {
        ok = 0;
        break;
      }
      acc = ins->dest.name;
      base = b;
      width = w;
    }
    if (!ok || !acc || !base) {
      continue;
    }
    /* And its seed, before the loop, is that array's first element. */
    {
      const IRInstruction *seed = NULL;
      for (size_t k = 0; k < header; k++) {
        const IRInstruction *ins = &function->instructions[k];
        if (ir_instruction_writes_destination(ins) &&
            ir_operand_is_symbol_named(&ins->dest, acc)) {
          seed = ins;
        }
        /* The base must not move between the seed and the loop. */
        if (seed && ir_instruction_writes_destination(ins) &&
            ir_operand_is_symbol_named(&ins->dest, base)) {
          seed = NULL;
          break;
        }
      }
      if (!seed || seed->op != IR_OP_LOAD ||
          !ir_operand_is_symbol_named(&seed->lhs, base) ||
          seed->rhs.kind != IR_OPERAND_INT || seed->rhs.int_value != width) {
        continue;
      }
    }
    {
      IRInstruction *init = &function->instructions[init_index];
      ir_operand_destroy(&init->lhs);
      init->lhs = ir_operand_int(0);
      if (changed) {
        *changed = 1;
      }
    }
  }
  return 1;
}

/* True if `temp` is produced by a comparison, so it holds 0 or 1 rather than
 * the merely-nonzero that `branch_zero` would also accept. */
static int ir_accum_condition_is_boolean(const IRFunction *function, size_t at,
                                         const char *temp) {
  const IRInstruction *p = ir_find_temp_producer_before(function, at, temp);
  if (!p || p->op != IR_OP_BINARY || p->is_float || !p->text) {
    return 0;
  }
  return strcmp(p->text, "<") == 0 || strcmp(p->text, ">") == 0 ||
         strcmp(p->text, "<=") == 0 || strcmp(p->text, ">=") == 0 ||
         strcmp(p->text, "==") == 0 || strcmp(p->text, "!=") == 0;
}

/* `if (a[i] != 0)` reaches here as a bare `branch_zero` on the loaded value:
 * an earlier peephole folds `t = x != 0; branch_zero t` to `branch_zero x`,
 * which is right for scalar code and erases the comparison this pass needs.
 * The value is an int32 that may be any number, so `c = c + x` would count 5
 * for a 5. Putting the comparison back makes it a boolean again.
 *
 * Only an integer LOAD qualifies. A guard on a computed value (`if (x & 6)`)
 * is left alone: rewriting it would be correct, and it is the shape the
 * reader most likely meant to spell as a comparison, so a silent conversion
 * would hide a real question about their code behind a speed win.
 *
 * The slot the folded comparison vacated is still there as a NOP, so the
 * comparison goes back where it was and nothing moves. */
static int ir_accum_condition_is_nonzero_load(const IRFunction *function,
                                              size_t at, const char *temp) {
  const IRInstruction *p = ir_find_temp_producer_before(function, at, temp);
  return p && p->op == IR_OP_LOAD && !p->is_float;
}

/* The NOP nearest the branch, searching back to the loop header. Returns 0
 * when the region holds none. */
static size_t ir_accum_free_nop_slot(const IRFunction *function, size_t header,
                                     size_t before) {
  for (size_t i = before; i > header + 1; i--) {
    if (function->instructions[i - 1].op == IR_OP_NOP) {
      return i - 1;
    }
  }
  return 0;
}

/* Structural equality of two operands, and of the chains that compute two
 * temps. Used to prove one load reads exactly what another already read. */
static int ir_accum_operand_same(const IROperand *a, const IROperand *b) {
  if (a->kind != b->kind) {
    return 0;
  }
  switch (a->kind) {
  case IR_OPERAND_NONE:
    return 1;
  case IR_OPERAND_INT:
    return a->int_value == b->int_value;
  case IR_OPERAND_TEMP:
  case IR_OPERAND_SYMBOL:
    return a->name && b->name && strcmp(a->name, b->name) == 0;
  default:
    return 0;
  }
}

static int ir_accum_chain_same(const IRFunction *function, size_t at_a,
                               const IROperand *a, size_t at_b,
                               const IROperand *b, int depth) {
  const IRInstruction *pa = NULL;
  const IRInstruction *pb = NULL;
  if (depth > 4) {
    return 0;
  }
  if (a->kind != IR_OPERAND_TEMP || b->kind != IR_OPERAND_TEMP) {
    return ir_accum_operand_same(a, b);
  }
  if (!a->name || !b->name) {
    return 0;
  }
  pa = ir_find_temp_producer_before(function, at_a, a->name);
  pb = ir_find_temp_producer_before(function, at_b, b->name);
  if (!pa || !pb || pa->op != pb->op || pa->is_float != pb->is_float ||
      pa->is_unsigned != pb->is_unsigned) {
    return 0;
  }
  if (pa->op != IR_OP_BINARY && pa->op != IR_OP_ADDRESS_OF &&
      pa->op != IR_OP_CAST) {
    return 0; /* only pure address arithmetic is followed */
  }
  if ((pa->text == NULL) != (pb->text == NULL) ||
      (pa->text && strcmp(pa->text, pb->text) != 0)) {
    return 0;
  }
  {
    size_t ia = (size_t)(pa - function->instructions);
    size_t ib = (size_t)(pb - function->instructions);
    return ir_accum_chain_same(function, ia, &pa->lhs, ib, &pb->lhs,
                               depth + 1) &&
           ir_accum_chain_same(function, ia, &pa->rhs, ib, &pb->rhs, depth + 1);
  }
}

/* True if the LOAD at `at` reads exactly what some load in [lo, at) already
 * read. Nothing between them writes memory (the caller admits no stores or
 * calls), so hoisting it out of its guard can neither fault nor see a
 * different value: the earlier load already touched that address. */
static int ir_accum_load_is_redundant(const IRFunction *function, size_t lo,
                                      size_t at) {
  const IRInstruction *load = &function->instructions[at];
  if (load->lhs.kind != IR_OPERAND_TEMP || !load->lhs.name) {
    return 0;
  }
  for (size_t i = lo; i < at; i++) {
    const IRInstruction *prior = &function->instructions[i];
    if (prior->op != IR_OP_LOAD || prior->rhs.kind != IR_OPERAND_INT ||
        load->rhs.kind != IR_OPERAND_INT ||
        prior->rhs.int_value != load->rhs.int_value) {
      continue;
    }
    if (ir_accum_chain_same(function, i, &prior->lhs, at, &load->lhs, 0)) {
      return 1;
    }
  }
  return 0;
}

/* Whether any jump or branch still names `label`. The accumulate rewrite below
 * retires the guard it converted, and may only retire the labels that guard
 * reached if nothing else reaches them. */
static int ir_accum_label_is_reached(const IRFunction *function,
                                     const char *label) {
  if (!label) {
    return 1;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if ((ins->op == IR_OP_JUMP || ins->op == IR_OP_BRANCH_ZERO ||
         ins->op == IR_OP_BRANCH_EQ) &&
        ins->text && strcmp(ins->text, label) == 0) {
      return 1;
    }
  }
  return 0;
}

/* Turn a counted-under-a-condition accumulator into an unconditional one.
 *
 * `if (a[i] > t) { c = c + 1; }` is the ordinary way to count matches, and no
 * reduction kernel reads a body that branches. `--explain` has been telling
 * writers to make the accumulation unconditional by multiplying in the
 * comparison, which is exactly this rewrite, done by hand.
 *
 * The comparison already holds 0 or 1, so `c = c + X * cond` adds X on the
 * iterations the branch would have taken and 0 on the rest. Doing it here
 * rather than in a kernel means the int32 sum, the general reduction and the
 * float sums all gain predicated counting together.
 *
 * The rewrite lands in the slots the branch and its jump occupied, so nothing
 * moves and no instruction is inserted. */
int ir_if_convert_accumulate_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }
  for (size_t header = 0; header < function->instruction_count; header++) {
    const IRInstruction *label = &function->instructions[header];
    size_t latch = 0;

    if (label->op != IR_OP_LABEL ||
        !ir_cleanup_label_is_loop_header(label->text)) {
      continue;
    }
    latch = ir_cleanup_loop_latch(function, header, label->text);
    if (!latch) {
      continue;
    }

    for (size_t i = header + 1; i < latch; i++) {
      const IRInstruction *br = &function->instructions[i];
      size_t add_index = 0, jump = 0, else_label = 0, end_label = 0;
      size_t rematerialize_at = 0;
      const char *cond = NULL;
      const char *acc = NULL;

      if (br->op != IR_OP_BRANCH_ZERO || !br->text ||
          br->lhs.kind != IR_OPERAND_TEMP || !br->lhs.name) {
        continue;
      }
      cond = br->lhs.name;
      /* Set when the guard reached here as a folded `!= 0` and the comparison
         has to be put back before the addend can be multiplied by it. */
      rematerialize_at = 0;
      if (!ir_accum_condition_is_boolean(function, i, cond)) {
        if (!ir_accum_condition_is_nonzero_load(function, i, cond)) {
          continue;
        }
        rematerialize_at = ir_accum_free_nop_slot(function, header, i);
        if (!rematerialize_at) {
          continue;
        }
      }
      /* The arm writes one symbol, the accumulator. Everything else in it must
       * be arithmetic into temps, and any load must be one the condition
       * already performed, since the rewrite makes the arm unconditional. */
      add_index = 0;
      for (jump = i + 1; jump < latch; jump++) {
        const IRInstruction *ins = &function->instructions[jump];
        if (ins->op == IR_OP_JUMP) {
          break;
        }
        if (ins->op == IR_OP_NOP) {
          continue;
        }
        if (ins->op == IR_OP_LOAD) {
          if (!ir_accum_load_is_redundant(function, header + 1, jump)) {
            add_index = 0;
            break;
          }
          continue;
        }
        if (ins->op == IR_OP_BINARY || ins->op == IR_OP_CAST ||
            ins->op == IR_OP_UNARY) {
          if (ins->dest.kind == IR_OPERAND_TEMP) {
            continue;
          }
        } else {
          add_index = 0;
          break;
        }
        if (add_index) {
          add_index = 0;
          break;
        }
        add_index = jump;
      }
      if (!add_index || jump >= latch || !function->instructions[jump].text) {
        continue;
      }
      {
        const IRInstruction *add = &function->instructions[add_index];
        if (add->op != IR_OP_BINARY || add->is_float || !add->text ||
            strcmp(add->text, "+") != 0 ||
            add->dest.kind != IR_OPERAND_SYMBOL || !add->dest.name ||
            !ir_operand_is_symbol_named(&add->lhs, add->dest.name) ||
            (add->rhs.kind != IR_OPERAND_INT &&
             add->rhs.kind != IR_OPERAND_TEMP &&
             add->rhs.kind != IR_OPERAND_SYMBOL)) {
          continue;
        }
        acc = add->dest.name;
      }
      /* Both labels must follow, and the else arm must be empty: a value
       * chosen on the other side is a select, not an accumulate. */
      if (!ir_find_next_non_nop(function, jump + 1, &else_label) ||
          else_label >= latch ||
          !ir_find_next_non_nop(function, else_label + 1, &end_label) ||
          end_label >= latch) {
        continue;
      }
      {
        const IRInstruction *el = &function->instructions[else_label];
        const IRInstruction *en = &function->instructions[end_label];
        if (el->op != IR_OP_LABEL || !el->text ||
            strcmp(el->text, br->text) != 0 || en->op != IR_OP_LABEL ||
            !en->text ||
            strcmp(en->text, function->instructions[jump].text) != 0) {
          continue;
        }
      }
      /* Nothing else in the loop may write the accumulator, or the two writes
       * would race in a way one unconditional add cannot reproduce. */
      {
        int other = 0;
        for (size_t k = header + 1; k < latch; k++) {
          if (k != add_index &&
              ir_instruction_writes_destination(&function->instructions[k]) &&
              ir_operand_is_symbol_named(&function->instructions[k].dest, acc)) {
            other = 1;
            break;
          }
        }
        if (other) {
          continue;
        }
      }
      {
        IRInstruction *add = &function->instructions[add_index];
        int addend_is_one =
            add->rhs.kind == IR_OPERAND_INT && add->rhs.int_value == 1;
        char boolean[64];

        /* Put the folded `!= 0` back, in the slot it was folded out of, and
         * multiply by that instead of by the raw loaded value. */
        if (rematerialize_at) {
          IRInstruction cmp = {0};
          snprintf(boolean, sizeof(boolean), ".ifne%zu", rematerialize_at);
          cmp.op = IR_OP_BINARY;
          cmp.location = function->instructions[i].location;
          cmp.text = mettle_strdup("!=");
          cmp.dest = ir_operand_temp(boolean);
          cmp.lhs = ir_operand_temp(cond);
          cmp.rhs = ir_operand_int(0);
          if (!cmp.text || !cmp.dest.name || !cmp.lhs.name) {
            ir_instruction_destroy_storage(&cmp);
            return 0;
          }
          ir_instruction_destroy_storage(&function->instructions[rematerialize_at]);
          function->instructions[rematerialize_at] = cmp;
          cond = boolean;
        }

        /* `cond` points into the branch's own operand, and retiring an
         * instruction frees its operands. The branch is therefore NOPed only
         * after both rewrites below have copied the name out of it. */
        if (addend_is_one) {
          /* `c = c + 1` under the condition IS `c = c + cond`. */
          ir_operand_destroy(&add->rhs);
          add->rhs = ir_operand_temp(cond);
          if (!add->rhs.name) {
            return 0;
          }
          ir_instruction_make_nop(&function->instructions[jump]);
        } else {
          /* The addend may be computed inside the arm, so the multiply has to
           * follow it: it takes the accumulate's slot and the accumulate moves
           * down into the one the jump occupied. */
          char product[64];
          IRInstruction mul = {0};
          IRInstruction sum = {0};
          snprintf(product, sizeof(product), ".ifacc%zu", add_index);
          mul.op = IR_OP_BINARY;
          mul.location = add->location;
          mul.text = mettle_strdup("*");
          mul.dest = ir_operand_temp(product);
          mul.lhs = ir_operand_copy(&add->rhs);
          mul.rhs = ir_operand_temp(cond);
          sum.op = IR_OP_BINARY;
          sum.location = add->location;
          sum.text = mettle_strdup("+");
          sum.dest = ir_operand_copy(&add->dest);
          sum.lhs = ir_operand_copy(&add->lhs);
          sum.rhs = ir_operand_temp(product);
          if (!mul.text || !mul.dest.name || !mul.rhs.name || !sum.text ||
              !sum.dest.name || !sum.lhs.name || !sum.rhs.name) {
            ir_instruction_destroy_storage(&mul);
            ir_instruction_destroy_storage(&sum);
            return 0;
          }
          ir_instruction_destroy_storage(&function->instructions[add_index]);
          function->instructions[add_index] = mul;
          ir_instruction_destroy_storage(&function->instructions[jump]);
          function->instructions[jump] = sum;
        }
        ir_instruction_make_nop(&function->instructions[i]);
        /* Both labels can have a second predecessor. A short-circuit `&&`
         * lowers to two branches at the same else label, so retiring it on
         * behalf of the inner guard alone left the outer branch pointing at
         * nothing -- codegen refused the function, and the shapes that did not
         * refuse it counted the wrong thing. Retire each only once the guard
         * that just went away held its last reference; remove_unused_labels
         * collects whatever this leaves behind. */
        if (!ir_accum_label_is_reached(function,
                                       function->instructions[else_label].text)) {
          ir_instruction_make_nop(&function->instructions[else_label]);
        }
        if (!ir_accum_label_is_reached(function,
                                       function->instructions[end_label].text)) {
          ir_instruction_make_nop(&function->instructions[end_label]);
        }
        if (changed) {
          *changed = 1;
        }
        i = end_label;
      }
    }
  }
  return 1;
}

int ir_eliminate_load_symbol_copy_pass(IRFunction *function,
                                              int *changed) {
  if (!function) {
    return 0;
  }

  for (size_t i = 0; i + 1 < function->instruction_count; i++) {
    IRInstruction *load = &function->instructions[i];
    IRInstruction *assign = NULL;
    size_t assign_index = i + 1;
    const char *sym = NULL;
    const char *temp = NULL;
    size_t window_reads = 0;
    size_t total_reads = 0;
    size_t window_end = function->instruction_count;
    size_t j = 0;
    int unsafe_use = 0;

    if (load->op != IR_OP_LOAD || load->dest.kind != IR_OPERAND_TEMP ||
        !load->dest.name) {
      continue;
    }

    /* The assign usually follows the load directly, but the inliner's
     * parameter materialization interposes the parameter's DECLARE_LOCAL
     * (`%t <- *addr; local @p; @p <- %t`). Neither a NOP nor a declaration
     * reads or writes the temp or the symbol, so skip past them. */
    while (assign_index < function->instruction_count &&
           (function->instructions[assign_index].op == IR_OP_NOP ||
            function->instructions[assign_index].op == IR_OP_DECLARE_LOCAL)) {
      assign_index++;
    }
    if (assign_index >= function->instruction_count) {
      continue;
    }
    assign = &function->instructions[assign_index];

    if (assign->op != IR_OP_ASSIGN ||
        assign->dest.kind != IR_OPERAND_SYMBOL || !assign->dest.name ||
        assign->lhs.kind != IR_OPERAND_TEMP || !assign->lhs.name ||
        strcmp(assign->lhs.name, load->dest.name) != 0) {
      continue;
    }

    /* A float ASSIGN may carry an IEEE-754 width contract (e.g. the inliner
     * tags a float32 parameter copy with float_bits=32 so a float64-tracked
     * argument is narrowed). Replacing the symbol's uses with the raw load
     * temp drops that conversion, so only fold when the loaded scalar already
     * has the assign's exact width (4-byte load for float32, 8 for float64) --
     * then the conversion is an identity and the copy is safe to elide. */
    if (assign->is_float) {
      long long width_bytes = (assign->float_bits == 32) ? 4 : 8;
      if (load->rhs.kind != IR_OPERAND_INT ||
          load->rhs.int_value != width_bytes) {
        continue;
      }
    }

    sym = assign->dest.name;
    temp = load->dest.name;

    /* An address-taken symbol can be read through memory the operand scan
     * below cannot see. */
    if (ir_symbol_address_taken(function, sym)) {
      continue;
    }

    for (j = 0; j < function->instruction_count; j++) {
      total_reads += ir_load_copy_count_symbol_reads(&function->instructions[j],
                                                     sym);
    }

    /* Scan the straight-line window after the assign. It ends at the first
     * control-flow instruction or the first re-write of the symbol. */
    for (j = assign_index + 1; j < function->instruction_count; j++) {
      const IRInstruction *ins = &function->instructions[j];
      if (ins->op == IR_OP_LABEL || ins->op == IR_OP_JUMP ||
          ins->op == IR_OP_BRANCH_ZERO || ins->op == IR_OP_BRANCH_EQ) {
        window_end = j;
        break;
      }
      if (ir_instruction_writes_symbol(ins) &&
          ir_operand_is_symbol_named(&ins->dest, sym)) {
        /* A re-write that also reads the symbol (`@s = @s + 1`) would read a
         * stale value once the copy is gone. */
        if (ir_load_copy_count_symbol_reads(ins, sym) > 0) {
          unsafe_use = 1;
        }
        window_end = j;
        break;
      }
      if (ins->op == IR_OP_STORE &&
          ir_operand_is_symbol_named(&ins->dest, sym)) {
        /* Store-through-symbol addresses are left alone; folding around one
         * would leave a stale read. */
        unsafe_use = 1;
        break;
      }
      window_reads += ir_load_copy_count_symbol_reads(ins, sym);
    }

    if (unsafe_use || window_reads == 0 || window_reads > 6) {
      continue;
    }

    /* The symbol may be read outside the window: beyond the control-flow edge
     * that ended it, or earlier in a loop body (reading the previous
     * iteration's value). Either read would go stale once the copy is nop'd,
     * so only fold when the window accounts for every read in the function. */
    if (window_reads != total_reads) {
      continue;
    }

    for (j = assign_index + 1; j < window_end; j++) {
      IRInstruction *ins = &function->instructions[j];
      ir_load_copy_replace_operand(&ins->lhs, sym, temp);
      ir_load_copy_replace_operand(&ins->rhs, sym, temp);
      for (size_t a = 0; a < ins->argument_count; a++) {
        ir_load_copy_replace_operand(&ins->arguments[a], sym, temp);
      }
    }

    ir_instruction_make_nop(assign);
    if (changed) {
      *changed = 1;
    }
  }

  /* Folding a copy can leave its DECLARE_LOCAL dead (the inliner's parameter
   * local once every read is rewritten to the argument temp). A dead
   * declaration in a loop body still spoils the vectorizers' body-shape
   * matching and the --explain diagnosis, so sweep declarations whose symbol
   * no other instruction references. */
  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *decl = &function->instructions[i];
    int referenced = 0;

    if (decl->op != IR_OP_DECLARE_LOCAL ||
        decl->dest.kind != IR_OPERAND_SYMBOL || !decl->dest.name) {
      continue;
    }

    for (size_t j = 0; j < function->instruction_count && !referenced; j++) {
      const IRInstruction *ins = &function->instructions[j];
      if (j == i || ins->op == IR_OP_NOP) {
        continue;
      }
      if (ir_operand_is_symbol_named(&ins->dest, decl->dest.name) ||
          ir_operand_is_symbol_named(&ins->lhs, decl->dest.name) ||
          ir_operand_is_symbol_named(&ins->rhs, decl->dest.name)) {
        referenced = 1;
        break;
      }
      for (size_t a = 0; a < ins->argument_count; a++) {
        if (ir_operand_is_symbol_named(&ins->arguments[a], decl->dest.name)) {
          referenced = 1;
          break;
        }
      }
    }

    if (!referenced) {
      ir_instruction_make_nop(decl);
      if (changed) {
        *changed = 1;
      }
    }
  }

  return 1;
}
