#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "run.h"
#include "main.h"
#include "runtime/owned.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void run_usage(FILE *stream) {
  fprintf(stream, "usage: mettle run [--release] <file.mettle> [args...]\n");
}

static char *run_launch_path(const char *executable_path) {
  size_t len;
  char *launch;

  if (strchr(executable_path, '/') || strchr(executable_path, '\\')) {
    return strdup(executable_path);
  }
  len = strlen(executable_path);
  launch = (char *)malloc(len + 3u);
  if (!launch) {
    return NULL;
  }
  launch[0] = '.';
#ifdef _WIN32
  launch[1] = '\\';
#else
  launch[1] = '/';
#endif
  memcpy(launch + 2, executable_path, len + 1u);
  return launch;
}

int mettle_run_main(int argc, char **argv, const char *self_path) {
  const char *input_path = NULL;
  int release = 0;
  char *executable_path = NULL;
  char *object_path = NULL;
  char *launch_path = NULL;
  const char *build_argv[8];
  const char **exec_argv;
  int program_argc = 0;
  int used = 0;
  int status;
  int i;

  /* Run options come before the file; everything after it is the program's. */
  for (i = 2; i < argc && !input_path; i++) {
    if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
      run_usage(stdout);
      return 0;
    }
    if (strcmp(argv[i], "--release") == 0 || strcmp(argv[i], "-r") == 0) {
      release = 1;
      continue;
    }
    if (argv[i][0] != '-') {
      input_path = argv[i];
      program_argc = argc - (i + 1);
      continue;
    }
    run_usage(stderr);
    return 1;
  }
  if (!input_path) {
    run_usage(stderr);
    return 1;
  }
  executable_path = mettle_default_executable_filename(input_path);
  object_path = mettle_object_filename_for(executable_path);
  if (!executable_path || !object_path) {
    fprintf(stderr, "Error: Could not determine output name for '%s'\n",
            input_path);
    free(executable_path);
    free(object_path);
    return 1;
  }

  build_argv[used++] = self_path;
  build_argv[used++] = "--build";
  build_argv[used++] = "--quiet";
  if (release) {
    build_argv[used++] = "--release";
  }
  build_argv[used++] = input_path;
  build_argv[used++] = "-o";
  build_argv[used++] = executable_path;
  build_argv[used] = NULL;

  status = mettle_run_process(self_path, build_argv);
  (void)remove(object_path);
  free(object_path);

  if (status != 0) {
    free(executable_path);
    return status;
  }

  launch_path = run_launch_path(executable_path);
  free(executable_path);
  exec_argv = (const char **)malloc(sizeof(*exec_argv) *
                                    ((size_t)program_argc + 2u));
  if (!launch_path || !exec_argv) {
    fprintf(stderr, "Error: Out of memory\n");
    free(launch_path);
    free(exec_argv);
    return 1;
  }
  exec_argv[0] = launch_path;
  for (i = 0; i < program_argc; i++) {
    exec_argv[i + 1] = argv[argc - program_argc + i];
  }
  exec_argv[program_argc + 1] = NULL;
  fflush(stdout);
  fflush(stderr);
  /* The program's exit code becomes ours, so `mettle run` is transparent to
     scripts and test harnesses. */
  status = mettle_run_process(launch_path, exec_argv);
  free(exec_argv);
  free(launch_path);
  return status;
}
