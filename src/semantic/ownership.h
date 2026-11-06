#ifndef OWNERSHIP_H
#define OWNERSHIP_H

#include <stddef.h>
#include <stdio.h>
#include "parser/ast.h"
#include "semantic/type_checker.h"

typedef enum {
  OWN_RECORD_SCOPE_DROP,
  OWN_RECORD_PARAM_DROP,
  OWN_RECORD_ARM_DROP,
  OWN_RECORD_ASSIGN_DROP,
  OWN_RECORD_DISCARD_DROP,
  OWN_RECORD_TEMPORARY,
  OWN_RECORD_GLUE,
  OWN_RECORD_MOVE,
  OWN_RECORD_LIVE_BIT,
  OWN_RECORD_DROP_RUN,
  OWN_RECORD_KIND_COUNT
} OwnRecordKind;

typedef struct {
  OwnRecordKind kind;
  SourceLocation at;
  SourceLocation origin;
  char *function;
  char *subject;
  char *type_name;
  char *text;
  char *reason;
  char *compiled_to;
  int inserted;
  int written;
  const void *key;
} OwnRecord;

#define OWN_PATH_MAX 8

typedef struct {
  int path[OWN_PATH_MAX];
  int depth;
  struct Type *type;
  int guard;
} OwnDropStep;

typedef struct {
  OwnDropStep *steps;
  size_t step_count;
  int unreachable;
} OwnDropPlan;

typedef struct {
  int bit;
  int value;
  int source;
} OwnBitWrite;

int ownership_run(TypeChecker *checker, ASTNode *program,
                  ErrorReporter *reporter);
void ownership_reset(void);

int ownership_type_owns(TypeChecker *checker, struct Type *type);
const char *ownership_drop_callee(TypeChecker *checker, struct Type *type);
int ownership_is_destructor_name(TypeChecker *checker, const char *name);

const OwnDropPlan *ownership_drop_plan(const ASTNode *drop_call,
                                       const ASTNode *exit);
int ownership_call_is_planned(const ASTNode *drop_call);
const OwnBitWrite *ownership_unit_writes(const void *unit, size_t *count);
const int *ownership_function_bits(const ASTNode *function, size_t *count);
const char *ownership_bit_name(int bit);
int ownership_bit_count(void);

const char *ownership_note(const ASTNode *node);
const char *ownership_block_end_note(const ASTNode *block);
int ownership_node_is_inserted(const ASTNode *node);
int ownership_apply_fixes_enabled(void);

size_t ownership_record_count(void);
const OwnRecord *ownership_record_at(size_t index);
void ownership_note_compiled(const ASTNode *drop_call, const char *what);
void ownership_publish_explain(void);

void ownership_set_report_focus(const char *focus);
void ownership_print_report(FILE *out);
void ownership_print_json(FILE *out, int leading_comma);
int ownership_write_fixes(FILE *out);
int ownership_print_fixed_source(FILE *out, const char *path);
void ownership_print_editor_json(FILE *out);
void ownership_set_apply_fixes(int enabled);

#endif
