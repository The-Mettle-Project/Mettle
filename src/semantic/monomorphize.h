#ifndef MONOMORPHIZE_H
#define MONOMORPHIZE_H

#include "../error/error_reporter.h"
#include "../parser/ast.h"

/* A struct method lifts to two free functions, one per receiver form: `S_m`
 * takes the struct by value, and `S_m` with this suffix takes `S*`. The type
 * checker appends it when the receiver was written as a pointer. */
#define MONO_PTR_RECEIVER_SUFFIX "__ptr"

int monomorphize_program(ASTNode *program, ErrorReporter *reporter);

/* Lift anonymous `fn(...) { }` lambda expressions to top-level functions. Runs
 * after monomorphization (so generic bodies are already concrete) and before
 * type checking. */
int closure_convert_program(ASTNode *program, ErrorReporter *reporter);

/* Wrap thin function values (`&func`, non-capturing lambdas) flowing into an
 * `Fn(...)->R` closure boundary (var declaration, return, or call argument to a
 * plain top-level function) in a generated adapter, so a closure-typed slot can
 * accept a plain function. Runs after closure_convert_program (lambdas must
 * already be lifted) and before type checking. */
int closure_adapt_program(ASTNode *program, ErrorReporter *reporter);

#endif // MONOMORPHIZE_H
