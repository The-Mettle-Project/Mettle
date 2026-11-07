#ifndef IR_EXPLAIN_OWNERSHIP_H
#define IR_EXPLAIN_OWNERSHIP_H

#include <stddef.h>

typedef struct {
  const char *file;
  size_t line;
  size_t column;
  const char *kind;
  const char *function;
  const char *subject;
  const char *type;
  const char *text;
  const char *reason;
  const char *lowered;
  int inserted;
  const void *key;
} IRExplainOwnershipRecord;

void ir_explain_ownership_set_collect(int enabled, const char *focus_file);
void ir_explain_ownership_note(const IRExplainOwnershipRecord *record);
void ir_explain_ownership_scan(const void *ir_program);
void ir_explain_attribute_remarks(const void *ir_program);
void ir_explain_set_expanded(char *text);

#endif
