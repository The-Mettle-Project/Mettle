#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "main.h"
#include "common.h"
#include "codegen/binary/startup.h"
#include "codegen/binary/mir_annotate.h"
#include "codegen/binary_emitter.h"
#include "codegen/binary/arm64_ir.h"
#include "codegen/program_entry.h"
#include "codegen/ptx_emitter.h"
#include "linker/pe_emitter.h"
#include "string_intern.h"
#include "compiler/compiler_context.h"
#include "compiler/compiler_crash.h"
#include "tracy_build.h"
#include "ir/ir.h"
#include "ir/ir_optimize.h"
#include "ir/ir_explain_memory.h"
#include "ir/ir_profile.h"
#include "ir/ir_debug_hooks.h"
#include "semantic/import_resolver.h"
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
#if !defined(__MINGW32__)
/* Avoid windows.h here: winnt.h defines TokenType, which clashes with lexer.h. */
typedef long long MettleQpcTicks;
__declspec(dllimport) int __stdcall QueryPerformanceFrequency(MettleQpcTicks *frequency);
__declspec(dllimport) int __stdcall QueryPerformanceCounter(MettleQpcTicks *counter);
#endif
#else
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
/* waitpid lives in <sys/wait.h>, but that header transitively pulls in
 * <sys/ucontext.h>, whose REG_R8.. enumerators collide with the compiler's
 * own register enum in semantic/register_allocator.h. Forward-declare the one
 * function we need instead (mirroring the manual QueryPerformanceCounter decls
 * used on Windows to dodge the windows.h/lexer.h clash). The wait-status
 * encoding decoded at the call site is the stable Linux/musl layout. */
extern pid_t waitpid(pid_t pid, int *wstatus, int options);
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

#define METTLE_STRINGIFY_(x) #x
#define METTLE_STRINGIFY(x) METTLE_STRINGIFY_(x)
#ifndef METTLE_VERSION
#ifdef METTLE_VERSION_RAW
#define METTLE_VERSION METTLE_STRINGIFY(METTLE_VERSION_RAW)
#else
#define METTLE_VERSION "v0.13.0"
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

static int compiler_options_use_profile_runtime(const CompilerOptions *options) {
  return options &&
         (options->profile_runtime || options->profile_runtime_ops);
}

typedef struct {
  int enabled;
  double phases_ms[PROFILE_PHASE_COUNT];
} CompilerProfile;

static double compiler_profile_now_ms(void) {
#if defined(_WIN32) && !defined(__MINGW32__)
  static MettleQpcTicks frequency = 0;
  MettleQpcTicks counter = 0;

  if (frequency == 0) {
    QueryPerformanceFrequency(&frequency);
  }
  if (frequency == 0) {
    return 0.0;
  }
  QueryPerformanceCounter(&counter);
  return (double)counter * 1000.0 / (double)frequency;
#else
  struct timeval tv;

  gettimeofday(&tv, NULL);
  return (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0;
#endif
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
}

static void compiler_set_phase(MettleCompilerPhase phase) {
  mettle_compiler_ctx_set_phase(phase);
}

static int directory_exists(const char *path) {
  if (!path || path[0] == '\0') {
    return 0;
  }
#ifdef _WIN32
  struct _stat st;
  return _stat(path, &st) == 0 && (st.st_mode & _S_IFDIR) != 0;
#else
  struct stat st;
  return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
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
  char *program_path = NULL;
  if (_get_pgmptr(&program_path) == 0 && program_path &&
      program_path[0] != '\0') {
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
  ssize_t len = readlink("/proc/self/exe", buffer, PATH_MAX);
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

/* Point the ML optimizer at its bundled model/libraries (bin/mlopt by the exe, or
 * tools/mlopt in a dev tree) via env vars; a user-set value always wins. */
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
        putenv(kv); /* putenv keeps the pointer; intentionally not freed */
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

/* Single source of truth for the help-topic list. Referenced by print_usage,
 * the topic dispatcher, and the unknown-topic error so they cannot drift. */
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
    printf("runtime - Mettle's (lack of a) language runtime\n\n");
    printf("  No GC, no async scheduler, no heap manager, no thread pool, no "
           "mandatory startup shim.\n");
    printf("  A typical program links libc and nothing else. `new`, array "
           "literals, and string concatenation\n");
    printf("  call calloc(1, n) directly.\n\n");
    printf("  Two opt-in helper objects ship with the compiler and are linked "
           "only when referenced:\n");
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
    printf("                                 must catch and heal it)\n");
    print_doc_reference(argv0, "translation-validation.md");
    return 0;
  }

  if (strcmp(topic, "diagnostics") == 0 || strcmp(topic, "errors") == 0 ||
      strcmp(topic, "warnings") == 0) {
    printf("diagnostics - compile errors, warnings, and tooling output\n\n");
    printf("  Every diagnostic carries a stable code (E0001..E0007, "
           "M0101..M0112), a source snippet\n");
    printf("  with the offending range underlined, and a help suggestion. The "
           "compiler recovers after\n");
    printf("  errors, so one compile reports every problem in the file.\n\n");
    printf("  mettle explain <CODE>       extended docs for a code (try: "
           "mettle explain E0004)\n");
    printf("  mettle explain list         index of every diagnostic code\n");
    printf("  --error-format=json         one JSON object per diagnostic on "
           "stderr, for editors/CI\n");
    printf("  NO_COLOR / CLICOLOR_FORCE   disable / force ANSI colors\n\n");
    printf("  Warnings include unused variables (prefix a name with '_' to "
           "opt out), unreachable code,\n");
    printf("  and compile-time memory-safety findings (use-after-free, leaks, "
           "double free, ...).\n");
    print_doc_reference(argv0, "diagnostics.md");
    return 0;
  }

  if (strcmp(topic, "interop") == 0 || strcmp(topic, "c") == 0) {
    printf("interop - calling C and OS APIs\n\n");
    printf("  Declare external C functions with extern fn.\n");
    printf("  Prefer std/win32 for common Windows OS APIs.\n");
    printf("  Use --link-arg for extra linker libraries in --build mode.\n");
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

static char *default_executable_filename(const char *input_filename) {
  if (!input_filename || input_filename[0] == '\0') {
    return NULL;
  }

  return replace_extension(input_filename, ".exe");
}

static const char *default_object_output_filename(void) {
  return binary_target_format_host_default() == BINARY_TARGET_FORMAT_ELF_X64
             ? "output.o"
             : "output.obj";
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

#ifndef _WIN32
#define METTLE_ELF_DYNAMIC_LINKER "/lib64/ld-linux-x86-64.so.2"

/* Runs `gcc -print-file-name=<file>` and returns the strdup'd path, or NULL
 * when gcc is missing or does not ship the file (gcc echoes the bare name
 * back, without a '/', when it has no path for it). <file> is always a
 * compiled-in literal, so the popen command cannot be influenced by user
 * input. */
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

/* Returns dirname(reference) + "/" + file (file may be "" to get the bare
 * directory prefix for -L). */
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

/* Plain builds get their ELF symbol table stripped at link: .symtab/.strtab
 * (every function and string-literal label) are dead weight at runtime and
 * outweigh the program's actual section content several times over for small
 * binaries. This matches the Windows internal PE linker, which never emits a
 * symbol table. Debug/trace/profile builds keep symbols for tooling. */
static int mettle_elf_keep_symbols(const CompilerOptions *options) {
  return options &&
         (options->debug_mode || options->generate_debug_symbols ||
          options->generate_line_mapping ||
          options->generate_stack_trace_support || options->tracy ||
          compiler_options_use_profile_runtime(options));
}

/* Links the ELF executable by invoking ld directly, reproducing the link line
 * the gcc driver would build (crt1/crti/crtbegin + objects + -lc -lpthread +
 * crtend/crtn against the glibc dynamic linker). The gcc driver itself costs
 * ~140ms per link in spec processing and collect2 while ld links these
 * programs in under 10ms; with mettle's own compile around 10ms the driver
 * would otherwise dominate total build time. Quiet on failure (stderr is
 * dropped in the child): the caller falls back to the gcc driver, which
 * reports any real link error. Returns 0 on success. */
static int mettle_link_elf_direct(const char *object_filename,
                                  const char *executable_filename,
                                  const char *posix_helpers_object,
                                  const char *atomics_object,
                                  const char *crash_handler_object,
                                  const char *profile_object,
                                  int strip_symbols) {
  char *crt1 = NULL;
  char *crti = NULL;
  char *crtn = NULL;
  char *crtbegin = NULL;
  char *crtend = NULL;
  char *libc_dir = NULL;
  char *gcc_lib_dir = NULL;
  const char *argv_list[32];
  size_t argc_used = 0u;
  int result = 1;
  pid_t pid;
  int status = 0;

  /* Non-glibc layouts (e.g. musl) keep using the gcc driver. */
  if (access(METTLE_ELF_DYNAMIC_LINKER, F_OK) != 0) {
    return 1;
  }

  crt1 = mettle_gcc_print_file_name("crt1.o");
  crtbegin = mettle_gcc_print_file_name("crtbegin.o");
  if (!crt1 || !crtbegin) {
    goto cleanup;
  }
  crti = mettle_sibling_path(crt1, "crti.o");
  crtn = mettle_sibling_path(crt1, "crtn.o");
  crtend = mettle_sibling_path(crtbegin, "crtend.o");
  libc_dir = mettle_sibling_path(crt1, "");
  gcc_lib_dir = mettle_sibling_path(crtbegin, "");
  if (!crti || !crtn || !crtend || !libc_dir || !gcc_lib_dir) {
    goto cleanup;
  }
  if (access(crti, F_OK) != 0 || access(crtn, F_OK) != 0 ||
      access(crtend, F_OK) != 0) {
    goto cleanup;
  }

  argv_list[argc_used++] = "ld";
  argv_list[argc_used++] = "--eh-frame-hdr";
  argv_list[argc_used++] = "--gc-sections";
  /* Merge the read-only and executable LOAD segments (the pre-binutils-2.31
   * layout): separate-code spends up to two 4K pages of file padding per
   * binary for page-exact W^X mapping, a poor trade for small executables
   * whose code is non-PIE at a fixed base to begin with. */
  argv_list[argc_used++] = "-z";
  argv_list[argc_used++] = "noseparate-code";
  if (strip_symbols) {
    argv_list[argc_used++] = "-s";
  }
  argv_list[argc_used++] = "-dynamic-linker";
  argv_list[argc_used++] = METTLE_ELF_DYNAMIC_LINKER;
  argv_list[argc_used++] = "-o";
  argv_list[argc_used++] = executable_filename;
  argv_list[argc_used++] = crt1;
  argv_list[argc_used++] = crti;
  argv_list[argc_used++] = crtbegin;
  argv_list[argc_used++] = object_filename;
  if (posix_helpers_object) {
    argv_list[argc_used++] = posix_helpers_object;
  }
  if (atomics_object) {
    argv_list[argc_used++] = atomics_object;
  }
  if (crash_handler_object) {
    argv_list[argc_used++] = crash_handler_object;
  }
  if (profile_object) {
    argv_list[argc_used++] = profile_object;
  }
  argv_list[argc_used++] = "-L";
  argv_list[argc_used++] = libc_dir;
  argv_list[argc_used++] = "-L";
  argv_list[argc_used++] = gcc_lib_dir;
  argv_list[argc_used++] = "-lc";
  argv_list[argc_used++] = "-lpthread";
  argv_list[argc_used++] = crtend;
  argv_list[argc_used++] = crtn;
  argv_list[argc_used] = NULL;

  pid = fork();
  if (pid == 0) {
    /* Errors from a failed direct link would be confusing noise: the caller
     * retries through the gcc driver, which reports anything real. A failed
     * redirect only makes a failed link noisier, so it is not checked. */
    if (!freopen("/dev/null", "w", stdout) ||
        !freopen("/dev/null", "w", stderr)) {
    }
    execvp("ld", (char *const *)argv_list);
    _exit(127);
  }
  if (pid > 0 && waitpid(pid, &status, 0) >= 0 && (status & 0x7f) == 0 &&
      ((status >> 8) & 0xff) == 0) {
    result = 0;
  }

cleanup:
  free(crt1);
  free(crti);
  free(crtn);
  free(crtbegin);
  free(crtend);
  free(libc_dir);
  free(gcc_lib_dir);
  return result;
}

/* Links a native ELF executable by invoking the system C compiler (gcc). gcc
 * supplies the C runtime startup (crt1.o/crti.o/crtn.o, hence `_start`), the
 * dynamic linker, and libc, so the program runs on the same C runtime routines
 * (allocator, stdio, errno) as the Windows build links against MSVCRT. The
 * resulting binary is dynamically linked against the target's libc — the
 * accepted tradeoff for a consistent runtime across Windows and Linux.
 * Used on ELF hosts (Linux); does not depend on the Windows-only link helpers
 * below. Returns 0 on success. */
static int mettle_link_elf_executable(const char *object_filename,
                                      const char *executable_filename,
                                      const CompilerOptions *options,
                                      const char *runtime_directory) {
  char **argv_list = NULL;
  char *posix_helpers_object = NULL;
  char *atomics_object = NULL;
  char *crash_handler_object = NULL;
  char *profile_object = NULL;
  const char *cc = (options && options->musl_link) ? "musl-gcc" : "gcc";
  int result = 1;
  int profile_runtime =
      options && compiler_options_use_profile_runtime(options) ? 1 : 0;
  int stack_trace = options && options->generate_stack_trace_support ? 1 : 0;

  /* Auto-link the Linux-side runtime helpers so users don't have to manage
   * link flags themselves (mirroring how the Windows internal PE linker
   * resolves ws2_32.dll / kernel32.dll automatically). Specifically:
   *  - bin/runtime/posix_helpers.o provides posix_get_errno + the Win32-style
   *    threading shims (mettle_thread_create etc.) std/thread (Linux variant)
   *    binds to.
   *  - bin/runtime/atomics.o provides mettle_atomic_* used by std/thread.
   *  - -lpthread provides pthread_create/join/mutex/etc. (no cost when
   *    unreferenced — libpthread is merged into libc on modern glibc).
   * The unused-section elimination in ld drops anything the program does not
   * reference, so always-linking these is essentially free. */
  if (runtime_directory) {
    posix_helpers_object = join_paths(runtime_directory, "posix_helpers.o");
    atomics_object = join_paths(runtime_directory, "atomics.o");
    if (stack_trace || profile_runtime) {
      crash_handler_object = join_paths(runtime_directory, "crash_handler.o");
    }
    if (profile_runtime) {
      profile_object = join_paths(runtime_directory, "profile.o");
    }
  }

  /* gcc -no-pie "<program.o>" "<posix_helpers.o>" "<atomics.o>" -o "<exe>"
   * -lpthread [user link args]. crt1.o (supplied by gcc) provides `_start`,
   * which calls the program's `main` directly per the SysV C startup contract;
   * the backend's `main` already takes argc(RDI)/argv(RSI) in SysV order.
   * `-no-pie` is required because the backend emits non-position-independent
   * code (direct R_X86_64_PC32 relocations), which a PIE link (the default on
   * modern gcc) rejects. */
  if ((stack_trace || profile_runtime) && !crash_handler_object) {
    fprintf(stderr,
            "Error: Could not locate bundled crash_handler.o for Linux runtime "
            "support\n");
    free(posix_helpers_object);
    free(atomics_object);
    return 1;
  }
  if (profile_runtime && !profile_object) {
    fprintf(stderr,
            "Error: Could not locate bundled profile.o for Linux runtime "
            "profiling\n");
    free(posix_helpers_object);
    free(atomics_object);
    free(crash_handler_object);
    return 1;
  }

  /* Fast path: link with ld directly. --static/--musl builds (library group
   * ordering and musl specs are the driver's job) and builds with user
   * --link-arg values (gcc-flavoured flags like -Wl,...) keep the driver;
   * everything else only falls back to it when the direct link fails. */
  if (!(options && (options->static_link || options->musl_link)) &&
      !(options && options->link_argument_count > 0) &&
      mettle_link_elf_direct(object_filename, executable_filename,
                             posix_helpers_object, atomics_object,
                             crash_handler_object, profile_object,
                             !mettle_elf_keep_symbols(options)) == 0) {
    result = 0;
  }

  /* Build the argv vector directly and exec the compiler via fork/execvp
   * instead of handing a constructed command string to system(). Because no
   * shell ever interprets the arguments, none of the caller-controlled
   * strings — the object/executable filenames or the user-supplied
   * --link-arg values — can inject shell commands (CWE-78) or be word-split
   * into unintended options (CWE-88). Each --link-arg is forwarded as exactly
   * one argv element, matching how it was collected at parse time. */
  if (result != 0) {
    /* Upper bound: cc, -no-pie, -Wl,--gc-sections, -s, -static, object, -o,
     * executable, -lpthread (9) + up to 4 runtime objects + every link
     * argument + NULL terminator. */
    size_t max_args = 9u + 4u + 1u +
                      (options ? options->link_argument_count : 0u);
    size_t argc_used = 0u;
    pid_t pid;
    int status = 0;

    argv_list = malloc(sizeof(*argv_list) * max_args);
    if (!argv_list) {
      fprintf(stderr, "Error: Failed to allocate ELF link argv\n");
      free(posix_helpers_object);
      free(atomics_object);
      free(crash_handler_object);
      free(profile_object);
      return 1;
    }

    /* execvp does not modify argv contents; the const casts are safe. */
    argv_list[argc_used++] = (char *)cc;
    argv_list[argc_used++] = (char *)"-no-pie";
    argv_list[argc_used++] = (char *)"-Wl,--gc-sections";
    if (!mettle_elf_keep_symbols(options)) {
      argv_list[argc_used++] = (char *)"-s";
    }
    if (options && (options->static_link || options->musl_link)) {
      argv_list[argc_used++] = (char *)"-static";
    }
    argv_list[argc_used++] = (char *)object_filename;
    if (posix_helpers_object) {
      argv_list[argc_used++] = posix_helpers_object;
    }
    if (atomics_object) {
      argv_list[argc_used++] = atomics_object;
    }
    if (crash_handler_object) {
      argv_list[argc_used++] = crash_handler_object;
    }
    if (profile_object) {
      argv_list[argc_used++] = profile_object;
    }
    argv_list[argc_used++] = (char *)"-o";
    argv_list[argc_used++] = (char *)executable_filename;
    argv_list[argc_used++] = (char *)"-lpthread";
    if (options) {
      for (size_t i = 0; i < options->link_argument_count; i++) {
        const char *arg = options->link_arguments[i];
        if (!arg || arg[0] == '\0') {
          continue;
        }
        argv_list[argc_used++] = (char *)arg;
      }
    }
    argv_list[argc_used] = NULL;

    pid = fork();
    if (pid < 0) {
      fprintf(stderr, "Error: Failed to fork to run %s: %s\n", cc,
              strerror(errno));
    } else if (pid == 0) {
      execvp(cc, argv_list);
      /* Reached only if exec failed. */
      fprintf(stderr, "Error: Failed to execute %s: %s\n", cc,
              strerror(errno));
      _exit(127);
    } else if (waitpid(pid, &status, 0) < 0) {
      fprintf(stderr, "Error: Failed to wait for %s: %s\n", cc,
              strerror(errno));
    } else if ((status & 0x7f) == 0 && ((status >> 8) & 0xff) == 0) {
      /* (status & 0x7f) == 0 means a normal exit (no terminating signal);
       * (status >> 8) & 0xff is the exit code — i.e. WIFEXITED && WEXITSTATUS
       * == 0 without pulling in <sys/wait.h>. */
      result = 0;
    } else {
      fprintf(stderr, "Error: %s failed to produce an ELF executable\n", cc);
    }
  }

  free(argv_list);
  free(posix_helpers_object);
  free(atomics_object);
  free(crash_handler_object);
  free(profile_object);
  return result;
}
#endif /* !_WIN32 */

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
      "kernel32.dll", "ucrtbase.dll", "msvcrt.dll", "ws2_32.dll",
      "user32.dll",   "gdi32.dll",    "advapi32.dll", "winmm.dll"};
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

static int object_has_undefined_symbol_prefix(const char *object_path,
                                              const char *prefix) {
  CoffObject *object = NULL;
  char *error_message = NULL;
  size_t i = 0u;
  int found = 0;

  if (!object_path || !prefix) {
    return 0;
  }

  if (!coff_object_read(object_path, &object, &error_message)) {
    free(error_message);
    return 1;
  }

  for (i = 0u; i < object->symbol_count; i++) {
    const CoffSymbol *symbol = &object->symbols[i];
    if (symbol->is_auxiliary || symbol->section_number != 0 || !symbol->name) {
      continue;
    }
    if (strncmp(symbol->name, prefix, strlen(prefix)) == 0) {
      found = 1;
      break;
    }
  }

  free(error_message);
  coff_object_destroy(object);
  return found;
}

static int object_needs_runtime_object(const char *object_path,
                                       const char *prefix) {
  return object_has_undefined_symbol_prefix(object_path, prefix);
}

static int object_needs_crash_handler(const char *object_path) {
  return object_needs_runtime_object(object_path, "mettle_crash_");
}

static int object_needs_atomics(const char *object_path) {
  return object_needs_runtime_object(object_path, "mettle_atomic_");
}

static int object_needs_profile_runtime(const char *object_path) {
  return object_needs_runtime_object(object_path, "mettle_profile_");
}

static int object_needs_debug_runtime(const char *object_path) {
  return object_needs_runtime_object(object_path, "mettle_dbg_");
}

static int object_needs_tracy_helpers(const char *object_path) {
  return object_needs_runtime_object(object_path, "mettle_tracy_");
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

static int append_gcc_link_arguments(char *buffer, size_t buffer_size,
                                     size_t *offset,
                                     const CompilerOptions *options) {
  if (!options) {
    return 1;
  }

  for (size_t i = 0; i < options->link_argument_count; i++) {
    const char *arg = options->link_arguments[i];
    if (!arg || arg[0] == '\0') {
      continue;
    }
    if (!append_argument_text(buffer, buffer_size, offset, " ")) {
      return 0;
    }
    if (!append_argument_text(buffer, buffer_size, offset, arg)) {
      return 0;
    }
  }

  return 1;
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
  if (!tool_name || tool_name[0] == '\0') {
    return 0;
  }

  size_t command_len = strlen(tool_name) + 32;
  char *command = malloc(command_len);
  if (!command) {
    return 0;
  }

  snprintf(command, command_len, "where %s >nul 2>&1", tool_name);
  int result = run_system_command(command);
  free(command);
  return result == 0;
}

static int write_internal_startup_object(const char *path, int profile_runtime,
                                         int stack_trace_init,
                                         int main_wants_argc_argv) {
  return binary_write_program_startup_object(path, profile_runtime,
                                             stack_trace_init,
                                             main_wants_argc_argv);
}

/* Build → link routing is documented in docs/linker-build-pipelines.md (asm+GCC
 * vs emit-obj+internal vs emit-obj+external GCC). */

static int mettle_link_internal(const char **object_paths,
                                  size_t object_count,
                                  const char *executable_filename,
                                  int include_shell32,
                                  const CompilerOptions *options) {
  LinkResolutionOptions resolution_options = {"mainCRTStartup", 16u, 1};
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
    fprintf(stderr, "Warning: Internal linker symbol resolution failed: %s\n",
            error_message ? error_message : "unknown error");
    goto cleanup;
  }

  emission_options.import_library_paths =
      (const char **)import_library_paths.items;
  emission_options.import_library_count = import_library_paths.count;
  emission_options.import_dll_names = (const char **)import_dll_names.items;
  emission_options.import_dll_count = import_dll_names.count;
  if (!pe_emit_executable(resolution, executable_filename, &emission_options,
                          &error_message)) {
    fprintf(stderr, "Warning: Internal linker PE emission failed: %s\n",
            error_message ? error_message : "unknown error");
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

static int mettle_link_objects_with_gxx(const char **object_paths,
                                        size_t object_count,
                                        const char *executable_filename,
                                        const CompilerOptions *options) {
  size_t cmd_len = strlen(executable_filename) + 512u;
  size_t i = 0u;
  size_t offset = 0u;
  char *command = NULL;
  int result = 1;

  if (!object_paths || object_count == 0u || !executable_filename) {
    fprintf(stderr, "Error: Missing inputs for g++ Tracy link\n");
    return 1;
  }

  for (i = 0u; i < object_count; i++) {
    if (object_paths[i] && object_paths[i][0] != '\0') {
      cmd_len += strlen(object_paths[i]) + 4u;
    }
  }
  if (options) {
    for (i = 0u; i < options->link_argument_count; i++) {
      if (options->link_arguments[i]) {
        cmd_len += strlen(options->link_arguments[i]) + 2u;
      }
    }
  }

  command = malloc(cmd_len);
  if (!command) {
    fprintf(stderr, "Error: Failed to allocate g++ Tracy link command\n");
    return 1;
  }

  if (!append_argument_text(command, cmd_len, &offset, "g++ -o ") ||
      !append_quoted_argument(command, cmd_len, &offset, executable_filename)) {
    free(command);
    fprintf(stderr, "Error: Failed to build g++ Tracy link command\n");
    return 1;
  }

  for (i = 0u; i < object_count; i++) {
    if (!object_paths[i] || object_paths[i][0] == '\0') {
      continue;
    }
    if (!append_argument_text(command, cmd_len, &offset, " ") ||
        !append_quoted_argument(command, cmd_len, &offset, object_paths[i])) {
      free(command);
      fprintf(stderr, "Error: Failed to build g++ Tracy link command\n");
      return 1;
    }
  }

  if (!append_argument_text(command, cmd_len, &offset,
                            " -lkernel32 -luser32 -lgdi32 -ladvapi32 -lws2_32 "
                            "-lsecur32 -ldbghelp") ||
      !append_gcc_link_arguments(command, cmd_len, &offset, options)) {
    free(command);
    fprintf(stderr, "Error: Failed to build g++ Tracy link command\n");
    return 1;
  }

  if (run_system_command(command) != 0) {
    fprintf(stderr, "Warning: g++ Tracy link step failed\n");
    result = 1;
  } else {
    result = 0;
  }

  free(command);
  return result;
}

static int mettle_link_object_with_gcc(const char *object_filename,
                                          const char *executable_filename,
                                          const char *const *runtime_objects,
                                          size_t runtime_object_count,
                                          const CompilerOptions *options) {
  size_t gcc_len = strlen(object_filename) + strlen(executable_filename) + 192;
  for (size_t i = 0; i < runtime_object_count; i++) {
    if (runtime_objects[i] && runtime_objects[i][0] != '\0') {
      gcc_len += strlen(runtime_objects[i]) + 1;
    }
  }
  if (options) {
    for (size_t i = 0; i < options->link_argument_count; i++) {
      if (options->link_arguments[i]) {
        gcc_len += strlen(options->link_arguments[i]) + 1;
      }
    }
  }

  char *gcc_command = malloc(gcc_len);
  if (!gcc_command) {
    fprintf(stderr, "Error: Failed to allocate GCC command\n");
    return 1;
  }

  size_t offset = 0;
  if (!append_argument_text(gcc_command, gcc_len, &offset,
                            "gcc -nostartfiles ") ||
      !append_quoted_argument(gcc_command, gcc_len, &offset, object_filename)) {
    free(gcc_command);
    fprintf(stderr, "Error: Failed to build GCC object link command\n");
    return 1;
  }
  for (size_t i = 0; i < runtime_object_count; i++) {
    if (!runtime_objects[i] || runtime_objects[i][0] == '\0') {
      continue;
    }
    if (!append_argument_text(gcc_command, gcc_len, &offset, " ") ||
        !append_quoted_argument(gcc_command, gcc_len, &offset,
                                runtime_objects[i])) {
      free(gcc_command);
      fprintf(stderr, "Error: Failed to build GCC object link command\n");
      return 1;
    }
  }
  if (!append_argument_text(gcc_command, gcc_len, &offset, " -o ") ||
      !append_quoted_argument(gcc_command, gcc_len, &offset,
                              executable_filename) ||
      !append_argument_text(gcc_command, gcc_len, &offset, " -lkernel32") ||
      !append_gcc_link_arguments(gcc_command, gcc_len, &offset, options)) {
    free(gcc_command);
    fprintf(stderr, "Error: Failed to build GCC object link command\n");
    return 1;
  }

  int result = run_system_command(gcc_command);
  free(gcc_command);
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
  if (!append_argument_text(link_command, link_len, &offset,
                            "link.exe /nologo /subsystem:console /out:") ||
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
                            " kernel32.lib msvcrt.lib") ||
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

static int mettle_link_object_file(const char *object_filename,
                                     const char *executable_filename,
                                     const char *runtime_directory,
                                     const CompilerOptions *options) {
  LinkerMode linker_mode =
      options ? options->linker_mode : LINKER_MODE_AUTO;
  int has_gcc = 0;
  int has_link = 0;

  if (!object_filename || !executable_filename || !runtime_directory) {
    fprintf(stderr, "Error: Missing build inputs for executable generation\n");
    return 1;
  }

  has_gcc = (linker_mode == LINKER_MODE_AUTO || linker_mode == LINKER_MODE_GCC)
                ? windows_tool_exists("gcc")
                : 0;
  has_link =
      (linker_mode == LINKER_MODE_AUTO || linker_mode == LINKER_MODE_MSVC)
          ? windows_tool_exists("link.exe")
          : 0;
  if (linker_mode == LINKER_MODE_GCC && !has_gcc) {
    fprintf(stderr, "Error: gcc was requested with --linker gcc but was not found.\n");
    return 1;
  }
  if (linker_mode == LINKER_MODE_MSVC && !has_link) {
    fprintf(stderr,
            "Error: link.exe was requested with --linker msvc but was not found.\n");
    return 1;
  }
  char *crash_gcc_object = join_paths(runtime_directory, "crash_handler.o");
  char *crash_msvc_object = join_paths(runtime_directory, "crash_handler.obj");
  char *atomics_gcc_object = join_paths(runtime_directory, "atomics.o");
  char *atomics_msvc_object = join_paths(runtime_directory, "atomics.obj");
  char *profile_gcc_object = join_paths(runtime_directory, "profile.o");
  char *profile_msvc_object = join_paths(runtime_directory, "profile.obj");
  if (!crash_gcc_object || !crash_msvc_object || !atomics_gcc_object ||
      !atomics_msvc_object || !profile_gcc_object || !profile_msvc_object) {
    fprintf(stderr, "Error: Failed to allocate build paths\n");
    free(crash_gcc_object);
    free(crash_msvc_object);
    free(atomics_gcc_object);
    free(atomics_msvc_object);
    free(profile_gcc_object);
    free(profile_msvc_object);
    return 1;
  }

  int needs_crash = object_needs_crash_handler(object_filename);
  int needs_atomics = object_needs_atomics(object_filename);
  int needs_profile = object_needs_profile_runtime(object_filename);
  int profile_runtime =
      options && compiler_options_use_profile_runtime(options) ? 1 : 0;
  if (profile_runtime) {
    needs_profile = 1;
  }
  if (needs_profile) {
    needs_crash = 1;
  }

  /* --debug-hooks: the program references mettle_dbg_* hooks resolved by the
   * bundled debug runtime object (same auto-link pattern as the profiler).
   * Stack buffers, so the error paths above/below need no extra frees. */
  char debug_gcc_object[1024];
  char debug_msvc_object[1024];
  int needs_debug = object_needs_debug_runtime(object_filename) ||
                    (options && options->debug_hooks);
  snprintf(debug_gcc_object, sizeof(debug_gcc_object), "%s/debug.o",
           runtime_directory);
  snprintf(debug_msvc_object, sizeof(debug_msvc_object), "%s/debug.obj",
           runtime_directory);

  int use_tracy = compiler_options_use_tracy(options);
  int needs_tracy_helpers =
      use_tracy || object_needs_tracy_helpers(object_filename);
  TracyBuildArtifacts tracy_artifacts = {0};
  char *tracy_directory = NULL;
  char *tracy_error = NULL;
  const char *tracy_helpers_object = NULL;
  char *tracy_helpers_gcc_object =
      join_paths(runtime_directory, "tracy_helpers.o");
  char *tracy_helpers_msvc_object =
      join_paths(runtime_directory, "tracy_helpers.obj");
  if (!tracy_helpers_gcc_object || !tracy_helpers_msvc_object) {
    fprintf(stderr, "Error: Failed to allocate Tracy build paths\n");
    free(tracy_helpers_gcc_object);
    free(tracy_helpers_msvc_object);
    free(crash_gcc_object);
    free(crash_msvc_object);
    free(atomics_gcc_object);
    free(atomics_msvc_object);
    free(profile_gcc_object);
    free(profile_msvc_object);
    return 1;
  }

  if (use_tracy) {
    TracyBuildRequest tracy_request = {
        .tracy_directory = options ? options->tracy_directory : NULL,
        .stdlib_directory = options ? options->stdlib_directory : NULL,
        .executable_filename = executable_filename,
    };
    tracy_directory = tracy_resolve_directory(&tracy_request, &tracy_error);
    if (!tracy_directory) {
      fprintf(stderr, "Error: %s\n",
              tracy_error ? tracy_error : "Failed to resolve Tracy directory");
      free(tracy_error);
      free(tracy_helpers_gcc_object);
      free(tracy_helpers_msvc_object);
      free(crash_gcc_object);
      free(crash_msvc_object);
      free(atomics_gcc_object);
      free(atomics_msvc_object);
      free(profile_gcc_object);
      free(profile_msvc_object);
      return 1;
    }
    if (!tracy_build_support_objects(&tracy_request, tracy_directory,
                                     &tracy_artifacts, &tracy_error)) {
      fprintf(stderr, "Error: %s\n",
              tracy_error ? tracy_error
                          : "Failed to build Tracy support objects");
      free(tracy_error);
      free(tracy_directory);
      tracy_free_artifacts(&tracy_artifacts);
      free(tracy_helpers_gcc_object);
      free(tracy_helpers_msvc_object);
      free(crash_gcc_object);
      free(crash_msvc_object);
      free(atomics_gcc_object);
      free(atomics_msvc_object);
      free(profile_gcc_object);
      free(profile_msvc_object);
      return 1;
    }
    free(tracy_error);
    tracy_error = NULL;
    tracy_helpers_object = tracy_artifacts.helpers_object;
  } else if (needs_tracy_helpers) {
    tracy_helpers_object =
        (_access(tracy_helpers_msvc_object, 0) == 0) ? tracy_helpers_msvc_object
                                                     : tracy_helpers_gcc_object;
    if (_access(tracy_helpers_object, 0) != 0) {
      fprintf(stderr,
              "Error: Program references Tracy helpers but bundled stub "
              "object not found in '%s'\n",
              runtime_directory);
      free(tracy_helpers_gcc_object);
      free(tracy_helpers_msvc_object);
      free(crash_gcc_object);
      free(crash_msvc_object);
      free(atomics_gcc_object);
      free(atomics_msvc_object);
      free(profile_gcc_object);
      free(profile_msvc_object);
      return 1;
    }
  }

  int build_result = 1;

  if (use_tracy && tracy_artifacts.use_gxx_link) {
    size_t gxx_capacity =
        4u + (needs_crash ? 1u : 0u) + (needs_atomics ? 1u : 0u) +
        (needs_profile ? 1u : 0u);
    const char **gxx_objects = calloc(gxx_capacity, sizeof(const char *));
    size_t gxx_count = 0u;

    if (!gxx_objects) {
      fprintf(stderr, "Error: Failed to allocate g++ Tracy link object list\n");
      goto cleanup;
    }

    gxx_objects[gxx_count++] = object_filename;
    if (needs_crash) {
      if (_access(crash_gcc_object, 0) != 0) {
        fprintf(stderr,
                "Error: Bundled crash-handler runtime object not found in '%s'\n",
                runtime_directory);
        free(gxx_objects);
        goto cleanup;
      }
      gxx_objects[gxx_count++] = crash_gcc_object;
    }
    if (needs_atomics) {
      if (_access(atomics_gcc_object, 0) != 0) {
        fprintf(stderr,
                "Error: Bundled atomics runtime object not found in '%s'\n",
                runtime_directory);
        free(gxx_objects);
        goto cleanup;
      }
      gxx_objects[gxx_count++] = atomics_gcc_object;
    }
    if (needs_profile) {
      if (_access(profile_gcc_object, 0) != 0) {
        fprintf(stderr,
                "Error: Bundled profile runtime object not found in '%s'\n",
                runtime_directory);
        free(gxx_objects);
        goto cleanup;
      }
      gxx_objects[gxx_count++] = profile_gcc_object;
    }
    gxx_objects[gxx_count++] = tracy_artifacts.helpers_object;
    gxx_objects[gxx_count++] = tracy_artifacts.client_object;

    if (mettle_link_objects_with_gxx(gxx_objects, gxx_count, executable_filename,
                                     options) == 0) {
      build_result = 0;
    } else {
      fprintf(stderr, "Error: g++ Tracy link failed\n");
    }
    free(gxx_objects);
    goto cleanup;
  }

  if (linker_mode == LINKER_MODE_INTERNAL || linker_mode == LINKER_MODE_AUTO) {
    size_t object_capacity =
        6u + (use_tracy ? 2u : (needs_tracy_helpers ? 1u : 0u)) +
        (options ? options->link_argument_count : 0u);
    const char **object_paths = calloc(object_capacity, sizeof(const char *));
    const char *crash_object = NULL;
    const char *atomics_object = NULL;
    const char *profile_object = NULL;
    const char *debug_object = NULL;
    char *startup_object = replace_extension(executable_filename, ".startup.obj");
    size_t object_count = 0u;
    int startup_ready = 0;

    if (!object_paths) {
      fprintf(stderr, "Error: Failed to allocate internal-linker object list\n");
      goto cleanup;
    }

    if (!startup_object) {
      if (linker_mode == LINKER_MODE_INTERNAL || (!has_gcc && !has_link)) {
        fprintf(stderr,
                "Error: Failed to allocate internal-linker startup object path\n");
        free(object_paths);
        goto cleanup;
      }
      fprintf(stderr,
              "Warning: Failed to allocate internal-linker startup object path, "
              "falling back to external linkers\n");
    } else if (write_internal_startup_object(
                   startup_object, profile_runtime,
                   options && options->generate_stack_trace_support ? 1 : 0,
                   options && options->main_wants_argc_argv ? 1 : 0) != 0) {
      if (linker_mode == LINKER_MODE_INTERNAL || (!has_gcc && !has_link)) {
        fprintf(stderr,
                "Error: Failed to generate internal-linker startup object\n");
        free(startup_object);
        free(object_paths);
        goto cleanup;
      }
      fprintf(stderr,
              "Warning: Failed to generate internal-linker startup object, "
              "falling back to external linkers\n");
    } else {
      startup_ready = 1;
    }

    if (startup_ready) {
      if (needs_crash) {
        crash_object = (_access(crash_msvc_object, 0) == 0) ? crash_msvc_object
                                                            : crash_gcc_object;
      }
      if (needs_atomics) {
        atomics_object = (_access(atomics_msvc_object, 0) == 0)
                             ? atomics_msvc_object
                             : atomics_gcc_object;
      }
      if (needs_profile) {
        profile_object = (_access(profile_msvc_object, 0) == 0)
                             ? profile_msvc_object
                             : profile_gcc_object;
        if (_access(profile_object, 0) != 0) {
          fprintf(stderr,
                  "Error: Bundled profile runtime object not found in '%s'\n",
                  runtime_directory);
          free(object_paths);
          if (startup_object) {
            if (startup_ready) {
              _unlink(startup_object);
            }
            free(startup_object);
          }
          goto cleanup;
        }
      }
      if (needs_debug) {
        debug_object = (_access(debug_msvc_object, 0) == 0) ? debug_msvc_object
                                                            : debug_gcc_object;
        if (_access(debug_object, 0) != 0) {
          fprintf(stderr,
                  "Error: Bundled debug runtime object not found in '%s'\n",
                  runtime_directory);
          free(object_paths);
          if (startup_object) {
            if (startup_ready) {
              _unlink(startup_object);
            }
            free(startup_object);
          }
          goto cleanup;
        }
      }

      object_paths[object_count++] = startup_object;
      object_paths[object_count++] = object_filename;
      if (crash_object) {
        object_paths[object_count++] = crash_object;
      }
      if (atomics_object) {
        object_paths[object_count++] = atomics_object;
      }
      if (profile_object) {
        object_paths[object_count++] = profile_object;
      }
      if (debug_object) {
        object_paths[object_count++] = debug_object;
      }
      if (use_tracy) {
        object_paths[object_count++] = tracy_artifacts.helpers_object;
        object_paths[object_count++] = tracy_artifacts.client_object;
      } else if (needs_tracy_helpers && tracy_helpers_object) {
        object_paths[object_count++] = tracy_helpers_object;
      }
      if (!append_internal_link_object_args(options, object_paths,
                                            object_capacity, &object_count)) {
        fprintf(stderr, "Error: Too many internal-linker object arguments\n");
        free(object_paths);
        if (startup_object) {
          if (startup_ready) {
            _unlink(startup_object);
          }
          free(startup_object);
        }
        goto cleanup;
      }

      if (mettle_link_internal(object_paths, object_count, executable_filename, 0,
                                 options) == 0) {
        build_result = 0;
      } else if (linker_mode == LINKER_MODE_INTERNAL) {
        fprintf(stderr, "Error: Internal linker failed to produce an executable\n");
      } else if (!has_gcc && !has_link) {
        fprintf(stderr,
                "Error: Internal linker failed and no external fallback linker is "
                "available.\n");
      } else {
        fprintf(stderr,
                "Warning: Internal linker failed in auto mode, falling back to "
                "external linkers\n");
      }
    }

    if (startup_object) {
      if (startup_ready) {
        _unlink(startup_object);
      }
      free(startup_object);
    }
    free(object_paths);

    if (build_result == 0 || linker_mode == LINKER_MODE_INTERNAL ||
        (!has_gcc && !has_link)) {
      goto cleanup;
    }
  }

  if (has_gcc && linker_mode != LINKER_MODE_MSVC) {
    const char *runtime_objects[6] = {NULL, NULL, NULL, NULL, NULL, NULL};
    size_t runtime_object_count = 0u;
    if (needs_crash) {
      if (_access(crash_gcc_object, 0) != 0) {
        fprintf(stderr,
                "Error: Bundled crash-handler runtime object not found in '%s'\n",
                runtime_directory);
        goto cleanup;
      }
      runtime_objects[runtime_object_count++] = crash_gcc_object;
    }
    if (needs_atomics) {
      if (_access(atomics_gcc_object, 0) != 0) {
        fprintf(stderr,
                "Error: Bundled atomics runtime object not found in '%s'\n",
                runtime_directory);
        goto cleanup;
      }
      runtime_objects[runtime_object_count++] = atomics_gcc_object;
    }
    if (needs_profile) {
      if (_access(profile_gcc_object, 0) != 0) {
        fprintf(stderr,
                "Error: Bundled profile runtime object not found in '%s'\n",
                runtime_directory);
        goto cleanup;
      }
      runtime_objects[runtime_object_count++] = profile_gcc_object;
    }
    if (needs_debug) {
      if (_access(debug_gcc_object, 0) != 0) {
        fprintf(stderr,
                "Error: Bundled debug runtime object not found in '%s'\n",
                runtime_directory);
        goto cleanup;
      }
      runtime_objects[runtime_object_count++] = debug_gcc_object;
    }
    if (!use_tracy && needs_tracy_helpers && tracy_helpers_object) {
      runtime_objects[runtime_object_count++] = tracy_helpers_object;
    }
    if (mettle_link_object_with_gcc(object_filename, executable_filename,
                                      runtime_objects, runtime_object_count,
                                      options) == 0) {
      build_result = 0;
      goto cleanup;
    }
  }

  if (has_link && linker_mode != LINKER_MODE_GCC) {
    const char *runtime_objects[6] = {NULL, NULL, NULL, NULL, NULL, NULL};
    size_t runtime_object_count = 0u;
    if (needs_crash) {
      const char *crash_object = (_access(crash_msvc_object, 0) == 0)
                                     ? crash_msvc_object
                                     : crash_gcc_object;
      if (_access(crash_object, 0) != 0) {
        fprintf(stderr,
                "Error: Bundled crash-handler runtime object not found in '%s'\n",
                runtime_directory);
        goto cleanup;
      }
      runtime_objects[runtime_object_count++] = crash_object;
    }
    if (needs_atomics) {
      const char *atomics_object = (_access(atomics_msvc_object, 0) == 0)
                                       ? atomics_msvc_object
                                       : atomics_gcc_object;
      if (_access(atomics_object, 0) != 0) {
        fprintf(stderr,
                "Error: Bundled atomics runtime object not found in '%s'\n",
                runtime_directory);
        goto cleanup;
      }
      runtime_objects[runtime_object_count++] = atomics_object;
    }
    if (needs_profile) {
      const char *profile_object = (_access(profile_msvc_object, 0) == 0)
                                       ? profile_msvc_object
                                       : profile_gcc_object;
      if (_access(profile_object, 0) != 0) {
        fprintf(stderr,
                "Error: Bundled profile runtime object not found in '%s'\n",
                runtime_directory);
        goto cleanup;
      }
      runtime_objects[runtime_object_count++] = profile_object;
    }
    if (needs_debug) {
      const char *msvc_debug_object = (_access(debug_msvc_object, 0) == 0)
                                          ? debug_msvc_object
                                          : debug_gcc_object;
      if (_access(msvc_debug_object, 0) != 0) {
        fprintf(stderr,
                "Error: Bundled debug runtime object not found in '%s'\n",
                runtime_directory);
        goto cleanup;
      }
      runtime_objects[runtime_object_count++] = msvc_debug_object;
    }
    if (!use_tracy && needs_tracy_helpers && tracy_helpers_object) {
      const char *stub_object =
          (_access(tracy_helpers_msvc_object, 0) == 0) ? tracy_helpers_msvc_object
                                                       : tracy_helpers_gcc_object;
      runtime_objects[runtime_object_count++] = stub_object;
    }
    if (mettle_link_object_with_link(object_filename, executable_filename,
                                       runtime_objects, runtime_object_count,
                                       options) == 0) {
      build_result = 0;
      goto cleanup;
    }
  }

  fprintf(stderr,
          "Error: Failed to link executable with the available linker backends\n");

cleanup:
  tracy_free_artifacts(&tracy_artifacts);
  free(tracy_directory);
  free(tracy_error);
  free(tracy_helpers_gcc_object);
  free(tracy_helpers_msvc_object);
  free(crash_gcc_object);
  free(crash_msvc_object);
  free(atomics_gcc_object);
  free(atomics_msvc_object);
  free(profile_gcc_object);
  free(profile_msvc_object);
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

int main(int argc, char *argv[]) {
  CompilerOptions options = {0};
  mettle_compiler_crash_install(argc, argv);
  char *auto_stdlib_directory = NULL;
  char *auto_runtime_directory = NULL;
  char *build_output_filename = NULL;
  char *object_output_filename = NULL;
  int build_executable = 0;
  int linker_mode_explicit = 0;
  int output_filename_explicit = 0;
  options.emit_object = 1;
  options.output_filename = default_object_output_filename();
  options.debug_format = "dwarf";

  if (argc >= 2) {
    if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0 ||
        strcmp(argv[1], "version") == 0) {
      const char *target =
          binary_target_format_host_default() == BINARY_TARGET_FORMAT_ELF_X64
              ? "x86_64-linux (ELF)"
              : "x86_64-windows (COFF)";
      printf("mettle %s\n", METTLE_VERSION);
      printf("target: %s\n", target);
      return 0;
    }
    if (strcmp(argv[1], "help") == 0) {
      return print_help_topic(argv[0], argv[0], argc >= 3 ? argv[2] : NULL);
    }
    if (strcmp(argv[1], "explain") == 0) {
      return mettle_explain_error_code(argc >= 3 ? argv[2] : NULL);
    }
    if (strcmp(argv[1], "test") == 0) {
      /* `mettle test <file> [--filter=S] [flags...]`: shift the subcommand
       * out and let the normal flag loop see the rest. */
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
      /* `mettle trace <file> <fn> [args...]` */
      if (argc < 4) {
        fprintf(stderr,
                "usage: mettle trace <file.mettle> <function> [args...]\n"
                "  int/float parameters take the CLI values in order; pointer\n"
                "  parameters get a synthesized buffer\n");
        return 1;
      }
      options.trace_function = argv[3];
      options.trace_args = (const char *const *)&argv[4];
      options.trace_arg_count = (size_t)(argc - 4);
      argv[1] = argv[2]; /* the input file */
      argc = 2;
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

  // Parse command line arguments
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
      options.input_filename = argv[++i];
    } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
      options.output_filename = argv[++i];
      output_filename_explicit = 1;
    } else if (strcmp(argv[i], "-I") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "Error: Missing import directory after '-I'\n");
        return 1;
      }
      if (!add_import_directory(&options, argv[++i])) {
        fprintf(stderr, "Error: Failed to add import directory\n");
        return 1;
      }
    } else if (strncmp(argv[i], "-I", 2) == 0 && argv[i][2] != '\0') {
      if (!add_import_directory(&options, argv[i] + 2)) {
        fprintf(stderr, "Error: Failed to add import directory\n");
        return 1;
      }
    } else if (strcmp(argv[i], "--stdlib") == 0 && i + 1 < argc) {
      options.stdlib_directory = argv[++i];
    } else if (strcmp(argv[i], "--build") == 0) {
      build_executable = 1;
    } else if (strcmp(argv[i], "--emit-asm") == 0) {
      fprintf(stderr,
              "Error: --emit-asm has been removed; Mettle only emits native "
              "objects now.\n");
      return 1;
    } else if (strcmp(argv[i], "--emit-obj") == 0) {
      options.emit_object = 1;
    } else if (strcmp(argv[i], "--linker") == 0 && i + 1 < argc) {
      linker_mode_explicit = 1;
      if (!parse_linker_mode(argv[++i], &options.linker_mode)) {
        fprintf(stderr,
                "Error: Unknown linker mode '%s' (expected auto, internal, gcc, or msvc)\n",
                argv[i]);
        return 1;
      }
    } else if (strcmp(argv[i], "--linker") == 0) {
      fprintf(stderr, "Error: Missing linker mode after '--linker'\n");
      return 1;
    } else if (strcmp(argv[i], "--link-arg") == 0 && i + 1 < argc) {
      if (!add_link_argument(&options, argv[++i])) {
        fprintf(stderr, "Error: Failed to add linker argument\n");
        return 1;
      }
    } else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--debug") == 0) {
      options.debug_mode = 1;
      options.generate_debug_symbols = 1;
      options.generate_line_mapping = 1;
      options.generate_stack_trace_support = 1;
    } else if (strcmp(argv[i], "--dump-ir") == 0) {
      options.dump_ir = 1;
    } else if (strcmp(argv[i], "--ml-opt") == 0) {
      options.ml_opt = 1;
      options.optimize = 1;
    } else if (strcmp(argv[i], "--ml-opt-speculative") == 0) {
      /* Unlocks the model's unproven actions (dead-code DELETE). They exist
       * only on the validator's word, so this implies --ml-opt; ml_gnn reads
       * the env to emit the speculative dispositions. */
      options.ml_opt = 1;
      options.optimize = 1;
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
        return 1;
      }
    } else if (strncmp(argv[i], "--filter=", 9) == 0) {
      options.test_filter = argv[i] + 9;
    } else if (strcmp(argv[i], "--pgo") == 0) {
      options.pgo = 1;
      options.optimize = 1;
    } else if (strcmp(argv[i], "--verify") == 0) {
      ir_verify_set_enabled(1);
      options.optimize = 1;
    } else if (strcmp(argv[i], "--simd-report") == 0) {
      options.simd_report = 1;
    } else if (strcmp(argv[i], "--explain") == 0) {
      options.explain = 1;
    } else if (strcmp(argv[i], "--explain-all") == 0) {
      /* Whole-program report: no focus-file filter, so imported modules'
       * loops and calls are analyzed too (stdlib included). */
      options.explain = 1;
      options.explain_all = 1;
    } else if (strcmp(argv[i], "--explain-json") == 0) {
      /* Machine-readable sidecar (<output-stem>.explain.json) alongside the
       * prose report; implies --explain. */
      options.explain = 1;
      options.explain_json = 1;
    } else if (strcmp(argv[i], "--annotate-asm") == 0) {
      /* Codegen provenance listing + <stem>.annot.json sidecar. Needs the
       * optimizer's decisions (and remarks) to be interesting, so it implies
       * -O and collects --explain remarks (retained past optimization for the
       * codegen join). The default syntax is both Intel and AT&T (toggle). */
      options.annotate_asm = 1;
      /* Reflect the codegen users actually ship: --release enables every
       * vectorizer/idiom, so the annotation matches release output (otherwise a
       * loop shown "not vectorized" at -O would mislead). */
      options.optimize = 1;
      options.release = 1;
      options.explain = 1;
      options.asm_syntax = 2; /* both */
    } else if (strncmp(argv[i], "--annotate-lines=", 17) == 0) {
      /* Focused codegen report for a source line range (LLM-facing): asm + cost
       * + covering loops + live registers + decisions for just those lines.
       * Accepts "A" (single line) or "A-B". Implies --annotate-asm. */
      const char *v = argv[i] + 17;
      int a = 0, b = 0;
      if (sscanf(v, "%d-%d", &a, &b) == 2) {
        /* range */
      } else if (sscanf(v, "%d", &a) == 1) {
        b = a;
      } else {
        fprintf(stderr, "Error: --annotate-lines expects A or A-B (got '%s')\n", v);
        return 1;
      }
      if (a <= 0 || b < a) {
        fprintf(stderr, "Error: --annotate-lines range invalid: %s\n", v);
        return 1;
      }
      options.annotate_q_lo = a;
      options.annotate_q_hi = b;
      options.annotate_asm = 1;
      options.optimize = 1;
      options.release = 1;
      options.explain = 1;
      if (!options.asm_syntax) options.asm_syntax = 0; /* intel-only is terser */
    } else if (strncmp(argv[i], "--annotate-fn=", 14) == 0) {
      options.annotate_q_fn = argv[i] + 14;
      options.annotate_asm = 1;
      options.optimize = 1;
      options.release = 1;
      options.explain = 1;
    } else if (strcmp(argv[i], "--annotate-hot") == 0 ||
               strncmp(argv[i], "--annotate-hot=", 15) == 0) {
      /* Top-N hotspots across the program (LLM-facing "where is the time"). */
      int n = 8;
      if (argv[i][14] == '=') n = atoi(argv[i] + 15);
      if (n <= 0) n = 8;
      options.annotate_hot = n;
      options.annotate_asm = 1;
      options.optimize = 1;
      options.release = 1;
      options.explain = 1;
    } else if (strncmp(argv[i], "--asm-syntax=", 13) == 0) {
      const char *v = argv[i] + 13;
      if (strcmp(v, "intel") == 0) {
        options.asm_syntax = 0;
      } else if (strcmp(v, "att") == 0) {
        options.asm_syntax = 1;
      } else if (strcmp(v, "both") == 0) {
        options.asm_syntax = 2;
      } else {
        fprintf(stderr,
                "Error: --asm-syntax must be intel, att, or both (got '%s')\n",
                v);
        return 1;
      }
    } else if (strcmp(argv[i], "--emit-ptx") == 0) {
      options.emit_ptx = 1;
    } else if (strcmp(argv[i], "--emit-arm64") == 0) {
      options.emit_arm64 = 1;
    } else if (strcmp(argv[i], "-g") == 0 ||
               strcmp(argv[i], "--debug-symbols") == 0) {
      options.generate_debug_symbols = 1;
    } else if (strcmp(argv[i], "-l") == 0 ||
               strcmp(argv[i], "--line-mapping") == 0) {
      options.generate_line_mapping = 1;
    } else if (strcmp(argv[i], "-s") == 0 ||
               strcmp(argv[i], "--stack-trace") == 0) {
      options.generate_stack_trace_support = 1;
    } else if (strcmp(argv[i], "--debug-format") == 0 && i + 1 < argc) {
      options.debug_format = argv[++i];
    } else if (strcmp(argv[i], "-O") == 0 ||
               strcmp(argv[i], "--optimize") == 0) {
      options.optimize = 1;
    } else if (strcmp(argv[i], "-r") == 0 ||
               strcmp(argv[i], "--release") == 0) {
      options.release = 1;
      options.optimize = 1;
    } else if (strcmp(argv[i], "--strip-comments") == 0) {
      fprintf(stderr,
              "Error: --strip-comments has been removed; Mettle no longer "
              "emits text assembly.\n");
      return 1;
    } else if (strcmp(argv[i], "--prelude") == 0) {
      options.prelude = 1;
    } else if (strcmp(argv[i], "--profile") == 0) {
      options.profile = 1;
    } else if (strcmp(argv[i], "--profile-runtime") == 0) {
      options.profile_runtime = 1;
    } else if (strcmp(argv[i], "--profile-runtime-ops") == 0) {
      options.profile_runtime_ops = 1;
    } else if (strcmp(argv[i], "--profile-blocks") == 0) {
      options.profile_blocks = 1;
      options.profile_runtime = 1;
    } else if (strcmp(argv[i], "--debug-hooks") == 0) {
      options.debug_hooks = 1;
    } else if (strcmp(argv[i], "--native-heap") == 0) {
      options.native_heap = 1;
    } else if (strcmp(argv[i], "--static") == 0) {
      options.static_link = 1;
    } else if (strcmp(argv[i], "--musl") == 0) {
      options.musl_link = 1;
      options.static_link = 1;
    } else if (strcmp(argv[i], "--tracy") == 0) {
      options.tracy = 1;
    } else if (strcmp(argv[i], "--tracy-dir") == 0 && i + 1 < argc) {
      options.tracy_directory = argv[++i];
    } else if (strcmp(argv[i], "--tracy-dir") == 0) {
      fprintf(stderr, "Error: Missing path after '--tracy-dir'\n");
      return 1;
    } else if (strcmp(argv[i], "--debug-compiler") == 0) {
      options.debug_compiler = 1;
    } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
      print_usage(argv[0]);
      return 0;
    } else if (!options.input_filename) {
      options.input_filename = argv[i];
    } else {
      fprintf(stderr, "Error: Unknown or misplaced argument '%s'\n", argv[i]);
      print_usage(argv[0]);
      return 1;
    }
  }

  if (!options.input_filename) {
    fprintf(stderr, "Error: No input file specified.\n");
    print_usage(argv[0]);
    free((void *)options.import_directories);
    free((void *)options.link_arguments);
    return 1;
  }

  if (options.tracy && !build_executable) {
    fprintf(stderr, "Error: --tracy requires --build\n");
    free((void *)options.import_directories);
    free((void *)options.link_arguments);
    return 1;
  }

  if (build_executable) {
    options.emit_object = 1;
    if (!linker_mode_explicit) {
      options.linker_mode = LINKER_MODE_INTERNAL;
    }
  }

  if (!options.stdlib_directory) {
    auto_stdlib_directory = infer_default_stdlib_directory(argv[0]);
    if (auto_stdlib_directory) {
      options.stdlib_directory = auto_stdlib_directory;
    }
  }

  auto_runtime_directory = infer_default_runtime_directory(argv[0]);

  if (options.ml_opt) {
    ml_opt_set_default_paths(argv[0]);
  }

  /* The native ELF backend supports --build on Linux via an ld-based link of
   * the emitted ELF object plus a self-contained _start. On Linux --build
   * always uses the direct-object backend (no asm/NASM path). */
  int elf_build = (binary_target_format_host_default() ==
                   BINARY_TARGET_FORMAT_ELF_X64);

  if (build_executable) {
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
    /* Linux: force direct-object emission; there is no NASM/asm link path. */
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
    if (output_filename_explicit) {
      build_output_filename = strdup(options.output_filename);
    } else {
      build_output_filename = default_executable_filename(options.input_filename);
    }
    if (!build_output_filename) {
      fprintf(stderr, "Error: Failed to determine executable output path\n");
      free((void *)options.import_directories);
      free((void *)options.link_arguments);
      free(auto_stdlib_directory);
      free(auto_runtime_directory);
      return 1;
    }

    /* ELF objects conventionally use .o; COFF uses .obj. */
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

  options.building_executable = build_executable;

  double command_profile_start =
      options.profile ? compiler_profile_now_ms() : 0.0;
  int result =
      compile_file(options.input_filename, options.output_filename, &options);
  if (result == 0 && build_executable) {
    double build_profile_start =
        options.profile ? compiler_profile_now_ms() : 0.0;
#ifndef _WIN32
    /* Linux: emit the ELF object (done by compile_file above) then link it
     * with our self-contained _start via ld. */
    result = mettle_link_elf_executable(options.output_filename,
                                        build_output_filename, &options,
                                        auto_runtime_directory);
#else
    result = mettle_link_object_file(options.output_filename,
                                     build_output_filename,
                                     auto_runtime_directory, &options);
#endif
    if (result == 0) {
      printf("Built executable '%s'\n", build_output_filename);
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
  free(auto_stdlib_directory);
  free(auto_runtime_directory);
  free(build_output_filename);
  free(object_output_filename);
  if (getenv("METTLE_FULL_CLEANUP")) {
    string_intern_clear();
  }
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

static int compile_lower_to_ir(ASTNode *program, TypeChecker *type_checker,
                               SymbolTable *symbol_table,
                               int emit_runtime_checks,
                               IRProgram **out_ir_program,
                               char **out_ir_error) {
  *out_ir_program = ir_lower_program(program, type_checker, symbol_table,
                                     out_ir_error, emit_runtime_checks);
  if (!*out_ir_program) {
    mettle_compiler_ice_report("IR lowering failed",
                               *out_ir_error ? *out_ir_error : NULL);
    return 0;
  }
  return 1;
}

#include "ir/ml_opt.h"

/* Collect non-extern, non-exported global integer `var`s whose initializer is
 * an integer literal (optionally negated). The optimizer proves each is never
 * written before folding its reads - this only supplies the candidates. */
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

static int compile_optimize_ir(IRProgram *ir_program, ASTNode *ast_program,
                               CompilerOptions *options) {
  IROptimizeOptions ir_optimize_options = {0};
  ir_optimize_options.preserve_function_boundaries =
      options->profile_runtime ? 1 : 0;
  ir_optimize_options.simd_report = options->simd_report;
  ir_optimize_options.explain = options->explain;
  ir_optimize_options.explain_focus_file =
      options->explain_all ? NULL : options->input_filename;
  /* Large --explain reports divert to `<output-stem>.explain.txt`. */
  ir_explain_set_output_path(options->output_filename);
  ir_explain_set_json(options->explain_json ? 1 : 0);
  /* --annotate-asm: arm the codegen annotator before codegen runs, and keep the
   * optimization remarks alive so it can join them onto the emitted asm. */
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
      collect_global_int_consts(ast_program, &global_const_count);
  ir_optimize_options.global_int_consts = global_consts;
  ir_optimize_options.global_int_const_count = global_const_count;
  ir_optimize_options.whole_program = options->building_executable;
  int opt_ok = ir_optimize_program(ir_program, &ir_optimize_options);
  free(global_consts);
  if (!opt_ok) {
    /* A violated `@simd!` contract is a user error already printed with a
     * source location; don't bury it under a generic internal-error report. */
    if (!ir_optimize_had_user_error()) {
      mettle_compiler_ice_report("IR optimization failed", NULL);
    }
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
      /* ml_gnn wrote _mlopt.explain (TSV). Render it styled like the main report. */
      ir_explain_ml_opt("_mlopt.explain");
    }
  }
  return 1;
}

static int compile_generate_code(CodeGenerator *code_generator,
                                 ASTNode *program) {
  if (!code_generator_generate_program(code_generator, program)) {
    fprintf(stderr, "Code generation error: %s\n",
            (code_generator && code_generator->error_message)
                ? code_generator->error_message
                : "Unknown error");
    mettle_compiler_ice_report("Code generation failed",
                               code_generator && code_generator->error_message
                                   ? code_generator->error_message
                                   : NULL);
    return 0;
  }
  return 1;
}

int compile_file(const char *input_filename, const char *output_filename,
                 CompilerOptions *options) {
  CompilerProfile profile;
  double phase_start = 0.0;

  compiler_profile_init(&profile, options && options->profile);

  mettle_compiler_ctx_reset();
  mettle_compiler_ctx_set_input_filename(input_filename);
  mettle_compiler_ctx_set_current_filename(input_filename);
  if (options) {
    mettle_compiler_ctx_set_options(options->debug_compiler, options->dump_ir);
  }

  compiler_set_phase(PROFILE_PHASE_READ_INPUT);
  phase_start = compiler_profile_begin(&profile);
  char *source = NULL;
  int read_ok = compile_read_source(input_filename, &source);
  compiler_profile_add(&profile, PROFILE_PHASE_READ_INPUT, phase_start);
  if (!read_ok) {
    compiler_profile_print_compile(&profile, input_filename, 1);
    return 1;
  }

  compiler_set_phase(PROFILE_PHASE_INIT);
  phase_start = compiler_profile_begin(&profile);
  ErrorReporter *error_reporter = error_reporter_create(input_filename, source);
  compiler_profile_add(&profile, PROFILE_PHASE_INIT, phase_start);
  if (!error_reporter) {
    fprintf(stderr, "Error: Could not initialize error reporter\n");
    free(source);
    compiler_profile_print_compile(&profile, input_filename, 1);
    return 1;
  }

  /* Lexical errors are reported inline by the parser (parser_advance calls
   * parser_report_lexer_token_error on any TOKEN_ERROR, into this same
   * error_reporter, and the post-parse check below aborts before codegen).
   * A separate pre-pass that re-tokenized the whole source just to find those
   * same errors was pure duplicate work -- a full extra lexer pass over the
   * input -- so it has been removed. The phase slot is kept (recorded as 0 ms)
   * to preserve the --profile output layout. */
  compiler_set_phase(PROFILE_PHASE_LEXICAL_VALIDATION);
  phase_start = compiler_profile_begin(&profile);
  compiler_profile_add(&profile, PROFILE_PHASE_LEXICAL_VALIDATION, phase_start);

  // Initialize compiler components
  compiler_set_phase(PROFILE_PHASE_INIT);
  phase_start = compiler_profile_begin(&profile);
  Lexer *lexer = lexer_create(source);
  Parser *parser = NULL;
  SymbolTable *symbol_table = symbol_table_create();
  TypeChecker *type_checker = NULL;
  RegisterAllocator *register_allocator = register_allocator_create();
  ASTNode *program = NULL;

  // Initialize debug info if debug mode is enabled
  DebugInfo *debug_info = NULL;
  CodeGenerator *code_generator = NULL;
  IRProgram *ir_program = NULL;
  char *ir_error_message = NULL;

  if (!lexer || !symbol_table || !register_allocator) {
    compiler_profile_add(&profile, PROFILE_PHASE_INIT, phase_start);
    error_reporter_add_error(error_reporter, ERROR_INTERNAL,
                             source_location_create(0, 0),
                             "Failed to initialize compiler components");
    error_reporter_print_errors(error_reporter);
    if (lexer)
      lexer_destroy(lexer);
    if (symbol_table)
      symbol_table_destroy(symbol_table);
    if (register_allocator)
      register_allocator_destroy(register_allocator);
    error_reporter_destroy(error_reporter);
    free(source);
    compiler_profile_print_compile(&profile, input_filename, 1);
    return 1;
  }

  parser = parser_create_with_error_reporter(lexer, error_reporter);
  if (parser) {
    /* Enable kernel index built-ins (thread.x etc.) for GPU compiles. */
    parser->gpu_mode = options->emit_ptx;
  }
  type_checker =
      type_checker_create_with_error_reporter(symbol_table, error_reporter);
  if (!parser || !type_checker) {
    compiler_profile_add(&profile, PROFILE_PHASE_INIT, phase_start);
    error_reporter_add_error(error_reporter, ERROR_INTERNAL,
                             source_location_create(0, 0),
                             "Failed to initialize parser or type checker");
    error_reporter_print_errors(error_reporter);
    if (parser)
      parser_destroy(parser);
    if (type_checker)
      type_checker_destroy(type_checker);
    register_allocator_destroy(register_allocator);
    symbol_table_destroy(symbol_table);
    lexer_destroy(lexer);
    error_reporter_destroy(error_reporter);
    free(source);
    compiler_profile_print_compile(&profile, input_filename, 1);
    return 1;
  }

  if (options->debug_mode || options->generate_debug_symbols ||
      options->generate_line_mapping || options->generate_stack_trace_support) {
    debug_info = debug_info_create(input_filename, output_filename);
    if (!debug_info) {
      compiler_profile_add(&profile, PROFILE_PHASE_INIT, phase_start);
      error_reporter_add_error(error_reporter, ERROR_INTERNAL,
                               source_location_create(0, 0),
                               "Failed to initialize debug information");
      error_reporter_print_errors(error_reporter);
      parser_destroy(parser);
      type_checker_destroy(type_checker);
      register_allocator_destroy(register_allocator);
      symbol_table_destroy(symbol_table);
      lexer_destroy(lexer);
      error_reporter_destroy(error_reporter);
      free(source);
      compiler_profile_print_compile(&profile, input_filename, 1);
      return 1;
    }
    code_generator = code_generator_create_with_debug(
        symbol_table, type_checker, register_allocator, debug_info);
  } else {
    code_generator =
        code_generator_create(symbol_table, type_checker, register_allocator);
  }

  if (!code_generator) {
    compiler_profile_add(&profile, PROFILE_PHASE_INIT, phase_start);
    error_reporter_add_error(error_reporter, ERROR_INTERNAL,
                             source_location_create(0, 0),
                             "Failed to initialize code generator");
    error_reporter_print_errors(error_reporter);
    parser_destroy(parser);
    type_checker_destroy(type_checker);
    register_allocator_destroy(register_allocator);
    symbol_table_destroy(symbol_table);
    lexer_destroy(lexer);
    if (debug_info)
      debug_info_destroy(debug_info);
    error_reporter_destroy(error_reporter);
    free(source);
    compiler_profile_print_compile(&profile, input_filename, 1);
    return 1;
  }

  if (debug_info) {
    code_generator_set_debug_sidecar_emission(
        code_generator,
        (options->debug_mode || options->generate_debug_symbols ||
         options->generate_line_mapping)
            ? 1
            : 0);
  }
  code_generator_set_stack_trace_support(
      code_generator, options->generate_stack_trace_support ? 1 : 0);
  code_generator_set_eliminate_unreachable_functions(
      code_generator, options->release ? 1 : 0);
  code_generator_set_profile_runtime(code_generator,
                                     compiler_options_use_profile_runtime(options)
                                         ? 1
                                         : 0);
  code_generator_set_debug_hooks(code_generator, options->debug_hooks ? 1 : 0);
  compiler_profile_add(&profile, PROFILE_PHASE_INIT, phase_start);

  int result = 0;
  options->emit_object = 1;

  compiler_set_phase(PROFILE_PHASE_PARSE);
  phase_start = compiler_profile_begin(&profile);
  int parse_ok = compile_lex_and_parse(parser, error_reporter, &program);
  compiler_profile_add(&profile, PROFILE_PHASE_PARSE, phase_start);
  if (!parse_ok) {
    result = 1;
    goto cleanup;
  }

  // Resolve imports (flatten imported module ASTs into the main program)
  ImportResolverOptions import_options = {0};
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
  /* Prefer `<name>.linux.mettle` std variants when targeting native ELF so the
   * stdlib resolves syscall-based modules on Linux. Mirrors the elf_build check
   * used elsewhere in main(). */
  import_options.target_is_elf =
      (binary_target_format_host_default() == BINARY_TARGET_FORMAT_ELF_X64) ? 1
                                                                            : 0;

  // Auto-inject the standard prelude only when --prelude was specified, and
  // std/alloc when --native-heap was specified (it provides the mettle_heap_*
  // shims the backend rewrites new/malloc/calloc/realloc/free to call).
  compiler_set_phase(PROFILE_PHASE_PRELUDE);
  phase_start = compiler_profile_begin(&profile);
  {
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
        // Prepend the import before all user declarations.
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
  compiler_profile_add(&profile, PROFILE_PHASE_PRELUDE, phase_start);

  compiler_set_phase(PROFILE_PHASE_IMPORTS);
  phase_start = compiler_profile_begin(&profile);
  int imports_ok = compile_resolve_imports(program, input_filename,
                                           error_reporter, &import_options);
  compiler_profile_add(&profile, PROFILE_PHASE_IMPORTS, phase_start);
  if (!imports_ok) {
    result = 1;
    goto cleanup;
  }

  options->main_wants_argc_argv = program_main_wants_argc_argv(program);

  compiler_set_phase(PROFILE_PHASE_MONOMORPHIZE);
  phase_start = compiler_profile_begin(&profile);
  int mono_ok = compile_monomorphize(program, error_reporter);
  compiler_profile_add(&profile, PROFILE_PHASE_MONOMORPHIZE, phase_start);
  if (!mono_ok) {
    result = 1;
    goto cleanup;
  }

  /* --explain: collect the memory analyzer's diagnostics so the optimization
   * report can surface them in a "memory" section. Enabled before type-check
   * (where they fire) and only when the optimizer will run -- the only path
   * that produces a report. */
  ir_explain_memory_set_collect(options->explain && options->optimize,
                                options->explain_all ? NULL
                                                     : options->input_filename);

  compiler_set_phase(PROFILE_PHASE_TYPE_CHECK);
  phase_start = compiler_profile_begin(&profile);
  int tc_ok = compile_type_check(type_checker, program, error_reporter);
  compiler_profile_add(&profile, PROFILE_PHASE_TYPE_CHECK, phase_start);
  if (!tc_ok) {
    result = 1;
    goto cleanup;
  }

  /* @test functions are type-checked in every build (so they can't rot) but
   * compiled only under `mettle test`: drop them before lowering. The node
   * lives in BOTH program->children and Program->declarations; remove it
   * from both before destroying it. */
  if (!options->test_mode) {
    Program *prog_data = (Program *)program->data;
    if (prog_data) {
      size_t kept = 0;
      for (size_t i = 0; i < prog_data->declaration_count; i++) {
        ASTNode *decl = prog_data->declarations[i];
        FunctionDeclaration *fd =
            decl && decl->type == AST_FUNCTION_DECLARATION && decl->data
                ? (FunctionDeclaration *)decl->data
                : NULL;
        if (fd && fd->is_test) {
          size_t child_kept = 0;
          for (size_t c = 0; c < program->child_count; c++) {
            if (program->children[c] == decl) {
              continue;
            }
            program->children[child_kept++] = program->children[c];
          }
          program->child_count = child_kept;
          ast_destroy_node(decl);
          continue;
        }
        prog_data->declarations[kept++] = decl;
      }
      prog_data->declaration_count = kept;
    }
  }

  /* --explain reports optimizer decisions, so it only means something when the
   * optimizer runs; lowering then brackets every loop with report-only markers
   * for the verifier to report on. */
  if (options->explain && !options->optimize) {
    fprintf(stderr, "note: --explain has no effect without -O/--release (it "
                    "reports optimization decisions)\n");
  }
  ir_lowering_set_explain(options->explain && options->optimize &&
                          !options->emit_ptx);

  int emit_runtime_checks =
      (options->release || options->emit_ptx || options->emit_arm64) ? 0 : 1;
  compiler_set_phase(PROFILE_PHASE_IR_LOWERING);
  phase_start = compiler_profile_begin(&profile);
  int ir_ok = compile_lower_to_ir(program, type_checker, symbol_table,
                                   emit_runtime_checks, &ir_program,
                                   &ir_error_message);
  compiler_profile_add(&profile, PROFILE_PHASE_IR_LOWERING, phase_start);
  if (!ir_ok) {
    result = 1;
    goto cleanup;
  }

  mettle_compiler_ctx_set_ir_program(ir_program);

  /* --pgo: interpret main() now, before optimization, so the optimizer can
   * consume measured call frequencies instead of static guesses. */
  if (options->pgo) {
    ir_pgo_profile_program(ir_program);
    ir_pgo_print_summary();
  }

  /* `mettle test` / `mettle trace`: execute in the compile-time interpreter
   * and stop - no optimization (unless requested), no codegen, no linking. */
  if (options->test_mode || options->trace_function) {
    if (options->optimize) {
      int opt_ok = compile_optimize_ir(ir_program, program, options);
      if (!opt_ok) {
        result = 1;
        goto cleanup;
      }
    }
    if (options->test_mode) {
      result = ir_comptime_run_tests(ir_program, error_reporter,
                                     input_filename, options->test_filter);
    } else {
      result = ir_comptime_trace(ir_program, error_reporter, input_filename,
                                 source, options->trace_function,
                                 options->trace_args,
                                 options->trace_arg_count);
    }
    goto cleanup;
  }

  /* --emit-ptx: lower every function to a PTX `.visible .entry` and write the
   * PTX text to the output file. No optimization (keeps the IR shape the PTX
   * emitter expects), no object, no link -- the GPU kernels are JIT-compiled by
   * the CUDA driver at runtime from this text. */
  if (options->emit_ptx) {
    FILE *ptx_out = fopen(output_filename, "w");
    if (!ptx_out) {
      fprintf(stderr, "Error: could not open PTX output '%s'\n",
              output_filename);
      result = 1;
      goto cleanup;
    }
    char *ptx_err = NULL;
    int ok = ptx_emit_program(ir_program, code_generator, ptx_out, &ptx_err);
    fclose(ptx_out);
    if (!ok) {
      fprintf(stderr, "Error: PTX emission failed: %s\n",
              ptx_err ? ptx_err : "unknown");
      free(ptx_err);
      result = 1;
      goto cleanup;
    }
    printf("Generated PTX: %s\n", output_filename);
    result = 0;
    goto cleanup;
  }

  /* --emit-arm64: lower the scalar-integer subset of every function directly to
   * an AArch64 ELF executable (from-scratch backend, no external assembler). A
   * `_start` calls main() and exits with its return value. No optimization (the
   * direct IR backend consumes the unoptimized IR shape), no x86 object. */
  if (options->emit_arm64) {
    Arm64Emit ae;
    arm64_emit_init(&ae);
    int ok = arm64_ir_encode_program(&ae, ir_program, "main") &&
             arm64_emit_finalize(&ae);
    if (ok) {
      ok = arm64_write_elf(output_filename, ae.code.data, ae.code.len);
      if (!ok) {
        fprintf(stderr, "Error: could not write AArch64 ELF '%s'\n",
                output_filename);
      }
    } else {
      fprintf(stderr, "Error: AArch64 lowering failed (an op outside the "
                      "supported scalar subset, or an unresolved call)\n");
    }
    arm64_emit_free(&ae);
    if (!ok) {
      result = 1;
      goto cleanup;
    }
    printf("Generated AArch64 ELF: %s\n", output_filename);
    result = 0;
    goto cleanup;
  }

  /* --native-heap: retarget new/malloc/calloc/realloc/free onto the std/alloc
   * Mettle allocator at the IR level (before optimization, so the rewritten
   * calls inline/optimize like any other). std/alloc is auto-injected above. */
  if (options->native_heap && !ir_program_route_to_native_heap(ir_program)) {
    fprintf(stderr, "Error: Failed to route allocation to the native heap\n");
    result = 1;
    goto cleanup;
  }

  if (compiler_options_use_profile_runtime(options)) {
    if (!ir_profile_instrument_program(ir_program)) {
      fprintf(stderr, "Error: Failed to instrument IR for runtime profiling\n");
      result = 1;
      goto cleanup;
    }
  }

  /* --debug-hooks: interactive debugger instrumentation (enter/exit/line
   * hooks + live-pointer variable registrations). Mutually exclusive with
   * the profiler (both own the fn-id registry) and intended for -O0: the
   * optimizer would move or delete the hooks. */
  if (options->debug_hooks) {
    if (compiler_options_use_profile_runtime(options)) {
      fprintf(stderr,
              "Error: --debug-hooks and --profile-runtime are mutually "
              "exclusive\n");
      result = 1;
      goto cleanup;
    }
    if (options->optimize) {
      fprintf(stderr,
              "Error: --debug-hooks requires an unoptimized build (drop "
              "--release/-O; optimized code moves and deletes the hooks)\n");
      result = 1;
      goto cleanup;
    }
    if (!ir_debug_hooks_instrument_program(ir_program)) {
      fprintf(stderr, "Error: Failed to instrument IR for debugging\n");
      result = 1;
      goto cleanup;
    }
  }

  if (options->optimize) {
    compiler_set_phase(PROFILE_PHASE_IR_OPTIMIZATION);
    phase_start = compiler_profile_begin(&profile);
    int opt_ok = compile_optimize_ir(ir_program, program, options);
    compiler_profile_add(&profile, PROFILE_PHASE_IR_OPTIMIZATION, phase_start);
    if (!opt_ok) {
      result = 1;
      goto cleanup;
    }
  } else {
    /* Vectorization (and thus `@simd` contract verification) only runs under
     * -O/--release. Tell the user their `@simd` loops went unchecked and strip
     * the markers so they never reach codegen. */
    ir_note_simd_contracts_unverified(ir_program);
  }

  /* Executable builds: drop functions unreachable from main. Importing a
   * stdlib module emits the whole module, so without this every binary
   * carries the unused siblings of each function it actually calls. Runs
   * after the optimizer so functions the inliner fully absorbed are swept
   * too. Skipped for profile/tracy builds, whose instrumentation tables
   * enumerate every function. */
  if (options->building_executable && !options->tracy &&
      !compiler_options_use_profile_runtime(options) && !options->debug_hooks &&
      !ir_program_eliminate_dead_functions(ir_program)) {
    fprintf(stderr, "Error: Failed to eliminate dead functions\n");
    result = 1;
    goto cleanup;
  }

  if (options->profile_runtime_ops) {
    if (!ir_profile_instrument_operation_counters(ir_program)) {
      fprintf(stderr,
              "Error: Failed to instrument IR operation counters for runtime profiling\n");
      result = 1;
      goto cleanup;
    }
  }

  if (options->profile_blocks) {
    if (!ir_profile_instrument_blocks(ir_program)) {
      fprintf(stderr,
              "Error: Failed to instrument IR basic-block counters for the "
              "codegen profile view\n");
      result = 1;
      goto cleanup;
    }
  }

  code_generator_set_ir_program(code_generator, ir_program);

  if (options->debug_mode || options->dump_ir) {
    compiler_set_phase(PROFILE_PHASE_IR_DUMP);
    phase_start = compiler_profile_begin(&profile);
    char *ir_output = build_sidecar_filename(output_filename, ".ir");
    if (!ir_output) {
      fprintf(stderr,
              "Warning: Failed to allocate IR output filename for '%s'\n",
              output_filename);
    } else {
      FILE *ir_file = fopen(ir_output, "w");
      if (!ir_file) {
        fprintf(stderr, "Warning: Could not create IR file '%s': %s\n",
                ir_output, strerror(errno));
      } else {
        if (!ir_program_dump(ir_program, ir_file)) {
          fprintf(stderr, "Warning: Failed to write IR dump to '%s'\n",
                  ir_output);
        }
        fclose(ir_file);
        if (options->debug_mode) {
          printf("Generated IR dump: %s\n", ir_output);
        }
      }
      free(ir_output);
    }
    compiler_profile_add(&profile, PROFILE_PHASE_IR_DUMP, phase_start);
  }

  compiler_set_phase(PROFILE_PHASE_CODEGEN);
  phase_start = compiler_profile_begin(&profile);
  int codegen_ok = compile_generate_code(code_generator, program);
  compiler_profile_add(&profile, PROFILE_PHASE_CODEGEN, phase_start);
  if (!codegen_ok) {
    result = 1;
    goto cleanup;
  }

  /* --explain: the MIR eligibility gate recorded, per function, whether it got
   * the register-allocating backend; print that section now that codegen ran.
   * (No-op unless --explain is on.) */
  if (options->explain && options->optimize) {
    ir_explain_backend_flush();
  }

  /* --annotate-asm: now that every function has been encoded, write the
   * annotated listing (stdout) and the <stem>.annot.json sidecar. */
  if (options->annotate_asm) {
    mir_annotate_flush();
  }

  compiler_set_phase(PROFILE_PHASE_WRITE_OUTPUT);
  phase_start = compiler_profile_begin(&profile);
  BinaryEmitter *binary_emitter =
      code_generator_get_binary_emitter(code_generator);
  if (!binary_emitter_write_object_file(binary_emitter, output_filename)) {
    compiler_profile_add(&profile, PROFILE_PHASE_WRITE_OUTPUT, phase_start);
    fprintf(stderr, "Error: Could not create object file '%s': %s\n",
            output_filename,
            binary_emitter_get_error(binary_emitter)
                ? binary_emitter_get_error(binary_emitter)
                : "Unknown error");
    result = 1;
    goto cleanup;
  }
  compiler_profile_add(&profile, PROFILE_PHASE_WRITE_OUTPUT, phase_start);

  // Generate debug information files if requested
  compiler_set_phase(PROFILE_PHASE_DEBUG_INFO);
  phase_start = compiler_profile_begin(&profile);
  if (debug_info) {
    if (options->debug_mode || options->generate_debug_symbols ||
        options->generate_line_mapping) {
      const char *format =
          (options->debug_format && options->debug_format[0] != '\0')
              ? options->debug_format
              : "dwarf";
      const char *suffix = ".dwarf";

      if (strcasecmp(format, "stabs") == 0) {
        suffix = ".stabs";
      } else if (strcasecmp(format, "map") == 0) {
        suffix = ".map";
      } else if (strcasecmp(format, "dwarf") != 0) {
        fprintf(stderr,
                "Warning: Unknown debug format '%s', defaulting to dwarf\n",
                format);
      }

      char *debug_output = build_sidecar_filename(output_filename, suffix);
      if (!debug_output) {
        compiler_profile_add(&profile, PROFILE_PHASE_DEBUG_INFO, phase_start);
        fprintf(stderr,
                "Error: Failed to allocate debug output filename for '%s'\n",
                output_filename);
        result = 1;
        goto cleanup;
      }

      if (strcasecmp(format, "stabs") == 0) {
        debug_info_generate_stabs(debug_info, debug_output);
      } else if (strcasecmp(format, "map") == 0) {
        debug_info_generate_debug_map(debug_info, debug_output);
      } else {
        debug_info_generate_dwarf(debug_info, debug_output);
      }

      if (options->debug_mode) {
        printf("Generated debug info: %s\n", debug_output);
      }
      free(debug_output);
    }

    if (options->generate_stack_trace_support && options->debug_mode) {
      printf("Embedded runtime stack trace support enabled\n");
    }
  }
  compiler_profile_add(&profile, PROFILE_PHASE_DEBUG_INFO, phase_start);

  if (options->debug_mode) {
    if (error_reporter->count > 0) {
      error_reporter_print_errors(error_reporter);
    }
    printf("Successfully compiled '%s' to '%s'\n", input_filename,
           output_filename);
  } else if (error_reporter->count > 0) {
    // Surface non-fatal diagnostics (e.g. circular/duplicate import warnings)
    // even on successful compilation.
    error_reporter_print_errors(error_reporter);
  }

cleanup:
  // Clean up resources
  compiler_set_phase(PROFILE_PHASE_CLEANUP);
  phase_start = compiler_profile_begin(&profile);
  /* compile_file runs once per process and the caller exits right after
   * linking, so the recursive AST/IR/type/symbol teardown (hundreds of ms on
   * large inputs) buys nothing: leave those to process exit by default.
   * METTLE_FULL_CLEANUP=1 restores the deep teardown for leak-hunting under
   * sanitizers or heap tooling. */
  if (getenv("METTLE_FULL_CLEANUP")) {
    if (program)
      ast_destroy_node(program);
    if (ir_program)
      ir_program_destroy(ir_program);
    type_checker_destroy(type_checker);
    symbol_table_destroy(symbol_table);
  }
  free(ir_error_message);
  code_generator_destroy(code_generator);
  register_allocator_destroy(register_allocator);
  parser_destroy(parser);
  lexer_destroy(lexer);
  if (debug_info)
    debug_info_destroy(debug_info);
  error_reporter_destroy(error_reporter);
  free(source);
  compiler_profile_add(&profile, PROFILE_PHASE_CLEANUP, phase_start);
  compiler_profile_print_compile(&profile, input_filename, result);

  return result;
}

void print_usage(const char *program_name) {
  printf("Usage: %s [options] <input.mettle>\n", program_name);
  printf("       %s help [topic]\n", program_name);
  printf("       %s docs [topic]\n", program_name);
  printf("       %s explain <CODE>   Explain a diagnostic code (e.g. E0004, M0103; 'list' for all)\n",
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
  printf("  --linker <mode>     Linker backend: auto, internal, gcc, or msvc "
         "(default: internal with --build, otherwise %s)\n",
         linker_mode_name(LINKER_MODE_AUTO));
  printf("  --link-arg <arg>    Pass an extra linker argument (repeatable; "
         "use with --build)\n");
  printf("  --tracy             Link std/tracy with the Tracy profiler "
         "(requires --build)\n");
  printf("  --tracy-dir <dir>   Tracy repo root (default: TRACY_DIR env, then "
         ".mettle\\tracy_dir)\n");
  printf("  -d, --debug         Enable debug output and symbols\n");
  printf("  --dump-ir           Write optimized IR sidecar (.ir) without debug metadata\n");
  printf("  --simd-report       Report what each @simd loop became (needs -O/--release)\n");
  printf("  --explain           Report every optimization decision in the input file --\n"
         "                      loop vectorization and call inlining, with the reason\n"
         "                      whenever the optimizer declined (needs -O/--release).\n"
         "                      Re-runs lead with what CHANGED since the last build,\n"
         "                      regressions first\n");
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
  printf("  -s, --stack-trace   Embed runtime crash traceback support\n");
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
  printf("  --native-heap       Route new/malloc/calloc/realloc/free through "
         "the Mettle allocator (std/alloc)\n");
  printf("  --static            On Linux, link executable statically\n");
  printf("  --musl              On Linux, link statically with musl-gcc\n");
  printf("  --debug-compiler    Track compiler context for internal error reports\n");
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

  // Get file size
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

  // Allocate buffer and read file
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
