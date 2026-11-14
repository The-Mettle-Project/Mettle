#include "explain_command.h"
#include "explain_json.h"
#include "explain_model.h"
#include "explain_term.h"
#include "explain_tui.h"
#include "../error/diag_style.h"
#include "../ir/ir_optimize.h"
#include "../runtime/owned.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#define ex_isatty _isatty
#define ex_fileno _fileno
#else
#include <unistd.h>
#define ex_isatty isatty
#define ex_fileno fileno
#endif

static int ex_ends_with(const char *text, const char *suffix) {
  size_t n = strlen(text);
  size_t m = strlen(suffix);
  return n >= m && strcmp(text + n - m, suffix) == 0;
}

int mettle_explain_is_view_target(const char *arg) {
  if (!arg || !(ex_ends_with(arg, ".mettle") || ex_ends_with(arg, ".json"))) {
    return 0;
  }
  if (!mettle_path_exists(arg) || mettle_path_is_directory(arg)) {
    fprintf(stderr, "Error: %s does not exist\n", arg);
    return -1;
  }
  return 1;
}

int mettle_explain_parse_flag(CompilerOptions *options, const char *arg) {
  if (strncmp(arg, "--screen=", 9) == 0) {
    int columns = 0;
    int rows = 0;
    if (sscanf(arg + 9, "%dx%d", &columns, &rows) != 2 || columns < 20 ||
        rows < 6 || columns > 1000 || rows > 1000) {
      fprintf(stderr, "Error: --screen expects COLUMNSxROWS, such as "
                      "--screen=120x40\n");
      return -1;
    }
    options->explain_view_columns = columns;
    options->explain_view_rows = rows;
    return 1;
  }
  if (strncmp(arg, "--keys=", 7) == 0) {
    options->explain_view_keys = arg + 7;
    return 1;
  }
  if (strcmp(arg, "--text") == 0) {
    options->explain_view_text = 1;
    return 1;
  }
  return 0;
}

unsigned mettle_explain_build_key(int argc, char **argv) {
  unsigned hash = 2166136261u;
  for (int i = 0; i < argc; i++) {
    const char *arg = argv[i];
    if (!arg || strncmp(arg, "--screen=", 9) == 0 ||
        strncmp(arg, "--keys=", 7) == 0 || strcmp(arg, "--text") == 0) {
      continue;
    }
    for (const char *p = arg; *p; p++) {
      hash = (hash ^ (unsigned char)*p) * 16777619u;
    }
    hash = (hash ^ 0xffu) * 16777619u;
  }
  return hash;
}

static char *ex_read_file(const char *path, size_t *length) {
  FILE *in = fopen(path, "rb");
  long size;
  char *text;
  if (!in) {
    return NULL;
  }
  if (fseek(in, 0, SEEK_END) != 0 || (size = ftell(in)) < 0 ||
      fseek(in, 0, SEEK_SET) != 0) {
    fclose(in);
    return NULL;
  }
  text = malloc((size_t)size + 1);
  if (!text) {
    fclose(in);
    return NULL;
  }
  *length = fread(text, 1, (size_t)size, in);
  fclose(in);
  text[*length] = '\0';
  return text;
}

static int ex_show(ExReport *report, const CompilerOptions *options) {
  ExTerm *term;
  if (options->explain_view_text) {
    ex_report_print(report, stdout);
    return 0;
  }
  if (options->explain_view_columns > 0) {
    term = ex_term_virtual(options->explain_view_columns,
                           options->explain_view_rows,
                           options->explain_view_keys);
    if (!term) {
      return 1;
    }
    ex_tui_run(report, term);
    ex_term_dump(term, stdout);
    ex_term_close(term);
    return 0;
  }
  term = ex_term_open();
  if (!term) {
    ex_report_print(report, stdout);
    return 0;
  }
  ex_tui_run(report, term);
  ex_term_close(term);
  return 0;
}

static ExReport *ex_load_report(const char *json_path, const char *source) {
  size_t length = 0;
  char error[160];
  char *text = ex_read_file(json_path, &length);
  ExJson *root;
  ExReport *report;
  if (!text) {
    fprintf(stderr, "Error: could not read %s\n", json_path);
    return NULL;
  }
  root = ex_json_parse(text, length, error, sizeof(error));
  free(text);
  if (!root) {
    fprintf(stderr, "Error: %s is not an explain report: %s\n", json_path,
            error);
    return NULL;
  }
  if (ex_json_int(root, "schema", 0) != 3) {
    fprintf(stderr, "Error: %s was written by a different compiler; rebuild "
                    "it with --explain-json\n",
            json_path);
    ex_json_free(root);
    return NULL;
  }
  report = ex_report_build(root, source);
  ex_json_free(root);
  if (!report) {
    fprintf(stderr, "Error: out of memory reading %s\n", json_path);
  }
  return report;
}

int mettle_explain_report_file(int argc, char **argv) {
  CompilerOptions options;
  ExReport *report;
  int result;
  memset(&options, 0, sizeof(options));
  for (int i = 1; i < argc; i++) {
    int parsed = mettle_explain_parse_flag(&options, argv[i]);
    if (parsed < 0) {
      return 1;
    }
    if (parsed == 0) {
      fprintf(stderr, "Error: mettle explain does not take '%s' with a "
                      "saved report\n",
              argv[i]);
      return 1;
    }
  }
  report = ex_load_report(argv[0], NULL);
  if (!report) {
    return 1;
  }
  result = ex_show(report, &options);
  ex_report_free(report);
  return result;
}

static const char *ex_cache_root(char *buffer, size_t size) {
  const char *value = getenv("METTLE_CACHE");
  if (value && *value) {
    snprintf(buffer, size, "%s", value);
    return buffer;
  }
  value = getenv("LOCALAPPDATA");
  if (value && *value) {
    snprintf(buffer, size, "%s/Mettle/cache", value);
    return buffer;
  }
  value = getenv("XDG_CACHE_HOME");
  if (value && *value) {
    snprintf(buffer, size, "%s/mettle", value);
    return buffer;
  }
  value = getenv("HOME");
  if (value && *value) {
    snprintf(buffer, size, "%s/.cache/mettle", value);
    return buffer;
  }
  return NULL;
}

static int ex_make_dirs(char *path) {
  for (char *at = path; *at; at++) {
    if ((*at == '/' || *at == '\\') && at > path && at[-1] != ':' &&
        at[-1] != '/' && at[-1] != '\\') {
      char saved = *at;
      *at = '\0';
      if (!mettle_path_is_directory(path)) {
        mettle_make_directory(path);
      }
      *at = saved;
    }
  }
  if (!mettle_path_is_directory(path)) {
    mettle_make_directory(path);
  }
  return mettle_path_is_directory(path);
}

static char *ex_cache_object(const char *input, const char *absolute,
                              unsigned key) {
  char root[1024];
  char dir[1100];
  const char *base = input;
  const char *dot = NULL;
  unsigned hash = key;
  char *path;
  size_t stem;
  for (const char *p = input; *p; p++) {
    if (*p == '/' || *p == '\\') {
      base = p + 1;
    }
  }
  for (const char *p = base; *p; p++) {
    if (*p == '.') {
      dot = p;
    }
  }
  stem = dot ? (size_t)(dot - base) : strlen(base);
  for (const char *p = absolute ? absolute : input; *p; p++) {
    hash = (hash ^ (unsigned char)*p) * 16777619u;
  }
  if (ex_cache_root(root, sizeof(root))) {
    snprintf(dir, sizeof(dir), "%s/explain", root);
    if (!ex_make_dirs(dir)) {
      snprintf(dir, sizeof(dir), ".");
    }
  } else {
    snprintf(dir, sizeof(dir), ".");
  }
  path = malloc(strlen(dir) + stem + 32);
  if (!path) {
    return NULL;
  }
#ifdef _WIN32
  sprintf(path, "%s/%.*s-%08x.obj", dir, (int)stem, base, hash);
#else
  sprintf(path, "%s/%.*s-%08x.o", dir, (int)stem, base, hash);
#endif
  return path;
}

static char *ex_json_beside(const char *object) {
  const char *dot = NULL;
  size_t stem;
  char *path;
  for (const char *p = object; *p; p++) {
    if (*p == '.') {
      dot = p;
    } else if (*p == '/' || *p == '\\') {
      dot = NULL;
    }
  }
  stem = dot ? (size_t)(dot - object) : strlen(object);
  path = malloc(stem + 16);
  if (path) {
    memcpy(path, object, stem);
    strcpy(path + stem, ".explain.json");
  }
  return path;
}

int mettle_explain_view(CompilerOptions *options) {
  char *absolute = mettle_realpath(options->input_filename, NULL);
  char *object = ex_cache_object(options->input_filename, absolute,
                                 options->explain_view_key);
  int headless = options->explain_view_columns > 0;
  int text = options->explain_view_text ||
             (!headless && !ex_term_interactive());
  int result;
  if (!object) {
    free(absolute);
    fprintf(stderr, "Error: out of memory\n");
    return 1;
  }
  options->explain = 1;
  options->optimize = 1;
  options->release = 1;
  options->device_image = 1;
  options->building_executable = 0;
  options->output_filename = object;
  if (text) {
    int fd = ex_fileno(stdout);
    options->explain_view = 0;
    options->explain_view_text = 1;
    options->explain_json = 0;
    ir_explain_set_stream(stdout);
    diag_style_set_color(fd >= 0 && ex_isatty(fd) && diag_style_color());
  } else {
    options->explain_json = 1;
  }
  result = compile_file(options->input_filename, object, options);
  if (result == 0 && !text) {
    char *json = ex_json_beside(object);
    ExReport *report = json ? ex_load_report(json, absolute) : NULL;
    if (report) {
      result = ex_show(report, options);
      ex_report_free(report);
    } else {
      result = 1;
    }
    free(json);
  }
  free(object);
  free(absolute);
  return result;
}
