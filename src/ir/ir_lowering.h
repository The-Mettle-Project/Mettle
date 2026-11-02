#ifndef IR_LOWERING_H
#define IR_LOWERING_H

#include "ir.h"
#include "../parser/ast.h"
#include "../semantic/symbol_table.h"
#include "../semantic/type_checker.h"

IRProgram *ir_lower_program(ASTNode *program, TypeChecker *type_checker,
                            SymbolTable *symbol_table, char **error_message,
                            int emit_runtime_checks, int emit_safety_checks);

void ir_lowering_set_explain(int enabled);

void ir_lowering_set_refinement_checks(int enabled);
void ir_lowering_set_task_checks(int enabled);
void ir_lowering_set_live(int enabled);
char *ir_lower_live_descriptor(const MtlcType *type);
size_t ir_lower_live_type_layouts(const char *const **names,
                                  const char *const **layouts);
void ir_lowering_set_overflow_checks(int enabled);
void ir_lowering_set_assume_no_signed_overflow(int enabled);
void ir_lowering_overflow_totals(size_t *emitted, size_t *proved);

#endif
