#include "translate/ctr_emit.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int spelled_hex(const char *s) { return s && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'); }

static char *strip_suffix(const char *s) {
  char *copy = ctr_strdup(s);
  size_t n = strlen(copy);
  while (n > 0 && strchr("uUlL", copy[n - 1])) {
    copy[--n] = '\0';
  }
  return copy;
}

static char *int_text(int64_t v, CtrType *ty, const char *spelling) {
  int hex = spelled_hex(spelling) && strtoull(spelling, NULL, 16) == (uint64_t)v;
  if (hex && (ty->is_unsigned || v >= 0)) {
    return strip_suffix(spelling);
  }
  if (ty->is_unsigned) {
    return ctr_format("%llu", (unsigned long long)v);
  }
  return ctr_format("%lld", (long long)v);
}

char *ctr_float_text(double v, int is_float) {
  if (isnan(v)) {
    return "math.NAN()";
  }
  if (isinf(v)) {
    return v > 0 ? "math.INF()" : "(0.0 - math.INF())";
  }
  char buf[64];
  for (int prec = 1; prec <= 17; prec++) {
    snprintf(buf, sizeof(buf), "%.*g", prec, v);
    double back = strtod(buf, NULL);
    if (is_float ? (float)back == (float)v : back == v) {
      break;
    }
  }
  char *e = strchr(buf, 'e');
  if (strchr(buf, '.')) {
    return ctr_strdup(buf);
  }
  if (e) {
    return ctr_format("%.*s.0%s", (int)(e - buf), buf, e);
  }
  return ctr_format("%s.0", buf);
}

static int char_spelled(const char *spelling) { return spelling && spelling[0] == '\''; }

static char *char_literal_text(int64_t v) {
  if (v >= 32 && v < 127 && v != '\'' && v != '\\') {
    return ctr_format("'%c'", (int)v);
  }
  switch (v) {
  case '\n':
    return "'\\n'";
  case '\t':
    return "'\\t'";
  case '\r':
    return "'\\r'";
  case '\\':
    return "'\\\\'";
  case '\'':
    return "'\\''";
  case 0:
    return "'\\0'";
  default:
    return ctr_format("'\\x%02x'", (unsigned)(v & 0xff));
  }
}

int ctr_char_literal_ok(CtrNode *node) {
  return node->kind == ND_NUM && char_spelled(node->spelling) && node->ival >= 0 && node->ival < 128;
}

char *ctr_literal_text(CtrNode *node, CtrType *ty) {
  if (node->is_float_lit || ctr_is_flonum(ty)) {
    double v = node->is_float_lit ? node->fval : (double)node->ival;
    if (ctr_is_integer(node->ty) && node->ty->is_unsigned && node->ty->size == 8) {
      v = (double)(uint64_t)node->ival;
    }
    return ctr_float_text(v, ty->kind == TY_FLOAT);
  }
  if (node->var && node->var->is_enum_const && node->var->mettle_name) {
    return (char *)ctr_mettle_name_of(node->var);
  }
  if (ty->kind == TY_BOOL) {
    return node->ival ? "true" : "false";
  }
  if (ctr_char_literal_ok(node) && (ty->size == 1 || (ty->size >= 4 && !ty->is_unsigned))) {
    return char_literal_text(node->ival);
  }
  return int_text(node->ival, ty, node->spelling);
}

static int utf8_length(const unsigned char *s, int avail) {
  int len = s[0] >= 0xF0 ? 4 : s[0] >= 0xE0 ? 3 : s[0] >= 0xC2 ? 2 : 0;
  if (len == 0 || len > avail || s[0] > 0xF4) {
    return 0;
  }
  for (int i = 1; i < len; i++) {
    if ((s[i] & 0xC0) != 0x80) {
      return 0;
    }
  }
  return len;
}

static void put_escaped(CtrBuf *buf, unsigned c) {
  switch (c) {
  case '"':
    ctr_buf_put(buf, "\\\"");
    return;
  case '\\':
    ctr_buf_put(buf, "\\\\");
    return;
  case '\n':
    ctr_buf_put(buf, "\\n");
    return;
  case '\t':
    ctr_buf_put(buf, "\\t");
    return;
  case '\r':
    ctr_buf_put(buf, "\\r");
    return;
  case '{':
    ctr_buf_put(buf, "{{");
    return;
  case 0:
    ctr_buf_put(buf, "\\0");
    return;
  default:
    if (c < 32 || c >= 127) {
      ctr_buf_printf(buf, "\\x%02x", c);
    } else {
      ctr_buf_putc(buf, (char)c);
    }
  }
}

char *ctr_string_literal_text(const char *bytes, int len, int elem) {
  CtrBuf buf = {0};
  ctr_buf_putc(&buf, '"');
  int count = len / elem - 1;
  if (elem != 1) {
    count = 0;
  }
  const unsigned char *s = (const unsigned char *)bytes;
  for (int i = 0; i < count;) {
    int n = s[i] >= 128 ? utf8_length(s + i, count - i) : 0;
    if (n > 0) {
      ctr_buf_putn(&buf, (const char *)s + i, (size_t)n);
      i += n;
      continue;
    }
    put_escaped(&buf, s[i]);
    i++;
  }
  ctr_buf_putc(&buf, '"');
  return ctr_buf_take(&buf);
}
