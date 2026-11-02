#include "live_driver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void live_mark_watched(const char *output, int on) {
  char path[4096];
  FILE *file;
  if (snprintf(path, sizeof(path), "%s.live.watched", output) >=
      (int)sizeof(path)) {
    return;
  }
  if (!on) {
    remove(path);
    return;
  }
  file = fopen(path, "wb");
  if (file) {
    fputs("1\n", file);
    fclose(file);
  }
}

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

static size_t live_scan_list(const char *list_path, const char *directory,
                             LiveWatched *files) {
  FILE *list = fopen(list_path, "rb");
  char line[MAX_PATH];
  size_t count = 0;
  if (!list) {
    return live_scan(directory, files);
  }
  while (count < LIVE_MAX_FILES && fgets(line, sizeof(line), list)) {
    WIN32_FILE_ATTRIBUTE_DATA data;
    size_t length = strlen(line);
    while (length && (line[length - 1u] == '\n' || line[length - 1u] == '\r')) {
      line[--length] = '\0';
    }
    if (!length) {
      continue;
    }
    snprintf(files[count].path, sizeof(files[count].path), "%s", line);
    memset(&files[count].written, 0, sizeof(files[count].written));
    if (GetFileAttributesExA(line, GetFileExInfoStandard, &data)) {
      files[count].written = data.ftLastWriteTime;
    }
    count++;
  }
  fclose(list);
  return count ? count : live_scan(directory, files);
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
  char list_path[MAX_PATH];
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
  {
    char full_output[MAX_PATH];
    if (GetFullPathNameA(output, (DWORD)sizeof(full_output), full_output,
                         NULL)) {
      snprintf(default_output, sizeof(default_output), "%s", full_output);
      output = default_output;
    }
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
  live_mark_watched(output, 1);
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
  snprintf(list_path, sizeof(list_path), "%s.live.files", output);
  watched_count = live_scan_list(list_path, directory, watched);
  fprintf(stderr,
          "[live] running %s; saving any of its %zu source files patches it\n",
          output, watched_count);
  while (WaitForSingleObject(running.hProcess, LIVE_POLL_MS) == WAIT_TIMEOUT) {
    size_t fresh_count = live_scan_list(list_path, directory, fresh);
    if (live_same(watched, watched_count, fresh, fresh_count)) {
      continue;
    }
    do {
      memcpy(watched, fresh, fresh_count * sizeof(LiveWatched));
      watched_count = fresh_count;
      Sleep(LIVE_SETTLE_MS);
      fresh_count = live_scan_list(list_path, directory, fresh);
    } while (!live_same(watched, watched_count, fresh, fresh_count));
    live_run(patch.text);
    watched_count = live_scan_list(list_path, directory, watched);
  }
  GetExitCodeProcess(running.hProcess, &exit_code);
  CloseHandle(running.hThread);
  CloseHandle(running.hProcess);
  live_mark_watched(output, 0);
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

#define LIVE_MAX_FILES 512u
#define LIVE_MAX_ARGUMENTS 256u
#define LIVE_POLL_MS 150u
#define LIVE_SETTLE_MS 60u

int mettle_run_process(const char *program, const char *const *arguments);
long mettle_spawn_process(const char *program, const char *const *arguments);
int mettle_poll_process(long pid, int *exit_code);
void mettle_sleep_ms(unsigned milliseconds);
int mettle_file_mtime(const char *path, long long *seconds,
                      long long *nanoseconds);

typedef struct {
  char path[4096];
  long long seconds;
  long long nanoseconds;
} LiveWatched;

static size_t live_scan_list(const char *list_path, const char *input,
                             LiveWatched *files) {
  FILE *list = fopen(list_path, "rb");
  char line[4096];
  size_t count = 0;
  if (!list) {
    snprintf(files[0].path, sizeof(files[0].path), "%s", input);
    files[0].seconds = 0;
    files[0].nanoseconds = 0;
    mettle_file_mtime(input, &files[0].seconds, &files[0].nanoseconds);
    return 1;
  }
  while (count < LIVE_MAX_FILES && fgets(line, sizeof(line), list)) {
    size_t length = strlen(line);
    while (length && (line[length - 1u] == '\n' || line[length - 1u] == '\r')) {
      line[--length] = '\0';
    }
    if (!length) {
      continue;
    }
    snprintf(files[count].path, sizeof(files[count].path), "%s", line);
    files[count].seconds = 0;
    files[count].nanoseconds = 0;
    mettle_file_mtime(line, &files[count].seconds, &files[count].nanoseconds);
    count++;
  }
  fclose(list);
  return count;
}

static int live_same(const LiveWatched *left, size_t left_count,
                     const LiveWatched *right, size_t right_count) {
  if (left_count != right_count) {
    return 0;
  }
  for (size_t i = 0; i < left_count; i++) {
    if (strcmp(left[i].path, right[i].path) != 0 ||
        left[i].seconds != right[i].seconds ||
        left[i].nanoseconds != right[i].nanoseconds) {
      return 0;
    }
  }
  return 1;
}

static int live_usage(void) {
  fprintf(stderr,
          "usage: mettle live <file.mettle> [compiler flags] [-o program] "
          "[-- program arguments]\n"
          "  Builds the program with --live, runs it, and turns every save of "
          "a source file\n"
          "  it reads into a patch the running program takes at its next "
          "quiesce point.\n");
  return 1;
}

int mettle_live_main(int argc, char **argv) {
  const char *input = NULL;
  const char *output = NULL;
  const char *flags[LIVE_MAX_ARGUMENTS];
  const char *program[LIVE_MAX_ARGUMENTS];
  const char *host[LIVE_MAX_ARGUMENTS + 8u];
  const char *patch[LIVE_MAX_ARGUMENTS + 8u];
  const char *launch[LIVE_MAX_ARGUMENTS + 2u];
  char self[4096];
  char default_output[4096];
  char full_output[4096];
  char list_path[4200];
  size_t flag_count = 0;
  size_t program_count = 0;
  size_t host_count = 0;
  size_t patch_count = 0;
  size_t launch_count = 0;
  LiveWatched *watched = NULL;
  LiveWatched *fresh = NULL;
  size_t watched_count = 0;
  long pid;
  int exit_code = 1;
  int program_arguments = 0;
  int result = 1;

  for (int i = 2; i < argc; i++) {
    size_t length = strlen(argv[i]);
    if (program_arguments) {
      if (program_count < LIVE_MAX_ARGUMENTS) {
        program[program_count++] = argv[i];
      }
    } else if (strcmp(argv[i], "--") == 0) {
      program_arguments = 1;
    } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
      output = argv[++i];
    } else if (!input && length > 7u &&
               strcmp(argv[i] + length - 7u, ".mettle") == 0) {
      input = argv[i];
    } else if (flag_count < LIVE_MAX_ARGUMENTS) {
      flags[flag_count++] = argv[i];
    }
  }
  if (!input) {
    return live_usage();
  }
  if (!output) {
    size_t length = strlen(input) - 7u;
    if (length + 1u > sizeof(default_output)) {
      return 1;
    }
    memcpy(default_output, input, length);
    default_output[length] = '\0';
    output = default_output;
  }
  if (output[0] == '/') {
    snprintf(full_output, sizeof(full_output), "%s", output);
  } else {
    char directory[4096];
    if (!getcwd(directory, sizeof(directory))) {
      directory[0] = '.';
      directory[1] = '\0';
    }
    snprintf(full_output, sizeof(full_output), "%s/%s", directory, output);
  }
  output = full_output;
  if (!realpath("/proc/self/exe", self) && !realpath(argv[0], self)) {
    snprintf(self, sizeof(self), "%s", argv[0]);
  }

  host[host_count++] = self;
  host[host_count++] = "--build";
  host[host_count++] = "--live";
  for (size_t i = 0; i < flag_count; i++) {
    host[host_count++] = flags[i];
  }
  host[host_count++] = input;
  host[host_count++] = "-o";
  host[host_count++] = output;
  host[host_count] = NULL;
  patch[patch_count++] = self;
  patch[patch_count++] = "--live-patch";
  patch[patch_count++] = output;
  for (size_t i = 0; i < flag_count; i++) {
    patch[patch_count++] = flags[i];
  }
  patch[patch_count++] = input;
  patch[patch_count] = NULL;
  launch[launch_count++] = output;
  for (size_t i = 0; i < program_count; i++) {
    launch[launch_count++] = program[i];
  }
  launch[launch_count] = NULL;

  if (mettle_run_process(self, host) != 0) {
    fprintf(stderr, "[live] the first build failed\n");
    return 1;
  }
  watched = calloc(LIVE_MAX_FILES, sizeof(LiveWatched));
  fresh = calloc(LIVE_MAX_FILES, sizeof(LiveWatched));
  if (!watched || !fresh) {
    goto done;
  }
  live_mark_watched(output, 1);
  pid = mettle_spawn_process(output, launch);
  if (pid <= 0) {
    fprintf(stderr, "[live] could not start '%s'\n", output);
    goto done;
  }
  snprintf(list_path, sizeof(list_path), "%s.live.files", output);
  watched_count = live_scan_list(list_path, input, watched);
  fprintf(stderr,
          "[live] running %s; saving any of its %zu source files patches it\n",
          output, watched_count);
  for (;;) {
    size_t fresh_count;
    int state = mettle_poll_process(pid, &exit_code);
    if (state != 0) {
      break;
    }
    mettle_sleep_ms(LIVE_POLL_MS);
    fresh_count = live_scan_list(list_path, input, fresh);
    if (live_same(watched, watched_count, fresh, fresh_count)) {
      continue;
    }
    do {
      memcpy(watched, fresh, fresh_count * sizeof(LiveWatched));
      watched_count = fresh_count;
      mettle_sleep_ms(LIVE_SETTLE_MS);
      fresh_count = live_scan_list(list_path, input, fresh);
    } while (!live_same(watched, watched_count, fresh, fresh_count));
    mettle_run_process(self, patch);
    watched_count = live_scan_list(list_path, input, watched);
  }
  live_mark_watched(output, 0);
  fprintf(stderr, "[live] %s exited with %d\n", output, exit_code);
  result = exit_code;

done:
  free(watched);
  free(fresh);
  return result;
}

#endif
