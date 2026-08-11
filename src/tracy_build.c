#include "tracy_build.h"

#include "common.h"
#include "runtime/owned.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

static char *tracy_join_paths(const char *left, const char *right) {
  if (!left || !right) {
    return NULL;
  }

  size_t left_len = strlen(left);
  size_t right_len = strlen(right);
  int has_sep = left_len > 0 &&
                (left[left_len - 1] == '/' || left[left_len - 1] == '\\');
  size_t total = left_len + right_len + (has_sep ? 1u : 2u);
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

static char *tracy_replace_extension(const char *path, const char *extension) {
  const char *last_dot = NULL;
  const char *cursor = path;

  if (!path || !extension) {
    return NULL;
  }

  while (*cursor != '\0') {
    if (*cursor == '.' &&
        (cursor[1] != '\\' && cursor[1] != '/')) {
      last_dot = cursor;
    }
    if (*cursor == '\\' || *cursor == '/') {
      last_dot = NULL;
    }
    cursor++;
  }

  size_t prefix_len = last_dot ? (size_t)(last_dot - path) : strlen(path);
  size_t extension_len = strlen(extension);
  char *result = malloc(prefix_len + extension_len + 1u);

  if (!result) {
    return NULL;
  }

  memcpy(result, path, prefix_len);
  memcpy(result + prefix_len, extension, extension_len);
  result[prefix_len + extension_len] = '\0';
  return result;
}

static int tracy_directory_exists(const char *path) {
  if (!path || path[0] == '\0') {
    return 0;
  }
  return mettle_path_is_directory(path);
}

static int tracy_file_exists(const char *path) {
  return mettle_path_exists(path);
}

static int tracy_run_command(const char *command) {
  if (!command || command[0] == '\0') {
    return 1;
  }
  return system(command);
}

static int tracy_tool_exists(const char *tool_name) {
  char command[128];

  if (!tool_name || tool_name[0] == '\0') {
    return 0;
  }

  snprintf(command, sizeof(command), "where %s >nul 2>&1", tool_name);
  return tracy_run_command(command) == 0;
}

static char *tracy_trim_quotes(char *text) {
  size_t len = 0;

  if (!text) {
    return NULL;
  }

  len = strlen(text);
  while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\t' ||
                     text[len - 1] == '\r' || text[len - 1] == '\n')) {
    text[--len] = '\0';
  }

  if (text[0] == '"') {
    memmove(text, text + 1, strlen(text));
    len = strlen(text);
    if (len > 0 && text[len - 1] == '"') {
      text[len - 1] = '\0';
    }
  }

  return text;
}

static char *tracy_parse_tracy_dir_from_line(const char *line) {
  const char *marker = "TRACY_DIR=";
  const char *start = NULL;
  char *value = NULL;

  if (!line) {
    return NULL;
  }

  start = strstr(line, marker);
  if (!start) {
    return NULL;
  }

  start += strlen(marker);
  value = mettle_strdup(start);
  if (!value) {
    return NULL;
  }

  return tracy_trim_quotes(value);
}

static char *tracy_read_plain_config_file(const char *path) {
  FILE *file = NULL;
  char line[1024];
  char *resolved = NULL;

  if (!path || !tracy_file_exists(path)) {
    return NULL;
  }

  file = fopen(path, "r");
  if (!file) {
    return NULL;
  }

  if (fgets(line, sizeof(line), file) != NULL) {
    resolved = mettle_strdup(line);
    if (resolved) {
      resolved = tracy_trim_quotes(resolved);
    }
  }

  fclose(file);
  return resolved;
}

static char *tracy_read_config_file(const char *path) {
  FILE *file = NULL;
  char line[1024];
  char *resolved = NULL;

  if (path) {
    resolved = tracy_read_plain_config_file(path);
  }
  return resolved;
}

static char *tracy_read_bat_config_file(const char *path) {
  FILE *file = NULL;
  char line[1024];
  char *resolved = NULL;

  if (!path || !tracy_file_exists(path)) {
    return NULL;
  }

  file = fopen(path, "r");
  if (!file) {
    return NULL;
  }

  while (fgets(line, sizeof(line), file) != NULL) {
    char *candidate = tracy_parse_tracy_dir_from_line(line);
    if (candidate && candidate[0] != '\0') {
      resolved = candidate;
      break;
    }
    free(candidate);
  }

  fclose(file);
  return resolved;
}

static char *tracy_read_local_config(void) {
  char *cwd = getcwd(NULL, 0);
  char *path = NULL;
  char *resolved = NULL;

  if (!cwd) {
    return NULL;
  }

  path = tracy_join_paths(cwd, ".mettle\\tracy_dir");
  free(cwd);
  if (path) {
    resolved = tracy_read_config_file(path);
    free(path);
  }
  return resolved;
}

static char *tracy_parent_directory(const char *path) {
  const char *last_slash = NULL;
  const char *last_backslash = NULL;
  const char *last_sep = NULL;
  size_t len = 0;
  char *parent = NULL;

  if (!path || path[0] == '\0') {
    return NULL;
  }

  last_slash = strrchr(path, '/');
  last_backslash = strrchr(path, '\\');
  last_sep = (last_slash > last_backslash) ? last_slash : last_backslash;
  if (!last_sep) {
    return NULL;
  }

  len = (size_t)(last_sep - path);
  parent = malloc(len + 1u);
  if (!parent) {
    return NULL;
  }

  memcpy(parent, path, len);
  parent[len] = '\0';
  return parent;
}

static char *tracy_read_demo_config(const char *stdlib_directory) {
  char *parent = NULL;
  char *bat_path = NULL;
  char *resolved = NULL;

  if (!stdlib_directory || stdlib_directory[0] == '\0') {
    return NULL;
  }

  parent = tracy_parent_directory(stdlib_directory);
  if (!parent) {
    return NULL;
  }

  bat_path = tracy_join_paths(parent,
                              "examples\\tracy_demo\\tracy_dir.local.bat");
  free(parent);
  if (!bat_path) {
    return NULL;
  }

  resolved = tracy_read_bat_config_file(bat_path);
  free(bat_path);
  return resolved;
}

int tracy_directory_is_valid(const char *directory) {
  char *header_path = NULL;
  char *client_path = NULL;
  int valid = 0;

  if (!directory || directory[0] == '\0') {
    return 0;
  }

  header_path = tracy_join_paths(directory, "public\\tracy\\TracyC.h");
  client_path = tracy_join_paths(directory, "public\\TracyClient.cpp");
  if (!header_path || !client_path) {
    free(header_path);
    free(client_path);
    return 0;
  }

  valid = tracy_file_exists(header_path) && tracy_file_exists(client_path);
  free(header_path);
  free(client_path);
  return valid;
}

int tracy_save_directory_config(const char *directory, char **error_out) {
  char *cwd = getcwd(NULL, 0);
  char *config_dir = NULL;
  char *config_path = NULL;
  FILE *file = NULL;

  if (error_out) {
    *error_out = NULL;
  }

  if (!directory || directory[0] == '\0') {
    mettle_set_error(error_out, "Cannot save an empty Tracy directory path");
    return 0;
  }

  if (!cwd) {
    mettle_set_error(error_out, "Failed to read the current working directory");
    return 0;
  }

  config_dir = tracy_join_paths(cwd, ".mettle");
  free(cwd);
  if (!config_dir) {
    mettle_set_error(error_out, "Out of memory while saving Tracy directory config");
    return 0;
  }

  (void)mettle_make_directory(config_dir);

  config_path = tracy_join_paths(config_dir, "tracy_dir");
  free(config_dir);
  if (!config_path) {
    mettle_set_error(error_out, "Out of memory while saving Tracy directory config");
    return 0;
  }

  file = fopen(config_path, "w");
  if (!file) {
    mettle_set_error(error_out, "Failed to write Tracy config file '%s'", config_path);
    free(config_path);
    return 0;
  }

  fprintf(file, "%s\n", directory);
  fclose(file);
  free(config_path);
  return 1;
}

char *tracy_resolve_directory(const TracyBuildRequest *request, char **error_out) {
  char *resolved = NULL;

  if (error_out) {
    *error_out = NULL;
  }

  if (request && request->tracy_directory && request->tracy_directory[0] != '\0') {
    resolved = mettle_strdup(request->tracy_directory);
  }

  if (!resolved) {
    const char *env_dir = getenv("TRACY_DIR");
    if (env_dir && env_dir[0] != '\0') {
      resolved = mettle_strdup(env_dir);
    }
  }

  if (!resolved) {
    resolved = tracy_read_local_config();
  }

  if (!resolved && request && request->stdlib_directory) {
    resolved = tracy_read_demo_config(request->stdlib_directory);
  }

  if (!resolved) {
    mettle_set_error(
        error_out,
        "Tracy directory not configured. Set TRACY_DIR, pass --tracy-dir, or "
        "create .mettle\\tracy_dir (see examples\\tracy_demo\\build.bat).");
    return NULL;
  }

  if (!tracy_directory_is_valid(resolved)) {
    mettle_set_error(error_out,
                     "TRACY_DIR does not look like a Tracy repo root (expected "
                     "public\\tracy\\TracyC.h and public\\TracyClient.cpp): %s",
                     resolved);
    free(resolved);
    return NULL;
  }

  if (!tracy_save_directory_config(resolved, error_out)) {
    free(resolved);
    return NULL;
  }

  return resolved;
}

static int tracy_compile_with_cl(const char *tracy_directory,
                                 const char *stdlib_directory,
                                 const char *helpers_object,
                                 const char *client_object, char **error_out) {
  char *public_dir = tracy_join_paths(tracy_directory, "public");
  char *helpers_source = tracy_join_paths(stdlib_directory, "tracy_helpers.c");
  char *client_source = tracy_join_paths(public_dir, "TracyClient.cpp");
  char *command = NULL;
  size_t command_len = 0;
  int result = 0;

  if (!public_dir || !helpers_source || !client_source) {
    mettle_set_error(error_out, "Out of memory while preparing Tracy build commands");
    result = 0;
    goto cleanup;
  }

  command_len = strlen(public_dir) + strlen(helpers_source) + strlen(client_source) +
                strlen(helpers_object) + strlen(client_object) + 512u;
  command = malloc(command_len);
  if (!command) {
    mettle_set_error(error_out, "Out of memory while preparing Tracy build commands");
    result = 0;
    goto cleanup;
  }

  snprintf(command, command_len,
           "cl /nologo /c /DTRACY_ENABLE /I \"%s\" \"%s\" /Fo\"%s\"",
           public_dir, helpers_source, helpers_object);
  if (tracy_run_command(command) != 0) {
    mettle_set_error(error_out, "Failed to compile stdlib\\tracy_helpers.c with TRACY_ENABLE");
    result = 0;
    goto cleanup;
  }

  snprintf(command, command_len,
           "cl /nologo /c /DTRACY_ENABLE /I \"%s\" /TP \"%s\" /Fo\"%s\"",
           public_dir, client_source, client_object);
  if (tracy_run_command(command) != 0) {
    mettle_set_error(error_out, "Failed to compile TracyClient.cpp with TRACY_ENABLE");
    result = 0;
    goto cleanup;
  }

  result = 1;

cleanup:
  free(command);
  free(public_dir);
  free(helpers_source);
  free(client_source);
  return result;
}

static int tracy_compile_with_gxx(const char *tracy_directory,
                                  const char *stdlib_directory,
                                  const char *helpers_object,
                                  const char *client_object, char **error_out) {
  char *public_dir = tracy_join_paths(tracy_directory, "public");
  char *helpers_source = tracy_join_paths(stdlib_directory, "tracy_helpers.c");
  char *client_source = tracy_join_paths(public_dir, "TracyClient.cpp");
  char *command = NULL;
  size_t command_len = 0;
  int result = 0;

  if (!public_dir || !helpers_source || !client_source) {
    mettle_set_error(error_out, "Out of memory while preparing Tracy build commands");
    result = 0;
    goto cleanup;
  }

  command_len = strlen(public_dir) + strlen(helpers_source) + strlen(client_source) +
                strlen(helpers_object) + strlen(client_object) + 512u;
  command = malloc(command_len);
  if (!command) {
    mettle_set_error(error_out, "Out of memory while preparing Tracy build commands");
    result = 0;
    goto cleanup;
  }

  snprintf(command, command_len,
           "gcc -c -DTRACY_ENABLE -I\"%s\" \"%s\" -o \"%s\"", public_dir,
           helpers_source, helpers_object);
  if (tracy_run_command(command) != 0) {
    mettle_set_error(error_out, "Failed to compile stdlib/tracy_helpers.c with TRACY_ENABLE");
    result = 0;
    goto cleanup;
  }

  snprintf(command, command_len,
           "g++ -c -DTRACY_ENABLE -I\"%s\" \"%s\" -o \"%s\"", public_dir,
           client_source, client_object);
  if (tracy_run_command(command) != 0) {
    mettle_set_error(error_out, "Failed to compile TracyClient.cpp with TRACY_ENABLE");
    result = 0;
    goto cleanup;
  }

  result = 1;

cleanup:
  free(command);
  free(public_dir);
  free(helpers_source);
  free(client_source);
  return result;
}

int tracy_build_support_objects(const TracyBuildRequest *request,
                                const char *tracy_directory,
                                TracyBuildArtifacts *artifacts,
                                char **error_out) {
  char *helpers_source = NULL;

  if (error_out) {
    *error_out = NULL;
  }

  if (!request || !tracy_directory || !artifacts || !request->executable_filename ||
      !request->stdlib_directory) {
    mettle_set_error(error_out, "Missing Tracy build inputs");
    return 0;
  }

  memset(artifacts, 0, sizeof(*artifacts));
  artifacts->helpers_object =
      tracy_replace_extension(request->executable_filename, ".tracy_helpers.obj");
  artifacts->client_object =
      tracy_replace_extension(request->executable_filename, ".TracyClient.obj");
  helpers_source = tracy_join_paths(request->stdlib_directory, "tracy_helpers.c");

  if (!artifacts->helpers_object || !artifacts->client_object || !helpers_source) {
    mettle_set_error(error_out, "Out of memory while preparing Tracy object paths");
    tracy_free_artifacts(artifacts);
    free(helpers_source);
    return 0;
  }

  if (!tracy_file_exists(helpers_source)) {
    mettle_set_error(error_out, "Tracy helper source not found: %s", helpers_source);
    tracy_free_artifacts(artifacts);
    free(helpers_source);
    return 0;
  }

  if (tracy_tool_exists("cl")) {
    if (!tracy_compile_with_cl(tracy_directory, request->stdlib_directory,
                               artifacts->helpers_object, artifacts->client_object,
                               error_out)) {
      tracy_free_artifacts(artifacts);
      free(helpers_source);
      return 0;
    }
    artifacts->use_gxx_link = 0;
  } else if (tracy_tool_exists("g++")) {
    if (!tracy_compile_with_gxx(tracy_directory, request->stdlib_directory,
                                artifacts->helpers_object, artifacts->client_object,
                                error_out)) {
      tracy_free_artifacts(artifacts);
      free(helpers_source);
      return 0;
    }
    artifacts->use_gxx_link = 1;
  } else {
    mettle_set_error(error_out,
                     "Need MSVC cl or MinGW g++ to compile the Tracy C++ client");
    tracy_free_artifacts(artifacts);
    free(helpers_source);
    return 0;
  }

  if (!tracy_file_exists(artifacts->helpers_object) ||
      !tracy_file_exists(artifacts->client_object)) {
    mettle_set_error(error_out, "Tracy support objects were not created");
    tracy_free_artifacts(artifacts);
    free(helpers_source);
    return 0;
  }

  free(helpers_source);
  return 1;
}

void tracy_free_artifacts(TracyBuildArtifacts *artifacts) {
  if (!artifacts) {
    return;
  }
  free(artifacts->helpers_object);
  free(artifacts->client_object);
  memset(artifacts, 0, sizeof(*artifacts));
}
