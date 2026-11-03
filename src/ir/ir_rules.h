#ifndef IR_RULES_H
#define IR_RULES_H

#include "ir.h"
#include "../error/error_reporter.h"
#include <stddef.h>
#include <stdio.h>

typedef struct {
  const char *file;
  size_t line;
  size_t column;
} IRRuleSite;

typedef struct {
  unsigned char *bytes;
  size_t size;
  size_t *pointer_offsets;
  size_t pointer_count;
  IRRuleSite *sites;
  size_t site_count;
  size_t function_count;
  size_t type_count;
} IRRuleImage;

void ir_rule_image_free(IRRuleImage *image);

struct IRInterpMachine;
int ir_rule_image_place(struct IRInterpMachine *machine,
                        const IRRuleImage *image,
                        unsigned long long *out_address);

typedef struct {
  size_t rules;
  size_t passed;
  size_t failed;
  size_t gaps;
  size_t malformed;
  long long steps;
} IRRuleStats;

typedef enum {
  IR_RULE_OVER_PROGRAM,
  IR_RULE_OVER_MACHINE,
  IR_RULE_OVER_TRACE
} IRRuleKind;

void ir_rules_set_apply_fixes(int on);
size_t ir_rules_proposal_count(void);
int ir_rules_apply_fixes(FILE *out);

IRRuleKind ir_rule_kind(const IRFunction *rule);
int ir_program_has_rules(const IRProgram *program);
int ir_program_has_rules_of(const IRProgram *program, IRRuleKind kind);

int ir_rules_run(IRProgram *program, const IRRuleImage *image,
                 ErrorReporter *reporter, FILE *report, long long budget,
                 IRRuleStats *stats);

int ir_rules_run_checked(IRProgram *program, const IRRuleImage *image,
                         ErrorReporter *reporter, FILE *report,
                         long long budget, int cross_check,
                         IRRuleStats *stats);

int ir_rules_run_kind(IRProgram *program, const IRRuleImage *image,
                      ErrorReporter *reporter, FILE *report, long long budget,
                      int cross_check, IRRuleKind kind, IRRuleStats *stats);

#endif
