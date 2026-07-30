/* mtlc_lower_module.h - populate the backend IR's own type/symbol tables.
 *
 * FRONTEND-side adapter (links into the driver, not libmtlc). After the AST is
 * lowered to IR, this fills the backend-owned module tables on IRProgram (the
 * name->MtlcType type registry and the module symbol table with global vars,
 * functions, externs, and folded constants) so the code generators no longer
 * need the frontend TypeChecker, SymbolTable, or AST. */
#ifndef MTLC_LOWER_MODULE_H
#define MTLC_LOWER_MODULE_H

#include "ir/ir.h"
#include "parser/ast.h"
#include "semantic/symbol_table.h"
#include "semantic/type_checker.h"

/* Populate program->type_registry and program->module_symbols from the frontend.
 * `ast_program` is the AST_PROGRAM root (for global initializers), `tc`/`st` the
 * type checker and symbol table. Safe to call once, after ir_lower_program. */
void mtlc_lower_populate_module(IRProgram *program, ASTNode *ast_program,
                                TypeChecker *tc, SymbolTable *st);

#endif /* MTLC_LOWER_MODULE_H */
