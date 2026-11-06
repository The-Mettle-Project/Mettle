#include "explain_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXJ_MAX_DEPTH 64

typedef struct {
  const char *text;
  size_t length;
  size_t at;
  char *error;
  size_t error_size;
  int failed;
} ExJsonParser;

static ExJson *exj_value(ExJsonParser *p, int depth);

static void exj_fail(ExJsonParser *p, const char *what) {
  if (!p->failed && p->error && p->error_size > 0) {
    snprintf(p->error, p->error_size, "%s at byte %zu", what, p->at);
  }
  p->failed = 1;
}

static void exj_space(ExJsonParser *p) {
  while (p->at < p->length) {
    char c = p->text[p->at];
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
      return;
    }
    p->at++;
  }
}

static ExJson *exj_new(ExJsonKind kind) {
  ExJson *value = calloc(1, sizeof(ExJson));
  if (value) {
    value->kind = kind;
  }
  return value;
}

static int exj_literal(ExJsonParser *p, const char *word) {
  size_t n = strlen(word);
  if (p->at + n > p->length || memcmp(p->text + p->at, word, n) != 0) {
    return 0;
  }
  p->at += n;
  return 1;
}

static int exj_hex4(ExJsonParser *p, unsigned *out) {
  unsigned value = 0;
  if (p->at + 4 > p->length) {
    return 0;
  }
  for (int i = 0; i < 4; i++) {
    char c = p->text[p->at++];
    value <<= 4;
    if (c >= '0' && c <= '9') {
      value |= (unsigned)(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      value |= (unsigned)(c - 'a' + 10);
    } else if (c >= 'A' && c <= 'F') {
      value |= (unsigned)(c - 'A' + 10);
    } else {
      return 0;
    }
  }
  *out = value;
  return 1;
}

static size_t exj_utf8(unsigned code, char *out) {
  if (code < 0x80) {
    out[0] = (char)code;
    return 1;
  }
  if (code < 0x800) {
    out[0] = (char)(0xC0 | (code >> 6));
    out[1] = (char)(0x80 | (code & 0x3F));
    return 2;
  }
  if (code < 0x10000) {
    out[0] = (char)(0xE0 | (code >> 12));
    out[1] = (char)(0x80 | ((code >> 6) & 0x3F));
    out[2] = (char)(0x80 | (code & 0x3F));
    return 3;
  }
  out[0] = (char)(0xF0 | (code >> 18));
  out[1] = (char)(0x80 | ((code >> 12) & 0x3F));
  out[2] = (char)(0x80 | ((code >> 6) & 0x3F));
  out[3] = (char)(0x80 | (code & 0x3F));
  return 4;
}

static char *exj_string(ExJsonParser *p) {
  size_t cap = 16;
  size_t len = 0;
  char *out = malloc(cap);
  p->at++;
  if (!out) {
    exj_fail(p, "out of memory");
    return NULL;
  }
  while (p->at < p->length) {
    char c = p->text[p->at++];
    char piece[4];
    size_t n = 1;
    if (c == '"') {
      out[len] = '\0';
      return out;
    }
    if ((unsigned char)c < 0x20) {
      exj_fail(p, "control character in a string");
      free(out);
      return NULL;
    }
    piece[0] = c;
    if (c == '\\') {
      char e = p->at < p->length ? p->text[p->at++] : '\0';
      switch (e) {
      case '"': piece[0] = '"'; break;
      case '\\': piece[0] = '\\'; break;
      case '/': piece[0] = '/'; break;
      case 'b': piece[0] = '\b'; break;
      case 'f': piece[0] = '\f'; break;
      case 'n': piece[0] = '\n'; break;
      case 'r': piece[0] = '\r'; break;
      case 't': piece[0] = '\t'; break;
      case 'u': {
        unsigned code = 0;
        unsigned low = 0;
        if (!exj_hex4(p, &code)) {
          exj_fail(p, "bad \\u escape");
          free(out);
          return NULL;
        }
        if (code >= 0xD800 && code <= 0xDBFF && p->at + 1 < p->length &&
            p->text[p->at] == '\\' && p->text[p->at + 1] == 'u') {
          p->at += 2;
          if (!exj_hex4(p, &low) || low < 0xDC00 || low > 0xDFFF) {
            exj_fail(p, "bad surrogate pair");
            free(out);
            return NULL;
          }
          code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
        }
        n = exj_utf8(code, piece);
        break;
      }
      default:
        exj_fail(p, "bad escape");
        free(out);
        return NULL;
      }
    }
    if (len + n + 1 > cap) {
      char *grown;
      while (len + n + 1 > cap) {
        cap *= 2;
      }
      grown = realloc(out, cap);
      if (!grown) {
        exj_fail(p, "out of memory");
        free(out);
        return NULL;
      }
      out = grown;
    }
    memcpy(out + len, piece, n);
    len += n;
  }
  exj_fail(p, "unterminated string");
  free(out);
  return NULL;
}

static ExJson *exj_number(ExJsonParser *p) {
  char buffer[64];
  size_t n = 0;
  ExJson *value;
  while (p->at < p->length && n + 1 < sizeof(buffer)) {
    char c = p->text[p->at];
    if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' ||
        c == 'e' || c == 'E') {
      buffer[n++] = c;
      p->at++;
    } else {
      break;
    }
  }
  buffer[n] = '\0';
  if (n == 0) {
    exj_fail(p, "expected a value");
    return NULL;
  }
  value = exj_new(EXJ_NUMBER);
  if (value) {
    value->number = strtod(buffer, NULL);
  }
  return value;
}

static int exj_push(ExJson *container, char *key, ExJson *item) {
  ExJson **items = realloc(container->items,
                           (container->count + 1) * sizeof(ExJson *));
  if (!items) {
    return 0;
  }
  container->items = items;
  if (container->kind == EXJ_OBJECT) {
    char **keys =
        realloc(container->keys, (container->count + 1) * sizeof(char *));
    if (!keys) {
      return 0;
    }
    container->keys = keys;
    container->keys[container->count] = key;
  }
  container->items[container->count++] = item;
  return 1;
}

static ExJson *exj_array(ExJsonParser *p, int depth) {
  ExJson *array = exj_new(EXJ_ARRAY);
  p->at++;
  exj_space(p);
  if (p->at < p->length && p->text[p->at] == ']') {
    p->at++;
    return array;
  }
  while (array && !p->failed) {
    ExJson *item = exj_value(p, depth + 1);
    if (!item || !exj_push(array, NULL, item)) {
      ex_json_free(item);
      break;
    }
    exj_space(p);
    if (p->at < p->length && p->text[p->at] == ',') {
      p->at++;
      continue;
    }
    if (p->at < p->length && p->text[p->at] == ']') {
      p->at++;
      return array;
    }
    exj_fail(p, "expected ',' or ']'");
  }
  exj_fail(p, "bad array");
  ex_json_free(array);
  return NULL;
}

static ExJson *exj_object(ExJsonParser *p, int depth) {
  ExJson *object = exj_new(EXJ_OBJECT);
  p->at++;
  exj_space(p);
  if (p->at < p->length && p->text[p->at] == '}') {
    p->at++;
    return object;
  }
  while (object && !p->failed) {
    char *key;
    ExJson *item;
    exj_space(p);
    if (p->at >= p->length || p->text[p->at] != '"') {
      exj_fail(p, "expected a key");
      break;
    }
    key = exj_string(p);
    if (!key) {
      break;
    }
    exj_space(p);
    if (p->at >= p->length || p->text[p->at] != ':') {
      free(key);
      exj_fail(p, "expected ':'");
      break;
    }
    p->at++;
    item = exj_value(p, depth + 1);
    if (!item || !exj_push(object, key, item)) {
      free(key);
      ex_json_free(item);
      break;
    }
    exj_space(p);
    if (p->at < p->length && p->text[p->at] == ',') {
      p->at++;
      continue;
    }
    if (p->at < p->length && p->text[p->at] == '}') {
      p->at++;
      return object;
    }
    exj_fail(p, "expected ',' or '}'");
  }
  exj_fail(p, "bad object");
  ex_json_free(object);
  return NULL;
}

static ExJson *exj_value(ExJsonParser *p, int depth) {
  ExJson *value = NULL;
  if (depth > EXJ_MAX_DEPTH) {
    exj_fail(p, "nested too deeply");
    return NULL;
  }
  exj_space(p);
  if (p->at >= p->length) {
    exj_fail(p, "unexpected end");
    return NULL;
  }
  switch (p->text[p->at]) {
  case '{':
    return exj_object(p, depth);
  case '[':
    return exj_array(p, depth);
  case '"': {
    char *text = exj_string(p);
    if (!text) {
      return NULL;
    }
    value = exj_new(EXJ_STRING);
    if (value) {
      value->string = text;
    } else {
      free(text);
    }
    return value;
  }
  case 't':
  case 'f': {
    int truth = p->text[p->at] == 't';
    if (exj_literal(p, truth ? "true" : "false")) {
      value = exj_new(EXJ_BOOL);
      if (value) {
        value->boolean = truth;
      }
      return value;
    }
    exj_fail(p, "bad literal");
    return NULL;
  }
  case 'n':
    if (exj_literal(p, "null")) {
      return exj_new(EXJ_NULL);
    }
    exj_fail(p, "bad literal");
    return NULL;
  default:
    return exj_number(p);
  }
}

ExJson *ex_json_parse(const char *text, size_t length, char *error,
                      size_t error_size) {
  ExJsonParser p = {text, length, 0, error, error_size, 0};
  ExJson *root;
  if (error && error_size > 0) {
    error[0] = '\0';
  }
  if (!text) {
    exj_fail(&p, "no text");
    return NULL;
  }
  root = exj_value(&p, 0);
  exj_space(&p);
  if (root && p.at != p.length) {
    exj_fail(&p, "trailing text");
  }
  if (p.failed) {
    ex_json_free(root);
    return NULL;
  }
  return root;
}

void ex_json_free(ExJson *value) {
  if (!value) {
    return;
  }
  for (size_t i = 0; i < value->count; i++) {
    ex_json_free(value->items[i]);
    if (value->keys) {
      free(value->keys[i]);
    }
  }
  free(value->items);
  free(value->keys);
  free(value->string);
  free(value);
}

const ExJson *ex_json_get(const ExJson *object, const char *key) {
  if (!object || object->kind != EXJ_OBJECT || !key) {
    return NULL;
  }
  for (size_t i = 0; i < object->count; i++) {
    if (strcmp(object->keys[i], key) == 0) {
      return object->items[i];
    }
  }
  return NULL;
}

const ExJson *ex_json_at(const ExJson *array, size_t index) {
  if (!array || array->kind != EXJ_ARRAY || index >= array->count) {
    return NULL;
  }
  return array->items[index];
}

size_t ex_json_count(const ExJson *value) {
  return value && value->kind == EXJ_ARRAY ? value->count : 0;
}

const char *ex_json_text(const ExJson *object, const char *key) {
  const ExJson *value = ex_json_get(object, key);
  return value && value->kind == EXJ_STRING ? value->string : NULL;
}

long long ex_json_int(const ExJson *object, const char *key,
                      long long fallback) {
  const ExJson *value = ex_json_get(object, key);
  return value && value->kind == EXJ_NUMBER ? (long long)value->number
                                            : fallback;
}

int ex_json_flag(const ExJson *object, const char *key) {
  const ExJson *value = ex_json_get(object, key);
  return value && value->kind == EXJ_BOOL && value->boolean;
}
