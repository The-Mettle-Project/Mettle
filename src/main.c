#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "main.h"
#include "common.h"
#include "parser/ast_dump.h"
#include "parser/ast_print.h"
#include "codegen/binary/startup.h"
#include "codegen/binary/mir_annotate.h"
#include "codegen/binary_emitter.h"
#include "codegen/binary/arm64_ir.h"
#include "codegen/gpu_detect.h"
#include "codegen/ptx_emitter.h"
#include "codegen/spirv_emitter.h"
#include "codegen/msl_emitter.h"
#include "codegen/target.h"
#include "codegen/flat_emitter.h"
#include "linker/elf_image.h"
#include "linker/elf_shared.h"
#include "linker/live_patch.h"
#include "live_driver.h"
#include "linker/pe_emitter.h"
#include "string_intern.h"
#include "compiler/compiler_context.h"
#include "compiler/compiler_crash.h"
#include "compiler/compiler_self_profile.h"
#include "runtime/owned.h"
#include "runtime/verify_owned.h"
#include "ir/ir.h"
#include "ir/ir_lowering.h"
#include "ir/ir_optimize.h"
#include "ir/ir_explain_memory.h"
#include "ir/ir_profile.h"
#include "ir/ir_debug_hooks.h"
#include "semantic/import_resolver.h"
#include "semantic/rule_reflect.h"
#include "semantic/meta_stage.h"
#include "semantic/target_desc.h"
#include "ir/ir_rules.h"
#include "ir/ir_deadline.h"
#include "ir/ir_opt_cost.h"
#include "ir/ir_trace_record.h"
#include "semantic/machine_desc.h"
#include "ir/ir_effects.h"
#include "ir/ir_purity.h"
#include "ir/ir_twins.h"
#include "ir/ir_machine.h"
#include "tune.h"
#include "ir/ir_trace.h"
#include "ir/ir_explain_ledger.h"
#include "ir/ir_interp.h"
#include "ir/ir_numerics_check.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32) || defined(__MINGW32__)
#include <sys/time.h>
#endif
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <sys/stat.h>
__declspec(dllimport) unsigned long __stdcall GetFullPathNameA(
    const char *name, unsigned long length, char *buffer, char **file_part);
#if !defined(__MINGW32__)
typedef long long MettleQpcTicks;
__declspec(dllimport) int __stdcall QueryPerformanceFrequency(MettleQpcTicks *frequency);
__declspec(dllimport) int __stdcall QueryPerformanceCounter(MettleQpcTicks *counter);
#endif
#else
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#endif

#define METTLE_STRINGIFY_(x) #x
#define METTLE_STRINGIFY(x) METTLE_STRINGIFY_(x)
#ifndef METTLE_VERSION
#ifdef METTLE_VERSION_RAW
#define METTLE_VERSION METTLE_STRINGIFY(METTLE_VERSION_RAW)
#else
#define METTLE_VERSION "v0.17.0"
#endif
#endif

#define PROFILE_PHASE_READ_INPUT METTLE_COMPILER_PHASE_READ_INPUT
#define PROFILE_PHASE_LEXICAL_VALIDATION METTLE_COMPILER_PHASE_LEXICAL_VALIDATION
#define PROFILE_PHASE_INIT METTLE_COMPILER_PHASE_INIT
#define PROFILE_PHASE_PARSE METTLE_COMPILER_PHASE_PARSE
#define PROFILE_PHASE_PRELUDE METTLE_COMPILER_PHASE_PRELUDE
#define PROFILE_PHASE_IMPORTS METTLE_COMPILER_PHASE_IMPORTS
#define PROFILE_PHASE_MONOMORPHIZE METTLE_COMPILER_PHASE_MONOMORPHIZE
#define PROFILE_PHASE_TYPE_CHECK METTLE_COMPILER_PHASE_TYPE_CHECK
#define PROFILE_PHASE_IR_LOWERING METTLE_COMPILER_PHASE_IR_LOWERING
#define PROFILE_PHASE_IR_OPTIMIZATION METTLE_COMPILER_PHASE_IR_OPTIMIZATION
#define PROFILE_PHASE_IR_DUMP METTLE_COMPILER_PHASE_IR_DUMP
#define PROFILE_PHASE_CODEGEN METTLE_COMPILER_PHASE_CODEGEN
#define PROFILE_PHASE_WRITE_OUTPUT METTLE_COMPILER_PHASE_WRITE_OUTPUT
#define PROFILE_PHASE_DEBUG_INFO METTLE_COMPILER_PHASE_DEBUG_INFO
#define PROFILE_PHASE_CLEANUP METTLE_COMPILER_PHASE_CLEANUP
#define PROFILE_PHASE_COUNT METTLE_COMPILER_PHASE_COUNT

static int explain_rule_code(const char *code, const char *path);

static int main_keep_machine_rule(const IRFunction *rule) {
  IRRuleKind kind = ir_rule_kind(rule);
  return kind == IR_RULE_OVER_MACHINE || kind == IR_RULE_OVER_TRACE;
}

static int program_declares_machine_rule(const ASTNode *program) {
  const Program *data;
  if (!program || program->type != AST_PROGRAM || !program->data) {
    return 0;
  }
  data = (const Program *)program->data;
  for (size_t i = 0; i < data->declaration_count; i++) {
    const ASTNode *decl = data->declarations[i];
    const FunctionDeclaration *fd;
    const char *type;
    const char *base;
    if (!decl || decl->type != AST_FUNCTION_DECLARATION || !decl->data) {
      continue;
    }
    fd = (const FunctionDeclaration *)decl->data;
    if (!fd->is_rule || fd->parameter_count != 1 || !fd->parameter_types ||
        !fd->parameter_types[0]) {
      continue;
    }
    type = fd->parameter_types[0];
    base = strrchr(type, '.');
    if (base) {
      type = base + 1;
    }
    if (strcmp(type, "Machine") == 0) {
      return 1;
    }
  }
  return 0;
}

static int compiler_options_use_profile_runtime(const CompilerOptions *options) {
  return options &&
         (options->profile_runtime || options->profile_runtime_ops);
}

static int compiler_options_install_crash_handler(const CompilerOptions *options) {
  if (!options) {
    return 0;
  }
  if (options->generate_stack_trace_support) {
    return 1;
  }
  return options->generate_crash_report && options->building_executable &&
         !options->flat_output && !mtlc_target()->freestanding;
}

typedef struct {
  int enabled;
  double phases_ms[PROFILE_PHASE_COUNT];
} CompilerProfile;

static double compiler_profile_now_ms(void) {
  return mettle_now_ms();
}

static void compiler_profile_init(CompilerProfile *profile, int enabled) {
  if (!profile) {
    return;
  }
  memset(profile, 0, sizeof(*profile));
  profile->enabled = enabled;
}

static double compiler_profile_begin(const CompilerProfile *profile) {
  return (profile && profile->enabled) ? compiler_profile_now_ms() : 0.0;
}

static void compiler_profile_add(CompilerProfile *profile,
                                 MettleCompilerPhase phase,
                                 double started_ms) {
  if (!profile || !profile->enabled || phase < 0 ||
      phase >= PROFILE_PHASE_COUNT) {
    return;
  }
  profile->phases_ms[phase] += compiler_profile_now_ms() - started_ms;
}

static void compiler_profile_print_compile(const CompilerProfile *profile,
                                           const char *input_filename,
                                           int result) {
  double total_ms = 0.0;

  if (!profile || !profile->enabled) {
    return;
  }

  for (int i = 0; i < PROFILE_PHASE_COUNT; i++) {
    total_ms += profile->phases_ms[i];
  }

  fprintf(stderr, "Compilation profile for '%s'%s:\n",
          input_filename ? input_filename : "(unknown)",
          result == 0 ? "" : " (failed)");
  for (int i = 0; i < PROFILE_PHASE_COUNT; i++) {
    double ms = profile->phases_ms[i];
    double percent = total_ms > 0.0 ? (ms * 100.0) / total_ms : 0.0;

    if (ms <= 0.0) {
      continue;
    }
    fprintf(stderr, "  %-20s %9.3f ms  %6.2f%%\n",
            mettle_compiler_phase_name((MettleCompilerPhase)i), ms, percent);
  }
  fprintf(stderr, "  %-20s %9.3f ms  %6.2f%%\n", "total", total_ms, 100.0);
  if (getenv("METTLE_ALLOC_REPORT")) {
    mettle_alloc_report();
  }
}

static void compiler_set_phase(MettleCompilerPhase phase) {
  mettle_compiler_ctx_set_phase(phase);
}

static int directory_exists(const char *path) {
  if (!path || path[0] == '\0') {
    return 0;
  }
  return mettle_path_is_directory(path);
}

static char *join_paths(const char *left, const char *right) {
  if (!left || !right) {
    return NULL;
  }

  size_t left_len = strlen(left);
  size_t right_len = strlen(right);
  int has_sep = left_len > 0 &&
                (left[left_len - 1] == '/' || left[left_len - 1] == '\\');
  size_t total = left_len + right_len + (has_sep ? 1 : 2);

  char *joined = malloc(total);
  if (!joined) {
    return NULL;
  }

  memcpy(joined, left, left_len);
  if (!has_sep) {
#ifdef _WIN32
    joined[left_len++] = '\\';
#else
    joined[left_len++] = '/';
#endif
  }
  memcpy(joined + left_len, right, right_len);
  joined[left_len + right_len] = '\0';
  return joined;
}

static char *directory_from_path(const char *path) {
  if (!path || path[0] == '\0') {
    return NULL;
  }

  const char *last_slash = strrchr(path, '/');
  const char *last_backslash = strrchr(path, '\\');
  const char *last_sep =
      (last_slash > last_backslash) ? last_slash : last_backslash;
  if (!last_sep) {
    return NULL;
  }

  size_t len = (size_t)(last_sep - path);
  char *dir = malloc(len + 1);
  if (!dir) {
    return NULL;
  }

  memcpy(dir, path, len);
  dir[len] = '\0';
  return dir;
}

static char *get_executable_path(const char *argv0) {
  static char *cached_path = NULL;
  static int cached = 0;
  if (cached) return cached_path ? strdup(cached_path) : NULL;
  cached = 1;

#ifdef _WIN32
  char program_path[4096];
  long long path_length =
      mettle_executable_path(program_path, sizeof(program_path));
  if (path_length > 0 && path_length < (long long)sizeof(program_path)) {
    program_path[path_length] = '\0';
    cached_path = strdup(program_path);
    return cached_path ? strdup(cached_path) : NULL;
  }
  if (argv0 && argv0[0] != '\0') {
    cached_path = strdup(argv0);
    return cached_path ? strdup(cached_path) : NULL;
  }
  return NULL;
#elif defined(__APPLE__)
  uint32_t size = 0;
  if (_NSGetExecutablePath(NULL, &size) != -1 || size == 0) {
    return NULL;
  }
  char *buffer = malloc((size_t)size + 1);
  if (!buffer) {
    return NULL;
  }
  if (_NSGetExecutablePath(buffer, &size) != 0) {
    free(buffer);
    return NULL;
  }
  buffer[size] = '\0';
  cached_path = buffer;
  return strdup(cached_path);
#else
  char buffer[PATH_MAX + 1];
  ssize_t len = (ssize_t)mettle_readlink("/proc/self/exe", buffer, PATH_MAX);
  if (len > 0) {
    buffer[len] = '\0';
    cached_path = strdup(buffer);
    return cached_path ? strdup(cached_path) : NULL;
  }
  if (argv0 && argv0[0] != '\0') {
    cached_path = strdup(argv0);
    return cached_path ? strdup(cached_path) : NULL;
  }
  return NULL;
#endif
}

static char *infer_default_sibling_directory(const char *argv0,
                                             const char *leaf_name,
                                             const char *fallback_path) {
  char *exe_path = get_executable_path(argv0);
  char *exe_dir = directory_from_path(exe_path);

  if (exe_dir) {
    char *parent_dir = join_paths(exe_dir, "..");
    if (parent_dir) {
      char *packaged = join_paths(parent_dir, leaf_name);
      free(parent_dir);
      if (packaged && directory_exists(packaged)) {
        free(exe_path);
        free(exe_dir);
        return packaged;
      }
      free(packaged);
    }

    char *local = join_paths(exe_dir, leaf_name);
    if (local && directory_exists(local)) {
      free(exe_path);
      free(exe_dir);
      return local;
    }
    free(local);
  }

  free(exe_path);
  free(exe_dir);

  if (directory_exists(leaf_name)) {
    return strdup(leaf_name);
  }

  return fallback_path ? strdup(fallback_path) : NULL;
}

static char *infer_default_stdlib_directory(const char *argv0) {
  return infer_default_sibling_directory(argv0, "stdlib", "stdlib");
}

static char *infer_default_runtime_directory(const char *argv0) {
  return infer_default_sibling_directory(argv0, "runtime", NULL);
}

static void ml_opt_set_default_paths(const char *argv0) {
  char *dir = infer_default_sibling_directory(argv0, "mlopt", "tools/mlopt");
  if (!dir) {
    return;
  }
  static const struct {
    const char *env;
    const char *file;
  } resources[] = {
      {"METTLE_ML_MODEL", "gnn_genius.bin"},
      {"METTLE_ML_BWLIB", "bw_lib.txt"},
      {"METTLE_ML_GF2LIB", "gf2_lib1.txt"},
  };
  for (size_t i = 0; i < sizeof(resources) / sizeof(resources[0]); i++) {
    if (getenv(resources[i].env)) {
      continue;
    }
    char *path = join_paths(dir, resources[i].file);
    if (path) {
      char *kv = malloc(strlen(resources[i].env) + strlen(path) + 2);
      if (kv) {
        sprintf(kv, "%s=%s", resources[i].env, path);
        putenv(kv);
      }
      free(path);
    }
  }
  free(dir);
}

static char *infer_default_docs_directory(const char *argv0) {
  return infer_default_sibling_directory(argv0, "docs", NULL);
}

static void print_doc_reference(const char *argv0, const char *relative_path) {
  char *docs_dir = infer_default_docs_directory(argv0);
  if (docs_dir && relative_path) {
    char *full_path = join_paths(docs_dir, relative_path);
    if (full_path) {
      printf("Doc: %s\n", full_path);
      free(full_path);
      free(docs_dir);
      return;
    }
  }

  if (relative_path) {
    printf("Doc: docs/%s\n", relative_path);
  }
  free(docs_dir);
}

#define METTLE_HELP_TOPICS "build, runtime (alias: heap, gc), interop, stdlib, web, diagnostics (alias: errors), verify, test (alias: trace)"

static int print_help_topic(const char *program_name, const char *argv0,
                            const char *topic) {
  if (!topic || topic[0] == '\0') {
    print_usage(program_name);
    return 0;
  }

  if (strcmp(topic, "all") == 0) {
    printf("Mettle help topics\n\n");
    print_help_topic(program_name, argv0, "build");
    printf("\n");
    print_help_topic(program_name, argv0, "runtime");
    printf("\n");
    print_help_topic(program_name, argv0, "interop");
    printf("\n");
    print_help_topic(program_name, argv0, "stdlib");
    printf("\n");
    print_help_topic(program_name, argv0, "web");
    return 0;
  }

  if (strcmp(topic, "build") == 0 || strcmp(topic, "compile") == 0) {
    printf("build - compile, assemble, and link an executable\n\n");
    printf("  Common:\n");
    printf("    mettle --build app.mettle -o app.exe\n");
    printf("    mettle --build --release app.mettle -o app.exe              "
           "   (optimized, stripped)\n");
    printf("\n");
    printf("  Notes:\n");
    printf("    --build emits a COFF object and links with the internal PE "
           "linker by default (no NASM/gcc/link.exe needed).\n");
    printf("    --linker auto tries internal, then gcc, then link.exe.\n");
    printf("    --linker internal forces the native PE linker and probes "
           "common Win32 DLLs directly.\n");
    printf("    --link-arg <arg> passes an extra linker argument (repeatable) "
           "for extra DLLs or import libraries.\n");
    printf("    --tracy links std/tracy with the Tracy profiler (requires a "
           "Tracy repo; see --tracy-dir / TRACY_DIR).\n");
    print_doc_reference(argv0, "compilation.md");
    return 0;
  }

  if (strcmp(topic, "runtime") == 0 || strcmp(topic, "heap") == 0 ||
      strcmp(topic, "gc") == 0) {
    printf("runtime - Mettle's owned freestanding runtime\n\n");
    printf("  No GC, C runtime, compiler runtime, async scheduler, or thread "
           "pool.\n");
    printf("  Startup, heap, files, text conversion, clocks, threads, sockets, "
           "and process calls\n");
    printf("  use Mettle code and direct OS calls. Linked executables are "
           "checked before success.\n\n");
    printf("  Optional owned helper objects are linked only when referenced:\n");
    printf("    crash_handler.o - symbolized backtraces; linked when an object "
           "references mettle_crash_*\n");
    printf("                      (compiled with -d, -s, -g, or with IR "
           "null/bounds traps active).\n");
    printf("    atomics.o       - Win32/__sync_* wrappers; linked when an "
           "object references mettle_atomic_*\n");
    printf("                      (any use of std/thread interlocked atomic "
           "helpers).\n");
    print_doc_reference(argv0, "runtime-model.md");
    return 0;
  }

  if (strcmp(topic, "test") == 0 || strcmp(topic, "tests") == 0 ||
      strcmp(topic, "trace") == 0) {
    printf("test / trace - compile-time execution (no codegen, no linking)\n\n");
    printf("  mettle test app.mettle [--filter=SUBSTR]\n");
    printf("      Run every @test function in the compiler's interpreter.\n");
    printf("      assert(cond) / assert_eq(left, right) failures render as\n");
    printf("      diagnostics with the actual values; unfreed allocations and\n");
    printf("      null/out-of-bounds accesses fail or flag the test. @test\n");
    printf("      functions are type-checked in every build but compiled out\n");
    printf("      of normal binaries.\n\n");
    printf("  mettle trace app.mettle sum_range 0 10\n");
    printf("      Interpret one function on concrete arguments and print its\n");
    printf("      source annotated with the values each line produced.\n");
    print_doc_reference(argv0, "testing.md");
    return 0;
  }

  if (strcmp(topic, "verify") == 0 || strcmp(topic, "validation") == 0) {
    printf("verify - per-pass translation validation (self-verifying optimizer)\n\n");
    printf("  mettle --verify app.mettle\n\n");
    printf("  After every optimization pass, each changed function's before/after IR\n");
    printf("  is executed on generated inputs and compared: return value, buffer\n");
    printf("  bytes, extern-call trace, globals. A diverging pass is reported with a\n");
    printf("  concrete counterexample, quarantined for that function, and the build\n");
    printf("  continues from the validated pre-pass IR - the binary is always built\n");
    printf("  from IR that passed validation.\n\n");
    printf("  METTLE_VERIFY_BREAK=pass[:fn]  sabotage self-test (corrupts one\n");
    printf("                                 constant after the named pass; --verify\n");
    printf("                                 must catch and heal it)\n\n");
    printf("  Three modes record a claim without establishing it. Each warns when\n");
    printf("  set, and --verify refuses to run beside one.\n");
    printf("  METTLE_TRUST_REFINEMENTS=1     accept every declared-type conversion\n");
    printf("                                 without running its proof\n");
    printf("  METTLE_TRUST_EFFECTS=1         skip every effect obligation\n");
    printf("  METTLE_TRUST_DEADLINES=1       price an unbounded loop at one turn\n");
    print_doc_reference(argv0, "translation-validation.md");
    return 0;
  }

  if (strcmp(topic, "diagnostics") == 0 || strcmp(topic, "errors") == 0 ||
      strcmp(topic, "warnings") == 0) {
    printf("diagnostics - compile errors, warnings, and tooling output\n\n");
    printf("  Every diagnostic carries a stable code (E0001..E0007, "
           "M0101..M0117), a source snippet\n");
    printf("  with the offending range underlined, and a help suggestion. The "
           "compiler recovers after\n");
    printf("  errors, so one compile reports every problem in the file.\n\n");
    printf("  mettle explain <CODE>       extended docs for a code (try: "
           "mettle explain E0004)\n");
    printf("  mettle explain list         index of every code\n");
    printf("  --error-format=json         one JSON object per diagnostic on "
           "stderr, for editors/CI\n");
    printf("  NO_COLOR / CLICOLOR_FORCE   disable / force ANSI colors\n\n");
    printf("  Warnings include unused variables (prefix a name with '_' to "
           "opt out), unreachable code,\n");
    printf("  and compile-time memory-safety findings (use-after-free, leaks, "
           "double free, ...).\n\n");
    printf("  `explain` also covers the optimizer's decision codes, the ids "
           "--explain prints in\n");
    printf("  brackets after each verdict: mettle explain "
           "dot-shape-address\n");
    print_doc_reference(argv0, "diagnostics.md");
    return 0;
  }

  if (strcmp(topic, "interop") == 0 || strcmp(topic, "c") == 0) {
    printf("interop - calling C and OS APIs\n\n");
    printf("  Declare external C functions with extern fn.\n");
    printf("  Prefer std/win32 for common Windows OS APIs.\n");
    printf("  Use --link-arg for extra linker libraries in --build mode.\n");
    printf("  syscall(number, ...) asks the kernel directly, with no stub to "
           "link.\n");
    printf("  Example:\n");
    printf("    mettle --build --emit-obj --linker internal main.mettle -o "
           "main.exe\n");
    print_doc_reference(argv0, "c-interop.md");
    return 0;
  }

  if (strcmp(topic, "stdlib") == 0) {
    printf("stdlib - standard library resolution\n\n");
    printf("  std/... imports resolve against the bundled stdlib by "
           "default.\n");
    printf("  No project-local stdlib/ folder is required.\n");
    printf("  Override with --stdlib <dir> only when you need a custom "
           "root.\n");
    print_doc_reference(argv0, "standard-library.md");
    return 0;
  }

  if (strcmp(topic, "web") == 0) {
    printf("web - the demo web server example\n\n");
    printf("  Build it with .\\web\\build.bat\n");
    printf("  That delegates to mettle --build with --link-arg -lws2_32.\n");
    print_doc_reference(argv0, "compilation.md");
    return 0;
  }

  if (strcmp(topic, "docs") == 0 || strcmp(topic, "topics") == 0) {
    printf("Help topics: " METTLE_HELP_TOPICS "\n");
    printf("Use 'mettle help <topic>' for one, or 'mettle help all' for "
           "everything.\n");
    print_doc_reference(argv0, "LANGUAGE.md");
    return 0;
  }

  fprintf(stderr, "Error: unknown help topic '%s'\n", topic);
  fprintf(stderr, "Available topics: " METTLE_HELP_TOPICS "\n");
  fprintf(stderr, "Try 'mettle help' for general usage.\n");
  return 1;
}

static char *build_sidecar_filename(const char *base_filename,
                                    const char *suffix) {
  if (!base_filename || !suffix) {
    return NULL;
  }

  size_t base_len = strlen(base_filename);
  size_t suffix_len = strlen(suffix);
  char *path = malloc(base_len + suffix_len + 1);
  if (!path) {
    return NULL;
  }

  memcpy(path, base_filename, base_len);
  memcpy(path + base_len, suffix, suffix_len);
  path[base_len + suffix_len] = '\0';
  return path;
}

static char *replace_extension(const char *path, const char *extension) {
  if (!path || !extension) {
    return NULL;
  }

  const char *last_slash = strrchr(path, '/');
  const char *last_backslash = strrchr(path, '\\');
  const char *last_sep =
      (last_slash > last_backslash) ? last_slash : last_backslash;
  const char *last_dot = strrchr(path, '.');
  size_t stem_len =
      (last_dot && (!last_sep || last_dot > last_sep)) ? (size_t)(last_dot - path)
                                                       : strlen(path);
  size_t ext_len = strlen(extension);

  char *result = malloc(stem_len + ext_len + 1);
  if (!result) {
    return NULL;
  }

  memcpy(result, path, stem_len);
  memcpy(result + stem_len, extension, ext_len);
  result[stem_len + ext_len] = '\0';
  return result;
}

int target_argument_is_description(const char *argument);
int load_target_description(CompilerOptions *options);

static int host_target_is_elf(void) {
  BinaryTargetFormat format = mtlc_target()->format;
  return format == BINARY_TARGET_FORMAT_ELF_X64 ||
         format == BINARY_TARGET_FORMAT_ELF_ARM64;
}

static int host_target_is_macho(void) {
  BinaryTargetFormat format = mtlc_target()->format;
  return format == BINARY_TARGET_FORMAT_MACHO_X64 ||
         format == BINARY_TARGET_FORMAT_MACHO_ARM64;
}

static int host_target_is_posix(void) {
  return host_target_is_elf() || host_target_is_macho();
}

static char *default_executable_filename(const char *input_filename) {
  if (!input_filename || input_filename[0] == '\0') {
    return NULL;
  }

  if (!host_target_is_posix()) {
    return replace_extension(input_filename, ".exe");
  }

  {
    char *stem = replace_extension(input_filename, "");
    if (stem && strcmp(stem, input_filename) == 0) {
      free(stem);
      return NULL;
    }
    return stem;
  }
}

static const char *default_object_output_filename(void) {
  return host_target_is_posix() ? "output.o" : "output.obj";
}

static const char *linker_mode_name(LinkerMode mode) {
  switch (mode) {
  case LINKER_MODE_INTERNAL:
    return "internal";
  case LINKER_MODE_GCC:
    return "gcc";
  case LINKER_MODE_MSVC:
    return "msvc";
  case LINKER_MODE_AUTO:
  default:
    return "auto";
  }
}

static int parse_linker_mode(const char *text, LinkerMode *mode_out) {
  if (!text || !mode_out) {
    return 0;
  }

  if (strcmp(text, "auto") == 0) {
    *mode_out = LINKER_MODE_AUTO;
    return 1;
  }
  if (strcmp(text, "internal") == 0) {
    *mode_out = LINKER_MODE_INTERNAL;
    return 1;
  }
  if (strcmp(text, "gcc") == 0) {
    *mode_out = LINKER_MODE_GCC;
    return 1;
  }
  if (strcmp(text, "msvc") == 0 || strcmp(text, "link") == 0) {
    *mode_out = LINKER_MODE_MSVC;
    return 1;
  }

  return 0;
}

static int object_has_undefined_symbol_prefix(const char *object_path,
                                              const char *prefix) {
  LinkObject *object = NULL;
  char *error_message = NULL;
  size_t i = 0u;
  int found = 0;

  if (!object_path || !prefix) {
    return 0;
  }

  if (!link_object_read(object_path, &object, &error_message)) {
    free(error_message);
    return 1;
  }

  for (i = 0u; i < object->symbol_count; i++) {
    const LinkSymbol *symbol = &object->symbols[i];
    if (symbol->is_auxiliary ||
        symbol->section_index != LINK_SECTION_INDEX_UNDEFINED ||
        !symbol->name) {
      continue;
    }
    if (strncmp(symbol->name, prefix, strlen(prefix)) == 0) {
      found = 1;
      break;
    }
  }

  free(error_message);
  link_object_destroy(object);
  return found;
}

static unsigned short read_u16_le(const unsigned char *p) {
  return (unsigned short)((unsigned)p[0] | ((unsigned)p[1] << 8));
}

static unsigned int read_u32_le(const unsigned char *p) {
  return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
         ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

static unsigned long long read_u64_le(const unsigned char *p) {
  return (unsigned long long)read_u32_le(p) |
         ((unsigned long long)read_u32_le(p + 4) << 32);
}

static int elf_object_has_undefined_symbol_prefix(const char *object_path,
                                                  const char *prefix) {
  static const unsigned char elf_magic[4] = {0x7f, 'E', 'L', 'F'};
  FILE *file = NULL;
  unsigned char *data = NULL;
  long file_size = 0;
  size_t prefix_length = 0u;
  size_t section_count = 0u;
  size_t section_offset = 0u;
  size_t section_header_size = 0u;
  size_t i = 0u;
  int found = 1;

  if (!object_path || !prefix) {
    return 1;
  }

  file = fopen(object_path, "rb");
  if (!file) {
    return 1;
  }
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return 1;
  }
  file_size = ftell(file);
  if (file_size < 64 || fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return 1;
  }
  data = malloc((size_t)file_size);
  if (!data) {
    fclose(file);
    return 1;
  }
  if (fread(data, 1, (size_t)file_size, file) != (size_t)file_size) {
    goto cleanup;
  }

  if (memcmp(data, elf_magic, sizeof(elf_magic)) != 0 || data[4] != 2 ||
      data[5] != 1) {
    goto cleanup;
  }

  section_offset = (size_t)read_u64_le(data + 0x28);
  section_header_size = (size_t)read_u16_le(data + 0x3a);
  section_count = (size_t)read_u16_le(data + 0x3c);
  if (section_header_size < 64u || section_count == 0u ||
      section_offset + section_count * section_header_size >
          (size_t)file_size) {
    goto cleanup;
  }

  prefix_length = strlen(prefix);
  found = 0;

  for (i = 0u; i < section_count; i++) {
    const unsigned char *header = data + section_offset + i * section_header_size;
    size_t symbol_table_offset = 0u;
    size_t symbol_table_size = 0u;
    size_t entry_size = 0u;
    size_t string_index = 0u;
    size_t string_offset = 0u;
    size_t string_size = 0u;
    size_t symbol = 0u;

    if (read_u32_le(header + 4) != 2u) {
      continue;
    }
    symbol_table_offset = (size_t)read_u64_le(header + 0x18);
    symbol_table_size = (size_t)read_u64_le(header + 0x20);
    string_index = (size_t)read_u32_le(header + 0x28);
    entry_size = (size_t)read_u64_le(header + 0x38);
    if (entry_size < 24u || string_index >= section_count ||
        symbol_table_offset + symbol_table_size > (size_t)file_size) {
      found = 1;
      goto cleanup;
    }

    {
      const unsigned char *string_header =
          data + section_offset + string_index * section_header_size;
      string_offset = (size_t)read_u64_le(string_header + 0x18);
      string_size = (size_t)read_u64_le(string_header + 0x20);
      if (string_offset + string_size > (size_t)file_size) {
        found = 1;
        goto cleanup;
      }
    }

    for (symbol = 0u; symbol + entry_size <= symbol_table_size;
         symbol += entry_size) {
      const unsigned char *entry = data + symbol_table_offset + symbol;
      size_t name_offset = (size_t)read_u32_le(entry);
      const char *name = NULL;

      if (read_u16_le(entry + 6) != 0u) {
        continue;
      }
      if (name_offset == 0u || name_offset >= string_size) {
        continue;
      }
      name = (const char *)(data + string_offset + name_offset);
      if (strncmp(name, prefix, prefix_length) == 0) {
        found = 1;
        goto cleanup;
      }
    }
  }

cleanup:
  free(data);
  fclose(file);
  return found;
}

static int macho_object_has_undefined_symbol_prefix(const char *object_path,
                                                    const char *prefix) {
  FILE *file = NULL;
  unsigned char *data = NULL;
  long file_size = 0;
  size_t prefix_length = 0u;
  size_t cursor = 32u;
  size_t commands_end = 0u;
  unsigned int command_count = 0u;
  unsigned int c = 0u;
  int found = 1;

  if (!object_path || !prefix) {
    return 1;
  }
  file = fopen(object_path, "rb");
  if (!file) {
    return 1;
  }
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return 1;
  }
  file_size = ftell(file);
  if (file_size < 32 || fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return 1;
  }
  data = malloc((size_t)file_size);
  if (!data) {
    fclose(file);
    return 1;
  }
  if (fread(data, 1, (size_t)file_size, file) != (size_t)file_size ||
      read_u32_le(data) != 0xfeedfacfu) {
    goto cleanup;
  }
  command_count = read_u32_le(data + 16);
  commands_end = 32u + (size_t)read_u32_le(data + 20);
  if (commands_end > (size_t)file_size) {
    goto cleanup;
  }
  prefix_length = strlen(prefix);
  for (c = 0u; c < command_count && cursor + 8u <= commands_end; c++) {
    const unsigned char *command = data + cursor;
    size_t command_size = (size_t)read_u32_le(command + 4);
    if (command_size < 8u || cursor + command_size > commands_end) {
      goto cleanup;
    }
    if (read_u32_le(command) == 0x2u && command_size >= 24u) {
      size_t symbol_offset = (size_t)read_u32_le(command + 8);
      size_t symbol_count = (size_t)read_u32_le(command + 12);
      size_t string_offset = (size_t)read_u32_le(command + 16);
      size_t string_size = (size_t)read_u32_le(command + 20);
      size_t s = 0u;
      if (symbol_offset + symbol_count * 16u > (size_t)file_size ||
          string_offset + string_size > (size_t)file_size) {
        goto cleanup;
      }
      found = 0;
      for (s = 0u; s < symbol_count; s++) {
        const unsigned char *entry = data + symbol_offset + s * 16u;
        size_t name_offset = (size_t)read_u32_le(entry);
        const char *name = NULL;
        if ((entry[4] & 0x0eu) != 0u || (entry[4] & 0x01u) == 0u ||
            name_offset == 0u || name_offset >= string_size) {
          continue;
        }
        name = (const char *)(data + string_offset + name_offset);
        if (name[0] == '_' && name_offset + 1u + prefix_length <= string_size &&
            strncmp(name + 1, prefix, prefix_length) == 0) {
          found = 1;
          goto cleanup;
        }
      }
      goto cleanup;
    }
    cursor += command_size;
  }

cleanup:
  free(data);
  fclose(file);
  return found;
}

static int object_needs_runtime_object(const char *object_path,
                                       const char *prefix) {
  if (host_target_is_macho()) {
    return macho_object_has_undefined_symbol_prefix(object_path, prefix);
  }
  if (host_target_is_elf()) {
    return elf_object_has_undefined_symbol_prefix(object_path, prefix);
  }
  return object_has_undefined_symbol_prefix(object_path, prefix);
}

static int object_needs_crash_handler(const char *object_path) {
  return object_needs_runtime_object(object_path, "mettle_crash_");
}

static int object_needs_atomics(const char *object_path) {
  return object_needs_runtime_object(object_path, "mettle_atomic_");
}

static int object_needs_parallel_runtime(const char *object_path) {
  return object_needs_runtime_object(object_path, "mettle_parallel_");
}

static int object_needs_profile_runtime(const char *object_path) {
  return object_needs_runtime_object(object_path, "mettle_profile_");
}

static int object_needs_debug_runtime(const char *object_path) {
  return object_needs_runtime_object(object_path, "mettle_dbg_");
}

static int object_needs_safety_runtime(const char *object_path) {
  return object_needs_runtime_object(object_path, "mettle_safety_");
}

static int object_needs_trace_runtime(const char *object_path) {
  return object_needs_runtime_object(object_path, "mettle_trace_");
}

static void live_write_file_list(const char *executable, const char *input) {
  char list_path[4096];
  char full[4096];
  FILE *file;
  if (!executable || !input) {
    return;
  }
  snprintf(list_path, sizeof(list_path), "%s.live.files", executable);
  file = fopen(list_path, "wb");
  if (!file) {
    return;
  }
#ifdef _WIN32
  if (GetFullPathNameA(input, (unsigned long)sizeof(full), full, NULL)) {
    fprintf(file, "%s\n", full);
  }
#else
  if (realpath(input, full)) {
    fprintf(file, "%s\n", full);
  }
#endif
  for (size_t i = 0; i < import_resolver_file_count(); i++) {
    const char *path = import_resolver_file_at(i);
#ifdef _WIN32
    if (path && GetFullPathNameA(path, (unsigned long)sizeof(full), full, NULL)) {
      fprintf(file, "%s\n", full);
    }
#else
    if (path && realpath(path, full)) {
      fprintf(file, "%s\n", full);
    }
#endif
  }
  fclose(file);
}

static int object_needs_live_runtime(const char *object_path) {
  return object_needs_runtime_object(object_path, "mettle_live_");
}

static int object_needs_swap_runtime(const char *object_path) {
  return object_needs_runtime_object(object_path, "mettle_swap_") ||
         object_needs_live_runtime(object_path);
}

static int object_needs_string_runtime(const char *object_path) {
  return object_needs_runtime_object(object_path, "mettle_string_");
}

static int object_needs_tracy_helpers(const char *object_path) {
  return object_needs_runtime_object(object_path, "mettle_tracy_");
}

#define MTLC_ELF_LINK_REPORTED 2

#define METTLE_DEFAULT_ELF_INTERPRETER "/lib64/ld-linux-x86-64.so.2"

static int mettle_elf_dynamic_link_requested(const CompilerOptions *options) {
  return options && (options->shared_library_count > 0u ||
                     options->shared_output || options->export_dynamic);
}

#ifndef _WIN32
#define METTLE_ELF_DYNAMIC_LINKER "/lib64/ld-linux-x86-64.so.2"

static char *mettle_gcc_print_file_name(const char *file) {
  char command[256];
  char line[1024];
  FILE *pipe;
  size_t len;

  if (snprintf(command, sizeof(command), "gcc -print-file-name=%s 2>/dev/null",
               file) >= (int)sizeof(command)) {
    return NULL;
  }
  pipe = popen(command, "r");
  if (!pipe) {
    return NULL;
  }
  if (!fgets(line, sizeof(line), pipe)) {
    pclose(pipe);
    return NULL;
  }
  pclose(pipe);
  len = strlen(line);
  while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
    line[--len] = '\0';
  }
  if (!strchr(line, '/')) {
    return NULL;
  }
  return strdup(line);
}

static char *mettle_sibling_path(const char *reference, const char *file) {
  const char *slash = strrchr(reference, '/');
  size_t dir_len;
  char *out;

  if (!slash) {
    return NULL;
  }
  dir_len = (size_t)(slash - reference) + 1u;
  out = malloc(dir_len + strlen(file) + 1u);
  if (!out) {
    return NULL;
  }
  memcpy(out, reference, dir_len);
  strcpy(out + dir_len, file);
  return out;
}

static int mettle_elf_keep_symbols(const CompilerOptions *options) {
  return options &&
         (options->debug_mode || options->generate_debug_symbols ||
          options->generate_line_mapping ||
          options->generate_stack_trace_support || options->tracy ||
          compiler_options_use_profile_runtime(options));
}

static int mettle_elf_external_linker_requested(const CompilerOptions *options) {
  if (!options) {
    return 0;
  }
  return options->linker_mode == LINKER_MODE_GCC ||
         options->linker_mode == LINKER_MODE_MSVC;
}

static char **mettle_resolve_shared_libraries(const CompilerOptions *options,
                                              size_t *count_out) {
  size_t count = options ? options->shared_library_count : 0u;
  char **paths = NULL;
  size_t i = 0u;

  *count_out = 0u;
  if (count == 0u) {
    return NULL;
  }
  paths = calloc(count, sizeof(char *));
  if (!paths) {
    fprintf(stderr, "Error: Out of memory while resolving libraries\n");
    return NULL;
  }
  for (i = 0u; i < count; i++) {
    const char *name = options->shared_libraries[i];
    char *error_message = NULL;

    if (strchr(name, '/') != NULL) {
      paths[i] = mettle_strdup(name);
    } else {
      paths[i] = elf_shared_library_locate(
          name, options->library_search_paths,
          options->library_search_path_count, &error_message);
    }
    if (!paths[i]) {
      fprintf(stderr, "Error: %s\n",
              error_message ? error_message : "Out of memory");
      free(error_message);
      while (i-- > 0u) {
        free(paths[i]);
      }
      free(paths);
      return NULL;
    }
    free(error_message);
  }
  *count_out = count;
  return paths;
}

static void mettle_free_shared_libraries(char **paths, size_t count) {
  size_t i = 0u;

  for (i = 0u; i < count; i++) {
    free(paths[i]);
  }
  free(paths);
}

static int mettle_link_elf_native(const char *startup_object,
                                  const char *object_filename,
                                  const char *executable_filename,
                                  const char *freestanding_object,
                                  const char *const *extra_objects,
                                  size_t extra_object_count,
                                  int strip_symbols,
                                  const CompilerOptions *options) {
  const char *object_paths[32];
  unsigned char runtime_defaults[32];
  LinkResolutionOptions resolution_options;
  ElfImageOptions emission_options;
  LinkResolution *resolution = NULL;
  char *error_message = NULL;
  char **library_paths = NULL;
  size_t library_count = 0u;
  size_t count = 0u;
  size_t i = 0u;
  int result = 1;

  memset(&resolution_options, 0, sizeof(resolution_options));
  memset(&emission_options, 0, sizeof(emission_options));
  resolution_options.entry_symbol_name = "_start";
  resolution_options.section_alignment = 16u;
  resolution_options.keep_all_sections = options && options->live;
  emission_options.image_base = 0x400000u;
  emission_options.page_size = 0x1000u;
  emission_options.interpreter = METTLE_DEFAULT_ELF_INTERPRETER;

  if (options) {
    if (options->shared_library_count != 0u) {
      library_paths = mettle_resolve_shared_libraries(options, &library_count);
      if (!library_paths) {
        return 1;
      }
    }
    resolution_options.shared_library_paths =
        (const char *const *)library_paths;
    resolution_options.shared_library_path_count = library_count;
    resolution_options.produce_shared_library = options->shared_output;
    if (options->shared_output) {
      resolution_options.entry_symbol_name = NULL;
      emission_options.produce_shared_library = 1;
      emission_options.soname = options->soname;
      emission_options.interpreter = NULL;
      emission_options.image_base = 0u;
    } else if (options->dynamic_linker) {
      emission_options.interpreter = options->dynamic_linker;
    }
    emission_options.export_dynamic = options->export_dynamic;
    emission_options.runpaths = options->runpaths;
    emission_options.runpath_count = options->runpath_count;
  }

  if (mtlc_target()->image_base_set) {
    if (mtlc_target()->image_base % 0x1000u) {
      fprintf(stderr,
              "Error: --image-base 0x%llx is not page-aligned; an ELF image "
              "must load on a 0x1000 boundary\n",
              (unsigned long long)mtlc_target()->image_base);
      mettle_free_shared_libraries(library_paths, library_count);
      return 1;
    }
    emission_options.image_base = mtlc_target()->image_base;
  }

  if ((!startup_object && !emission_options.produce_shared_library) ||
      !object_filename || !executable_filename || !freestanding_object ||
      extra_object_count > sizeof(object_paths) / sizeof(object_paths[0]) - 3u) {
    mettle_free_shared_libraries(library_paths, library_count);
    return 1;
  }

  if (startup_object) {
    object_paths[count] = startup_object;
    runtime_defaults[count++] = 1u;
  }
  object_paths[count] = freestanding_object;
  runtime_defaults[count++] = 1u;
  for (i = 0u; i < extra_object_count; i++) {
    if (!extra_objects[i]) {
      continue;
    }
    object_paths[count] = extra_objects[i];
    runtime_defaults[count++] = 1u;
  }
  object_paths[count] = object_filename;
  runtime_defaults[count++] = 0u;

  resolution_options.object_is_runtime_default = runtime_defaults;
  emission_options.strip_symbols = strip_symbols;

  if (!link_resolution_build(object_paths, count, &resolution_options,
                             &resolution, &error_message)) {
    int unresolved = error_message &&
                     strstr(error_message, "Unresolved external symbol") != NULL;
    if (unresolved) {
      fprintf(stderr, "Error: %s\n", error_message);
    } else {
      fprintf(stderr, "Error: Native ELF link failed: %s\n",
              error_message ? error_message : "symbol resolution failed");
    }
    free(error_message);
    mettle_free_shared_libraries(library_paths, library_count);
    return unresolved ? MTLC_ELF_LINK_REPORTED : 1;
  }

  if (!elf_image_emit_executable(resolution, executable_filename,
                                 &emission_options, &error_message)) {
    int unresolved = error_message &&
                     strstr(error_message, "Unresolved external symbol") != NULL;
    if (unresolved) {
      fprintf(stderr, "Error: %s\n", error_message);
    } else {
      fprintf(stderr, "Error: Native ELF link failed: %s\n",
              error_message ? error_message : "image emission failed");
    }
    free(error_message);
    link_resolution_destroy(resolution);
    mettle_free_shared_libraries(library_paths, library_count);
    return unresolved ? MTLC_ELF_LINK_REPORTED : 1;
  }

  if (options && options->live) {
    live_write_file_list(executable_filename, options->input_filename);
    if (!live_write_state(resolution, emission_options.image_base,
                          executable_filename, &error_message)) {
      fprintf(stderr, "Error: %s\n",
              error_message ? error_message : "Failed to write the live state");
      free(error_message);
      remove(executable_filename);
      link_resolution_destroy(resolution);
      mettle_free_shared_libraries(library_paths, library_count);
      return MTLC_ELF_LINK_REPORTED;
    }
  }
  free(error_message);
  link_resolution_destroy(resolution);
  mettle_free_shared_libraries(library_paths, library_count);
  result = 0;
  return result;
}

static int mettle_link_elf_direct(const char *startup_object,
                                  const char *object_filename,
                                  const char *executable_filename,
                                  const char *freestanding_object,
                                  const char *const *extra_objects,
                                  size_t extra_object_count,
                                  int strip_symbols) {
  const char *argv_list[32];
  size_t argc_used = 0u;
  size_t i = 0u;
  int result = 1;

  if (!startup_object || !object_filename || !executable_filename ||
      !freestanding_object ||
      extra_object_count > sizeof(argv_list) / sizeof(argv_list[0]) - 12u) {
    return 1;
  }

  argv_list[argc_used++] = "ld";
  argv_list[argc_used++] = "--gc-sections";
  argv_list[argc_used++] = "-z";
  argv_list[argc_used++] = "noseparate-code";
  argv_list[argc_used++] = "-e";
  argv_list[argc_used++] = "_start";
  if (strip_symbols) {
    argv_list[argc_used++] = "-s";
  }
  argv_list[argc_used++] = "-o";
  argv_list[argc_used++] = executable_filename;
  argv_list[argc_used++] = startup_object;
  argv_list[argc_used++] = freestanding_object;
  argv_list[argc_used++] = object_filename;
  for (i = 0u; i < extra_object_count; i++) {
    if (extra_objects[i]) {
      argv_list[argc_used++] = extra_objects[i];
    }
  }
  argv_list[argc_used] = NULL;

  if (mettle_run_process("ld", argv_list) == 0) {
    result = 0;
  }

  return result;
}

static void elf_select_runtime_helpers(const char *runtime_directory,
                                       const char *object_filename,
                                       int stack_trace, int profile_runtime,
                                       int needs_safety,
                                       char **crash_handler_object,
                                       char **profile_object) {
  if (stack_trace || profile_runtime || needs_safety ||
      object_needs_crash_handler(object_filename)) {
    *crash_handler_object = join_paths(runtime_directory, "crash_handler.o");
  }
  if (profile_runtime) {
    *profile_object = join_paths(runtime_directory, "profile.o");
  }
}

static int object_needs_metal_runtime(const char *object_path) {
  return object_needs_runtime_object(object_path, "mettle_metal_rt_");
}

static int elf_collect_on_demand_objects(const char *runtime_directory,
                                         const char *object_filename,
                                         int live, int shared_output,
                                         char **extra_objects,
                                         size_t *extra_object_count,
                                         size_t *on_demand_object_count) {
  static const struct {
    const char *file;
    const char *shared_file;
    int (*needed)(const char *);
  } on_demand[] = {
      {"string.o", "string.o", object_needs_string_runtime},
      {"swap.o", "swap.o", object_needs_swap_runtime},
      {"safety.o", "safety_shared.o", object_needs_safety_runtime},
      {"trace.o", "trace.o", object_needs_trace_runtime},
      {"debug.o", "debug.o", object_needs_debug_runtime},
      {"atomics.o", "atomics.o", object_needs_atomics},
      {"parallel.o", "parallel.o", object_needs_parallel_runtime},
      {"metal_runtime.o", "metal_runtime.o", object_needs_metal_runtime},
      {"metal_provider.o", "metal_provider.o", object_needs_metal_runtime},
      {"tracy_helpers.o", "tracy_helpers.o", object_needs_tracy_helpers},
      {"live.o", "live.o", object_needs_live_runtime},
  };
  size_t i = 0u;

  for (i = 0u; i < sizeof(on_demand) / sizeof(on_demand[0]); i++) {
    char *candidate = NULL;
    const char *file = shared_output ? on_demand[i].shared_file
                                     : on_demand[i].file;
    int general = on_demand[i].needed == object_needs_string_runtime ||
                  on_demand[i].needed == object_needs_swap_runtime ||
                  on_demand[i].needed == object_needs_safety_runtime ||
                  on_demand[i].needed == object_needs_atomics ||
                  on_demand[i].needed == object_needs_parallel_runtime ||
                  on_demand[i].needed == object_needs_live_runtime;
    if (!(live && general) && !on_demand[i].needed(object_filename)) {
      continue;
    }
    candidate = join_paths(runtime_directory, file);
    if (!candidate) {
      continue;
    }
    if (access(candidate, F_OK) != 0) {
      fprintf(stderr,
              "Error: Program references the %s runtime but '%s' is not in "
              "'%s'\n",
              on_demand[i].file, file, runtime_directory);
      free(candidate);
      return 0;
    }
    extra_objects[(*extra_object_count)++] = candidate;
    *on_demand_object_count = *extra_object_count;
  }
  return 1;
}

static int elf_append_runtime_helpers(int stack_trace, int profile_runtime,
                                      char *crash_handler_object,
                                      char *profile_object,
                                      char **extra_objects,
                                      size_t *extra_object_count) {
  if ((stack_trace || profile_runtime) && !crash_handler_object) {
    fprintf(stderr,
            "Error: Could not locate bundled crash_handler.o for Linux runtime "
            "support\n");
    return 0;
  }
  if (profile_runtime && !profile_object) {
    fprintf(stderr,
            "Error: Could not locate bundled profile.o for Linux runtime "
            "profiling\n");
    return 0;
  }

  if (crash_handler_object) {
    extra_objects[(*extra_object_count)++] = crash_handler_object;
  }
  if (profile_object) {
    extra_objects[(*extra_object_count)++] = profile_object;
  }
  return 1;
}

static size_t elf_cc_fixed_flags(char **argv_list, size_t used,
                                 const CompilerOptions *options,
                                 const char *cc) {
  argv_list[used++] = (char *)cc;
  argv_list[used++] = (char *)"-nostdlib";
  argv_list[used++] = (char *)"-nostartfiles";
  argv_list[used++] = (char *)"-nodefaultlibs";
  argv_list[used++] = (char *)"-no-pie";
  argv_list[used++] = (char *)"-Wl,--gc-sections";
  argv_list[used++] = (char *)"-Wl,-e,_start";
  if (!mettle_elf_keep_symbols(options)) {
    argv_list[used++] = (char *)"-s";
  }
  if (options && options->static_link) {
    argv_list[used++] = (char *)"-static";
  }
  return used;
}

static int elf_link_with_cc(const CompilerOptions *options, const char *cc,
                            const char *startup_object,
                            const char *freestanding_object,
                            const char *object_filename,
                            const char *executable_filename,
                            char **extra_objects, size_t extra_object_count) {
  size_t max_args =
      20u + 8u + 6u + 1u + (options ? options->link_argument_count : 0u);
  char **argv_list = malloc(sizeof(*argv_list) * max_args);
  size_t used = 0u;
  int linked;

  if (!argv_list) {
    fprintf(stderr, "Error: Failed to allocate ELF link argv\n");
    return 1;
  }
  used = elf_cc_fixed_flags(argv_list, used, options, cc);
  argv_list[used++] = (char *)startup_object;
  argv_list[used++] = (char *)freestanding_object;
  argv_list[used++] = (char *)object_filename;
  for (size_t i = 0u; i < extra_object_count; i++) {
    if (extra_objects[i]) {
      argv_list[used++] = extra_objects[i];
    }
  }
  argv_list[used++] = (char *)"-o";
  argv_list[used++] = (char *)executable_filename;
  if (options) {
    for (size_t i = 0; i < options->link_argument_count; i++) {
      const char *arg = options->link_arguments[i];
      if (!arg || arg[0] == '\0') {
        continue;
      }
      argv_list[used++] = (char *)arg;
    }
  }
  argv_list[used] = NULL;
  linked = mettle_run_process(cc, (const char *const *)argv_list) == 0;
  if (!linked) {
    fprintf(stderr, "Error: %s failed to produce an ELF executable\n", cc);
  }
  free(argv_list);
  return linked ? 0 : 1;
}

static int elf_prepare_runtime_objects(const CompilerOptions *options,
                                       const char *object_filename,
                                       const char *runtime_directory,
                                       int stack_trace, int profile_runtime,
                                       char **freestanding_object,
                                       char **crash_handler_object,
                                       char **profile_object,
                                       char **extra_objects,
                                       size_t *extra_object_count,
                                       size_t *on_demand_object_count) {
  int needs_safety;

  if (!runtime_directory) {
    return 1;
  }
  *freestanding_object = join_paths(runtime_directory,
                                    options && options->shared_output
                                        ? "freestanding_shared.o"
                                        : "freestanding.o");
  needs_safety = object_needs_safety_runtime(object_filename);
  elf_select_runtime_helpers(runtime_directory, object_filename,
                             stack_trace || (options && options->live),
                             profile_runtime, needs_safety,
                             crash_handler_object, profile_object);
  return elf_collect_on_demand_objects(
      runtime_directory, object_filename, options && options->live,
      options && options->shared_output,
      extra_objects, extra_object_count, on_demand_object_count);
}

static int g_link_output_ownership_verified;

static int mettle_hosted_runtime_requested(void) {
  const char *setting = getenv("METTLE_HOSTED_RUNTIME");
  if (setting && setting[0]) {
    return setting[0] != '0';
  }
#if defined(MT_HOSTED)
  return 1;
#else
  return 0;
#endif
}

static char *mettle_hosted_flag(const char *prefix, const char *value) {
  size_t prefix_length = strlen(prefix);
  size_t value_length = strlen(value);
  char *flag = malloc(prefix_length + value_length + 1u);
  if (flag) {
    memcpy(flag, prefix, prefix_length);
    memcpy(flag + prefix_length, value, value_length + 1u);
  }
  return flag;
}

static int mettle_hosted_add_flag(const char **argv_list, size_t *used,
                                  char **owned, size_t *owned_count,
                                  const char *prefix, const char *value) {
  char *flag = mettle_hosted_flag(prefix, value);
  if (!flag) {
    return 0;
  }
  owned[(*owned_count)++] = flag;
  argv_list[(*used)++] = flag;
  return 1;
}

static int mettle_hosted_add_mode_flags(const char **argv_list, size_t *used,
                                        char **owned, size_t *owned_count,
                                        const CompilerOptions *options) {
  int shared_output = options && options->shared_output;

  argv_list[(*used)++] = shared_output ? "-shared" : "-no-pie";
  argv_list[(*used)++] = "-Wl,--gc-sections";
  argv_list[(*used)++] = "-Wl,-z,noexecstack";
  if (shared_output) {
    argv_list[(*used)++] = "-Wl,-Bsymbolic";
    argv_list[(*used)++] = "-Wl,-z,notext";
  }
  if (!mettle_elf_keep_symbols(options)) {
    argv_list[(*used)++] = "-s";
  }
  if (!options) {
    return 1;
  }
  if (options->static_link) {
    argv_list[(*used)++] = "-static";
  }
  if (options->export_dynamic) {
    argv_list[(*used)++] = "-rdynamic";
  }
  if (shared_output && options->soname) {
    return mettle_hosted_add_flag(argv_list, used, owned, owned_count,
                                  "-Wl,-soname,", options->soname);
  }
  if (!shared_output && options->dynamic_linker) {
    return mettle_hosted_add_flag(argv_list, used, owned, owned_count,
                                  "-Wl,--dynamic-linker=",
                                  options->dynamic_linker);
  }
  return 1;
}

static int mettle_hosted_add_library_flags(const char **argv_list,
                                           size_t *used, char **owned,
                                           size_t *owned_count,
                                           const CompilerOptions *options) {
  size_t i;

  if (!options) {
    return 1;
  }
  for (i = 0u; i < options->library_search_path_count; i++) {
    if (!mettle_hosted_add_flag(argv_list, used, owned, owned_count, "-L",
                                options->library_search_paths[i])) {
      return 0;
    }
  }
  for (i = 0u; i < options->runpath_count; i++) {
    if (!mettle_hosted_add_flag(argv_list, used, owned, owned_count,
                                "-Wl,-rpath,", options->runpaths[i])) {
      return 0;
    }
  }
  for (i = 0u; i < options->shared_library_count; i++) {
    const char *name = options->shared_libraries[i];
    if (strchr(name, '/')) {
      argv_list[(*used)++] = name;
    } else if (!mettle_hosted_add_flag(argv_list, used, owned, owned_count,
                                       "-l", name)) {
      return 0;
    }
  }
  for (i = 0u; i < options->link_argument_count; i++) {
    const char *argument = options->link_arguments[i];
    if (argument && argument[0]) {
      argv_list[(*used)++] = argument;
    }
  }
  return 1;
}

static int mettle_link_elf_hosted(const char *object_filename,
                                  const char *executable_filename,
                                  const CompilerOptions *options,
                                  const char *runtime_directory) {
  char *crash_handler_object = NULL;
  char *profile_object = NULL;
  char *freestanding_object = NULL;
  char *extra_objects[16];
  size_t extra_object_count = 0u;
  size_t on_demand_object_count = 0u;
  const char *cc = getenv("METTLE_HOSTED_CC");
  const char **argv_list = NULL;
  char **owned = NULL;
  size_t owned_count = 0u;
  size_t used = 0u;
  size_t capacity;
  size_t i;
  int profile_runtime =
      options && (compiler_options_use_profile_runtime(options) ||
                  options->pgo_gen)
          ? 1
          : 0;
  int stack_trace = compiler_options_install_crash_handler(options);
  int result = 1;
  char reason[256];

  memset(extra_objects, 0, sizeof(extra_objects));
  if (!cc || !cc[0]) {
    cc = "cc";
  }
  if (!elf_prepare_runtime_objects(options, object_filename, runtime_directory,
                                   stack_trace, profile_runtime,
                                   &freestanding_object, &crash_handler_object,
                                   &profile_object, extra_objects,
                                   &extra_object_count,
                                   &on_demand_object_count)) {
    goto cleanup;
  }
  if (!freestanding_object || access(freestanding_object, F_OK) != 0) {
    fprintf(stderr,
            "Error: Required hosted runtime object not found in '%s'\n",
            runtime_directory ? runtime_directory : "");
    goto cleanup;
  }
  if (!elf_append_runtime_helpers(stack_trace, profile_runtime,
                                  crash_handler_object, profile_object,
                                  extra_objects, &extra_object_count)) {
    goto cleanup;
  }

  capacity = 24u + extra_object_count +
             (options ? options->link_argument_count +
                            options->shared_library_count +
                            options->library_search_path_count +
                            options->runpath_count
                      : 0u);
  argv_list = calloc(capacity, sizeof(*argv_list));
  owned = calloc(capacity, sizeof(*owned));
  if (!argv_list || !owned) {
    fprintf(stderr, "Error: Failed to allocate the hosted link command\n");
    goto cleanup;
  }

  argv_list[used++] = cc;
  if (!mettle_hosted_add_mode_flags(argv_list, &used, owned, &owned_count,
                                    options)) {
    goto cleanup;
  }
  argv_list[used++] = freestanding_object;
  argv_list[used++] = object_filename;
  for (i = 0u; i < extra_object_count; i++) {
    if (extra_objects[i]) {
      argv_list[used++] = extra_objects[i];
    }
  }
  argv_list[used++] = "-o";
  argv_list[used++] = executable_filename;
  if (!mettle_hosted_add_library_flags(argv_list, &used, owned, &owned_count,
                                       options)) {
    goto cleanup;
  }
  argv_list[used++] = "-pthread";
  argv_list[used++] = "-ldl";
  argv_list[used] = NULL;

  if (mettle_run_process(cc, argv_list) != 0) {
    fprintf(stderr, "Error: %s failed to link the hosted executable '%s'\n",
            cc, executable_filename);
    goto cleanup;
  }
  if (!mettle_verify_owned_dynamic_executable(executable_filename, reason,
                                              sizeof(reason))) {
    fprintf(stderr, "Error: Refusing linked output '%s': %s\n",
            executable_filename, reason);
    remove(executable_filename);
    goto cleanup;
  }
  g_link_output_ownership_verified = 1;
  result = 0;

cleanup:
  for (i = 0u; i < owned_count; i++) {
    free(owned[i]);
  }
  free(owned);
  free(argv_list);
  for (i = 0u; i < on_demand_object_count; i++) {
    free(extra_objects[i]);
  }
  free(crash_handler_object);
  free(profile_object);
  free(freestanding_object);
  return result;
}

static int macho_file_is_executable_image(const char *path, char *reason,
                                          size_t reason_size) {
  unsigned char header[16];
  FILE *file = fopen(path, "rb");
  size_t got;
  unsigned int filetype;
  if (!file) {
    snprintf(reason, reason_size, "cannot open the linked output");
    return 0;
  }
  got = fread(header, 1, sizeof(header), file);
  fclose(file);
  if (got != sizeof(header) || read_u32_le(header) != 0xfeedfacfu) {
    snprintf(reason, reason_size, "the linked output is not a 64-bit Mach-O");
    return 0;
  }
  filetype = read_u32_le(header + 12);
  if (filetype != 2u && filetype != 6u) {
    snprintf(reason, reason_size,
             "the linked output is Mach-O file type %u, not an executable or "
             "a dynamic library",
             filetype);
    return 0;
  }
  return 1;
}

static int mettle_link_macho_hosted(const char *object_filename,
                                    const char *executable_filename,
                                    const CompilerOptions *options,
                                    const char *runtime_directory) {
  char *crash_handler_object = NULL;
  char *profile_object = NULL;
  char *freestanding_object = NULL;
  char *extra_objects[16];
  size_t extra_object_count = 0u;
  size_t on_demand_object_count = 0u;
  const char *cc = getenv("METTLE_HOSTED_CC");
  const char **argv_list = NULL;
  char **owned = NULL;
  size_t owned_count = 0u;
  size_t used = 0u;
  size_t capacity;
  size_t i;
  int shared_output = options && options->shared_output;
  int metal = object_needs_metal_runtime(object_filename);
  int profile_runtime =
      options && (compiler_options_use_profile_runtime(options) ||
                  options->pgo_gen)
          ? 1
          : 0;
  int stack_trace = compiler_options_install_crash_handler(options);
  int result = 1;
  char reason[256];

  memset(extra_objects, 0, sizeof(extra_objects));
  if (!cc || !cc[0]) {
    cc = "cc";
  }
  if (options && options->static_link) {
    fprintf(stderr,
            "Error: macOS has no static executables; libSystem is always "
            "linked dynamically, so drop --static\n");
    return 1;
  }
  if (options && options->dynamic_linker) {
    fprintf(stderr,
            "Error: --dynamic-linker names an ELF interpreter; macOS "
            "executables always load through dyld\n");
    return 1;
  }
  if (!elf_prepare_runtime_objects(options, object_filename, runtime_directory,
                                   stack_trace, profile_runtime,
                                   &freestanding_object, &crash_handler_object,
                                   &profile_object, extra_objects,
                                   &extra_object_count,
                                   &on_demand_object_count)) {
    goto cleanup;
  }
  if (!freestanding_object || access(freestanding_object, F_OK) != 0) {
    fprintf(stderr,
            "Error: Required hosted runtime object not found in '%s'\n",
            runtime_directory ? runtime_directory : "");
    goto cleanup;
  }
  if (!elf_append_runtime_helpers(stack_trace, profile_runtime,
                                  crash_handler_object, profile_object,
                                  extra_objects, &extra_object_count)) {
    goto cleanup;
  }

  capacity = 32u + extra_object_count +
             (options ? options->link_argument_count +
                            options->shared_library_count +
                            options->library_search_path_count +
                            options->runpath_count
                      : 0u);
  argv_list = calloc(capacity, sizeof(*argv_list));
  owned = calloc(capacity, sizeof(*owned));
  if (!argv_list || !owned) {
    fprintf(stderr, "Error: Failed to allocate the macOS link command\n");
    goto cleanup;
  }

  argv_list[used++] = cc;
  argv_list[used++] = "-Wl,-dead_strip";
  if (shared_output) {
    argv_list[used++] = "-dynamiclib";
    if (options->soname &&
        !mettle_hosted_add_flag(argv_list, &used, owned, &owned_count,
                                "-Wl,-install_name,", options->soname)) {
      goto cleanup;
    }
  }
  if (!mettle_elf_keep_symbols(options)) {
    argv_list[used++] = "-Wl,-S";
  }
  if (options && options->export_dynamic) {
    argv_list[used++] = "-Wl,-export_dynamic";
  }
  argv_list[used++] = freestanding_object;
  argv_list[used++] = object_filename;
  for (i = 0u; i < extra_object_count; i++) {
    if (extra_objects[i]) {
      argv_list[used++] = extra_objects[i];
    }
  }
  argv_list[used++] = "-o";
  argv_list[used++] = executable_filename;
  if (!mettle_hosted_add_library_flags(argv_list, &used, owned, &owned_count,
                                       options)) {
    goto cleanup;
  }
  if (metal) {
    argv_list[used++] = "-framework";
    argv_list[used++] = "Metal";
    argv_list[used++] = "-framework";
    argv_list[used++] = "Foundation";
    argv_list[used++] = "-lobjc";
  }
  argv_list[used++] = "-pthread";
  argv_list[used] = NULL;

  if (mettle_run_process(cc, argv_list) != 0) {
    fprintf(stderr, "Error: %s failed to link the macOS executable '%s'\n",
            cc, executable_filename);
    goto cleanup;
  }
  if (!macho_file_is_executable_image(executable_filename, reason,
                                      sizeof(reason))) {
    fprintf(stderr, "Error: Refusing linked output '%s': %s\n",
            executable_filename, reason);
    remove(executable_filename);
    goto cleanup;
  }
  g_link_output_ownership_verified = 1;
  result = 0;

cleanup:
  for (i = 0u; i < owned_count; i++) {
    free(owned[i]);
  }
  free(owned);
  free(argv_list);
  for (i = 0u; i < on_demand_object_count; i++) {
    free(extra_objects[i]);
  }
  free(crash_handler_object);
  free(profile_object);
  free(freestanding_object);
  return result;
}

static int mettle_link_elf_executable(const char *object_filename,
                                      const char *executable_filename,
                                      const CompilerOptions *options,
                                      const char *runtime_directory) {
  char *crash_handler_object = NULL;
  char *profile_object = NULL;
  char *freestanding_object = NULL;
  char *startup_object = NULL;
  char *extra_objects[16];
  size_t extra_object_count = 0u;
  size_t on_demand_object_count = 0u;
  size_t extra_index = 0u;
  const char *cc = "gcc";
  int result = 1;
  int profile_runtime =
      options && (compiler_options_use_profile_runtime(options) ||
                  options->pgo_gen)
          ? 1
          : 0;
  int stack_trace = compiler_options_install_crash_handler(options);

  if (host_target_is_macho()) {
    return mettle_link_macho_hosted(object_filename, executable_filename,
                                    options, runtime_directory);
  }
  if (mettle_hosted_runtime_requested()) {
    return mettle_link_elf_hosted(object_filename, executable_filename,
                                  options, runtime_directory);
  }

  memset(extra_objects, 0, sizeof(extra_objects));

  if (!elf_prepare_runtime_objects(options, object_filename, runtime_directory,
                                   stack_trace, profile_runtime,
                                   &freestanding_object, &crash_handler_object,
                                   &profile_object, extra_objects,
                                   &extra_object_count,
                                   &on_demand_object_count)) {
    goto cleanup;
  }

  if (!freestanding_object || access(freestanding_object, F_OK) != 0) {
    fprintf(stderr,
            "Error: Required freestanding runtime object not found in '%s'\n",
            runtime_directory ? runtime_directory : "");
    goto cleanup;
  }
  if (!(options && options->shared_output)) {
    startup_object = replace_extension(executable_filename, ".startup.o");
    if (!startup_object ||
        binary_write_program_startup_object(
            startup_object, profile_runtime, stack_trace,
            options && options->main_wants_argc_argv ? 1 : 0) != 0) {
      fprintf(stderr, "Error: Could not generate freestanding ELF startup\n");
      goto cleanup;
    }
  }

  if (!elf_append_runtime_helpers(stack_trace, profile_runtime,
                                 crash_handler_object, profile_object,
                                 extra_objects, &extra_object_count)) {
    goto cleanup;
  }

  int native_status = 1;
  if (!(options && options->link_argument_count > 0) &&
      !mettle_elf_external_linker_requested(options)) {
    native_status = mettle_link_elf_native(
        startup_object, object_filename, executable_filename,
        freestanding_object, (const char *const *)extra_objects,
        extra_object_count, !mettle_elf_keep_symbols(options), options);
    if (native_status == 0) {
      result = 0;
    }
  }

  if (native_status == MTLC_ELF_LINK_REPORTED) {
    goto cleanup;
  }

  if (mettle_elf_dynamic_link_requested(options)) {
    if (result != 0) {
      fprintf(stderr,
              "Error: The internal ELF linker could not produce '%s'\n",
              executable_filename);
    }
    goto cleanup;
  }

  if (result != 0 && !(options && options->link_argument_count > 0) &&
      mettle_link_elf_direct(startup_object, object_filename,
                             executable_filename, freestanding_object,
                             (const char *const *)extra_objects,
                             extra_object_count,
                             !mettle_elf_keep_symbols(options)) == 0) {
    result = 0;
  }

  if (result != 0 &&
      elf_link_with_cc(options, cc, startup_object, freestanding_object,
                       object_filename, executable_filename, extra_objects,
                       extra_object_count) == 0) {
    result = 0;
  }

cleanup:
  if (startup_object) {
    unlink(startup_object);
  }
  for (extra_index = 0u; extra_index < on_demand_object_count; extra_index++) {
    free(extra_objects[extra_index]);
  }
  free(crash_handler_object);
  free(profile_object);
  free(freestanding_object);
  free(startup_object);
  return result;
}
#endif

static int g_link_output_ownership_verified;

#ifdef _WIN32
typedef struct {
  char **items;
  size_t count;
  size_t capacity;
} StringList;

static void string_list_destroy(StringList *list) {
  size_t i = 0u;

  if (!list) {
    return;
  }

  for (i = 0u; i < list->count; i++) {
    free(list->items[i]);
  }

  free(list->items);
  memset(list, 0, sizeof(*list));
}

static int string_list_contains(const StringList *list, const char *value) {
  size_t i = 0u;

  if (!list || !value) {
    return 0;
  }

  for (i = 0u; i < list->count; i++) {
    if (list->items[i] && strcmp(list->items[i], value) == 0) {
      return 1;
    }
  }

  return 0;
}

static int string_list_append_owned(StringList *list, char *value) {
  char **grown = NULL;
  size_t new_capacity = 0u;

  if (!list || !value) {
    free(value);
    return 0;
  }
  if (string_list_contains(list, value)) {
    free(value);
    return 1;
  }

  if (list->count == list->capacity) {
    new_capacity = list->capacity ? list->capacity * 2u : 4u;
    grown = realloc(list->items, new_capacity * sizeof(char *));
    if (!grown) {
      free(value);
      return 0;
    }
    list->items = grown;
    list->capacity = new_capacity;
  }

  list->items[list->count++] = value;
  return 1;
}

static int string_list_append_copy(StringList *list, const char *value) {
  char *copy = NULL;

  if (!value) {
    return 0;
  }

  copy = strdup(value);
  if (!copy) {
    return 0;
  }

  return string_list_append_owned(list, copy);
}

static int path_exists_windows(const char *path) {
  return path && path[0] != '\0' && _access(path, 0) == 0;
}

static int text_ends_with_ignore_case(const char *text, const char *suffix) {
  size_t text_length = 0u;
  size_t suffix_length = 0u;
  size_t i = 0u;

  if (!text || !suffix) {
    return 0;
  }

  text_length = strlen(text);
  suffix_length = strlen(suffix);
  if (suffix_length > text_length) {
    return 0;
  }

  for (i = 0u; i < suffix_length; i++) {
    unsigned char left =
        (unsigned char)text[text_length - suffix_length + i];
    unsigned char right = (unsigned char)suffix[i];
    if (tolower(left) != tolower(right)) {
      return 0;
    }
  }

  return 1;
}

static char *normalize_link_library_name(const char *argument,
                                         const char *extension) {
  size_t length = 0u;
  size_t extension_length = 0u;
  char *normalized = NULL;

  if (!argument || !extension) {
    return NULL;
  }

  length = strlen(argument);
  extension_length = strlen(extension);
  if (text_ends_with_ignore_case(argument, extension)) {
    return strdup(argument);
  }

  normalized = malloc(length + extension_length + 1u);
  if (!normalized) {
    return NULL;
  }

  memcpy(normalized, argument, length);
  memcpy(normalized + length, extension, extension_length + 1u);
  return normalized;
}

static int resolve_import_library_path(const char *library_name,
                                       const StringList *search_directories,
                                       StringList *resolved_paths) {
  char *candidate = NULL;
  char *env_copy = NULL;
  char *token = NULL;

  if (!library_name || !resolved_paths) {
    return 0;
  }

  if (strchr(library_name, '\\') || strchr(library_name, '/') ||
      strchr(library_name, ':') || path_exists_windows(library_name)) {
    return string_list_append_copy(resolved_paths, library_name);
  }

  if (search_directories) {
    size_t i = 0u;
    for (i = 0u; i < search_directories->count; i++) {
      candidate = join_paths(search_directories->items[i], library_name);
      if (!candidate) {
        return 0;
      }
      if (path_exists_windows(candidate)) {
        return string_list_append_owned(resolved_paths, candidate);
      }
      free(candidate);
      candidate = NULL;
    }
  }

  const char *lib_env = getenv("LIB");
  env_copy = lib_env ? strdup(lib_env) : NULL;
  token = env_copy ? strtok(env_copy, ";") : NULL;
  while (token) {
    candidate = join_paths(token, library_name);
    if (!candidate) {
      free(env_copy);
      return 0;
    }
    if (path_exists_windows(candidate)) {
      free(env_copy);
      return string_list_append_owned(resolved_paths, candidate);
    }
    free(candidate);
    candidate = NULL;
    token = strtok(NULL, ";");
  }
  free(env_copy);

  return string_list_append_copy(resolved_paths, library_name);
}

static int collect_internal_link_imports(const CompilerOptions *options,
                                          int include_shell32,
                                          StringList *import_library_paths,
                                          StringList *import_dll_names,
                                          char **error_message_out) {
  static const char *default_import_dlls[] = {
      "kernel32.dll", "ws2_32.dll", "user32.dll",
      "gdi32.dll",    "advapi32.dll", "winmm.dll"};
  size_t i = 0u;
  StringList search_directories = {0};

  if (error_message_out) {
    *error_message_out = NULL;
  }
  if (!import_library_paths || !import_dll_names) {
    return 0;
  }

  for (i = 0u; i < sizeof(default_import_dlls) / sizeof(default_import_dlls[0]);
       i++) {
    if (!string_list_append_copy(import_dll_names, default_import_dlls[i])) {
      if (error_message_out) {
        *error_message_out =
            strdup("Out of memory while preparing internal linker defaults");
      }
      string_list_destroy(&search_directories);
      return 0;
    }
  }

  if (include_shell32) {
    if (!string_list_append_copy(import_dll_names, "shell32.dll")) {
      if (error_message_out) {
        *error_message_out =
            strdup("Out of memory while preparing internal linker defaults");
      }
      string_list_destroy(&search_directories);
      return 0;
    }
  }

  if (options && options->tracy) {
    static const char *tracy_import_dlls[] = {"secur32.dll", "dbghelp.dll"};
    for (i = 0u; i < sizeof(tracy_import_dlls) / sizeof(tracy_import_dlls[0]); i++) {
      if (!string_list_append_copy(import_dll_names, tracy_import_dlls[i])) {
        if (error_message_out) {
          *error_message_out =
              strdup("Out of memory while preparing Tracy linker imports");
        }
        string_list_destroy(&search_directories);
        return 0;
      }
    }
  }

  if (!options) {
    string_list_destroy(&search_directories);
    return 1;
  }

  for (i = 0u; i < options->link_argument_count; i++) {
    const char *argument = options->link_arguments[i];

    if (!argument || argument[0] == '\0') {
      continue;
    }
    if (strncmp(argument, "-L", 2) == 0 && argument[2] != '\0') {
      if (!string_list_append_copy(&search_directories, argument + 2)) {
        if (error_message_out) {
          *error_message_out = strdup("Out of memory while storing internal linker search directories");
        }
        string_list_destroy(&search_directories);
        return 0;
      }
      continue;
    }
  }

  for (i = 0u; i < options->link_argument_count; i++) {
    const char *argument = options->link_arguments[i];
    char *normalized = NULL;

    if (!argument || argument[0] == '\0') {
      continue;
    }
    if (strncmp(argument, "-L", 2) == 0 && argument[2] != '\0') {
      continue;
    }
    if (strncmp(argument, "-l", 2) == 0 && argument[2] != '\0') {
      normalized = normalize_link_library_name(argument + 2u, ".dll");
      if (!normalized) {
        if (error_message_out) {
          *error_message_out = strdup("Out of memory while preparing internal linker DLL imports");
        }
        string_list_destroy(&search_directories);
        return 0;
      }
      if (!string_list_append_owned(import_dll_names, normalized)) {
        if (error_message_out) {
          *error_message_out = strdup("Out of memory while preparing internal linker DLL imports");
        }
        string_list_destroy(&search_directories);
        return 0;
      }
      continue;
    }
    if (text_ends_with_ignore_case(argument, ".lib")) {
      if (!resolve_import_library_path(argument, &search_directories,
                                       import_library_paths)) {
        if (error_message_out) {
          *error_message_out = strdup("Out of memory while preparing internal linker import libraries");
        }
        string_list_destroy(&search_directories);
        return 0;
      }
    }
  }

  string_list_destroy(&search_directories);
  return 1;
}

static int append_internal_link_object_args(const CompilerOptions *options,
                                            const char **object_paths,
                                            size_t object_capacity,
                                            size_t *object_count) {
  size_t i = 0u;
  if (!options || !object_paths || !object_count) {
    return 1;
  }

  for (i = 0u; i < options->link_argument_count; i++) {
    const char *argument = options->link_arguments[i];
    if (!argument || argument[0] == '\0') {
      continue;
    }
    if (!text_ends_with_ignore_case(argument, ".o") &&
        !text_ends_with_ignore_case(argument, ".obj")) {
      continue;
    }
    if (*object_count >= object_capacity) {
      return 0;
    }
    object_paths[(*object_count)++] = argument;
  }

  return 1;
}

static int compiler_options_use_tracy(const CompilerOptions *options) {
  return options && options->tracy;
}

static int append_argument_text(char *buffer, size_t buffer_size, size_t *offset,
                                const char *text) {
  if (!buffer || !offset || !text) {
    return 0;
  }

  size_t text_len = strlen(text);
  if (*offset + text_len >= buffer_size) {
    return 0;
  }

  memcpy(buffer + *offset, text, text_len);
  *offset += text_len;
  buffer[*offset] = '\0';
  return 1;
}

static int append_quoted_argument(char *buffer, size_t buffer_size,
                                  size_t *offset, const char *argument) {
  if (!append_argument_text(buffer, buffer_size, offset, "\"")) {
    return 0;
  }
  if (!append_argument_text(buffer, buffer_size, offset, argument)) {
    return 0;
  }
  return append_argument_text(buffer, buffer_size, offset, "\"");
}

static int append_msvc_link_argument(char *buffer, size_t buffer_size,
                                     size_t *offset, const char *argument) {
  if (!argument || argument[0] == '\0') {
    return 1;
  }

  if (strncmp(argument, "-l", 2) == 0 && argument[2] != '\0') {
    if (!append_argument_text(buffer, buffer_size, offset, " ")) {
      return 0;
    }
    if (!append_argument_text(buffer, buffer_size, offset, argument + 2)) {
      return 0;
    }
    return append_argument_text(buffer, buffer_size, offset, ".lib");
  }

  if (strncmp(argument, "-L", 2) == 0 && argument[2] != '\0') {
    if (!append_argument_text(buffer, buffer_size, offset, " /LIBPATH:\"")) {
      return 0;
    }
    if (!append_argument_text(buffer, buffer_size, offset, argument + 2)) {
      return 0;
    }
    return append_argument_text(buffer, buffer_size, offset, "\"");
  }

  if (!append_argument_text(buffer, buffer_size, offset, " ")) {
    return 0;
  }
  return append_argument_text(buffer, buffer_size, offset, argument);
}

static int append_msvc_link_arguments(char *buffer, size_t buffer_size,
                                      size_t *offset,
                                      const CompilerOptions *options) {
  if (!options) {
    return 1;
  }

  for (size_t i = 0; i < options->link_argument_count; i++) {
    if (!append_msvc_link_argument(buffer, buffer_size, offset,
                                   options->link_arguments[i])) {
      return 0;
    }
  }

  return 1;
}

static int run_system_command(const char *command) {
  if (!command || command[0] == '\0') {
    return 0;
  }
  return system(command);
}

static int windows_tool_exists(const char *tool_name) {
  return mettle_find_executable(tool_name);
}

static int write_internal_startup_object(const char *path, int profile_runtime,
                                         int stack_trace_init,
                                         int main_wants_argc_argv) {
  return binary_write_program_startup_object(path, profile_runtime,
                                             stack_trace_init,
                                             main_wants_argc_argv);
}

static int mettle_link_internal(const char **object_paths,
                                  const unsigned char *object_is_runtime_default,
                                  size_t object_count,
                                  const char *executable_filename,
                                  int include_shell32,
                                  const CompilerOptions *options) {
  int want_shared =
      options && options->shared_output ? 1 : 0;
  LinkResolutionOptions resolution_options = {
      .entry_symbol_name = want_shared ? NULL : "mettle_start",
      .section_alignment = 16u,
      .allow_unresolved_externals = 1,
      .object_is_runtime_default = object_is_runtime_default,
      .keep_all_sections = options && options->live};
  LinkResolution *resolution = NULL;
  PeEmissionOptions emission_options = {0};
  StringList import_library_paths = {0};
  StringList import_dll_names = {0};
  char *error_message = NULL;
  int result = 1;

  if (!object_paths || object_count == 0u || !executable_filename) {
    fprintf(stderr, "Error: Missing inputs for internal linker\n");
    return 1;
  }

  if (!collect_internal_link_imports(options, include_shell32,
                                     &import_library_paths, &import_dll_names,
                                     &error_message)) {
    fprintf(stderr, "Error: %s\n",
            error_message ? error_message
                          : "Failed to prepare internal linker imports");
    free(error_message);
    string_list_destroy(&import_library_paths);
    string_list_destroy(&import_dll_names);
    return 1;
  }

  if (!link_resolution_build(object_paths, object_count, &resolution_options,
                             &resolution, &error_message)) {
    if (error_message &&
        strstr(error_message, "Unresolved external symbol") != NULL) {
      fprintf(stderr, "Error: %s\n", error_message);
    } else {
      fprintf(stderr, "Error: Internal linker symbol resolution failed: %s\n",
              error_message ? error_message : "unknown error");
    }
    goto cleanup;
  }

  if (options && options->windows_subsystem) {
    emission_options.subsystem = 2u;
  }
  if (mtlc_target()->image_base_set) {
    if (mtlc_target()->image_base % 0x10000u) {
      fprintf(stderr,
              "Error: --image-base 0x%llx is not 64K-aligned; a PE image must "
              "load on a 0x10000 boundary\n",
              (unsigned long long)mtlc_target()->image_base);
      goto cleanup;
    }
    emission_options.image_base = mtlc_target()->image_base;
  }
  emission_options.import_library_paths =
      (const char **)import_library_paths.items;
  emission_options.import_library_count = import_library_paths.count;
  emission_options.import_dll_names = (const char **)import_dll_names.items;
  emission_options.import_dll_count = import_dll_names.count;
  if (want_shared) {
    emission_options.produce_shared_library = 1;
    emission_options.dll_name =
        options && options->soname ? options->soname : NULL;
  }
  if (!pe_emit_executable(resolution, executable_filename, &emission_options,
                          &error_message)) {
    if (error_message &&
        strstr(error_message, "Unresolved external symbol") != NULL) {
      fprintf(stderr, "Error: %s\n", error_message);
    } else {
      fprintf(stderr, "Error: Internal linker PE emission failed: %s\n",
              error_message ? error_message : "unknown error");
    }
    goto cleanup;
  }
  if (options && options->live) {
    live_write_file_list(executable_filename, options->input_filename);
  }
  if (options && options->live &&
      !live_write_state(resolution,
                        emission_options.image_base
                            ? emission_options.image_base
                            : 0x140000000ull,
                        executable_filename, &error_message)) {
    fprintf(stderr, "Error: %s\n",
            error_message ? error_message : "Failed to write the live state");
    remove(executable_filename);
    goto cleanup;
  }

  result = 0;

cleanup:
  free(error_message);
  string_list_destroy(&import_library_paths);
  string_list_destroy(&import_dll_names);
  link_resolution_destroy(resolution);
  return result;
}

static int mettle_link_object_with_gcc(const char *object_filename,
                                        const char *executable_filename,
                                        const char *const *runtime_objects,
                                        size_t runtime_object_count,
                                        const CompilerOptions *options) {
  size_t link_argument_count = options ? options->link_argument_count : 0u;
  size_t capacity = 24u + runtime_object_count + link_argument_count;
  const char **arguments = calloc(capacity, sizeof(*arguments));
  size_t count = 0u;
  int result;
  if (!arguments) {
    fprintf(stderr, "Error: Failed to allocate GCC arguments\n");
    return 1;
  }

  arguments[count++] = "gcc";
  arguments[count++] = "-nostdlib";
  arguments[count++] = "-nostartfiles";
  arguments[count++] = "-nodefaultlibs";
  arguments[count++] = "-Wl,--disable-runtime-pseudo-reloc";
  arguments[count++] = "-Wl,-e,mettle_start,--gc-sections";
  if (options && options->windows_subsystem) {
    arguments[count++] = "-Wl,--subsystem,windows";
  }
  arguments[count++] = object_filename;
  for (size_t i = 0; i < runtime_object_count; i++) {
    if (runtime_objects[i] && runtime_objects[i][0]) {
      arguments[count++] = runtime_objects[i];
    }
  }
  arguments[count++] = "-o";
  arguments[count++] = executable_filename;
  arguments[count++] = "-lkernel32";
  arguments[count++] = "-luser32";
  arguments[count++] = "-lgdi32";
  arguments[count++] = "-ladvapi32";
  arguments[count++] = "-lws2_32";
  arguments[count++] = "-lwinmm";
  if (options) {
    for (size_t i = 0; i < options->link_argument_count; i++) {
      if (options->link_arguments[i] && options->link_arguments[i][0]) {
        arguments[count++] = options->link_arguments[i];
      }
    }
  }
  arguments[count] = NULL;

  result = mettle_run_process("gcc", arguments);
  free(arguments);
  if (result != 0) {
    fprintf(stderr, "Warning: GCC object link step failed\n");
    return 1;
  }
  return 0;
}

static int mettle_link_object_with_link(const char *object_filename,
                                          const char *executable_filename,
                                          const char *const *runtime_objects,
                                          size_t runtime_object_count,
                                          const CompilerOptions *options) {
  size_t link_len = strlen(object_filename) + strlen(executable_filename) + 320;
  for (size_t i = 0; i < runtime_object_count; i++) {
    if (runtime_objects[i] && runtime_objects[i][0] != '\0') {
      link_len += strlen(runtime_objects[i]) + 16;
    }
  }
  if (options) {
    for (size_t i = 0; i < options->link_argument_count; i++) {
      if (options->link_arguments[i]) {
        link_len += strlen(options->link_arguments[i]) + 16;
      }
    }
  }

  char *link_command = malloc(link_len);
  if (!link_command) {
    fprintf(stderr, "Error: Failed to allocate MSVC link command\n");
    return 1;
  }

  size_t offset = 0;
  if (!append_argument_text(
          link_command, link_len, &offset,
          (options && options->windows_subsystem)
              ? "link.exe /nologo /nodefaultlib /entry:mettle_start "
                "/subsystem:windows /out:"
              : "link.exe /nologo /nodefaultlib /entry:mettle_start "
                "/subsystem:console /out:") ||
      !append_quoted_argument(link_command, link_len, &offset,
                              executable_filename) ||
      !append_argument_text(link_command, link_len, &offset, " ") ||
      !append_quoted_argument(link_command, link_len, &offset, object_filename)) {
    free(link_command);
    fprintf(stderr, "Error: Failed to build MSVC object link command\n");
    return 1;
  }
  for (size_t i = 0; i < runtime_object_count; i++) {
    if (!runtime_objects[i] || runtime_objects[i][0] == '\0') {
      continue;
    }
    if (!append_argument_text(link_command, link_len, &offset, " ") ||
        !append_quoted_argument(link_command, link_len, &offset,
                                runtime_objects[i])) {
      free(link_command);
      fprintf(stderr, "Error: Failed to build MSVC object link command\n");
      return 1;
    }
  }
  if (!append_argument_text(link_command, link_len, &offset,
                             " kernel32.lib") ||
      !append_msvc_link_arguments(link_command, link_len, &offset, options)) {
    free(link_command);
    fprintf(stderr, "Error: Failed to build MSVC object link command\n");
    return 1;
  }

  int result = run_system_command(link_command);
  free(link_command);
  if (result != 0) {
    fprintf(stderr, "Warning: MSVC object link step failed\n");
    return 1;
  }
  return 0;
}

typedef enum {
  RUNTIME_OBJECT_FREESTANDING,
  RUNTIME_OBJECT_CRASH,
  RUNTIME_OBJECT_ATOMICS,
  RUNTIME_OBJECT_PROFILE,
  RUNTIME_OBJECT_DEBUG,
  RUNTIME_OBJECT_SAFETY,
  RUNTIME_OBJECT_TRACE,
  RUNTIME_OBJECT_SWAP,
  RUNTIME_OBJECT_LIVE,
  RUNTIME_OBJECT_STRING,
  RUNTIME_OBJECT_TRACY_HELPERS,
  RUNTIME_OBJECT_PARALLEL,
  RUNTIME_OBJECT_COUNT
} RuntimeObjectKind;

typedef struct {
  const char *stem;
  const char *label;
  int (*needed)(const char *);
} RuntimeObjectSpec;

static const RuntimeObjectSpec RUNTIME_OBJECT_SPECS[RUNTIME_OBJECT_COUNT] = {
    [RUNTIME_OBJECT_FREESTANDING] = {"freestanding", "freestanding", NULL},
    [RUNTIME_OBJECT_CRASH] = {"crash_handler", "crash-handler",
                              object_needs_crash_handler},
    [RUNTIME_OBJECT_ATOMICS] = {"atomics", "atomics", object_needs_atomics},
    [RUNTIME_OBJECT_PROFILE] = {"profile", "profile",
                                object_needs_profile_runtime},
    [RUNTIME_OBJECT_DEBUG] = {"debug", "debug", object_needs_debug_runtime},
    [RUNTIME_OBJECT_SAFETY] = {"safety", "safety", object_needs_safety_runtime},
    [RUNTIME_OBJECT_TRACE] = {"trace", "trace", object_needs_trace_runtime},
    [RUNTIME_OBJECT_SWAP] = {"swap", "swap", object_needs_swap_runtime},
    [RUNTIME_OBJECT_LIVE] = {"live", "live", object_needs_live_runtime},
    [RUNTIME_OBJECT_STRING] = {"string", "string", object_needs_string_runtime},
    [RUNTIME_OBJECT_PARALLEL] = {"parallel", "parallel",
                                 object_needs_parallel_runtime},
    [RUNTIME_OBJECT_TRACY_HELPERS] = {"tracy_helpers", "Tracy helpers",
                                      object_needs_tracy_helpers},
};

typedef struct {
  const char *object_filename;
  const char *executable_filename;
  const char *runtime_directory;
  const CompilerOptions *options;
  LinkerMode linker_mode;
  int has_gcc;
  int has_link;
  int want_shared;
  int profile_runtime;
  int needed[RUNTIME_OBJECT_COUNT];
  char *gcc_path[RUNTIME_OBJECT_COUNT];
  char *msvc_path[RUNTIME_OBJECT_COUNT];
  const char *freestanding_object;
} LinkPlan;

typedef enum {
  LINK_OBJECT_ANY,
  LINK_OBJECT_SKIP_MISSING,
  LINK_OBJECT_REQUIRED
} LinkObjectPolicy;

typedef struct {
  RuntimeObjectKind kind;
  int gcc_only;
  LinkObjectPolicy policy;
} LinkObjectRequest;

typedef struct {
  const char **paths;
  unsigned char *is_default;
  size_t count;
} LinkObjectList;

static const LinkObjectRequest LINK_INTERNAL_REQUESTS[] = {
    {RUNTIME_OBJECT_CRASH, 0, LINK_OBJECT_ANY},
    {RUNTIME_OBJECT_ATOMICS, 0, LINK_OBJECT_ANY},
    {RUNTIME_OBJECT_PROFILE, 0, LINK_OBJECT_REQUIRED},
    {RUNTIME_OBJECT_DEBUG, 0, LINK_OBJECT_REQUIRED},
    {RUNTIME_OBJECT_SAFETY, 0, LINK_OBJECT_REQUIRED},
    {RUNTIME_OBJECT_TRACE, 0, LINK_OBJECT_SKIP_MISSING},
    {RUNTIME_OBJECT_SWAP, 0, LINK_OBJECT_ANY},
    {RUNTIME_OBJECT_LIVE, 0, LINK_OBJECT_REQUIRED},
    {RUNTIME_OBJECT_STRING, 0, LINK_OBJECT_ANY},
    {RUNTIME_OBJECT_TRACY_HELPERS, 0, LINK_OBJECT_ANY},
    {RUNTIME_OBJECT_PARALLEL, 0, LINK_OBJECT_ANY},
};

static const LinkObjectRequest LINK_GCC_REQUESTS[] = {
    {RUNTIME_OBJECT_FREESTANDING, 1, LINK_OBJECT_ANY},
    {RUNTIME_OBJECT_CRASH, 1, LINK_OBJECT_REQUIRED},
    {RUNTIME_OBJECT_ATOMICS, 1, LINK_OBJECT_REQUIRED},
    {RUNTIME_OBJECT_PROFILE, 1, LINK_OBJECT_REQUIRED},
    {RUNTIME_OBJECT_STRING, 0, LINK_OBJECT_ANY},
    {RUNTIME_OBJECT_SWAP, 0, LINK_OBJECT_REQUIRED},
    {RUNTIME_OBJECT_LIVE, 0, LINK_OBJECT_REQUIRED},
    {RUNTIME_OBJECT_SAFETY, 1, LINK_OBJECT_REQUIRED},
    {RUNTIME_OBJECT_DEBUG, 1, LINK_OBJECT_REQUIRED},
    {RUNTIME_OBJECT_TRACY_HELPERS, 0, LINK_OBJECT_ANY},
    {RUNTIME_OBJECT_PARALLEL, 0, LINK_OBJECT_ANY},
};

static const LinkObjectRequest LINK_MSVC_REQUESTS[] = {
    {RUNTIME_OBJECT_FREESTANDING, 0, LINK_OBJECT_ANY},
    {RUNTIME_OBJECT_CRASH, 0, LINK_OBJECT_REQUIRED},
    {RUNTIME_OBJECT_ATOMICS, 0, LINK_OBJECT_REQUIRED},
    {RUNTIME_OBJECT_PROFILE, 0, LINK_OBJECT_REQUIRED},
    {RUNTIME_OBJECT_STRING, 0, LINK_OBJECT_ANY},
    {RUNTIME_OBJECT_SWAP, 0, LINK_OBJECT_REQUIRED},
    {RUNTIME_OBJECT_LIVE, 0, LINK_OBJECT_REQUIRED},
    {RUNTIME_OBJECT_TRACE, 0, LINK_OBJECT_SKIP_MISSING},
    {RUNTIME_OBJECT_SAFETY, 0, LINK_OBJECT_REQUIRED},
    {RUNTIME_OBJECT_DEBUG, 0, LINK_OBJECT_REQUIRED},
    {RUNTIME_OBJECT_TRACY_HELPERS, 0, LINK_OBJECT_ANY},
    {RUNTIME_OBJECT_PARALLEL, 0, LINK_OBJECT_ANY},
};

static const char *link_plan_pick(const LinkPlan *plan,
                                  RuntimeObjectKind kind) {
  return (_access(plan->msvc_path[kind], 0) == 0) ? plan->msvc_path[kind]
                                                  : plan->gcc_path[kind];
}

static void link_plan_release(LinkPlan *plan) {
  for (int i = 0; i < RUNTIME_OBJECT_COUNT; i++) {
    free(plan->gcc_path[i]);
    free(plan->msvc_path[i]);
  }
}

static int link_plan_build_paths(LinkPlan *plan) {
  for (int i = 0; i < RUNTIME_OBJECT_COUNT; i++) {
    char filename[128];
    snprintf(filename, sizeof(filename), "%s.o", RUNTIME_OBJECT_SPECS[i].stem);
    plan->gcc_path[i] = join_paths(plan->runtime_directory, filename);
    snprintf(filename, sizeof(filename), "%s.obj",
             RUNTIME_OBJECT_SPECS[i].stem);
    plan->msvc_path[i] = join_paths(plan->runtime_directory, filename);
    if (!plan->gcc_path[i] || !plan->msvc_path[i]) {
      return 0;
    }
  }
  return 1;
}

static void link_plan_survey(LinkPlan *plan) {
  for (int i = 0; i < RUNTIME_OBJECT_COUNT; i++) {
    plan->needed[i] = RUNTIME_OBJECT_SPECS[i].needed
                          ? RUNTIME_OBJECT_SPECS[i].needed(
                                plan->object_filename)
                          : 1;
  }
  if (plan->profile_runtime) {
    plan->needed[RUNTIME_OBJECT_PROFILE] = 1;
  }
  if (plan->options && plan->options->debug_hooks) {
    plan->needed[RUNTIME_OBJECT_DEBUG] = 1;
  }
  if (plan->needed[RUNTIME_OBJECT_LIVE]) {
    plan->needed[RUNTIME_OBJECT_ATOMICS] = 1;
    plan->needed[RUNTIME_OBJECT_SAFETY] = 1;
    plan->needed[RUNTIME_OBJECT_SWAP] = 1;
    plan->needed[RUNTIME_OBJECT_STRING] = 1;
    plan->needed[RUNTIME_OBJECT_PARALLEL] = 1;
  }
  if (plan->needed[RUNTIME_OBJECT_PROFILE] ||
      plan->needed[RUNTIME_OBJECT_SAFETY] ||
      plan->needed[RUNTIME_OBJECT_LIVE]) {
    plan->needed[RUNTIME_OBJECT_CRASH] = 1;
  }
}

static int link_plan_validate(LinkPlan *plan) {
  if (!plan->object_filename || !plan->executable_filename ||
      !plan->runtime_directory) {
    fprintf(stderr, "Error: Missing build inputs for executable generation\n");
    return 0;
  }
  if (plan->want_shared && plan->linker_mode != LINKER_MODE_INTERNAL &&
      plan->linker_mode != LINKER_MODE_AUTO) {
    fprintf(stderr,
            "Error: DLL emission requires --linker internal; external linkers "
            "cannot emit a Mettle DLL\n");
    return 0;
  }
  plan->has_gcc = (plan->linker_mode == LINKER_MODE_AUTO ||
                   plan->linker_mode == LINKER_MODE_GCC)
                      ? windows_tool_exists("gcc")
                      : 0;
  plan->has_link = (plan->linker_mode == LINKER_MODE_AUTO ||
                    plan->linker_mode == LINKER_MODE_MSVC)
                       ? windows_tool_exists("link.exe")
                       : 0;
  if (plan->linker_mode == LINKER_MODE_GCC && !plan->has_gcc) {
    fprintf(stderr,
            "Error: gcc was requested with --linker gcc but was not found.\n");
    return 0;
  }
  if (plan->linker_mode == LINKER_MODE_MSVC && !plan->has_link) {
    fprintf(stderr,
            "Error: link.exe was requested with --linker msvc but was not found.\n");
    return 0;
  }
  return 1;
}

static int link_plan_prepare(LinkPlan *plan) {
  if (!link_plan_build_paths(plan)) {
    fprintf(stderr, "Error: Failed to allocate build paths\n");
    return 0;
  }
  link_plan_survey(plan);
  if (compiler_options_use_tracy(plan->options)) {
    fprintf(stderr,
            "Error: --tracy cannot use the external TracyClient in owned "
            "runtime mode because it requires a C++ runtime. Use "
            "--profile-runtime instead.\n");
    return 0;
  }
  plan->freestanding_object = link_plan_pick(plan, RUNTIME_OBJECT_FREESTANDING);
  if (_access(plan->freestanding_object, 0) != 0) {
    fprintf(stderr,
            "Error: Required freestanding runtime object not found in '%s'\n",
            plan->runtime_directory);
    return 0;
  }
  if (plan->needed[RUNTIME_OBJECT_TRACY_HELPERS] &&
      _access(link_plan_pick(plan, RUNTIME_OBJECT_TRACY_HELPERS), 0) != 0) {
    fprintf(stderr,
            "Error: Program references Tracy helpers but bundled stub "
            "object not found in '%s'\n",
            plan->runtime_directory);
    return 0;
  }
  return 1;
}

static int link_collect_objects(const LinkPlan *plan,
                                const LinkObjectRequest *requests,
                                size_t request_count, LinkObjectList *list) {
  for (size_t i = 0u; i < request_count; i++) {
    const LinkObjectRequest *request = &requests[i];
    const char *path = NULL;
    if (!plan->needed[request->kind]) {
      continue;
    }
    path = request->gcc_only ? plan->gcc_path[request->kind]
                             : link_plan_pick(plan, request->kind);
    if (request->policy != LINK_OBJECT_ANY && _access(path, 0) != 0) {
      if (request->policy == LINK_OBJECT_SKIP_MISSING) {
        continue;
      }
      fprintf(stderr, "Error: Bundled %s runtime object not found in '%s'\n",
              RUNTIME_OBJECT_SPECS[request->kind].label,
              plan->runtime_directory);
      return 0;
    }
    if (list->is_default) {
      list->is_default[list->count] = 1u;
    }
    list->paths[list->count++] = path;
  }
  return 1;
}

static int link_internal_prepare_startup(const LinkPlan *plan,
                                         char **startup_object) {
  int fatal = plan->linker_mode == LINKER_MODE_INTERNAL ||
              (!plan->has_gcc && !plan->has_link);
  if (plan->want_shared) {
    return 1;
  }
  *startup_object =
      replace_extension(plan->executable_filename, ".startup.obj");
  if (!*startup_object) {
    fputs(fatal ? "Error: Failed to allocate internal-linker startup object "
                  "path\n"
                : "Warning: Failed to allocate internal-linker startup object "
                  "path, falling back to external linkers\n",
          stderr);
    return fatal ? -1 : 0;
  }
  if (write_internal_startup_object(
          *startup_object, plan->profile_runtime,
          compiler_options_install_crash_handler(plan->options),
          plan->options && plan->options->main_wants_argc_argv ? 1 : 0) != 0) {
    fputs(fatal ? "Error: Failed to generate internal-linker startup object\n"
                : "Warning: Failed to generate internal-linker startup object, "
                  "falling back to external linkers\n",
          stderr);
    return fatal ? -1 : 0;
  }
  return 1;
}

static void link_internal_report(const LinkPlan *plan, int linked) {
  if (linked || plan->linker_mode == LINKER_MODE_INTERNAL) {
    return;
  }
  if (!plan->has_gcc && !plan->has_link) {
    fprintf(stderr,
            "Error: Internal linker failed and no external fallback linker is "
            "available.\n");
    return;
  }
  fprintf(stderr,
          "Warning: Internal linker failed in auto mode, falling back to "
          "external linkers\n");
}

static int link_internal_assemble(const LinkPlan *plan, LinkObjectList *list,
                                  size_t capacity, char *startup_object) {
  if (!plan->want_shared) {
    list->paths[list->count++] = startup_object;
  }
  list->is_default[list->count] = 1u;
  list->paths[list->count++] = plan->freestanding_object;
  list->paths[list->count++] = plan->object_filename;
  if (!link_collect_objects(plan, LINK_INTERNAL_REQUESTS,
                            sizeof(LINK_INTERNAL_REQUESTS) /
                                sizeof(LINK_INTERNAL_REQUESTS[0]),
                            list)) {
    return 0;
  }
  if (!append_internal_link_object_args(plan->options, list->paths, capacity,
                                        &list->count)) {
    fprintf(stderr, "Error: Too many internal-linker object arguments\n");
    return 0;
  }
  return 1;
}

static int link_build_internal(const LinkPlan *plan, int *build_result) {
  size_t capacity = RUNTIME_OBJECT_COUNT + 2u +
                    (plan->options ? plan->options->link_argument_count : 0u);
  const char **paths = calloc(capacity, sizeof(const char *));
  unsigned char *is_default = calloc(capacity, 1u);
  char *startup_object = NULL;
  LinkObjectList list = {NULL, NULL, 0u};
  int ready = 0;
  int stop = 1;

  if (!paths || !is_default) {
    fprintf(stderr, "Error: Failed to allocate internal-linker object list\n");
    goto done;
  }
  ready = link_internal_prepare_startup(plan, &startup_object);
  if (ready < 0) {
    goto done;
  }
  list.paths = paths;
  list.is_default = is_default;
  if (ready > 0) {
    int linked = 0;
    if (!link_internal_assemble(plan, &list, capacity, startup_object)) {
      goto done;
    }
    linked = mettle_link_internal(paths, is_default, list.count,
                                  plan->executable_filename, 0,
                                  plan->options) == 0;
    if (linked) {
      *build_result = 0;
      g_link_output_ownership_verified = 1;
    }
    link_internal_report(plan, linked);
  }
  stop = plan->want_shared || *build_result == 0 ||
         plan->linker_mode == LINKER_MODE_INTERNAL ||
         (!plan->has_gcc && !plan->has_link);

done:
  if (startup_object) {
    if (ready > 0) {
      _unlink(startup_object);
    }
    free(startup_object);
  }
  free(paths);
  free(is_default);
  return stop;
}

typedef int (*ExternalLinkFn)(const char *, const char *, const char *const *,
                              size_t, const CompilerOptions *);

static int link_build_external(const LinkPlan *plan,
                               const LinkObjectRequest *requests,
                               size_t request_count,
                               const char *startup_object,
                               ExternalLinkFn link_fn) {
  const char *objects[RUNTIME_OBJECT_COUNT + 1u] = {0};
  LinkObjectList list = {objects, NULL, 0u};
  list.paths[list.count++] = startup_object;
  if (!link_collect_objects(plan, requests, request_count, &list)) {
    return -1;
  }
  return link_fn(plan->object_filename, plan->executable_filename, objects,
                 list.count, plan->options) == 0;
}

static int mettle_link_object_file(const char *object_filename,
                                   const char *executable_filename,
                                   const char *runtime_directory,
                                   const CompilerOptions *options) {
  LinkPlan plan = {0};
  char *external_startup_object = NULL;
  int build_result = 1;

  plan.object_filename = object_filename;
  plan.executable_filename = executable_filename;
  plan.runtime_directory = runtime_directory;
  plan.options = options;
  plan.linker_mode = options ? options->linker_mode : LINKER_MODE_AUTO;
  plan.want_shared = options && options->shared_output ? 1 : 0;
  plan.profile_runtime =
      options && (compiler_options_use_profile_runtime(options) ||
                  options->pgo_gen)
          ? 1
          : 0;

  if (!link_plan_validate(&plan) || !link_plan_prepare(&plan)) {
    goto cleanup;
  }
  if ((plan.linker_mode == LINKER_MODE_INTERNAL ||
       plan.linker_mode == LINKER_MODE_AUTO) &&
      link_build_internal(&plan, &build_result)) {
    goto cleanup;
  }
  if ((plan.has_gcc && plan.linker_mode != LINKER_MODE_MSVC) ||
      (plan.has_link && plan.linker_mode != LINKER_MODE_GCC)) {
    external_startup_object =
        replace_extension(executable_filename, ".external-startup.obj");
    if (!external_startup_object ||
        write_internal_startup_object(
            external_startup_object, plan.profile_runtime,
            compiler_options_install_crash_handler(options),
            options && options->main_wants_argc_argv ? 1 : 0) != 0) {
      fprintf(stderr,
              "Error: Failed to generate external-linker startup object\n");
      goto cleanup;
    }
  }
  if (plan.has_gcc && plan.linker_mode != LINKER_MODE_MSVC) {
    int linked = link_build_external(
        &plan, LINK_GCC_REQUESTS,
        sizeof(LINK_GCC_REQUESTS) / sizeof(LINK_GCC_REQUESTS[0]),
        external_startup_object, mettle_link_object_with_gcc);
    if (linked > 0) {
      build_result = 0;
    }
    if (linked != 0) {
      goto cleanup;
    }
  }
  if (plan.has_link && plan.linker_mode != LINKER_MODE_GCC) {
    int linked = link_build_external(
        &plan, LINK_MSVC_REQUESTS,
        sizeof(LINK_MSVC_REQUESTS) / sizeof(LINK_MSVC_REQUESTS[0]),
        external_startup_object, mettle_link_object_with_link);
    if (linked > 0) {
      build_result = 0;
    }
    if (linked != 0) {
      goto cleanup;
    }
  }
  fprintf(stderr,
          "Error: Failed to link executable with the available linker backends\n");

cleanup:
  if (external_startup_object) {
    _unlink(external_startup_object);
  }
  free(external_startup_object);
  link_plan_release(&plan);
  return build_result;
}
#endif

static int add_import_directory(CompilerOptions *options, const char *path) {
  if (!options || !path || path[0] == '\0') {
    return 0;
  }

  size_t next_count = options->import_directory_count + 1;
  const char **grown = realloc((void *)options->import_directories,
                               next_count * sizeof(const char *));
  if (!grown) {
    return 0;
  }

  grown[options->import_directory_count] = path;
  options->import_directories = grown;
  options->import_directory_count = next_count;
  return 1;
}

static int add_string_option(const char ***list, size_t *count,
                             const char *value) {
  const char **grown = NULL;

  if (!value || value[0] == '\0') {
    return 0;
  }
  grown = realloc((void *)*list, (*count + 1u) * sizeof(const char *));
  if (!grown) {
    return 0;
  }
  grown[*count] = value;
  *list = grown;
  *count += 1u;
  return 1;
}

static int add_link_argument(CompilerOptions *options, const char *argument) {
  if (!options || !argument || argument[0] == '\0') {
    return 0;
  }

  size_t next_count = options->link_argument_count + 1;
  const char **grown = realloc((void *)options->link_arguments,
                               next_count * sizeof(const char *));
  if (!grown) {
    return 0;
  }

  grown[options->link_argument_count] = argument;
  options->link_arguments = grown;
  options->link_argument_count = next_count;
  return 1;
}

static int detect_gpu_sm_count(void) {
  const GpuDetectResult *local = gpu_detect_local();
  if (!local->available || local->device_count <= 0) return 0;
  int count = local->devices[0].multiprocessor_count;
  return count > 0 && count < 100000 ? count : 0;
}

static long long ptxas_bytes_before(const char *line, const char *label) {
  const char *found = strstr(line, label);
  if (!found) return -1;
  const char *cursor = found;
  while (cursor > line && cursor[-1] == ' ') cursor--;
  const char *end = cursor;
  while (cursor > line && cursor[-1] >= '0' && cursor[-1] <= '9') cursor--;
  if (cursor == end) return -1;
  return atoll(cursor);
}

static int function_holds_tiles(const IRFunction *function) {
  for (size_t i = 0; function && i < function->instruction_count; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (in->op == IR_OP_ADDRESS_SPACE_ALLOC &&
        ir_tile_operand_is_tile(in->value_type)) {
      return 1;
    }
  }
  return 0;
}

static int program_tile_kernel(const IRProgram *program, const char *name) {
  for (size_t i = 0; program && name && i < program->function_count; i++) {
    const IRFunction *function = program->functions[i];
    if (function && function->name && strcmp(function->name, name) == 0) {
      return function_holds_tiles(function);
    }
  }
  return 0;
}

static int confirm_tile_residency(const IRProgram *program,
                                  const char *ptx_path, const char *arch,
                                  const char *source) {
  char cubin[512];
  char command[1200];
  char line[512];
  char entry[256] = {0};
  int any = 0;
  int ok = 1;
  for (size_t i = 0; program && i < program->function_count; i++) {
    if (program->functions[i] && program->functions[i]->is_kernel &&
        function_holds_tiles(program->functions[i])) {
      any = 1;
    }
  }
  if (!any) {
    return 1;
  }
  if (!gpu_detect_ptxas_version()) {
    fprintf(stderr,
            "note: the register tiles in '%s' are unconfirmed: ptxas is not "
            "on PATH, so nothing checked that the assembler kept them in "
            "registers\n",
            source ? source : "?");
    return 1;
  }
  char real_arch[64];
  snprintf(real_arch, sizeof(real_arch), "%s", arch ? arch : "sm_121a");
  if (strncmp(real_arch, "compute_", 8) == 0) {
    snprintf(real_arch, sizeof(real_arch), "sm_%s", (arch ? arch : "") + 8);
  }
  snprintf(cubin, sizeof(cubin), "%s.tiles.cubin", ptx_path);
  snprintf(command, sizeof(command), "ptxas -v -arch=%s \"%s\" -o \"%s\" 2>&1",
           real_arch, ptx_path, cubin);
#ifdef _WIN32
  FILE *pipe = _popen(command, "r");
#else
  FILE *pipe = popen(command, "r");
#endif
  if (!pipe) {
    fprintf(stderr,
            "note: the register tiles in '%s' are unconfirmed: ptxas could "
            "not be started\n",
            source ? source : "?");
    return 1;
  }
  while (fgets(line, sizeof(line), pipe)) {
    char name[256];
    if (sscanf(line, " ptxas info : Compiling entry function '%255[^']'",
               name) == 1) {
      snprintf(entry, sizeof(entry), "%s", name);
      continue;
    }
    long long stores = ptxas_bytes_before(line, "bytes spill stores");
    if (stores >= 0 && entry[0] && program_tile_kernel(program, entry)) {
      long long loads = ptxas_bytes_before(line, "bytes spill loads");
      if (stores > 0 || loads > 0) {
        fprintf(stderr,
                "error[G0002]: ptxas spilled kernel '%s' (%lld bytes of spill "
                "stores, %lld of spill loads) and it holds register tiles; "
                "tiles never spill, so shrink a tile or what is live beside it\n"
                "  --> %s\n",
                entry, stores, loads > 0 ? loads : 0, source ? source : "?");
        ok = 0;
      }
    }
  }
#ifdef _WIN32
  _pclose(pipe);
#else
  pclose(pipe);
#endif
  remove(cubin);
  return ok;
}

static void report_ptx_occupancy(const IRProgram *program,
                                 const char *ptx_path, const char *arch,
                                 int sm_count, int sm_count_is_local) {
  char cubin[512];
  char command[1200];
  snprintf(cubin, sizeof(cubin), "%s.occupancy.cubin", ptx_path);
  snprintf(command, sizeof(command), "ptxas -v -arch=%s \"%s\" -o \"%s\" 2>&1",
           arch ? arch : "sm_121a", ptx_path, cubin);
#ifdef _WIN32
  FILE *pipe = _popen(command, "r");
#else
  FILE *pipe = popen(command, "r");
#endif
  if (!pipe) {
    fprintf(stderr, "--report-occupancy: could not run ptxas\n");
    return;
  }
  printf("Occupancy report (%s; upper bound: 64K regs/SM, 48 warps/SM, "
         "100KB smem/SM, allocation unit 1",
         arch ? arch : "sm_121a");
  if (sm_count > 0) {
    printf("; %d SMs, %s", sm_count, sm_count_is_local ? "local GPU" : "--sms");
  }
  printf("):\n");
  char line[512];
  char entry[256] = {0};
  long long entry_smem = 0;
  long long spill_stores = 0;
  long long spill_loads = 0;
  long long stack_frame = 0;
  int reported = 0;
  int any_spill = 0;
  while (fgets(line, sizeof(line), pipe)) {
    char name[256];
    if (sscanf(line, " ptxas info : Compiling entry function '%255[^']'",
               name) == 1) {
      snprintf(entry, sizeof(entry), "%s", name);
      entry_smem = 0;
      spill_stores = 0;
      spill_loads = 0;
      stack_frame = 0;
      continue;
    }
    long long stores = ptxas_bytes_before(line, "bytes spill stores");
    if (stores >= 0) {
      long long loads = ptxas_bytes_before(line, "bytes spill loads");
      long long frame = ptxas_bytes_before(line, "bytes stack frame");
      spill_stores = stores;
      spill_loads = loads > 0 ? loads : 0;
      stack_frame = frame > 0 ? frame : 0;
      continue;
    }
    const char *used = strstr(line, "Used ");
    if (!used || !entry[0]) {
      continue;
    }
    int registers = 0;
    if (sscanf(used, "Used %d registers", &registers) != 1) {
      continue;
    }
    long long smem = ptxas_bytes_before(line, "bytes smem");
    if (smem >= 0) {
      entry_smem = smem;
    }
    long long register_warp_limit =
        registers > 0 ? 65536ll / ((long long)registers * 32) : 48;
    if (register_warp_limit > 48) register_warp_limit = 48;

    int block = 0;
    for (size_t f = 0; program && f < program->function_count; f++) {
      const IRFunction *function = program->functions[f];
      if (function && function->is_kernel && function->name &&
          strcmp(function->name, entry) == 0 && function->kernel_block[0] > 0) {
        block = function->kernel_block[0] *
                (function->kernel_block[1] > 0 ? function->kernel_block[1] : 1) *
                (function->kernel_block[2] > 0 ? function->kernel_block[2] : 1);
        break;
      }
    }

    long long warps = register_warp_limit;
    const char *limiter = registers > 0 && register_warp_limit < 48
                              ? ", register-limited"
                              : "";
    if (block > 0) {
      long long warps_per_block = (block + 31) / 32;
      long long blocks = warps_per_block > 0
                             ? register_warp_limit / warps_per_block
                             : 0;
      if (blocks > 24) blocks = 24;
      if (entry_smem > 0) {
        long long smem_blocks = 102400ll / entry_smem;
        if (smem_blocks < blocks) {
          blocks = smem_blocks;
          limiter = ", shared-memory-limited";
        }
      }
      warps = blocks * warps_per_block;
      if (warps > 48) warps = 48;
      if (warps < register_warp_limit && !*limiter) limiter = ", block-limited";
      printf("  %s: %d registers, block %d (%lld warps/block, %lld blocks) -> "
             "%lld/48 resident warps (%lld%%)%s",
             entry, registers, block, warps_per_block, blocks, warps,
             warps * 100 / 48, limiter);
      if (sm_count > 0 && blocks > 0) {
        printf("; full card = %lld blocks (%d SMs x %lld)",
               (long long)sm_count * blocks, sm_count, blocks);
      }
    } else {
      printf("  %s: %d registers -> %lld/48 resident warps (%lld%%)%s",
             entry, registers, warps, warps * 100 / 48, limiter);
      if (sm_count > 0 && warps > 0) {
        printf("; full card = %lld warps (%d SMs x %lld)",
               (long long)sm_count * warps, sm_count, warps);
      }
    }
    if (spill_stores > 0 || spill_loads > 0) {
      printf("; SPILLS %lld bytes stored, %lld loaded", spill_stores,
             spill_loads);
      any_spill = 1;
    } else if (stack_frame > 0) {
      printf("; %lld byte stack frame", stack_frame);
    }
    printf("\n");
    reported = 1;
    entry[0] = '\0';
  }
#ifdef _WIN32
  int status = _pclose(pipe);
#else
  int status = pclose(pipe);
#endif
  remove(cubin);
  if (any_spill) {
    printf("  note: a spilling kernel pays a local-memory round trip per "
           "spilled access; that costs more than the residency above.\n");
  }
  if (!reported) {
    fprintf(stderr,
            "--report-occupancy: no ptxas resource report (is ptxas on PATH "
            "and the target '%s' supported?); ptxas exit %d\n",
            arch ? arch : "sm_121a", status);
  }
}

static int detect_host_gpu_ptx_target(char *out, size_t out_size) {
  return gpu_detect_ptx_target(0, out, out_size);
}

static size_t kernel_decl_base_type(const char *spelling, char *out,
                                    size_t capacity) {
  if (!spelling || !out || capacity == 0) return 0;
  size_t n = 0;
  while (spelling[n] && spelling[n] != '*' && spelling[n] != '[' &&
         spelling[n] != ' ' && n + 1 < capacity) {
    out[n] = spelling[n];
    n++;
  }
  out[n] = 0;
  return n;
}

static void kernel_decl_mark_record(Program *prog, char *wanted,
                                    const char *name) {
  if (!prog || !wanted || !name || !*name) return;
  for (size_t i = 0; i < prog->declaration_count; i++) {
    ASTNode *decl = prog->declarations[i];
    if (!decl || decl->type != AST_STRUCT_DECLARATION || !decl->data) continue;
    StructDeclaration *record = (StructDeclaration *)decl->data;
    if (!record->name || strcmp(record->name, name) != 0 || wanted[i]) continue;
    wanted[i] = 1;
    for (size_t f = 0; f < record->field_count; f++) {
      char base[128];
      if (record->field_types &&
          kernel_decl_base_type(record->field_types[f], base, sizeof(base))) {
        kernel_decl_mark_record(prog, wanted, base);
      }
    }
    return;
  }
}

static int write_kernel_declarations(ASTNode *program, const char *path,
                                     const char *source_name) {
  if (!program || program->type != AST_PROGRAM || !program->data) return 0;
  Program *prog = (Program *)program->data;

  FILE *out = fopen(path, "w");
  if (!out) {
    fprintf(stderr, "Error: could not open '%s' for the kernel declarations\n",
            path);
    return 0;
  }
  fprintf(out,
          "// Generated by `mettle --emit-kernel-decls` from %s.\n"
          "// Import this from the host so `dispatch` checks every launch\n"
          "// against the kernel as it was actually compiled. Do not edit:\n"
          "// re-emit it whenever the kernels change.\n\n",
          source_name ? source_name : "a GPU module");

  char *wanted = prog->declaration_count
                     ? (char *)calloc(prog->declaration_count, 1)
                     : NULL;
  if (prog->declaration_count && !wanted) {
    fclose(out);
    fprintf(stderr, "Error: out of memory writing the kernel declarations\n");
    return 0;
  }
  for (size_t i = 0; i < prog->declaration_count; i++) {
    ASTNode *decl = prog->declarations[i];
    if (!decl || decl->type != AST_FUNCTION_DECLARATION || !decl->data) continue;
    FunctionDeclaration *fn = (FunctionDeclaration *)decl->data;
    if (!fn->is_kernel || fn->is_extern) continue;
    for (size_t p = 0; p < fn->parameter_count; p++) {
      char base[128];
      if (fn->parameter_types &&
          kernel_decl_base_type(fn->parameter_types[p], base, sizeof(base))) {
        kernel_decl_mark_record(prog, wanted, base);
      }
    }
  }
  size_t records = 0;
  for (size_t i = 0; i < prog->declaration_count; i++) {
    if (!wanted || !wanted[i]) continue;
    StructDeclaration *record = (StructDeclaration *)prog->declarations[i]->data;
    fprintf(out, "struct %s {\n", record->name ? record->name : "record");
    for (size_t f = 0; f < record->field_count; f++) {
      fprintf(out, "  %s: %s;\n",
              record->field_names && record->field_names[f]
                  ? record->field_names[f]
                  : "field",
              record->field_types && record->field_types[f]
                  ? record->field_types[f]
                  : "int64");
    }
    fprintf(out, "}\n\n");
    records++;
  }
  free(wanted);

  size_t written = 0;
  for (size_t i = 0; i < prog->declaration_count; i++) {
    ASTNode *decl = prog->declarations[i];
    if (!decl || decl->type != AST_FUNCTION_DECLARATION || !decl->data) {
      continue;
    }
    FunctionDeclaration *fn = (FunctionDeclaration *)decl->data;
    if (!fn->is_kernel || !fn->name || fn->is_extern) continue;

    fprintf(out, "extern kernel");
    if (fn->kernel_block[0] > 0) {
      if (fn->kernel_block[1] > 1 || fn->kernel_block[2] > 1) {
        fprintf(out, "(block = (%d, %d, %d)", fn->kernel_block[0],
                fn->kernel_block[1] > 0 ? fn->kernel_block[1] : 1,
                fn->kernel_block[2] > 0 ? fn->kernel_block[2] : 1);
      } else {
        fprintf(out, "(block = %d", fn->kernel_block[0]);
      }
      if (fn->kernel_threads_per_item > 1) {
        fprintf(out, ", per = warp");
      }
      fprintf(out, ")");
    }
    fprintf(out, " %s(", fn->name);
    for (size_t p = 0; p < fn->parameter_count; p++) {
      const char *name = fn->parameter_names ? fn->parameter_names[p] : NULL;
      const char *type = fn->parameter_types ? fn->parameter_types[p] : NULL;
      fprintf(out, "%s%s: %s", p ? ", " : "", name ? name : "arg",
              type ? type : "int64");
    }
    fprintf(out, ");\n");
    written++;
  }
  fclose(out);
  if (records) {
    printf("Generated kernel declarations: %s (%zu kernel%s, %zu record%s)\n",
           path, written, written == 1 ? "" : "s", records,
           records == 1 ? "" : "s");
  } else {
    printf("Generated kernel declarations: %s (%zu kernel%s)\n", path, written,
           written == 1 ? "" : "s");
  }
  return 1;
}

static void gpu_info_format_memory(long long bytes, char *out, size_t out_size) {
  if (bytes <= 0) {
    snprintf(out, out_size, "unknown");
    return;
  }
  long long tenths = (bytes * 10 + (1LL << 29)) / (1LL << 30);
  snprintf(out, out_size, "%lld.%lld GiB", tenths / 10, tenths % 10);
}

static void report_metal_info(const GpuMetalDetect *metal) {
  char memory[32];
  gpu_info_format_memory(metal->working_set, memory, sizeof(memory));
  printf("  Metal device      %s\n", metal->name[0] ? metal->name : "(unnamed)");
  printf("      Metal 3              %s\n",
         metal->metal3 ? "yes" : "no (Mettle kernels need Metal 3)");
  printf("      threadgroup memory   %d KiB\n", metal->threadgroup_memory / 1024);
  printf("      working set          %s%s\n", memory,
         metal->unified_memory ? " (unified with host)" : "");
  printf("  --emit-metal builds kernels for it; the runtime compiles them at\n"
         "  load time (Metal 3.2 by default).\n");
}

static int report_gpu_info(const char *default_target, int isa_major,
                           int isa_minor) {
  const GpuDetectResult *local = gpu_detect_local();
  const GpuMetalDetect *metal = gpu_detect_metal();
  printf("Mettle GPU target report\n");
  if (metal->available) {
    report_metal_info(metal);
    if (!local->available) {
      return metal->metal3 ? 0 : 1;
    }
  }

  if (!local->available) {
    printf("  Local devices     none (%s)\n", local->source);
    printf("  Default target    %s, PTX ISA %d.%d (cross-compile default)\n",
           default_target, isa_major, isa_minor);
    const char *ptxas_version = gpu_detect_ptxas_version();
    printf("  Assembler         %s%s\n", ptxas_version ? "ptxas " : "",
           ptxas_version ? ptxas_version : "ptxas not on PATH");
    printf("\n  --emit-ptx still works: PTX is text the driver compiles at\n"
           "  load time, so kernels can be built here and run elsewhere.\n");
    return 1;
  }

  if (local->driver_version > 0) {
    printf("  Driver            CUDA %d.%d (%s)\n", local->driver_version / 1000,
           (local->driver_version % 1000) / 10, local->source);
  } else {
    printf("  Driver            %s\n", local->source);
  }
  printf("  Devices           %d\n", local->device_count);
  for (int i = 0; i < local->device_count; i++) {
    const GpuDetectDevice *device = &local->devices[i];
    char target[32];
    char memory[32];
    if (!gpu_detect_ptx_target(i, target, sizeof(target))) {
      snprintf(target, sizeof(target), "unknown");
    }
    gpu_info_format_memory(device->total_memory, memory, sizeof(memory));
    printf("  [%d] %s\n", i, device->name[0] ? device->name : "(unnamed)");
    printf("      compute capability   %d.%d  ->  %s\n", device->compute_major,
           device->compute_minor, target);
    if (device->multiprocessor_count > 0) {
      printf("      multiprocessors      %d\n", device->multiprocessor_count);
    }
    if (device->warp_size > 0) {
      printf("      warp size            %d\n", device->warp_size);
    }
    if (device->max_threads_per_block > 0) {
      printf("      max threads / block  %d\n", device->max_threads_per_block);
    }
    if (device->max_shared_memory_per_block > 0) {
      printf("      shared mem / block   %d KiB\n",
             device->max_shared_memory_per_block / 1024);
    }
    printf("      global memory        %s%s\n", memory,
           device->integrated ? " (unified with host)" : "");
  }

  const char *ptxas_version = gpu_detect_ptxas_version();
  char selected[32];
  int have_selected = gpu_detect_ptx_target(0, selected, sizeof(selected));
  if (ptxas_version) {
    printf("  Assembler         ptxas %s", ptxas_version);
    if (have_selected) {
      printf(" (%s %s)", selected,
             gpu_detect_ptxas_supports(selected) ? "supported"
                                                 : "NOT supported");
    }
    printf("\n");
  } else {
    printf("  Assembler         ptxas not on PATH (only --report-occupancy "
           "needs it)\n");
  }
  printf("  Default target    %s, PTX ISA %d.%d\n",
         have_selected ? selected : default_target, isa_major, isa_minor);
  printf("\n  Build kernels for this machine with:\n"
         "    mettle --emit-ptx kernels.mettle -o kernels.ptx\n"
         "  Override the target with --gpu-arch=sm_NN, --gpu-arch=native, or\n"
         "  --gpu-arch=portable to build PTX that runs on older cards.\n");
  return 0;
}

typedef struct {
  int build_executable;
  int linker_mode_explicit;
  int output_filename_explicit;
  int ptx_version_explicit;
  int gpu_arch_explicit;
  char detected_ptx_target[16];
} DriverFlags;

typedef enum {
  DRIVER_FLAG_UNMATCHED = 0,
  DRIVER_FLAG_TAKEN,
  DRIVER_FLAG_FAILED
} DriverFlagResult;

static DriverFlagResult parse_flag_shared_library(CompilerOptions *options,
                                                  int argc, char *argv[],
                                                  int *index) {
  int i = *index;
  (void)argc;

  if (strncmp(argv[i], "-l", 2) == 0 && argv[i][2] != '\0') {
    if (!add_string_option(&options->shared_libraries,
                           &options->shared_library_count, argv[i] + 2)) {
      fprintf(stderr, "Error: Failed to add library '%s'\n", argv[i] + 2);
      return DRIVER_FLAG_FAILED;
    }
  } else if (strcmp(argv[i], "--library") == 0 && i + 1 < argc) {
    if (!add_string_option(&options->shared_libraries,
                           &options->shared_library_count, argv[++i])) {
      fprintf(stderr, "Error: Failed to add library '%s'\n", argv[i]);
      return DRIVER_FLAG_FAILED;
    }
  } else if (strncmp(argv[i], "-L", 2) == 0 && argv[i][2] != '\0') {
    if (!add_string_option(&options->library_search_paths,
                           &options->library_search_path_count, argv[i] + 2)) {
      fprintf(stderr, "Error: Failed to add library path '%s'\n", argv[i] + 2);
      return DRIVER_FLAG_FAILED;
    }
  } else if ((strcmp(argv[i], "--library-path") == 0 ||
              strcmp(argv[i], "-L") == 0) &&
             i + 1 < argc) {
    if (!add_string_option(&options->library_search_paths,
                           &options->library_search_path_count, argv[++i])) {
      fprintf(stderr, "Error: Failed to add library path '%s'\n", argv[i]);
      return DRIVER_FLAG_FAILED;
    }
  } else if (strcmp(argv[i], "--rpath") == 0 && i + 1 < argc) {
    if (!add_string_option(&options->runpaths, &options->runpath_count,
                           argv[++i])) {
      fprintf(stderr, "Error: Failed to add rpath '%s'\n", argv[i]);
      return DRIVER_FLAG_FAILED;
    }
  } else if (strcmp(argv[i], "--shared") == 0) {
    options->shared_output = 1;
  } else if (strcmp(argv[i], "--export-dynamic") == 0 ||
             strcmp(argv[i], "-rdynamic") == 0) {
    options->export_dynamic = 1;
  } else if (strcmp(argv[i], "--soname") == 0 && i + 1 < argc) {
    options->soname = argv[++i];
  } else if (strcmp(argv[i], "--live") == 0) {
    options->live = 1;
  } else if (strcmp(argv[i], "--live-patch-all") == 0) {
    options->live_patch_all = 1;
  } else if (strcmp(argv[i], "--live-at-tick") == 0 && i + 1 < argc) {
    options->live_at_tick = strtoll(argv[++i], NULL, 10);
  } else if (strcmp(argv[i], "--live-patch") == 0 && i + 1 < argc) {
    options->live = 1;
    options->live_patch_target = argv[++i];
  } else if (strcmp(argv[i], "--dynamic-linker") == 0 && i + 1 < argc) {
    options->dynamic_linker = argv[++i];
  } else if (strcmp(argv[i], "--soname") == 0 ||
             strcmp(argv[i], "--live-patch") == 0 ||
             strcmp(argv[i], "--live-at-tick") == 0 ||
             strcmp(argv[i], "--dynamic-linker") == 0 ||
             strcmp(argv[i], "--rpath") == 0 ||
             strcmp(argv[i], "--library") == 0 ||
             strcmp(argv[i], "--library-path") == 0) {
    fprintf(stderr, "Error: Missing value after '%s'\n", argv[i]);
    return DRIVER_FLAG_FAILED;
  } else {
    return DRIVER_FLAG_UNMATCHED;
  }
  *index = i;
  return DRIVER_FLAG_TAKEN;
}

static DriverFlagResult parse_flag_output(CompilerOptions *options,
                                     DriverFlags *flags,
                                     int argc, char *argv[],
                                     int *index) {
  int i = *index;
  (void)argc;
  (void)flags;
  if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
    options->input_filename = argv[++i];
  } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
    options->output_filename = argv[++i];
    flags->output_filename_explicit = 1;
  } else if (strcmp(argv[i], "-I") == 0) {
    if (i + 1 >= argc) {
      fprintf(stderr, "Error: Missing import directory after '-I'\n");
      return DRIVER_FLAG_FAILED;
    }
    if (!add_import_directory(options, argv[++i])) {
      fprintf(stderr, "Error: Failed to add import directory\n");
      return DRIVER_FLAG_FAILED;
    }
  } else if (strncmp(argv[i], "-I", 2) == 0 && argv[i][2] != '\0') {
    if (!add_import_directory(options, argv[i] + 2)) {
      fprintf(stderr, "Error: Failed to add import directory\n");
      return DRIVER_FLAG_FAILED;
    }
  } else if (strcmp(argv[i], "--stdlib") == 0 && i + 1 < argc) {
    options->stdlib_directory = argv[++i];
  } else if (strcmp(argv[i], "--build") == 0) {
    flags->build_executable = 1;
  } else if (strcmp(argv[i], "--emit-asm") == 0) {
    fprintf(stderr,
            "Error: --emit-asm has been removed; Mettle only emits native "
            "objects now.\n");
    return DRIVER_FLAG_FAILED;
  } else if (strcmp(argv[i], "--emit-obj") == 0) {
    options->emit_object = 1;
  } else if (strcmp(argv[i], "--linker") == 0 && i + 1 < argc) {
    flags->linker_mode_explicit = 1;
    if (!parse_linker_mode(argv[++i], &options->linker_mode)) {
      fprintf(stderr,
              "Error: Unknown linker mode '%s' (expected auto, internal, gcc, or msvc)\n",
              argv[i]);
      return DRIVER_FLAG_FAILED;
    }
  } else if (strcmp(argv[i], "--linker") == 0) {
    fprintf(stderr, "Error: Missing linker mode after '--linker'\n");
    return DRIVER_FLAG_FAILED;
  } else if (strcmp(argv[i], "--subsystem") == 0 && i + 1 < argc) {
    const char *name = argv[++i];
    if (strcmp(name, "windows") == 0 || strcmp(name, "gui") == 0) {
      options->windows_subsystem = 1;
    } else if (strcmp(name, "console") == 0) {
      options->windows_subsystem = 0;
    } else {
      fprintf(stderr,
              "Error: Unknown subsystem '%s' (expected console or windows)\n",
              name);
      return DRIVER_FLAG_FAILED;
    }
  } else if (strcmp(argv[i], "--subsystem") == 0) {
    fprintf(stderr, "Error: Missing subsystem after '--subsystem'\n");
    return DRIVER_FLAG_FAILED;
  } else if (strcmp(argv[i], "--link-arg") == 0 && i + 1 < argc) {
    if (!add_link_argument(options, argv[++i])) {
      fprintf(stderr, "Error: Failed to add linker argument\n");
      return DRIVER_FLAG_FAILED;
    }
  } else {
    DriverFlagResult shared =
        parse_flag_shared_library(options, argc, argv, &i);
    if (shared == DRIVER_FLAG_UNMATCHED) {
      return DRIVER_FLAG_UNMATCHED;
    }
    if (shared == DRIVER_FLAG_FAILED) {
      return DRIVER_FLAG_FAILED;
    }
  }
  *index = i;
  return DRIVER_FLAG_TAKEN;
}

static DriverFlagResult parse_flag_diagnostics(CompilerOptions *options,
                                     DriverFlags *flags,
                                     int argc, char *argv[],
                                     int *index) {
  int i = *index;
  (void)argc;
  (void)flags;
  if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--debug") == 0) {
    options->debug_mode = 1;
    options->generate_debug_symbols = 1;
    options->generate_line_mapping = 1;
    options->generate_stack_trace_support = 1;
  } else if (strcmp(argv[i], "--dump-ast") == 0) {
    options->dump_ast = 1;
  } else if (strcmp(argv[i], "--dump-ir") == 0) {
    options->dump_ir = 1;
  } else if (strcmp(argv[i], "--ml-opt") == 0) {
    options->ml_opt = 1;
    options->optimize = 1;
  } else if (strcmp(argv[i], "--ml-opt-speculative") == 0) {
    options->ml_opt = 1;
    options->optimize = 1;
    putenv("METTLE_ML_SPECULATIVE=1");
  } else if (strncmp(argv[i], "--error-format=", 15) == 0) {
    const char *fmt = argv[i] + 15;
    if (strcmp(fmt, "json") == 0) {
      error_reporter_set_format_json(1);
    } else if (strcmp(fmt, "human") == 0) {
      error_reporter_set_format_json(0);
    } else {
      fprintf(stderr,
              "Error: Unknown error format '%s' (expected human or json)\n",
              fmt);
      return DRIVER_FLAG_FAILED;
    }
  } else if (strncmp(argv[i], "--filter=", 9) == 0) {
    options->test_filter = argv[i] + 9;
  } else if (strcmp(argv[i], "--pgo") == 0) {
    options->pgo = 1;
    options->optimize = 1;
  } else if (strcmp(argv[i], "--assume-no-signed-overflow") == 0) {
    options->assume_no_signed_overflow = 1;
  } else if (strcmp(argv[i], "--pgo-gen") == 0) {
    options->pgo_gen = 1;
    options->optimize = 1;
  } else if (strncmp(argv[i], "--pgo-use=", 10) == 0) {
    options->pgo_use = argv[i] + 10;
    options->optimize = 1;
  } else if (strcmp(argv[i], "--verify") == 0) {
    const char *trusted = NULL;
    if (mettle_trust_mode_active(&trusted)) {
      fprintf(stderr,
              "error: --verify cannot run while %s is set: that mode records "
              "as established what it did not check, so validation would be "
              "checking a claim nothing made\n",
              trusted);
      return DRIVER_FLAG_FAILED;
    }
    ir_verify_set_enabled(1);
    options->optimize = 1;
  } else if (strcmp(argv[i], "--simd-report") == 0) {
    options->simd_report = 1;
  } else if (strcmp(argv[i], "--explain") == 0) {
    options->explain = 1;
  } else if (strncmp(argv[i], "--explain=", 10) == 0) {
    options->explain = 1;
    options->explain_filter = argv[i] + 10;
  } else if (strcmp(argv[i], "--explain-all") == 0) {
    options->explain = 1;
    options->explain_all = 1;
  } else if (strcmp(argv[i], "--explain-json") == 0) {
    options->explain = 1;
    options->explain_json = 1;
  } else if (strcmp(argv[i], "--annotate-asm") == 0) {
    options->annotate_asm = 1;
    options->optimize = 1;
    options->release = 1;
    options->explain = 1;
    options->asm_syntax = 2;
  } else if (strncmp(argv[i], "--annotate-lines=", 17) == 0) {
    const char *v = argv[i] + 17;
    int a = 0, b = 0;
    if (sscanf(v, "%d-%d", &a, &b) == 2) {
    } else if (sscanf(v, "%d", &a) == 1) {
      b = a;
    } else {
      fprintf(stderr, "Error: --annotate-lines expects A or A-B (got '%s')\n", v);
      return DRIVER_FLAG_FAILED;
    }
    if (a <= 0 || b < a) {
      fprintf(stderr, "Error: --annotate-lines range invalid: %s\n", v);
      return DRIVER_FLAG_FAILED;
    }
    options->annotate_q_lo = a;
    options->annotate_q_hi = b;
    options->annotate_asm = 1;
    options->optimize = 1;
    options->release = 1;
    options->explain = 1;
    if (!options->asm_syntax) options->asm_syntax = 0;
  } else if (strncmp(argv[i], "--annotate-fn=", 14) == 0) {
    options->annotate_q_fn = argv[i] + 14;
    options->annotate_asm = 1;
    options->optimize = 1;
    options->release = 1;
    options->explain = 1;
  } else if (strcmp(argv[i], "--annotate-hot") == 0 ||
             strncmp(argv[i], "--annotate-hot=", 15) == 0) {
    int n = 8;
    if (argv[i][14] == '=') n = atoi(argv[i] + 15);
    if (n <= 0) n = 8;
    options->annotate_hot = n;
    options->annotate_asm = 1;
    options->optimize = 1;
    options->release = 1;
    options->explain = 1;
  } else if (strncmp(argv[i], "--asm-syntax=", 13) == 0) {
    const char *v = argv[i] + 13;
    if (strcmp(v, "intel") == 0) {
      options->asm_syntax = 0;
    } else if (strcmp(v, "att") == 0) {
      options->asm_syntax = 1;
    } else if (strcmp(v, "both") == 0) {
      options->asm_syntax = 2;
    } else {
      fprintf(stderr,
              "Error: --asm-syntax must be intel, att, or both (got '%s')\n",
              v);
      return DRIVER_FLAG_FAILED;
    }
  } else {
    return DRIVER_FLAG_UNMATCHED;
  }
  *index = i;
  return DRIVER_FLAG_TAKEN;
}

static DriverFlagResult parse_flag_gpu(CompilerOptions *options,
                                     DriverFlags *flags,
                                     int argc, char *argv[],
                                     int *index) {
  int i = *index;
  (void)argc;
  (void)flags;
  if (strcmp(argv[i], "--emit-ptx") == 0) {
    options->emit_ptx = 1;
  } else if (strncmp(argv[i], "--emit-kernel-decls", 19) == 0) {
    options->emit_kernel_decls =
        argv[i][19] == '=' ? argv[i] + 20 : "";
    if (argv[i][19] != '\0' && argv[i][19] != '=') {
      fprintf(stderr, "Error: unknown option '%s'\n", argv[i]);
      return DRIVER_FLAG_FAILED;
    }
  } else if (strncmp(argv[i], "--gpu-arch=", 11) == 0) {
    const char *arch = argv[i] + 11;
    flags->gpu_arch_explicit = 1;
    if (strcmp(arch, "gb10") == 0) {
      options->ptx_target = "sm_121a";
      if (!flags->ptx_version_explicit) {
        options->ptx_isa_major = 8;
        options->ptx_isa_minor = 8;
      }
    } else if (strcmp(arch, "native") == 0) {
      if (!gpu_detect_ptx_target(0, flags->detected_ptx_target,
                                 sizeof(flags->detected_ptx_target))) {
        fprintf(stderr,
                "Error: --gpu-arch=native found no local NVIDIA device (%s); "
                "name a target with --gpu-arch=sm_NN to cross-compile\n",
                gpu_detect_local()->source);
        return DRIVER_FLAG_FAILED;
      }
      options->ptx_target = flags->detected_ptx_target;
    } else if (strcmp(arch, "portable") == 0) {
      options->ptx_target = "compute_75";
      if (!flags->ptx_version_explicit) {
        options->ptx_isa_major = 6;
        options->ptx_isa_minor = 4;
      }
    } else if (strncmp(arch, "sm_", 3) == 0 ||
               strncmp(arch, "compute_", 8) == 0) {
      options->ptx_target = arch;
    } else {
      fprintf(stderr,
              "Error: --gpu-arch expects gb10, portable, sm_NN, or "
              "compute_NN (got '%s')\n",
              arch);
      return DRIVER_FLAG_FAILED;
    }
  } else if (strncmp(argv[i], "--ptx-version=", 14) == 0) {
    const char *version = argv[i] + 14;
    int major = 0, minor = 0;
    char trailing = '\0';
    if (sscanf(version, "%d.%d%c", &major, &minor, &trailing) != 2 ||
        major < 1 || major > 99 || minor < 0 || minor > 9) {
      fprintf(stderr,
              "Error: --ptx-version expects MAJOR.MINOR (got '%s')\n",
              version);
      return DRIVER_FLAG_FAILED;
    }
    options->ptx_isa_major = major;
    options->ptx_isa_minor = minor;
    flags->ptx_version_explicit = 1;
  } else if (strncmp(argv[i], "--gpu-tensor-tuple-budget=", 26) == 0) {
    const char *value = argv[i] + 26;
    int budget = 0;
    char trailing = '\0';
    if (sscanf(value, "%d%c", &budget, &trailing) != 1 || budget < 0 ||
        budget > 4096) {
      fprintf(stderr,
              "Error: --gpu-tensor-tuple-budget expects 0..4096 (got '%s')\n",
              value);
      return DRIVER_FLAG_FAILED;
    }
    options->ptx_tensor_tuple_budget = budget;
  } else if (strcmp(argv[i], "--report-occupancy") == 0) {
    options->report_occupancy = 1;
  } else if (strcmp(argv[i], "--gpu-checks") == 0) {
    options->gpu_checks = 1;
  } else if (strcmp(argv[i], "--report-launches") == 0) {
    options->report_launches = 1;
  } else if (strcmp(argv[i], "--report-gpu-types") == 0) {
    options->report_gpu_types = 1;
  } else {
    return DRIVER_FLAG_UNMATCHED;
  }
  *index = i;
  return DRIVER_FLAG_TAKEN;
}

static DriverFlagResult parse_flag_checks(CompilerOptions *options,
                                     DriverFlags *flags,
                                     int argc, char *argv[],
                                     int *index) {
  int i = *index;
  (void)argc;
  (void)flags;
  if (strcmp(argv[i], "--old") == 0 && i + 1 < argc) {
    options->swap_old_name = argv[++i];
  } else if (strcmp(argv[i], "--new") == 0 && i + 1 < argc) {
    options->swap_new_name = argv[++i];
  } else if (strcmp(argv[i], "--report-rules") == 0) {
    options->report_rules = 1;
  } else if (strcmp(argv[i], "--check-proofs") == 0) {
    options->check_proofs = 1;
  } else if (strcmp(argv[i], "--record-trace") == 0) {
    options->record_trace = 1;
  } else if (strcmp(argv[i], "--check-overflow") == 0) {
    options->check_overflow = 1;
  } else if (strcmp(argv[i], "--check-deadlines") == 0) {
    options->check_deadlines = 1;
  } else if (strcmp(argv[i], "--report-deadlines") == 0) {
    options->report_deadlines = 1;
  } else if (strcmp(argv[i], "--check-tasks") == 0) {
    options->check_tasks = 1;
  } else if (strcmp(argv[i], "--check-effects") == 0) {
    options->check_effects = 1;
  } else if (strcmp(argv[i], "--check-purity-fault") == 0) {
    options->check_purity_fault = 1;
  } else if (strcmp(argv[i], "--report-proofs") == 0) {
    options->report_proofs = 1;
  } else if (strcmp(argv[i], "--report-twins") == 0) {
    options->report_twins = 1;
  } else if (strcmp(argv[i], "--fix") == 0) {
    options->apply_rule_fixes = 1;
  } else if (strcmp(argv[i], "--report-effects") == 0) {
    options->report_effects = 1;
  } else if (strncmp(argv[i], "--proof-budget=", 15) == 0) {
    long long budget = strtoll(argv[i] + 15, NULL, 10);
    if (budget <= 0) {
      fprintf(stderr, "--proof-budget must be a positive step count\n");
      return DRIVER_FLAG_FAILED;
    }
    options->proof_budget = budget;
    options->proof_budget_set = 1;
  } else if (strncmp(argv[i], "--effect-budget=", 16) == 0) {
    long long budget = strtoll(argv[i] + 16, NULL, 10);
    if (budget <= 0) {
      fprintf(stderr, "--effect-budget must be a positive step count\n");
      return DRIVER_FLAG_FAILED;
    }
    options->effect_budget = budget;
    options->effect_budget_set = 1;
  } else if (strncmp(argv[i], "--rule-budget=", 14) == 0) {
    long long budget = strtoll(argv[i] + 14, NULL, 10);
    if (budget <= 0) {
      fprintf(stderr, "--rule-budget must be a positive step count\n");
      return DRIVER_FLAG_FAILED;
    }
    options->rule_budget = budget;
    options->rule_budget_set = 1;
  } else if (strcmp(argv[i], "--report-meta") == 0) {
    options->report_meta = 1;
  } else if (strncmp(argv[i], "--meta-budget=", 14) == 0) {
    long long budget = atoll(argv[i] + 14);
    if (budget <= 0) {
      fprintf(stderr, "--meta-budget must be a positive step count\n");
      return DRIVER_FLAG_FAILED;
    }
    options->meta_budget = budget;
    options->meta_budget_set = 1;
  } else if (strcmp(argv[i], "--report-expansion") == 0) {
    options->report_expansion = 1;
  } else if (strncmp(argv[i], "--expansion-budget=", 19) == 0) {
    long long budget = atoll(argv[i] + 19);
    if (budget < 0) {
      fprintf(stderr, "--expansion-budget must not be negative\n");
      return DRIVER_FLAG_FAILED;
    }
    options->expansion_budget = (size_t)budget;
    options->expansion_budget_set = 1;
  } else if (strncmp(argv[i], "--sms=", 6) == 0) {
    int sms = atoi(argv[i] + 6);
    if (sms < 1 || sms > 1024) {
      fprintf(stderr, "Error: --sms expects an SM count from 1 to 1024 "
                      "(got '%s')\n",
              argv[i] + 6);
      return DRIVER_FLAG_FAILED;
    }
    options->report_sms = sms;
  } else if (strcmp(argv[i], "--emit-spirv") == 0) {
    options->emit_spirv = 1;
  } else if (strcmp(argv[i], "--emit-metal") == 0) {
    options->emit_metal = 1;
  } else if (strncmp(argv[i], "--metal-version=", 16) == 0) {
    MslEmitOptions metal_options;
    msl_emit_default_options(&metal_options);
    if (!msl_parse_version(argv[i] + 16, &metal_options)) {
      fprintf(stderr,
              "Error: --metal-version expects 3.1, 3.2, 4.0 or 4.1 (got "
              "'%s')\n",
              argv[i] + 16);
      return DRIVER_FLAG_FAILED;
    }
    options->metal_version_major = metal_options.version_major;
    options->metal_version_minor = metal_options.version_minor;
  } else if (strcmp(argv[i], "--metal-fast-math") == 0) {
    options->metal_fast_math = 1;
  } else if (strncmp(argv[i], "--gpu-provider=", 15) == 0) {
    if (strcmp(argv[i] + 15, "cuda") == 0) {
      options->gpu_provider = 1;
    } else if (strcmp(argv[i] + 15, "metal") == 0) {
      options->gpu_provider = 2;
    } else {
      fprintf(stderr,
              "Error: --gpu-provider expects cuda or metal (got '%s')\n",
              argv[i] + 15);
      return DRIVER_FLAG_FAILED;
    }
  } else {
    return DRIVER_FLAG_UNMATCHED;
  }
  *index = i;
  return DRIVER_FLAG_TAKEN;
}

static DriverFlagResult parse_flag_codegen(CompilerOptions *options,
                                     DriverFlags *flags,
                                     int argc, char *argv[],
                                     int *index) {
  int i = *index;
  (void)argc;
  (void)flags;
  if (strcmp(argv[i], "--emit-arm64") == 0) {
    options->emit_arm64 = 1;
  } else if (strcmp(argv[i], "--emit-arm64-obj") == 0) {
    options->emit_arm64_obj = 1;
  } else if (strcmp(argv[i], "-g") == 0 ||
             strcmp(argv[i], "--debug-symbols") == 0) {
    options->generate_debug_symbols = 1;
  } else if (strcmp(argv[i], "-l") == 0 ||
             strcmp(argv[i], "--line-mapping") == 0) {
    options->generate_line_mapping = 1;
  } else if (strcmp(argv[i], "-s") == 0 ||
             strcmp(argv[i], "--stack-trace") == 0) {
    options->generate_stack_trace_support = 1;
  } else if (strcmp(argv[i], "--no-crash-report") == 0) {
    options->generate_crash_report = 0;
  } else if (strcmp(argv[i], "--debug-format") == 0 && i + 1 < argc) {
    options->debug_format = argv[++i];
  } else if (strcmp(argv[i], "-O") == 0 ||
             strcmp(argv[i], "--optimize") == 0) {
    options->optimize = 1;
  } else if (strcmp(argv[i], "-r") == 0 ||
             strcmp(argv[i], "--release") == 0) {
    options->release = 1;
    options->optimize = 1;
  } else if (strcmp(argv[i], "--strip-comments") == 0) {
    fprintf(stderr,
            "Error: --strip-comments has been removed; Mettle no longer "
            "emits text assembly.\n");
    return DRIVER_FLAG_FAILED;
  } else if (strcmp(argv[i], "--prelude") == 0) {
    options->prelude = 1;
  } else if (strcmp(argv[i], "--profile") == 0) {
    options->profile = 1;
  } else if (strcmp(argv[i], "--profile-runtime") == 0) {
    options->profile_runtime = 1;
  } else if (strcmp(argv[i], "--profile-runtime-ops") == 0) {
    options->profile_runtime_ops = 1;
  } else if (strcmp(argv[i], "--profile-blocks") == 0) {
    options->profile_blocks = 1;
    options->profile_runtime = 1;
  } else if (strcmp(argv[i], "--debug-hooks") == 0) {
    options->debug_hooks = 1;
  } else if (strcmp(argv[i], "--safe") == 0) {
    options->safe = 1;
  } else if (strcmp(argv[i], "--native-heap") == 0) {
    options->native_heap = 1;
  } else if (strcmp(argv[i], "--static") == 0) {
    options->static_link = 1;
  } else if (strcmp(argv[i], "--musl") == 0) {
    options->musl_link = 1;
    options->static_link = 1;
  } else if (strcmp(argv[i], "--tracy") == 0) {
    options->tracy = 1;
  } else if (strcmp(argv[i], "--tracy-dir") == 0 && i + 1 < argc) {
    options->tracy_directory = argv[++i];
  } else if (strcmp(argv[i], "--tracy-dir") == 0) {
    fprintf(stderr, "Error: Missing path after '--tracy-dir'\n");
    return DRIVER_FLAG_FAILED;
  } else {
    return DRIVER_FLAG_UNMATCHED;
  }
  *index = i;
  return DRIVER_FLAG_TAKEN;
}

static DriverFlagResult parse_flag_target(CompilerOptions *options,
                                     DriverFlags *flags,
                                     int argc, char *argv[],
                                     int *index) {
  int i = *index;
  (void)argc;
  (void)flags;
  if (strcmp(argv[i], "--target") == 0 && i + 1 < argc) {
    char target_error[256];
    options->target_triple = argv[++i];
    if (target_argument_is_description(options->target_triple)) {
      options->target_desc_path = options->target_triple;
      *index = i;
      return DRIVER_FLAG_TAKEN;
    }
    if (!mtlc_target_select(options->target_triple, target_error,
                            sizeof(target_error))) {
      fprintf(stderr, "Error: %s\n", target_error);
      return DRIVER_FLAG_FAILED;
    }
    if (mtlc_target()->arch == MTLC_TARGET_ARCH_AARCH64) {
      options->emit_arm64_obj = 1;
    }
  } else if (strcmp(argv[i], "--report-target") == 0) {
    options->report_target = 1;
  } else if (strcmp(argv[i], "--target") == 0) {
    fprintf(stderr, "Error: Missing triple after '--target'; known targets "
                    "are %s\n",
            mtlc_target_triple_list());
    return DRIVER_FLAG_FAILED;
  } else if (strncmp(argv[i], "--target=", 9) == 0) {
    char target_error[256];
    options->target_triple = argv[i] + 9;
    if (target_argument_is_description(options->target_triple)) {
      options->target_desc_path = options->target_triple;
      *index = i;
      return DRIVER_FLAG_TAKEN;
    }
    if (!mtlc_target_select(options->target_triple, target_error,
                            sizeof(target_error))) {
      fprintf(stderr, "Error: %s\n", target_error);
      return DRIVER_FLAG_FAILED;
    }
    if (mtlc_target()->arch == MTLC_TARGET_ARCH_AARCH64) {
      options->emit_arm64_obj = 1;
    }
  } else if (strcmp(argv[i], "--image-base") == 0 && i + 1 < argc) {
    char *end = NULL;
    unsigned long long base = strtoull(argv[++i], &end, 0);
    if (!end || *end != '\0') {
      fprintf(stderr, "Error: '--image-base' takes an address, e.g. 0x7c00\n");
      return DRIVER_FLAG_FAILED;
    }
    options->image_base = base;
    options->image_base_set = 1;
    mtlc_target_set_image_base(base);
  } else if (strcmp(argv[i], "--image-base") == 0) {
    fprintf(stderr, "Error: Missing address after '--image-base'\n");
    return DRIVER_FLAG_FAILED;
  } else if (strcmp(argv[i], "--emit-flat") == 0 && i + 1 < argc) {
    options->flat_output = argv[++i];
    options->emit_object = 1;
  } else if (strcmp(argv[i], "--emit-flat") == 0) {
    fprintf(stderr, "Error: Missing output path after '--emit-flat'\n");
    return DRIVER_FLAG_FAILED;
  } else if (strcmp(argv[i], "--debug-compiler") == 0) {
    options->debug_compiler = 1;
  } else {
    return DRIVER_FLAG_UNMATCHED;
  }
  *index = i;
  return DRIVER_FLAG_TAKEN;
}

typedef DriverFlagResult (*DriverFlagParser)(CompilerOptions *, DriverFlags *,
                                             int, char **, int *);

static const DriverFlagParser DRIVER_FLAG_PARSERS[] = {
    parse_flag_output, parse_flag_diagnostics, parse_flag_gpu,
    parse_flag_checks,
    parse_flag_codegen, parse_flag_target};

static int subcommand_flag_takes_value(const char *argument) {
  static const char *const with_value[] = {
      "-I",        "-L",       "-l",         "-o",
      "--stdlib",  "--rpath",  "--link-arg", "--target",
      "--library", "--soname", "--dynamic-linker", "--library-path"};
  for (size_t i = 0; i < sizeof(with_value) / sizeof(with_value[0]); i++) {
    if (strcmp(argument, with_value[i]) == 0) {
      return 1;
    }
  }
  return 0;
}

static int subcommand_argument_is_flag(const char *argument) {
  return argument[0] == '-' && argument[1] != '\0' &&
         !(argument[1] >= '0' && argument[1] <= '9') && argument[1] != '.';
}

static int split_subcommand_flags(int argc, char *argv[], int first,
                                  const char **positional,
                                  int *positional_count) {
  char **flags = malloc((size_t)argc * sizeof(char *));
  int flag_count = 0;
  *positional_count = 0;
  if (!flags) {
    return argc;
  }
  for (int i = first; i < argc; i++) {
    if (subcommand_argument_is_flag(argv[i])) {
      flags[flag_count++] = argv[i];
      if (subcommand_flag_takes_value(argv[i]) && i + 1 < argc) {
        flags[flag_count++] = argv[++i];
      }
    } else if (positional) {
      positional[(*positional_count)++] = argv[i];
    }
  }
  for (int i = 0; i < flag_count; i++) {
    argv[2 + i] = flags[i];
  }
  free(flags);
  return 2 + flag_count;
}

static int parse_arguments(CompilerOptions *options, DriverFlags *flags,
                           int argc, char *argv[]) {
  for (int i = 1; i < argc; i++) {
    DriverFlagResult taken = DRIVER_FLAG_UNMATCHED;
    size_t group;
    for (group = 0;
         group < sizeof(DRIVER_FLAG_PARSERS) / sizeof(DRIVER_FLAG_PARSERS[0]);
         group++) {
      taken = DRIVER_FLAG_PARSERS[group](options, flags, argc, argv, &i);
      if (taken != DRIVER_FLAG_UNMATCHED) {
        break;
      }
    }
    if (taken == DRIVER_FLAG_FAILED) {
      return 1;
    }
    if (taken == DRIVER_FLAG_TAKEN) {
      continue;
    }
    if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
      print_usage(argv[0]);
      return 0;
    }
    if (!options->input_filename) {
      options->input_filename = argv[i];
      continue;
    }
    fprintf(stderr, "Error: Unknown or misplaced argument '%s'\n", argv[i]);
    print_usage(argv[0]);
    return 1;
  }
  return -1;
}

static int mettle_check_target_options(CompilerOptions *options,
                                       DriverFlags *flags) {
  if (options->flat_output && flags->build_executable) {
    fprintf(stderr,
            "Error: --emit-flat writes the linked image itself; drop --build\n");
    free((void *)options->import_directories);
    free((void *)options->link_arguments);
    return 1;
  }

  if (!mtlc_target_is_object_capable(mtlc_target()) && !options->flat_output) {
    fprintf(stderr,
            "Error: the %s target emits a flat image only; add --emit-flat "
            "<file> (and --image-base <addr>)\n",
            mtlc_target()->triple);
    free((void *)options->import_directories);
    free((void *)options->link_arguments);
    return 1;
  }

  if (flags->build_executable && mtlc_target()->explicit_triple) {
    const MtlcTarget *target = mtlc_target();
    if (target->freestanding) {
      fprintf(stderr,
              "Error: --build links against the host runtime, which the %s "
              "target has none of; emit the image with --emit-flat, or the "
              "object with --emit-obj and link it yourself\n",
              target->triple);
      free((void *)options->import_directories);
      free((void *)options->link_arguments);
      return 1;
    }
    if (target->os != mtlc_target_host_os()) {
      fprintf(stderr,
              "Error: --build for %s has to run that machine's linker against "
              "that machine's runtime, and neither is here; emit the object "
              "with --emit-obj and link it on a %s host\n",
              target->triple, mtlc_target_os_name(target->os));
      free((void *)options->import_directories);
      free((void *)options->link_arguments);
      return 1;
    }
  }

  if (mtlc_target()->image_base_set && !options->flat_output) {
    if (!flags->build_executable) {
      fprintf(stderr,
              "Error: --image-base says where a linked image loads, and this "
              "compile produces a relocatable object; add --build, or "
              "--emit-flat <file> for a raw image\n");
      free((void *)options->import_directories);
      free((void *)options->link_arguments);
      return 1;
    }
    {
      uint64_t base = mtlc_target()->image_base;
      uint64_t alignment =
          mtlc_target()->format == BINARY_TARGET_FORMAT_COFF_WIN64 ? 0x10000u
                                                                   : 0x1000u;
      if (base % alignment) {
        fprintf(stderr,
                "Error: --image-base 0x%llx is not aligned to 0x%llx, which is "
                "the boundary a %s image loads on\n",
                (unsigned long long)base, (unsigned long long)alignment,
                mtlc_target()->format == BINARY_TARGET_FORMAT_COFF_WIN64
                    ? "PE"
                    : "ELF");
        free((void *)options->import_directories);
        free((void *)options->link_arguments);
        return 1;
      }
    }
  }

  if (options->safe && mtlc_target()->freestanding) {
    fprintf(stderr,
            "Error: --safe on the %s target has no runtime to report a "
            "violation to; its checks call into the shadow map the runtime "
            "owns, and a freestanding image links no library\n",
            mtlc_target()->triple);
    free((void *)options->import_directories);
    free((void *)options->link_arguments);
    return 1;
  }
  if (options->check_effects && mtlc_target()->freestanding) {
    fprintf(stderr,
            "Error: --check-effects on the %s target has no runtime to keep "
            "the effect stack in, and a freestanding image links no "
            "library\n",
            mtlc_target()->triple);
    free((void *)options->import_directories);
    free((void *)options->link_arguments);
    return 1;
  }

  if (options->emit_ptx && !flags->gpu_arch_explicit &&
      detect_host_gpu_ptx_target(flags->detected_ptx_target,
                                 sizeof(flags->detected_ptx_target))) {
    options->ptx_target = flags->detected_ptx_target;
  }

  if (options->emit_ptx && !flags->ptx_version_explicit) {
    int driver_major = 0, driver_minor = 0;
    if (gpu_detect_ptx_isa(&driver_major, &driver_minor) &&
        (driver_major < options->ptx_isa_major ||
         (driver_major == options->ptx_isa_major &&
          driver_minor < options->ptx_isa_minor))) {
      options->ptx_isa_major = driver_major;
      options->ptx_isa_minor = driver_minor;
    }
  }

  if (options->tracy && !flags->build_executable) {
    fprintf(stderr, "Error: --tracy requires --build\n");
    free((void *)options->import_directories);
    free((void *)options->link_arguments);
    return 1;
  }

  if (options->emit_arm64_obj && options->emit_arm64) {
    fprintf(stderr,
            "Error: --emit-arm64 (self-contained AArch64 executable) and "
            "--emit-arm64-obj (AArch64 relocatable object) are different "
            "outputs; pick one\n");
    free((void *)options->import_directories);
    free((void *)options->link_arguments);
    return 1;
  }

#if !defined(__aarch64__) && !defined(_M_ARM64)
  if (options->emit_arm64_obj && flags->build_executable) {
    fprintf(stderr,
            "Error: --emit-arm64-obj cannot be combined with --build on an "
            "x86-64 host: the object is AArch64 and this host's linker cannot "
            "link it. Link it on an ARM machine, or use --emit-arm64 for a "
            "self-contained executable\n");
    free((void *)options->import_directories);
    free((void *)options->link_arguments);
    return 1;
  }
#endif

  return 0;
}

int main(int argc, char *argv[]) {
  CompilerOptions options = {0};
  mettle_compiler_crash_install(argc, argv);
  mettle_compiler_self_profile_start();
  char *auto_stdlib_directory = NULL;
  char *auto_runtime_directory = NULL;
  char *build_output_filename = NULL;
  char *object_output_filename = NULL;
  DriverFlags flags = {0};
  options.emit_object = 1;
  options.generate_crash_report = 1;
  options.output_filename = default_object_output_filename();
  options.debug_format = "dwarf";
  options.ptx_target = "sm_121a";
  options.ptx_isa_major = 8;
  options.ptx_isa_minor = 8;

  if (argc >= 2) {
    if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0 ||
        strcmp(argv[1], "version") == 0) {
#if defined(__aarch64__) && defined(__linux__)
      const char *host = "aarch64";
      const char *target = "aarch64-linux (ELF relocatable object)";
#else
      const char *host = "x86_64";
      const char *target =
          host_target_is_elf() ? "x86_64-linux (ELF)" : "x86_64-windows (COFF)";
#endif
      printf("mettle %s\n", METTLE_VERSION);
      printf("host: %s\n", host);
      printf("target: %s\n", target);
      return 0;
    }
    if (strcmp(argv[1], "--gpu-info") == 0 || strcmp(argv[1], "gpu") == 0) {
      return report_gpu_info(options.ptx_target, options.ptx_isa_major,
                             options.ptx_isa_minor);
    }
    if (strcmp(argv[1], "help") == 0) {
      return print_help_topic(argv[0], argv[0], argc >= 3 ? argv[2] : NULL);
    }
    if (strcmp(argv[1], "tune") == 0) {
      char *self_path = get_executable_path(argv[0]);
      int tuned = mettle_tune_main(argc - 1, argv + 1,
                                   self_path ? self_path : argv[0]);
      free(self_path);
      return tuned;
    }
    if (strcmp(argv[1], "explain") == 0) {
      if (argc >= 4) {
        return explain_rule_code(argv[2], argv[3]);
      }
      return mettle_explain_error_code(argc >= 3 ? argv[2] : NULL);
    }
    if (strcmp(argv[1], "expand") == 0) {
      options.expand_mode = 1;
      for (int i = 1; i + 1 < argc; i++) {
        argv[i] = argv[i + 1];
      }
      argc--;
      if (argc < 2) {
        fprintf(stderr, "usage: mettle expand <file.mettle>\n");
        return 1;
      }
    }
    if (strcmp(argv[1], "check-trace") == 0) {
      if (argc < 4) {
        fprintf(stderr,
                "usage: mettle check-trace <file.mettle> <trace>\n"
                "  runs the file's `@rule` functions over `Trace` against a "
                "run --record-trace wrote down\n");
        return 1;
      }
      options.check_trace_path = argv[3];
      argv[1] = argv[2];
      for (int k = 4; k < argc; k++) {
        argv[k - 2] = argv[k];
      }
      argc -= 2;
    }
    if (strcmp(argv[1], "machine") == 0 ||
        strcmp(argv[1], "emulate") == 0) {
      if (strcmp(argv[1], "machine") == 0) {
        options.machine_mode = 1;
      } else {
        options.emulate_mode = 1;
      }
      for (int i = 1; i + 1 < argc; i++) {
        argv[i] = argv[i + 1];
      }
      argc--;
      if (argc < 2) {
        fprintf(stderr,
                "usage: mettle machine <file.mettle>   prints the machine the "
                "file describes\n"
                "       mettle emulate <file.mettle>   assembles PROGRAM into "
                "that machine's encoding, decodes it back, and runs it\n");
        return 1;
      }
    }
    if (strcmp(argv[1], "why") == 0) {
      if (argc < 5) {
        fprintf(stderr,
                "usage: mettle why <file.mettle> <function> <effect>\n"
                "       mettle why <file.mettle> <line[:column]> <type>\n"
                "  prints the chain that established a fact this build "
                "rested on\n");
        return 1;
      }
      options.why_mode = 1;
      options.why_subject = argv[3];
      options.why_what = argv[4];
      argv[1] = argv[2];
      argc = split_subcommand_flags(argc, argv, 5, NULL, &(int){0});
    }
    if (strcmp(argv[1], "live") == 0) {
      return mettle_live_main(argc, argv);
    }
    if (strcmp(argv[1], "--live-undo") == 0 && argc >= 3) {
      LivePatchSummary summary;
      char *live_error = NULL;
      if (!live_build_undo(argv[2], &summary, &live_error)) {
        fprintf(stderr, "Error: %s\n",
                live_error ? live_error : "Failed to undo a live patch");
        free(live_error);
        return 1;
      }
      printf("Patch %u undoes patch %u\n", summary.patch_index,
             summary.undone);
      return 0;
    }
    if (strcmp(argv[1], "swap-check") == 0) {
      options.swap_check_mode = 1;
      for (int i = 1; i + 1 < argc; i++) {
        argv[i] = argv[i + 1];
      }
      argc--;
      if (argc < 2) {
        fprintf(stderr,
                "usage: mettle swap-check <file.mettle> --old <fn> --new <fn>\n");
        return 1;
      }
    }
    if (strcmp(argv[1], "target") == 0) {
      MtlcTargetDescription description;
      char error[256];
      if (argc < 3) {
        fprintf(stderr, "usage: mettle target <triple>   (prints the target's "
                        "description as Mettle; known targets: %s)\n",
                mtlc_target_triple_list());
        return 1;
      }
      if (!mtlc_target_description_of(argv[2], &description, error,
                                      sizeof(error))) {
        fprintf(stderr, "Error: %s\n", error);
        return 1;
      }
      mtlc_target_print_description(stdout, &description);
      return 0;
    }
    if (strcmp(argv[1], "test") == 0) {
      options.test_mode = 1;
      for (int i = 1; i + 1 < argc; i++) {
        argv[i] = argv[i + 1];
      }
      argc--;
      if (argc < 2) {
        fprintf(stderr, "usage: mettle test <file.mettle> [--filter=SUBSTR]\n");
        return 1;
      }
    } else if (strcmp(argv[1], "trace") == 0) {
      if (argc < 4) {
        fprintf(stderr,
                "usage: mettle trace <file.mettle> <function> [args...]\n"
                "  int/float parameters take the CLI values in order; pointer\n"
                "  parameters get a synthesized buffer\n");
        return 1;
      }
      const char **trace_positional = malloc((size_t)argc * sizeof(char *));
      int trace_positional_count = 0;
      if (!trace_positional) {
        return 1;
      }
      options.trace_function = argv[3];
      argv[1] = argv[2];
      argc = split_subcommand_flags(argc, argv, 4, trace_positional,
                                    &trace_positional_count);
      options.trace_args = (const char *const *)trace_positional;
      options.trace_arg_count = (size_t)trace_positional_count;
    }
    if (strcmp(argv[1], "docs") == 0) {
      if (argc >= 3) {
        return print_help_topic(argv[0], argv[0], argv[2]);
      }
      printf("Mettle documentation topics: build, runtime (alias: heap, gc), interop, stdlib, web\n");
      print_doc_reference(argv[0], "LANGUAGE.md");
      print_doc_reference(argv[0], "compilation.md");
      print_doc_reference(argv[0], "runtime-model.md");
      print_doc_reference(argv[0], "heap-allocation.md");
      return 0;
    }
  }

  {
    int early = parse_arguments(&options, &flags, argc, argv);
    if (early >= 0) {
      return early;
    }
  }

  if (!options.input_filename) {
    fprintf(stderr, "Error: No input file specified.\n");
    print_usage(argv[0]);
    free((void *)options.import_directories);
    free((void *)options.link_arguments);
    return 1;
  }

  if (!options.stdlib_directory) {
    auto_stdlib_directory = infer_default_stdlib_directory(argv[0]);
    if (auto_stdlib_directory) {
      options.stdlib_directory = auto_stdlib_directory;
    }
  }

  if (!load_target_description(&options)) {
    free((void *)options.import_directories);
    free((void *)options.link_arguments);
    free(auto_stdlib_directory);
    return 1;
  }
  if (options.report_target) {
    const MtlcTargetDescription *current = mtlc_target_current_description();
    if (current) {
      mtlc_target_print_description(stdout, current);
    }
  }

  {
    int target_status = mettle_check_target_options(&options, &flags);
    if (target_status != 0) {
      free(auto_stdlib_directory);
      return target_status;
    }
  }
  if (flags.build_executable) {
    options.emit_object = 1;
    if (!flags.linker_mode_explicit) {
      options.linker_mode = LINKER_MODE_INTERNAL;
    }
  }

  auto_runtime_directory = infer_default_runtime_directory(argv[0]);

  if (options.ml_opt) {
    ml_opt_set_default_paths(argv[0]);
  }

  BinaryTargetFormat host_format = mtlc_target()->format;
  int elf_build = host_format == BINARY_TARGET_FORMAT_ELF_X64 ||
                  host_format == BINARY_TARGET_FORMAT_ELF_ARM64 ||
                  host_format == BINARY_TARGET_FORMAT_MACHO_X64 ||
                  host_format == BINARY_TARGET_FORMAT_MACHO_ARM64;

  if (options.live && host_format != BINARY_TARGET_FORMAT_COFF_WIN64 &&
      host_format != BINARY_TARGET_FORMAT_ELF_X64) {
    fprintf(stderr,
            "Error: --live builds are Windows and Linux x64 only for now\n");
    return 1;
  }
  if (options.live_patch_target) {
    static char live_object_path[1024];
    if (flags.build_executable) {
      fprintf(stderr, "Error: --live-patch builds a patch, not an executable; "
                      "drop --build\n");
      return 1;
    }
    snprintf(live_object_path, sizeof(live_object_path), "%s.live.obj",
             options.live_patch_target);
    options.output_filename = live_object_path;
    options.emit_object = 1;
  }

  if (flags.build_executable) {
    if (!elf_build && (options.shared_library_count > 0u ||
                       options.export_dynamic || options.runpath_count > 0u ||
                       options.dynamic_linker)) {
      fprintf(stderr,
              "Error: -l, -L, --rpath, --export-dynamic "
              "and --dynamic-linker are ELF options; a PE build takes its "
              "libraries through --link-arg\n");
      free((void *)options.import_directories);
      free((void *)options.link_arguments);
      free((void *)options.shared_libraries);
      free((void *)options.library_search_paths);
      free((void *)options.runpaths);
      free(auto_stdlib_directory);
      free(auto_runtime_directory);
      return 1;
    }
    if (options.static_link && mettle_elf_dynamic_link_requested(&options)) {
      fprintf(stderr,
              "Error: --static and shared libraries ask for opposite images; "
              "drop one\n");
      free((void *)options.import_directories);
      free((void *)options.link_arguments);
      free((void *)options.shared_libraries);
      free((void *)options.library_search_paths);
      free((void *)options.runpaths);
      free(auto_stdlib_directory);
      free(auto_runtime_directory);
      return 1;
    }
    if (options.musl_link) {
      fprintf(stderr,
              "Error: --musl is not available in owned runtime mode because "
              "Mettle does not link a C library\n");
      free((void *)options.import_directories);
      free((void *)options.link_arguments);
      free(auto_stdlib_directory);
      free(auto_runtime_directory);
      return 1;
    }
    for (size_t i = 0; i < options.link_argument_count; i++) {
      if (mettle_link_argument_uses_forbidden_runtime(
              options.link_arguments[i])) {
        fprintf(stderr,
                "Error: --link-arg '%s' names a forbidden C or compiler "
                "runtime\n",
                options.link_arguments[i]);
        free((void *)options.import_directories);
        free((void *)options.link_arguments);
        free(auto_stdlib_directory);
        free(auto_runtime_directory);
        return 1;
      }
    }
#ifndef _WIN32
    if (!elf_build) {
      fprintf(stderr,
              "Error: --build is supported on Windows and Linux (ELF) only\n");
      free((void *)options.import_directories);
      free((void *)options.link_arguments);
      free(auto_stdlib_directory);
      free(auto_runtime_directory);
      return 1;
    }
    options.emit_object = 1;
#else
    if (!auto_runtime_directory) {
      fprintf(stderr,
              "Error: Could not locate bundled runtime directory for --build\n");
      free((void *)options.import_directories);
      free((void *)options.link_arguments);
      free(auto_stdlib_directory);
      free(auto_runtime_directory);
      return 1;
    }
#endif
    if (flags.output_filename_explicit) {
      build_output_filename = strdup(options.output_filename);
    } else if (!elf_build && options.shared_output) {
      build_output_filename =
          replace_extension(options.input_filename, ".dll");
    } else {
      build_output_filename = default_executable_filename(options.input_filename);
    }
    if (!build_output_filename) {
      fprintf(stderr,
              "Error: Could not choose an output name for '%s'. Pass -o "
              "<name>\n",
              options.input_filename ? options.input_filename : "");
      free((void *)options.import_directories);
      free((void *)options.link_arguments);
      free(auto_stdlib_directory);
      free(auto_runtime_directory);
      return 1;
    }

    object_output_filename = replace_extension(
        build_output_filename, elf_build ? ".o" : ".obj");
    if (!object_output_filename) {
      fprintf(stderr, "Error: Failed to determine object output path\n");
      free(build_output_filename);
      free((void *)options.import_directories);
      free((void *)options.link_arguments);
      free(auto_stdlib_directory);
      free(auto_runtime_directory);
      return 1;
    }
    options.output_filename = object_output_filename;
  }

  options.building_executable =
      flags.build_executable || options.live_patch_target != NULL;

  double command_profile_start =
      options.profile ? compiler_profile_now_ms() : 0.0;
  int result =
      compile_file(options.input_filename, options.output_filename, &options);
  if (result == 0 && options.live_patch_target) {
    live_write_file_list(options.live_patch_target, options.input_filename);
    LivePatchSummary summary;
    char *live_error = NULL;
    live_set_patch_tick(options.live_at_tick > 0
                            ? (uint64_t)options.live_at_tick
                            : 0u);
    if (!live_build_patch(options.output_filename, options.live_patch_target,
                          options.live_patch_all, &summary, &live_error)) {
      fprintf(stderr, "Error: %s\n",
              live_error ? live_error : "Failed to build a live patch");
      if (summary.names[0]) {
        fprintf(stderr, "  The patch was to replace: %s\n", summary.names);
      }
      result = 1;
    } else if (summary.patch_index == 0u) {
      printf("No function changed\n");
    } else {
      printf("Patch %u: %s (%zu bytes", summary.patch_index, summary.names,
             summary.blob_size);
      if (summary.new_globals) {
        printf(", %zu new global%s", summary.new_globals,
               summary.new_globals == 1u ? "" : "s");
      }
      if (summary.converted_globals) {
        printf(", %zu global%s converted to a new layout",
               summary.converted_globals,
               summary.converted_globals == 1u ? "" : "s");
      }
      if (summary.converted_types) {
        printf(", %zu heap type%s converted to a new layout",
               summary.converted_types,
               summary.converted_types == 1u ? "" : "s");
      }
      if (summary.at_tick) {
        printf(", at tick %llu", (unsigned long long)summary.at_tick);
      }
      printf(")\n");
      if (summary.moving[0]) {
        printf("  running frames of %s move to the new code at the next "
               "statement that can reach a quiesce point\n",
               summary.moving);
      }
      if (summary.running[0]) {
        printf("  %s %s running at the quiesce point and %s the old code "
               "until %s next called\n",
               summary.running, strchr(summary.running, ',') ? "are" : "is",
               strchr(summary.running, ',') ? "keep" : "keeps",
               strchr(summary.running, ',') ? "they are" : "it is");
      }
    }
    free(live_error);
    remove(options.output_filename);
  } else if (result == 0 && flags.build_executable) {
    double build_profile_start =
        options.profile ? compiler_profile_now_ms() : 0.0;
#ifndef _WIN32
    result = mettle_link_elf_executable(options.output_filename,
                                        build_output_filename, &options,
                                        auto_runtime_directory);
#else
    result = mettle_link_object_file(options.output_filename,
                                     build_output_filename,
                                     auto_runtime_directory, &options);
#endif
    if (result == 0 && !g_link_output_ownership_verified) {
      char ownership_error[256];
      int owned =
          mettle_elf_dynamic_link_requested(&options)
              ? mettle_verify_owned_dynamic_executable(build_output_filename,
                                                       ownership_error,
                                                       sizeof(ownership_error))
              : mettle_verify_owned_executable(build_output_filename,
                                               ownership_error,
                                               sizeof(ownership_error));
      if (!owned) {
        fprintf(stderr,
                "Error: Refusing linked output '%s': %s\n",
                build_output_filename, ownership_error);
        remove(build_output_filename);
        result = 1;
      }
    }
    if (result == 0) {
      if (options.shared_output) {
        printf("Built shared library '%s'\n", build_output_filename);
      } else {
        printf("Built executable '%s'\n", build_output_filename);
      }
    }
    if (options.profile) {
      fprintf(stderr, "Executable build profile%s:\n",
              result == 0 ? "" : " (failed)");
      fprintf(stderr, "  %-20s %9.3f ms\n", "assemble/link",
              compiler_profile_now_ms() - build_profile_start);
    }
  } else if (result == 0 && auto_runtime_directory && options.debug_mode &&
             !options.dump_ir) {
    fprintf(stderr,
            "Note: transitional runtime objects detected at '%s'. Use --build "
            "to assemble and link them automatically when needed (most "
            "programs link nothing from this directory).\n",
            auto_runtime_directory);
  }
  if (options.profile) {
    fprintf(stderr, "Command profile%s:\n", result == 0 ? "" : " (failed)");
    fprintf(stderr, "  %-20s %9.3f ms\n", "total",
            compiler_profile_now_ms() - command_profile_start);
  }
  free((void *)options.import_directories);
  free((void *)options.link_arguments);
  free((void *)options.shared_libraries);
  free((void *)options.library_search_paths);
  free((void *)options.runpaths);
  free(auto_stdlib_directory);
  free(auto_runtime_directory);
  free(build_output_filename);
  free(object_output_filename);
  if (getenv("METTLE_FULL_CLEANUP")) {
    string_intern_clear();
  }
  mettle_compiler_self_profile_report();
  return result;
}

static int compile_read_source(const char *filename, char **out_source) {
  *out_source = read_file(filename);
  if (!*out_source) {
    fprintf(stderr, "Error: Could not read file '%s'\n", filename);
    return 0;
  }
  return 1;
}

static int compile_lex_and_parse(Parser *parser, ErrorReporter *error_reporter,
                                 ASTNode **out_program) {
  *out_program = parser_parse_program(parser);
  if (!*out_program || parser->had_error ||
      error_reporter_has_errors(error_reporter)) {
    if (error_reporter_has_errors(error_reporter)) {
      error_reporter_print_errors(error_reporter);
    } else {
      fprintf(stderr, "Parse error: %s\n",
              parser->error_message ? parser->error_message : "Unknown error");
    }
    return 0;
  }
  return 1;
}

static int compile_resolve_imports(ASTNode *program, const char *input_filename,
                                   ErrorReporter *error_reporter,
                                   ImportResolverOptions *import_options) {
  if (!resolve_imports_with_options(program, input_filename, error_reporter,
                                    import_options)) {
    if (error_reporter_has_errors(error_reporter)) {
      error_reporter_print_errors(error_reporter);
    } else {
      fprintf(stderr, "Import resolution error\n");
    }
    return 0;
  }
  return 1;
}

static int compile_monomorphize(ASTNode *program,
                                ErrorReporter *error_reporter) {
  if (!monomorphize_program(program, error_reporter)) {
    if (error_reporter_has_errors(error_reporter)) {
      error_reporter_print_errors(error_reporter);
    } else {
      fprintf(stderr, "Generic monomorphization error\n");
    }
    return 0;
  }
  if (!closure_convert_program(program, error_reporter)) {
    if (error_reporter_has_errors(error_reporter)) {
      error_reporter_print_errors(error_reporter);
    } else {
      fprintf(stderr, "Closure conversion error\n");
    }
    return 0;
  }
  if (!closure_adapt_program(program, error_reporter)) {
    if (error_reporter_has_errors(error_reporter)) {
      error_reporter_print_errors(error_reporter);
    } else {
      fprintf(stderr, "Closure adaptation error\n");
    }
    return 0;
  }
  return 1;
}

static const char *expand_annotate(void *context, const ASTNode *block) {
  if (block && block->type == AST_STRUCT_DECLARATION && block->data) {
    const TypeMetaLayout *layout =
        ((const StructDeclaration *)block->data)->meta_layout;
    static char note[4096];
    size_t used = 0;
    if (!layout) {
      return NULL;
    }
    used += (size_t)snprintf(note, sizeof(note), "layout from @%s, %zu region%s:",
                             layout->written_by ? layout->written_by : "?",
                             layout->region_count,
                             layout->region_count == 1 ? "" : "s");
    for (size_t r = 0; r < layout->region_count && used < sizeof(note); r++) {
      used += (size_t)snprintf(note + used, sizeof(note) - used,
                               " [stride %zu align %zu]",
                               layout->regions[r].stride,
                               layout->regions[r].alignment);
    }
    for (size_t p = 0; p < layout->place_count && used < sizeof(note); p++) {
      const TypeMetaPlace *place = &layout->places[p];
      used += (size_t)snprintf(note + used, sizeof(note) - used,
                               " %s=%u+%zu", place->field, place->region,
                               place->offset);
      if (place->width > 0 && used < sizeof(note)) {
        used += (size_t)snprintf(note + used, sizeof(note) - used,
                                 ".%u:%u<<%u", place->bit, place->width,
                                 place->scale);
      }
    }
    if (layout->allocator && used < sizeof(note)) {
      snprintf(note + used, sizeof(note) - used, " allocated by %s",
               layout->allocator);
    }
    return note;
  }
  return type_checker_expansion_note((TypeChecker *)context, block, NULL);
}

static int explain_rule_walk(const ASTNode *node, const char *code,
                             int *found) {
  if (!node) {
    return 0;
  }
  if (node->type == AST_FUNCTION_DECLARATION && node->data) {
    const FunctionDeclaration *fd = (const FunctionDeclaration *)node->data;
    if (fd->explain_code && strcmp(fd->explain_code, code) == 0) {
      printf("%s: rule `%s`\n\n%s\n", fd->explain_code,
             fd->name ? fd->name : "?",
             fd->explain_text ? fd->explain_text : "(no text)");
      *found = 1;
    }
  }
  for (size_t i = 0; i < node->child_count; i++) {
    explain_rule_walk(node->children[i], code, found);
  }
  return *found;
}

static int explain_rule_code(const char *code, const char *path) {
  char *source = NULL;
  ErrorReporter *reporter = NULL;
  Lexer *lexer = NULL;
  Parser *parser = NULL;
  ASTNode *program = NULL;
  int found = 0;
  if (!code || !path) {
    return 1;
  }
  source = read_file(path);
  if (!source) {
    fprintf(stderr, "Error: could not read '%s'\n", path);
    return 1;
  }
  reporter = error_reporter_create(path, source);
  lexer = lexer_create(source);
  parser = lexer && reporter
               ? parser_create_with_error_reporter(lexer, reporter)
               : NULL;
  if (parser) {
    program = parser_parse_program(parser);
  }
  if (program) {
    explain_rule_walk(program, code, &found);
  }
  if (!found) {
    fprintf(stderr,
            "no rule in '%s' carries the code %s; a rule declares one with "
            "`explain %s \"...\"` after its signature\n",
            path, code, code);
  }
  if (program) {
    ast_destroy_node(program);
  }
  if (parser) {
    parser_destroy(parser);
  }
  if (lexer) {
    lexer_destroy(lexer);
  }
  if (reporter) {
    error_reporter_destroy(reporter);
  }
  free(source);
  return found ? 0 : 1;
}

static int compile_type_check(TypeChecker *type_checker, ASTNode *program,
                              ErrorReporter *error_reporter) {
  if (!type_checker_check_program(type_checker, program)) {
    if (error_reporter_has_errors(error_reporter)) {
      error_reporter_print_errors(error_reporter);
    } else {
      fprintf(stderr, "Type error: %s\n",
              type_checker->error_message ? type_checker->error_message
                                          : "Unknown error");
    }
    return 0;
  }
  return 1;
}

static IRFunction *swap_find_function(IRProgram *program, const char *name) {
  if (!program || !name) {
    return NULL;
  }
  for (size_t i = 0; i < program->function_count; i++) {
    IRFunction *fn = program->functions[i];
    if (fn && fn->name && strcmp(fn->name, name) == 0) {
      return fn;
    }
  }
  return NULL;
}

static int swap_signatures_match(const IRFunction *old_fn,
                                 const IRFunction *new_fn, char *why,
                                 size_t why_capacity) {
  if (old_fn->parameter_count != new_fn->parameter_count) {
    snprintf(why, why_capacity,
             "parameter count differs: '%s' takes %zu, '%s' takes %zu",
             old_fn->name, old_fn->parameter_count, new_fn->name,
             new_fn->parameter_count);
    return 0;
  }
  for (size_t i = 0; i < old_fn->parameter_count; i++) {
    const char *a = old_fn->parameter_types ? old_fn->parameter_types[i] : NULL;
    const char *b = new_fn->parameter_types ? new_fn->parameter_types[i] : NULL;
    if ((a == NULL) != (b == NULL) || (a && b && strcmp(a, b) != 0)) {
      snprintf(why, why_capacity,
               "parameter %zu differs: '%s' takes '%s', '%s' takes '%s'",
               i + 1, old_fn->name, a ? a : "?", new_fn->name, b ? b : "?");
      return 0;
    }
  }
  const char *ra = old_fn->return_type_name;
  const char *rb = new_fn->return_type_name;
  if ((ra == NULL) != (rb == NULL) || (ra && rb && strcmp(ra, rb) != 0)) {
    snprintf(why, why_capacity,
             "return type differs: '%s' returns '%s', '%s' returns '%s'",
             old_fn->name, ra ? ra : "?", new_fn->name, rb ? rb : "?");
    return 0;
  }
  return 1;
}

static int compile_run_swap_check(IRProgram *ir_program,
                                  const char *old_name,
                                  const char *new_name) {
  IRFunction *old_fn = swap_find_function(ir_program, old_name);
  IRFunction *new_fn = swap_find_function(ir_program, new_name);
  if (!old_fn) {
    fprintf(stderr, "swap-check: no function named '%s' in this program\n",
            old_name);
    return 1;
  }
  if (!new_fn) {
    fprintf(stderr, "swap-check: no function named '%s' in this program\n",
            new_name);
    return 1;
  }
  if (old_fn == new_fn) {
    fprintf(stderr,
            "swap-check: --old and --new name the same function ('%s')\n",
            old_name);
    return 1;
  }

  char why[512];
  why[0] = '\0';
  if (!swap_signatures_match(old_fn, new_fn, why, sizeof(why))) {
    fprintf(stderr, "swap-check: REFUSED - %s\n", why);
    fprintf(stderr,
            "  A swap has to keep the boundary it replaces. Change the "
            "signature and the callers change with it, which is a rebuild "
            "rather than a swap.\n");
    return 1;
  }

  IRVerifySnapshot *before = ir_verify_snapshot_capture(old_fn);
  if (!before) {
    fprintf(stderr, "swap-check: could not snapshot '%s'\n", old_name);
    return 1;
  }

  char divergence[512];
  char counterexample[512];
  char skip_reason[256];
  IRVerifyRewriteVerdict verdict = ir_verify_check_rewrite(
      ir_program, new_fn, before, divergence, sizeof(divergence),
      counterexample, sizeof(counterexample), skip_reason,
      sizeof(skip_reason));
  ir_verify_snapshot_free(before);

  switch (verdict) {
  case IR_VERIFY_REWRITE_VALIDATED:
    printf("swap-check: OK - '%s' matched '%s' on %d generated input sets\n",
           new_name, old_name, ir_verify_last_input_run_count());
    printf("  Inputs cover a fixed shape table plus the constants these two "
           "functions compare against, tested on both sides of each.\n");
    printf("  This is a differential test, not a proof: behavior on inputs "
           "outside those sets was not observed.\n");
    return 0;
  case IR_VERIFY_REWRITE_DIVERGED:
    fprintf(stderr, "swap-check: DIVERGED - '%s' does not match '%s'\n",
            new_name, old_name);
    fprintf(stderr, "  %s\n", divergence);
    if (counterexample[0]) {
      fprintf(stderr, "  %s\n", counterexample);
    }
    return 1;
  case IR_VERIFY_REWRITE_UNVERIFIABLE:
  default:
    fprintf(stderr,
            "swap-check: UNVERIFIABLE - the gate could not run '%s': %s\n",
            new_name, skip_reason[0] ? skip_reason : "unknown");
    return 2;
  }
}

IRFunction *ir_program_find_function(IRProgram *program, const char *name);

#define MACHINE_FUEL 20000000LL
#define MACHINE_STATE_WORDS 8
#define MACHINE_CODE_MAX 4096
#define MACHINE_STEP_MAX 1000000
#define MACHINE_LINE_MAX 256

static size_t machine_program_lines(ASTNode *program_node, const char **out,
                                    size_t capacity) {
  Program *program = program_node ? (Program *)program_node->data : NULL;
  size_t count = 0;
  for (size_t i = 0; program && i < program->declaration_count; i++) {
    ASTNode *declaration = program->declarations[i];
    VarDeclaration *variable = NULL;
    AggregateLiteral *rows = NULL;
    if (!declaration || declaration->type != AST_VAR_DECLARATION) {
      continue;
    }
    variable = (VarDeclaration *)declaration->data;
    if (!variable || !variable->name || strcmp(variable->name, "PROGRAM") != 0) {
      continue;
    }
    rows = variable->initializer &&
                   variable->initializer->type == AST_AGGREGATE_LITERAL
               ? (AggregateLiteral *)variable->initializer->data
               : NULL;
    if (!rows || rows->is_struct) {
      return 0;
    }
    for (size_t k = 0; k < rows->element_count && count < capacity; k++) {
      ASTNode *row = rows->elements[k];
      if (!row || row->type != AST_STRING_LITERAL || !row->data) {
        return 0;
      }
      out[count++] = ((StringLiteral *)row->data)->value;
    }
    return count;
  }
  return 0;
}

static size_t machine_drain_output(IRInterpMachine *interp, size_t from) {
  size_t count = ir_interp_extern_trace_count(interp);
  int wrote = 0;
  for (size_t i = from; i < count; i++) {
    const IRInterpExternCall *call = ir_interp_extern_trace(interp, i);
    size_t which = 0;
    if (!call) {
      continue;
    }
    if (strcmp(call->name, "write") == 0) {
      which = 1;
    } else if (strcmp(call->name, "fwrite") == 0 ||
               strcmp(call->name, "puts") == 0 ||
               strcmp(call->name, "fputs") == 0) {
      which = 0;
    } else {
      continue;
    }
    if (which < call->arg_count && call->arg_mem_len[which] > 0) {
      size_t length = call->arg_mem_len[which];
      if (strcmp(call->name, "write") == 0 && call->arg_count >= 3 &&
          call->args[2].i >= 0 && (size_t)call->args[2].i < length) {
        length = (size_t)call->args[2].i;
      }
      if (strcmp(call->name, "fwrite") == 0 && call->arg_count >= 3) {
        long long span = call->args[1].i * call->args[2].i;
        if (span >= 0 && (size_t)span < length) {
          length = (size_t)span;
        }
      }
      while (length > 0 && call->arg_mem[which][length - 1] == 0) {
        length--;
      }
      fwrite(call->arg_mem[which], 1, length, stdout);
      wrote = 1;
    }
  }
  if (wrote) {
    printf("\n");
  }
  return count;
}

static int machine_emulate(const MachineDesc *desc, ASTNode *program_node,
                           IRProgram *ir_program) {
  const char *lines[MACHINE_CODE_MAX];
  size_t starts[MACHINE_CODE_MAX];
  unsigned char code[MACHINE_CODE_MAX];
  unsigned char again[MACHINE_CODE_MAX];
  size_t line_count = machine_program_lines(program_node, lines,
                                            MACHINE_CODE_MAX);
  size_t length = 0;
  size_t steps = 0;
  size_t at = 0;
  size_t printed = 0;
  IRInterpMachine *interp = NULL;
  char error[256];
  if (line_count == 0) {
    fprintf(stderr,
            "error[N0002]: this file describes a machine and no program to "
            "run on it; a program is `const PROGRAM: string[N]`, one "
            "assembly line per row\n");
    return 1;
  }
  for (size_t i = 0; i < line_count; i++) {
    size_t written = 0;
    starts[i] = length;
    if (!machine_assemble(desc, lines[i], code + length,
                          MACHINE_CODE_MAX - length, &written, error,
                          sizeof(error))) {
      fprintf(stderr, "error[N0003]: line %zu of PROGRAM: %s\n", i + 1, error);
      return 1;
    }
    length += written;
  }

  {
    size_t cursor = 0;
    size_t out = 0;
    while (cursor < length) {
      const MachineInsn *insn = NULL;
      long long operands[MACHINE_OPERANDS];
      size_t consumed = 0;
      if (!machine_decode(desc, code, length, cursor, &insn, operands,
                          &consumed)) {
        fprintf(stderr,
                "error[N0004]: nothing in '%s' decodes the byte at offset "
                "%zu, so what it assembles is not what it can read back\n",
                desc->name, cursor);
        return 1;
      }
      for (size_t k = 0; k < insn->length; k++) {
        again[out + k] = insn->slot[k] >= 0
                             ? (unsigned char)operands[insn->slot[k]]
                             : insn->bytes[k];
      }
      out += insn->length;
      cursor += consumed;
    }
    if (out != length || memcmp(code, again, length) != 0) {
      fprintf(stderr,
              "error[N0004]: '%s' does not round-trip: what it assembled and "
              "what it decoded back are different bytes\n",
              desc->name);
      return 1;
    }
  }

  interp = ir_interp_create(ir_program);
  if (!interp) {
    fprintf(stderr, "error[N0005]: could not start the interpreter\n");
    return 1;
  }
  while (at < length) {
    const MachineInsn *insn = NULL;
    long long operands[MACHINE_OPERANDS];
    size_t consumed = 0;
    IRFunction *body = NULL;
    IRInterpValue args[MACHINE_OPERANDS];
    IRInterpValue answer;
    IRInterpStatus status;
    if (steps++ >= MACHINE_STEP_MAX) {
      fprintf(stderr,
              "error[N0006]: '%s' ran %d instructions without halting\n",
              desc->name, MACHINE_STEP_MAX);
      ir_interp_destroy(interp);
      return 1;
    }
    if (!machine_decode(desc, code, length, at, &insn, operands, &consumed)) {
      fprintf(stderr,
              "error[N0004]: nothing in '%s' decodes the byte at offset %zu\n",
              desc->name, at);
      ir_interp_destroy(interp);
      return 1;
    }
    body = ir_program_find_function(ir_program, insn->semantics);
    if (!body) {
      fprintf(stderr,
              "error[N0005]: '%s' says '%s' is what it does, and there is no "
              "such function to run\n",
              insn->name, insn->semantics);
      ir_interp_destroy(interp);
      return 1;
    }
    for (int k = 0; k < MACHINE_OPERANDS; k++) {
      memset(&args[k], 0, sizeof(args[k]));
      args[k].i = operands[k];
    }
    memset(&answer, 0, sizeof(answer));
    status = ir_interp_run(interp, body, args, MACHINE_OPERANDS, &answer,
                           MACHINE_FUEL);
    if (status != IR_INTERP_OK) {
      fprintf(stderr,
              "error[N0005]: '%s' at offset %zu did not finish; its semantics "
              "function trapped or ran out of steps\n",
              insn->name, at);
      ir_interp_destroy(interp);
      return 1;
    }
    printed = machine_drain_output(interp, printed);
    if (answer.i < 0) {
      at += consumed;
      continue;
    }
    if ((size_t)answer.i >= line_count) {
      break;
    }
    at = starts[answer.i];
  }
  {
    size_t globals = ir_interp_global_count(interp);
    for (size_t i = 0; i < globals; i++) {
      const char *name = ir_interp_global_name(interp, i);
      IRInterpValue value = ir_interp_global_value(interp, i);
      if (!name) {
        continue;
      }
      if (value.is_float) {
        printf("  %s = %f\n", name, value.f);
        continue;
      }
      {
        unsigned char words[MACHINE_STATE_WORDS * 8];
        long long read = ir_interp_read_bytes(
            interp, (unsigned long long)value.i, words, sizeof(words));
        if (read == (long long)sizeof(words)) {
          printf("  %s =", name);
          for (size_t k = 0; k < MACHINE_STATE_WORDS; k++) {
            long long word = 0;
            memcpy(&word, words + k * 8, 8);
            printf(" %lld", word);
          }
          printf("\n");
          continue;
        }
      }
      printf("  %s = %lld\n", name, value.i);
    }
  }
  ir_interp_destroy(interp);
  printf("%s: %zu instructions in %zu bytes, %zu executed\n", desc->name,
         line_count, length, steps);
  return 0;
}

static void live_record_global_layouts(const IRProgram *program) {
  const char **names = calloc(program->module_symbol_count + 1u,
                              sizeof(const char *));
  char **layouts = calloc(program->module_symbol_count + 1u, sizeof(char *));
  size_t count = 0;
  if (names && layouts) {
    for (size_t i = 0; i < program->module_symbol_count; i++) {
      const IRModuleSymbol *symbol = &program->module_symbols[i];
      if (symbol->kind != IR_MODSYM_VARIABLE || symbol->is_extern ||
          !symbol->type || !symbol->name) {
        continue;
      }
      layouts[count] = ir_lower_live_descriptor(symbol->type);
      if (layouts[count]) {
        names[count] = symbol->name;
        count++;
      }
    }
    live_set_global_layouts(names, (const char *const *)layouts, count);
  }
  for (size_t i = 0; layouts && i < count; i++) {
    free(layouts[i]);
  }
  free(names);
  free(layouts);
}

static int compile_lower_to_ir(ASTNode *program, TypeChecker *type_checker,
                               SymbolTable *symbol_table,
                               int emit_runtime_checks, int emit_safety_checks,
                               IRProgram **out_ir_program,
                               char **out_ir_error) {
  *out_ir_program =
      ir_lower_program(program, type_checker, symbol_table, out_ir_error,
                       emit_runtime_checks, emit_safety_checks);
  if (*out_ir_program) {
    const char *const *type_names = NULL;
    const char *const *type_layouts = NULL;
    size_t type_count = ir_lower_live_type_layouts(&type_names, &type_layouts);
    live_record_global_layouts(*out_ir_program);
    live_set_type_layouts(type_names, type_layouts, type_count);
  }
  if (!*out_ir_program) {
    if (type_checker && type_checker->error_reporter &&
        error_reporter_has_errors(type_checker->error_reporter)) {
      error_reporter_print_errors(type_checker->error_reporter);
      return 0;
    }
    mettle_compiler_ice_report("IR lowering failed",
                               *out_ir_error ? *out_ir_error : NULL);
    return 0;
  }
  return 1;
}

#include "ir/ml_opt.h"

static IRGlobalIntConst *collect_global_int_consts(ASTNode *program,
                                                   size_t *out_count) {
  *out_count = 0;
  if (!program || program->type != AST_PROGRAM || !program->data) {
    return NULL;
  }
  Program *prog = (Program *)program->data;
  IRGlobalIntConst *consts = NULL;
  size_t count = 0, capacity = 0;
  for (size_t i = 0; i < prog->declaration_count; i++) {
    ASTNode *decl = prog->declarations[i];
    if (!decl || decl->type != AST_VAR_DECLARATION || !decl->data) {
      continue;
    }
    VarDeclaration *vd = (VarDeclaration *)decl->data;
    if (!vd->name || !vd->type_name || vd->is_extern || vd->is_exported ||
        vd->link_name || !vd->initializer) {
      continue;
    }
    if (strcmp(vd->type_name, "int8") != 0 &&
        strcmp(vd->type_name, "int16") != 0 &&
        strcmp(vd->type_name, "int32") != 0 &&
        strcmp(vd->type_name, "int64") != 0 &&
        strcmp(vd->type_name, "uint8") != 0 &&
        strcmp(vd->type_name, "uint16") != 0 &&
        strcmp(vd->type_name, "uint32") != 0 &&
        strcmp(vd->type_name, "uint64") != 0) {
      continue;
    }
    ASTNode *init = vd->initializer;
    long long sign = 1;
    if (init->type == AST_UNARY_EXPRESSION && init->data) {
      UnaryExpression *ue = (UnaryExpression *)init->data;
      if (!ue->operator|| strcmp(ue->operator, "-") != 0 || !ue->operand) {
        continue;
      }
      sign = -1;
      init = ue->operand;
    }
    if (init->type != AST_NUMBER_LITERAL || !init->data) {
      continue;
    }
    NumberLiteral *nl = (NumberLiteral *)init->data;
    if (nl->is_float) {
      continue;
    }
    if (count >= capacity) {
      size_t nc = capacity ? capacity * 2 : 16;
      IRGlobalIntConst *grown =
          (IRGlobalIntConst *)realloc(consts, nc * sizeof(*grown));
      if (!grown) {
        free(consts);
        return NULL;
      }
      consts = grown;
      capacity = nc;
    }
    consts[count].name = vd->name;
    consts[count].value = sign * nl->int_value;
    count++;
  }
  *out_count = count;
  return consts;
}

static int compile_targets_arm64_object(const CompilerOptions *options) {
#if defined(__aarch64__) || defined(_M_ARM64)
  (void)options;
  return 1;
#else
  return options && options->emit_arm64_obj;
#endif
}

static void compile_publish_optimizer_costs(void) {
  const MtlcTargetDescription *machine = mtlc_target_current_description();
  IROptCost cost;
  if (!machine) {
    ir_opt_cost_reset();
    return;
  }
  cost.op = machine->cost_op;
  cost.load = machine->cost_load;
  cost.store = machine->cost_store;
  cost.branch = machine->cost_branch;
  cost.multiply = machine->cost_multiply;
  cost.multiply_float = machine->cost_multiply_float;
  cost.divide = machine->cost_divide;
  cost.divide_float = machine->cost_divide_float;
  cost.call = machine->cost_call;
  cost.allocate = machine->cost_allocate;
  cost.vector_width = machine->vector_width;
  cost.subgroup_width = machine->subgroup_width;
  ir_opt_cost_describe(&cost);
}

static int compile_check_numerics(IRProgram *ir_program,
                                  const CompilerOptions *options) {
  IRNumericsFailure failure;
  char *report = NULL;
  if (options->test_mode || !ir_numerics_program_has_contracts(ir_program)) {
    return 1;
  }
  if (!ir_numerics_check(ir_program, &failure, &report)) {
    fprintf(stderr, "error[%s]: %s\n  --> %s\n", failure.code,
            failure.message,
            options->input_filename ? options->input_filename : "?");
    free(report);
    return 0;
  }
  if (report && (options->explain || options->report_gpu_types)) {
    fputs(report, stderr);
  }
  free(report);
  ir_program_drop_numerics_harnesses(ir_program);
  return 1;
}

static int compile_optimize_ir(IRProgram *ir_program, ASTNode *ast_program,
                               const CompilerOptions *options) {
  IROptimizeOptions ir_optimize_options = {0};
  compile_publish_optimizer_costs();
  int target_neutral = options->emit_arm64 || options->emit_ptx ||
                       options->emit_spirv || options->emit_metal ||
                       compile_targets_arm64_object(options);
  if (options->ml_opt && target_neutral) {
    fprintf(stderr,
            "Error: --ml-opt is not target-neutral and cannot be combined "
            "with --emit-arm64, --emit-arm64-obj, --emit-ptx, "
            "--emit-spirv, or --emit-metal%s\n",
            compile_targets_arm64_object(options) && !options->emit_arm64_obj
                ? " (this host emits AArch64 objects)"
                : "");
    return 0;
  }
  ir_optimize_options.preserve_function_boundaries =
      options->profile_runtime ? 1 : 0;
  ir_optimize_options.simd_report = options->simd_report;
  ir_optimize_options.explain = options->explain;
  ir_optimize_options.explain_focus_file =
      options->explain_all ? NULL : options->input_filename;
  ir_explain_set_output_path(options->output_filename);
  ir_explain_set_json(options->explain_json ? 1 : 0);
  ir_explain_set_filter(options->explain_filter);
  if (options->explain_json && !options->annotate_asm) {
    mir_annotate_set_enabled(1);
    mir_annotate_set_cost_only(1);
    mir_annotate_set_output_path(options->output_filename);
    mir_annotate_set_source_file(options->input_filename);
  }
  if (options->explain_json) {
    ir_explain_set_retain_remarks(1);
  }
  if (options->annotate_asm) {
    mir_annotate_set_enabled(1);
    mir_annotate_set_syntax((MirAnnotSyntax)options->asm_syntax);
    mir_annotate_set_output_path(options->output_filename);
    mir_annotate_set_source_file(options->input_filename);
    if (options->annotate_q_lo)
      mir_annotate_set_line_query(options->annotate_q_lo, options->annotate_q_hi,
                                  options->annotate_q_fn);
    else if (options->annotate_q_fn)
      mir_annotate_set_line_query(0, 0, options->annotate_q_fn);
    if (options->annotate_hot) mir_annotate_set_hot_query(options->annotate_hot);
    ir_explain_set_retain_remarks(1);
  }
  size_t global_const_count = 0;
  IRGlobalIntConst *global_consts =
      options->live ? NULL
                    : collect_global_int_consts(ast_program, &global_const_count);
  ir_optimize_options.global_int_consts = global_consts;
  ir_optimize_options.global_int_const_count = global_const_count;
  ir_optimize_options.whole_program = options->building_executable;
  ir_optimize_options.target_neutral_only = target_neutral;
  ir_optimize_options.gpu_device_only =
      options->emit_ptx || options->emit_spirv || options->emit_metal;
  int opt_ok = ir_optimize_program(ir_program, &ir_optimize_options);
  free(global_consts);
  if (opt_ok) {
    ir_program_drop_rewrite_rules(ir_program);
  }
  if (opt_ok && options->safe && !options->emit_ptx && !options->emit_spirv &&
      !options->emit_metal && !ir_safety_retire_dangling_notes(ir_program)) {
    mettle_compiler_ice_report("Failed to retire --safe stack notes", NULL);
    return 0;
  }
  if (!opt_ok) {
    if (!ir_optimize_had_user_error()) {
      mettle_compiler_ice_report("IR optimization failed", NULL);
    }
    return 0;
  }
  if (!compile_check_numerics(ir_program, options)) {
    return 0;
  }
  if (options->ml_opt) {
    MLOptStats ml = {0};
    ir_apply_ml_opt(ir_program, &ml);
    int hoisted = ir_hoist_constants(ir_program);
    fprintf(stderr, "--ml-opt: %d model proposal%s", ml.proposals,
            ml.proposals == 1 ? "" : "s");
    if (ml.proposals > 0) {
      fprintf(stderr, ": %d applied (%d validated equivalent, %d proven-only)",
              ml.validated + ml.proven, ml.validated, ml.proven);
      if (ml.rejected > 0) {
        fprintf(stderr, ", %d REJECTED by the validator", ml.rejected);
      }
      if (ml.skipped > 0) {
        fprintf(stderr, ", %d skipped", ml.skipped);
      }
    }
    fprintf(stderr, "; hoisted %d large constants\n", hoisted);
    if (options->explain) {
      ir_explain_ml_opt("_mlopt.explain");
    }
  }
  return 1;
}

static int compile_generate_code(CodeGenerator *code_generator) {
  if (!code_generator_generate_program(code_generator)) {
    const char *message = (code_generator && code_generator->error_message)
                              ? code_generator->error_message
                              : "Unknown error";
    if (code_generator && code_generator->has_user_error) {
      fprintf(stderr, "error: %s\n", message);
      return 0;
    }
    fprintf(stderr, "Code generation error: %s\n", message);
    mettle_compiler_ice_report("Code generation failed",
                               code_generator && code_generator->error_message
                                   ? code_generator->error_message
                                   : NULL);
    return 0;
  }
  return 1;
}

static void compile_dump_device_ir(IRProgram *program,
                                   const char *output_filename) {
  char *ir_output = build_sidecar_filename(output_filename, ".ir");
  if (!ir_output) {
    fprintf(stderr,
            "Warning: Failed to allocate IR output filename for '%s'\n",
            output_filename ? output_filename : "<device module>");
    return;
  }
  FILE *ir_file = fopen(ir_output, "w");
  if (!ir_file) {
    fprintf(stderr, "Warning: Could not create IR file '%s': %s\n",
            ir_output, strerror(errno));
  } else {
    if (!ir_program_dump(program, ir_file)) {
      fprintf(stderr, "Warning: Failed to write IR dump to '%s'\n",
              ir_output);
    }
    fclose(ir_file);
  }
  free(ir_output);
}

static void compile_dump_ast(ASTNode *program, const char *output_filename) {
  char *ast_output = build_sidecar_filename(output_filename, ".ast");
  if (!ast_output) {
    fprintf(stderr,
            "Warning: Failed to allocate AST output filename for '%s'\n",
            output_filename ? output_filename : "<output>");
    return;
  }
  FILE *ast_file = fopen(ast_output, "w");
  if (!ast_file) {
    fprintf(stderr, "Warning: Could not create AST file '%s': %s\n",
            ast_output, strerror(errno));
  } else {
    int ast_ok = ast_dump_program(ast_file, program);
    if (fclose(ast_file) != 0) {
      ast_ok = 0;
    }
    if (!ast_ok) {
      fprintf(stderr, "Warning: Failed to write AST dump to '%s'\n",
              ast_output);
    }
  }
  free(ast_output);
}

typedef struct {
  IRProgram *program;
  TypeChecker *type_checker;
  ErrorReporter *reporter;
  const char *filename;
  FILE *report;
  long long budget;
} TraceRuleContext;

static int trace_line_load(char *line) {
  char *field[6];
  size_t count = 0;
  char *at = line;
  while (count < 6) {
    field[count++] = at;
    at = strchr(at, '|');
    if (!at) {
      break;
    }
    *at++ = '\0';
  }
  if (count != 6) {
    return 0;
  }
  ir_trace_record(field[0], field[1], field[2][0] ? field[2] : NULL,
                  (size_t)strtoull(field[3], NULL, 10),
                  (size_t)strtoull(field[4], NULL, 10),
                  strtoll(field[5], NULL, 10));
  return 1;
}

static int trace_file_load(const char *path, size_t *out_events,
                           size_t *out_dropped) {
  char *text = read_file(path);
  char *at = text;
  size_t events = 0;
  size_t dropped = 0;
  if (!text) {
    fprintf(stderr, "Error: could not read the recorded trace '%s'\n", path);
    return 0;
  }
  ir_trace_set_collect(1);
  ir_trace_begin(path);
  while (at && *at) {
    char *end = strchr(at, '\n');
    if (end) {
      *end = '\0';
    }
    while (*at == '\r') {
      at++;
    }
    {
      size_t length = strlen(at);
      while (length > 0 && (at[length - 1] == '\r' || at[length - 1] == ' ')) {
        at[--length] = '\0';
      }
    }
    if (*at) {
      if (strncmp(at, "dropped|", 8) == 0) {
        dropped++;
      }
      if (!trace_line_load(at)) {
        fprintf(stderr,
                "Error: '%s' is not a recorded trace: a line is not "
                "kind|name|file|line|column|value\n",
                path);
        free(text);
        return 0;
      }
      events++;
    }
    at = end ? end + 1 : NULL;
  }
  free(text);
  if (out_events) {
    *out_events = events;
  }
  if (out_dropped) {
    *out_dropped = dropped;
  }
  return 1;
}

static int compile_run_trace_rules(void *raw, const char *test_name) {
  TraceRuleContext *ctx = (TraceRuleContext *)raw;
  IRRuleImage image;
  IRRuleStats stats;
  char *error = NULL;
  int ok;
  (void)test_name;
  if (!ctx || !ir_program_has_rules_of(ctx->program, IR_RULE_OVER_TRACE)) {
    return 1;
  }
  if (!rule_reflect_build_trace(ctx->type_checker, ctx->filename, &image,
                                &error)) {
    fprintf(stderr, "Error: %s\n",
            error ? error : "could not reflect the trace");
    free(error);
    return 0;
  }
  ok = ir_rules_run_kind(ctx->program, &image, ctx->reporter, ctx->report,
                         ctx->budget, 0, IR_RULE_OVER_TRACE, &stats);
  ir_rule_image_free(&image);
  if (!ok) {
    error_reporter_print_errors(ctx->reporter);
  }
  return ok;
}

static TraceRuleContext g_trace_rule_context;

static int compile_run_comptime(IRProgram *ir_program, ASTNode *program,
                                const CompilerOptions *options,
                                ErrorReporter *error_reporter,
                                const char *input_filename,
                                const char *source) {
  if (options->optimize && !compile_optimize_ir(ir_program, program, options)) {
    return 1;
  }
  ir_program_drop_rewrite_rules(ir_program);
  ir_interp_set_purity_fault(options->check_purity_fault);
  if (options->test_mode) {
    int status;
    if (ir_program_has_rules_of(ir_program, IR_RULE_OVER_TRACE)) {
      ir_trace_set_collect(1);
      g_trace_rule_context.program = ir_program;
      g_trace_rule_context.type_checker = (TypeChecker *)options->trace_rule_checker;
      g_trace_rule_context.reporter = error_reporter;
      g_trace_rule_context.filename = input_filename;
      g_trace_rule_context.report = options->report_rules ? stdout : NULL;
      g_trace_rule_context.budget =
          options->rule_budget_set ? options->rule_budget : 0;
      ir_comptime_set_trace_rules(compile_run_trace_rules,
                                  &g_trace_rule_context);
    }
    status = ir_comptime_run_tests(ir_program, error_reporter, input_filename,
                                   options->test_filter);
    ir_comptime_set_trace_rules(NULL, NULL);
    ir_trace_set_collect(0);
    ir_trace_reset();
    return status;
  }
  return ir_comptime_trace(ir_program, error_reporter, input_filename, source,
                           options->trace_function, options->trace_args,
                           options->trace_arg_count);
}

static int compile_optimize_device_ir(IRProgram *ir_program, ASTNode *program,
                                      const CompilerOptions *options,
                                      CompilerProfile *profile) {
  double phase_start;
  int opt_ok;
  if (!options->optimize) {
    return compile_check_numerics(ir_program, options);
  }
  compiler_set_phase(PROFILE_PHASE_IR_OPTIMIZATION);
  phase_start = compiler_profile_begin(profile);
  opt_ok = compile_optimize_ir(ir_program, program, options);
  compiler_profile_add(profile, PROFILE_PHASE_IR_OPTIMIZATION, phase_start);
  return opt_ok;
}

static int compile_write_kernel_declarations(ASTNode *program,
                                             const CompilerOptions *options,
                                             const char *input_filename,
                                             const char *output_filename) {
  char decls_path[1024];
  if (!options->emit_kernel_decls) {
    return 1;
  }
  if (options->emit_kernel_decls[0]) {
    snprintf(decls_path, sizeof(decls_path), "%s", options->emit_kernel_decls);
  } else {
    snprintf(decls_path, sizeof(decls_path), "%s.mettle", output_filename);
  }
  return write_kernel_declarations(program, decls_path, input_filename);
}

static int compile_emit_ptx(IRProgram *ir_program, ASTNode *program,
                            CodeGenerator *code_generator,
                            const CompilerOptions *options,
                            const char *input_filename,
                            const char *output_filename,
                            CompilerProfile *profile) {
  FILE *ptx_out;
  char *ptx_err = NULL;
  PtxEmitOptions ptx_options = {options->ptx_target, options->ptx_isa_major,
                                options->ptx_isa_minor,
                                options->ptx_tensor_tuple_budget,
                                options->gpu_checks,
                                options->report_gpu_types};
  int ok;

  if (!ir_program_bind_device_asm(ir_program, &ptx_err)) {
    fprintf(stderr, "Error: PTX emission failed: %s\n",
            ptx_err ? ptx_err : "unknown");
    free(ptx_err);
    return 1;
  }
  if (!compile_optimize_device_ir(ir_program, program, options, profile)) {
    return 1;
  }
  if (options->dump_ir) {
    compile_dump_device_ir(ir_program, output_filename);
  }
  ptx_out = fopen(output_filename, "w");
  if (!ptx_out) {
    fprintf(stderr, "Error: could not open PTX output '%s'\n", output_filename);
    return 1;
  }
  ok = ptx_emit_program(ir_program, code_generator, ptx_out, &ptx_options,
                        &ptx_err);
  fclose(ptx_out);
  if (!ok) {
    if (ptx_err && strlen(ptx_err) > 7 && ptx_err[0] >= 'A' &&
        ptx_err[0] <= 'Z' && ptx_err[5] == ':' && ptx_err[6] == ' ') {
      fprintf(stderr, "error[%.5s]: %s\n  --> %s\n", ptx_err, ptx_err + 7,
              input_filename ? input_filename : "?");
    } else {
      fprintf(stderr, "Error: PTX emission failed: %s\n",
              ptx_err ? ptx_err : "unknown");
    }
    free(ptx_err);
    return 1;
  }
  if (options->explain && options->optimize) {
    ir_explain_target_flush("PTX");
  }
  if (!confirm_tile_residency(ir_program, output_filename, options->ptx_target,
                              input_filename)) {
    remove(output_filename);
    return 1;
  }
  if ((options->explain || options->report_gpu_types) &&
      ptx_tile_report_text()[0]) {
    printf("register tiles\n%s\n", ptx_tile_report_text());
  }
  printf("Generated PTX: %s\n", output_filename);
  if (!compile_write_kernel_declarations(program, options, input_filename,
                                         output_filename)) {
    return 1;
  }
  if (options->report_occupancy) {
    int sm_count = options->report_sms;
    int sm_count_is_local = 0;
    if (sm_count <= 0) {
      sm_count = detect_gpu_sm_count();
      sm_count_is_local = sm_count > 0;
    }
    report_ptx_occupancy(ir_program, output_filename, options->ptx_target,
                         sm_count, sm_count_is_local);
  }
  return 0;
}

static int compile_emit_spirv(IRProgram *ir_program, ASTNode *program,
                              CodeGenerator *code_generator,
                              const CompilerOptions *options,
                              const char *output_filename,
                              CompilerProfile *profile) {
  FILE *spv_out;
  char *spv_err = NULL;
  int ok;

  if (!compile_optimize_device_ir(ir_program, program, options, profile)) {
    return 1;
  }
  if (options->dump_ir) {
    compile_dump_device_ir(ir_program, output_filename);
  }
  spv_out = fopen(output_filename, "wb");
  if (!spv_out) {
    fprintf(stderr, "Error: could not open SPIR-V output '%s'\n",
            output_filename);
    return 1;
  }
  ok = spirv_emit_program(ir_program, code_generator, spv_out, &spv_err);
  fclose(spv_out);
  if (!ok) {
    fprintf(stderr, "Error: SPIR-V emission failed: %s\n",
            spv_err ? spv_err : "unknown");
    free(spv_err);
    return 1;
  }
  if (options->explain && options->optimize) {
    ir_explain_target_flush("SPIR-V");
  }
  printf("Generated SPIR-V: %s\n", output_filename);
  return 0;
}

static int compile_emit_metal(IRProgram *ir_program, ASTNode *program,
                              const CompilerOptions *options,
                              const char *input_filename,
                              const char *output_filename,
                              CompilerProfile *profile) {
  FILE *metal_out;
  char *metal_err = NULL;
  MslEmitOptions metal_options;
  int ok;

  msl_emit_default_options(&metal_options);
  if (options->metal_version_major) {
    metal_options.version_major = options->metal_version_major;
    metal_options.version_minor = options->metal_version_minor;
  }
  metal_options.fast_math = options->metal_fast_math;
  metal_options.gpu_checks = options->gpu_checks;
  if (!compile_optimize_device_ir(ir_program, program, options, profile)) {
    return 1;
  }
  if (options->dump_ir) {
    compile_dump_device_ir(ir_program, output_filename);
  }
  metal_out = fopen(output_filename, "w");
  if (!metal_out) {
    fprintf(stderr, "Error: could not open Metal output '%s'\n",
            output_filename);
    return 1;
  }
  ok = msl_emit_program(ir_program, metal_out, &metal_options, &metal_err);
  fclose(metal_out);
  if (!ok) {
    fprintf(stderr, "Error: Metal emission failed: %s\n  --> %s\n",
            metal_err ? metal_err : "unknown",
            input_filename ? input_filename : "?");
    free(metal_err);
    remove(output_filename);
    return 1;
  }
  if (options->explain && options->optimize) {
    ir_explain_target_flush("Metal");
  }
  printf("Generated Metal: %s\n", output_filename);
  if (!compile_write_kernel_declarations(program, options, input_filename,
                                         output_filename)) {
    return 1;
  }
  return 0;
}

static int compile_emit_arm64(IRProgram *ir_program, ASTNode *program,
                              const CompilerOptions *options,
                              const char *output_filename) {
  Arm64Emit ae;
  unsigned char *arm64_data = NULL;
  size_t arm64_data_len = 0;
  int ok;

  if (options->optimize && !compile_optimize_ir(ir_program, program, options)) {
    return 1;
  }
  ir_program_drop_rewrite_rules(ir_program);
  arm64_emit_init(&ae);
  ok = arm64_ir_encode_program(&ae, ir_program, "main", &arm64_data,
                               &arm64_data_len) &&
       arm64_emit_finalize(&ae);
  if (ok) {
    ok = arm64_write_elf(output_filename, ae.code.data, ae.code.len,
                         arm64_data, arm64_data_len);
    if (!ok) {
      fprintf(stderr,
              "Error: could not write AArch64 ELF '%s' (I/O failure, or the "
              "program's %zu bytes of code reach the fixed address of the "
              "writable segment; use the object path for a program this "
              "large)\n",
              output_filename, ae.code.len);
    }
  } else {
    fprintf(stderr, "Error: AArch64 lowering failed: %s\n",
            arm64_error_reason(&ae));
  }
  free(arm64_data);
  arm64_emit_free(&ae);
  if (!ok) {
    return 1;
  }
  printf("Generated AArch64 ELF: %s\n", output_filename);
  return 0;
}

static int compile_wants_debug_info(const CompilerOptions *options) {
  return options->debug_mode || options->generate_debug_symbols ||
         options->generate_line_mapping ||
         options->generate_stack_trace_support ||
         (options->generate_crash_report && options->building_executable);
}

static int compile_wants_debug_sidecar(const CompilerOptions *options) {
  return options->debug_mode || options->generate_debug_symbols ||
         options->generate_line_mapping
             ? 1
             : 0;
}

static void compile_prepend_auto_imports(const CompilerOptions *options,
                                         ASTNode *program) {
  const char *auto_imports[2];
  size_t auto_import_count = 0;
  if (options->prelude) {
    auto_imports[auto_import_count++] = "std/prelude";
  }
  if (options->native_heap) {
    auto_imports[auto_import_count++] = "std/alloc";
  }
  for (size_t ai = 0; ai < auto_import_count; ai++) {
    Program *prog_data = (Program *)program->data;
    SourceLocation auto_loc = {0, 0, NULL};
    ASTNode *auto_import = ast_create_import_declaration(
        auto_imports[ai], NULL, NULL, 0, auto_loc);
    if (auto_import) {
      ASTNode **grown =
          realloc(prog_data->declarations,
                  (prog_data->declaration_count + 1) * sizeof(ASTNode *));
      if (grown) {
        memmove(grown + 1, grown,
                prog_data->declaration_count * sizeof(ASTNode *));
        grown[0] = auto_import;
        prog_data->declarations = grown;
        prog_data->declaration_count++;
        ast_add_child(program, auto_import);
      } else {
        ast_destroy_node(auto_import);
      }
    }
  }
}

int target_argument_is_description(const char *argument) {
  size_t length = argument ? strlen(argument) : 0;
  return length > 7 && strcmp(argument + length - 7, ".mettle") == 0;
}

int load_target_description(CompilerOptions *options) {
  const char *path = options ? options->target_desc_path : NULL;
  char *source = NULL;
  ErrorReporter *reporter = NULL;
  Lexer *lexer = NULL;
  Parser *parser = NULL;
  ASTNode *program = NULL;
  MtlcTargetDescription description;
  char error[512];
  int ok = 0;
  if (!path) {
    return 1;
  }
  source = read_file(path);
  if (!source) {
    fprintf(stderr, "Error: Could not read target description '%s'\n", path);
    return 0;
  }
  reporter = error_reporter_create(path, source);
  lexer = lexer_create(source);
  parser = lexer && reporter ? parser_create_with_error_reporter(lexer, reporter)
                             : NULL;
  if (!lexer || !reporter || !parser) {
    fprintf(stderr, "Error: could not set up to read '%s'\n", path);
    goto done;
  }
  program = parser_parse_program(parser);
  if (!program || parser->had_error || error_reporter_has_errors(reporter)) {
    error_reporter_print_errors(reporter);
    goto done;
  }
  if (!target_desc_read(program, &description, error, sizeof(error))) {
    fprintf(stderr, "Error: target description '%s': %s\n", path, error);
    goto done;
  }
  if (!mtlc_target_describe(&description, error, sizeof(error))) {
    fprintf(stderr, "Error: target description '%s': %s\n", path, error);
    goto done;
  }
  if (mtlc_target()->arch == MTLC_TARGET_ARCH_AARCH64 &&
      !options->emit_arm64) {
    options->emit_arm64_obj = 1;
  }
  ok = 1;
done:
  if (program) {
    ast_destroy_node(program);
  }
  if (parser) {
    parser_destroy(parser);
  }
  if (lexer) {
    lexer_destroy(lexer);
  }
  if (reporter) {
    error_reporter_destroy(reporter);
  } else {
    free(source);
  }
  return ok;
}

static int compile_run_effects(IRProgram *ir_program, TypeChecker *type_checker,
                               const CompilerOptions *options,
                               ErrorReporter *error_reporter,
                               IREffectResults **effect_results) {
  if (!ir_program_declares_effects(ir_program) &&
      type_checker->effect_obligation_count == 0 && !options->report_effects &&
      !options->effect_budget_set && !options->explain && !options->why_mode) {
    return 1;
  }
  IREffectInput effect_input;
  long long effect_steps = 0;
  IREffectDecl *effect_decls =
      calloc(type_checker->effect_count ? type_checker->effect_count : 1,
             sizeof(IREffectDecl));
  IREffectObligation *obligations = calloc(
      type_checker->effect_obligation_count
          ? type_checker->effect_obligation_count
          : 1,
      sizeof(IREffectObligation));
  int effects_ok = effect_decls && obligations;
  if (effects_ok) {
    for (size_t i = 0; i < type_checker->effect_count; i++) {
      effect_decls[i].name = type_checker->effects[i].name;
      effect_decls[i].site = type_checker->effects[i].site;
      effect_decls[i].is_builtin = type_checker->effects[i].is_builtin;
      effect_decls[i].is_exported = type_checker->effects[i].is_exported;
    }
    for (size_t i = 0; i < type_checker->effect_obligation_count; i++) {
      obligations[i].function = type_checker->effect_obligations[i].function;
      obligations[i].signature =
          type_checker->effect_obligations[i].signature;
      obligations[i].location = type_checker->effect_obligations[i].location;
    }
    memset(&effect_input, 0, sizeof(effect_input));
    effect_input.effects = effect_decls;
    effect_input.effect_count = type_checker->effect_count;
    effect_input.obligations = obligations;
    effect_input.obligation_count = type_checker->effect_obligation_count;
    effect_input.instrument = options->check_effects || options->test_mode ||
                              options->trace_function != NULL ||
                              ir_verify_enabled();
    effect_input.library_build = options->shared_output;
    effect_input.report = options->report_effects ? stdout : NULL;
    if (options->why_mode) {
      effect_input.why_function = options->why_subject;
      effect_input.why_effect = options->why_what;
      effect_input.why_out = stdout;
    }
    effects_ok = ir_effects_run(ir_program, &effect_input, error_reporter,
                                effect_results, &effect_steps);
  }
  free(effect_decls);
  free(obligations);
  if (!effects_ok) {
    if (error_reporter_has_errors(error_reporter)) {
      error_reporter_print_errors(error_reporter);
    } else {
      fprintf(stderr, "Error: could not analyse the program's effects\n");
    }
    return 0;
  }
  if (options->why_mode) {
    return 2;
  }
  if (options->effect_budget_set && effect_steps > options->effect_budget) {
    fprintf(stderr,
            "error[F0005]: the effect pass spent %lld steps, more than the "
            "%lld --effect-budget allows\n",
            effect_steps, options->effect_budget);
    fprintf(stderr,
            "  help: --report-effects prints what the pass settled\n");
    return 0;
  }
  return 1;
}

static int compile_run_rules(IRProgram *ir_program, TypeChecker *type_checker,
                             const CompilerOptions *options,
                             ErrorReporter *error_reporter, ASTNode *program,
                             const char *input_filename,
                             IREffectResults *effect_results,
                             int *machine_rules_pending) {
  if (!ir_program_has_rules(ir_program) && !options->report_rules) {
    return 1;
  }
  IRRuleImage rule_image;
  char *rule_error = NULL;
  IRRuleStats rule_stats;
  int rules_ok;
  if (!rule_reflect_build(type_checker, program, input_filename,
                          mtlc_target()->triple, effect_results, &rule_image,
                          &rule_error)) {
    fprintf(stderr, "Error: %s\n",
            rule_error ? rule_error : "could not reflect the program");
    free(rule_error);
    return 0;
  }
  ir_rules_set_apply_fixes(options->apply_rule_fixes);
  rules_ok = ir_rules_run_kind(
      ir_program, &rule_image, error_reporter,
      options->report_rules ? stdout : NULL,
      options->rule_budget_set ? options->rule_budget : 0,
      options->test_mode, IR_RULE_OVER_PROGRAM, &rule_stats);
  ir_rule_image_free(&rule_image);
  if ((*machine_rules_pending) ||
      ir_program_has_rules_of(ir_program, IR_RULE_OVER_TRACE)) {
    ir_program_drop_rules_except(ir_program, main_keep_machine_rule);
  } else {
    ir_program_drop_rules(ir_program);
  }
  if (!rules_ok) {
    error_reporter_print_errors(error_reporter);
    if (options->apply_rule_fixes && ir_rules_proposal_count() > 0) {
      int applied = ir_rules_apply_fixes(stdout);
      if (applied > 0) {
        fprintf(stdout,
                "%d line%s rewritten; build again to check the result\n",
                applied, applied == 1 ? "" : "s");
      }
    }
    return 0;
  }
  return 1;
}

#define COMPILE_CONTINUE (-1)

typedef struct {
  const char *input_filename;
  const char *output_filename;
  CompilerOptions *options;
  CompilerProfile profile;
  double phase_start;
  char *source;
  ErrorReporter *error_reporter;
  Lexer *lexer;
  Parser *parser;
  SymbolTable *symbol_table;
  TypeChecker *type_checker;
  RegisterAllocator *register_allocator;
  DebugInfo *debug_info;
  CodeGenerator *code_generator;
  ASTNode *program;
  IRProgram *ir_program;
  char *ir_error_message;
  IREffectResults *effect_results;
  IRTwinSnapshots *twin_snapshots;
  int arm64_object_output;
  int machine_rules_pending;
  int explain_forced_for_rules;
  int emit_safety_checks;
  MetaStageReport meta_report;
} CompileContext;

typedef int (*CompileStage)(CompileContext *ctx);

static const char *compile_session_open(CompileContext *ctx) {
  ctx->lexer = lexer_create(ctx->source);
  ctx->symbol_table = symbol_table_create();
  ctx->register_allocator = register_allocator_create();
  if (!ctx->lexer || !ctx->symbol_table || !ctx->register_allocator) {
    return "Failed to initialize compiler components";
  }

  ctx->parser =
      parser_create_with_error_reporter(ctx->lexer, ctx->error_reporter);
  if (ctx->parser) {
    ctx->parser->gpu_mode = ctx->options->emit_ptx ||
                            ctx->options->emit_spirv ||
                            ctx->options->emit_metal;
  }
  ctx->type_checker = type_checker_create_with_error_reporter(
      ctx->symbol_table, ctx->error_reporter);
  type_checker_set_launch_report(ctx->options->report_launches);
  type_checker_set_gpu_type_report(ctx->options->report_gpu_types ||
                                   ctx->options->explain ||
                                   ctx->options->explain_all);
  if (!ctx->parser || !ctx->type_checker) {
    return "Failed to initialize parser or type checker";
  }
  ctx->type_checker->device_module = ctx->options->emit_ptx ||
                                     ctx->options->emit_spirv ||
                                     ctx->options->emit_metal;

  if (compile_wants_debug_info(ctx->options)) {
    ctx->debug_info =
        debug_info_create(ctx->input_filename, ctx->output_filename);
    if (!ctx->debug_info) {
      return "Failed to initialize debug information";
    }
    ctx->code_generator = code_generator_create_with_debug(ctx->debug_info);
  } else {
    ctx->code_generator = code_generator_create();
  }
  if (!ctx->code_generator) {
    return "Failed to initialize code generator";
  }
  return NULL;
}

static void compile_session_configure(CompileContext *ctx) {
  CompilerOptions *options = ctx->options;
  if (ctx->debug_info) {
    code_generator_set_debug_sidecar_emission(
        ctx->code_generator, compile_wants_debug_sidecar(options));
  }
  code_generator_set_stack_trace_support(
      ctx->code_generator, options->generate_stack_trace_support ? 1 : 0);
  code_generator_set_crash_report(
      ctx->code_generator,
      (options->generate_crash_report && options->building_executable &&
       !options->flat_output && !mtlc_target()->freestanding)
          ? 1
          : 0);
  code_generator_set_eliminate_unreachable_functions(
      ctx->code_generator, options->release ? 1 : 0);
  code_generator_set_assume_no_signed_overflow(
      ctx->code_generator, options->assume_no_signed_overflow);
  code_generator_set_profile_runtime(
      ctx->code_generator,
      (compiler_options_use_profile_runtime(options) || options->pgo_gen) ? 1
                                                                         : 0);
  code_generator_set_debug_hooks(ctx->code_generator,
                                 options->debug_hooks ? 1 : 0);
  code_generator_set_live(ctx->code_generator, options->live ? 1 : 0);
  ctx->code_generator->whole_program = options->building_executable ? 1 : 0;
}

static void compile_session_close(CompileContext *ctx) {
  ir_machine_set_collect(0);
  ir_machine_reset();
  ir_explain_set_quiet(0);
  if (ctx->options && ctx->options->explain && !ctx->options->optimize) {
    ir_explain_ledger_standalone(ctx->input_filename);
  }
  ir_twins_snapshots_free(ctx->twin_snapshots);
  ctx->twin_snapshots = NULL;
  compiler_set_phase(PROFILE_PHASE_CLEANUP);
  ctx->phase_start = compiler_profile_begin(&ctx->profile);
  if (getenv("METTLE_FULL_CLEANUP")) {
    if (ctx->program)
      ast_destroy_node(ctx->program);
    if (ctx->ir_program)
      ir_program_destroy(ctx->ir_program);
    type_checker_destroy(ctx->type_checker);
    symbol_table_destroy(ctx->symbol_table);
  }
  free(ctx->ir_error_message);
  ir_effect_results_free(ctx->effect_results);
  code_generator_destroy(ctx->code_generator);
  register_allocator_destroy(ctx->register_allocator);
  parser_destroy(ctx->parser);
  lexer_destroy(ctx->lexer);
  if (ctx->debug_info)
    debug_info_destroy(ctx->debug_info);
  error_reporter_destroy(ctx->error_reporter);
  free(ctx->source);
  compiler_profile_add(&ctx->profile, PROFILE_PHASE_CLEANUP, ctx->phase_start);
}

static int compile_stage_parse(CompileContext *ctx) {
  int parse_ok = 0;
  ctx->options->emit_object = 1;
  compiler_set_phase(PROFILE_PHASE_PARSE);
  ctx->phase_start = compiler_profile_begin(&ctx->profile);
  parse_ok =
      compile_lex_and_parse(ctx->parser, ctx->error_reporter, &ctx->program);
  compiler_profile_add(&ctx->profile, PROFILE_PHASE_PARSE, ctx->phase_start);
  if (!parse_ok) {
    return 1;
  }
  if (ctx->options->dump_ast) {
    compile_dump_ast(ctx->program, ctx->output_filename);
  }
  return COMPILE_CONTINUE;
}

static int compile_stage_imports(CompileContext *ctx) {
  CompilerOptions *options = ctx->options;
  ImportResolverOptions import_options = {0};
  int imports_ok = 0;
  if (options) {
    import_options.import_directories = options->import_directories;
    import_options.import_directory_count = options->import_directory_count;
    import_options.stdlib_directory =
        (options->stdlib_directory && options->stdlib_directory[0] != '\0')
            ? options->stdlib_directory
            : "stdlib";
  } else {
    import_options.stdlib_directory = "stdlib";
  }
  import_options.target_is_elf =
      (options && (options->emit_arm64 || options->emit_arm64_obj)) ||
      host_target_is_posix();
  import_options.target_is_macos = host_target_is_macho();
  import_options.gpu_provider_metal =
      options && (options->gpu_provider == 2 ||
                  (options->gpu_provider == 0 && host_target_is_macho()));

  compiler_set_phase(PROFILE_PHASE_PRELUDE);
  ctx->phase_start = compiler_profile_begin(&ctx->profile);
  compile_prepend_auto_imports(options, ctx->program);
  compiler_profile_add(&ctx->profile, PROFILE_PHASE_PRELUDE, ctx->phase_start);

  compiler_set_phase(PROFILE_PHASE_IMPORTS);
  ctx->phase_start = compiler_profile_begin(&ctx->profile);
  imports_ok = compile_resolve_imports(ctx->program, ctx->input_filename,
                                       ctx->error_reporter, &import_options);
  compiler_profile_add(&ctx->profile, PROFILE_PHASE_IMPORTS, ctx->phase_start);
  return imports_ok ? COMPILE_CONTINUE : 1;
}

static int compile_stage_monomorphize(CompileContext *ctx) {
  int mono_ok = 0;
  compiler_set_phase(PROFILE_PHASE_MONOMORPHIZE);
  ctx->phase_start = compiler_profile_begin(&ctx->profile);
  mono_ok = compile_monomorphize(ctx->program, ctx->error_reporter);
  compiler_profile_add(&ctx->profile, PROFILE_PHASE_MONOMORPHIZE,
                       ctx->phase_start);
  return mono_ok ? COMPILE_CONTINUE : 1;
}

static int compile_stage_metaprograms(CompileContext *ctx) {
  CompilerOptions *options = ctx->options;
  compiler_set_phase(PROFILE_PHASE_MONOMORPHIZE);
  type_checker_set_launch_report(0);
  type_checker_set_gpu_type_report(0);
  int ran = meta_stage_run(ctx->program, ctx->error_reporter,
                           ctx->input_filename, ctx->source,
                           mtlc_target()->triple,
                           options->meta_budget_set ? options->meta_budget : 0,
                           &ctx->meta_report);
  type_checker_set_launch_report(options->report_launches);
  type_checker_set_gpu_type_report(options->report_gpu_types || options->explain ||
                                   options->explain_all);
  if (!ran) {
    error_reporter_print_errors(ctx->error_reporter);
    return 1;
  }
  return COMPILE_CONTINUE;
}

static int compile_stage_metaprogram_report(CompileContext *ctx) {
  CompilerOptions *options = ctx->options;
  if (ctx->meta_report.pending_error) {
    fprintf(stderr, "error[L0001]: the metaprogram could not be built: %s\n",
            ctx->meta_report.pending_error);
    return 1;
  }
  if (options->report_meta) {
    meta_stage_print_report(&ctx->meta_report, stdout);
  }
  if (options->meta_budget_set &&
      ctx->meta_report.steps > options->meta_budget) {
    fprintf(stderr,
            "error[L0001]: metaprograms spent %lld interpreter steps, more "
            "than the %lld --meta-budget allows\n",
            ctx->meta_report.steps, options->meta_budget);
    return 1;
  }
  return COMPILE_CONTINUE;
}

static int compile_stage_type_check(CompileContext *ctx) {
  int tc_ok = 0;
  ir_explain_ledger_set_collect(ctx->options->explain);
  ir_explain_memory_set_collect(
      ctx->options->explain && ctx->options->optimize,
      ctx->options->explain_all ? NULL : ctx->options->input_filename);

  compiler_set_phase(PROFILE_PHASE_TYPE_CHECK);
  ctx->phase_start = compiler_profile_begin(&ctx->profile);
  tc_ok = compile_type_check(ctx->type_checker, ctx->program,
                             ctx->error_reporter);
  compiler_profile_add(&ctx->profile, PROFILE_PHASE_TYPE_CHECK,
                       ctx->phase_start);
  return tc_ok ? COMPILE_CONTINUE : 1;
}

static int compile_stage_proof_budget(CompileContext *ctx) {
  CompilerOptions *options = ctx->options;
  if (type_checker_proof_ceiling_hit(ctx->type_checker)) {
    fprintf(stderr,
            "warning[P0004]: the declared-type prover stopped after %lld "
            "steps and answered the rest as unknown\n",
            type_checker_proof_steps(ctx->type_checker));
    fprintf(stderr,
            "  help: a proof that needed more than that refuses here the same "
            "way it would if it were false; --report-proofs prints what each "
            "one cost\n");
  }
  if (options->proof_budget_set &&
      type_checker_proof_steps(ctx->type_checker) > options->proof_budget) {
    fprintf(stderr,
            "error[P0003]: the declared-type prover spent %lld steps, more "
            "than the %lld --proof-budget allows\n",
            type_checker_proof_steps(ctx->type_checker),
            options->proof_budget);
    fprintf(stderr,
            "  help: --report-proofs prints what each proof cost\n");
    return 1;
  }
  return COMPILE_CONTINUE;
}

static int compile_stage_type_reports(CompileContext *ctx) {
  CompilerOptions *options = ctx->options;
  if (options->expansion_budget_set &&
      !type_checker_check_expansion_budget(ctx->type_checker,
                                           options->expansion_budget)) {
    error_reporter_print_errors(ctx->error_reporter);
    return 1;
  }
  if (options->report_expansion) {
    type_checker_report_expansion(ctx->type_checker, stdout);
  }
  if (options->report_proofs) {
    type_checker_report_proofs(ctx->type_checker, stdout);
  }
  if (options->report_gpu_types || options->explain || options->explain_all) {
    type_checker_print_gpu_type_report(stderr);
  }
  if (options->why_mode && options->why_subject &&
      (options->why_subject[0] >= '0' && options->why_subject[0] <= '9')) {
    return type_checker_why_proof(ctx->type_checker, options->why_subject,
                                  options->why_what, stdout)
               ? 0
               : 1;
  }
  return COMPILE_CONTINUE;
}

static int compile_stage_expand(CompileContext *ctx) {
  size_t unprintable = 0;
  if (!ctx->options->expand_mode) {
    return COMPILE_CONTINUE;
  }
  unprintable = ast_print_program(stdout, ctx->program, expand_annotate,
                                  ctx->type_checker);
  if (unprintable > 0) {
    fprintf(stderr,
            "\nnote: %zu node%s had no source form and were printed as "
            "marked comments; this output is not a complete program\n",
            unprintable, unprintable == 1 ? "" : "s");
  }
  return 0;
}

static int compile_stage_drop_tests(CompileContext *ctx) {
  Program *prog_data = NULL;
  size_t kept = 0;
  size_t i = 0;
  if (ctx->options->test_mode) {
    return COMPILE_CONTINUE;
  }
  prog_data = (Program *)ctx->program->data;
  if (!prog_data) {
    return COMPILE_CONTINUE;
  }
  for (i = 0; i < prog_data->declaration_count; i++) {
    ASTNode *decl = prog_data->declarations[i];
    FunctionDeclaration *fd =
        decl && decl->type == AST_FUNCTION_DECLARATION && decl->data
            ? (FunctionDeclaration *)decl->data
            : NULL;
    if (fd && fd->is_test) {
      size_t child_kept = 0;
      for (size_t c = 0; c < ctx->program->child_count; c++) {
        if (ctx->program->children[c] == decl) {
          continue;
        }
        ctx->program->children[child_kept++] = ctx->program->children[c];
      }
      ctx->program->child_count = child_kept;
      ast_destroy_node(decl);
      continue;
    }
    prog_data->declarations[kept++] = decl;
  }
  prog_data->declaration_count = kept;
  return COMPILE_CONTINUE;
}

static int compile_stage_lowering_modes(CompileContext *ctx) {
  CompilerOptions *options = ctx->options;
  if (options->explain && !options->optimize) {
    fprintf(stderr,
            "note: --explain without -O/--release reports only what does not "
            "depend on the optimizer: the types proven, the effects held, the "
            "rules run, the checks a declared type deleted, and the beliefs "
            "the build rested on\n");
  }
  if (program_declares_machine_rule(ctx->program)) {
    ctx->machine_rules_pending = 1;
    ir_machine_set_collect(1);
    if (!options->explain) {
      ir_explain_set_quiet(1);
      options->explain = 1;
      ctx->explain_forced_for_rules = 1;
    }
  }
  ir_lowering_set_explain(options->explain && options->optimize &&
                          !options->emit_ptx && !options->emit_spirv &&
                          !options->emit_metal);
  ir_lowering_set_refinement_checks(
      options->check_proofs || options->test_mode ||
      options->trace_function != NULL || ir_verify_enabled());
  ir_lowering_set_task_checks(options->check_tasks);
  ir_lowering_set_live(options->live);
  ir_lowering_set_overflow_checks(options->check_overflow);
  ir_lowering_set_assume_no_signed_overflow(options->assume_no_signed_overflow);
  ir_explain_safety_set_collect(options->explain && options->optimize,
                                ctx->input_filename);
  return COMPILE_CONTINUE;
}

static int compile_stage_lower_ir(CompileContext *ctx) {
  CompilerOptions *options = ctx->options;
  int emit_runtime_checks =
      (options->release || options->emit_ptx || options->emit_spirv ||
       options->emit_metal || mtlc_target()->freestanding)
          ? 0
          : 1;
  int ir_ok = 0;
  ctx->emit_safety_checks = options->safe && !options->emit_ptx &&
                            !options->emit_spirv && !options->emit_metal;
  compiler_set_phase(PROFILE_PHASE_IR_LOWERING);
  ctx->phase_start = compiler_profile_begin(&ctx->profile);
  ir_ok = compile_lower_to_ir(ctx->program, ctx->type_checker,
                              ctx->symbol_table, emit_runtime_checks,
                              ctx->emit_safety_checks, &ctx->ir_program,
                              &ctx->ir_error_message);
  compiler_profile_add(&ctx->profile, PROFILE_PHASE_IR_LOWERING,
                       ctx->phase_start);
  return ir_ok ? COMPILE_CONTINUE : 1;
}

static int compile_stage_swap_check(CompileContext *ctx) {
  CompilerOptions *options = ctx->options;
  if (!options->swap_check_mode) {
    return COMPILE_CONTINUE;
  }
  if (!options->swap_old_name || !options->swap_new_name) {
    fprintf(stderr, "swap-check needs both functions: --old <fn> --new <fn>\n");
    return 1;
  }
  return compile_run_swap_check(ctx->ir_program, options->swap_old_name,
                                options->swap_new_name);
}

static int compile_stage_effects(CompileContext *ctx) {
  int stage = compile_run_effects(ctx->ir_program, ctx->type_checker,
                                  ctx->options, ctx->error_reporter,
                                  &ctx->effect_results);
  if (stage == 1) {
    return COMPILE_CONTINUE;
  }
  return stage == 0 ? 1 : 0;
}

static int compile_stage_twins(CompileContext *ctx) {
  IRTwinStats twin_stats;
  if (!ir_program_has_twins(ctx->ir_program) && !ctx->options->report_twins) {
    return COMPILE_CONTINUE;
  }
  if (!ir_twins_check(ctx->ir_program, ctx->error_reporter,
                      ctx->options->report_twins ? stdout : NULL, "as written",
                      &twin_stats)) {
    error_reporter_print_errors(ctx->error_reporter);
    return 1;
  }
  if (error_reporter_has_errors(ctx->error_reporter)) {
    error_reporter_print_errors(ctx->error_reporter);
  }
  if (ir_verify_enabled()) {
    ctx->twin_snapshots = ir_twins_capture(ctx->ir_program);
  }
  return COMPILE_CONTINUE;
}

static int compile_stage_purity(CompileContext *ctx) {
  IRPurityStats purity_stats;
  if (!ir_purity_check_contracts(ctx->ir_program, ctx->error_reporter,
                                 &purity_stats)) {
    error_reporter_print_errors(ctx->error_reporter);
    return 1;
  }
  return COMPILE_CONTINUE;
}

static int compile_stage_rules(CompileContext *ctx) {
  if (!compile_run_rules(ctx->ir_program, ctx->type_checker, ctx->options,
                         ctx->error_reporter, ctx->program,
                         ctx->input_filename, ctx->effect_results,
                         &ctx->machine_rules_pending)) {
    return 1;
  }
  return COMPILE_CONTINUE;
}

static int compile_stage_instrument_program(CompileContext *ctx) {
  size_t trace_sites = 0;
  mettle_compiler_ctx_set_ir_program(ctx->ir_program);
  ctx->options->main_wants_argc_argv = ctx->ir_program->main_wants_argc_argv;
  if (ctx->options->pgo) {
    ir_pgo_profile_program(ctx->ir_program);
    ir_pgo_print_summary();
  }
  if (ctx->options->record_trace &&
      !ir_trace_record_instrument(ctx->ir_program, &trace_sites)) {
    mettle_compiler_ice_report("Trace recording instrumentation failed", NULL);
    return 1;
  }
  return COMPILE_CONTINUE;
}

static int compile_stage_machine(CompileContext *ctx) {
  MachineDesc desc;
  char error[256];
  SourceLocation where;
  if (!ctx->options->machine_mode && !ctx->options->emulate_mode) {
    return COMPILE_CONTINUE;
  }
  if (!machine_desc_read(ctx->program, &desc, error, sizeof(error), &where)) {
    if (where.line) {
      error_reporter_add_error(ctx->error_reporter, ERROR_SEMANTIC, where,
                               error);
      error_reporter_set_last_code(ctx->error_reporter, "N0001");
      error_reporter_print_errors(ctx->error_reporter);
    } else {
      fprintf(stderr, "error[N0001]: %s\n", error);
    }
    return 1;
  }
  if (ctx->options->machine_mode) {
    machine_desc_print(stdout, &desc);
    return 0;
  }
  return machine_emulate(&desc, ctx->program, ctx->ir_program);
}

static int compile_stage_trace_rules(CompileContext *ctx) {
  CompilerOptions *options = ctx->options;
  size_t events = 0;
  size_t dropped = 0;
  int verdict = 0;
  if (!options->check_trace_path) {
    return COMPILE_CONTINUE;
  }
  if (!ir_program_has_rules_of(ctx->ir_program, IR_RULE_OVER_TRACE)) {
    fprintf(stderr,
            "Error: '%s' declares no `@rule fn f(t: Trace) -> Verdict`, so "
            "there is nothing to hold a recorded run to\n",
            ctx->input_filename);
    return 1;
  }
  if (!trace_file_load(options->check_trace_path, &events, &dropped)) {
    return 1;
  }
  g_trace_rule_context.program = ctx->ir_program;
  g_trace_rule_context.type_checker = ctx->type_checker;
  g_trace_rule_context.reporter = ctx->error_reporter;
  g_trace_rule_context.filename = ctx->input_filename;
  g_trace_rule_context.report = options->report_rules ? stdout : NULL;
  g_trace_rule_context.budget =
      options->rule_budget_set ? options->rule_budget : 0;
  printf("trace: %zu events from '%s'%s\n", events, options->check_trace_path,
         dropped ? ", and the run said it dropped some" : "");
  verdict = compile_run_trace_rules(&g_trace_rule_context, NULL) ? 0 : 1;
  ir_trace_reset();
  return verdict;
}

static int compile_stage_comptime(CompileContext *ctx) {
  if (!ctx->options->test_mode && !ctx->options->trace_function) {
    return COMPILE_CONTINUE;
  }
  ctx->options->trace_rule_checker = ctx->type_checker;
  return compile_run_comptime(ctx->ir_program, ctx->program, ctx->options,
                              ctx->error_reporter, ctx->input_filename,
                              ctx->source);
}

static int compile_stage_safety(CompileContext *ctx) {
  IRSafetyStats safety_stats = {0};
  if (ctx->options->native_heap &&
      !ir_program_route_to_native_heap(ctx->ir_program)) {
    fprintf(stderr, "Error: Failed to route allocation to the native heap\n");
    return 1;
  }
  if (!ctx->emit_safety_checks) {
    return COMPILE_CONTINUE;
  }
  if (!ir_safety_register_allocations(ctx->ir_program)) {
    fprintf(stderr, "Error: Failed to instrument allocations for --safe\n");
    return 1;
  }
  if (ctx->options->optimize &&
      (!ir_safety_analyze_origins(ctx->ir_program) ||
       !ir_optimize_safety_analysis(
           ctx->ir_program,
           compiler_options_use_profile_runtime(ctx->options)))) {
    mettle_compiler_ice_report("Safety analysis failed", NULL);
    return 1;
  }
  ir_explain_safety_set_collect(ctx->options->explain && ctx->options->optimize,
                                ctx->input_filename);
  if (!ir_safety_resolve_program(ctx->ir_program, &safety_stats)) {
    mettle_compiler_ice_report("Safety check resolution failed", NULL);
    return 1;
  }
  ir_explain_safety_totals(safety_stats.emitted, safety_stats.proved,
                           safety_stats.hoisted, safety_stats.spanned,
                           safety_stats.exempt, safety_stats.extent_tests,
                           safety_stats.region_calls);
  return COMPILE_CONTINUE;
}

static void compile_deadline_costs(const CompilerOptions *options,
                                   IRDeadlineCosts *costs) {
  const IROptCost *cost;
  compile_publish_optimizer_costs();
  cost = ir_opt_cost();
  memset(costs, 0, sizeof(*costs));
  costs->op = cost->op;
  costs->load = cost->load;
  costs->store = cost->store;
  costs->branch = cost->branch;
  costs->multiply = cost->multiply;
  costs->multiply_float = cost->multiply_float;
  costs->divide = cost->divide;
  costs->divide_float = cost->divide_float;
  costs->call = cost->call;
  costs->allocate = cost->allocate;
  costs->described = options->target_desc_path != NULL;
}

static int compile_stage_deadlines(CompileContext *ctx) {
  CompilerOptions *options = ctx->options;
  IRDeadlineStats deadline_stats;
  IRDeadlineCosts deadline_costs;
  int instrumented = 0;
  if (options->test_mode) {
    return COMPILE_CONTINUE;
  }
  compile_deadline_costs(options, &deadline_costs);
  instrumented = options->record_trace || options->check_overflow ||
                 options->check_tasks || options->check_effects ||
                 options->check_proofs || options->safe || ir_verify_enabled();
  if (ir_deadline_run(ctx->ir_program, ctx->error_reporter, &deadline_costs,
                      options->check_deadlines, instrumented,
                      options->report_deadlines ? stdout : NULL,
                      &deadline_stats)) {
    return COMPILE_CONTINUE;
  }
  if (error_reporter_has_errors(ctx->error_reporter)) {
    error_reporter_print_errors(ctx->error_reporter);
  }
  return 1;
}

typedef enum {
  MTLC_BACKEND_DEVICE = 0,
  MTLC_BACKEND_HOST
} MtlcBackendFamily;

typedef struct {
  const char *name;
  MtlcBackendFamily family;
  int (*selected)(const CompilerOptions *options);
  int (*run)(CompileContext *ctx);
} MtlcBackend;

static int backend_ptx_selected(const CompilerOptions *options) {
  return options->emit_ptx;
}

static int backend_ptx_run(CompileContext *ctx) {
  return compile_emit_ptx(ctx->ir_program, ctx->program, ctx->code_generator,
                          ctx->options, ctx->input_filename,
                          ctx->output_filename, &ctx->profile);
}

static int backend_spirv_selected(const CompilerOptions *options) {
  return options->emit_spirv;
}

static int backend_spirv_run(CompileContext *ctx) {
  return compile_emit_spirv(ctx->ir_program, ctx->program, ctx->code_generator,
                            ctx->options, ctx->output_filename, &ctx->profile);
}

static int backend_metal_selected(const CompilerOptions *options) {
  return options->emit_metal;
}

static int backend_metal_run(CompileContext *ctx) {
  return compile_emit_metal(ctx->ir_program, ctx->program, ctx->options,
                            ctx->input_filename, ctx->output_filename,
                            &ctx->profile);
}

static int backend_arm64_selected(const CompilerOptions *options) {
  return options->emit_arm64;
}

static int backend_arm64_run(CompileContext *ctx) {
  return compile_emit_arm64(ctx->ir_program, ctx->program, ctx->options,
                            ctx->output_filename);
}

static const MtlcBackend MTLC_BACKENDS[] = {
    {"PTX", MTLC_BACKEND_DEVICE, backend_ptx_selected, backend_ptx_run},
    {"SPIR-V", MTLC_BACKEND_DEVICE, backend_spirv_selected, backend_spirv_run},
    {"Metal", MTLC_BACKEND_DEVICE, backend_metal_selected, backend_metal_run},
    {"AArch64", MTLC_BACKEND_HOST, backend_arm64_selected, backend_arm64_run},
};

#define MTLC_BACKEND_COUNT                                                     \
  (sizeof(MTLC_BACKENDS) / sizeof(MTLC_BACKENDS[0]))

static const MtlcBackend *mtlc_backend_selected(const CompilerOptions *options,
                                                MtlcBackendFamily family) {
  for (size_t i = 0; i < MTLC_BACKEND_COUNT; i++) {
    if (MTLC_BACKENDS[i].family == family &&
        MTLC_BACKENDS[i].selected(options)) {
      return &MTLC_BACKENDS[i];
    }
  }
  return NULL;
}

static int compile_stage_device_targets(CompileContext *ctx) {
  const MtlcBackend *backend =
      mtlc_backend_selected(ctx->options, MTLC_BACKEND_DEVICE);
  if (backend) {
    return backend->run(ctx);
  }
  if (!ir_program_lower_gpu_launches(ctx->ir_program)) {
    fprintf(stderr,
            "Error: Failed to lower GPU launches for the host runtime\n");
    return 1;
  }
  backend = mtlc_backend_selected(ctx->options, MTLC_BACKEND_HOST);
  if (backend) {
    return backend->run(ctx);
  }
  return COMPILE_CONTINUE;
}

static int compile_stage_hooks(CompileContext *ctx) {
  CompilerOptions *options = ctx->options;
  if (compiler_options_use_profile_runtime(options) &&
      !ir_profile_instrument_program(ctx->ir_program)) {
    fprintf(stderr, "Error: Failed to instrument IR for runtime profiling\n");
    return 1;
  }
  if (options->pgo_gen && options->pgo_use) {
    fprintf(stderr, "Error: --pgo-gen and --pgo-use are mutually exclusive\n");
    return 1;
  }
  if (options->pgo_gen && !ir_profile_instrument_blocks(ctx->ir_program)) {
    fprintf(stderr, "Error: Failed to instrument IR for profile generation\n");
    return 1;
  }
  if (options->pgo_use &&
      !ir_pgo_load_profile(options->pgo_use, ctx->ir_program)) {
    fprintf(stderr, "Error: Could not read a usable profile from '%s'\n",
            options->pgo_use);
    return 1;
  }
  if (!options->debug_hooks) {
    return COMPILE_CONTINUE;
  }
  if (compiler_options_use_profile_runtime(options)) {
    fprintf(stderr,
            "Error: --debug-hooks and --profile-runtime are mutually "
            "exclusive\n");
    return 1;
  }
  if (options->optimize) {
    fprintf(stderr,
            "Error: --debug-hooks requires an unoptimized build (drop "
            "--release/-O; optimized code moves and deletes the hooks)\n");
    return 1;
  }
  if (!ir_debug_hooks_instrument_program(ctx->ir_program)) {
    fprintf(stderr, "Error: Failed to instrument IR for debugging\n");
    return 1;
  }
  return COMPILE_CONTINUE;
}

static int compile_wants_dead_function_sweep(const CompilerOptions *options) {
  return options->building_executable && !options->tracy &&
         !compiler_options_use_profile_runtime(options) &&
         !options->debug_hooks;
}

static int compile_sweep_dead_functions(CompileContext *ctx) {
  const CompilerOptions *options = ctx->options;
  int keep_exports = options->link_argument_count > 0 ||
                     options->shared_output || options->export_dynamic;
  if (!compile_wants_dead_function_sweep(options)) {
    return 1;
  }
  if (ir_program_eliminate_dead_functions(ctx->ir_program, keep_exports)) {
    return 1;
  }
  fprintf(stderr, "Error: Failed to eliminate dead functions\n");
  return 0;
}

static int compile_stage_optimize(CompileContext *ctx) {
  size_t i = 0;
  if (!compile_sweep_dead_functions(ctx)) {
    return 1;
  }
  if (ctx->options->optimize) {
    int opt_ok = 0;
    compiler_set_phase(PROFILE_PHASE_IR_OPTIMIZATION);
    ctx->phase_start = compiler_profile_begin(&ctx->profile);
    opt_ok = compile_optimize_ir(ctx->ir_program, ctx->program, ctx->options);
    compiler_profile_add(&ctx->profile, PROFILE_PHASE_IR_OPTIMIZATION,
                         ctx->phase_start);
    if (!opt_ok) {
      return 1;
    }
  } else {
    ir_note_simd_contracts_unverified(ctx->ir_program);
    ir_note_parallel_loops_unverified(ctx->ir_program);
  }

  if (ctx->twin_snapshots) {
    IRTwinStats twin_stats;
    int twins_ok = ir_twins_recheck(
        ctx->ir_program, ctx->twin_snapshots, ctx->error_reporter,
        ctx->options->report_twins ? stdout : NULL, "after the optimizer",
        &twin_stats);
    ir_twins_snapshots_free(ctx->twin_snapshots);
    ctx->twin_snapshots = NULL;
    if (!twins_ok) {
      error_reporter_print_errors(ctx->error_reporter);
      return 1;
    }
  }
  ir_program_drop_rewrite_rules(ctx->ir_program);

  if (!compile_sweep_dead_functions(ctx)) {
    return 1;
  }
  for (i = 0; i < ctx->ir_program->function_count; i++) {
    ir_function_drop_dead_nops(ctx->ir_program->functions[i]);
  }
  return COMPILE_CONTINUE;
}

static int compile_stage_counters(CompileContext *ctx) {
  if (ctx->options->profile_runtime_ops &&
      !ir_profile_instrument_operation_counters(ctx->ir_program)) {
    fprintf(stderr, "Error: Failed to instrument IR operation counters for "
                    "runtime profiling\n");
    return 1;
  }
  if (ctx->options->profile_blocks &&
      !ir_profile_instrument_blocks(ctx->ir_program)) {
    fprintf(stderr,
            "Error: Failed to instrument IR basic-block counters for the "
            "codegen profile view\n");
    return 1;
  }
  code_generator_set_ir_program(ctx->code_generator, ctx->ir_program);
  return COMPILE_CONTINUE;
}

static void compile_write_ir_dump(CompileContext *ctx, const char *path) {
  FILE *ir_file = fopen(path, "w");
  if (!ir_file) {
    fprintf(stderr, "Warning: Could not create IR file '%s': %s\n", path,
            strerror(errno));
    return;
  }
  if (!ir_program_dump(ctx->ir_program, ir_file)) {
    fprintf(stderr, "Warning: Failed to write IR dump to '%s'\n", path);
  }
  fclose(ir_file);
  if (ctx->options->debug_mode) {
    printf("Generated IR dump: %s\n", path);
  }
}

static int compile_stage_dump_ir(CompileContext *ctx) {
  char *ir_output = NULL;
  if (!ctx->options->debug_mode && !ctx->options->dump_ir) {
    return COMPILE_CONTINUE;
  }
  compiler_set_phase(PROFILE_PHASE_IR_DUMP);
  ctx->phase_start = compiler_profile_begin(&ctx->profile);
  ir_output = build_sidecar_filename(ctx->output_filename, ".ir");
  if (!ir_output) {
    fprintf(stderr, "Warning: Failed to allocate IR output filename for '%s'\n",
            ctx->output_filename);
  } else {
    compile_write_ir_dump(ctx, ir_output);
    free(ir_output);
  }
  compiler_profile_add(&ctx->profile, PROFILE_PHASE_IR_DUMP, ctx->phase_start);
  return COMPILE_CONTINUE;
}

static int compile_stage_codegen(CompileContext *ctx) {
  int codegen_ok = 0;
  compiler_set_phase(PROFILE_PHASE_CODEGEN);
  ctx->phase_start = compiler_profile_begin(&ctx->profile);
  codegen_ok = ctx->arm64_object_output
                   ? 1
                   : compile_generate_code(ctx->code_generator);
  compiler_profile_add(&ctx->profile, PROFILE_PHASE_CODEGEN, ctx->phase_start);
  if (!codegen_ok) {
    return 1;
  }
  if (ctx->options->annotate_asm ||
      (ctx->options->explain_json && mir_annotate_enabled())) {
    mir_annotate_flush();
  }
  if (ctx->options->explain && ctx->options->optimize) {
    ir_explain_backend_flush();
  }
  return COMPILE_CONTINUE;
}

static int compile_stage_machine_rules(CompileContext *ctx) {
  IRRuleImage machine_image;
  IRRuleStats machine_stats;
  char *machine_error = NULL;
  int machine_ok = 0;
  if (!ctx->machine_rules_pending) {
    return COMPILE_CONTINUE;
  }
  if (ctx->explain_forced_for_rules) {
    ctx->options->explain = 0;
  }
  if (!rule_reflect_build_machine(ctx->type_checker, ctx->ir_program,
                                  ctx->input_filename, mtlc_target()->triple,
                                  ctx->effect_results, &machine_image,
                                  &machine_error)) {
    fprintf(stderr, "Error: %s\n",
            machine_error ? machine_error : "could not reflect the machine");
    free(machine_error);
    return 1;
  }
  machine_ok = ir_rules_run_kind(
      ctx->ir_program, &machine_image, ctx->error_reporter,
      ctx->options->report_rules ? stdout : NULL,
      ctx->options->rule_budget_set ? ctx->options->rule_budget : 0, 0,
      IR_RULE_OVER_MACHINE, &machine_stats);
  ir_rule_image_free(&machine_image);
  ir_program_drop_rules(ctx->ir_program);
  ctx->machine_rules_pending = 0;
  if (!machine_ok) {
    error_reporter_print_errors(ctx->error_reporter);
    return 1;
  }
  return COMPILE_CONTINUE;
}

static const char *compile_flat_entry_symbol(const IRProgram *ir_program) {
  for (size_t i = 0; i < ir_program->function_count; i++) {
    if (ir_program->functions[i] && ir_program->functions[i]->name &&
        strcmp(ir_program->functions[i]->name, "_start") == 0) {
      return "_start";
    }
  }
  return "main";
}

static int compile_write_flat_image(CompileContext *ctx) {
  BinaryEmitter *binary_emitter =
      code_generator_get_binary_emitter(ctx->code_generator);
  char flat_error[512] = {0};
  const unsigned char boot_signature[2] = {0x55, 0xAA};
  const unsigned char *trailer = NULL;
  size_t pad_to = 0;
  size_t trailer_size = 0;
  if (mtlc_target()->image_base == 0x7C00ull) {
    pad_to = 512;
    trailer = boot_signature;
    trailer_size = sizeof(boot_signature);
  }
  if (!binary_emitter_write_flat(binary_emitter, ctx->options->flat_output,
                                 mtlc_target()->image_base,
                                 compile_flat_entry_symbol(ctx->ir_program),
                                 pad_to, 0x00, trailer, trailer_size,
                                 flat_error, sizeof(flat_error))) {
    fprintf(stderr, "Error: Could not create flat image '%s': %s\n",
            ctx->options->flat_output,
            flat_error[0] ? flat_error : "Unknown error");
    return 0;
  }
  fprintf(stderr, "Wrote flat image '%s' at 0x%llx\n",
          ctx->options->flat_output,
          (unsigned long long)mtlc_target()->image_base);
  return 1;
}

static int compile_write_object(CompileContext *ctx) {
  BinaryEmitter *binary_emitter =
      code_generator_get_binary_emitter(ctx->code_generator);
  if (binary_emitter_write_object_file(binary_emitter, ctx->output_filename)) {
    return 1;
  }
  fprintf(stderr, "Error: Could not create object file '%s': %s\n",
          ctx->output_filename,
          binary_emitter_get_error(binary_emitter)
              ? binary_emitter_get_error(binary_emitter)
              : "Unknown error");
  return 0;
}

static int compile_write_arm64_object(CompileContext *ctx) {
  char arm64_error[512] = {0};
  if (arm64_ir_write_object(ctx->ir_program, ctx->output_filename, arm64_error,
                            sizeof(arm64_error))) {
    return 1;
  }
  fprintf(stderr, "Error: Could not create AArch64 object file '%s': %s\n",
          ctx->output_filename,
          arm64_error[0] ? arm64_error : "Unknown error");
  return 0;
}

static int compile_stage_write_output(CompileContext *ctx) {
  int written = 0;
  compiler_set_phase(PROFILE_PHASE_WRITE_OUTPUT);
  ctx->phase_start = compiler_profile_begin(&ctx->profile);
  if (ctx->arm64_object_output) {
    written = compile_write_arm64_object(ctx);
  } else if (ctx->options->flat_output) {
    written = compile_write_flat_image(ctx);
  } else {
    written = compile_write_object(ctx);
  }
  compiler_profile_add(&ctx->profile, PROFILE_PHASE_WRITE_OUTPUT,
                       ctx->phase_start);
  return written ? COMPILE_CONTINUE : 1;
}

static const char *compile_debug_suffix(const char *format) {
  if (strcasecmp(format, "stabs") == 0) {
    return ".stabs";
  }
  if (strcasecmp(format, "map") == 0) {
    return ".map";
  }
  if (strcasecmp(format, "dwarf") != 0) {
    fprintf(stderr, "Warning: Unknown debug format '%s', defaulting to dwarf\n",
            format);
  }
  return ".dwarf";
}

static int compile_write_debug_sidecar(CompileContext *ctx) {
  const char *format = (ctx->options->debug_format &&
                        ctx->options->debug_format[0] != '\0')
                           ? ctx->options->debug_format
                           : "dwarf";
  const char *suffix = compile_debug_suffix(format);
  char *debug_output = build_sidecar_filename(ctx->output_filename, suffix);
  if (!debug_output) {
    fprintf(stderr,
            "Error: Failed to allocate debug output filename for '%s'\n",
            ctx->output_filename);
    return 0;
  }
  if (strcasecmp(format, "stabs") == 0) {
    debug_info_generate_stabs(ctx->debug_info, debug_output);
  } else if (strcasecmp(format, "map") == 0) {
    debug_info_generate_debug_map(ctx->debug_info, debug_output);
  } else {
    debug_info_generate_dwarf(ctx->debug_info, debug_output);
  }
  if (ctx->options->debug_mode) {
    printf("Generated debug info: %s\n", debug_output);
  }
  free(debug_output);
  return 1;
}

static int compile_stage_debug_info(CompileContext *ctx) {
  CompilerOptions *options = ctx->options;
  int written = 1;
  compiler_set_phase(PROFILE_PHASE_DEBUG_INFO);
  ctx->phase_start = compiler_profile_begin(&ctx->profile);
  if (ctx->debug_info) {
    if (options->debug_mode || options->generate_debug_symbols ||
        options->generate_line_mapping) {
      written = compile_write_debug_sidecar(ctx);
    }
    if (written && options->generate_stack_trace_support &&
        options->debug_mode) {
      printf("Embedded runtime stack trace support enabled\n");
    }
  }
  compiler_profile_add(&ctx->profile, PROFILE_PHASE_DEBUG_INFO,
                       ctx->phase_start);
  return written ? COMPILE_CONTINUE : 1;
}

static int compile_stage_report(CompileContext *ctx) {
  if (ctx->error_reporter->count > 0) {
    error_reporter_print_errors(ctx->error_reporter);
  }
  if (ctx->options->debug_mode) {
    printf("Successfully compiled '%s' to '%s'\n", ctx->input_filename,
           ctx->output_filename);
  }
  return COMPILE_CONTINUE;
}

static const CompileStage COMPILE_STAGES[] = {
    compile_stage_parse,
    compile_stage_imports,
    compile_stage_monomorphize,
    compile_stage_metaprograms,
    compile_stage_type_check,
    compile_stage_metaprogram_report,
    compile_stage_type_reports,
    compile_stage_proof_budget,
    compile_stage_expand,
    compile_stage_drop_tests,
    compile_stage_lowering_modes,
    compile_stage_lower_ir,
    compile_stage_swap_check,
    compile_stage_effects,
    compile_stage_twins,
    compile_stage_purity,
    compile_stage_rules,
    compile_stage_instrument_program,
    compile_stage_machine,
    compile_stage_trace_rules,
    compile_stage_comptime,
    compile_stage_safety,
    compile_stage_deadlines,
    compile_stage_device_targets,
    compile_stage_hooks,
    compile_stage_optimize,
    compile_stage_counters,
    compile_stage_dump_ir,
    compile_stage_codegen,
    compile_stage_machine_rules,
    compile_stage_write_output,
    compile_stage_debug_info,
    compile_stage_report,
};

int compile_file(const char *input_filename, const char *output_filename,
                 CompilerOptions *options) {
  CompileContext ctx = {0};
  const char *open_error = NULL;
  int result = 0;
  size_t i = 0;

  ctx.input_filename = input_filename;
  ctx.output_filename = output_filename;
  ctx.options = options;
  ctx.arm64_object_output = compile_targets_arm64_object(options);
  compiler_profile_init(&ctx.profile, options && options->profile);

  mettle_compiler_ctx_reset();
  mettle_compiler_ctx_set_input_filename(input_filename);
  mettle_compiler_ctx_set_current_filename(input_filename);
  if (options) {
    mettle_compiler_ctx_set_options(options->debug_compiler, options->dump_ir);
  }

  mettle_trust_mode_announce();
  compiler_set_phase(PROFILE_PHASE_READ_INPUT);
  ctx.phase_start = compiler_profile_begin(&ctx.profile);
  result = compile_read_source(input_filename, &ctx.source) ? 0 : 1;
  compiler_profile_add(&ctx.profile, PROFILE_PHASE_READ_INPUT, ctx.phase_start);
  if (result) {
    compiler_profile_print_compile(&ctx.profile, input_filename, 1);
    return 1;
  }

  compiler_set_phase(PROFILE_PHASE_INIT);
  ctx.phase_start = compiler_profile_begin(&ctx.profile);
  ctx.error_reporter = error_reporter_create(input_filename, ctx.source);
  if (!ctx.error_reporter) {
    fprintf(stderr, "Error: Could not initialize error reporter\n");
    free(ctx.source);
    compiler_profile_print_compile(&ctx.profile, input_filename, 1);
    return 1;
  }
  compiler_set_phase(PROFILE_PHASE_LEXICAL_VALIDATION);
  ctx.phase_start = compiler_profile_begin(&ctx.profile);
  compiler_profile_add(&ctx.profile, PROFILE_PHASE_LEXICAL_VALIDATION,
                       ctx.phase_start);

  compiler_set_phase(PROFILE_PHASE_INIT);
  ctx.phase_start = compiler_profile_begin(&ctx.profile);
  open_error = compile_session_open(&ctx);
  if (open_error) {
    error_reporter_add_error(ctx.error_reporter, ERROR_INTERNAL,
                             source_location_create(0, 0), open_error);
    error_reporter_print_errors(ctx.error_reporter);
    result = 1;
  } else {
    compile_session_configure(&ctx);
  }
  compiler_profile_add(&ctx.profile, PROFILE_PHASE_INIT, ctx.phase_start);

  for (i = 0; !result && i < sizeof(COMPILE_STAGES) / sizeof(COMPILE_STAGES[0]);
       i++) {
    int stage = COMPILE_STAGES[i](&ctx);
    if (stage != COMPILE_CONTINUE) {
      result = stage;
      break;
    }
  }

  compile_session_close(&ctx);
  compiler_profile_print_compile(&ctx.profile, input_filename, result);
  return result;
}

void print_usage(const char *program_name) {
  printf("Usage: %s [options] <input.mettle>\n", program_name);
  printf("       %s help [topic]\n", program_name);
  printf("       %s docs [topic]\n", program_name);
  printf("       %s explain <CODE>   Explain a code: a diagnostic (E0004, M0103) or an --explain\n"
         "                            decision (dot-shape-address); 'list' for the index\n",
         program_name);
  printf("       %s target <triple>            Print a built-in target's description as\n"
         "                           Mettle; `--target desc.mettle` builds for one\n",
         program_name);
  printf("       %s test <file> [--filter=S]   Run @test functions in the compile-time\n"
         "                           interpreter (instant; no codegen or linking)\n",
         program_name);
  printf("       %s trace <file> <fn> [args...] Interpret a function and print a\n"
         "                           line-by-line value trace\n",
         program_name);
  printf("Options:\n");
  printf("  --error-format=F    Diagnostic output format: human (default) or json\n"
         "                      (one JSON object per diagnostic on stderr, for tooling)\n");
  printf("  --pgo               Zero-run profile-guided optimization: interpret main()\n"
         "                      at compile time (deterministic, sandboxed) and feed the\n"
         "                      measured call frequencies to the optimizer - a hot\n"
         "                      callee bypasses the inliner's static size budget like an\n"
         "                      explicit @inline. No instrumented build, no training\n"
         "                      run. Implies -O. METTLE_PGO_HOT sets the threshold.\n");
  printf("  --pgo-gen           Instrument basic blocks for a measured profile.\n"
         "                      Run the program, then feed the sidecar back with\n"
         "                      --pgo-use. METTLE_PROFILE_OUT names the sidecar.\n");
  printf("  --pgo-use=FILE      Optimize with counts measured by a --pgo-gen run.\n");
  printf("  --assume-no-signed-overflow\n"
         "                      Signed int8/int16/int32 arithmetic never leaves\n"
         "                      its type, so results skip the wrap truncation.\n"
         "                      Unsigned still wraps. See docs/types.md.\n");
  printf("  --verify            Translation validation: after every optimization pass,\n"
         "                      execute each changed function's before/after IR on\n"
         "                      generated inputs and compare behavior. A diverging pass\n"
         "                      is reported with a concrete counterexample, quarantined\n"
         "                      for that function, and the build continues from the\n"
         "                      validated IR. Implies -O.\n");
  printf("  -i <file>           Input file\n");
  printf("  -o <file>           Output file (default: output.obj/output.o, or "
         "executable path with --build)\n");
  printf("  -I <dir>            Add import search directory (repeatable)\n");
  printf("  --stdlib <dir>      Set stdlib root directory (default: auto-detect "
         "bundled stdlib, then ./stdlib)\n");
  printf("  --build             Compile and link to an executable (COFF/PE on "
         "Windows, ELF on Linux)\n");
  printf("  --emit-obj          Emit a native object directly (default)\n");
  printf("  --target <triple>   Compile for another machine: x86_64-windows,\n"
         "                      x86_64-linux, x86_64-none, aarch64-linux,\n"
         "                      aarch64-none, i386-none, i686-none,\n"
         "                      i8086-none. The 16- and 32-bit targets emit a\n"
         "                      flat image only (--emit-flat)\n");
  printf("  --image-base <addr> Load address of the linked image, replacing "
         "the\n"
         "                      format's default (e.g. 0x7c00 for a boot "
         "sector)\n");
  printf("  --emit-flat <file>  Write a raw image with no object or executable\n"
         "                      container, laid out at --image-base. At 0x7c00\n"
         "                      it is padded to 512 bytes and signed 0x55AA\n");
  printf("  --emit-arm64        Emit a self-contained AArch64 Linux "
         "executable\n");
  printf("  --emit-arm64-obj    Emit an AArch64 relocatable object (link it "
         "on an\n"
         "                      ARM machine); the default output on an ARM "
         "host\n");
  printf("  --emit-ptx          Emit declared kernels as NVIDIA PTX (targets the\n"
         "                      local GPU, and the PTX ISA its driver can load,\n"
         "                      when one is visible; otherwise DGX Spark GB10,\n"
         "                      PTX 8.8 / sm_121a)\n");
  printf("  --gpu-info          Report the local GPUs, the driver, ptxas, and\n"
         "                      the target --emit-ptx would pick; no input file\n");
  printf("  --emit-kernel-decls[=F]\n"
         "                      With --emit-ptx, also write each kernel's\n"
         "                      host-side `extern kernel` declaration, so an\n"
         "                      importing host cannot drift from the module\n"
         "                      it launches (default: <output>.mettle)\n");
  printf("  --gpu-arch=A        PTX profile: native (this machine, and fail if\n"
         "                      there is none), gb10, portable (compute_75),\n"
         "                      sm_NN, or compute_NN\n");
  printf("  --ptx-version=M.m   Override the emitted PTX ISA version\n");
  printf("  --report-occupancy  With --emit-ptx: run ptxas -v on the emitted\n"
         "                      module and print each kernel's registers per\n"
         "                      thread plus the occupancy ceiling they imply,\n"
         "                      with whole-card fill thresholds when the SM\n"
         "                      count is known\n");
  printf("  --sms=N             SM count for those fill thresholds (default:\n"
         "                      ask the local driver; omitted when neither\n"
         "                      answers)\n");
  printf("  --gpu-checks        Emit the trap for each kernel-side\n"
         "                      gpu_assert; without it they cost nothing\n");
  printf("  --report-launches   List every dispatch site with the grid and\n"
         "                      block the compiler can fold, and the kernel\n"
         "                      each names\n");
  printf("  --report-gpu-types  Say what the device address-space, alignment,\n"
         "                      layout and uniformity analyses concluded, and\n"
         "                      what they cost\n");
  printf("  --gpu-tensor-tuple-budget=N\n"
         "                      PTX resident-fragment ceiling (0=architecture\n"
         "                      default); enables measured resident/replay variants\n"
         "                      without changing source or shared IR\n");
  printf("  --emit-spirv        Emit declared kernels as OpenCL SPIR-V\n");
  printf("  --emit-metal        Emit declared kernels as Metal Shading Language\n");
  printf("  --metal-version=V   Metal language version: 3.1, 3.2 (default), 4.0, 4.1\n");
  printf("  --gpu-provider=P    std/gpu runs launches through cuda or metal (metal on macOS)\n");
  printf("  --metal-fast-math   Let Metal reassociate and contract float math\n");
  printf("  --linker <mode>     Linker backend: auto, internal, gcc, or msvc "
         "(default: internal with --build, otherwise %s)\n",
         linker_mode_name(LINKER_MODE_AUTO));
  printf("  --subsystem <kind>  Windows subsystem: console or windows. A windows "
         "image gets no console window\n");
  printf("  --link-arg <arg>    Pass an extra linker argument (repeatable; "
         "use with --build)\n");
  printf("  -l<name>            Bind shared library lib<name>.so (ELF; "
         "repeatable, attached form only)\n");
  printf("  -L<dir>             Search <dir> for shared libraries "
         "(ELF; repeatable)\n");
  printf("  --rpath <dir>       Record <dir> in DT_RUNPATH (ELF; "
         "repeatable)\n");
  printf("  --shared            Emit a shared object instead of a program "
         "(ELF .so, Windows .dll)\n");
  printf("  --soname <name>     DT_SONAME for --shared output (ELF); DLL export "
         "name on Windows\n");
  printf("  --export-dynamic    Publish the program's own symbols so a loaded "
         "library can bind them\n");
  printf("  --dynamic-linker <path>\n");
  printf("                      Program loader for PT_INTERP (default "
         "%s)\n",
         METTLE_DEFAULT_ELF_INTERPRETER);
  printf("  --tracy             Link std/tracy with the Tracy profiler "
         "(requires --build)\n");
  printf("  --tracy-dir <dir>   Tracy repo root (default: TRACY_DIR env, then "
         ".mettle\\tracy_dir)\n");
  printf("  -d, --debug         Enable debug output and symbols\n");
  printf("  --dump-ast          Write parsed AST sidecar (.ast)\n");
  printf("  --dump-ir           Write optimized IR sidecar (.ir) without debug metadata\n");
  printf("  --simd-report       Report what each @simd loop became (needs -O/--release)\n");
  printf("  --report-rules      Print the verdict and interpreter steps of every @rule\n");
  printf("  --check-proofs      Trap at run time when a value the compiler proved to be a\n"
         "                      declared type is not one (survives --release)\n");
  printf("  --check-tasks       Trap at run time when a pointer handed to a task lies in\n"
         "                      the stack of the thread that spawned it\n");
  printf("  --check-overflow    Trap at run time when a signed +, - or * leaves its type;\n"
         "                      a declared range that bounds the operands deletes the\n"
         "                      check\n");
  printf("  --record-trace      Write what the run did to METTLE_TRACE (default\n"
         "                      mettle-trace.txt), for `mettle check-trace` to hold the\n"
         "                      trace rules to\n");
  printf("  check-trace <file.mettle> <trace>   Run the file's @rule over Trace against a\n"
         "                      run that --record-trace wrote down\n");
  printf("  --report-deadlines  Print each `where cycles < N` deadline, what its longest\n"
         "                      path costs, and whether it was proven or measured\n");
  printf("  --check-deadlines   Count what a path actually costs while the program runs and\n"
         "                      trap when it passes the longest path the compiler proved\n");
  printf("  --check-effects     Trap at run time when an effect the compiler proved absent\n"
         "                      is performed, or one it proved provided is not (survives\n"
         "                      --release)\n");
  printf("  --report-target     Print the target in effect as a Mettle TargetDesc\n");
  printf("  --rule-budget=N     Fail the build when the rules spend more than N steps\n");
  printf("  --report-meta       Print what metafunctions ran and what they cost\n");
  printf("  --meta-budget=N     Fail the build when metafunctions spend more than N steps\n");
  printf("  --report-proofs     Print every declared-type proof, its route and its cost\n");
  printf("  --report-twins      Print what each `reference` twin was checked on\n");
  printf("  --fix               Apply the replacement lines failing @rules proposed\n");
  printf("  why <file> <fn> <effect>    Print the chain that made an effect hold\n");
  printf("  why <file> <line> <type>    Print the proof that made a conversion hold\n");
  printf("  --proof-budget=N    Fail the build when the prover spends more than N steps\n");
  printf("  --report-effects    Print what each function performs and needs, and the cost\n");
  printf("  --effect-budget=N   Fail the build when the effect pass spends more than N\n"
         "                      steps\n");
  printf("  --explain           Report every optimization decision in the input file --\n"
         "                      loop vectorization and call inlining, with the reason\n"
         "                      whenever the optimizer declined (needs -O/--release).\n"
         "                      Re-runs lead with what CHANGED since the last build,\n"
         "                      regressions first\n");
  printf("  --explain=SELECTOR  Narrow the report to one slice of it: missed,\n"
         "                      fixable, proven, loops, calls, a function name, or a\n"
         "                      decision code (the id in brackets after a verdict)\n");
  printf("  --explain-json      Also write <output-stem>.explain.json (machine-\n"
         "                      readable report; implies --explain)\n");
  printf("  --annotate-asm      Print the emitted assembly annotated with the codegen\n"
         "                      decision behind each instruction (spill, vectorized\n"
         "                      kernel, strength-reduced divide, ...), a per-op\n"
         "                      latency/throughput cost model, recovered loops with\n"
         "                      their port bottleneck, a register-lifetime map, and an\n"
         "                      instruction-mix summary; also writes a\n"
         "                      <output-stem>.annot.json sidecar. Implies -O and joins\n"
         "                      the --explain remarks. Pair with --asm-syntax=\n");
  printf("  --asm-syntax=S       Assembly syntax for --annotate-asm: intel, att, or\n"
         "                      both (default both)\n");
  printf("  --annotate-lines=A-B Focused codegen report for source lines A..B (or a\n"
         "                      single line A): the emitted asm, per-op cost, the loops\n"
         "                      covering the range, the registers live across it, and\n"
         "                      the optimizer's decisions. Compact, for tools/LLMs.\n");
  printf("  --annotate-fn=NAME   Restrict --annotate-lines/--annotate-asm to one\n"
         "                      function\n");
  printf("  --annotate-hot[=N]  Print the program's top N codegen hotspots (hottest\n"
         "                      loops by cycles/iteration and functions by weighted\n"
         "                      cost); default N=8. For tools/LLMs.\n");
  printf("  --ml-opt            Run the learned ML IR optimizer after the classical\n"
         "                      passes (experimental). A GNN flags redundancy/algebra\n"
         "                      classical missed; sound transforms realize each, and\n"
         "                      every applied rewrite is re-executed through the\n"
         "                      translation-validation interpreter and discarded on\n"
         "                      divergence. Enables -O. See docs/ml-opt.md; with\n"
         "                      --explain it reports each rewrite and its verdict.\n");
  printf("  --ml-opt-speculative  Also apply the model's unproven proposals (dead-\n"
         "                      code deletes). These stand ONLY when the validator\n"
         "                      can execute the function and finds no divergence.\n"
         "                      Implies --ml-opt.\n");
  printf("  -g, --debug-symbols Generate debug symbols\n");
  printf("  -l, --line-mapping  Generate source line mapping\n");
  printf("  -s, --stack-trace   Report a crash at the exact statement. Records\n"
         "                      a location per instruction, which the\n"
         "                      register-allocating backend carries through\n"
         "                      lowering\n");
  printf("  --no-crash-report   Drop the default crash report from a linked\n"
         "                      executable. By default a fault names itself,\n"
         "                      its address and the function it happened in,\n"
         "                      for about 8 KB and no change to codegen\n");
  printf("  --debug-format <fmt> Debug format: dwarf, stabs, or map (default: "
         "dwarf)\n");
  printf("  -O, --optimize      Enable optimizations\n");
  printf("  -r, --release       Optimize for size (enables -O, strips comments, "
         "and drops unreachable functions)\n");
  printf("  --prelude           Auto-import the standard prelude (std/io, "
         "std/net, etc.)\n");
  printf("  --profile           Print per-phase compilation timings\n");
  printf("  --profile-runtime   Emit function-level runtime timing report "
         "(disables inlining)\n");
  printf("  --profile-runtime-ops  Emit runtime op-class counters per function "
         "(after optimization)\n");
  printf("  --profile-blocks    Emit per-basic-block execution counters to a "
         ".mprof sidecar\n"
         "                      (path via METTLE_PROFILE_OUT); fuses with "
         "--annotate-asm for\n"
         "                      the VTune-style codegen profile view. Implies "
         "--profile-runtime\n");
  printf("  --debug-hooks       Instrument for the interactive source-level "
         "debugger (requires -O0; used by the editor's F5)\n");
  printf("  --safe              Check every memory access the compiler cannot "
         "prove in bounds (kept under --release)\n");
  printf("  --native-heap       Route new/malloc/calloc/realloc/free through "
         "the Mettle allocator (std/alloc)\n");
  printf("  --static            Accepted for compatibility; owned ELF builds are "
         "always static\n");
  printf("  --musl              Rejected; owned runtime builds never link musl\n");
  printf("  --debug-compiler    Track compiler context for internal error reports\n");
  printf("  -V, --version       Show version information\n");
  printf("  -h, --help          Show this help message\n");
  printf("\nExamples:\n");
  printf("  %s app.mettle -o app.obj\n", program_name);
  printf("      Compile to a native object file.\n");
  printf("  %s --build app.mettle -o app.exe\n", program_name);
  printf("      Self-contained build: COFF object + internal PE linker.\n");
  printf("  %s --build --release app.mettle -o app.exe\n", program_name);
  printf("      Optimized, comment-stripped release build.\n");
  printf("  %s --build --tracy app.mettle -o app.exe\n", program_name);
  printf("      Build with Tracy instrumentation (set TRACY_DIR or "
         "--tracy-dir).\n");
  printf("\nHelp:\n");
  printf("  %s help <topic>     Detail on a topic (" METTLE_HELP_TOPICS ")\n",
         program_name);
  printf("  %s help all         Print every topic\n", program_name);
  printf("  %s docs [topic]     Show the matching documentation file path\n",
         program_name);
}

char *read_file(const char *filename) {
  FILE *file = fopen(filename, "r");
  if (!file) {
    return NULL;
  }

  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return NULL;
  }
  long size = ftell(file);
  if (size < 0) {
    fclose(file);
    return NULL;
  }
  if (fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return NULL;
  }

  char *buffer = malloc(size + 1);
  if (!buffer) {
    fclose(file);
    return NULL;
  }

  size_t bytes_read = fread(buffer, 1, size, file);
  if (bytes_read < (size_t)size && ferror(file)) {
    free(buffer);
    fclose(file);
    return NULL;
  }
  buffer[bytes_read] = '\0';

  fclose(file);
  return buffer;
}
