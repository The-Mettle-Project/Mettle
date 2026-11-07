#ifndef IR_EXPLAIN_SAFETY_H
#define IR_EXPLAIN_SAFETY_H

#include <stddef.h>

typedef enum {
  IR_SAFETY_SURVIVOR_EXTENT = 0,
  IR_SAFETY_SURVIVOR_REGION = 1,
  IR_SAFETY_SURVIVOR_SPAN = 2
} IRSafetySurvivorKind;

typedef enum {
  IR_SAFETY_SETTLED_PROVED = 0,
  IR_SAFETY_SETTLED_HOISTED = 1,
  IR_SAFETY_SETTLED_EXEMPT = 2
} IRSafetySettledKind;

void ir_explain_safety_set_collect(int enabled, const char *focus_file);

void ir_explain_safety_settled(const char *file, IRSafetySettledKind kind);

void ir_explain_safety_note(const char *file, size_t line,
                            const char *function_name,
                            IRSafetySurvivorKind kind);

void ir_explain_safety_totals(size_t emitted, size_t proved, size_t hoisted,
                              size_t spanned, size_t exempt,
                              size_t extent_tests, size_t region_calls);

void ir_explain_safety_typed_note(const char *file, size_t line,
                                  const char *function_name,
                                  const char *type_name, long long min,
                                  long long max, size_t length);

#endif
