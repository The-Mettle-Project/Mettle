#include "ir_optimize_internal.h"
#include "../../common.h" // mettle_free_string

/* A chain is the exact sequential composition
 *
 *   D = A0*B0 + C
 *   D = A1*B1 + D
 *   ...
 *
 * represented in one neutral instruction. It does not promise a backend
 * fragment ABI: a backend may replay the component operations. Backends with
 * cooperative-register tiles can instead load C once, keep D resident, and
 * store once. */

static size_t tensor_stride_operand_index(const MtlcTensorMmaDesc *desc,
                                          unsigned requested) {
  size_t index = 4u +
                 (desc->sparsity != MTLC_TENSOR_SPARSITY_DENSE ? 1u : 0u) +
                 (desc->a_scale_mode != MTLC_TENSOR_SCALE_NONE ? 1u : 0u) +
                 (desc->b_scale_mode != MTLC_TENSOR_SCALE_NONE ? 1u : 0u);
  unsigned mask = ir_tensor_mma_runtime_stride_mask(desc);
  for (unsigned bit = MTLC_TENSOR_RUNTIME_STRIDE_A;
       bit <= MTLC_TENSOR_RUNTIME_STRIDE_D; bit <<= 1) {
    if (!(mask & bit)) continue;
    if (bit == requested) return index;
    index++;
  }
  return SIZE_MAX;
}

static int tensor_desc_can_accumulate(const MtlcTensorMmaDesc *desc) {
  return desc && desc->accumulator_element == desc->result_element &&
         desc->c_layout == desc->d_layout &&
         ((desc->c_leading_dimension == 0) ==
          (desc->d_leading_dimension == 0)) &&
         (desc->c_leading_dimension == 0 ||
          desc->c_leading_dimension == desc->d_leading_dimension);
}

static int tensor_runtime_accumulator_stride_matches(
    const IRInstruction *first, const IRInstruction *candidate) {
  const MtlcTensorMmaDesc *desc = &IR_TENSOR_MMA(first);
  size_t c = tensor_stride_operand_index(desc, MTLC_TENSOR_RUNTIME_STRIDE_C);
  size_t d = tensor_stride_operand_index(desc, MTLC_TENSOR_RUNTIME_STRIDE_D);
  if (c == SIZE_MAX) return 1;
  return d != SIZE_MAX &&
         ir_operand_equals(&candidate->arguments[c], &first->arguments[d]) &&
         ir_operand_equals(&candidate->arguments[d], &first->arguments[d]);
}

static int tensor_instruction_can_join(const IRInstruction *first,
                                       const IRInstruction *candidate) {
  if (!first || !candidate || first->op != IR_OP_TENSOR_MMA ||
      candidate->op != IR_OP_TENSOR_MMA ||
      ir_tensor_mma_instruction_count(first) != 1 ||
      ir_tensor_mma_instruction_count(candidate) != 1 ||
      !ir_tensor_mma_desc_equal(&IR_TENSOR_MMA(first),
                                &IR_TENSOR_MMA(candidate)) ||
      !tensor_desc_can_accumulate(&IR_TENSOR_MMA(first)))
    return 0;
  size_t per_tile = ir_tensor_mma_operand_count(&IR_TENSOR_MMA(first));
  if (!per_tile || first->argument_count != per_tile ||
      candidate->argument_count != per_tile)
    return 0;
  const IROperand *output = &first->arguments[3];
  return ir_operand_equals(&candidate->arguments[2], output) &&
         ir_operand_equals(&candidate->arguments[3], output) &&
         tensor_runtime_accumulator_stride_matches(first, candidate);
}

static int tensor_instruction_mentions_operand(const IRInstruction *instruction,
                                               const IROperand *operand) {
  if (!instruction || !operand) return 0;
  if (ir_operand_equals(&instruction->dest, operand) ||
      ir_operand_equals(&instruction->lhs, operand) ||
      ir_operand_equals(&instruction->rhs, operand))
    return 1;
  for (size_t i = 0; i < instruction->argument_count; i++) {
    if (ir_operand_equals(&instruction->arguments[i], operand)) return 1;
  }
  return 0;
}

static int tensor_loop_scalar_instruction(const IRInstruction *instruction) {
  if (!instruction) return 0;
  switch (instruction->op) {
  case IR_OP_NOP:
  case IR_OP_DECLARE_LOCAL:
  case IR_OP_ASSIGN:
  case IR_OP_BINARY:
  case IR_OP_UNARY:
  case IR_OP_CAST:
  case IR_OP_SELECT:
    return 1;
  default:
    return 0;
  }
}

static int tensor_pipeline_barrier(const IRInstruction *instruction) {
  return instruction && instruction->op == IR_OP_BARRIER &&
         instruction->memory_scope == MTLC_MEMORY_SCOPE_WORKGROUP &&
         (instruction->memory_regions & MTLC_MEMORY_REGION_WORKGROUP) != 0 &&
         (instruction->memory_order == MTLC_MEMORY_ORDER_ACQ_REL ||
          instruction->memory_order == MTLC_MEMORY_ORDER_SEQ_CST);
}

/* Retain one accumulator across completion/publication of one or more staged
 * tiles. Copy groups are commonly issued before START, so each legal region
 * between connected MMAs is pure scalar work plus neutral async-copy
 * bookkeeping and a wait-then-workgroup-barrier handoff:
 *
 *   MMA(stage[0], C -> D)
 *   async_copy.wait(...)
 *   barrier(workgroup, acq_rel)
 *   MMA(stage[1], D -> D)
 *
 *   async_copy.wait(...)
 *   barrier(workgroup, acq_rel)
 *   MMA(stage[2], D -> D)
 *   ...
 *
 * The verifier independently rechecks every handoff after every pass. */
static int tensor_try_form_pipeline_residency(IRFunction *function,
                                              size_t first_index,
                                              uint32_t *next_group_id,
                                              int *changed) {
  if (!function || !next_group_id || !changed ||
      first_index >= function->instruction_count)
    return 0;
  IRInstruction *first = &function->instructions[first_index];
  if (first->op != IR_OP_TENSOR_MMA ||
      ir_tensor_mma_instruction_count(first) != 1 ||
      first->tensor_residency_role != IR_TENSOR_RESIDENCY_NONE ||
      first->tensor_residency_scope != IR_TENSOR_RESIDENCY_SCOPE_NONE ||
      !tensor_desc_can_accumulate(&IR_TENSOR_MMA(first)) ||
      first->argument_count < 4)
    return 0;

  const IROperand *output = &first->arguments[3];
  size_t last_update_index = SIZE_MAX;
  size_t update_count = 0;
  int handoff_state = 0; /* 0 issuing, 1 waited, 2 published */
  for (size_t i = first_index + 1; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->op == IR_OP_TENSOR_MMA) {
      if (handoff_state != 2 ||
          instruction->tensor_residency_role != IR_TENSOR_RESIDENCY_NONE ||
          instruction->tensor_residency_scope !=
              IR_TENSOR_RESIDENCY_SCOPE_NONE ||
          !tensor_instruction_can_join(first, instruction))
        break;
      last_update_index = i;
      update_count++;
      handoff_state = 0;
      continue;
    }
    if (tensor_instruction_mentions_operand(instruction, output)) break;
    if (tensor_loop_scalar_instruction(instruction)) continue;
    if (handoff_state == 0 &&
        (instruction->op == IR_OP_ASYNC_COPY ||
         instruction->op == IR_OP_ASYNC_COMMIT))
      continue;
    if (handoff_state == 0 && instruction->op == IR_OP_ASYNC_WAIT) {
      handoff_state = 1;
      continue;
    }
    if (handoff_state == 1 && tensor_pipeline_barrier(instruction)) {
      handoff_state = 2;
      continue;
    }
    break;
  }
  if (last_update_index == SIZE_MAX || *next_group_id == 0 ||
      *next_group_id == UINT32_MAX)
    return 0;

  uint32_t group_id = *next_group_id;
  IRInstruction commit = *first;
  commit.op = IR_OP_TENSOR_COMMIT;
  commit.tensor_mma_count = 1;
  commit.tensor_residency_id = group_id;
  commit.tensor_residency_role = IR_TENSOR_RESIDENCY_COMMIT;
  commit.tensor_residency_scope = IR_TENSOR_RESIDENCY_SCOPE_PIPELINE;
  if (!ir_function_insert_instruction(function, last_update_index + 1,
                                      &commit))
    return -1;

  first = &function->instructions[first_index];
  first->tensor_residency_id = group_id;
  first->tensor_residency_role = IR_TENSOR_RESIDENCY_START;
  first->tensor_residency_scope = IR_TENSOR_RESIDENCY_SCOPE_PIPELINE;
  for (size_t i = first_index + 1; i <= last_update_index; i++) {
    IRInstruction *update = &function->instructions[i];
    if (update->op != IR_OP_TENSOR_MMA) continue;
    update->tensor_residency_id = group_id;
    update->tensor_residency_role = IR_TENSOR_RESIDENCY_UPDATE;
    update->tensor_residency_scope = IR_TENSOR_RESIDENCY_SCOPE_PIPELINE;
  }
  *next_group_id = group_id + 1;
  *changed = 1;

  if (ir_explain_enabled() &&
      ir_explain_location_enabled(&first->location)) {
    char reason[192];
    snprintf(reason, sizeof(reason),
             "%llu connected updates are completed and published through ordered async handoffs while D remains unobservable",
             (unsigned long long)update_count);
    ir_explain_remark(
        function->name, "tensor MMA pipeline", first->location, 1,
        "formed an asynchronously staged tensor accumulator pipeline",
        reason, NULL, NULL);
  }
  return 1;
}

static int tensor_label_exists(const IRFunction *function, const char *label) {
  if (!function || !label) return 0;
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->op == IR_OP_LABEL && instruction->text &&
        strcmp(instruction->text, label) == 0)
      return 1;
  }
  return 0;
}

/* Recognize one exact loop-carried composition without assuming a particular
 * induction variable or trip-count spelling:
 *
 *   D = A0*B0 + C
 *   while (uniform condition) {
 *     <register-only address/counter arithmetic>
 *     D = Ai*Bi + D
 *     <register-only counter arithmetic>
 *   }
 *
 * The body must be a single linear block with exactly one MMA and no memory,
 * call, barrier, atomic, or other observable operation. We split only the
 * loop-exit edge into a commit block; outer guards that share the original end
 * label bypass the commit and therefore cannot observe an uninitialized
 * resident accumulator. */
static int tensor_try_form_loop_residency(IRFunction *function,
                                          size_t first_index,
                                          uint32_t *next_group_id,
                                          int *changed) {
  if (!function || !next_group_id || !changed ||
      first_index >= function->instruction_count)
    return 0;
  IRInstruction *first = &function->instructions[first_index];
  if (first->op != IR_OP_TENSOR_MMA ||
      ir_tensor_mma_instruction_count(first) != 1 ||
      first->tensor_residency_role != IR_TENSOR_RESIDENCY_NONE ||
      first->tensor_residency_scope != IR_TENSOR_RESIDENCY_SCOPE_NONE ||
      !tensor_desc_can_accumulate(&IR_TENSOR_MMA(first)) ||
      first->argument_count < 4)
    return 0;

  const IROperand *output = &first->arguments[3];
  size_t header_index = SIZE_MAX;
  for (size_t i = first_index + 1; i < function->instruction_count; i++) {
    IRInstruction *instruction = &function->instructions[i];
    if (instruction->op == IR_OP_LABEL) {
      header_index = i;
      break;
    }
    if (!tensor_loop_scalar_instruction(instruction) ||
        tensor_instruction_mentions_operand(instruction, output))
      return 0;
  }
  if (header_index == SIZE_MAX || !function->instructions[header_index].text)
    return 0;
  const char *header_label = function->instructions[header_index].text;

  size_t branch_index = SIZE_MAX;
  for (size_t i = header_index + 1; i < function->instruction_count; i++) {
    IRInstruction *instruction = &function->instructions[i];
    if (instruction->op == IR_OP_BRANCH_ZERO && instruction->text) {
      branch_index = i;
      break;
    }
    if (instruction->op == IR_OP_LABEL || instruction->op == IR_OP_JUMP ||
        !tensor_loop_scalar_instruction(instruction) ||
        tensor_instruction_mentions_operand(instruction, output))
      return 0;
  }
  if (branch_index == SIZE_MAX) return 0;
  const char *original_exit = function->instructions[branch_index].text;

  size_t update_index = SIZE_MAX;
  size_t jump_index = SIZE_MAX;
  for (size_t i = branch_index + 1; i < function->instruction_count; i++) {
    IRInstruction *instruction = &function->instructions[i];
    if (instruction->op == IR_OP_JUMP) {
      if (!instruction->text || strcmp(instruction->text, header_label) != 0)
        return 0;
      jump_index = i;
      break;
    }
    if (instruction->op == IR_OP_LABEL ||
        instruction->op == IR_OP_BRANCH_ZERO ||
        instruction->op == IR_OP_BRANCH_EQ)
      return 0;
    if (instruction->op == IR_OP_TENSOR_MMA) {
      if (update_index != SIZE_MAX ||
          !tensor_instruction_can_join(first, instruction))
        return 0;
      update_index = i;
      continue;
    }
    if (!tensor_loop_scalar_instruction(instruction) ||
        tensor_instruction_mentions_operand(instruction, output))
      return 0;
  }
  if (update_index == SIZE_MAX || jump_index == SIZE_MAX) return 0;

  /* No second entry to the loop header may bypass the residency start. */
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (i == jump_index) continue;
    const IRInstruction *instruction = &function->instructions[i];
    if ((instruction->op == IR_OP_JUMP ||
         instruction->op == IR_OP_BRANCH_ZERO ||
         instruction->op == IR_OP_BRANCH_EQ) &&
        instruction->text && strcmp(instruction->text, header_label) == 0)
      return 0;
  }

  size_t exit_label_index = SIZE_MAX;
  for (size_t i = jump_index + 1; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->op == IR_OP_LABEL) {
      if (instruction->text && strcmp(instruction->text, original_exit) == 0)
        exit_label_index = i;
      break;
    }
    if (instruction->op != IR_OP_NOP) return 0;
  }
  if (exit_label_index == SIZE_MAX) return 0;

  uint32_t group_id = *next_group_id;
  char commit_label[96];
  do {
    if (group_id == 0 || group_id == UINT32_MAX) return 0;
    snprintf(commit_label, sizeof(commit_label),
             "__mtlc_tensor_commit_%u", group_id);
    if (!tensor_label_exists(function, commit_label)) break;
    group_id++;
  } while (1);

  IRInstruction label = {0};
  label.op = IR_OP_LABEL;
  label.text = commit_label;
  label.location = first->location;
  IRInstruction commit = *first;
  commit.op = IR_OP_TENSOR_COMMIT;
  commit.tensor_residency_id = group_id;
  commit.tensor_residency_role = IR_TENSOR_RESIDENCY_COMMIT;
  commit.tensor_residency_scope = IR_TENSOR_RESIDENCY_SCOPE_LOOP;
  commit.tensor_mma_count = 1;
  IRInstruction jump = {0};
  jump.op = IR_OP_JUMP;
  jump.text = (char *)original_exit;
  jump.location = first->location;

  /* Insertion clones all borrowed strings/operands. It occurs after the source
   * instructions, so their indices remain stable even if the array grows. */
  if (!ir_function_insert_instruction(function, exit_label_index, &label) ||
      !ir_function_insert_instruction(function, exit_label_index + 1,
                                      &commit) ||
      !ir_function_insert_instruction(function, exit_label_index + 2, &jump))
    return -1;

  IRInstruction *branch = &function->instructions[branch_index];
  char *new_target = strdup(commit_label);
  if (!new_target) return -1;
  mettle_free_string(branch->text);
  branch->text = new_target;
  first = &function->instructions[first_index];
  IRInstruction *update = &function->instructions[update_index];
  first->tensor_residency_id = group_id;
  first->tensor_residency_role = IR_TENSOR_RESIDENCY_START;
  first->tensor_residency_scope = IR_TENSOR_RESIDENCY_SCOPE_LOOP;
  update->tensor_residency_id = group_id;
  update->tensor_residency_role = IR_TENSOR_RESIDENCY_UPDATE;
  update->tensor_residency_scope = IR_TENSOR_RESIDENCY_SCOPE_LOOP;
  *next_group_id = group_id + 1;
  *changed = 1;

  if (ir_explain_enabled() &&
      ir_explain_location_enabled(&first->location)) {
    ir_explain_remark(
        function->name, "tensor MMA loop", first->location, 1,
        "formed a loop-carried tensor accumulator residency region",
        "the loop has one connected MMA update and no intervening observable operation; only its exit edge commits D",
        NULL, NULL);
  }
  return 1;
}

static void tensor_destroy_cloned_arguments(IROperand *arguments,
                                            size_t count,
                                            MtlcType **argument_types) {
  if (arguments) {
    for (size_t i = 0; i < count; i++) ir_operand_destroy(&arguments[i]);
  }
  free(arguments);
  free(argument_types);
}

int ir_fuse_tensor_mma_chains_pass(IRFunction *function, int *changed) {
  if (!function || !changed) return 0;
  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *first = &function->instructions[i];
    if (first->op != IR_OP_TENSOR_MMA ||
        ir_tensor_mma_instruction_count(first) != 1 ||
        !tensor_desc_can_accumulate(&IR_TENSOR_MMA(first)))
      continue;
    size_t end = i + 1;
    while (end < function->instruction_count &&
           tensor_instruction_can_join(first, &function->instructions[end]))
      end++;
    size_t tile_count = end - i;
    if (tile_count < 2 || tile_count > UINT32_MAX) continue;
    size_t per_tile = first->argument_count;
    if (per_tile == 0 || tile_count > SIZE_MAX / per_tile) return 0;
    size_t total = per_tile * tile_count;
    IROperand *arguments = calloc(total, sizeof(*arguments));
    int have_argument_types = 0;
    for (size_t tile = i; tile < end; tile++) {
      if (function->instructions[tile].argument_types) {
        have_argument_types = 1;
        break;
      }
    }
    MtlcType **argument_types =
        have_argument_types ? calloc(total, sizeof(*argument_types)) : NULL;
    if (!arguments || (have_argument_types && !argument_types)) {
      tensor_destroy_cloned_arguments(arguments, 0, argument_types);
      return 0;
    }
    size_t cloned = 0;
    for (size_t tile = 0; tile < tile_count; tile++) {
      const IRInstruction *source = &function->instructions[i + tile];
      for (size_t arg = 0; arg < per_tile; arg++) {
        size_t destination = tile * per_tile + arg;
        if (!ir_operand_clone(&source->arguments[arg],
                              &arguments[destination])) {
          tensor_destroy_cloned_arguments(arguments, cloned, argument_types);
          return 0;
        }
        cloned++;
        if (argument_types && source->argument_types)
          argument_types[destination] = source->argument_types[arg];
      }
    }
    ir_instruction_clear_arguments(first);
    first->arguments = arguments;
    first->argument_types = argument_types;
    first->argument_count = total;
    first->tensor_mma_count = (uint32_t)tile_count;
    for (size_t tile = i + 1; tile < end; tile++)
      ir_instruction_make_nop(&function->instructions[tile]);
    *changed = 1;
    if (ir_explain_enabled() &&
        ir_explain_location_enabled(&first->location)) {
      char headline[128];
      snprintf(headline, sizeof(headline),
               "fused %llu tensor tiles into one accumulator-resident chain",
               (unsigned long long)tile_count);
      ir_explain_remark(function->name, "tensor MMA chain", first->location,
                        1, headline,
                        "each tile consumes the same output as its accumulator; no observable operation intervenes",
                        NULL, NULL);
    }
    i = end - 1;
  }

  uint32_t next_group_id = 1;
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (function->instructions[i].tensor_residency_id >= next_group_id) {
      if (function->instructions[i].tensor_residency_id == UINT32_MAX)
        return 0;
      next_group_id = function->instructions[i].tensor_residency_id + 1;
    }
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    int result = tensor_try_form_pipeline_residency(
        function, i, &next_group_id, changed);
    if (result < 0) return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    int result = tensor_try_form_loop_residency(function, i, &next_group_id,
                                                changed);
    if (result < 0) return 0;
  }
  return 1;
}
