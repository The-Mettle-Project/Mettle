#include "type_checker_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  const char *name;
  const char *return_type;
  size_t parameter_count;
  const char *parameter_names[5];
  const char *parameter_types[5];
} GpuIntrinsicSignature;

static const GpuIntrinsicSignature g_gpu_intrinsic_signatures[] = {
    {"gpu_tid_x", "int32", 0, {0}, {0}},
    {"gpu_tid_y", "int32", 0, {0}, {0}},
    {"gpu_tid_z", "int32", 0, {0}, {0}},
    {"gpu_ntid_x", "int32", 0, {0}, {0}},
    {"gpu_ntid_y", "int32", 0, {0}, {0}},
    {"gpu_ntid_z", "int32", 0, {0}, {0}},
    {"gpu_ctaid_x", "int32", 0, {0}, {0}},
    {"gpu_ctaid_y", "int32", 0, {0}, {0}},
    {"gpu_ctaid_z", "int32", 0, {0}, {0}},
    {"gpu_nctaid_x", "int32", 0, {0}, {0}},
    {"gpu_nctaid_y", "int32", 0, {0}, {0}},
    {"gpu_nctaid_z", "int32", 0, {0}, {0}},
    {"gpu_barrier", NULL, 0, {0}, {0}},
    {"subgroup_local_id", "uint32", 0, {0}, {0}},
    {"subgroup_size", "uint32", 0, {0}, {0}},
    {"subgroup_broadcast_u32", "uint32", 2, {"value", "lane"}, {"uint32", "uint32"}},
    {"subgroup_broadcast_f32", "float32", 2, {"value", "lane"}, {"float32", "uint32"}},
    {"subgroup_shuffle_u32", "uint32", 2, {"value", "lane"}, {"uint32", "uint32"}},
    {"subgroup_shuffle_f32", "float32", 2, {"value", "lane"}, {"float32", "uint32"}},
    {"subgroup_reduce_add_u32", "uint32", 1, {"value"}, {"uint32"}},
    {"subgroup_reduce_add_f32", "float32", 1, {"value"}, {"float32"}},
    {"subgroup_reduce_min_u32", "uint32", 1, {"value"}, {"uint32"}},
    {"subgroup_reduce_min_f32", "float32", 1, {"value"}, {"float32"}},
    {"subgroup_reduce_max_u32", "uint32", 1, {"value"}, {"uint32"}},
    {"subgroup_reduce_max_f32", "float32", 1, {"value"}, {"float32"}},
    {"subgroup_scan_inclusive_add_u32", "uint32", 1, {"value"}, {"uint32"}},
    {"subgroup_scan_inclusive_add_f32", "float32", 1, {"value"}, {"float32"}},
    {"subgroup_scan_exclusive_add_u32", "uint32", 1, {"value"}, {"uint32"}},
    {"subgroup_scan_exclusive_add_f32", "float32", 1, {"value"}, {"float32"}},
    {"subgroup_ballot_word", "uint32", 2, {"predicate", "word"}, {"bool", "uint32"}},
    {"subgroup_any", "bool", 1, {"predicate"}, {"bool"}},
    {"subgroup_all", "bool", 1, {"predicate"}, {"bool"}},
    {"sqrtf", "float32", 1, {"x"}, {"float32"}},
    {"rsqrtf", "float32", 1, {"x"}, {"float32"}},
    {"fabsf", "float32", 1, {"x"}, {"float32"}},
    {"sinf", "float32", 1, {"x"}, {"float32"}},
    {"cosf", "float32", 1, {"x"}, {"float32"}},
    {"logf", "float32", 1, {"x"}, {"float32"}},
    {"expf", "float32", 1, {"x"}, {"float32"}},
    {"h2f", "float32", 1, {"bits"}, {"int32"}},
    {"f2h", "int32", 1, {"x"}, {"float32"}},
    {"f32_from_bits", "float32", 1, {"bits"}, {"uint32"}},
    {"bits_from_f32", "uint32", 1, {"x"}, {"float32"}},
    {"dp4a_u32", "uint32", 3, {"a", "b", "c"}, {"uint32", "uint32", "uint32"}},
    {"dp4a_s32", "int32", 3, {"a", "b", "c"}, {"int32", "int32", "int32"}},
    {"dp2a_lo_u32", "uint32", 3, {"a", "b", "c"}, {"uint32", "uint32", "uint32"}},
    {"dp2a_lo_s32", "int32", 3, {"a", "b", "c"}, {"int32", "int32", "int32"}},
    {"dp2a_hi_u32", "uint32", 3, {"a", "b", "c"}, {"uint32", "uint32", "uint32"}},
    {"dp2a_hi_s32", "int32", 3, {"a", "b", "c"}, {"int32", "int32", "int32"}},
    {"prmt_b32", "uint32", 3, {"a", "b", "sel"}, {"uint32", "uint32", "uint32"}},
    {"hadd2", "uint32", 2, {"a", "b"}, {"uint32", "uint32"}},
    {"hmul2", "uint32", 2, {"a", "b"}, {"uint32", "uint32"}},
    {"hfma2", "uint32", 3, {"a", "b", "c"}, {"uint32", "uint32", "uint32"}},
    {"h2f_lo", "float32", 1, {"p"}, {"uint32"}},
    {"h2f_hi", "float32", 1, {"p"}, {"uint32"}},
    {"f2h2", "uint32", 2, {"lo", "hi"}, {"float32", "float32"}},
    {"bf2f", "float32", 1, {"bits"}, {"uint32"}},
    {"f2bf", "uint32", 1, {"x"}, {"float32"}},
    {"workgroup_uniform", "uint32", 1, {"value"}, {"uint32"}},
    {"gpu_print", NULL, 1, {"fmt"}, {"cstring"}},
    {"gpu_print_i32", NULL, 2, {"fmt", "v"}, {"cstring", "int32"}},
    {"gpu_print_f32", NULL, 2, {"fmt", "v"}, {"cstring", "float32"}},
    {"gpu_print_2i32", NULL, 3, {"fmt", "a", "b"}, {"cstring", "int32", "int32"}},
    {"gpu_assert", NULL, 1, {"cond"}, {"int32"}},
    {"load4_f32", NULL, 2, {"src", "dst"}, {"float32*", "float32*"}},
    {"load4_u32", NULL, 2, {"src", "dst"}, {"uint32*", "uint32*"}},
    {"store4_f32", NULL, 2, {"dst", "src"}, {"float32*", "float32*"}},
    {"store4_u32", NULL, 2, {"dst", "src"}, {"uint32*", "uint32*"}},
    {"mbarrier_init", NULL, 2, {"bar", "count"}, {"uint64 shared*", "uint32"}},
    {"mbarrier_arrive_expect_tx", NULL, 2, {"bar", "bytes"}, {"uint64 shared*", "uint32"}},
    {"mbarrier_wait_parity", NULL, 2, {"bar", "parity"}, {"uint64 shared*", "uint32"}},
    {"fence_mbarrier_init", NULL, 0, {0}, {0}},
    {"fence_proxy_async", NULL, 0, {0}, {0}},
    {"tma_load_2d", NULL, 5, {"dst", "map", "c0", "c1", "bar"},
     {"uint8 shared*", "uint8*", "int32", "int32", "uint64 shared*"}},
    {"tensormap_acquire", NULL, 1, {"map"}, {"uint8*"}},
};

static const GpuIntrinsicSignature *gpu_intrinsic_find(const char *name) {
  if (!name) {
    return NULL;
  }
  for (size_t i = 0; i < sizeof(g_gpu_intrinsic_signatures) /
                             sizeof(g_gpu_intrinsic_signatures[0]);
       i++) {
    if (strcmp(g_gpu_intrinsic_signatures[i].name, name) == 0) {
      return &g_gpu_intrinsic_signatures[i];
    }
  }
  return NULL;
}

static void gpu_intrinsic_describe(const GpuIntrinsicSignature *signature,
                                   char *out, size_t capacity) {
  size_t used = (size_t)snprintf(out, capacity, "%s(", signature->name);
  for (size_t i = 0; i < signature->parameter_count && used < capacity; i++) {
    used += (size_t)snprintf(out + used, capacity - used, "%s%s: %s",
                             i ? ", " : "", signature->parameter_names[i],
                             signature->parameter_types[i]);
  }
  if (used < capacity) {
    used += (size_t)snprintf(out + used, capacity - used, ")");
  }
  if (signature->return_type && used < capacity) {
    snprintf(out + used, capacity - used, " -> %s", signature->return_type);
  }
}

static Type *gpu_intrinsic_return_type(TypeChecker *checker,
                                       const GpuIntrinsicSignature *signature) {
  return signature->return_type
             ? type_checker_get_type_by_name(checker, signature->return_type)
             : checker->builtin_void;
}

Symbol *type_checker_declare_gpu_intrinsic(TypeChecker *checker,
                                           const char *name) {
  const GpuIntrinsicSignature *signature = gpu_intrinsic_find(name);
  if (!signature || !checker || !checker->symbol_table ||
      !checker->symbol_table->global_scope ||
      (!checker->device_module && !type_checker_module_has_kernel(checker))) {
    return NULL;
  }
  Type *return_type = gpu_intrinsic_return_type(checker, signature);
  if (!return_type) {
    return NULL;
  }
  Type **parameter_types = NULL;
  char **parameter_names = NULL;
  if (signature->parameter_count > 0) {
    parameter_types = calloc(signature->parameter_count, sizeof(Type *));
    parameter_names = calloc(signature->parameter_count, sizeof(char *));
    if (!parameter_types || !parameter_names) {
      free(parameter_types);
      free(parameter_names);
      return NULL;
    }
    for (size_t i = 0; i < signature->parameter_count; i++) {
      parameter_types[i] = type_checker_get_type_by_name(
          checker, signature->parameter_types[i]);
      parameter_names[i] = strdup(signature->parameter_names[i]);
      if (!parameter_types[i] || !parameter_names[i]) {
        for (size_t j = 0; j <= i; j++) {
          free(parameter_names[j]);
        }
        free(parameter_names);
        free(parameter_types);
        return NULL;
      }
    }
  }
  Symbol *symbol = symbol_create(name, SYMBOL_FUNCTION, return_type);
  if (!symbol) {
    for (size_t i = 0; i < signature->parameter_count; i++) {
      free(parameter_names[i]);
    }
    free(parameter_names);
    free(parameter_types);
    return NULL;
  }
  symbol->data.function.parameter_count = signature->parameter_count;
  symbol->data.function.parameter_names = parameter_names;
  symbol->data.function.parameter_types = parameter_types;
  symbol->data.function.return_type = return_type;
  symbol->is_extern = 1;
  symbol->is_initialized = 1;
  symbol->link_name = strdup(name);
  if (!symbol->link_name) {
    symbol_destroy(symbol);
    return NULL;
  }
  Scope *current = checker->symbol_table->current_scope;
  checker->symbol_table->current_scope = checker->symbol_table->global_scope;
  int declared = symbol_table_declare(checker->symbol_table, symbol);
  checker->symbol_table->current_scope = current;
  if (!declared) {
    symbol_destroy(symbol);
    return NULL;
  }
  return symbol;
}

int type_checker_check_gpu_intrinsic_declaration(TypeChecker *checker,
                                                 ASTNode *declaration,
                                                 FunctionDeclaration *decl,
                                                 Type *return_type) {
  const GpuIntrinsicSignature *signature =
      decl && decl->is_extern ? gpu_intrinsic_find(decl->name) : NULL;
  if (!signature ||
      (!checker->device_module && !type_checker_module_has_kernel(checker))) {
    return 1;
  }
  int matches = decl->parameter_count == signature->parameter_count;
  Type *expected_return = gpu_intrinsic_return_type(checker, signature);
  if (matches &&
      !type_checker_types_equal(expected_return, return_type)) {
    matches = 0;
  }
  for (size_t i = 0; matches && i < decl->parameter_count; i++) {
    Type *declared =
        type_checker_get_type_by_name(checker, decl->parameter_types[i]);
    Type *expected = type_checker_get_type_by_name(
        checker, signature->parameter_types[i]);
    if (!declared || !expected ||
        !type_checker_types_equal(declared, expected)) {
      matches = 0;
    }
  }
  if (matches) {
    return 1;
  }
  char expected_text[256];
  gpu_intrinsic_describe(signature, expected_text, sizeof(expected_text));
  type_checker_set_error_at_location(
      checker, declaration->location,
      "extern '%s' does not match the GPU intrinsic %s; drop the declaration "
      "(device code knows it) or declare exactly that signature",
      decl->name, expected_text);
  return 0;
}
