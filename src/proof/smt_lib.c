#include "smt_internal.h"
#include "smt_sat.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Sx {
  int is_list;
  char *atom;
  struct Sx **items;
  unsigned count;
  int line;
} Sx;

typedef struct {
  const char *src;
  size_t pos;
  int line;
  const char *error;
} SxReader;

typedef struct {
  char *name;
  SmtTerm term;
} Binding;

typedef struct {
  char *name;
  unsigned nparams;
  SmtTerm *params;
  SmtTerm body;
} Macro;

typedef struct {
  SmtCtx *ctx;
  Binding *binds;
  unsigned bind_count;
  unsigned bind_cap;
  Macro *macros;
  unsigned macro_count;
  unsigned macro_cap;
  SmtTerm *asserts;
  unsigned assert_count;
  unsigned assert_cap;
  unsigned *frames;
  unsigned frame_count;
  unsigned frame_cap;
  FILE *out;
  int certify;
  int show_stats;
  SmtSolver *last;
  const char *path;
  int line;
  char error[512];
  int failed;
  unsigned last_fp_width;
  SmtTerm *fp_terms;
  unsigned fp_count;
  unsigned fp_cap;
  SmtTerm rne;
} SmtLib;

static int g_smtlib_normalize = 1;

static void sx_free(Sx *s) {
  if (!s) {
    return;
  }
  free(s->atom);
  for (unsigned i = 0; i < s->count; i++) {
    sx_free(s->items[i]);
  }
  free(s->items);
  free(s);
}

static void sx_skip(SxReader *r) {
  for (;;) {
    char c = r->src[r->pos];
    if (c == '\n') {
      r->line++;
      r->pos++;
    } else if (c == ' ' || c == '\t' || c == '\r') {
      r->pos++;
    } else if (c == ';') {
      while (r->src[r->pos] && r->src[r->pos] != '\n') {
        r->pos++;
      }
    } else {
      return;
    }
  }
}

static Sx *sx_read(SxReader *r) {
  sx_skip(r);
  char c = r->src[r->pos];
  if (!c) {
    return NULL;
  }
  Sx *s = calloc(1, sizeof(Sx));
  s->line = r->line;
  if (c == '(') {
    r->pos++;
    s->is_list = 1;
    unsigned cap = 0;
    for (;;) {
      sx_skip(r);
      if (r->src[r->pos] == ')') {
        r->pos++;
        return s;
      }
      if (!r->src[r->pos]) {
        r->error = "unbalanced parentheses";
        sx_free(s);
        return NULL;
      }
      Sx *item = sx_read(r);
      if (!item) {
        sx_free(s);
        return NULL;
      }
      if (s->count == cap) {
        cap = cap ? cap * 2u : 4u;
        s->items = realloc(s->items, cap * sizeof(Sx *));
      }
      s->items[s->count++] = item;
    }
  }
  if (c == ')') {
    r->error = "unexpected ')'";
    free(s);
    return NULL;
  }
  size_t start = r->pos;
  if (c == '|') {
    r->pos++;
    while (r->src[r->pos] && r->src[r->pos] != '|') {
      r->pos++;
    }
    r->pos++;
    size_t len = r->pos - start - 2u;
    s->atom = malloc(len + 1u);
    memcpy(s->atom, r->src + start + 1u, len);
    s->atom[len] = 0;
    return s;
  }
  if (c == '"') {
    r->pos++;
    while (r->src[r->pos] && r->src[r->pos] != '"') {
      r->pos++;
    }
    r->pos++;
  } else {
    while (r->src[r->pos] && !isspace((unsigned char)r->src[r->pos]) &&
           r->src[r->pos] != '(' && r->src[r->pos] != ')' &&
           r->src[r->pos] != ';') {
      r->pos++;
    }
  }
  size_t len = r->pos - start;
  s->atom = malloc(len + 1u);
  memcpy(s->atom, r->src + start, len);
  s->atom[len] = 0;
  return s;
}

static void lib_error(SmtLib *lib, const Sx *at, const char *fmt,
                      const char *detail) {
  if (lib->failed) {
    return;
  }
  lib->failed = 1;
  char msg[384];
  snprintf(msg, sizeof(msg), fmt, detail ? detail : "");
  snprintf(lib->error, sizeof(lib->error), "%s:%d: %s", lib->path,
           at ? at->line : lib->line, msg);
}

static int is_atom(const Sx *s, const char *text) {
  return s && !s->is_list && strcmp(s->atom, text) == 0;
}

static int parse_uint(const Sx *s, unsigned *out) {
  if (!s || s->is_list) {
    return 0;
  }
  char *end = NULL;
  unsigned long long v = strtoull(s->atom, &end, 10);
  if (!end || *end || v > 0xffffffffull) {
    return 0;
  }
  *out = (unsigned)v;
  return 1;
}

static int fp_format_of_width(unsigned w, unsigned *eb, unsigned *sb) {
  switch (w) {
  case 16:
    *eb = 5;
    *sb = 11;
    return 1;
  case 32:
    *eb = 8;
    *sb = 24;
    return 1;
  case 64:
    *eb = 11;
    *sb = 53;
    return 1;
  default:
    return 0;
  }
}

static int fp_width_of(unsigned eb, unsigned sb) {
  unsigned e, s;
  return fp_format_of_width(eb + sb, &e, &s) && e == eb && s == sb
             ? (int)(eb + sb)
             : 0;
}

static int fp_is_term(SmtLib *lib, SmtTerm t) {
  for (unsigned i = 0; i < lib->fp_count; i++) {
    if (lib->fp_terms[i] == t) {
      return 1;
    }
  }
  return 0;
}

static SmtTerm fp_mark(SmtLib *lib, SmtTerm t) {
  if (!t || fp_is_term(lib, t)) {
    return t;
  }
  if (lib->fp_count == lib->fp_cap) {
    lib->fp_cap = lib->fp_cap ? lib->fp_cap * 2u : 64u;
    lib->fp_terms = realloc(lib->fp_terms, lib->fp_cap * sizeof(SmtTerm));
  }
  lib->fp_terms[lib->fp_count++] = t;
  return t;
}

static SmtTerm fp_nan_const(SmtCtx *ctx, unsigned eb, unsigned sb) {
  uint64_t bits = (((1ull << eb) - 1u) << (sb - 1u)) | (1ull << (sb - 2u));
  return smt_bv_u64(ctx, eb + sb, bits);
}

static SmtTerm fp_canon(SmtLib *lib, SmtTerm t) {
  SmtCtx *ctx = lib->ctx;
  unsigned eb, sb;
  if (!fp_format_of_width(smt_width(ctx, t), &eb, &sb)) {
    return t;
  }
  return fp_mark(lib, smt_ite(ctx, smt_fp_is_nan(ctx, eb, sb, t),
                              fp_nan_const(ctx, eb, sb), t));
}

static SmtSort parse_sort(SmtLib *lib, const Sx *s) {
  SmtCtx *ctx = lib->ctx;
  lib->last_fp_width = 0;
  if (is_atom(s, "Bool")) {
    return smt_bool_sort(ctx);
  }
  static const struct {
    const char *name;
    unsigned width;
  } fp_sorts[] = {{"Float16", 16}, {"Float32", 32}, {"Float64", 64}};
  for (size_t i = 0; s && !s->is_list && i < 3; i++) {
    if (strcmp(s->atom, fp_sorts[i].name) == 0) {
      lib->last_fp_width = fp_sorts[i].width;
      return smt_bv_sort(ctx, fp_sorts[i].width);
    }
  }
  if (s && s->is_list && s->count == 4 && is_atom(s->items[0], "_") &&
      is_atom(s->items[1], "FloatingPoint")) {
    unsigned eb, sb;
    if (parse_uint(s->items[2], &eb) && parse_uint(s->items[3], &sb) &&
        fp_width_of(eb, sb)) {
      lib->last_fp_width = eb + sb;
      return smt_bv_sort(ctx, eb + sb);
    }
    lib_error(lib, s, "only Float16, Float32 and Float64 are supported", NULL);
    return 0;
  }
  if (s && s->is_list && s->count == 3 && is_atom(s->items[0], "_") &&
      is_atom(s->items[1], "BitVec")) {
    unsigned w;
    if (parse_uint(s->items[2], &w) && w >= 1u && w <= SMT_MAX_WIDTH) {
      return smt_bv_sort(ctx, w);
    }
  }
  if (s && s->is_list && s->count == 3 && is_atom(s->items[0], "Array")) {
    SmtSort i = parse_sort(lib, s->items[1]);
    SmtSort e = parse_sort(lib, s->items[2]);
    if (i && e) {
      return smt_array_sort(ctx, i, e);
    }
  }
  lib_error(lib, s, "unsupported sort", NULL);
  return 0;
}

static SmtTerm lookup_binding(SmtLib *lib, const char *name) {
  for (unsigned i = lib->bind_count; i-- > 0;) {
    if (strcmp(lib->binds[i].name, name) == 0) {
      return lib->binds[i].term;
    }
  }
  return 0;
}

static void push_binding(SmtLib *lib, const char *name, SmtTerm t) {
  if (lib->bind_count == lib->bind_cap) {
    lib->bind_cap = lib->bind_cap ? lib->bind_cap * 2u : 64u;
    lib->binds = realloc(lib->binds, lib->bind_cap * sizeof(Binding));
  }
  size_t len = strlen(name);
  lib->binds[lib->bind_count].name = malloc(len + 1u);
  memcpy(lib->binds[lib->bind_count].name, name, len + 1u);
  lib->binds[lib->bind_count].term = t;
  lib->bind_count++;
}

static void pop_bindings(SmtLib *lib, unsigned to) {
  while (lib->bind_count > to) {
    free(lib->binds[--lib->bind_count].name);
  }
}

static Macro *find_macro(SmtLib *lib, const char *name) {
  for (unsigned i = lib->macro_count; i-- > 0;) {
    if (strcmp(lib->macros[i].name, name) == 0) {
      return &lib->macros[i];
    }
  }
  return NULL;
}

static SmtTerm parse_const(SmtLib *lib, const Sx *s) {
  SmtCtx *ctx = lib->ctx;
  const char *a = s->atom;
  uint64_t words[SMT_WORDS] = {0, 0, 0, 0};
  unsigned width = 0;
  if (a[0] == '#' && a[1] == 'b') {
    size_t n = strlen(a + 2);
    if (n == 0 || n > SMT_MAX_WIDTH) {
      lib_error(lib, s, "bad binary literal", NULL);
      return 0;
    }
    for (size_t i = 0; i < n; i++) {
      char c = a[2 + n - 1 - i];
      if (c == '1') {
        words[i / 64u] |= ((uint64_t)1) << (i % 64u);
      } else if (c != '0') {
        lib_error(lib, s, "bad binary literal", NULL);
        return 0;
      }
    }
    width = (unsigned)n;
  } else if (a[0] == '#' && a[1] == 'x') {
    size_t n = strlen(a + 2);
    if (n == 0 || n * 4u > SMT_MAX_WIDTH) {
      lib_error(lib, s, "bad hex literal", NULL);
      return 0;
    }
    for (size_t i = 0; i < n; i++) {
      char c = (char)tolower((unsigned char)a[2 + n - 1 - i]);
      unsigned v;
      if (c >= '0' && c <= '9') {
        v = (unsigned)(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        v = (unsigned)(c - 'a' + 10);
      } else {
        lib_error(lib, s, "bad hex literal", NULL);
        return 0;
      }
      words[(i * 4u) / 64u] |= ((uint64_t)v) << ((i * 4u) % 64u);
    }
    width = (unsigned)n * 4u;
  } else {
    return 0;
  }
  return smt_bv_words(ctx, width, words);
}

static SmtTerm parse_term(SmtLib *lib, const Sx *s);

static SmtTerm decimal_bv(SmtLib *lib, const Sx *s, const char *digits,
                          unsigned width) {
  SmtBits acc, ten, digit;
  smt_bits_zero(&acc);
  smt_bits_from_u64(&ten, 10, SMT_MAX_WIDTH);
  for (const char *p = digits; *p; p++) {
    if (!isdigit((unsigned char)*p)) {
      lib_error(lib, s, "bad decimal bit-vector literal", NULL);
      return 0;
    }
    smt_bits_mul(&acc, &acc, &ten, SMT_MAX_WIDTH);
    smt_bits_from_u64(&digit, (uint64_t)(*p - '0'), SMT_MAX_WIDTH);
    smt_bits_add(&acc, &acc, &digit, SMT_MAX_WIDTH);
  }
  return smt_bv_words(lib->ctx, width, acc.w);
}

static SmtKind binary_kind(const char *op) {
  static const struct {
    const char *name;
    SmtKind kind;
  } table[] = {
      {"bvand", SMT_K_BVAND},   {"bvor", SMT_K_BVOR},
      {"bvxor", SMT_K_BVXOR},   {"bvadd", SMT_K_BVADD},
      {"bvsub", SMT_K_BVSUB},   {"bvmul", SMT_K_BVMUL},
      {"bvudiv", SMT_K_BVUDIV}, {"bvurem", SMT_K_BVUREM},
      {"bvsdiv", SMT_K_BVSDIV}, {"bvsrem", SMT_K_BVSREM},
      {"bvsmod", SMT_K_BVSMOD}, {"bvshl", SMT_K_BVSHL},
      {"bvlshr", SMT_K_BVLSHR}, {"bvashr", SMT_K_BVASHR},
      {"concat", SMT_K_CONCAT}, {"bvult", SMT_K_BVULT},
      {"bvule", SMT_K_BVULE},   {"bvslt", SMT_K_BVSLT},
      {"bvsle", SMT_K_BVSLE},
  };
  for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
    if (strcmp(table[i].name, op) == 0) {
      return table[i].kind;
    }
  }
  return SMT_K_INVALID;
}

static SmtTerm rotate(SmtCtx *ctx, SmtTerm a, unsigned k, int left) {
  unsigned w = smt_width(ctx, a);
  k %= w;
  if (k == 0) {
    return a;
  }
  if (!left) {
    k = w - k;
  }
  SmtTerm hi = smt_extract(ctx, a, w - k - 1u, 0);
  SmtTerm lo = smt_extract(ctx, a, w - 1u, w - k);
  return smt_mk2(ctx, SMT_K_CONCAT, hi, lo);
}

static SmtTerm parse_indexed(SmtLib *lib, const Sx *head, const Sx *s,
                             SmtTerm *args, unsigned nargs) {
  SmtCtx *ctx = lib->ctx;
  const char *op = head->items[1]->is_list ? "" : head->items[1]->atom;
  unsigned i0 = 0, i1 = 0;
  int has0 = head->count > 2 && parse_uint(head->items[2], &i0);
  int has1 = head->count > 3 && parse_uint(head->items[3], &i1);
  if (nargs != 1) {
    lib_error(lib, s, "indexed operator %s takes one argument", op);
    return 0;
  }
  SmtTerm a = args[0];
  if (!smt_sort_is_bv(ctx, smt_term_sort(ctx, a))) {
    lib_error(lib, s, "indexed operator %s needs a bit-vector", op);
    return 0;
  }
  unsigned w = smt_width(ctx, a);
  if (strcmp(op, "extract") == 0 && has0 && has1 && i0 >= i1 && i0 < w) {
    return smt_extract(ctx, a, i0, i1);
  }
  if (strcmp(op, "zero_extend") == 0 && has0) {
    return smt_zext(ctx, a, i0);
  }
  if (strcmp(op, "sign_extend") == 0 && has0) {
    return smt_sext(ctx, a, i0);
  }
  if (strcmp(op, "rotate_left") == 0 && has0) {
    return rotate(ctx, a, i0, 1);
  }
  if (strcmp(op, "rotate_right") == 0 && has0) {
    return rotate(ctx, a, i0, 0);
  }
  if (strcmp(op, "repeat") == 0 && has0 && i0 >= 1u &&
      i0 * w <= SMT_MAX_WIDTH) {
    SmtTerm out = a;
    for (unsigned k = 1; k < i0; k++) {
      out = smt_mk2(ctx, SMT_K_CONCAT, out, a);
    }
    return out;
  }
  lib_error(lib, s, "unsupported indexed operator %s", op);
  return 0;
}

static int fp_operands(SmtLib *lib, const Sx *s, SmtTerm *args, unsigned n,
                       unsigned *eb, unsigned *sb) {
  SmtCtx *ctx = lib->ctx;
  unsigned w = 0;
  for (unsigned i = 0; i < n; i++) {
    if (args[i] == lib->rne) {
      continue;
    }
    if (!smt_sort_is_bv(ctx, smt_term_sort(ctx, args[i]))) {
      lib_error(lib, s, "a floating-point operand is not a float", NULL);
      return 0;
    }
    unsigned aw = smt_width(ctx, args[i]);
    if (w && aw != w) {
      lib_error(lib, s, "floating-point operands of two formats", NULL);
      return 0;
    }
    w = aw;
  }
  if (!fp_format_of_width(w, eb, sb)) {
    lib_error(lib, s, "only Float16, Float32 and Float64 are supported", NULL);
    return 0;
  }
  return 1;
}

static SmtTerm fp_app(SmtLib *lib, const Sx *s, const char *op, SmtTerm *args,
                      unsigned n) {
  SmtCtx *ctx = lib->ctx;
  unsigned eb, sb;
  if (strcmp(op, "fp") == 0) {
    if (n != 3) {
      lib_error(lib, s, "fp takes a sign, an exponent and a significand", NULL);
      return 0;
    }
    SmtTerm bits =
        smt_mk2(ctx, SMT_K_CONCAT, smt_mk2(ctx, SMT_K_CONCAT, args[0], args[1]),
                args[2]);
    if (!fp_format_of_width(smt_width(ctx, bits), &eb, &sb) ||
        smt_width(ctx, args[1]) != eb) {
      lib_error(lib, s, "only Float16, Float32 and Float64 are supported",
                NULL);
      return 0;
    }
    return fp_canon(lib, bits);
  }
  int rounded = strcmp(op, "fp.add") == 0 || strcmp(op, "fp.sub") == 0 ||
                strcmp(op, "fp.mul") == 0 || strcmp(op, "fp.div") == 0 ||
                strcmp(op, "fp.sqrt") == 0;
  if (rounded && (n < 2 || args[0] != lib->rne)) {
    lib_error(lib, s, "%s needs the rounding mode RNE first", op);
    return 0;
  }
  if (!fp_operands(lib, s, args, n, &eb, &sb)) {
    return 0;
  }
  SmtTerm nan = fp_nan_const(ctx, eb, sb);
  if (rounded && strcmp(op, "fp.sqrt") == 0 && n == 2) {
    return fp_mark(lib, smt_fp_sqrt(ctx, eb, sb, args[1], nan));
  }
  if (rounded && n == 3) {
    SmtTerm x = args[1], y = args[2];
    SmtTerm r = op[3] == 'a'   ? smt_fp_add(ctx, eb, sb, x, y, nan)
                : op[3] == 's' ? smt_fp_sub(ctx, eb, sb, x, y, nan)
                : op[3] == 'm' ? smt_fp_mul(ctx, eb, sb, x, y, nan)
                               : smt_fp_div(ctx, eb, sb, x, y, nan);
    return fp_mark(lib, r);
  }
  if (n == 1) {
    SmtTerm x = args[0];
    unsigned w = eb + sb;
    SmtTerm sign =
        smt_eq(ctx, smt_extract(ctx, x, w - 1u, w - 1u), smt_bv_u64(ctx, 1, 1));
    SmtTerm magnitude = smt_extract(ctx, x, w - 2u, 0);
    SmtTerm exponent = smt_extract(ctx, x, w - 2u, sb - 1u);
    SmtTerm ones = smt_bv_u64(ctx, eb, (1ull << eb) - 1u);
    SmtTerm nan_x = smt_fp_is_nan(ctx, eb, sb, x);
    if (strcmp(op, "fp.abs") == 0) {
      return fp_mark(
          lib, smt_mk2(ctx, SMT_K_CONCAT, smt_bv_u64(ctx, 1, 0), magnitude));
    }
    if (strcmp(op, "fp.neg") == 0) {
      return fp_canon(lib, smt_fp_neg(ctx, eb, sb, x));
    }
    if (strcmp(op, "fp.isNaN") == 0) {
      return nan_x;
    }
    if (strcmp(op, "fp.isInfinite") == 0) {
      return smt_and(ctx, smt_eq(ctx, exponent, ones), smt_not(ctx, nan_x));
    }
    if (strcmp(op, "fp.isZero") == 0) {
      return smt_eq(ctx, magnitude, smt_bv_u64(ctx, w - 1u, 0));
    }
    if (strcmp(op, "fp.isNormal") == 0) {
      return smt_and(
          ctx, smt_not(ctx, smt_eq(ctx, exponent, ones)),
          smt_not(ctx, smt_eq(ctx, exponent, smt_bv_u64(ctx, eb, 0))));
    }
    if (strcmp(op, "fp.isSubnormal") == 0) {
      return smt_and(
          ctx, smt_eq(ctx, exponent, smt_bv_u64(ctx, eb, 0)),
          smt_not(ctx, smt_eq(ctx, magnitude, smt_bv_u64(ctx, w - 1u, 0))));
    }
    if (strcmp(op, "fp.isNegative") == 0) {
      return smt_and(ctx, sign, smt_not(ctx, nan_x));
    }
    if (strcmp(op, "fp.isPositive") == 0) {
      return smt_and(ctx, smt_not(ctx, sign), smt_not(ctx, nan_x));
    }
  }
  if (n >= 2) {
    int which = strcmp(op, "fp.eq") == 0    ? 1
                : strcmp(op, "fp.lt") == 0  ? 2
                : strcmp(op, "fp.leq") == 0 ? 3
                : strcmp(op, "fp.gt") == 0  ? 4
                : strcmp(op, "fp.geq") == 0 ? 5
                                            : 0;
    if (which) {
      SmtTerm out = ctx->bool_true;
      for (unsigned i = 0; i + 1u < n; i++) {
        SmtTerm x = args[i], y = args[i + 1u];
        SmtTerm c = which == 1   ? smt_fp_eq(ctx, eb, sb, x, y)
                    : which == 2 ? smt_fp_lt(ctx, eb, sb, x, y)
                    : which == 3 ? smt_fp_le(ctx, eb, sb, x, y)
                    : which == 4 ? smt_fp_lt(ctx, eb, sb, y, x)
                                 : smt_fp_le(ctx, eb, sb, y, x);
        out = smt_and(ctx, out, c);
      }
      return out;
    }
  }
  lib_error(lib, s, "unsupported floating-point operator %s", op);
  return 0;
}

static SmtTerm fp_to_fp(SmtLib *lib, const Sx *head, const Sx *s, SmtTerm *args,
                        unsigned n) {
  SmtCtx *ctx = lib->ctx;
  unsigned eb, sb;
  if (!parse_uint(head->items[2], &eb) || !parse_uint(head->items[3], &sb) ||
      !fp_width_of(eb, sb)) {
    lib_error(lib, s, "only Float16, Float32 and Float64 are supported", NULL);
    return 0;
  }
  int is_unsigned = is_atom(head->items[1], "to_fp_unsigned");
  if (n == 1 && !is_unsigned) {
    if (smt_width(ctx, args[0]) != eb + sb) {
      lib_error(lib, s, "to_fp of a bit-vector needs the format's width", NULL);
      return 0;
    }
    return fp_canon(lib, args[0]);
  }
  if (n != 2 || args[0] != lib->rne ||
      !smt_sort_is_bv(ctx, smt_term_sort(ctx, args[1]))) {
    lib_error(lib, s, "to_fp needs the rounding mode RNE and a value", NULL);
    return 0;
  }
  SmtTerm x = args[1];
  unsigned from_eb, from_sb;
  if (!is_unsigned && fp_is_term(lib, x) &&
      fp_format_of_width(smt_width(ctx, x), &from_eb, &from_sb)) {
    if (from_eb == eb && from_sb == sb) {
      return x;
    }
    return fp_mark(lib, smt_fp_convert(ctx, from_eb, from_sb, eb, sb, x,
                                       fp_nan_const(ctx, eb, sb)));
  }
  return fp_mark(lib, smt_fp_from_int(ctx, eb, sb, x, !is_unsigned));
}

static SmtTerm fold_chain(SmtLib *lib, const Sx *s, SmtKind kind, SmtTerm *args,
                          unsigned n) {
  SmtCtx *ctx = lib->ctx;
  if (n < 2) {
    lib_error(lib, s, "operator needs two or more arguments", NULL);
    return 0;
  }
  SmtTerm out = ctx->bool_true;
  for (unsigned i = 0; i + 1u < n; i++) {
    out = smt_and(ctx, out, smt_mk2(ctx, kind, args[i], args[i + 1u]));
  }
  return out;
}

static SmtTerm parse_app(SmtLib *lib, const Sx *s) {
  SmtCtx *ctx = lib->ctx;
  const Sx *head = s->items[0];
  if (is_atom(head, "let")) {
    if (s->count != 3 || !s->items[1]->is_list) {
      lib_error(lib, s, "malformed let", NULL);
      return 0;
    }
    const Sx *bindings = s->items[1];
    SmtTerm values[256];
    if (bindings->count > 256u) {
      lib_error(lib, s, "let with more than 256 bindings", NULL);
      return 0;
    }
    for (unsigned i = 0; i < bindings->count; i++) {
      const Sx *b = bindings->items[i];
      if (!b->is_list || b->count != 2 || b->items[0]->is_list) {
        lib_error(lib, b, "malformed let binding", NULL);
        return 0;
      }
      values[i] = parse_term(lib, b->items[1]);
      if (!values[i]) {
        return 0;
      }
    }
    unsigned mark = lib->bind_count;
    for (unsigned i = 0; i < bindings->count; i++) {
      push_binding(lib, bindings->items[i]->items[0]->atom, values[i]);
    }
    SmtTerm body = parse_term(lib, s->items[2]);
    pop_bindings(lib, mark);
    return body;
  }
  if (is_atom(head, "forall") || is_atom(head, "exists")) {
    if (s->count != 3 || !s->items[1]->is_list) {
      lib_error(lib, s, "malformed quantifier", NULL);
      return 0;
    }
    const Sx *vars = s->items[1];
    unsigned mark = lib->bind_count;
    SmtTerm bound[64];
    if (vars->count > 64u) {
      lib_error(lib, s, "too many bound variables", NULL);
      return 0;
    }
    for (unsigned i = 0; i < vars->count; i++) {
      const Sx *v = vars->items[i];
      if (!v->is_list || v->count != 2 || v->items[0]->is_list) {
        lib_error(lib, v, "malformed bound variable", NULL);
        return 0;
      }
      SmtSort sort = parse_sort(lib, v->items[1]);
      if (!sort) {
        return 0;
      }
      bound[i] = smt_fresh(ctx, v->items[0]->atom, sort);
      push_binding(lib, v->items[0]->atom, bound[i]);
    }
    SmtTerm body = parse_term(lib, s->items[2]);
    pop_bindings(lib, mark);
    if (!body) {
      return 0;
    }
    for (unsigned i = vars->count; i-- > 0;) {
      body = is_atom(head, "forall") ? smt_forall(ctx, bound[i], body)
                                     : smt_exists(ctx, bound[i], body);
    }
    return body;
  }
  if (is_atom(head, "!")) {
    return s->count >= 2 ? parse_term(lib, s->items[1]) : 0;
  }
  SmtTerm stack_args[16];
  unsigned n = s->count - 1u;
  SmtTerm *args = n <= 16u ? stack_args : malloc(n * sizeof(SmtTerm));
  for (unsigned i = 0; i < n; i++) {
    args[i] = parse_term(lib, s->items[i + 1u]);
    if (!args[i]) {
      if (args != stack_args) {
        free(args);
      }
      return 0;
    }
  }
  SmtTerm out = 0;
  if (head->is_list && head->count == 4 && is_atom(head->items[0], "_") &&
      (is_atom(head->items[1], "to_fp") ||
       is_atom(head->items[1], "to_fp_unsigned"))) {
    out = fp_to_fp(lib, head, s, args, n);
    goto done;
  }
  if (!head->is_list && strncmp(head->atom, "fp", 2) == 0 &&
      (head->atom[2] == '\0' || head->atom[2] == '.')) {
    out = fp_app(lib, s, head->atom, args, n);
    goto done;
  }
  if (head->is_list) {
    if (head->count >= 3 && is_atom(head->items[0], "_")) {
      out = parse_indexed(lib, head, s, args, n);
    } else {
      lib_error(lib, s, "unsupported operator form", NULL);
    }
    goto done;
  }
  const char *op = head->atom;
  Macro *macro = find_macro(lib, op);
  if (macro) {
    if (macro->nparams != n) {
      lib_error(lib, s, "wrong number of arguments to %s", op);
      goto done;
    }
    out = smt_subst(ctx, macro->body, n, macro->params, args);
    goto done;
  }
  SmtFunc func = smt_func_lookup(ctx, op);
  if (func) {
    out = smt_apply(ctx, func, n, args);
    goto done;
  }
  if (strcmp(op, "not") == 0 && n == 1) {
    out = smt_not(ctx, args[0]);
  } else if (strcmp(op, "and") == 0 || strcmp(op, "or") == 0) {
    int is_and = op[0] == 'a';
    out = is_and ? ctx->bool_true : ctx->bool_false;
    for (unsigned i = 0; i < n; i++) {
      out = is_and ? smt_and(ctx, out, args[i]) : smt_or(ctx, out, args[i]);
    }
  } else if (strcmp(op, "xor") == 0 && n >= 2) {
    out = args[0];
    for (unsigned i = 1; i < n; i++) {
      out = smt_xor(ctx, out, args[i]);
    }
  } else if (strcmp(op, "=>") == 0 && n >= 2) {
    out = args[n - 1u];
    for (unsigned i = n - 1u; i-- > 0;) {
      out = smt_implies(ctx, args[i], out);
    }
  } else if (strcmp(op, "=") == 0) {
    out = fold_chain(lib, s, SMT_K_EQ, args, n);
  } else if (strcmp(op, "distinct") == 0) {
    out = ctx->bool_true;
    for (unsigned i = 0; i < n; i++) {
      for (unsigned j = i + 1u; j < n; j++) {
        out = smt_and(ctx, out, smt_not(ctx, smt_eq(ctx, args[i], args[j])));
      }
    }
  } else if (strcmp(op, "ite") == 0 && n == 3) {
    out = smt_ite(ctx, args[0], args[1], args[2]);
  } else if (strcmp(op, "bvnot") == 0 && n == 1) {
    out = smt_mk1(ctx, SMT_K_BVNOT, args[0]);
  } else if (strcmp(op, "bvneg") == 0 && n == 1) {
    out = smt_mk1(ctx, SMT_K_BVNEG, args[0]);
  } else if (strcmp(op, "bvnand") == 0 && n == 2) {
    out =
        smt_mk1(ctx, SMT_K_BVNOT, smt_mk2(ctx, SMT_K_BVAND, args[0], args[1]));
  } else if (strcmp(op, "bvnor") == 0 && n == 2) {
    out = smt_mk1(ctx, SMT_K_BVNOT, smt_mk2(ctx, SMT_K_BVOR, args[0], args[1]));
  } else if (strcmp(op, "bvxnor") == 0 && n == 2) {
    out =
        smt_mk1(ctx, SMT_K_BVNOT, smt_mk2(ctx, SMT_K_BVXOR, args[0], args[1]));
  } else if (strcmp(op, "bvcomp") == 0 && n == 2) {
    out = smt_ite(ctx, smt_eq(ctx, args[0], args[1]), smt_bv_u64(ctx, 1, 1),
                  smt_bv_u64(ctx, 1, 0));
  } else if (strcmp(op, "bvugt") == 0 && n == 2) {
    out = smt_mk2(ctx, SMT_K_BVULT, args[1], args[0]);
  } else if (strcmp(op, "bvuge") == 0 && n == 2) {
    out = smt_mk2(ctx, SMT_K_BVULE, args[1], args[0]);
  } else if (strcmp(op, "bvsgt") == 0 && n == 2) {
    out = smt_mk2(ctx, SMT_K_BVSLT, args[1], args[0]);
  } else if (strcmp(op, "bvsge") == 0 && n == 2) {
    out = smt_mk2(ctx, SMT_K_BVSLE, args[1], args[0]);
  } else if (strcmp(op, "select") == 0 && n == 2) {
    out = smt_select(ctx, args[0], args[1]);
  } else if (strcmp(op, "store") == 0 && n == 3) {
    out = smt_store(ctx, args[0], args[1], args[2]);
  } else {
    SmtKind k = binary_kind(op);
    if (k == SMT_K_INVALID || n < 2) {
      lib_error(lib, s, "unsupported operator %s", op);
      goto done;
    }
    int chain = k == SMT_K_BVULT || k == SMT_K_BVULE || k == SMT_K_BVSLT ||
                k == SMT_K_BVSLE;
    if (chain) {
      out = fold_chain(lib, s, k, args, n);
    } else {
      out = args[0];
      for (unsigned i = 1; i < n; i++) {
        out = smt_mk2(ctx, k, out, args[i]);
      }
    }
  }
done:
  if (args != stack_args) {
    free(args);
  }
  const char *err;
  if (!lib->failed && smt_ctx_error(ctx, &err)) {
    lib_error(lib, s, "%s", err);
    smt_ctx_clear_error(ctx);
    return 0;
  }
  return out;
}

static SmtTerm parse_term(SmtLib *lib, const Sx *s) {
  SmtCtx *ctx = lib->ctx;
  if (lib->failed) {
    return 0;
  }
  if (!s->is_list) {
    SmtTerm b = lookup_binding(lib, s->atom);
    if (b) {
      return b;
    }
    if (strcmp(s->atom, "true") == 0) {
      return ctx->bool_true;
    }
    if (strcmp(s->atom, "false") == 0) {
      return ctx->bool_false;
    }
    if (strcmp(s->atom, "RNE") == 0 ||
        strcmp(s->atom, "roundNearestTiesToEven") == 0) {
      if (!lib->rne) {
        lib->rne = smt_var(ctx, "\x01RNE", smt_bool_sort(ctx));
      }
      return lib->rne;
    }
    if (strcmp(s->atom, "RNA") == 0 || strcmp(s->atom, "RTP") == 0 ||
        strcmp(s->atom, "RTN") == 0 || strcmp(s->atom, "RTZ") == 0 ||
        strncmp(s->atom, "roundToward", 11) == 0 ||
        strcmp(s->atom, "roundNearestTiesToAway") == 0) {
      lib_error(lib, s, "only the rounding mode RNE is supported", NULL);
      return 0;
    }
    if (s->atom[0] == '#') {
      return parse_const(lib, s);
    }
    Macro *m = find_macro(lib, s->atom);
    if (m && m->nparams == 0) {
      return m->body;
    }
    SmtFunc f = smt_func_lookup(ctx, s->atom);
    if (f && smt_func_arity(ctx, f) == 0) {
      return smt_apply(ctx, f, 0, NULL);
    }
    lib_error(lib, s, "unknown symbol %s", s->atom);
    return 0;
  }
  if (s->count == 0) {
    lib_error(lib, s, "empty application", NULL);
    return 0;
  }
  if (s->count == 4 && is_atom(s->items[0], "_") && !s->items[1]->is_list) {
    const char *name = s->items[1]->atom;
    unsigned eb, sb;
    int known = strcmp(name, "+zero") == 0 || strcmp(name, "-zero") == 0 ||
                strcmp(name, "+oo") == 0 || strcmp(name, "-oo") == 0 ||
                strcmp(name, "NaN") == 0;
    if (known) {
      if (!parse_uint(s->items[2], &eb) || !parse_uint(s->items[3], &sb) ||
          !fp_width_of(eb, sb)) {
        lib_error(lib, s, "only Float16, Float32 and Float64 are supported",
                  NULL);
        return 0;
      }
      if (name[0] == 'N') {
        return fp_mark(lib, fp_nan_const(ctx, eb, sb));
      }
      uint64_t bits = name[1] == 'o' ? ((1ull << eb) - 1u) << (sb - 1u) : 0;
      if (name[0] == '-') {
        bits |= 1ull << (eb + sb - 1u);
      }
      return fp_mark(lib, smt_bv_u64(ctx, eb + sb, bits));
    }
  }
  if (s->count == 3 && is_atom(s->items[0], "_") && !s->items[1]->is_list &&
      strncmp(s->items[1]->atom, "bv", 2) == 0) {
    unsigned w;
    if (!parse_uint(s->items[2], &w) || w == 0 || w > SMT_MAX_WIDTH) {
      lib_error(lib, s, "bad bit-vector width", NULL);
      return 0;
    }
    return decimal_bv(lib, s, s->items[1]->atom + 2, w);
  }
  return parse_app(lib, s);
}

static void lib_assert(SmtLib *lib, SmtTerm t) {
  if (lib->assert_count == lib->assert_cap) {
    lib->assert_cap = lib->assert_cap ? lib->assert_cap * 2u : 64u;
    lib->asserts = realloc(lib->asserts, lib->assert_cap * sizeof(SmtTerm));
  }
  lib->asserts[lib->assert_count++] = t;
}

static void declare(SmtLib *lib, const Sx *s, const char *name,
                    const Sx *domain, const Sx *range) {
  SmtCtx *ctx = lib->ctx;
  SmtSort r = parse_sort(lib, range);
  if (!r) {
    return;
  }
  unsigned n = domain ? domain->count : 0u;
  SmtSort dom[64];
  if (n > 64u) {
    lib_error(lib, s, "function arity above 64", NULL);
    return;
  }
  for (unsigned i = 0; i < n; i++) {
    dom[i] = parse_sort(lib, domain->items[i]);
    if (!dom[i]) {
      return;
    }
  }
  if (n == 0) {
    SmtTerm v = smt_var(ctx, name, r);
    push_binding(lib, name, lib->last_fp_width ? fp_canon(lib, v) : v);
    return;
  }
  smt_func(ctx, name, n, dom, r);
}

static void print_stats(SmtLib *lib, SmtSolver *solver) {
  SmtStats st;
  smt_solver_stats(solver, &st);
  fprintf(stderr,
          "(:terms %llu :gates %llu :variables %llu :clauses %llu "
          ":conflicts %llu :decisions %llu :propagations %llu :restarts %llu "
          ":lemmas-checked %llu :blast-time %.4f :sat-time %.4f "
          ":check-time %.4f)\n",
          (unsigned long long)st.terms, (unsigned long long)st.gates,
          (unsigned long long)st.variables, (unsigned long long)st.clauses,
          (unsigned long long)st.conflicts, (unsigned long long)st.decisions,
          (unsigned long long)st.propagations, (unsigned long long)st.restarts,
          (unsigned long long)st.lemmas_checked, st.blast_seconds,
          st.sat_seconds, st.check_seconds);
  (void)lib;
}

static int run_command(SmtLib *lib, const Sx *s) {
  SmtCtx *ctx = lib->ctx;
  lib->line = s->line;
  if (!s->is_list || s->count == 0 || s->items[0]->is_list) {
    lib_error(lib, s, "expected a command", NULL);
    return 0;
  }
  const char *cmd = s->items[0]->atom;
  if (strcmp(cmd, "set-logic") == 0 || strcmp(cmd, "set-info") == 0 ||
      strcmp(cmd, "set-option") == 0 || strcmp(cmd, "get-info") == 0) {
    return 1;
  }
  if (strcmp(cmd, "exit") == 0) {
    return 2;
  }
  if (strcmp(cmd, "declare-const") == 0 && s->count == 3 &&
      !s->items[1]->is_list) {
    declare(lib, s, s->items[1]->atom, NULL, s->items[2]);
    return !lib->failed;
  }
  if (strcmp(cmd, "declare-fun") == 0 && s->count == 4 &&
      !s->items[1]->is_list && s->items[2]->is_list) {
    declare(lib, s, s->items[1]->atom, s->items[2], s->items[3]);
    return !lib->failed;
  }
  if (strcmp(cmd, "define-fun") == 0 && s->count == 5 &&
      !s->items[1]->is_list && s->items[2]->is_list) {
    const Sx *params = s->items[2];
    unsigned n = params->count;
    SmtTerm *vars = malloc((n ? n : 1u) * sizeof(SmtTerm));
    unsigned mark = lib->bind_count;
    for (unsigned i = 0; i < n; i++) {
      const Sx *p = params->items[i];
      if (!p->is_list || p->count != 2 || p->items[0]->is_list) {
        lib_error(lib, p, "malformed parameter", NULL);
        free(vars);
        return 0;
      }
      SmtSort sort = parse_sort(lib, p->items[1]);
      if (!sort) {
        free(vars);
        return 0;
      }
      vars[i] = smt_fresh(ctx, p->items[0]->atom, sort);
      push_binding(lib, p->items[0]->atom, vars[i]);
    }
    SmtTerm body = parse_term(lib, s->items[4]);
    pop_bindings(lib, mark);
    if (!body) {
      free(vars);
      return 0;
    }
    if (lib->macro_count == lib->macro_cap) {
      lib->macro_cap = lib->macro_cap ? lib->macro_cap * 2u : 16u;
      lib->macros = realloc(lib->macros, lib->macro_cap * sizeof(Macro));
    }
    Macro *m = &lib->macros[lib->macro_count++];
    size_t len = strlen(s->items[1]->atom);
    m->name = malloc(len + 1u);
    memcpy(m->name, s->items[1]->atom, len + 1u);
    m->nparams = n;
    m->params = vars;
    m->body = body;
    return 1;
  }
  if (strcmp(cmd, "assert") == 0 && s->count == 2) {
    SmtTerm t = parse_term(lib, s->items[1]);
    if (!t) {
      return 0;
    }
    if (!smt_sort_is_bool(ctx, smt_term_sort(ctx, t))) {
      lib_error(lib, s, "assertion is not Boolean", NULL);
      return 0;
    }
    lib_assert(lib, t);
    return 1;
  }
  if (strcmp(cmd, "push") == 0) {
    unsigned n = 1;
    if (s->count == 2) {
      parse_uint(s->items[1], &n);
    }
    while (n--) {
      if (lib->frame_count == lib->frame_cap) {
        lib->frame_cap = lib->frame_cap ? lib->frame_cap * 2u : 16u;
        lib->frames = realloc(lib->frames, lib->frame_cap * sizeof(unsigned));
      }
      lib->frames[lib->frame_count++] = lib->assert_count;
    }
    return 1;
  }
  if (strcmp(cmd, "pop") == 0) {
    unsigned n = 1;
    if (s->count == 2) {
      parse_uint(s->items[1], &n);
    }
    while (n-- && lib->frame_count) {
      lib->assert_count = lib->frames[--lib->frame_count];
    }
    return 1;
  }
  if (strcmp(cmd, "check-sat") == 0) {
    smt_solver_free(lib->last);
    SmtSolver *solver = smt_solver_new(ctx);
    smt_solver_set_certify(solver, lib->certify);
    smt_solver_set_normalize(solver, g_smtlib_normalize);
    for (unsigned i = 0; i < lib->assert_count; i++) {
      smt_solver_assert(solver, lib->asserts[i]);
    }
    SmtResult r = smt_solver_check(solver);
    fputs(r == SMT_SAT     ? "sat\n"
          : r == SMT_UNSAT ? "unsat\n"
                           : "unknown\n",
          lib->out);
    if (r == SMT_UNKNOWN && smt_solver_reason(solver)) {
      fprintf(stderr, "; %s\n", smt_solver_reason(solver));
    }
    if (lib->show_stats) {
      print_stats(lib, solver);
    }
    lib->last = solver;
    return 1;
  }
  if (strcmp(cmd, "get-model") == 0) {
    if (lib->last) {
      smt_solver_print_model(lib->last, lib->out);
    }
    return 1;
  }
  if (strcmp(cmd, "get-value") == 0 && s->count == 2 && s->items[1]->is_list) {
    if (!lib->last) {
      return 1;
    }
    fputs("(", lib->out);
    for (unsigned i = 0; i < s->items[1]->count; i++) {
      SmtTerm t = parse_term(lib, s->items[1]->items[i]);
      uint64_t w[SMT_WORDS];
      if (!t || !smt_solver_value(lib->last, t, w)) {
        continue;
      }
      fputs(i ? " (" : "(", lib->out);
      const Sx *query = s->items[1]->items[i];
      if (query->is_list) {
        smt_term_print(ctx, t, lib->out);
      } else {
        fputs(query->atom, lib->out);
      }
      fputc(' ', lib->out);
      SmtSort sort = smt_term_sort(ctx, t);
      if (smt_sort_is_bool(ctx, sort)) {
        fputs(w[0] ? "true" : "false", lib->out);
      } else {
        SmtBits b;
        memcpy(b.w, w, sizeof(b.w));
        smt_bits_print_hex(&b, smt_sort_width(ctx, sort), lib->out);
      }
      fputc(')', lib->out);
    }
    fputs(")\n", lib->out);
    return 1;
  }
  if (strcmp(cmd, "echo") == 0 && s->count == 2) {
    const char *text = s->items[1]->atom ? s->items[1]->atom : "";
    size_t len = strlen(text);
    if (len >= 2 && text[0] == '"' && text[len - 1] == '"') {
      fprintf(lib->out, "%.*s\n", (int)(len - 2), text + 1);
    } else {
      fprintf(lib->out, "%s\n", text);
    }
    return 1;
  }
  lib_error(lib, s, "unsupported command %s", cmd);
  return 0;
}

static char *read_all(FILE *f) {
  size_t cap = 65536, len = 0;
  char *buf = malloc(cap);
  for (;;) {
    if (len + 4096u + 1u > cap) {
      cap *= 2u;
      buf = realloc(buf, cap);
    }
    size_t got = fread(buf + len, 1, cap - len - 1u, f);
    if (got == 0) {
      break;
    }
    len += got;
  }
  buf[len] = 0;
  return buf;
}

static int dimacs_run(const char *src, const char *path, int certify,
                      int stats) {
  SatSolver *sat = sat_new();
  if (certify) {
    sat_enable_proof(sat);
  }
  const char *p = src;
  uint32_t vars = 0;
  SatLit *clause = NULL;
  size_t len = 0, cap = 0;
  uint32_t *original = NULL;
  size_t olen = 0, ocap = 0;
  while (*p) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
      p++;
    }
    if (*p == 'c' || *p == '%') {
      while (*p && *p != '\n') {
        p++;
      }
      continue;
    }
    if (*p == 'p') {
      while (*p && !isdigit((unsigned char)*p)) {
        p++;
      }
      vars = (uint32_t)strtoull(p, (char **)&p, 10);
      while (sat_var_count(sat) < vars) {
        sat_new_var(sat);
      }
      while (*p && *p != '\n') {
        p++;
      }
      continue;
    }
    if (!*p) {
      break;
    }
    int neg = 0;
    if (*p == '-') {
      neg = 1;
      p++;
    }
    if (!isdigit((unsigned char)*p)) {
      fprintf(stderr, "%s: malformed DIMACS\n", path);
      sat_free(sat);
      free(clause);
      free(original);
      return 1;
    }
    uint64_t v = strtoull(p, (char **)&p, 10);
    if (v == 0) {
      sat_add_clause(sat, clause, (unsigned)len);
      if (olen + len + 1 > ocap) {
        ocap = (olen + len + 1) * 2;
        original = realloc(original, ocap * sizeof(uint32_t));
      }
      original[olen++] = (uint32_t)len;
      memcpy(original + olen, clause, len * sizeof(SatLit));
      olen += len;
      len = 0;
      continue;
    }
    while (sat_var_count(sat) < v) {
      sat_new_var(sat);
    }
    if (len == cap) {
      cap = cap ? cap * 2 : 16;
      clause = realloc(clause, cap * sizeof(SatLit));
    }
    clause[len++] = SAT_LIT((uint32_t)(v - 1), neg);
  }
  double t0 = smt_now_seconds();
  int r = sat_solve(sat, 0);
  double t1 = smt_now_seconds();
  int status = 0;
  if (r == SAT_RESULT_SAT) {
    size_t i = 0;
    int ok = 1;
    while (i < olen && ok) {
      uint32_t n = original[i++];
      int sat_clause = 0;
      for (uint32_t k = 0; k < n; k++) {
        SatLit l = original[i + k];
        sat_clause |= sat_value(sat, SAT_VAR(l)) != (int)SAT_SIGN(l);
      }
      ok = sat_clause;
      i += n;
    }
    printf(ok ? "s SATISFIABLE\n" : "s UNKNOWN\nc model check failed\n");
  } else if (r == SAT_RESULT_UNSAT) {
    int ok = 1;
    uint64_t checked = 0;
    if (certify) {
      SatProofView view;
      sat_proof_view(sat, &view);
      ok = sat_drup_check(&view, &checked);
    }
    printf(ok ? "s UNSATISFIABLE\n" : "s UNKNOWN\nc refutation check failed\n");
    if (stats) {
      fprintf(stderr, "c lemmas checked %llu in %.3f s\n",
              (unsigned long long)checked, smt_now_seconds() - t1);
    }
  } else {
    printf("s UNKNOWN\n");
    status = 1;
  }
  if (stats) {
    SatStats st;
    sat_get_stats(sat, &st);
    fprintf(stderr,
            "c vars %u clauses %llu conflicts %llu decisions %llu "
            "propagations %llu restarts %llu search %.3f s\n",
            sat_var_count(sat), (unsigned long long)st.original_clauses,
            (unsigned long long)st.conflicts, (unsigned long long)st.decisions,
            (unsigned long long)st.propagations,
            (unsigned long long)st.restarts, t1 - t0);
  }
  sat_free(sat);
  free(clause);
  free(original);
  return status;
}

int smtlib_main(int argc, char **argv) {
  int certify = 1, stats = 0, status = 0, files = 0, dimacs = 0;
  for (int i = 0; i < argc; i++) {
    if (strcmp(argv[i], "--no-certify") == 0) {
      certify = 0;
    } else if (strcmp(argv[i], "--stats") == 0) {
      stats = 1;
    } else if (strcmp(argv[i], "--dimacs") == 0) {
      dimacs = 1;
    } else if (strcmp(argv[i], "--no-normalize") == 0) {
      g_smtlib_normalize = 0;
    }
  }
  for (int i = 0; i < argc; i++) {
    if (argv[i][0] == '-' && argv[i][1]) {
      continue;
    }
    files++;
    FILE *f = strcmp(argv[i], "-") == 0 ? stdin : fopen(argv[i], "rb");
    if (!f) {
      fprintf(stderr, "error: cannot open %s\n", argv[i]);
      status = 1;
      continue;
    }
    char *src = read_all(f);
    if (f != stdin) {
      fclose(f);
    }
    size_t plen = strlen(argv[i]);
    if (dimacs || (plen > 4 && strcmp(argv[i] + plen - 4, ".cnf") == 0)) {
      status |= dimacs_run(src, argv[i], certify, stats);
      free(src);
      continue;
    }
    status |= smtlib_run(src, argv[i], stdout, certify, stats);
    free(src);
  }
  if (!files) {
    fprintf(stderr,
            "usage: mettle smt <file.smt2|-> [--stats] [--no-certify]\n"
            "  decides SMT-LIB 2 problems over bit-vectors, arrays and "
            "uninterpreted functions;\n"
            "  every unsat answer is checked against a DRUP refutation and "
            "every sat answer\n"
            "  against its model\n");
    return 2;
  }
  return status;
}

int smtlib_run(const char *source, const char *path, FILE *out, int certify,
               int show_stats) {
  SmtLib lib;
  memset(&lib, 0, sizeof(lib));
  lib.ctx = smt_ctx_new();
  lib.out = out;
  lib.certify = certify;
  lib.show_stats = show_stats;
  lib.path = path ? path : "<input>";
  SxReader r;
  r.src = source;
  r.pos = 0;
  r.line = 1;
  r.error = NULL;
  int status = 0;
  for (;;) {
    Sx *s = sx_read(&r);
    if (!s) {
      if (r.error) {
        fprintf(stderr, "%s:%d: error: %s\n", lib.path, r.line, r.error);
        status = 1;
      }
      break;
    }
    int rc = run_command(&lib, s);
    sx_free(s);
    if (lib.failed) {
      fprintf(stderr, "%s\n", lib.error);
      status = 1;
      break;
    }
    if (rc == 2) {
      break;
    }
  }
  smt_solver_free(lib.last);
  pop_bindings(&lib, 0);
  free(lib.binds);
  for (unsigned i = 0; i < lib.macro_count; i++) {
    free(lib.macros[i].name);
    free(lib.macros[i].params);
  }
  free(lib.macros);
  free(lib.asserts);
  free(lib.fp_terms);
  free(lib.frames);
  smt_ctx_free(lib.ctx);
  return status;
}
