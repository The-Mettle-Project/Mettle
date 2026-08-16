#ifndef PARSER_H
#define PARSER_H

#include "../error/error_reporter.h"
#include "../lexer/lexer.h"
#include "ast.h"

#define PARSER_PREV_TEXT_MAX 32

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
  /* The token just consumed. Diagnostics name it when an expression turns up
     missing, so the reader is told what the compiler was reading past. */
  TokenType previous_token_type;
  char previous_token_text[PARSER_PREV_TEXT_MAX];
  /* '{' consumed minus '}' consumed. Error recovery uses it to stay inside
     the block it started in and to find that block's own closing brace. */
  int brace_depth;
  /* What the innermost open '(' belongs to: "parameter list", "argument
     list", "condition of 'if'", and so on. A diagnostic about the matching
     ')' quotes this rather than guessing that every paren is a signature. */
  const char *group_context;
  /* Set when a loop or conditional header failed. Recovery then runs to that
     construct's '{' rather than stopping at the first ';' inside the broken
     header, so the body is still parsed and checked. */
  int recover_at_body_brace;
  /* Set once the lexer reports a bad token. From there the token stream is
     guesswork, so syntax diagnostics after it are suppressed and the lexical
     error stands alone. */
  int saw_lexical_error;
  const char *source_filename;
  /* Set when compiling a GPU module. Enables the kernel index built-ins
   * (thread/block/block_dim/grid_dim member access on x/y/z) to desugar to
   * target-neutral gpu_* intrinsics. Off for normal CPU compiles, so member
   * access on an ordinary struct named e.g. `block` is unaffected. */
  int gpu_mode;
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
ASTNode *parser_parse_barrier_statement(Parser *parser);
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
/* Skip to the next statement boundary that belongs to the block opened at
   `block_depth`, leaving that block's own '}' unconsumed. */
void parser_recover_in_block(Parser *parser, int block_depth);
/* Skip to the next token that can begin a top-level declaration, past any
   braces still open. */
void parser_recover_to_declaration(Parser *parser);
int parser_get_operator_precedence(TokenType type);
int parser_is_binary_operator(TokenType type);
int parser_is_unary_operator(TokenType type);
int parser_is_identifier_like(TokenType type);
int parser_is_type_keyword(TokenType type);
int parser_is_builtin_type_name(const char *text);
int parser_is_assignment_token(TokenType type);

#endif // PARSER_H
