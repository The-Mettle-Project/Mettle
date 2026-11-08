#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "run.h"
#include "main.h"
#include "runtime/owned.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define RUN_SEPARATOR '\\'
#else
#define RUN_SEPARATOR '/'
#endif

#define RUN_DIR_ATTEMPTS 4096
#define RUN_LISTING_BYTES 65536

static void run_usage(FILE *stream) {
  fprintf(stream,
          "usage: mettle run [build flags] [--] <file.mettle> [args...]\n");
}

static const char *run_temp_base(void) {
#ifdef _WIN32
  static const char *const variables[] = {"TEMP", "TMP"};
#else
  static const char *const variables[] = {"TMPDIR"};
#endif
  for (size_t i = 0; i < sizeof(variables) / sizeof(variables[0]); i++) {
    const char *value = getenv(variables[i]);
    if (value && value[0]) {
      return value;
    }
  }
#ifdef _WIN32
  return ".";
#else
  return "/tmp";
#endif
}

static char *run_join(const char *directory, const char *name) {
  size_t directory_length = strlen(directory);
  size_t name_length = strlen(name);
  char *path = (char *)malloc(directory_length + name_length + 2u);
  if (!path) {
    return NULL;
  }
  memcpy(path, directory, directory_length);
  path[directory_length] = RUN_SEPARATOR;
  memcpy(path + directory_length + 1u, name, name_length + 1u);
  return path;
}

static char *run_claim_directory(void) {
  const char *base = run_temp_base();
  char name[32];
  for (unsigned attempt = 0; attempt < RUN_DIR_ATTEMPTS; attempt++) {
    char *directory;
    snprintf(name, sizeof(name), "mettle-run-%u", attempt);
    directory = run_join(base, name);
    if (!directory || mettle_make_directory(directory) == 0) {
      return directory;
    }
    free(directory);
  }
  return NULL;
}

static void run_remove_directory(const char *directory) {
  char *listing = (char *)malloc(RUN_LISTING_BYTES);
  int count = listing ? mettle_dir_list(directory, listing, RUN_LISTING_BYTES)
                      : -1;
  const char *entry = listing;
  for (int i = 0; i < count; i++) {
    size_t length = strlen(entry + 1);
    if (entry[0] == 'f') {
      char *path = run_join(directory, entry + 1);
      if (path) {
        (void)remove(path);
        free(path);
      }
    }
    entry += length + 2u;
  }
  free(listing);
  (void)mettle_remove_directory(directory);
}

static const char *run_base_name(const char *path) {
  const char *name = path;
  for (const char *p = path; *p; p++) {
    if (*p == '/' || *p == '\\') {
      name = p + 1;
    }
  }
  return name;
}

static char *run_executable_in(const char *directory, const char *input_path) {
  char *default_path = mettle_default_executable_filename(input_path);
  char *executable;
  if (!default_path) {
    return NULL;
  }
  executable = run_join(directory, run_base_name(default_path));
  free(default_path);
  return executable;
}

static int run_build(const char *self_path, int argc, char **argv, int input,
                     const char *executable) {
  const char **build_argv =
      (const char **)malloc(sizeof(*build_argv) * ((size_t)argc + 6u));
  int used = 0;
  int status;
  if (!build_argv) {
    fprintf(stderr, "Error: Out of memory\n");
    return 1;
  }
  build_argv[used++] = self_path;
  build_argv[used++] = "--build";
  build_argv[used++] = "--quiet";
  for (int i = 2; i < input; i++) {
    if (strcmp(argv[i], "--") != 0) {
      build_argv[used++] = argv[i];
    }
  }
  build_argv[used++] = argv[input];
  build_argv[used++] = "-o";
  build_argv[used++] = executable;
  build_argv[used] = NULL;
  status = mettle_run_process(self_path, build_argv);
  free(build_argv);
  return status;
}

static int run_program(const char *executable, int argc, char **argv,
                       int input) {
  int program_argc = argc - (input + 1);
  const char **exec_argv = (const char **)malloc(
      sizeof(*exec_argv) * ((size_t)program_argc + 2u));
  int status;
  if (!exec_argv) {
    fprintf(stderr, "Error: Out of memory\n");
    return 1;
  }
  exec_argv[0] = executable;
  for (int i = 0; i < program_argc; i++) {
    exec_argv[i + 1] = argv[input + 1 + i];
  }
  exec_argv[program_argc + 1] = NULL;
  fflush(stdout);
  fflush(stderr);
  status = mettle_run_process(executable, exec_argv);
  free(exec_argv);
  return status;
}

int mettle_run_main(int argc, char **argv, const char *self_path) {
  int input = mettle_input_argument_index(argc, argv, 2);
  char *directory;
  char *executable;
  int status;

  if (input == METTLE_INPUT_HELP) {
    run_usage(stdout);
    return 0;
  }
  if (input < 0) {
    run_usage(stderr);
    return 1;
  }
  directory = run_claim_directory();
  if (!directory) {
    fprintf(stderr, "Error: Could not create a build directory in '%s'\n",
            run_temp_base());
    return 1;
  }
  executable = run_executable_in(directory, argv[input]);
  if (!executable) {
    fprintf(stderr, "Error: Could not determine output name for '%s'\n",
            argv[input]);
    run_remove_directory(directory);
    free(directory);
    return 1;
  }

  status = run_build(self_path, argc, argv, input, executable);
  if (status == 0 && !mettle_path_exists(executable)) {
    fprintf(stderr, "Error: The build reported success but wrote no '%s'\n",
            executable);
    status = 1;
  }
  if (status == 0) {
    status = run_program(executable, argc, argv, input);
  }

  run_remove_directory(directory);
  free(executable);
  free(directory);
  return status;
}
