#ifndef MAIN_H
#define MAIN_H

#include "codegen/code_generator.h"
#include "debug/debug_info.h"
#include "error/error_reporter.h"
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
  int optimize;
  int release;
  int strip_asm_comments;
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
  int tracy;
  int static_link;
  int musl_link;
  const char *tracy_directory;
  int debug_compiler;
  int main_wants_argc_argv;
  LinkerMode linker_mode;
} CompilerOptions;

int compile_file(const char *input_filename, const char *output_filename,
                 CompilerOptions *options);
void print_usage(const char *program_name);
char *read_file(const char *filename);

#endif // MAIN_H
