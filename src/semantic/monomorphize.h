#ifndef MONOMORPHIZE_H
#define MONOMORPHIZE_H

#include "../error/error_reporter.h"
#include "../parser/ast.h"

int monomorphize_program(ASTNode *program, ErrorReporter *reporter);

/* Lift anonymous `fn(...) { }` lambda expressions to top-level functions. Runs
 * after monomorphization (so generic bodies are already concrete) and before
 * type checking. */
int closure_convert_program(ASTNode *program, ErrorReporter *reporter);

#endif // MONOMORPHIZE_H
