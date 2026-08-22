#ifndef AST_DUMP_H
#define AST_DUMP_H

#include "ast.h"
#include <stdio.h>

int ast_dump_program(FILE *out, const ASTNode *program);

#endif
