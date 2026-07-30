#ifndef MAIN_H
#define MAIN_H

#include "codegen/code_generator.h"
#include "debug/debug_info.h"
#include "error/error_explain.h"
#include "error/error_reporter.h"
#include "ir/ir_comptime.h"
#include "ir/ir_pgo.h"
#include "ir/ir_verify.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
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
  int dump_ir;
  int ml_opt; /* --ml-opt: run the learned ML optimizer pass on the IR */
  int emit_ptx; /* --emit-ptx: lower declared kernels to PTX .entry, no object */
  const char *ptx_target; /* backend-only target such as sm_121a */
  int ptx_isa_major;
  int ptx_isa_minor;
  int ptx_tensor_tuple_budget; /* 0=arch default; backend-only residency knob */
  int emit_spirv; /* --emit-spirv: lower declared kernels to SPIR-V, no object */
  int emit_arm64; /* --emit-arm64: lower scalar functions to an AArch64 ELF */
  int optimize;
  int release;
  int simd_report; /* --simd-report: note what each `@simd` loop became */
  int explain;     /* --explain: report optimization decisions (vectorization,
                      inlining) for the main input file, with reasons */
  int explain_all; /* --explain-all: drop the focus filter (whole program) */
  int explain_json; /* --explain-json: machine-readable .explain.json sidecar */
  int annotate_asm; /* --annotate-asm: emit asm annotated with codegen decisions
                       (a listing on stdout + <stem>.annot.json sidecar) */
  int asm_syntax;   /* 0=intel, 1=att, 2=both (matches MirAnnotSyntax) */
  /* LLM-facing focused codegen queries (imply --annotate-asm). When a line
   * range is set the annotator prints a compact report for just those source
   * lines (asm + cost + covering loops + live registers + decisions) instead of
   * the full listing; the hot query prints the program's top hotspots. */
  int annotate_q_lo; /* --annotate-lines=A-B: first source line (0 = unset) */
  int annotate_q_hi; /* last source line of the range */
  const char *annotate_q_fn; /* --annotate-fn=NAME: restrict to one function */
  int annotate_hot; /* --annotate-hot[=N]: top-N hotspots (0 = unset) */
  /* --pgo: zero-run profile-guided optimization - interpret main() at
   * compile time and feed measured call frequencies to the optimizer. */
  int pgo;
  /* `mettle test`: run every @test function in the compile-time interpreter
   * instead of generating code. */
  int test_mode;
  const char *test_filter; /* --filter=SUBSTR: run matching tests only */
  /* `mettle trace <file> <fn> [args...]`: interpret one function on the given
   * arguments and print a line-by-line value trace. */
  const char *trace_function;
  const char *const *trace_args;
  size_t trace_arg_count;
  int emit_object;
  int generate_debug_symbols;
  int generate_line_mapping;
  int generate_stack_trace_support;
  const char *debug_format; // "dwarf", "stabs", or "map"
  const char **import_directories;
  size_t import_directory_count;
  const char **link_arguments;
  size_t link_argument_count;
  const char *stdlib_directory;
  int prelude;
  int profile;
  int profile_runtime;
  int profile_runtime_ops;
  int profile_blocks; /* --profile-blocks: per-basic-block execution counters
                         dumped to a .mprof sidecar for the VTune-style codegen
                         view; implies --profile-runtime. */
  int debug_hooks; /* --debug-hooks: interactive debugger instrumentation */
  int native_heap;
  int tracy;
  int static_link;
  int musl_link;
  const char *tracy_directory;
  int debug_compiler;
  int main_wants_argc_argv;
  /* Set when this compile feeds a --build executable link (as opposed to a
   * bare --emit-obj library object). Gates whole-program transforms that are
   * only sound when `main` is the single entry point, e.g. dead-function
   * elimination. */
  int building_executable;
  LinkerMode linker_mode;
} CompilerOptions;

int compile_file(const char *input_filename, const char *output_filename,
                 CompilerOptions *options);
void print_usage(const char *program_name);
char *read_file(const char *filename);

#endif // MAIN_H
