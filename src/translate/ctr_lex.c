#include "translate/ctr.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static CtrFile *current_file;
static int at_bol;
static int has_space;
static const char *pending_comment;

CtrFile *ctr_new_file(const char *name, int index, char *contents) {
  CtrFile *file = ctr_alloc(sizeof(CtrFile));
  file->name = ctr_strdup(name);
  file->display = file->name;
  file->key = ctr_path_key(name);
  file->index = index;
  file->contents = contents;
  return file;
}

static CtrToken *new_token(CtrTokenKind kind, const char *start, const char *end) {
  CtrToken *tok = ctr_alloc(sizeof(CtrToken));
  tok->kind = kind;
  tok->loc = start;
  tok->len = (int)(end - start);
  tok->file = current_file;
  tok->at_bol = at_bol;
  tok->has_space = has_space;
  tok->comment = pending_comment;
  pending_comment = NULL;
  at_bol = has_space = 0;
  return tok;
}

CtrToken *ctr_copy_token(CtrToken *tok) {
  CtrToken *t = ctr_alloc(sizeof(CtrToken));
  *t = *tok;
  t->next = NULL;
  return t;
}

CtrToken *ctr_new_eof(CtrToken *tok) {
  CtrToken *t = ctr_copy_token(tok);
  t->kind = TK_EOF;
  t->len = 0;
  return t;
}

static int starts_with(const char *p, const char *q) { return strncmp(p, q, strlen(q)) == 0; }

uint32_t ctr_decode_utf8(const char **new_pos, const char *p) {
  const unsigned char *s = (const unsigned char *)p;
  if (s[0] < 128) {
    *new_pos = p + 1;
    return s[0];
  }
  int len;
  uint32_t c;
  if (s[0] >= 0xF0) {
    len = 4;
    c = s[0] & 0x7;
  } else if (s[0] >= 0xE0) {
    len = 3;
    c = s[0] & 0xF;
  } else {
    len = 2;
    c = s[0] & 0x1F;
  }
  for (int i = 1; i < len; i++) {
    if ((s[i] >> 6) != 2) {
      *new_pos = p + 1;
      return s[0];
    }
    c = (c << 6) | (s[i] & 0x3F);
  }
  *new_pos = p + len;
  return c;
}

int ctr_encode_utf8(char *buf, uint32_t c) {
  if (c <= 0x7F) {
    buf[0] = (char)c;
    return 1;
  }
  if (c <= 0x7FF) {
    buf[0] = (char)(0xC0 | (c >> 6));
    buf[1] = (char)(0x80 | (c & 0x3F));
    return 2;
  }
  if (c <= 0xFFFF) {
    buf[0] = (char)(0xE0 | (c >> 12));
    buf[1] = (char)(0x80 | ((c >> 6) & 0x3F));
    buf[2] = (char)(0x80 | (c & 0x3F));
    return 3;
  }
  buf[0] = (char)(0xF0 | (c >> 18));
  buf[1] = (char)(0x80 | ((c >> 12) & 0x3F));
  buf[2] = (char)(0x80 | ((c >> 6) & 0x3F));
  buf[3] = (char)(0x80 | (c & 0x3F));
  return 4;
}

int ctr_is_ident1(uint32_t c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$' || c >= 0x80;
}

int ctr_is_ident2(uint32_t c) { return ctr_is_ident1(c) || (c >= '0' && c <= '9'); }

static int read_ident(const char *start) {
  const char *p = start;
  uint32_t c = ctr_decode_utf8(&p, p);
  if (!ctr_is_ident1(c)) {
    return 0;
  }
  for (;;) {
    const char *q;
    c = ctr_decode_utf8(&q, p);
    if (!ctr_is_ident2(c)) {
      return (int)(p - start);
    }
    p = q;
  }
}

static const char *punctuators[] = {
    "<<=", ">>=", "...", "==", "!=", "<=", ">=", "->", "+=", "-=", "*=", "/=", "++", "--",
    "%=",  "&=",  "|=",  "^=", "&&", "||", "<<", ">>", "##", "<:", ":>", "<%", "%>", "%:%:", "%:"};

static int read_punct(const char *p) {
  for (size_t i = 0; i < sizeof(punctuators) / sizeof(*punctuators); i++) {
    if (starts_with(p, punctuators[i])) {
      return (int)strlen(punctuators[i]);
    }
  }
  return ispunct((unsigned char)*p) ? 1 : 0;
}

static const char *keywords[] = {
    "return", "if", "else", "for", "while", "int", "sizeof", "char", "struct", "union",
    "short", "long", "void", "typedef", "_Bool", "enum", "static", "goto", "break",
    "continue", "switch", "case", "default", "extern", "_Alignof", "_Alignas", "do",
    "signed", "unsigned", "const", "volatile", "auto", "register", "restrict",
    "__restrict", "__restrict__", "_Noreturn", "float", "double", "typeof", "asm",
    "_Thread_local", "__thread", "_Atomic", "__attribute__", "__attribute", "inline",
    "__inline", "__inline__", "__volatile__", "__volatile", "__const", "__const__",
    "__signed__", "__signed", "__asm__", "__asm", "__typeof__", "__typeof", "_Static_assert",
    "__extension__", "__declspec", "__cdecl", "__stdcall", "__fastcall", "_Complex",
    "__int64", "__int32", "__int16", "__int8", "__w64", "__forceinline", "_Generic",
    "__alignof__", "__alignof", "_Float32", "_Float64", "__builtin_va_list", "__unaligned",
    "__ptr64", "__ptr32", "__label__", "__auto_type"};

static CtrMap keyword_map;

int ctr_is_keyword(CtrToken *tok) {
  if (!keyword_map.entries) {
    for (size_t i = 0; i < sizeof(keywords) / sizeof(*keywords); i++) {
      ctr_map_put(&keyword_map, keywords[i], (void *)1);
    }
  }
  return ctr_map_get2(&keyword_map, tok->loc, tok->len) != NULL;
}

static int from_hex(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  return c - 'A' + 10;
}

static int read_ucn(const char **new_pos, const char *p, int digits) {
  uint32_t c = 0;
  for (int i = 0; i < digits && isxdigit((unsigned char)p[i]); i++) {
    c = (c << 4) | (uint32_t)from_hex(p[i]);
  }
  *new_pos = p + digits;
  return (int)c;
}

static int read_octal_escape(const char **new_pos, const char *p) {
  int c = *p++ - '0';
  if ('0' <= *p && *p <= '7') {
    c = (c << 3) + (*p++ - '0');
    if ('0' <= *p && *p <= '7') {
      c = (c << 3) + (*p++ - '0');
    }
  }
  *new_pos = p;
  return c;
}

static int simple_escape(char c) {
  switch (c) {
  case 'a':
    return 7;
  case 'b':
    return 8;
  case 't':
    return 9;
  case 'n':
    return 10;
  case 'v':
    return 11;
  case 'f':
    return 12;
  case 'r':
    return 13;
  case 'e':
    return 27;
  default:
    return c;
  }
}

int ctr_read_escaped_char(const char **new_pos, const char *p) {
  if ('0' <= *p && *p <= '7') {
    return read_octal_escape(new_pos, p);
  }
  if (*p == 'x') {
    p++;
    int c = 0;
    for (; isxdigit((unsigned char)*p); p++) {
      c = (c << 4) + from_hex(*p);
    }
    *new_pos = p;
    return c;
  }
  if (*p == 'u') {
    return read_ucn(new_pos, p + 1, 4);
  }
  if (*p == 'U') {
    return read_ucn(new_pos, p + 1, 8);
  }
  *new_pos = p + 1;
  return simple_escape(*p);
}

static const char *string_literal_end(const char *p) {
  const char *start = p;
  for (; *p != '"'; p++) {
    if (*p == '\n' || *p == '\0') {
      CtrToken tmp = {0};
      tmp.file = current_file;
      tmp.loc = start;
      ctr_fatal_tok(&tmp, "unclosed string literal");
    }
    if (*p == '\\') {
      p++;
    }
  }
  return p;
}

typedef struct {
  CtrBuf bytes;
  int elem;
} WideBuf;

static void put_elem(CtrBuf *buf, uint32_t c, int elem) {
  for (int i = 0; i < elem; i++) {
    ctr_buf_putc(buf, (char)((c >> (8 * i)) & 0xFF));
  }
}

static int needs_utf8_decode(int elem) { return elem > 1; }

static void put_wide(CtrBuf *buf, uint32_t c, int elem) {
  if (elem == 2 && c >= 0x10000) {
    c -= 0x10000;
    put_elem(buf, 0xD800 + ((c >> 10) & 0x3FF), 2);
    put_elem(buf, 0xDC00 + (c & 0x3FF), 2);
    return;
  }
  put_elem(buf, c, elem);
}

static void put_literal_char(CtrBuf *buf, const char **pp, int elem) {
  const char *p = *pp;
  if (*p == '\\') {
    int is_ucn = p[1] == 'u' || p[1] == 'U';
    uint32_t c = (uint32_t)ctr_read_escaped_char(&p, p + 1);
    if (elem == 1 && is_ucn) {
      char tmp[4];
      int n = ctr_encode_utf8(tmp, c);
      ctr_buf_putn(buf, tmp, (size_t)n);
    } else if (elem == 1) {
      ctr_buf_putc(buf, (char)c);
    } else {
      put_wide(buf, c, elem);
    }
  } else if (needs_utf8_decode(elem)) {
    uint32_t c = ctr_decode_utf8(&p, p);
    put_wide(buf, c, elem);
  } else {
    ctr_buf_putc(buf, *p++);
  }
  *pp = p;
}

static CtrToken *read_string_literal(const char *start, const char *quote, int elem, CtrType *ty) {
  const char *end = string_literal_end(quote + 1);
  CtrBuf buf = {0};
  for (const char *p = quote + 1; p < end;) {
    put_literal_char(&buf, &p, elem);
  }
  put_elem(&buf, 0, elem);
  CtrToken *tok = new_token(TK_STR, start, end + 1);
  tok->str_len = (int)buf.len;
  tok->str = ctr_buf_take(&buf);
  tok->ty = ctr_array_of(ty, tok->str_len / elem);
  return tok;
}

static CtrToken *read_char_literal(const char *start, const char *quote, CtrType *ty, int elem) {
  const char *p = quote + 1;
  if (*p == '\0') {
    CtrToken tmp = {0};
    tmp.file = current_file;
    tmp.loc = start;
    ctr_fatal_tok(&tmp, "unclosed char literal");
  }
  int64_t value = 0;
  int count = 0;
  while (*p != '\'') {
    if (*p == '\n' || *p == '\0') {
      CtrToken tmp = {0};
      tmp.file = current_file;
      tmp.loc = start;
      ctr_fatal_tok(&tmp, "unclosed char literal");
    }
    uint32_t c;
    if (*p == '\\') {
      c = (uint32_t)ctr_read_escaped_char(&p, p + 1);
    } else if (elem > 1) {
      c = ctr_decode_utf8(&p, p);
    } else {
      c = (unsigned char)*p++;
    }
    value = elem == 1 ? ((value << 8) | (c & 0xFF)) : (int64_t)c;
    count++;
  }
  CtrToken *tok = new_token(TK_NUM, start, p + 1);
  if (elem == 1 && count == 1) {
    value = (int64_t)(signed char)value;
    if (ctr_ty_char->is_unsigned) {
      value &= 0xFF;
    }
  } else if (elem == 1) {
    value = (int32_t)value;
  }
  tok->ival = value;
  tok->ty = ty;
  return tok;
}

static int convert_pp_int_suffix(const char *p, int *l, int *u) {
  int len = 0;
  *l = 0;
  *u = 0;
  for (;;) {
    char c = (char)tolower((unsigned char)p[len]);
    if (c == 'u' && !*u) {
      *u = 1;
      len++;
    } else if (c == 'l' && !*l) {
      *l = (tolower((unsigned char)p[len + 1]) == 'l') ? 2 : 1;
      len += *l;
    } else if (c == 'i' && p[len + 1] == '6' && p[len + 2] == '4') {
      *l = 2;
      len += 3;
    } else if (c == 'i' && p[len + 1] == '3' && p[len + 2] == '2') {
      len += 3;
    } else {
      return len;
    }
  }
}

static CtrType *int_literal_type(uint64_t val, int base, int l, int u) {
  int long_bits = ctr_ty_long->size * 8;
  if (l == 2) {
    if (u || (base != 10 && (val >> 63))) {
      return ctr_ty_ullong;
    }
    return ctr_ty_llong;
  }
  if (l == 1) {
    if (u) {
      return (long_bits == 32 && (val >> 32)) ? ctr_ty_ullong : ctr_ty_ulong;
    }
    if (long_bits == 64) {
      return (base != 10 && (val >> 63)) ? ctr_ty_ulong : ctr_ty_long;
    }
    if (val >> 32) {
      return (base != 10 && (val >> 63)) ? ctr_ty_ullong : ctr_ty_llong;
    }
    return (base != 10 && (val >> 31)) ? ctr_ty_ulong : ctr_ty_long;
  }
  if (u) {
    if (val >> 32) {
      return long_bits == 64 ? ctr_ty_ulong : ctr_ty_ullong;
    }
    return ctr_ty_uint;
  }
  if (val >> 63) {
    return base == 10 ? ctr_ty_llong : ctr_ty_ullong;
  }
  if (val >> 32) {
    if (long_bits == 64) {
      return (base != 10 && (val >> 63)) ? ctr_ty_ulong : ctr_ty_long;
    }
    return ctr_ty_llong;
  }
  if (val >> 31) {
    if (base != 10) {
      return ctr_ty_uint;
    }
    return long_bits == 64 ? ctr_ty_long : ctr_ty_llong;
  }
  return ctr_ty_int;
}

static int convert_pp_int(CtrToken *tok) {
  const char *p = tok->loc;
  int base = 10;
  if ((p[0] == '0') && (p[1] == 'x' || p[1] == 'X') && isxdigit((unsigned char)p[2])) {
    p += 2;
    base = 16;
  } else if (p[0] == '0' && (p[1] == 'b' || p[1] == 'B') && (p[2] == '0' || p[2] == '1')) {
    p += 2;
    base = 2;
  } else if (*p == '0') {
    base = 8;
  }
  errno = 0;
  char *endp;
  uint64_t val = strtoull(p, &endp, base);
  p = endp;
  int l, u;
  p += convert_pp_int_suffix(p, &l, &u);
  if (p != tok->loc + tok->len) {
    return 0;
  }
  tok->kind = TK_NUM;
  tok->ival = (int64_t)val;
  tok->ty = int_literal_type(val, base, l, u);
  return 1;
}

void ctr_convert_pp_number(CtrToken *tok) {
  if (convert_pp_int(tok)) {
    return;
  }
  char *text = ctr_token_text(tok);
  char *end;
  double val = strtod(text, &end);
  CtrType *ty = ctr_ty_double;
  if (*end == 'f' || *end == 'F') {
    ty = ctr_ty_float;
    end++;
  } else if (*end == 'l' || *end == 'L') {
    ty = ctr_ty_ldouble;
    end++;
  }
  if (*end != '\0') {
    ctr_fatal_tok(tok, "invalid numeric constant");
  }
  tok->kind = TK_NUM;
  tok->fval = val;
  tok->ty = ty;
}

void ctr_convert_pp_tokens(CtrToken *tok) {
  for (CtrToken *t = tok; t && t->kind != TK_EOF; t = t->next) {
    if (t->kind == TK_IDENT && ctr_is_keyword(t)) {
      t->kind = TK_KEYWORD;
    } else if (t->kind == TK_PPNUM) {
      ctr_convert_pp_number(t);
    }
  }
}

static void add_line_numbers(CtrToken *tok) {
  const char *p = current_file->contents;
  const char *line_start = p;
  int n = 1;
  do {
    if (p == tok->loc) {
      tok->line = n;
      tok->col = (int)(p - line_start) + 1;
      tok = tok->next;
    }
    if (*p == '\n') {
      n++;
      line_start = p + 1;
    }
  } while (*p++);
}

static const char *skip_comment(const char *p, int keep) {
  const char *start = p;
  if (starts_with(p, "//")) {
    p += 2;
    while (*p != '\n' && *p) {
      p++;
    }
  } else {
    const char *q = strstr(p + 2, "*/");
    if (!q) {
      CtrToken tmp = {0};
      tmp.file = current_file;
      tmp.loc = p;
      ctr_fatal_tok(&tmp, "unclosed block comment");
    }
    p = q + 2;
  }
  if (keep) {
    char *text = ctr_strndup(start, (size_t)(p - start));
    pending_comment = pending_comment ? ctr_format("%s\n%s", pending_comment, text) : text;
  }
  has_space = 1;
  return p;
}

static const char *read_ppnumber(const char *p) {
  for (;;) {
    if (p[0] && p[1] && strchr("eEpP", p[0]) && strchr("+-", p[1])) {
      p += 2;
    } else if (isalnum((unsigned char)*p) || *p == '.' || *p == '_') {
      p++;
    } else if (*p == '\'' && isalnum((unsigned char)p[1])) {
      p++;
    } else {
      return p;
    }
  }
}

typedef struct {
  const char *prefix;
  int elem;
  int which;
} LiteralPrefix;

static CtrType *prefix_type(int which) {
  switch (which) {
  case 1:
    return ctr_ty_ushort;
  case 2:
    return ctr_ty_uint;
  case 3:
    return ctr_wchar_type();
  default:
    return ctr_ty_char;
  }
}

static const LiteralPrefix literal_prefixes[] = {
    {"u8\"", 1, 0}, {"u\"", 2, 1}, {"U\"", 4, 2}, {"L\"", 0, 3}, {"\"", 1, 0}};

static CtrToken *try_string(const char *p) {
  for (size_t i = 0; i < sizeof(literal_prefixes) / sizeof(*literal_prefixes); i++) {
    const LiteralPrefix *lp = &literal_prefixes[i];
    if (starts_with(p, lp->prefix)) {
      CtrType *ty = prefix_type(lp->which);
      int elem = lp->elem ? lp->elem : ty->size;
      return read_string_literal(p, p + strlen(lp->prefix) - 1, elem, ty);
    }
  }
  return NULL;
}

static CtrToken *try_char(const char *p) {
  if (*p == '\'') {
    return read_char_literal(p, p, ctr_ty_int, 1);
  }
  if (starts_with(p, "u'")) {
    CtrToken *t = read_char_literal(p, p + 1, ctr_ty_ushort, 2);
    t->ival &= 0xffff;
    return t;
  }
  if (starts_with(p, "L'")) {
    return read_char_literal(p, p + 1, ctr_wchar_type(), 4);
  }
  if (starts_with(p, "U'")) {
    return read_char_literal(p, p + 1, ctr_ty_uint, 4);
  }
  return NULL;
}

static CtrToken *lex_one(const char **pp, int keep) {
  const char *p = *pp;
  if (isdigit((unsigned char)*p) || (*p == '.' && isdigit((unsigned char)p[1]))) {
    const char *q = read_ppnumber(p);
    CtrToken *t = new_token(TK_PPNUM, p, q);
    *pp = q;
    return t;
  }
  CtrToken *t = try_string(p);
  if (!t) {
    t = try_char(p);
  }
  if (t) {
    *pp = p + t->len;
    return t;
  }
  int ident_len = read_ident(p);
  if (ident_len) {
    t = new_token(TK_IDENT, p, p + ident_len);
    *pp = p + ident_len;
    return t;
  }
  int punct_len = read_punct(p);
  if (punct_len) {
    t = new_token(TK_PUNCT, p, p + punct_len);
    *pp = p + punct_len;
    return t;
  }
  (void)keep;
  CtrToken tmp = {0};
  tmp.file = current_file;
  tmp.loc = p;
  ctr_fatal_tok(&tmp, "invalid token");
  return NULL;
}

CtrToken *ctr_tokenize(CtrFile *file) {
  current_file = file;
  const char *p = file->contents;
  CtrToken head = {0};
  CtrToken *cur = &head;
  int keep = ctr_prog && ctr_prog->options && ctr_prog->options->keep_comments && !file->is_system;
  at_bol = 1;
  has_space = 0;
  pending_comment = NULL;
  while (*p) {
    if (starts_with(p, "//") || starts_with(p, "/*")) {
      p = skip_comment(p, keep);
      continue;
    }
    if (*p == '\n') {
      p++;
      at_bol = 1;
      has_space = 0;
      continue;
    }
    if (isspace((unsigned char)*p)) {
      p++;
      has_space = 1;
      continue;
    }
    cur = cur->next = lex_one(&p, keep);
  }
  cur = cur->next = new_token(TK_EOF, p, p);
  add_line_numbers(head.next);
  return head.next;
}

static void canonicalize_newline(char *p) {
  int i = 0, j = 0;
  while (p[i]) {
    if (p[i] == '\r' && p[i + 1] == '\n') {
      i += 2;
      p[j++] = '\n';
    } else if (p[i] == '\r') {
      i++;
      p[j++] = '\n';
    } else {
      p[j++] = p[i++];
    }
  }
  p[j] = '\0';
}

static void remove_backslash_newline(char *p) {
  int i = 0, j = 0;
  int n = 0;
  while (p[i]) {
    if (p[i] == '\\' && p[i + 1] == '\n') {
      i += 2;
      n++;
    } else if (p[i] == '\n') {
      p[j++] = p[i++];
      for (; n > 0; n--) {
        p[j++] = '\n';
      }
    } else {
      p[j++] = p[i++];
    }
  }
  for (; n > 0; n--) {
    p[j++] = '\n';
  }
  p[j] = '\0';
}

static void strip_bom(char **p) {
  if ((unsigned char)(*p)[0] == 0xEF && (unsigned char)(*p)[1] == 0xBB &&
      (unsigned char)(*p)[2] == 0xBF) {
    *p += 3;
  }
}

CtrToken *ctr_tokenize_file(const char *path, int is_system) {
  char *contents = ctr_read_file(path);
  if (!contents) {
    return NULL;
  }
  strip_bom(&contents);
  canonicalize_newline(contents);
  remove_backslash_newline(contents);
  CtrFile *file = ctr_new_file(path, ctr_prog ? ctr_prog->files.len + 1 : 1, contents);
  file->is_system = is_system;
  size_t n = strlen(path);
  file->is_header = n > 2 && (path[n - 1] == 'h' || path[n - 1] == 'H') && path[n - 2] == '.';
  if (ctr_prog) {
    ctr_vec_push(&ctr_prog->files, file);
  }
  return ctr_tokenize(file);
}
