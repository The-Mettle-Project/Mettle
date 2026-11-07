#include "ast_print.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  FILE *out;
  int depth;
  size_t unprintable;
  AstPrintAnnotator annotate;
  void *context;
  int skip_indent;
  const ASTNode *inline_end;
} AstPrinter;

static void print_statement(AstPrinter *printer, const ASTNode *node);
static void print_expression(AstPrinter *printer, const ASTNode *node);
static void print_number_literal(AstPrinter *printer,
                                 const NumberLiteral *literal);
static void print_member_access(AstPrinter *printer,
                                const MemberAccess *member);
static void print_call(AstPrinter *printer, const CallExpression *call);
static void print_aggregate(AstPrinter *printer,
                            const AggregateLiteral *literal);
static void print_block(AstPrinter *printer, const ASTNode *node);
static void print_match_statement(AstPrinter *printer, const ASTNode *node);
static void print_switch_statement(AstPrinter *printer, const ASTNode *node);
static void print_declaration_text(AstPrinter *printer,
                                   const VarDeclaration *decl);
static void print_assignment_text(AstPrinter *printer,
                                  const Assignment *assign);

static AstPrintNote g_statement_note = NULL;
static AstPrintNote g_block_end_note = NULL;
static int g_line_tags = 0;
static size_t g_tag_line = 0;

void ast_print_set_line_tags(int enabled) {
  g_line_tags = enabled;
  g_tag_line = 0;
}
static AstPrintFilter g_keep_declaration = NULL;
static const void *g_keep_context = NULL;

void ast_print_set_notes(AstPrintNote statement_note,
                         AstPrintNote block_end_note) {
  g_statement_note = statement_note;
  g_block_end_note = block_end_note;
}

static void print_indent(AstPrinter *printer);

static void print_note_line(AstPrinter *printer, const char *note) {
  if (!note) {
    return;
  }
  print_indent(printer);
  fprintf(printer->out, "// %s\n", note);
}

static void print_block_end_note(AstPrinter *printer, const ASTNode *block) {
  if (g_block_end_note) {
    printer->depth++;
    print_note_line(printer, g_block_end_note(block));
    printer->depth--;
  }
}

static void print_inline_statement(AstPrinter *printer, const ASTNode *node) {
  const ASTNode *saved = printer->inline_end;
  printer->skip_indent = 1;
  printer->inline_end = node;
  print_statement(printer, node);
  printer->skip_indent = 0;
  printer->inline_end = saved;
}

static void print_value_scope(AstPrinter *printer, const ValueScope *scope) {
  if (!scope || !scope->body) {
    fputs("{ }", printer->out);
    return;
  }
  if (scope->label) {
    fprintf(printer->out, "%s: ", scope->label);
  }
  const Program *block = scope->body->type == AST_PROGRAM
                             ? (const Program *)scope->body->data
                             : NULL;
  if (scope->kind == VALUE_SCOPE_LOOP && block &&
      block->declaration_count == 2) {
    print_inline_statement(printer, block->declarations[0]);
    fputs(" else", printer->out);
    print_block(printer, block->declarations[1]);
    return;
  }
  print_inline_statement(printer, scope->body);
}

static void print_indent(AstPrinter *printer) {
  if (printer->skip_indent) {
    printer->skip_indent = 0;
    return;
  }
  if (g_line_tags) {
    fprintf(printer->out, "\x01%zu\x02", g_tag_line);
  }
  for (int i = 0; i < printer->depth; i++) {
    fputs("    ", printer->out);
  }
}

static void print_line_end(AstPrinter *printer, const ASTNode *node) {
  if (printer->inline_end == node) {
    printer->inline_end = NULL;
    return;
  }
  fputc('\n', printer->out);
}

static void print_provenance(AstPrinter *printer, const ASTNode *block) {
  const char *note =
      printer->annotate ? printer->annotate(printer->context, block) : NULL;
  if (!note) {
    return;
  }
  print_indent(printer);
  fprintf(printer->out, "// %s\n", note);
}

static void print_meta_decorators(AstPrinter *printer,
                                  const MetaDecorator *decorators,
                                  size_t count) {
  for (size_t d = 0; d < count; d++) {
    fprintf(printer->out, "@%s", decorators[d].name);
    if (decorators[d].arg_count > 0) {
      fputc('(', printer->out);
      for (size_t a = 0; a < decorators[d].arg_count; a++) {
        fprintf(printer->out, "%s%s", a ? ", " : "", decorators[d].args[a]);
      }
      fputc(')', printer->out);
    }
    fputc(' ', printer->out);
  }
}

static void print_unprintable(AstPrinter *printer, const char *what) {
  fprintf(printer->out, "/* <mettle expand: no source form for %s> */", what);
  printer->unprintable++;
}

static void print_string_body(AstPrinter *printer, const char *value);

static void print_string_literal(AstPrinter *printer, const char *value) {
  fputc('"', printer->out);
  print_string_body(printer, value);
  fputc('"', printer->out);
}

static int print_is_interp_call(const ASTNode *node) {
  const CallExpression *call = node && node->type == AST_FUNCTION_CALL
                                   ? (const CallExpression *)node->data
                                   : NULL;
  return call && !call->object && call->function_name &&
         call->argument_count == 1 &&
         strcmp(call->function_name, "__mtl_interp") == 0;
}

static int print_same_place(const ASTNode *a, const ASTNode *b) {
  return a->location.line == b->location.line &&
         a->location.column == b->location.column &&
         a->location.filename == b->location.filename;
}

static int print_interp_chain(const ASTNode *node, const ASTNode *origin,
                              int *calls) {
  if (!node || !print_same_place(node, origin)) {
    return 0;
  }
  if (node->type == AST_STRING_LITERAL) {
    return 1;
  }
  if (print_is_interp_call(node)) {
    (*calls)++;
    return 1;
  }
  if (node->type != AST_BINARY_EXPRESSION || !node->data) {
    return 0;
  }
  const BinaryExpression *binary = (const BinaryExpression *)node->data;
  return binary->operator && strcmp(binary->operator, "+") == 0 &&
         print_interp_chain(binary->left, origin, calls) &&
         print_interp_chain(binary->right, origin, calls);
}

static void print_interp_pieces(AstPrinter *printer, const ASTNode *node) {
  if (node->type == AST_BINARY_EXPRESSION) {
    const BinaryExpression *binary = (const BinaryExpression *)node->data;
    print_interp_pieces(printer, binary->left);
    print_interp_pieces(printer, binary->right);
  } else if (node->type == AST_STRING_LITERAL) {
    const StringLiteral *literal = (const StringLiteral *)node->data;
    print_string_body(printer, literal ? literal->value : "");
  } else {
    fputc('{', printer->out);
    print_expression(printer,
                     ((const CallExpression *)node->data)->arguments[0]);
    fputc('}', printer->out);
  }
}

static int print_interpolation(AstPrinter *printer, const ASTNode *node) {
  int calls = 0;
  if (!print_interp_chain(node, node, &calls) || calls == 0) {
    return 0;
  }
  fputc('"', printer->out);
  print_interp_pieces(printer, node);
  fputc('"', printer->out);
  return 1;
}

static void print_string_body(AstPrinter *printer, const char *value) {
  for (const char *p = value ? value : ""; *p; p++) {
    if (*p == '{') {
      fputs("{{", printer->out);
      continue;
    }
    switch (*p) {
    case '"':
      fputs("\\\"", printer->out);
      break;
    case '\\':
      fputs("\\\\", printer->out);
      break;
    case '\n':
      fputs("\\n", printer->out);
      break;
    case '\t':
      fputs("\\t", printer->out);
      break;
    case '\r':
      fputs("\\r", printer->out);
      break;
    default:
      fputc(*p, printer->out);
      break;
    }
  }
}

static void print_new_expression(AstPrinter *printer,
                                 const NewExpression *expr) {
  fprintf(printer->out, "new %s",
          expr && expr->type_name ? expr->type_name : "<type>");
  if (expr && expr->count) {
    fputc('[', printer->out);
    print_expression(printer, expr->count);
    for (size_t i = 0; i < expr->extent_count; i++) {
      fputs(", ", printer->out);
      print_expression(printer, expr->extents[i]);
    }
    fputc(']', printer->out);
  }
}

static void print_expression(AstPrinter *printer, const ASTNode *node) {
  if (!node) {
    fputs("/* <null> */", printer->out);
    return;
  }

  switch (node->type) {
  case AST_IDENTIFIER: {
    const Identifier *id = (const Identifier *)node->data;
    fputs(id && id->name ? id->name : "<anonymous>", printer->out);
    break;
  }
  case AST_NUMBER_LITERAL:
    print_number_literal(printer, (const NumberLiteral *)node->data);
    break;
  case AST_STRING_LITERAL: {
    const StringLiteral *literal = (const StringLiteral *)node->data;
    print_string_literal(printer, literal ? literal->value : "");
    break;
  }
  case AST_BINARY_EXPRESSION: {
    const BinaryExpression *binary = (const BinaryExpression *)node->data;
    if (!binary) {
      print_unprintable(printer, "binary expression");
      break;
    }
    if (print_interpolation(printer, node)) {
      break;
    }
    fputc('(', printer->out);
    print_expression(printer, binary->left);
    fprintf(printer->out, " %s ", binary->operator ? binary->operator : "?");
    print_expression(printer, binary->right);
    fputc(')', printer->out);
    break;
  }
  case AST_UNARY_EXPRESSION: {
    const UnaryExpression *unary = (const UnaryExpression *)node->data;
    if (!unary) {
      print_unprintable(printer, "unary expression");
      break;
    }
    fputs(unary->operator ? unary->operator : "?", printer->out);
    fputc('(', printer->out);
    print_expression(printer, unary->operand);
    fputc(')', printer->out);
    break;
  }
  case AST_MEMBER_ACCESS:
    print_member_access(printer, (const MemberAccess *)node->data);
    break;
  case AST_INDEX_EXPRESSION: {
    const ArrayIndexExpression *index =
        (const ArrayIndexExpression *)node->data;
    if (!index) {
      print_unprintable(printer, "index expression");
      break;
    }
    print_expression(printer, index->array);
    fputc('[', printer->out);
    print_expression(printer, index->index);
    fputc(']', printer->out);
    break;
  }
  case AST_CAST_EXPRESSION: {
    const CastExpression *cast = (const CastExpression *)node->data;
    if (!cast) {
      print_unprintable(printer, "cast");
      break;
    }
    fprintf(printer->out, "(%s)",
            cast->type_name ? cast->type_name : "<type>");
    print_expression(printer, cast->operand);
    break;
  }
  case AST_NEW_EXPRESSION: {
    print_new_expression(printer, (const NewExpression *)node->data);
    break;
  }
  case AST_VALUE_SCOPE:
    print_value_scope(printer, (const ValueScope *)node->data);
    break;
  case AST_FUNCTION_CALL:
    if (!node->data || !print_interpolation(printer, node)) {
      print_call(printer, (const CallExpression *)node->data);
    }
    break;
  case AST_AGGREGATE_LITERAL:
    print_aggregate(printer, (const AggregateLiteral *)node->data);
    break;
  default:
    print_unprintable(printer, "this expression");
    break;
  }
}

static void print_number_literal(AstPrinter *printer,
                                 const NumberLiteral *literal) {
  if (!literal) {
    fputs("0", printer->out);
  } else if (literal->is_float) {
    char spelled[64];
    for (int precision = 6; precision <= 17; precision++) {
      snprintf(spelled, sizeof(spelled), "%.*g", precision,
               literal->float_value);
      if (atof(spelled) == literal->float_value) {
        break;
      }
    }
    fputs(spelled, printer->out);
  } else if (literal->int_radix == 16) {
    fprintf(printer->out, "0x%llx", (unsigned long long)literal->int_value);
  } else {
    fprintf(printer->out, "%lld", literal->int_value);
  }
}

static void print_member_access(AstPrinter *printer,
                                const MemberAccess *member) {
  const UnaryExpression *through = NULL;
  if (!member) {
    print_unprintable(printer, "member access");
    return;
  }
  through = member->object && member->object->type == AST_UNARY_EXPRESSION
                ? (const UnaryExpression *)member->object->data
                : NULL;
  if (through && through->operator && strcmp(through->operator, "*") == 0) {
    print_expression(printer, through->operand);
    fprintf(printer->out, "->%s", member->member ? member->member : "<field>");
    return;
  }
  print_expression(printer, member->object);
  fprintf(printer->out, ".%s", member->member ? member->member : "<field>");
}

static void print_call(AstPrinter *printer, const CallExpression *call) {
  if (!call) {
    print_unprintable(printer, "call");
    return;
  }
  if (call->object) {
    print_expression(printer, call->object);
    fputc('.', printer->out);
  }
  fputs(call->function_name ? call->function_name : "<callee>", printer->out);
  fputc('(', printer->out);
  for (size_t i = 0; i < call->argument_count; i++) {
    if (i) {
      fputs(", ", printer->out);
    }
    if (call->argument_names && call->argument_names[i]) {
      fprintf(printer->out, "%s: ", call->argument_names[i]);
    }
    print_expression(printer, call->arguments[i]);
  }
  fputc(')', printer->out);
}

static void print_aggregate(AstPrinter *printer,
                            const AggregateLiteral *literal) {
  if (!literal) {
    print_unprintable(printer, "aggregate literal");
    return;
  }
  fputs(literal->is_struct ? "{ " : "[", printer->out);
  for (size_t i = 0; i < literal->element_count; i++) {
    if (i) {
      fputs(", ", printer->out);
    }
    if (literal->is_struct && literal->field_names && literal->field_names[i]) {
      fprintf(printer->out, "%s: ", literal->field_names[i]);
    }
    print_expression(printer, literal->elements[i]);
  }
  if (literal->repeat_count) {
    fputs("; ", printer->out);
    print_expression(printer, literal->repeat_count);
  }
  fputs(literal->is_struct ? " }" : "]", printer->out);
}

static void print_block(AstPrinter *printer, const ASTNode *node) {
  if (!node || node->type != AST_PROGRAM) {
    fputs(" {\n", printer->out);
    printer->depth++;
    print_statement(printer, node);
    printer->depth--;
    print_indent(printer);
    fputs("}", printer->out);
    return;
  }
  fputs(" {\n", printer->out);
  printer->depth++;
  for (size_t i = 0; i < node->child_count; i++) {
    print_statement(printer, node->children[i]);
  }
  printer->depth--;
  print_block_end_note(printer, node);
  print_indent(printer);
  fputs("}", printer->out);
}

static int print_names(const ASTNode *node, const char *name) {
  return node && node->type == AST_IDENTIFIER && node->data &&
         ((const Identifier *)node->data)->name &&
         strcmp(((const Identifier *)node->data)->name, name) == 0;
}

static int print_range_header(AstPrinter *printer, const ForStatement *loop) {
  const VarDeclaration *decl =
      loop->initializer && loop->initializer->type == AST_VAR_DECLARATION
          ? (const VarDeclaration *)loop->initializer->data
          : NULL;
  const BinaryExpression *bound =
      loop->condition && loop->condition->type == AST_BINARY_EXPRESSION
          ? (const BinaryExpression *)loop->condition->data
          : NULL;
  const Assignment *step =
      loop->increment && loop->increment->type == AST_ASSIGNMENT
          ? (const Assignment *)loop->increment->data
          : NULL;
  const BinaryExpression *next =
      step && step->value && step->value->type == AST_BINARY_EXPRESSION
          ? (const BinaryExpression *)step->value->data
          : NULL;
  const NumberLiteral *one =
      next && next->right && next->right->type == AST_NUMBER_LITERAL
          ? (const NumberLiteral *)next->right->data
          : NULL;
  if (!decl || !decl->name || !decl->initializer || decl->is_const ||
      !bound || !bound->operator || !step || step->target ||
      !step->variable_name || !next || !next->operator || !one ||
      one->is_float || one->int_value != 1 ||
      strcmp(step->variable_name, decl->name) != 0 ||
      strcmp(next->operator, "+") != 0 ||
      !print_names(next->left, decl->name) ||
      !print_names(bound->left, decl->name) ||
      (strcmp(bound->operator, "<") != 0 &&
       strcmp(bound->operator, "<=") != 0)) {
    return 0;
  }
  fprintf(printer->out, "for %s", decl->name);
  if (decl->type_name) {
    fprintf(printer->out, ": %s", decl->type_name);
  }
  fputs(" in ", printer->out);
  print_expression(printer, decl->initializer);
  fputs(strcmp(bound->operator, "<=") == 0 ? "..=" : "..", printer->out);
  print_expression(printer, bound->right);
  return 1;
}

static void print_for_header(AstPrinter *printer, const ForStatement *loop) {
  fputs("for (", printer->out);
  if (loop->initializer &&
      loop->initializer->type == AST_VAR_DECLARATION &&
      loop->initializer->data) {
    print_declaration_text(printer,
                           (const VarDeclaration *)loop->initializer->data);
  } else if (loop->initializer &&
             loop->initializer->type == AST_ASSIGNMENT &&
             loop->initializer->data) {
    print_assignment_text(printer,
                          (const Assignment *)loop->initializer->data);
  } else if (loop->initializer) {
    print_expression(printer, loop->initializer);
  }
  fputs("; ", printer->out);
  if (loop->condition) {
    print_expression(printer, loop->condition);
  }
  fputs("; ", printer->out);
  if (loop->increment && loop->increment->type == AST_ASSIGNMENT &&
      loop->increment->data) {
    print_assignment_text(printer, (const Assignment *)loop->increment->data);
  } else if (loop->increment) {
    print_expression(printer, loop->increment);
  }
  fputc(')', printer->out);
}

static void print_loop_statement(AstPrinter *printer,
                                 const ASTNode *node) {
  switch (node->type) {
case AST_WHILE_STATEMENT: {
  const WhileStatement *loop = (const WhileStatement *)node->data;
  if (!loop) {
    break;
  }
  print_indent(printer);
  if (loop->label) {
    fprintf(printer->out, "%s: ", loop->label);
  }
  fputs("while (", printer->out);
  print_expression(printer, loop->condition);
  fputc(')', printer->out);
  print_block(printer, loop->body);
  print_line_end(printer, node);
  break;
}

case AST_FOR_STATEMENT: {
  const ForStatement *loop = (const ForStatement *)node->data;
  if (!loop) {
    break;
  }
  print_indent(printer);
  if (loop->label) {
    fprintf(printer->out, "%s: ", loop->label);
  }
  if (!print_range_header(printer, loop)) {
    print_for_header(printer, loop);
  }
  print_block(printer, loop->body);
  print_line_end(printer, node);
  break;
}

case AST_COMPTIME_FOR: {
  const ComptimeForStatement *directive =
      (const ComptimeForStatement *)node->data;
  print_indent(printer);
  fprintf(printer->out, "comptime for %s in ",
          directive && directive->binding_name ? directive->binding_name
                                               : "<binding>");
  print_expression(printer, directive ? directive->sequence : NULL);
  if (directive && directive->range_end) {
    fputs(directive->range_inclusive ? "..=" : "..", printer->out);
    print_expression(printer, directive->range_end);
  }
  print_block(printer, directive ? directive->body : NULL);
  fputc('\n', printer->out);
  break;
}
  default:
    break;
  }
}

static void print_declaration_text(AstPrinter *printer,
                                   const VarDeclaration *decl) {
  fprintf(printer->out, "%s %s", decl->is_const ? "const" : "var",
          decl->name ? decl->name : "<name>");
  if (decl->type_name) {
    fprintf(printer->out, ": %s", decl->type_name);
  }
  if (decl->initializer) {
    fputs(" = ", printer->out);
    print_expression(printer, decl->initializer);
  }
}

static void print_assignment_text(AstPrinter *printer,
                                  const Assignment *assign) {
  if (assign->target) {
    print_expression(printer, assign->target);
  } else {
    fputs(assign->variable_name ? assign->variable_name : "<target>",
          printer->out);
  }
  fputs(" = ", printer->out);
  print_expression(printer, assign->value);
}

static void print_if_statement(AstPrinter *printer, const ASTNode *node) {
  const IfStatement *branch = (const IfStatement *)node->data;
  if (!branch) {
    return;
  }
  print_indent(printer);
  fputs("if (", printer->out);
  print_expression(printer, branch->condition);
  fputc(')', printer->out);
  print_block(printer, branch->then_branch);
  for (size_t i = 0; i < branch->else_if_count; i++) {
    fputs(" else if (", printer->out);
    print_expression(printer, branch->else_ifs[i].condition);
    fputc(')', printer->out);
    print_block(printer, branch->else_ifs[i].body);
  }
  if (branch->else_branch) {
    fputs(" else", printer->out);
    print_block(printer, branch->else_branch);
  }
  print_line_end(printer, node);
}

static void print_loop_control(AstPrinter *printer, const ASTNode *node) {
  const LoopControlStatement *control =
      (const LoopControlStatement *)node->data;
  print_indent(printer);
  fputs(node->type == AST_BREAK_STATEMENT ? "break" : "continue",
        printer->out);
  if (control && control->target_label) {
    fprintf(printer->out, " %s", control->target_label);
  }
  if (control && control->value) {
    fputc(' ', printer->out);
    print_expression(printer, control->value);
  }
  fputs(";\n", printer->out);
}

static void print_yield_statement(AstPrinter *printer,
                                  const YieldStatement *yield) {
  print_indent(printer);
  if (yield && yield->from_break) {
    fputs("break", printer->out);
    if (yield->label) {
      fprintf(printer->out, " %s", yield->label);
    }
    if (yield->value) {
      fputc(' ', printer->out);
      print_expression(printer, yield->value);
    }
    fputs(";\n", printer->out);
    return;
  }
  if (yield && yield->value) {
    print_expression(printer, yield->value);
  }
  fputc('\n', printer->out);
}

static void print_statement(AstPrinter *printer, const ASTNode *node) {
  int inline_form = 0;
  if (!node) {
    return;
  }
  inline_form = printer->inline_end == node;
  if (node->location.line) {
    g_tag_line = node->location.line;
  }
  if (g_statement_note && !inline_form) {
    print_note_line(printer, g_statement_note(node));
  }

  switch (node->type) {
  case AST_PROGRAM:
    if (!inline_form) {
      print_provenance(printer, node);
    }
    print_indent(printer);
    fputs("{\n", printer->out);
    printer->depth++;
    for (size_t i = 0; i < node->child_count; i++) {
      print_statement(printer, node->children[i]);
    }
    printer->depth--;
    print_block_end_note(printer, node);
    print_indent(printer);
    fputs("}", printer->out);
    print_line_end(printer, node);
    break;

  case AST_VAR_DECLARATION: {
    const VarDeclaration *decl = (const VarDeclaration *)node->data;
    if (!decl) {
      break;
    }
    print_indent(printer);
    print_declaration_text(printer, decl);
    fputs(";\n", printer->out);
    break;
  }

  case AST_ASSIGNMENT: {
    const Assignment *assign = (const Assignment *)node->data;
    if (!assign) {
      break;
    }
    print_indent(printer);
    print_assignment_text(printer, assign);
    fputs(";\n", printer->out);
    break;
  }

  case AST_RETURN_STATEMENT: {
    const ReturnStatement *ret = (const ReturnStatement *)node->data;
    print_indent(printer);
    fputs("return", printer->out);
    if (ret && ret->value) {
      fputc(' ', printer->out);
      print_expression(printer, ret->value);
    }
    fputs(";\n", printer->out);
    break;
  }

  case AST_IF_STATEMENT:
    print_if_statement(printer, node);
    break;

  case AST_WHILE_STATEMENT:
  case AST_FOR_STATEMENT:
  case AST_COMPTIME_FOR:
    print_loop_statement(printer, node);
    break;

  case AST_BREAK_STATEMENT:
  case AST_CONTINUE_STATEMENT:
    print_loop_control(printer, node);
    break;

  case AST_MATCH_STATEMENT:
    print_match_statement(printer, node);
    break;

  case AST_SWITCH_STATEMENT:
    print_switch_statement(printer, node);
    break;

  case AST_VALUE_SCOPE:
    print_indent(printer);
    print_value_scope(printer, (const ValueScope *)node->data);
    fputc('\n', printer->out);
    break;

  case AST_YIELD_STATEMENT:
    print_yield_statement(printer, (const YieldStatement *)node->data);
    break;

  case AST_FUNCTION_CALL:
    print_indent(printer);
    print_expression(printer, node);
    fputs(";\n", printer->out);
    break;

  case AST_QUIESCE_STATEMENT:
    print_indent(printer);
    fputs("quiesce;\n", printer->out);
    break;

  case AST_FALLTHROUGH_STATEMENT:
    print_indent(printer);
    fputs("fallthrough;\n", printer->out);
    break;

  case AST_DEFER_STATEMENT:
  case AST_ERRDEFER_STATEMENT: {
    const DeferStatement *defer = (const DeferStatement *)node->data;
    print_indent(printer);
    fputs(node->type == AST_DEFER_STATEMENT ? "defer " : "errdefer ",
          printer->out);
    if (defer && defer->statement) {
      print_expression(printer, defer->statement);
    }
    fputs(";\n", printer->out);
    break;
  }

  default:
    print_indent(printer);
    if (node->type >= AST_IDENTIFIER && node->type <= AST_LAMBDA_EXPRESSION) {
      print_expression(printer, node);
      fputs(";\n", printer->out);
      break;
    }
    print_unprintable(printer, "this statement");
    fputc('\n', printer->out);
    break;
  }
}

static void print_arm_body(AstPrinter *printer, const ASTNode *body) {
  if (body && body->type == AST_PROGRAM) {
    print_block(printer, body);
    fputc('\n', printer->out);
    return;
  }
  fputc('\n', printer->out);
  printer->depth++;
  print_statement(printer, body);
  printer->depth--;
}

static void print_match_statement(AstPrinter *printer, const ASTNode *node) {
  const MatchStatement *match = (const MatchStatement *)node->data;
  if (!match) {
    return;
  }
  print_indent(printer);
  fputs("match ", printer->out);
  print_expression(printer, match->expression);
  fputs(" {\n", printer->out);
  printer->depth++;
  for (size_t i = 0; i < match->arm_count; i++) {
    const MatchArm *arm = &match->arms[i];
    print_indent(printer);
    if (arm->is_default) {
      fputs("default:", printer->out);
    } else {
      fprintf(printer->out, "case %s", arm->variant_name ? arm->variant_name
                                                          : "<variant>");
      if (arm->binding_name) {
        fprintf(printer->out, "(%s)", arm->binding_name);
      }
      fputc(':', printer->out);
    }
    print_arm_body(printer, arm->body);
  }
  printer->depth--;
  print_indent(printer);
  fputs("}", printer->out);
  print_line_end(printer, node);
}

static void print_switch_statement(AstPrinter *printer, const ASTNode *node) {
  const SwitchStatement *sw = (const SwitchStatement *)node->data;
  if (!sw) {
    return;
  }
  print_indent(printer);
  fputs("switch (", printer->out);
  print_expression(printer, sw->expression);
  fputs(") {\n", printer->out);
  printer->depth++;
  for (size_t i = 0; i < sw->case_count; i++) {
    const CaseClause *clause =
        sw->cases[i] ? (const CaseClause *)sw->cases[i]->data : NULL;
    if (!clause) {
      continue;
    }
    print_indent(printer);
    if (clause->is_default) {
      fputs("default:", printer->out);
    } else {
      fputs("case ", printer->out);
      print_expression(printer, clause->value);
      if (clause->value_high) {
        fputs("..", printer->out);
        print_expression(printer, clause->value_high);
      }
      fputc(':', printer->out);
    }
    if (clause->body) {
      print_arm_body(printer, clause->body);
    } else {
      fputc('\n', printer->out);
    }
  }
  printer->depth--;
  print_indent(printer);
  fputs("}", printer->out);
  print_line_end(printer, node);
}

static void print_effect_clause(AstPrinter *printer, const char *word,
                                char **names, size_t count) {
  if (count == 0) {
    return;
  }
  fprintf(printer->out, " %s ", word);
  for (size_t i = 0; i < count; i++) {
    fprintf(printer->out, "%s%s", i ? ", " : "", names[i] ? names[i] : "?");
  }
}

static void print_function_declaration(AstPrinter *printer, const ASTNode *node) {
  const FunctionDeclaration *decl = (const FunctionDeclaration *)node->data;
  if (!decl) {
    return;
  }
  g_tag_line = node->location.line;
  if (decl->generated_by) {
    fprintf(printer->out, "// generated by %s\n", decl->generated_by);
  }
  if (g_statement_note) {
    print_note_line(printer, g_statement_note(node));
  }
  if (g_line_tags) {
    fprintf(printer->out, "\x01%zu\x02", g_tag_line);
  }
  if (decl->is_explicit) {
    fputs("@explicit ", printer->out);
  }
  if (decl->is_inline) {
    fputs(decl->is_inline_contract ? "@inline! " : "@inline ", printer->out);
  }
  if (decl->is_noinline) {
    fputs("@noinline ", printer->out);
  }
  if (decl->is_pure) {
    fputs("@pure ", printer->out);
  }
  if (decl->is_noalloc) {
    fputs("@noalloc ", printer->out);
  }
  if (decl->is_test) {
    fputs("@test ", printer->out);
  }
  if (decl->is_rule) {
    fputs("@rule ", printer->out);
  }
  if (decl->meta_role == META_ROLE_LAYOUT) {
    fputs("@layout ", printer->out);
  } else if (decl->meta_role == META_ROLE_GENERATE) {
    fputs(decl->meta_optimized ? "@generate(optimized) " : "@generate ",
          printer->out);
  } else if (decl->meta_role == META_ROLE_PASS) {
    fprintf(printer->out, "@pass(%s) ", decl->meta_point ? decl->meta_point : "");
  } else if (decl->meta_role == META_ROLE_REGALLOC) {
    fputs("@regalloc ", printer->out);
  }
  print_meta_decorators(printer, decl->meta_decorators,
                        decl->meta_decorator_count);
  if (decl->is_swappable) {
    fputs("@swappable ", printer->out);
  }
  if (decl->is_naked) {
    fputs("@naked ", printer->out);
  }
  if (decl->is_interrupt) {
    fputs("@interrupt ", printer->out);
  }
  if (decl->simd_mode == SIMD_ATTR_HINT) {
    fputs("@simd ", printer->out);
  } else if (decl->simd_mode == SIMD_ATTR_CONTRACT) {
    fputs("@simd! ", printer->out);
  }
  if (decl->is_exported) {
    fputs("export ", printer->out);
  }
  fprintf(printer->out, "fn %s(", decl->name ? decl->name : "<name>");
  for (size_t i = 0; i < decl->parameter_count; i++) {
    fprintf(printer->out, "%s%s: %s", i ? ", " : "",
            decl->parameter_names[i], decl->parameter_types[i]);
  }
  fputc(')', printer->out);
  if (decl->return_type) {
    fprintf(printer->out, " -> %s", decl->return_type);
  }
  print_effect_clause(printer, "with", decl->effects_with,
                      decl->effects_with_count);
  print_effect_clause(printer, "forbids", decl->effects_forbids,
                      decl->effects_forbids_count);
  print_effect_clause(printer, "requires", decl->effects_requires,
                      decl->effects_requires_count);
  print_effect_clause(printer, "provides", decl->effects_provides,
                      decl->effects_provides_count);
  if (!decl->body) {
    fputs(";\n\n", printer->out);
    return;
  }
  fputs(" {\n", printer->out);
  printer->depth++;
  for (size_t i = 0; i < decl->body->child_count; i++) {
    print_statement(printer, decl->body->children[i]);
  }
  printer->depth--;
  print_block_end_note(printer, decl->body);
  fputs("}\n\n", printer->out);
  return;
}

static void print_declaration(AstPrinter *printer, const ASTNode *node) {
  if (!node) {
    return;
  }

  print_provenance(printer, node);

  switch (node->type) {
  case AST_STRUCT_DECLARATION: {
    const StructDeclaration *decl = (const StructDeclaration *)node->data;
    if (!decl) {
      break;
    }
    print_provenance(printer, node);
    print_meta_decorators(printer, decl->meta_decorators,
                          decl->meta_decorator_count);
    fprintf(printer->out, "struct %s {\n",
            decl->name ? decl->name : "<name>");
    for (size_t i = 0; i < decl->field_count; i++) {
      fprintf(printer->out, "    %s: %s;\n",
              decl->field_names ? decl->field_names[i] : "<field>",
              decl->field_types ? decl->field_types[i] : "<type>");
    }
    fputs("}\n\n", printer->out);
    break;
  }

  case AST_EFFECT_DECLARATION: {
    const EffectDeclaration *decl = (const EffectDeclaration *)node->data;
    if (!decl) {
      break;
    }
    if (decl->is_exported) {
      fputs("export ", printer->out);
    }
    fprintf(printer->out, "effect %s;\n\n", decl->name ? decl->name : "<name>");
    break;
  }

  case AST_TYPE_DECLARATION: {
    const TypeDeclaration *decl = (const TypeDeclaration *)node->data;
    if (!decl) {
      break;
    }
    if (decl->is_exported) {
      fputs("export ", printer->out);
    }
    fprintf(printer->out, "type %s = %s", decl->name ? decl->name : "<name>",
            decl->base_type ? decl->base_type : "<base>");
    if (decl->predicate) {
      fputs(" where ", printer->out);
      if (decl->binding) {
        fprintf(printer->out, "%s: ", decl->binding);
      }
      print_expression(printer, decl->predicate);
    }
    fputs(";\n\n", printer->out);
    break;
  }
  case AST_ENUM_DECLARATION: {
    const EnumDeclaration *decl = (const EnumDeclaration *)node->data;
    if (!decl) {
      break;
    }
    fprintf(printer->out, "enum %s {\n", decl->name ? decl->name : "<name>");
    for (size_t i = 0; i < decl->variant_count; i++) {
      fprintf(printer->out, "    %s%s\n", decl->variants[i].name,
              i + 1 < decl->variant_count ? "," : "");
    }
    fputs("}\n\n", printer->out);
    break;
  }

  case AST_FUNCTION_DECLARATION:
    return print_function_declaration(printer, node);

  case AST_IMPORT:
  case AST_IMPORT_STR:
    break;

  case AST_VAR_DECLARATION:
    print_statement(printer, node);
    break;

  case AST_FUNCTION_CALL:
    print_statement(printer, node);
    break;

  default:
    print_unprintable(printer, "this declaration");
    fputc('\n', printer->out);
    break;
  }
}

size_t ast_print_program(FILE *out, const ASTNode *program,
                         AstPrintAnnotator annotate, void *context) {
  if (!out || !program) {
    return 0;
  }
  AstPrinter printer = {out, 0, 0, annotate, context, 0, NULL};
  for (size_t i = 0; i < program->child_count; i++) {
    if (g_keep_declaration &&
        !g_keep_declaration(program->children[i], g_keep_context)) {
      continue;
    }
    print_declaration(&printer, program->children[i]);
  }
  return printer.unprintable;
}

void ast_print_set_filter(AstPrintFilter keep, const void *context) {
  g_keep_declaration = keep;
  g_keep_context = context;
}

void ast_print_statement_at(FILE *out, const ASTNode *statement, int depth) {
  AstPrinter printer = {out, depth, 0, NULL, NULL, 0, NULL};
  AstPrintNote saved_statement = g_statement_note;
  AstPrintNote saved_block = g_block_end_note;
  g_statement_note = NULL;
  g_block_end_note = NULL;
  print_statement(&printer, statement);
  g_statement_note = saved_statement;
  g_block_end_note = saved_block;
}
