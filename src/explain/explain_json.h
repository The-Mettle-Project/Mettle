#ifndef METTLE_EXPLAIN_JSON_H
#define METTLE_EXPLAIN_JSON_H

#include <stddef.h>

typedef enum {
  EXJ_NULL,
  EXJ_BOOL,
  EXJ_NUMBER,
  EXJ_STRING,
  EXJ_ARRAY,
  EXJ_OBJECT
} ExJsonKind;

typedef struct ExJson {
  ExJsonKind kind;
  double number;
  int boolean;
  char *string;
  char **keys;
  struct ExJson **items;
  size_t count;
} ExJson;

ExJson *ex_json_parse(const char *text, size_t length, char *error,
                      size_t error_size);
void ex_json_free(ExJson *value);

const ExJson *ex_json_get(const ExJson *object, const char *key);
const ExJson *ex_json_at(const ExJson *array, size_t index);
size_t ex_json_count(const ExJson *value);
const char *ex_json_text(const ExJson *object, const char *key);
long long ex_json_int(const ExJson *object, const char *key,
                      long long fallback);
int ex_json_flag(const ExJson *object, const char *key);

#endif
