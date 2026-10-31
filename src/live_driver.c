#include "live_driver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>

#define LIVE_MAX_FILES 512u
#define LIVE_POLL_MS 150u
#define LIVE_SETTLE_MS 60u

typedef struct {
  char path[MAX_PATH];
  FILETIME written;
} LiveWatched;

typedef struct {
  char *text;
  size_t length;
  size_t capacity;
} LiveCommand;

static int live_command_append(LiveCommand *command, const char *text,
                               size_t length) {
  if (command->length + length + 1u > command->capacity) {
    size_t capacity = command->capacity ? command->capacity : 256u;
    while (capacity < command->length + length + 1u) {
      capacity *= 2u;
    }
    char *grown = realloc(command->text, capacity);
    if (!grown) {
      return 0;
    }
    command->text = grown;
    command->capacity = capacity;
  }
  memcpy(command->text + command->length, text, length);
  command->length += length;
  command->text[command->length] = '\0';
  return 1;
}

static int live_command_argument(LiveCommand *command, const char *argument) {
  size_t backslashes = 0;
  if (command->length && !live_command_append(command, " ", 1u)) {
    return 0;
  }
  if (argument[0] && !strpbrk(argument, " \t\"")) {
    return live_command_append(command, argument, strlen(argument));
  }
  if (!live_command_append(command, "\"", 1u)) {
    return 0;
  }
  for (const char *at = argument; *at; at++) {
    if (*at == '\\') {
      backslashes++;
    } else {
      if (*at == '"') {
        for (size_t i = 0; i <= backslashes; i++) {
          if (!live_command_append(command, "\\", 1u)) {
            return 0;
          }
        }
      }
      backslashes = 0;
    }
    if (!live_command_append(command, at, 1u)) {
      return 0;
    }
  }
  for (size_t i = 0; i < backslashes; i++) {
    if (!live_command_append(command, "\\", 1u)) {
      return 0;
    }
  }
  return live_command_append(command, "\"", 1u);
}

static int live_start(const char *command_line, PROCESS_INFORMATION *process) {
  STARTUPINFOA startup;
  char *mutable_line = strdup(command_line);
  int started = 0;
  memset(&startup, 0, sizeof(startup));
  memset(process, 0, sizeof(*process));
  startup.cb = sizeof(startup);
  if (mutable_line) {
    started = CreateProcessA(NULL, mutable_line, NULL, NULL, TRUE, 0, NULL,
                             NULL, &startup, process) != 0;
  }
  free(mutable_line);
  return started;
}

static int live_run(const char *command_line) {
  PROCESS_INFORMATION process;
  DWORD code = 1;
  if (!live_start(command_line, &process)) {
    return -1;
  }
  WaitForSingleObject(process.hProcess, INFINITE);
  GetExitCodeProcess(process.hProcess, &code);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return (int)code;
}

static size_t live_scan(const char *directory, LiveWatched *files) {
  char pattern[MAX_PATH];
  WIN32_FIND_DATAA found;
  HANDLE search;
  size_t count = 0;
  snprintf(pattern, sizeof(pattern), "%s\\*.mettle", directory);
  search = FindFirstFileA(pattern, &found);
  if (search == INVALID_HANDLE_VALUE) {
    return 0;
  }
  do {
    if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
        count < LIVE_MAX_FILES) {
      snprintf(files[count].path, sizeof(files[count].path), "%s\\%s",
               directory, found.cFileName);
      files[count].written = found.ftLastWriteTime;
      count++;
    }
  } while (FindNextFileA(search, &found));
  FindClose(search);
  return count;
}

static int live_same(const LiveWatched *left, size_t left_count,
                     const LiveWatched *right, size_t right_count) {
  if (left_count != right_count) {
    return 0;
  }
  for (size_t i = 0; i < left_count; i++) {
    if (strcmp(left[i].path, right[i].path) != 0 ||
        CompareFileTime(&left[i].written, &right[i].written) != 0) {
      return 0;
    }
  }
  return 1;
}

static int live_usage(void) {
  fprintf(stderr,
          "usage: mettle live <file.mettle> [compiler flags] [-o program.exe] "
          "[-- program arguments]\n"
          "  Builds the program with --live, runs it, and turns every save of "
          "a .mettle file\n"
          "  next to it into a patch the running program takes at its next "
          "quiesce point.\n");
  return 1;
}

int mettle_live_main(int argc, char **argv) {
  const char *input = NULL;
  const char *output = NULL;
  char default_output[MAX_PATH];
  char self[MAX_PATH];
  char directory[MAX_PATH];
  LiveCommand flags = {0};
  LiveCommand host = {0};
  LiveCommand patch = {0};
  LiveCommand program = {0};
  LiveWatched *watched = NULL;
  LiveWatched *fresh = NULL;
  size_t watched_count = 0;
  PROCESS_INFORMATION running;
  DWORD exit_code = 1;
  int program_arguments = 0;
  int result = 1;

  for (int i = 2; i < argc; i++) {
    if (program_arguments) {
      if (!live_command_argument(&program, argv[i])) {
        goto done;
      }
    } else if (strcmp(argv[i], "--") == 0) {
      program_arguments = 1;
    } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
      output = argv[++i];
    } else if (!input && strlen(argv[i]) > 7u &&
               strcmp(argv[i] + strlen(argv[i]) - 7u, ".mettle") == 0) {
      input = argv[i];
    } else if (!live_command_argument(&flags, argv[i])) {
      goto done;
    }
  }
  if (!input) {
    result = live_usage();
    goto done;
  }
  if (!output) {
    size_t length = strlen(input) - 7u;
    if (length + 5u > sizeof(default_output)) {
      goto done;
    }
    memcpy(default_output, input, length);
    memcpy(default_output + length, ".exe", 5u);
    output = default_output;
  }
  if (!GetModuleFileNameA(NULL, self, sizeof(self))) {
    goto done;
  }
  snprintf(directory, sizeof(directory), "%s", input);
  {
    char *slash = strrchr(directory, '\\');
    char *forward = strrchr(directory, '/');
    if (forward && (!slash || forward > slash)) {
      slash = forward;
    }
    if (slash) {
      *slash = '\0';
    } else {
      snprintf(directory, sizeof(directory), ".");
    }
  }

  if (!live_command_argument(&host, self) ||
      !live_command_argument(&host, "--build") ||
      !live_command_argument(&host, "--live") ||
      (flags.length && !live_command_append(&host, " ", 1u)) ||
      (flags.length && !live_command_append(&host, flags.text, flags.length)) ||
      !live_command_argument(&host, input) ||
      !live_command_argument(&host, "-o") ||
      !live_command_argument(&host, output) ||
      !live_command_argument(&patch, self) ||
      !live_command_argument(&patch, "--live-patch") ||
      !live_command_argument(&patch, output) ||
      (flags.length && !live_command_append(&patch, " ", 1u)) ||
      (flags.length && !live_command_append(&patch, flags.text, flags.length)) ||
      !live_command_argument(&patch, input)) {
    goto done;
  }
  if (live_run(host.text) != 0) {
    fprintf(stderr, "[live] the first build failed\n");
    goto done;
  }
  {
    LiveCommand launch = {0};
    int started = live_command_argument(&launch, output) &&
                  (!program.length || live_command_append(&launch, " ", 1u)) &&
                  (!program.length ||
                   live_command_append(&launch, program.text, program.length)) &&
                  live_start(launch.text, &running);
    free(launch.text);
    if (!started) {
      fprintf(stderr, "[live] could not start '%s'\n", output);
      goto done;
    }
  }
  watched = calloc(LIVE_MAX_FILES, sizeof(LiveWatched));
  fresh = calloc(LIVE_MAX_FILES, sizeof(LiveWatched));
  if (!watched || !fresh) {
    TerminateProcess(running.hProcess, 1);
    goto done;
  }
  watched_count = live_scan(directory, watched);
  fprintf(stderr, "[live] running %s; saving a .mettle file in %s patches it\n",
          output, directory);
  while (WaitForSingleObject(running.hProcess, LIVE_POLL_MS) == WAIT_TIMEOUT) {
    size_t fresh_count = live_scan(directory, fresh);
    if (live_same(watched, watched_count, fresh, fresh_count)) {
      continue;
    }
    do {
      memcpy(watched, fresh, fresh_count * sizeof(LiveWatched));
      watched_count = fresh_count;
      Sleep(LIVE_SETTLE_MS);
      fresh_count = live_scan(directory, fresh);
    } while (!live_same(watched, watched_count, fresh, fresh_count));
    live_run(patch.text);
  }
  GetExitCodeProcess(running.hProcess, &exit_code);
  CloseHandle(running.hThread);
  CloseHandle(running.hProcess);
  fprintf(stderr, "[live] %s exited with %lu\n", output,
          (unsigned long)exit_code);
  result = (int)exit_code;

done:
  free(flags.text);
  free(host.text);
  free(patch.text);
  free(program.text);
  free(watched);
  free(fresh);
  return result;
}

#else

int mettle_live_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  fprintf(stderr, "Error: mettle live runs on Windows x64 only for now\n");
  return 1;
}

#endif
