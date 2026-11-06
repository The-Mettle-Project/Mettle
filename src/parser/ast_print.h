#ifndef AST_PRINT_H
#define AST_PRINT_H

#include "ast.h"
#include <stdio.h>

typedef const char *(*AstPrintAnnotator)(void *context, const ASTNode *block);
typedef const char *(*AstPrintNote)(const ASTNode *node);
typedef int (*AstPrintFilter)(const ASTNode *declaration, const void *context);

void ast_print_set_filter(AstPrintFilter keep, const void *context);

size_t ast_print_program(FILE *out, const ASTNode *program,
                         AstPrintAnnotator annotate, void *context);
void ast_print_set_notes(AstPrintNote statement_note,
                         AstPrintNote block_end_note);
void ast_print_statement_at(FILE *out, const ASTNode *statement, int depth);

#endif
