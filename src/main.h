#ifndef MAIN_H
#define MAIN_H

#include "parser/parser.h"
#include "codegen/code_generator.h"
#include "debug/debug_info.h"
#include "error/error_explain.h"
#include "error/error_reporter.h"
#include "ir/ir_comptime.h"
#include "ir/ir_explain_safety.h"
#include "ir/ir_pgo.h"
#include "ir/ir_safety.h"
#include "ir/ir_verify.h"
#include "lexer/lexer.h"
#include "semantic/register_allocator.h"
#include "semantic/symbol_table.h"
#include "semantic/monomorphize.h"
#include "semantic/type_checker.h"
#include <stddef.h>

typedef enum {
  LINKER_MODE_AUTO = 0,
  LINKER_MODE_INTERNAL,
  LINKER_MODE_GCC,
  LINKER_MODE_MSVC,
} LinkerMode;

typedef struct {
  const char *input_filename;
  const char *output_filename;
  int debug_mode;
  int dump_ast;
  int dump_ir;
  int ml_opt;
  int emit_ptx;
  const char *emit_kernel_decls;
  const char *ptx_target;
  int ptx_isa_major;
  int ptx_isa_minor;
  int ptx_tensor_tuple_budget;
  int report_occupancy;
  int report_sms;
  int gpu_checks;
  int report_launches;
  int report_gpu_types;
  int emit_spirv;
  int emit_metal;
  int metal_version_major;
  int metal_version_minor;
  int metal_fast_math;
  int gpu_provider;
  int emit_arm64;
  int emit_arm64_obj;
  int optimize;
  int release;
  int safe;
  int simd_report;
  int explain;
  int explain_all;
  int explain_json;
  const char *explain_filter;
  int explain_view;
  int explain_view_text;
  int explain_view_columns;
  int explain_view_rows;
  const char *explain_view_keys;
  int annotate_asm;
  int asm_syntax;
  int annotate_q_lo;
  int annotate_q_hi;
  const char *annotate_q_fn;
  int annotate_hot;
  int pgo;
  int test_mode;
  const char *test_filter;
  const char *trace_function;
  const char *const *trace_args;
  size_t trace_arg_count;
  int expand_mode;
  int swap_check_mode;
  const char *swap_old_name;
  const char *swap_new_name;
  int report_expansion;
  size_t expansion_budget;
  int expansion_budget_set;
  int report_rules;
  long long rule_budget;
  int rule_budget_set;
  int report_meta;
  long long meta_budget;
  int meta_budget_set;
  void *trace_rule_checker;
  int report_twins;
  int apply_rule_fixes;
  int print_fixes;
  int ownership_json;
  int expand_focus;
  int why_mode;
  const char *why_subject;
  const char *why_what;
  int report_proofs;
  long long proof_budget;
  int proof_budget_set;
  int report_effects;
  long long effect_budget;
  int effect_budget_set;
  const char *target_desc_path;
  int report_target;
  int check_proofs;
  int check_tasks;
  int check_deadlines;
  int check_overflow;
  int record_trace;
  const char *check_trace_path;
  int machine_mode;
  int emulate_mode;
  int report_deadlines;
  int check_effects;
  int check_purity_fault;
  int emit_object;
  int generate_debug_symbols;
  int generate_line_mapping;
  int generate_stack_trace_support;
  int generate_crash_report;
  const char *debug_format;
  const char **import_directories;
  size_t import_directory_count;
  const char **link_arguments;
  size_t link_argument_count;
  const char **shared_libraries;
  size_t shared_library_count;
  const char **library_search_paths;
  size_t library_search_path_count;
  const char **runpaths;
  size_t runpath_count;
  int shared_output;
  int export_dynamic;
  const char *soname;
  int live;
  const char *live_patch_target;
  int live_patch_all;
  long long live_at_tick;
  const char *dynamic_linker;
  const char *stdlib_directory;
  int prelude;
  int profile;
  int profile_runtime;
  int profile_runtime_ops;
  int profile_blocks;
  int pgo_gen;
  const char *pgo_use;
  int assume_no_signed_overflow;
  int debug_hooks;
  int native_heap;
  int tracy;
  int static_link;
  int musl_link;
  const char *tracy_directory;
  int debug_compiler;
  int main_wants_argc_argv;
  int building_executable;
  int windows_subsystem;
  const char *target_triple;
  unsigned long long image_base;
  int image_base_set;
  const char *flat_output;
  LinkerMode linker_mode;
} CompilerOptions;

int compile_file(const char *input_filename, const char *output_filename,
                 CompilerOptions *options);
void print_usage(const char *program_name);
char *read_file(const char *filename);

#endif
