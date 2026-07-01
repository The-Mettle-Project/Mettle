#ifndef PARSER_H
#define PARSER_H

#include "../error/error_reporter.h"
#include "../lexer/lexer.h"
#include "ast.h"

typedef struct {
  Lexer *lexer;
  Token current_token;
  Token peek_token;
  int has_error;
  int had_error;
  size_t error_count;
  char *error_message;
  ErrorReporter *error_reporter;
  int error_recovery_mode;
  const char *source_filename;
  /* Set when compiling for the GPU (--emit-ptx). Enables the kernel index
   * built-ins (thread/block/block_dim/grid_dim member access on x/y/z) to
   * desugar to the gpu_* intrinsics. Off for normal CPU compiles, so member
   * access on an ordinary struct named e.g. `block` is unaffected. */
  int gpu_mode;
  /* Monotonic id for synthesizing unique local names in `dispatch` desugaring. */
  int dispatch_counter;
} Parser;

// Function declarations
Parser *parser_create(Lexer *lexer);
Parser *parser_create_with_error_reporter(Lexer *lexer,
                                          ErrorReporter *error_reporter);
void parser_destroy(Parser *parser);
ASTNode *parser_parse_program(Parser *parser);
ASTNode *parser_parse_declaration(Parser *parser);
ASTNode *parser_parse_statement(Parser *parser);
ASTNode *parser_parse_expression(Parser *parser);

// Expression parsing with precedence
ASTNode *parser_parse_primary_expression(Parser *parser);
ASTNode *parser_parse_unary_expression(Parser *parser);
ASTNode *parser_parse_binary_expression(Parser *parser, int min_precedence);
ASTNode *parser_parse_postfix_expression(Parser *parser);
ASTNode *parser_parse_cast_expression(Parser *parser);

// Specific parsing functions
ASTNode *parser_parse_import_declaration(Parser *parser);
ASTNode *parser_parse_var_declaration(Parser *parser);
ASTNode *parser_parse_function_declaration(Parser *parser);
ASTNode *parser_parse_struct_declaration(Parser *parser);
ASTNode *parser_parse_enum_declaration(Parser *parser);
ASTNode *parser_parse_trait_declaration(Parser *parser);
ASTNode *parser_parse_impl_declaration(Parser *parser);
ASTNode *parser_parse_method_declaration(Parser *parser);
ASTNode *parser_parse_inline_asm(Parser *parser);
ASTNode *parser_parse_return_statement(Parser *parser);
ASTNode *parser_parse_if_statement(Parser *parser);
ASTNode *parser_parse_while_statement(Parser *parser);
ASTNode *parser_parse_for_statement(Parser *parser);
ASTNode *parser_parse_switch_statement(Parser *parser);
ASTNode *parser_parse_match_statement(Parser *parser);
ASTNode *parser_parse_match_expression(Parser *parser);
ASTNode *parser_parse_break_statement(Parser *parser);
ASTNode *parser_parse_continue_statement(Parser *parser);
ASTNode *parser_parse_defer_statement(Parser *parser);
ASTNode *parser_parse_errdefer_statement(Parser *parser);
ASTNode *parser_parse_block(Parser *parser);

// Utility functions
void parser_advance(Parser *parser);
int parser_match(Parser *parser, TokenType type);
int parser_expect(Parser *parser, TokenType type);
void parser_set_error(Parser *parser, const char *message);
void parser_set_error_with_suggestion(Parser *parser, const char *message,
                                      const char *suggestion);
void parser_refine_error(Parser *parser, const char *message);
void parser_recover_from_error(Parser *parser);
void parser_synchronize(Parser *parser);
int parser_get_operator_precedence(TokenType type);
int parser_is_binary_operator(TokenType type);
int parser_is_unary_operator(TokenType type);
int parser_is_identifier_like(TokenType type);
int parser_is_type_keyword(TokenType type);
int parser_is_assignment_token(TokenType type);

#endif // PARSER_H
