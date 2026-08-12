#include "ir_optimize_internal.h"

/* Target-neutral automatic GPU staging.
 *
 * This pass deliberately recognizes semantics, not source spellings: an
 * ordinary typed global LOAD immediately consumed by a workgroup STORE, one
 * or more times, followed in the same straight-line region by an acquire+
 * release workgroup barrier. It proves address-space provenance, natural
 * transaction alignment, single use of every loaded value, and the absence of
 * intervening memory/control effects before replacing the pairs with a
 * balanced ASYNC_COPY/COMMIT ... WAIT/BARRIER region. Backends remain free to
 * implement the neutral operations natively or synchronously.
 */

typedef struct {
  MtlcAddressSpace space;
  int aligned;
} IRGpuAddressProof;

static const IRInstruction *gpu_find_definition(const IRFunction *function,
                                                size_t before,
                                                const IROperand *operand,
                                                size_t *out_index) {
  if (!function || !operand || !operand->name ||
      (operand->kind != IR_OPERAND_TEMP &&
       operand->kind != IR_OPERAND_SYMBOL))
    return NULL;
  if (before > function->instruction_count)
    before = function->instruction_count;
  for (size_t i = before; i > 0; i--) {
    const IRInstruction *instruction = &function->instructions[i - 1];
    if (instruction->dest.kind != operand->kind || !instruction->dest.name ||
        strcmp(instruction->dest.name, operand->name) != 0)
      continue;
    if (!ir_instruction_writes_destination(instruction) &&
        instruction->op != IR_OP_ADDRESS_SPACE_ALLOC &&
        instruction->op != IR_OP_DECLARE_LOCAL)
      continue;
    if (out_index) *out_index = i - 1;
    return instruction;
  }
  return NULL;
}

static unsigned long long gpu_abs_u64(long long value) {
  return value < 0 ? (unsigned long long)(-(value + 1)) + 1ull
                   : (unsigned long long)value;
}

static unsigned gpu_gcd_u32(unsigned a, unsigned b) {
  while (b != 0) {
    unsigned next = a % b;
    a = b;
    b = next;
  }
  return a;
}

static int gpu_offset_divisible(const IRFunction *function, size_t before,
                                const IROperand *operand, unsigned divisor,
                                unsigned depth) {
  if (divisor <= 1) return 1;
  if (!function || !operand || depth > 64) return 0;
  if (operand->kind == IR_OPERAND_INT)
    return gpu_abs_u64(operand->int_value) % divisor == 0;
  size_t producer_index = 0;
  const IRInstruction *producer =
      gpu_find_definition(function, before, operand, &producer_index);
  if (!producer) return 0;
  if (producer->op == IR_OP_ASSIGN || producer->op == IR_OP_CAST)
    return gpu_offset_divisible(function, producer_index, &producer->lhs,
                                divisor, depth + 1);
  if (producer->op == IR_OP_UNARY && producer->text &&
      strcmp(producer->text, "-") == 0)
    return gpu_offset_divisible(function, producer_index, &producer->lhs,
                                divisor, depth + 1);
  if (producer->op != IR_OP_BINARY || !producer->text) return 0;
  if (strcmp(producer->text, "+") == 0 ||
      strcmp(producer->text, "-") == 0) {
    return gpu_offset_divisible(function, producer_index, &producer->lhs,
                                divisor, depth + 1) &&
           gpu_offset_divisible(function, producer_index, &producer->rhs,
                                divisor, depth + 1);
  }
  if (strcmp(producer->text, "<<") == 0 &&
      producer->rhs.kind == IR_OPERAND_INT &&
      producer->rhs.int_value >= 0 && producer->rhs.int_value < 63) {
    unsigned long long factor = 1ull << producer->rhs.int_value;
    unsigned remaining = divisor / gpu_gcd_u32(divisor, (unsigned)factor);
    return gpu_offset_divisible(function, producer_index, &producer->lhs,
                                remaining, depth + 1);
  }
  if (strcmp(producer->text, "*") != 0) return 0;
  if (producer->lhs.kind == IR_OPERAND_INT ||
      producer->rhs.kind == IR_OPERAND_INT) {
    const IROperand *constant = producer->lhs.kind == IR_OPERAND_INT
                                    ? &producer->lhs
                                    : &producer->rhs;
    const IROperand *variable = producer->lhs.kind == IR_OPERAND_INT
                                    ? &producer->rhs
                                    : &producer->lhs;
    unsigned factor = (unsigned)(gpu_abs_u64(constant->int_value) % divisor);
    unsigned remaining = divisor / gpu_gcd_u32(divisor, factor);
    return gpu_offset_divisible(function, producer_index, variable, remaining,
                                depth + 1);
  }
  /* Transactions are powers of two. Let two non-constant factors contribute
   * complementary powers when the IR proves both. */
  for (unsigned left = 2; left <= divisor; left <<= 1) {
    unsigned right = divisor / left;
    if (gpu_offset_divisible(function, producer_index, &producer->lhs, left,
                             depth + 1) &&
        gpu_offset_divisible(function, producer_index, &producer->rhs, right,
                             depth + 1))
      return 1;
  }
  return 0;
}

static size_t gpu_type_alignment(const MtlcType *type) {
  if (!type) return 0;
  if (type->kind == MTLC_TYPE_POINTER && type->base_type)
    return type->base_type->alignment;
  return type->alignment;
}

static size_t gpu_parameter_alignment(const char *type_name) {
  if (!type_name || !strchr(type_name, '*')) return 0;
  const char *base = strrchr(type_name, ':');
  base = base ? base + 1 : type_name;
  static const struct {
    const char *name;
    size_t alignment;
  } types[] = {
      {"int8*", 1},     {"uint8*", 1},   {"bool*", 1},
      {"int16*", 2},    {"uint16*", 2},  {"int32*", 4},
      {"uint32*", 4},   {"float32*", 4}, {"int64*", 8},
      {"uint64*", 8},   {"float64*", 8},
  };
  for (size_t i = 0; i < IR_ARRAY_COUNT(types); i++)
    if (strcmp(base, types[i].name) == 0) return types[i].alignment;
  return 0;
}

static IRGpuAddressProof gpu_address_proof_impl(
    const IRFunction *function, size_t before, const IROperand *operand,
    unsigned transaction, unsigned depth) {
  IRGpuAddressProof none = {MTLC_ADDRESS_SPACE_DEFAULT, 0};
  if (!function || !operand || !operand->name || depth > 64) return none;

  size_t producer_index = 0;
  const IRInstruction *producer =
      gpu_find_definition(function, before, operand, &producer_index);
  if (!producer && operand->kind == IR_OPERAND_SYMBOL) {
    for (size_t p = 0; p < function->parameter_count; p++) {
      if (!function->parameter_names || !function->parameter_names[p] ||
          strcmp(function->parameter_names[p], operand->name) != 0)
        continue;
      const char *type = function->parameter_types
                             ? function->parameter_types[p]
                             : NULL;
      size_t alignment = gpu_parameter_alignment(type);
      if (!alignment) return none;
      MtlcAddressSpace space =
          type && strncmp(type, "workgroup:", 10) == 0
              ? MTLC_ADDRESS_SPACE_WORKGROUP
              : (type && strncmp(type, "constant:", 9) == 0
                     ? MTLC_ADDRESS_SPACE_CONSTANT
                     : MTLC_ADDRESS_SPACE_GLOBAL);
      IRGpuAddressProof proof = {space, alignment >= transaction};
      return proof;
    }
    return none;
  }
  if (!producer) return none;
  if (producer->op == IR_OP_ADDRESS_SPACE_ALLOC) {
    IRGpuAddressProof proof = {
        producer->address_space,
        gpu_type_alignment(producer->value_type) >= transaction};
    return proof;
  }
  if (producer->op == IR_OP_ASSIGN || producer->op == IR_OP_ADDRESS_OF)
    return gpu_address_proof_impl(function, producer_index, &producer->lhs,
                                  transaction, depth + 1);
  if (producer->op != IR_OP_BINARY || !producer->text ||
      (strcmp(producer->text, "+") != 0 &&
       strcmp(producer->text, "-") != 0))
    return none;

  IRGpuAddressProof lhs = gpu_address_proof_impl(
      function, producer_index, &producer->lhs, transaction, depth + 1);
  IRGpuAddressProof rhs = gpu_address_proof_impl(
      function, producer_index, &producer->rhs, transaction, depth + 1);
  if (lhs.space != MTLC_ADDRESS_SPACE_DEFAULT &&
      rhs.space == MTLC_ADDRESS_SPACE_DEFAULT && lhs.aligned &&
      gpu_offset_divisible(function, producer_index, &producer->rhs,
                           transaction, depth + 1))
    return lhs;
  if (strcmp(producer->text, "+") == 0 &&
      rhs.space != MTLC_ADDRESS_SPACE_DEFAULT &&
      lhs.space == MTLC_ADDRESS_SPACE_DEFAULT && rhs.aligned &&
      gpu_offset_divisible(function, producer_index, &producer->lhs,
                           transaction, depth + 1))
    return rhs;
  return none;
}

static int gpu_address_matches(const IRFunction *function, size_t before,
                               const IROperand *operand,
                               MtlcAddressSpace expected,
                               unsigned transaction) {
  IRGpuAddressProof proof = gpu_address_proof_impl(
      function, before, operand, transaction, 0);
  return proof.space == expected && proof.aligned;
}

static size_t gpu_operand_use_count(const IRFunction *function,
                                    const IROperand *operand) {
  if (!function || !operand || !operand->name) return 0;
  size_t uses = 0;
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->lhs.kind == operand->kind && instruction->lhs.name &&
        strcmp(instruction->lhs.name, operand->name) == 0)
      uses++;
    if (instruction->rhs.kind == operand->kind && instruction->rhs.name &&
        strcmp(instruction->rhs.name, operand->name) == 0)
      uses++;
    if (!ir_instruction_writes_destination(instruction) &&
        instruction->dest.kind == operand->kind && instruction->dest.name &&
        strcmp(instruction->dest.name, operand->name) == 0)
      uses++;
    for (size_t a = 0; a < instruction->argument_count; a++)
      if (instruction->arguments[a].kind == operand->kind &&
          instruction->arguments[a].name &&
          strcmp(instruction->arguments[a].name, operand->name) == 0)
        uses++;
  }
  return uses;
}

static const MtlcType *gpu_loaded_element_type(const IRInstruction *load,
                                               unsigned bytes) {
  if (!load || (bytes != 4 && bytes != 8)) return NULL;
  if (load->value_type && load->value_type->size == bytes &&
      load->value_type->kind >= MTLC_TYPE_INT8 &&
      load->value_type->kind <= MTLC_TYPE_FLOAT64 &&
      load->value_type->kind != MTLC_TYPE_STRING)
    return load->value_type;
  if (load->is_float)
    return mtlc_type_scalar(bytes == 4 ? MTLC_TYPE_FLOAT32
                                      : MTLC_TYPE_FLOAT64);
  if (load->is_unsigned)
    return mtlc_type_scalar(bytes == 4 ? MTLC_TYPE_UINT32
                                      : MTLC_TYPE_UINT64);
  return mtlc_type_scalar(bytes == 4 ? MTLC_TYPE_INT32 : MTLC_TYPE_INT64);
}

static int gpu_pair_is_promotable(const IRFunction *function,
                                  size_t load_index, size_t store_index) {
  const IRInstruction *load = &function->instructions[load_index];
  const IRInstruction *store = &function->instructions[store_index];
  if (load->op != IR_OP_LOAD || store->op != IR_OP_STORE ||
      load->dest.kind != IR_OPERAND_TEMP || !load->dest.name ||
      store->lhs.kind != IR_OPERAND_TEMP || !store->lhs.name ||
      strcmp(load->dest.name, store->lhs.name) != 0 ||
      load->rhs.kind != IR_OPERAND_INT ||
      store->rhs.kind != IR_OPERAND_INT ||
      load->rhs.int_value != store->rhs.int_value ||
      (load->rhs.int_value != 4 && load->rhs.int_value != 8) ||
      gpu_operand_use_count(function, &load->dest) != 1)
    return 0;
  unsigned transaction = (unsigned)load->rhs.int_value;
  return gpu_loaded_element_type(load, transaction) &&
         gpu_address_matches(function, load_index, &load->lhs,
                             MTLC_ADDRESS_SPACE_GLOBAL, transaction) &&
         gpu_address_matches(function, store_index, &store->dest,
                             MTLC_ADDRESS_SPACE_WORKGROUP, transaction);
}

static int gpu_overlap_instruction_is_pure(const IRInstruction *instruction) {
  if (!instruction) return 0;
  switch (instruction->op) {
  case IR_OP_NOP:
  case IR_OP_DECLARE_LOCAL:
  case IR_OP_ASSIGN:
  case IR_OP_ADDRESS_OF:
  case IR_OP_BINARY:
  case IR_OP_UNARY:
  case IR_OP_CAST:
  case IR_OP_SELECT:
    return 1;
  default:
    return 0;
  }
}

static int gpu_barrier_completes_staging(const IRInstruction *instruction) {
  return instruction && instruction->op == IR_OP_BARRIER &&
         instruction->memory_scope == MTLC_MEMORY_SCOPE_WORKGROUP &&
         (instruction->memory_regions & MTLC_MEMORY_REGION_WORKGROUP) != 0 &&
         (instruction->memory_order == MTLC_MEMORY_ORDER_ACQ_REL ||
          instruction->memory_order == MTLC_MEMORY_ORDER_SEQ_CST);
}

static int gpu_next_effect_boundary(const IRFunction *function, size_t start,
                                    size_t *out_index) {
  if (!function || !out_index) return 0;
  for (size_t i = start; i < function->instruction_count; i++) {
    if (gpu_overlap_instruction_is_pure(&function->instructions[i])) continue;
    *out_index = i;
    return 1;
  }
  return 0;
}

static int gpu_build_async_copy(const IRInstruction *load,
                                const IRInstruction *store,
                                IRInstruction *out) {
  unsigned transaction = (unsigned)load->rhs.int_value;
  const MtlcType *element = gpu_loaded_element_type(load, transaction);
  const MtlcType *destination_type =
      mtlc_type_pointer_in(element, MTLC_ADDRESS_SPACE_WORKGROUP);
  const MtlcType *source_type =
      mtlc_type_pointer_in(element, MTLC_ADDRESS_SPACE_GLOBAL);
  if (!element || !destination_type || !source_type) return 0;
  memset(out, 0, sizeof(*out));
  out->op = IR_OP_ASYNC_COPY;
  out->location = load->location;
  out->async_copy_element_count = 1;
  out->async_copy_transaction_bytes = transaction;
  out->async_copy_cache = MTLC_ASYNC_CACHE_ALL;
  out->async_copy_generated = 1;
  out->argument_count = 2;
  out->arguments = calloc(2, sizeof(*out->arguments));
  out->argument_types = calloc(2, sizeof(*out->argument_types));
  if (!out->arguments || !out->argument_types ||
      !ir_operand_clone(&store->dest, &out->arguments[0]) ||
      !ir_operand_clone(&load->lhs, &out->arguments[1])) {
    ir_instruction_destroy_storage(out);
    return 0;
  }
  out->argument_types[0] = (MtlcType *)destination_type;
  out->argument_types[1] = (MtlcType *)source_type;
  return 1;
}

static int gpu_reserve_instruction_slots(IRFunction *function, size_t extra) {
  if (!function || extra > SIZE_MAX - function->instruction_count) return 0;
  size_t required = function->instruction_count + extra;
  if (required <= function->instruction_capacity) return 1;
  size_t capacity = function->instruction_capacity
                        ? function->instruction_capacity
                        : 64;
  while (capacity < required) {
    if (capacity > SIZE_MAX / 2) {
      capacity = required;
      break;
    }
    capacity *= 2;
  }
  IRInstruction *grown =
      realloc(function->instructions, capacity * sizeof(*grown));
  if (!grown) return 0;
  function->instructions = grown;
  function->instruction_capacity = capacity;
  return 1;
}

int ir_promote_gpu_async_staging_pass(IRFunction *function, int *changed) {
  if (!function || !changed) return 0;
  if (!function->is_kernel) return 1;
  for (size_t i = 0; i < function->instruction_count; i++)
    if (function->instructions[i].op == IR_OP_ASYNC_COPY ||
        function->instructions[i].op == IR_OP_ASYNC_COMMIT ||
        function->instructions[i].op == IR_OP_ASYNC_WAIT)
      return 1; /* Do not splice into an explicit user-managed group state. */

  for (size_t i = 0; i < function->instruction_count;) {
    if (function->instructions[i].op != IR_OP_LOAD) {
      i++;
      continue;
    }
    IRIndexVector loads = {0}, stores = {0};
    size_t cursor = i, after_pairs = i;
    int candidate = 1;
    while (cursor < function->instruction_count &&
           function->instructions[cursor].op == IR_OP_LOAD) {
      size_t store_index = 0;
      if (!gpu_next_effect_boundary(function, cursor + 1, &store_index) ||
          !gpu_pair_is_promotable(function, cursor, store_index) ||
          !ir_index_vector_append(&loads, cursor) ||
          !ir_index_vector_append(&stores, store_index)) {
        candidate = 0;
        break;
      }
      after_pairs = store_index + 1;
      size_t next = 0;
      if (!gpu_next_effect_boundary(function, after_pairs, &next) ||
          function->instructions[next].op != IR_OP_LOAD)
        break;
      cursor = next;
    }

    size_t barrier_index = after_pairs;
    if (candidate && loads.count > 0) {
      for (; barrier_index < function->instruction_count; barrier_index++) {
        const IRInstruction *instruction =
            &function->instructions[barrier_index];
        if (gpu_barrier_completes_staging(instruction)) break;
        if (!gpu_overlap_instruction_is_pure(instruction)) {
          candidate = 0;
          break;
        }
      }
      if (barrier_index >= function->instruction_count) candidate = 0;
    }

    IRInstruction *copies = NULL;
    if (candidate && loads.count > 0) {
      copies = calloc(loads.count, sizeof(*copies));
      if (!copies) {
        ir_index_vector_destroy(&loads);
        ir_index_vector_destroy(&stores);
        return 0;
      }
      for (size_t p = 0; p < loads.count; p++) {
        if (!gpu_build_async_copy(&function->instructions[loads.items[p]],
                                  &function->instructions[stores.items[p]],
                                  &copies[p])) {
          for (size_t q = 0; q <= p; q++)
            ir_instruction_destroy_storage(&copies[q]);
          free(copies);
          ir_index_vector_destroy(&loads);
          ir_index_vector_destroy(&stores);
          return 0;
        }
      }
      if (!gpu_reserve_instruction_slots(function, 2)) {
        for (size_t p = 0; p < loads.count; p++)
          ir_instruction_destroy_storage(&copies[p]);
        free(copies);
        ir_index_vector_destroy(&loads);
        ir_index_vector_destroy(&stores);
        return 0;
      }

      IRInstruction wait = {0};
      wait.op = IR_OP_ASYNC_WAIT;
      wait.location = function->instructions[barrier_index].location;
      wait.async_copy_pending_groups = 0;
      if (!ir_instruction_insert_move(function, barrier_index, &wait)) {
        for (size_t p = 0; p < loads.count; p++)
          ir_instruction_destroy_storage(&copies[p]);
        free(copies);
        ir_index_vector_destroy(&loads);
        ir_index_vector_destroy(&stores);
        return 0;
      }

      IRInstruction commit = {0};
      commit.op = IR_OP_ASYNC_COMMIT;
      commit.location = function->instructions[stores.items[loads.count - 1]]
                            .location;
      if (!ir_instruction_insert_move(function, after_pairs, &commit)) {
        for (size_t p = 0; p < loads.count; p++)
          ir_instruction_destroy_storage(&copies[p]);
        free(copies);
        ir_index_vector_destroy(&loads);
        ir_index_vector_destroy(&stores);
        return 0;
      }

      SourceLocation location = copies[0].location;
      for (size_t p = 0; p < loads.count; p++) {
        ir_instruction_make_nop(&function->instructions[loads.items[p]]);
        ir_instruction_destroy_storage(
            &function->instructions[stores.items[p]]);
        function->instructions[stores.items[p]] = copies[p];
        memset(&copies[p], 0, sizeof(copies[p]));
      }
      free(copies);
      function->cfg_valid = 0;
      *changed = 1;
      if (ir_explain_enabled() && ir_explain_location_enabled(&location)) {
        char headline[160];
        snprintf(headline, sizeof(headline),
                 "promoted %llu typed global-to-workgroup copy%s to asynchronous staging",
                 (unsigned long long)loads.count,
                 loads.count == 1 ? "" : " operations");
        ir_explain_remark(
            function->name, "GPU workgroup staging", location, 1, headline,
            "the copies are naturally aligned, single-use, and reach an acquire/release workgroup barrier through a straight-line register-only region",
            NULL,
            "neutral ASYNC_COPY/COMMIT/WAIT IR preserves synchronous fallback semantics");
      }
      i = barrier_index + 3; /* inserted commit + wait + original barrier */
    } else {
      i++;
    }
    ir_index_vector_destroy(&loads);
    ir_index_vector_destroy(&stores);
  }
  return 1;
}
