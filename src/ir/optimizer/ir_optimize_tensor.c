#include "ir_optimize_internal.h"
#include "../../common.h"

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
  int handoff_state = 0;
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

static int tensor_control_transfer(const IRInstruction *instruction) {
  return instruction &&
         (instruction->op == IR_OP_JUMP ||
          instruction->op == IR_OP_BRANCH_ZERO ||
          instruction->op == IR_OP_BRANCH_EQ);
}

// Whether a region starting at `start` whose commit is inserted before
// `end` has the start as its only entry and the commit as its only exit.
// targets[j] is the label index control transfer j goes to (SIZE_MAX for
// other instructions). A transfer from inside to a run of labels beginning
// at `end` stays legal by retargeting it to a commit label (*needs_label).
static int tensor_region_control_valid(const IRFunction *function,
                                       const size_t *targets, size_t start,
                                       size_t end, int *needs_label) {
  *needs_label = 0;
  for (size_t j = 0; j < function->instruction_count; j++) {
    size_t target = targets[j];
    if (target == SIZE_MAX) continue;
    int inside = j > start && j < end;
    int target_inside = target > start && target < end;
    if (!inside) {
      if (target_inside) return 0;
      continue;
    }
    if (target_inside) continue;
    if (target < end) return 0;
    for (size_t k = end; k < target; k++) {
      IROpcode op = function->instructions[k].op;
      if (op != IR_OP_LABEL && op != IR_OP_NOP) return 0;
    }
    *needs_label = 1;
  }
  return 1;
}

static int tensor_scale_rows_joins(const IRProgram *program,
                                   const IRFunction *function,
                                   const IRInstruction *first,
                                   const IRInstruction *instruction) {
  return instruction->op == IR_OP_TENSOR_SCALE_ROWS &&
         instruction->tensor_residency_role == IR_TENSOR_RESIDENCY_NONE &&
         instruction->tensor_residency_scope == IR_TENSOR_RESIDENCY_SCOPE_NONE &&
         ir_tensor_scale_rows_joins(program, function, first, instruction);
}

static size_t tensor_region_scan_updates(const IRProgram *program,
                                         const IRFunction *function,
                                         const IRInstruction *first,
                                         size_t first_index,
                                         const IROperand *output,
                                         size_t *limit,
                                         size_t *update_count) {
  size_t last_update_index = SIZE_MAX;
  for (size_t i = first_index + 1; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->op == IR_OP_TENSOR_MMA &&
        instruction->tensor_residency_role == IR_TENSOR_RESIDENCY_NONE &&
        instruction->tensor_residency_scope ==
            IR_TENSOR_RESIDENCY_SCOPE_NONE &&
        tensor_instruction_can_join(first, instruction) &&
        ir_tensor_region_update_operands_clean(instruction, output)) {
      last_update_index = i;
      (*update_count)++;
      continue;
    }
    if (tensor_scale_rows_joins(program, function, first, instruction)) {
      last_update_index = i;
      continue;
    }
    if (!ir_tensor_region_instruction_allowed(program, function, instruction,
                                              output)) {
      *limit = i;
      break;
    }
  }
  return last_update_index;
}

static int tensor_region_transfer_targets(const IRFunction *function,
                                          size_t **out_targets) {
  size_t *targets = malloc(function->instruction_count * sizeof(*targets));
  if (!targets) return -1;
  for (size_t j = 0; j < function->instruction_count; j++) {
    const IRInstruction *instruction = &function->instructions[j];
    targets[j] = SIZE_MAX;
    if (!tensor_control_transfer(instruction)) continue;
    size_t target = SIZE_MAX;
    for (size_t k = 0; instruction->text && k < function->instruction_count;
         k++) {
      const IRInstruction *label = &function->instructions[k];
      if (label->op == IR_OP_LABEL && label->text &&
          strcmp(label->text, instruction->text) == 0) {
        target = k;
        break;
      }
    }
    if (target == SIZE_MAX) {
      free(targets);
      return 0;
    }
    targets[j] = target;
  }
  *out_targets = targets;
  return 1;
}

// The general form: one start, any number of connected updates anywhere
// in a single-entry, single-exit stretch of code -- loops, branches,
// barriers, asynchronous copies, and loads and stores that provably do not
// reach D (ir_tensor_region_instruction_allowed). The commit goes on the
// one exit.
static int tensor_try_form_region_residency(IRFunction *function,
                                            size_t first_index,
                                            uint32_t *next_group_id,
                                            int *changed) {
  if (!function || !next_group_id || !changed ||
      first_index >= function->instruction_count)
    return 0;
  const IRProgram *program = ir_optimize_get_program();
  IRInstruction *first = &function->instructions[first_index];
  if (!program || first->op != IR_OP_TENSOR_MMA ||
      ir_tensor_mma_instruction_count(first) != 1 ||
      first->tensor_residency_role != IR_TENSOR_RESIDENCY_NONE ||
      first->tensor_residency_scope != IR_TENSOR_RESIDENCY_SCOPE_NONE ||
      !tensor_desc_can_accumulate(&IR_TENSOR_MMA(first)) ||
      first->argument_count < 4)
    return 0;

  const IROperand *output = &first->arguments[3];
  size_t limit = function->instruction_count;
  size_t update_count = 0;
  size_t last_update_index = tensor_region_scan_updates(
      program, function, first, first_index, output, &limit, &update_count);
  if (last_update_index == SIZE_MAX) return 0;

  size_t *targets = NULL;
  int targets_ready = tensor_region_transfer_targets(function, &targets);
  if (targets_ready <= 0) return targets_ready;
  size_t end = SIZE_MAX;
  int needs_label = 0;
  for (size_t candidate = limit; candidate > last_update_index; candidate--) {
    if (tensor_region_control_valid(function, targets, first_index, candidate,
                                    &needs_label)) {
      end = candidate;
      break;
    }
  }
  if (end == SIZE_MAX) {
    free(targets);
    return 0;
  }

  uint32_t group_id = *next_group_id;
  char commit_label[96];
  do {
    if (group_id == 0 || group_id == UINT32_MAX) {
      free(targets);
      return 0;
    }
    snprintf(commit_label, sizeof(commit_label), "__mtlc_tensor_commit_%u",
             group_id);
    if (!tensor_label_exists(function, commit_label)) break;
    group_id++;
  } while (1);

  if (needs_label) {
    for (size_t j = first_index + 1; j < end; j++) {
      if (targets[j] == SIZE_MAX || targets[j] < end) continue;
      IRInstruction *transfer = &function->instructions[j];
      char *new_target = strdup(commit_label);
      if (!new_target) {
        free(targets);
        return -1;
      }
      mettle_free_string(transfer->text);
      transfer->text = new_target;
    }
  }
  free(targets);

  IRInstruction label = {0};
  label.op = IR_OP_LABEL;
  label.text = commit_label;
  label.location = first->location;
  IRInstruction commit = *first;
  commit.op = IR_OP_TENSOR_COMMIT;
  commit.tensor_residency_id = group_id;
  commit.tensor_residency_role = IR_TENSOR_RESIDENCY_COMMIT;
  commit.tensor_residency_scope = IR_TENSOR_RESIDENCY_SCOPE_REGION;
  commit.tensor_mma_count = 1;
  size_t commit_index = end;
  if (needs_label) {
    if (!ir_function_insert_instruction(function, end, &label)) return -1;
    commit_index = end + 1;
  }
  if (!ir_function_insert_instruction(function, commit_index, &commit))
    return -1;

  first = &function->instructions[first_index];
  output = &first->arguments[3];
  first->tensor_residency_id = group_id;
  first->tensor_residency_role = IR_TENSOR_RESIDENCY_START;
  first->tensor_residency_scope = IR_TENSOR_RESIDENCY_SCOPE_REGION;
  for (size_t i = first_index + 1; i < end; i++) {
    IRInstruction *update = &function->instructions[i];
    // Only the MMAs that join this accumulator: another tensor op in the
    // region (its own tile, disjoint from D) stays outside the group.
    int joins_as_mma =
        update->op == IR_OP_TENSOR_MMA &&
        update->tensor_residency_role == IR_TENSOR_RESIDENCY_NONE &&
        tensor_instruction_can_join(first, update) &&
        ir_tensor_region_update_operands_clean(update, output);
    if (!joins_as_mma &&
        !tensor_scale_rows_joins(program, function, first, update))
      continue;
    update->tensor_residency_id = group_id;
    update->tensor_residency_role = IR_TENSOR_RESIDENCY_UPDATE;
    update->tensor_residency_scope = IR_TENSOR_RESIDENCY_SCOPE_REGION;
  }
  *next_group_id = group_id + 1;
  *changed = 1;

  if (ir_explain_enabled() &&
      ir_explain_location_enabled(&first->location)) {
    char reason[192];
    snprintf(reason, sizeof(reason),
             "%llu connected updates in a single-entry, single-exit region; no instruction in it names or can reach D",
             (unsigned long long)update_count);
    ir_explain_remark(function->name, "tensor MMA region", first->location,
                      1, "formed a region-resident tensor accumulator",
                      reason, NULL, NULL);
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
        first->tensor_residency_role != IR_TENSOR_RESIDENCY_NONE ||
        IR_TENSOR_MMA(first).c_scale_mode != MTLC_TENSOR_SCALE_NONE ||
        !tensor_desc_can_accumulate(&IR_TENSOR_MMA(first)))
      continue;
    size_t end = i + 1;
    while (end < function->instruction_count &&
           function->instructions[end].tensor_residency_role ==
               IR_TENSOR_RESIDENCY_NONE &&
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
  for (size_t i = 0; i < function->instruction_count; i++) {
    int result = tensor_try_form_region_residency(function, i, &next_group_id,
                                                  changed);
    if (result < 0) return 0;
  }
  return 1;
}
