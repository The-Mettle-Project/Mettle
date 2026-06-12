#include "ir_optimize_internal.h"

/* -------------------------------------------------------------------------- */
/* Invariant fill -> IR_OP_SIMD_FILL (the memset / frame-clear class)          */
/*                                                                            */
/* Recognizes counted loops whose entire body is one store of a loop-         */
/* invariant value, in the forms such loops actually have by vectorizer time: */
/*                                                                            */
/*   A (indexed, `while (i < n) { buf[i] = c; i++ }`, including rect fills    */
/*      `while (xx < x1) { px[row + xx] = c; xx++ }` with xx from x0):        */
/*        [%m = yy * fw]              (invariant offset producer, optional)   */
/*        [%a = %m + @xx]             (offset + iv, optional)                 */
/*        %t1 = (@i | %a) << shift    (absent for byte stores)                */
/*        %t2 = @base + %t1                                                   */
/*        *%t2 <- value [size]                                                */
/*        @i = @i + 1                                                         */
/*                                                                            */
/*   B (pointer walk, what range-for fills become after pointer induction):   */
/*        *@p <- value [size]; @p = @p + size; [@i = @i + 1 dead counter]     */
/*      with header compare `@p < @pend`.                                     */
/*                                                                            */
/*   C (byte-offset walk, the stdlib mem_zero/mem_fill shape):                */
/*        %t = @base + @i; *%t <- value [size]; @i = @i + size                */
/*      with int64 locals and `@i < bound`.                                   */
/*                                                                            */
/* The fused kernel stores 16 bytes per iteration with a scalar element tail. */
/* Fills are bit-pattern operations: float and integer stores of every width  */
/* (1/2/4/8) use the same kernel, and constant float values are recorded as   */
/* their raw bit patterns.                                                    */

/* A loop-invariant fill value: a constant, a parameter, or a local that the
 * region never writes and whose address is never taken (an escaped local
 * could alias the destination buffer). Constants are normalized to raw-bit
 * INT operands so the backend splat is type-blind. */
static int ir_fill_value_operand(const IRFunction *function, size_t begin,
                                 size_t end, const IROperand *value,
                                 long long size, IROperand *out) {
  if (value->kind == IR_OPERAND_INT) {
    *out = ir_operand_int(value->int_value);
    return 1;
  }
  if (value->kind == IR_OPERAND_FLOAT) {
    if (size == 4) {
      float f = (float)value->float_value;
      unsigned int bits;
      memcpy(&bits, &f, sizeof(bits));
      *out = ir_operand_int((long long)bits);
      return 1;
    }
    if (size == 8) {
      double d = value->float_value;
      unsigned long long bits;
      memcpy(&bits, &d, sizeof(bits));
      *out = ir_operand_int((long long)bits);
      return 1;
    }
    return 0;
  }
  if (value->kind != IR_OPERAND_SYMBOL || !value->name) {
    return 0;
  }
  /* Param, local, or global -- in every case with no escaped address (an
   * aliased value symbol could be overwritten by the fill's own stores). */
  if (ir_symbol_address_taken(function, value->name)) {
    return 0;
  }
  for (size_t i = begin; i < end; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ir_instruction_writes_destination(ins) &&
        ins->dest.kind == IR_OPERAND_SYMBOL && ins->dest.name &&
        strcmp(ins->dest.name, value->name) == 0) {
      return 0;
    }
  }
  *out = ir_operand_symbol(value->name);
  return out->name != NULL;
}

/* A usable base/bound/offset symbol: parameter, declared local, or a global
 * whose address never escapes -- in every case not written inside the loop.
 * (Real applications keep hot buffers in global pointers; see
 * ir_symbol_is_float_array_base for the same policy.) */
static int ir_fill_symbol_is_invariant(const IRFunction *function,
                                       size_t begin, size_t end,
                                       const char *name) {
  if (!name) {
    return 0;
  }
  if (!ir_function_symbol_is_parameter(function, name) &&
      !ir_function_local_declared_type(function, name) &&
      ir_symbol_address_taken(function, name)) {
    return 0; /* a global with an escaped address could alias anything */
  }
  for (size_t i = begin; i < end; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ir_instruction_writes_destination(ins) &&
        ins->dest.kind == IR_OPERAND_SYMBOL && ins->dest.name &&
        strcmp(ins->dest.name, name) == 0) {
      return 0;
    }
  }
  return 1;
}

static int ir_fill_local_has_type(const IRFunction *function, const char *name,
                                  const char *type) {
  const char *declared = ir_function_local_declared_type(function, name);
  return declared && strcmp(declared, type) == 0;
}

/* Install the fused op (and, when the index offset is a body-computed temp,
 * a clone of its producer just before it -- that clone becomes the temp's
 * single definition once the body is nop'd). */
static int ir_fill_install(IRFunction *function, size_t header_index,
                           size_t jump_index, int mode, long long size,
                           const IROperand *lhs_sym, const IROperand *rhs_op,
                           const IROperand *value, const IROperand *start_op,
                           const IROperand *offset_op,
                           const IRInstruction *offset_producer,
                           const char *final_assign_to,
                           const char *final_assign_from, int *changed) {
  IRInstruction fused = {0};
  fused.op = IR_OP_SIMD_FILL;
  fused.location = function->instructions[header_index].location;
  if (!ir_operand_clone(lhs_sym, &fused.lhs) ||
      !ir_operand_clone(rhs_op, &fused.rhs)) {
    ir_instruction_destroy_storage(&fused);
    return 0;
  }
  fused.arguments = calloc(5, sizeof(IROperand));
  if (!fused.arguments) {
    ir_instruction_destroy_storage(&fused);
    return 0;
  }
  fused.argument_count = 5;
  fused.arguments[0] = ir_operand_int(size);
  fused.arguments[1] = ir_operand_int(mode);
  if (!ir_operand_clone(value, &fused.arguments[2])) {
    ir_instruction_destroy_storage(&fused);
    return 0;
  }
  if (start_op) {
    if (!ir_operand_clone(start_op, &fused.arguments[3])) {
      ir_instruction_destroy_storage(&fused);
      return 0;
    }
  } else {
    fused.arguments[3] = ir_operand_int(0);
  }
  if (offset_op) {
    if (!ir_operand_clone(offset_op, &fused.arguments[4])) {
      ir_instruction_destroy_storage(&fused);
      return 0;
    }
  } else {
    fused.arguments[4] = ir_operand_int(0);
  }

  IRInstruction producer_clone = {0};
  if (offset_producer) {
    if (!ir_clone_instruction_plain(offset_producer, &producer_clone)) {
      ir_instruction_destroy_storage(&fused);
      return 0;
    }
  }

  ir_instruction_destroy_storage(&function->instructions[header_index]);
  memset(&function->instructions[header_index], 0,
         sizeof(function->instructions[header_index]));
  for (size_t i = header_index + 1; i <= jump_index; i++) {
    ir_instruction_make_nop(&function->instructions[i]);
  }
  if (offset_producer) {
    function->instructions[header_index] = producer_clone;
    function->instructions[header_index + 1] = fused;
  } else {
    function->instructions[header_index] = fused;
  }

  /* Pointer-walk mode: the walking pointer ends at `pend` in the scalar
   * loop; preserve that for any later reads (dead-store elimination removes
   * it when nothing looks). */
  if (final_assign_to && final_assign_from &&
      header_index + 2 <= jump_index) {
    IRInstruction *assign =
        &function->instructions[header_index + (offset_producer ? 2 : 1)];
    assign->op = IR_OP_ASSIGN;
    assign->dest = ir_operand_symbol(final_assign_to);
    assign->lhs = ir_operand_symbol(final_assign_from);
    assign->location = function->instructions[header_index].location;
  }

  if (changed) {
    *changed = 1;
  }
  return 1;
}

/* Shared loop frame: header label, `<` compare, branch_zero to exit, back
 * jump, no nested loop. Returns 1 with *matched on a clean frame. */
static int ir_fill_frame(IRFunction *function, size_t header_index,
                         size_t *compare_out, size_t *branch_out,
                         size_t *jump_out, int *matched) {
  *matched = 0;
  if (!function || header_index + 4 >= function->instruction_count) {
    return 1;
  }
  IRInstruction *header = &function->instructions[header_index];
  if (header->op != IR_OP_LABEL || !ir_label_is_while_header(header->text)) {
    return 1;
  }
  size_t compare_index = 0, branch_index = 0;
  if (!ir_find_next_non_nop(function, header_index + 1, &compare_index) ||
      !ir_find_next_non_nop(function, compare_index + 1, &branch_index)) {
    return 1;
  }
  IRInstruction *compare = &function->instructions[compare_index];
  IRInstruction *branch = &function->instructions[branch_index];
  if (compare->op != IR_OP_BINARY || compare->is_float || !compare->text ||
      strcmp(compare->text, "<") != 0 ||
      compare->dest.kind != IR_OPERAND_TEMP || !compare->dest.name ||
      compare->lhs.kind != IR_OPERAND_SYMBOL || !compare->lhs.name ||
      branch->op != IR_OP_BRANCH_ZERO ||
      !ir_operand_is_temp_named(&branch->lhs, compare->dest.name) ||
      !branch->text) {
    return 1;
  }
  size_t jump_index = (size_t)-1;
  for (size_t i = branch_index + 1; i < function->instruction_count; i++) {
    if (function->instructions[i].op == IR_OP_JUMP &&
        function->instructions[i].text &&
        strcmp(function->instructions[i].text, header->text) == 0) {
      jump_index = i;
      break;
    }
    if (function->instructions[i].op == IR_OP_LABEL &&
        function->instructions[i].text &&
        strcmp(function->instructions[i].text, branch->text) == 0) {
      break;
    }
  }
  if (jump_index == (size_t)-1 ||
      ir_loop_body_has_nested_while(function, branch_index + 1, jump_index)) {
    return 1;
  }
  *compare_out = compare_index;
  *branch_out = branch_index;
  *jump_out = jump_index;
  *matched = 1;
  return 1;
}

/* Form results: 1 = installed, 0 = hard failure (abort the pass),
 * -1 = no match (try the next form). */
#define FILL_NO_MATCH (-1)

static int ir_fill_try_pointer_walk(IRFunction *function, size_t header_index,
                                    size_t branch_index, size_t jump_index,
                                    const IRInstruction *compare,
                                    const IRInstruction *const *body,
                                    size_t body_count, int *changed) {
  if (compare->rhs.kind != IR_OPERAND_SYMBOL || !compare->rhs.name) {
    return FILL_NO_MATCH;
  }
  const char *p = compare->lhs.name;
  const char *pend = compare->rhs.name;
  const IRInstruction *store = NULL;
  const IRInstruction *advance = NULL;
  const char *dead_counter = NULL;
  for (size_t k = 0; k < body_count; k++) {
    const IRInstruction *ins = body[k];
    if (ins->op == IR_OP_STORE && !store &&
        ir_operand_is_symbol_named(&ins->dest, p) &&
        ins->rhs.kind == IR_OPERAND_INT) {
      store = ins;
    } else if (ins->op == IR_OP_BINARY && ins->text &&
               strcmp(ins->text, "+") == 0 && !ins->is_float &&
               ins->dest.kind == IR_OPERAND_SYMBOL && ins->dest.name &&
               strcmp(ins->dest.name, p) == 0 &&
               ir_operand_is_symbol_named(&ins->lhs, p) &&
               ins->rhs.kind == IR_OPERAND_INT && !advance) {
      advance = ins;
    } else if (ins->op == IR_OP_BINARY && ins->text &&
               strcmp(ins->text, "+") == 0 && !ins->is_float &&
               ins->dest.kind == IR_OPERAND_SYMBOL && ins->dest.name &&
               strcmp(ins->dest.name, p) != 0 && !dead_counter &&
               ir_operand_is_symbol_named(&ins->lhs, ins->dest.name) &&
               ins->rhs.kind == IR_OPERAND_INT) {
      dead_counter = ins->dest.name;
    } else {
      return FILL_NO_MATCH;
    }
  }
  if (!store || !advance) {
    return FILL_NO_MATCH;
  }
  long long size = store->rhs.int_value;
  if ((size != 1 && size != 2 && size != 4 && size != 8) ||
      advance->rhs.int_value != size ||
      !ir_fill_symbol_is_invariant(function, branch_index + 1, jump_index,
                                   pend) ||
      (dead_counter &&
       ir_symbol_live_after_loop(function, jump_index + 1, dead_counter))) {
    return FILL_NO_MATCH;
  }
  IROperand value = {0};
  if (!ir_fill_value_operand(function, branch_index + 1, jump_index,
                             &store->lhs, size, &value)) {
    return FILL_NO_MATCH;
  }
  int p_live_after = ir_symbol_live_after_loop(function, jump_index + 1, p);
  int ok = ir_fill_install(function, header_index, jump_index, /*mode=*/1,
                            size, &compare->lhs, &compare->rhs, &value, NULL,
                            NULL, NULL, p_live_after ? p : NULL,
                            p_live_after ? pend : NULL, changed);
  ir_operand_destroy(&value);
  return ok;
}

static int ir_fill_try_indexed(IRFunction *function, size_t header_index,
                               size_t branch_index, size_t jump_index,
                               const IRInstruction *compare,
                               const IRInstruction *const *body,
                               size_t body_count, int *changed) {
  const char *iv = compare->lhs.name;
  if (compare->rhs.kind != IR_OPERAND_SYMBOL &&
      compare->rhs.kind != IR_OPERAND_INT) {
    return FILL_NO_MATCH;
  }
  if (compare->rhs.kind == IR_OPERAND_SYMBOL &&
      !ir_fill_symbol_is_invariant(function, branch_index + 1, jump_index,
                                   compare->rhs.name)) {
    return FILL_NO_MATCH;
  }
  int iv_from_zero = ir_iv_zero_at_header(function, header_index, iv);

  const IRInstruction *offset_producer_seen = NULL;
  const IRInstruction *idx_add = NULL; /* %a = offset + iv (rect fills) */
  const IRInstruction *shl = NULL;
  const IRInstruction *addr = NULL;
  const IRInstruction *store = NULL;
  const IRInstruction *increment = NULL;
  for (size_t k = 0; k < body_count; k++) {
    const IRInstruction *ins = body[k];
    if (ins->op == IR_OP_BINARY && ins->text && strcmp(ins->text, "+") == 0 &&
        !ins->is_float && !idx_add && !shl && !addr &&
        ins->dest.kind == IR_OPERAND_TEMP && ins->dest.name &&
        (ir_operand_is_symbol_named(&ins->lhs, iv) ||
         ir_operand_is_symbol_named(&ins->rhs, iv))) {
      idx_add = ins;
    } else if (ins->op == IR_OP_BINARY && ins->text &&
               strcmp(ins->text, "<<") == 0 && !ins->is_float && !shl &&
               (ir_operand_is_symbol_named(&ins->lhs, iv) ||
                (idx_add && ir_operand_is_temp_named(&ins->lhs,
                                                     idx_add->dest.name))) &&
               ins->rhs.kind == IR_OPERAND_INT &&
               ins->dest.kind == IR_OPERAND_TEMP) {
      shl = ins;
    } else if (ins->op == IR_OP_BINARY && ins->text &&
               strcmp(ins->text, "+") == 0 && !ins->is_float && !addr &&
               ins->dest.kind == IR_OPERAND_TEMP && ins->dest.name &&
               ins->lhs.kind == IR_OPERAND_SYMBOL && ins->lhs.name &&
               ((shl && ir_operand_is_temp_named(&ins->rhs, shl->dest.name)) ||
                (!shl && ir_operand_is_symbol_named(&ins->rhs, iv)) ||
                (!shl && idx_add &&
                 ir_operand_is_temp_named(&ins->rhs, idx_add->dest.name)))) {
      addr = ins;
    } else if (ins->op == IR_OP_STORE && !store && addr &&
               ir_operand_is_temp_named(&ins->dest, addr->dest.name) &&
               ins->rhs.kind == IR_OPERAND_INT) {
      store = ins;
    } else if (ins->op == IR_OP_BINARY && ins->text &&
               strcmp(ins->text, "+") == 0 && !ins->is_float && !increment &&
               ins->dest.kind == IR_OPERAND_SYMBOL && ins->dest.name &&
               strcmp(ins->dest.name, iv) == 0 &&
               ir_operand_is_symbol_named(&ins->lhs, iv) &&
               ins->rhs.kind == IR_OPERAND_INT && ins->rhs.int_value == 1) {
      increment = ins;
    } else if (ins->op == IR_OP_BINARY && !ins->is_float && k == 0 &&
               ins->dest.kind == IR_OPERAND_TEMP && ins->dest.name) {
      /* A leading temp computation (e.g. `yy * fw`): tolerated as the
       * potential invariant-offset producer, validated below if the index
       * add actually consumes it. */
      offset_producer_seen = ins;
    } else {
      return FILL_NO_MATCH;
    }
  }
  if (!store || !addr || !increment) {
    return FILL_NO_MATCH;
  }

  /* Validate the offset half of `offset + iv` when present. */
  const IROperand *offset_op = NULL;
  const IRInstruction *offset_producer = NULL;
  if (idx_add) {
    const IROperand *other = ir_operand_is_symbol_named(&idx_add->lhs, iv)
                                 ? &idx_add->rhs
                                 : &idx_add->lhs;
    if (other->kind == IR_OPERAND_INT) {
      offset_op = other;
    } else if (other->kind == IR_OPERAND_SYMBOL) {
      if (strcmp(other->name, iv) == 0 ||
          !ir_fill_symbol_is_invariant(function, branch_index + 1, jump_index,
                                       other->name)) {
        return FILL_NO_MATCH;
      }
      offset_op = other;
    } else if (other->kind == IR_OPERAND_TEMP && other->name &&
               offset_producer_seen &&
               ir_operand_is_temp_named(other, offset_producer_seen->dest.name)) {
      /* Offset computed INSIDE the body (`yy * fw` each iteration): its
       * producer is validated and cloned ahead of the fused op. */
      const IRInstruction *prod = offset_producer_seen;
      const IROperand *sides[2] = {&prod->lhs, &prod->rhs};
      for (int s = 0; s < 2; s++) {
        if (sides[s]->kind == IR_OPERAND_SYMBOL) {
          if (strcmp(sides[s]->name, iv) == 0 ||
              !ir_fill_symbol_is_invariant(function, branch_index + 1,
                                           jump_index, sides[s]->name)) {
            return FILL_NO_MATCH;
          }
        } else if (sides[s]->kind != IR_OPERAND_INT) {
          return FILL_NO_MATCH;
        }
      }
      offset_op = other;
      offset_producer = prod;
    } else if (other->kind == IR_OPERAND_TEMP && other->name) {
      /* Offset temp defined BEFORE the loop (CSE hoisted `h*HD` shared by
       * several uses): single-definition temps are invariant by
       * construction, and the temp is a valid operand at the op's position.
       * Just confirm the definition really is above the header. */
      int defined_before = 0;
      for (size_t i = 0; i < header_index; i++) {
        const IRInstruction *ins = &function->instructions[i];
        if (ir_instruction_writes_destination(ins) &&
            ins->dest.kind == IR_OPERAND_TEMP && ins->dest.name &&
            strcmp(ins->dest.name, other->name) == 0) {
          defined_before = 1;
          break;
        }
      }
      if (!defined_before) {
        return FILL_NO_MATCH;
      }
      offset_op = other;
    } else {
      return FILL_NO_MATCH;
    }
  } else if (offset_producer_seen) {
    return FILL_NO_MATCH; /* a leading temp nothing consumed: not a fill */
  }

  /* Nonzero start or an offset term need `bound - start` / `offset + start`
   * arithmetic in the kernel at the iv's own width: int32 ivs use 32-bit
   * math (their 8-byte homes may carry garbage upper bits), int64 ivs use
   * 64-bit. Anything else stays scalar. */
  long long index_width = 0;
  if (!iv_from_zero || idx_add) {
    if (ir_fill_local_has_type(function, iv, "int32")) {
      index_width = 32;
    } else if (ir_fill_local_has_type(function, iv, "int64")) {
      index_width = 64;
    } else {
      return FILL_NO_MATCH;
    }
  }

  long long size = store->rhs.int_value;
  if (size != 1 && size != 2 && size != 4 && size != 8) {
    return FILL_NO_MATCH;
  }
  if (shl) {
    if ((1LL << shl->rhs.int_value) != size) {
      return FILL_NO_MATCH;
    }
  } else if (size != 1) {
    return FILL_NO_MATCH; /* no scale shift: only byte stores index directly */
  }
  if (!ir_fill_symbol_is_invariant(function, branch_index + 1, jump_index,
                                   addr->lhs.name)) {
    return FILL_NO_MATCH;
  }
  /* A live-after iv is fine: the kernel writes back the exact final value,
   * max(start, bound) for this unit-stride frame -- robust against any
   * control flow after the loop (textual liveness scans are not). The
   * write-back needs the iv's width for exact arithmetic. */
  int iv_live_after = ir_symbol_live_after_loop(function, jump_index + 1, iv);
  if (iv_live_after && index_width == 0) {
    if (ir_fill_local_has_type(function, iv, "int32")) {
      index_width = 32;
    } else if (ir_fill_local_has_type(function, iv, "int64")) {
      index_width = 64;
    } else {
      return FILL_NO_MATCH;
    }
  }
  IROperand value = {0};
  if (!ir_fill_value_operand(function, branch_index + 1, jump_index,
                             &store->lhs, size, &value)) {
    return FILL_NO_MATCH;
  }
  IROperand start = {0};
  const IROperand *start_op = NULL;
  if (!iv_from_zero) {
    start = ir_operand_symbol(iv); /* reads the iv's ENTRY value at the op */
    if (!start.name) {
      ir_operand_destroy(&value);
      return 0;
    }
    start_op = &start;
  }
  int ok = ir_fill_install(function, header_index, jump_index, /*mode=*/0,
                           size, &addr->lhs, &compare->rhs, &value, start_op,
                           offset_op, offset_producer, NULL, NULL, changed);
  if (ok && iv_live_after) {
    IRInstruction *fused =
        &function->instructions[header_index + (offset_producer ? 1 : 0)];
    fused->dest = ir_operand_symbol(iv);
  }
  if (ok && index_width == 64) {
    /* Flag the 64-bit index path for the kernel (args[5]). */
    IRInstruction *fused =
        &function->instructions[header_index + (offset_producer ? 1 : 0)];
    IROperand *grown = realloc(fused->arguments, 6 * sizeof(IROperand));
    if (grown) {
      fused->arguments = grown;
      fused->arguments[5] = ir_operand_int(64);
      fused->argument_count = 6;
    }
  }
  ir_operand_destroy(&value);
  ir_operand_destroy(&start);
  return ok;
}

/* Resolve an operand through at most one non-float CAST in the body's cast
 * list: `(int64)8` becomes the INT 8, `(int64)@v` becomes the symbol @v, a
 * pointer cast of an address temp becomes that temp. Integer casts are
 * bit-preserving for a fill (the store writes the low `size` bytes either
 * way); float conversions are not, and are rejected. */
static const IROperand *ir_fill_uncast(const IRInstruction *const *casts,
                                       size_t cast_count,
                                       const IROperand *operand) {
  if (operand->kind != IR_OPERAND_TEMP || !operand->name) {
    return operand;
  }
  for (size_t c = 0; c < cast_count; c++) {
    if (ir_operand_is_temp_named(&casts[c]->dest, operand->name)) {
      return casts[c]->is_float ? NULL : &casts[c]->lhs;
    }
  }
  return operand;
}

static int ir_fill_try_byte_walk(IRFunction *function, size_t header_index,
                                 size_t branch_index, size_t jump_index,
                                 const IRInstruction *compare,
                                 const IRInstruction *const *body,
                                 size_t body_count, int *changed) {
  const char *iv = compare->lhs.name;
  if (compare->rhs.kind != IR_OPERAND_SYMBOL &&
      compare->rhs.kind != IR_OPERAND_INT) {
    return FILL_NO_MATCH;
  }
  if (compare->rhs.kind == IR_OPERAND_SYMBOL &&
      !ir_fill_symbol_is_invariant(function, branch_index + 1, jump_index,
                                   compare->rhs.name)) {
    return FILL_NO_MATCH;
  }
  /* 64-bit byte math throughout: the iv must be a declared int64 so its
   * 8-byte home holds a clean value. */
  if (!ir_fill_local_has_type(function, iv, "int64")) {
    return FILL_NO_MATCH;
  }

  /* This form (the stdlib mem_zero/mem_fill style) routes the value, the
   * pointer, and the stride through explicit no-op casts; collect them and
   * look through. */
  const IRInstruction *casts[4];
  size_t cast_count = 0;
  const IRInstruction *addr = NULL;
  const IRInstruction *store = NULL;
  const IRInstruction *advance = NULL;
  for (size_t k = 0; k < body_count; k++) {
    const IRInstruction *ins = body[k];
    if (ins->op == IR_OP_CAST && ins->dest.kind == IR_OPERAND_TEMP &&
        ins->dest.name && cast_count < 4) {
      casts[cast_count++] = ins;
    } else if (ins->op == IR_OP_BINARY && ins->text &&
               strcmp(ins->text, "+") == 0 && !ins->is_float && !addr &&
               ins->dest.kind == IR_OPERAND_TEMP && ins->dest.name &&
               ins->lhs.kind == IR_OPERAND_SYMBOL && ins->lhs.name &&
               ir_operand_is_symbol_named(&ins->rhs, iv)) {
      addr = ins;
    } else if (ins->op == IR_OP_STORE && !store && addr &&
               ins->rhs.kind == IR_OPERAND_INT) {
      const IROperand *dest = ir_fill_uncast(casts, cast_count, &ins->dest);
      if (!dest || !ir_operand_is_temp_named(dest, addr->dest.name)) {
        return FILL_NO_MATCH;
      }
      store = ins;
    } else if (ins->op == IR_OP_BINARY && ins->text &&
               strcmp(ins->text, "+") == 0 && !ins->is_float && !advance &&
               ins->dest.kind == IR_OPERAND_SYMBOL && ins->dest.name &&
               strcmp(ins->dest.name, iv) == 0 &&
               ir_operand_is_symbol_named(&ins->lhs, iv)) {
      advance = ins;
    } else {
      return FILL_NO_MATCH;
    }
  }
  if (!store || !addr || !advance) {
    return FILL_NO_MATCH;
  }
  const IROperand *stride = ir_fill_uncast(casts, cast_count, &advance->rhs);
  if (!stride || stride->kind != IR_OPERAND_INT) {
    return FILL_NO_MATCH;
  }
  long long size = store->rhs.int_value;
  if ((size != 1 && size != 2 && size != 4 && size != 8) ||
      stride->int_value != size ||
      !ir_fill_symbol_is_invariant(function, branch_index + 1, jump_index,
                                   addr->lhs.name)) {
    return FILL_NO_MATCH;
  }
  const IROperand *raw_value = ir_fill_uncast(casts, cast_count, &store->lhs);
  if (!raw_value) {
    return FILL_NO_MATCH;
  }
  IROperand value = {0};
  if (!ir_fill_value_operand(function, branch_index + 1, jump_index,
                             raw_value, size, &value)) {
    return FILL_NO_MATCH;
  }
  IROperand start = ir_operand_symbol(iv);
  if (!start.name) {
    ir_operand_destroy(&value);
    return 0;
  }
  /* The iv often feeds a follow-up loop (mem_zero's word loop hands its
   * offset to the byte tail). The kernel computes the exact final offset --
   * start + ceil(len/size)*size, overshoot included -- and writes it back
   * when anything reads the iv afterwards. */
  int iv_live_after = ir_symbol_live_after_loop(function, jump_index + 1, iv);
  int ok = ir_fill_install(function, header_index, jump_index, /*mode=*/2,
                           size, &addr->lhs, &compare->rhs, &value, &start,
                           NULL, NULL, NULL, NULL, changed);
  if (ok && iv_live_after) {
    /* install placed the fused op at header_index. */
    IRInstruction *fused = &function->instructions[header_index];
    fused->dest = ir_operand_symbol(iv);
  }
  ir_operand_destroy(&value);
  ir_operand_destroy(&start);
  return ok;
}

static int ir_try_vectorize_fill_at(IRFunction *function, size_t header_index,
                                    int *changed) {
  size_t compare_index = 0, branch_index = 0, jump_index = 0;
  int matched = 0;
  if (!ir_fill_frame(function, header_index, &compare_index, &branch_index,
                     &jump_index, &matched)) {
    return 0;
  }
  if (!matched) {
    return 1;
  }
  const IRInstruction *compare = &function->instructions[compare_index];

  /* Collect the body's non-nop instructions (excluding the back jump). */
  const IRInstruction *body[6];
  size_t body_count = 0;
  for (size_t i = branch_index + 1; i < jump_index; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_NOP) {
      continue;
    }
    if (body_count >= 6) {
      return 1; /* too big to be a pure fill */
    }
    body[body_count++] = ins;
  }
  if (body_count < 2) {
    return 1;
  }

  int r = ir_fill_try_pointer_walk(function, header_index, branch_index,
                                   jump_index, compare, body, body_count,
                                   changed);
  if (r != FILL_NO_MATCH) {
    return r;
  }
  r = ir_fill_try_indexed(function, header_index, branch_index, jump_index,
                          compare, body, body_count, changed);
  if (r != FILL_NO_MATCH) {
    return r;
  }
  r = ir_fill_try_byte_walk(function, header_index, branch_index, jump_index,
                            compare, body, body_count, changed);
  if (r != FILL_NO_MATCH) {
    return r;
  }
  return 1;
}

int ir_simd_fill_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (function->instructions[i].op == IR_OP_LABEL &&
        ir_label_is_while_header(function->instructions[i].text)) {
      if (!ir_try_vectorize_fill_at(function, i, changed)) {
        return 0;
      }
    }
  }
  return 1;
}
