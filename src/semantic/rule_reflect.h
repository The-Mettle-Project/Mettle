#ifndef RULE_REFLECT_H
#define RULE_REFLECT_H

#include "type_checker.h"
#include "../ir/ir_rules.h"
#include "../ir/ir_effects.h"

const char *rule_reflect_strip_import_prefix(const char *name);

int rule_reflect_build_function(TypeChecker *checker, IRProgram *ir,
                                const char *name, const char *inner_name,
                                IRRuleImage *out, char **error_message);

int rule_reflect_build(TypeChecker *checker, ASTNode *program,
                       const char *root_file, const char *target,
                       const IREffectResults *effects, IRRuleImage *out,
                       char **error_message);

int rule_reflect_build_trace(TypeChecker *checker, const char *root_file,
                             IRRuleImage *out, char **error_message);

int rule_reflect_build_machine(TypeChecker *checker, const IRProgram *program,
                               const char *root_file, const char *target,
                               const IREffectResults *effects,
                               IRRuleImage *out, char **error_message);

#endif
