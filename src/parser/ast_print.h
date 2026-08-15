#ifndef AST_PRINT_H
#define AST_PRINT_H

#include "ast.h"
#include <stdio.h>

/* Print `program` as Mettle source. Used by `mettle expand` to show what
 * compile-time expansion produced, in the language it produced it in.
 *
 * Returns the number of nodes with no faithful source spelling, each of which
 * was printed as a marked comment rather than guessed at. A non-zero return
 * means the output is not a complete program, and the caller should say so. */
/* Optional hook returning a one-line description of what generated `block`,
 * or NULL for ordinary source. Kept as a callback so this printer does not
 * have to know what a type checker is. */
typedef const char *(*AstPrintAnnotator)(void *context, const ASTNode *block);

size_t ast_print_program(FILE *out, const ASTNode *program,
                         AstPrintAnnotator annotate, void *context);

#endif // AST_PRINT_H
