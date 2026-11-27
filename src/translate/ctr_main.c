#include "translate/ctr_parse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

CtrProgram *ctr_prog;

static void usage(void) {
  fprintf(stderr,
          "usage: mettle translate [options] <file.c>...\n"
          "\n"
          "Translates C99 source into Mettle source.\n"
          "\n"
          "  -o <path>          output file for one input, output directory for several\n"
          "  -I <dir>           add an include directory\n"
          "  -D <name>[=value]  define a macro\n"
          "  -U <name>          undefine a macro\n"
          "  --target <t>       windows-x64, linux-x64, linux-arm64 or macos-arm64\n"
          "  --expand-macros    expand every macro; recover none as const or fn\n"
          "  --no-comments      drop the C comments\n"
          "  --c-include <dir>  the C library headers to use\n"
          "  -E                 print the preprocessed C and stop\n");
}

static int parse_target(const char *s, CtrTarget *out) {
  static const struct {
    const char *name;
    CtrTarget target;
  } names[] = {{"windows-x64", CTR_TARGET_WINDOWS_X64}, {"windows", CTR_TARGET_WINDOWS_X64},
               {"linux-x64", CTR_TARGET_LINUX_X64},     {"linux", CTR_TARGET_LINUX_X64},
               {"linux-arm64", CTR_TARGET_LINUX_ARM64}, {"linux-aarch64", CTR_TARGET_LINUX_ARM64},
               {"macos-arm64", CTR_TARGET_MACOS_ARM64}, {"macos", CTR_TARGET_MACOS_ARM64}};
  for (size_t i = 0; i < sizeof(names) / sizeof(*names); i++) {
    if (strcmp(s, names[i].name) == 0) {
      *out = names[i].target;
      return 1;
    }
  }
  return 0;
}

static CtrTarget host_target(void) {
#if defined(_WIN32)
  return CTR_TARGET_WINDOWS_X64;
#elif defined(__APPLE__)
  return CTR_TARGET_MACOS_ARM64;
#elif defined(__aarch64__)
  return CTR_TARGET_LINUX_ARM64;
#else
  return CTR_TARGET_LINUX_X64;
#endif
}

static const char *take_value(int argc, char **argv, int *i, const char *arg, const char *flag) {
  size_t n = strlen(flag);
  if (strcmp(arg, flag) == 0) {
    if (*i + 1 >= argc) {
      ctr_fatal("'%s' needs a value", flag);
    }
    return argv[++*i];
  }
  if (n == 2 && strncmp(arg, flag, n) == 0 && arg[n]) {
    return arg + n;
  }
  return NULL;
}

static int parse_option(int argc, char **argv, int *i, CtrOptions *opt) {
  const char *arg = argv[*i];
  const char *v;
  if ((v = take_value(argc, argv, i, arg, "-o"))) {
    opt->output = v;
  } else if ((v = take_value(argc, argv, i, arg, "-I"))) {
    ctr_vec_push(&opt->include_dirs, (void *)v);
  } else if ((v = take_value(argc, argv, i, arg, "-D"))) {
    ctr_vec_push(&opt->defines, (void *)v);
  } else if ((v = take_value(argc, argv, i, arg, "-U"))) {
    ctr_vec_push(&opt->defines, ctr_format("-%s", v));
  } else if ((v = take_value(argc, argv, i, arg, "--target"))) {
    if (!parse_target(v, &opt->target)) {
      ctr_fatal("unknown target '%s'", v);
    }
  } else if ((v = take_value(argc, argv, i, arg, "--c-include"))) {
    opt->system_include = v;
  } else if (strcmp(arg, "--expand-macros") == 0) {
    opt->expand_macros = 1;
  } else if (strcmp(arg, "--no-comments") == 0) {
    opt->keep_comments = 0;
  } else if (strcmp(arg, "-E") == 0) {
    opt->emit_preprocessed = 1;
  } else if (strcmp(arg, "-v") == 0) {
    opt->verbose = 1;
  } else if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
    usage();
    exit(0);
  } else if (arg[0] == '-' && arg[1]) {
    ctr_fatal("unknown option '%s'", arg);
  } else {
    ctr_vec_push(&opt->inputs, (void *)arg);
  }
  return 1;
}

static char *exe_dir(const char *argv0) {
  char *self = ctr_strdup(argv0);
  return ctr_path_dir(self);
}

static const char *find_c_include(const char *argv0) {
  const char *env = getenv("METTLE_C_INCLUDE");
  if (env && *env) {
    return env;
  }
  char *dir = exe_dir(argv0);
  const char *candidates[] = {"stdlib/c/include", "../stdlib/c/include"};
  for (size_t i = 0; i < sizeof(candidates) / sizeof(*candidates); i++) {
    char *path = ctr_path_join(dir, candidates[i]);
    if (ctr_file_exists(ctr_path_join(path, "stdio.h"))) {
      return path;
    }
  }
  if (ctr_file_exists("stdlib/c/include/stdio.h")) {
    return "stdlib/c/include";
  }
  return NULL;
}

static void print_tokens(CtrToken *tok) {
  int line = 0;
  for (CtrToken *t = tok; t->kind != TK_EOF; t = t->next) {
    if (t->at_bol && line) {
      fputc('\n', stdout);
    } else if (t->has_space && line) {
      fputc(' ', stdout);
    }
    line = 1;
    fwrite(t->loc, 1, (size_t)t->len, stdout);
  }
  fputc('\n', stdout);
}

static void apply_undefs(CtrOptions *opt) {
  for (int i = 0; i < opt->defines.len; i++) {
    const char *d = opt->defines.items[i];
    if (d[0] == '-') {
      ctr_pp_undef(d + 1);
    }
  }
}

static CtrObj *translate_unit(const char *path, CtrOptions *opt) {
  CtrToken *tok = ctr_tokenize_file(path, 0);
  if (!tok) {
    ctr_fatal("cannot open '%s'", path);
  }
  CtrFile *file = tok->file;
  CtrUnit *unit = ctr_alloc(sizeof(CtrUnit));
  unit->file = file;
  unit->path = path;
  ctr_unit_files = &unit->files;
  ctr_pp_init();
  apply_undefs(opt);
  tok = ctr_preprocess(tok);
  ctr_unit_files = NULL;
  if (opt->emit_preprocessed) {
    print_tokens(tok);
    return NULL;
  }
  unit->globals = ctr_parse_unit(tok, file);
  ctr_vec_push(&ctr_prog->units, unit);
  return unit->globals;
}

CtrMap ctr_libc_mappings;

static const char *libc_headers[] = {"assert.h", "ctype.h",  "errno.h",  "inttypes.h", "locale.h",
                                     "math.h",   "setjmp.h", "signal.h", "stdio.h",    "stdlib.h",
                                     "string.h", "time.h",   "wchar.h"};

static void load_libc_mappings(CtrOptions *opt) {
  CtrBuf src = {0};
  for (size_t i = 0; i < sizeof(libc_headers) / sizeof(*libc_headers); i++) {
    ctr_buf_printf(&src, "#include <%s>\n", libc_headers[i]);
  }
  CtrFile *file = ctr_new_file("<libc>", 0, ctr_buf_take(&src));
  file->is_system = 1;
  ctr_pp_init();
  apply_undefs(opt);
  CtrToken *tok = ctr_preprocess(ctr_tokenize(file));
  for (CtrObj *g = ctr_parse_unit(tok, file); g; g = g->next) {
    if (g->is_function && g->mettle_module) {
      ctr_map_put(&ctr_libc_mappings, g->name, g);
    }
  }
}

int mettle_translate_main(int argc, char **argv) {
  CtrOptions *opt = ctr_alloc(sizeof(CtrOptions));
  opt->target = host_target();
  opt->keep_comments = 1;
  int start = 1;
  if (argc > 1 && strcmp(argv[1], "translate") == 0) {
    start = 2;
  }
  for (int i = start; i < argc; i++) {
    parse_option(argc, argv, &i, opt);
  }
  if (opt->inputs.len == 0) {
    usage();
    return 1;
  }
  if (!opt->system_include) {
    opt->system_include = find_c_include(argv[0]);
  }
  if (!opt->system_include) {
    ctr_fatal("cannot find the C library headers (stdlib/c/include); pass --c-include");
  }
  ctr_prog = ctr_alloc(sizeof(CtrProgram));
  ctr_prog->options = opt;
  ctr_types_init(opt->target);
  if (!opt->emit_preprocessed) {
    load_libc_mappings(opt);
  }
  for (int i = 0; i < opt->inputs.len; i++) {
    translate_unit(opt->inputs.items[i], opt);
    if (ctr_prog->errors) {
      break;
    }
  }
  if (opt->emit_preprocessed) {
    return 0;
  }
  if (ctr_prog->errors) {
    fprintf(stderr, "mettle translate: %d error%s\n", ctr_prog->errors, ctr_prog->errors == 1 ? "" : "s");
    return 1;
  }
  ctr_emit_program(ctr_prog);
  return ctr_prog->errors ? 1 : 0;
}
