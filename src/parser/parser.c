#include "parser.h"
#include "../error/error_reporter.h"
#include "../string_intern.h"
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PARSER_ERROR_BUF_SIZE 512
#define PARSER_SCRATCH_BUF_SIZE 1024

static void parser_report_lexer_token_error(Parser *parser,
                                            const Token *token) {
  if (!parser || !token || token->type != TOKEN_ERROR) {
    return;
  }

  const char *message = (token->value && token->value[0] != '\0')
                            ? token->value
                            : "Invalid token";
  char error_msg[PARSER_ERROR_BUF_SIZE];
  snprintf(error_msg, sizeof(error_msg), "Lexical error: %s", message);

  parser->has_error = 1;
  parser->had_error = 1;
  parser->error_count++;
  free(parser->error_message);
  parser->error_message = strdup(error_msg);

  if (parser->error_reporter) {
    SourceLocation location =
        source_location_create(token->line, token->column);
    location.filename = parser->source_filename;
    error_reporter_add_error(parser->error_reporter, ERROR_LEXICAL, location,
                             error_msg);
  }
}

static SourceLocation parser_current_location(Parser *parser) {
  SourceLocation location = source_location_create(
      parser ? parser->current_token.line : 0,
      parser ? parser->current_token.column : 0);
  location.filename = parser ? parser->source_filename : NULL;
  return location;
}

Parser *parser_create(Lexer *lexer) {
  return parser_create_with_error_reporter(lexer, NULL);
}

Parser *parser_create_with_error_reporter(Lexer *lexer,
                                          ErrorReporter *error_reporter) {
  if (!lexer) {
    return NULL;
  }

  Parser *parser = malloc(sizeof(Parser));
  if (!parser)
    return NULL;

  parser->lexer = lexer;
  parser->current_token = lexer_next_token(lexer);
  parser->peek_token = lexer_next_token(lexer);
  parser->has_error = 0;
  parser->had_error = 0;
  parser->error_count = 0;
  parser->error_message = NULL;
  parser->error_reporter = error_reporter;
  parser->error_recovery_mode = 0;
  parser->source_filename = error_reporter_current_filename(error_reporter);
  parser->gpu_mode = 0;
  parser->dispatch_counter = 0;

  if (parser->current_token.type == TOKEN_ERROR) {
    parser_report_lexer_token_error(parser, &parser->current_token);
  } else if (parser->peek_token.type == TOKEN_ERROR) {
    parser_report_lexer_token_error(parser, &parser->peek_token);
  }

  return parser;
}

void parser_destroy(Parser *parser) {
  if (parser) {
    token_destroy(&parser->current_token);
    token_destroy(&parser->peek_token);
    free(parser->error_message);
    free(parser);
  }
}

void parser_advance(Parser *parser) {
  if (!parser || parser->current_token.type == TOKEN_EOF)
    return;

  token_destroy(&parser->current_token);
  parser->current_token = parser->peek_token;

  // Clear peek_token to avoid double-free
  parser->peek_token.type = TOKEN_EOF;
  parser->peek_token.value = NULL;
  parser->peek_token.lexeme.data = NULL;
  parser->peek_token.lexeme.length = 0;
  parser->peek_token.line = 0;
  parser->peek_token.column = 0;
  parser->peek_token.is_interned = 0;

  // Get new peek token
  parser->peek_token = lexer_next_token(parser->lexer);

  if (parser->current_token.type == TOKEN_ERROR) {
    parser_report_lexer_token_error(parser, &parser->current_token);
  }
}

int parser_match(Parser *parser, TokenType type) {
  if (!parser)
    return 0;
  return parser->current_token.type == type;
}

static const char *token_type_to_string(TokenType type) {
  switch (type) {
  case TOKEN_EOF:
    return "end of file";
  case TOKEN_IDENTIFIER:
    return "identifier";
  case TOKEN_NUMBER:
    return "number";
  case TOKEN_STRING:
    return "string";
  case TOKEN_IMPORT:
    return "'import'";
  case TOKEN_IMPORT_STR:
    return "'import_str'";
  case TOKEN_EXTERN:
    return "'extern'";
  case TOKEN_EXPORT:
    return "'export'";
  case TOKEN_VAR:
    return "'var'";
  case TOKEN_FUNCTION:
  case TOKEN_FN:
    return "'fn'";
  case TOKEN_STRUCT:
    return "'struct'";
  case TOKEN_METHOD:
    return "'method'";
  case TOKEN_RETURN:
    return "'return'";
  case TOKEN_IF:
    return "'if'";
  case TOKEN_ELSE:
    return "'else'";
  case TOKEN_WHILE:
    return "'while'";
  case TOKEN_FOR:
    return "'for'";
  case TOKEN_SWITCH:
    return "'switch'";
  case TOKEN_CASE:
    return "'case'";
  case TOKEN_DEFAULT:
    return "'default'";
  case TOKEN_BREAK:
    return "'break'";
  case TOKEN_CONTINUE:
    return "'continue'";
  case TOKEN_DEFER:
    return "'defer'";
  case TOKEN_ERRDEFER:
    return "'errdefer'";
  case TOKEN_ASM:
    return "'asm'";
  case TOKEN_THIS:
    return "'this'";
  case TOKEN_NEW:
    return "'new'";
  case TOKEN_COLON:
    return "':'";
  case TOKEN_SEMICOLON:
    return "';'";
  case TOKEN_COMMA:
    return "','";
  case TOKEN_EQUALS:
    return "'='";
  case TOKEN_ARROW:
    return "'->'";
  case TOKEN_LPAREN:
    return "'('";
  case TOKEN_RPAREN:
    return "')'";
  case TOKEN_LBRACE:
    return "'{'";
  case TOKEN_RBRACE:
    return "'}'";
  case TOKEN_LBRACKET:
    return "'['";
  case TOKEN_RBRACKET:
    return "']'";
  case TOKEN_PLUS:
    return "'+'";
  case TOKEN_MINUS:
    return "'-'";
  case TOKEN_MULTIPLY:
    return "'*'";
  case TOKEN_AMPERSAND:
    return "'&'";
  case TOKEN_PIPE:
    return "'|'";
  case TOKEN_CARET:
    return "'^'";
  case TOKEN_LSHIFT:
    return "'<<'";
  case TOKEN_RSHIFT:
    return "'>>'";
  case TOKEN_TILDE:
    return "'~'";
  case TOKEN_AND_AND:
    return "'&&'";
  case TOKEN_OR_OR:
    return "'||'";
  case TOKEN_DIVIDE:
    return "'/'";
  case TOKEN_PERCENT:
    return "'%'";
  case TOKEN_DOT:
    return "'.'";
  case TOKEN_NEWLINE:
    return "newline";
  case TOKEN_ERROR:
    return "lexical error";
  default:
    return "unknown token";
  }
}

int parser_expect_statement_end(Parser *parser) {
  if (parser_match(parser, TOKEN_SEMICOLON) ||
      parser_match(parser, TOKEN_NEWLINE)) {
    parser_advance(parser);
    return 1;
  }

  // Skip over any extra newlines
  while (parser_match(parser, TOKEN_NEWLINE)) {
    parser_advance(parser);
  }

  if (parser_match(parser, TOKEN_SEMICOLON)) {
    parser_advance(parser);
    return 1;
  }

  parser_set_error(parser,
                   "Expected ';' or newline at the end of the statement");
  return 0;
}

int parser_expect(Parser *parser, TokenType type) {
  if (!parser)
    return 0;

  if (parser->current_token.type == type) {
    parser_advance(parser);
    return 1;
  }

  char error_msg[PARSER_ERROR_BUF_SIZE];
  const char *expected_str = token_type_to_string(type);
  const char *actual_str = token_type_to_string(parser->current_token.type);

  snprintf(error_msg, sizeof(error_msg), "Expected %s, found %s", expected_str,
           actual_str);

  // Generate context-specific suggestions
  const char *suggestion = NULL;
  if (type == TOKEN_SEMICOLON) {
    suggestion = "add a semicolon ';' to end the statement";
  } else if (type == TOKEN_RPAREN &&
             parser->current_token.type == TOKEN_COMMA) {
    suggestion = "check if you have an extra comma in the parameter list";
  } else if (type == TOKEN_RBRACE && parser->current_token.type == TOKEN_EOF) {
    suggestion = "add a closing brace '}' to match the opening brace";
  } else if (type == TOKEN_COLON &&
             parser->current_token.type == TOKEN_EQUALS) {
    suggestion = "use ':' for type annotations, '=' for assignments";
  }

  parser_set_error_with_suggestion(parser, error_msg, suggestion);
  return 0;
}

void parser_set_error(Parser *parser, const char *message) {
  parser_set_error_with_suggestion(parser, message, NULL);
}

void parser_set_error_with_suggestion(Parser *parser, const char *message,
                                      const char *suggestion) {
  if (!parser || !message)
    return;

  parser->has_error = 1;
  parser->had_error = 1;
  parser->error_count++;
  free(parser->error_message);
  parser->error_message = strdup(message);

  // If we have an error reporter, add the error to it
  if (parser->error_reporter) {
    SourceLocation location = source_location_create(
        parser->current_token.line, parser->current_token.column);
    location.filename = parser->source_filename;

    size_t span_len = 1;
    if (parser->current_token.lexeme.length > 0) {
      span_len = parser->current_token.lexeme.length;
    } else if (parser->current_token.value &&
               parser->current_token.value[0] != '\0') {
      span_len = strlen(parser->current_token.value);
    }
    SourceSpan span = source_span_from_location(location, span_len);

    if (suggestion) {
      error_reporter_add_error_with_span_and_suggestion(
          parser->error_reporter, ERROR_SYNTAX, span, message, suggestion);
    } else {
      // Try to generate a helpful suggestion
      const char *auto_suggestion = NULL;
      if (parser->current_token.value) {
        auto_suggestion =
            error_reporter_suggest_for_token(parser->current_token.value);
      }

      if (auto_suggestion) {
        error_reporter_add_error_with_span_and_suggestion(
            parser->error_reporter, ERROR_SYNTAX, span, message,
            auto_suggestion);
      } else {
        error_reporter_add_error_with_span(parser->error_reporter, ERROR_SYNTAX,
                                           span, message);
      }
    }
  }
}

void parser_refine_error(Parser *parser, const char *message) {
  if (!parser || !message)
    return;
  free(parser->error_message);
  parser->error_message = strdup(message);
  if (parser->error_reporter)
    error_reporter_refine_last(parser->error_reporter, message);
}

void parser_recover_from_error(Parser *parser) {
  if (!parser)
    return;

  parser->error_recovery_mode = 1;
  parser_synchronize(parser);

  // Clear error state to continue parsing
  parser->has_error = 0;
  free(parser->error_message);
  parser->error_message = NULL;
  parser->error_recovery_mode = 0;
}

void parser_synchronize(Parser *parser) {
  if (!parser)
    return;

  while (parser->current_token.type != TOKEN_EOF) {
    if (parser->current_token.type == TOKEN_SEMICOLON ||
        parser->current_token.type == TOKEN_NEWLINE) {
      parser_advance(parser);
      return;
    }

    // Synchronize on the current token, not peek. Using peek can return
    // without consuming an invalid current token (e.g. current '=>', peek
    // 'return'), causing parse/recover loops to spin forever at top level.
    switch (parser->current_token.type) {
    case TOKEN_FUNCTION:
    case TOKEN_FN:
    case TOKEN_VAR:
    case TOKEN_STRUCT:
    case TOKEN_RETURN:
    case TOKEN_IF:
    case TOKEN_WHILE:
    case TOKEN_FOR:
    case TOKEN_SWITCH:
    case TOKEN_MATCH:
    case TOKEN_BREAK:
    case TOKEN_CONTINUE:
    case TOKEN_RBRACE:
      return;
    default:
      break;
    }

    parser_advance(parser);
  }
}

int parser_get_operator_precedence(TokenType type) {
  switch (type) {
  case TOKEN_DOT:
    return 13; // Member access (highest precedence)
  case TOKEN_MULTIPLY:
  case TOKEN_DIVIDE:
  case TOKEN_PERCENT:
    return 11; // Multiplicative
  case TOKEN_PLUS:
  case TOKEN_MINUS:
    return 10; // Additive
  case TOKEN_LSHIFT:
  case TOKEN_RSHIFT:
    return 9; // Shift
  case TOKEN_LESS_THAN:
  case TOKEN_LESS_EQUALS:
  case TOKEN_GREATER_THAN:
  case TOKEN_GREATER_EQUALS:
    return 8; // Relational
  case TOKEN_EQUALS_EQUALS:
  case TOKEN_NOT_EQUALS:
    return 7; // Equality
  case TOKEN_AMPERSAND:
    return 6; // Bitwise AND
  case TOKEN_CARET:
    return 5; // Bitwise XOR
  case TOKEN_PIPE:
    return 4; // Bitwise OR
  case TOKEN_AND_AND:
    return 3; // Logical AND
  case TOKEN_OR_OR:
    return 2; // Logical OR
  default:
    return 0; // Not a binary operator
  }
}

int parser_is_binary_operator(TokenType type) {
  switch (type) {
  case TOKEN_PLUS:
  case TOKEN_MINUS:
  case TOKEN_MULTIPLY:
  case TOKEN_DIVIDE:
  case TOKEN_PERCENT:
  case TOKEN_EQUALS_EQUALS:
  case TOKEN_NOT_EQUALS:
  case TOKEN_LESS_THAN:
  case TOKEN_LESS_EQUALS:
  case TOKEN_GREATER_THAN:
  case TOKEN_GREATER_EQUALS:
  case TOKEN_AND_AND:
  case TOKEN_OR_OR:
  case TOKEN_AMPERSAND:
  case TOKEN_PIPE:
  case TOKEN_CARET:
  case TOKEN_LSHIFT:
  case TOKEN_RSHIFT:
  case TOKEN_DOT:
    return 1;
  default:
    return 0;
  }
}

int parser_is_unary_operator(TokenType type) {
  switch (type) {
  case TOKEN_MINUS:
  case TOKEN_PLUS:
  case TOKEN_MULTIPLY:
  case TOKEN_AMPERSAND:
  case TOKEN_TILDE:
  case TOKEN_NOT:
    return 1;
  default:
    return 0;
  }
}

ASTNode *parser_parse_program(Parser *parser) {
  if (!parser)
    return NULL;

  ASTNode *program = ast_create_program();
  if (!program)
    return NULL;

  Program *prog_data = (Program *)program->data;

  while (parser->current_token.type != TOKEN_EOF) {
    size_t token_pos_before = parser->lexer->position;

    // Skip empty statements/newlines at the top level
    if (parser->current_token.type == TOKEN_NEWLINE ||
        parser->current_token.type == TOKEN_SEMICOLON) {
      parser_advance(parser);
      continue;
    }

    ASTNode *declaration = parser_parse_declaration(parser);

    if (declaration) {
      // Add to program's declarations array
      prog_data->declarations =
          realloc(prog_data->declarations,
                  (prog_data->declaration_count + 1) * sizeof(ASTNode *));
      if (prog_data->declarations) {
        prog_data->declarations[prog_data->declaration_count] = declaration;
        prog_data->declaration_count++;
        ast_add_child(program, declaration);
      }
    } else if (!parser->has_error) {
      parser_set_error(parser, "Failed to parse declaration");
      parser_advance(parser);
    }

    if (parser->has_error) {
      parser_recover_from_error(parser);
      // Hard guard against non-advancing recovery loops.
      if (parser->current_token.type != TOKEN_EOF &&
          parser->lexer->position == token_pos_before) {
        parser_advance(parser);
      }
    }
  }

  return program;
}

static ASTNode *parser_parse_extern_var_declaration(Parser *parser);

// Flags collected from a run of `@ident[!]` decorators.
typedef struct {
  int is_inline;          // `@inline`
  int is_inline_contract; // `@inline!` (implies is_inline)
  int is_noinline;        // `@noinline`
  int is_pure;            // `@pure`
  int is_noalloc;         // `@noalloc`
  int is_test;            // `@test`: compile-time unit test (mettle test)
  int simd_mode; // SimdAttr from `@simd` / `@simd!` (SIMD_ATTR_NONE if absent)
} ParsedDecorators;

// Consume a run of `@ident[!]` decorators into `out`. Assumes the current token
// is TOKEN_AT. Returns 1 on success (parser positioned on the decorated
// construct), 0 on error (a parser error is set). Recognizes `@inline` /
// `@inline!`, `@noinline`, `@pure`, `@noalloc`, and `@simd` / `@simd!`;
// rejects unknown names, duplicates, and the `@inline`+`@noinline` conflict.
static int parser_parse_decorator_chain(Parser *parser, ParsedDecorators *out) {
  out->is_inline = 0;
  out->is_inline_contract = 0;
  out->is_noinline = 0;
  out->is_pure = 0;
  out->is_noalloc = 0;
  out->is_test = 0;
  out->simd_mode = SIMD_ATTR_NONE;

  while (parser->current_token.type == TOKEN_AT) {
    parser_advance(parser); // consume '@'
    if (!parser_is_identifier_like(parser->current_token.type)) {
      parser_set_error(parser,
                       "Expected a decorator name after '@' (one of 'inline', "
                       "'noinline', 'pure', 'noalloc', 'simd')");
      return 0;
    }
    const char *name = parser->current_token.value;
    if (strcmp(name, "inline") == 0) {
      if (out->is_inline) {
        parser_set_error(parser, "Duplicate '@inline' decorator");
        return 0;
      }
      out->is_inline = 1;
      parser_advance(parser);
      if (parser->current_token.type == TOKEN_NOT) {
        out->is_inline_contract = 1; // `@inline!`: contract, not just a hint
        parser_advance(parser);      // consume '!'
      }
    } else if (strcmp(name, "noalloc") == 0) {
      if (out->is_noalloc) {
        parser_set_error(parser, "Duplicate '@noalloc' decorator");
        return 0;
      }
      out->is_noalloc = 1;
      parser_advance(parser);
    } else if (strcmp(name, "noinline") == 0) {
      if (out->is_noinline) {
        parser_set_error(parser, "Duplicate '@noinline' decorator");
        return 0;
      }
      out->is_noinline = 1;
      parser_advance(parser);
    } else if (strcmp(name, "pure") == 0) {
      if (out->is_pure) {
        parser_set_error(parser, "Duplicate '@pure' decorator");
        return 0;
      }
      out->is_pure = 1;
      parser_advance(parser);
    } else if (strcmp(name, "test") == 0) {
      if (out->is_test) {
        parser_set_error(parser, "Duplicate '@test' decorator");
        return 0;
      }
      out->is_test = 1;
      parser_advance(parser);
    } else if (strcmp(name, "simd") == 0) {
      if (out->simd_mode != SIMD_ATTR_NONE) {
        parser_set_error(parser, "Duplicate '@simd' decorator");
        return 0;
      }
      parser_advance(parser); // consume 'simd'
      out->simd_mode = SIMD_ATTR_HINT;
      if (parser->current_token.type == TOKEN_NOT) {
        out->simd_mode = SIMD_ATTR_CONTRACT;
        parser_advance(parser); // consume '!'
      }
    } else {
      parser_set_error(parser,
                       "Unknown decorator after '@' (expected 'inline', "
                       "'noinline', 'pure', 'noalloc', 'test', or 'simd')");
      return 0;
    }
  }

  if (out->is_inline && out->is_noinline) {
    parser_set_error(parser,
                     "'@inline' and '@noinline' are mutually exclusive");
    return 0;
  }
  return 1;
}

ASTNode *parser_parse_declaration(Parser *parser) {
  if (!parser)
    return NULL;

  // Function decorators: `@inline` / `@noinline` / `@pure` / `@simd` may
  // prefix a (possibly `export`-qualified) function declaration. Parse the
  // chain, then stamp the flags onto the function it decorates.
  if (parser->current_token.type == TOKEN_AT) {
    ParsedDecorators decos;
    if (!parser_parse_decorator_chain(parser, &decos))
      return NULL;
    ASTNode *decl = parser_parse_declaration(parser);
    if (!decl)
      return NULL;
    if (decl->type != AST_FUNCTION_DECLARATION) {
      parser_set_error(parser,
                       "Decorators (@inline/@noinline/@pure/@simd) may only "
                       "precede a function declaration");
      ast_destroy_node(decl);
      return NULL;
    }
    FunctionDeclaration *fd = (FunctionDeclaration *)decl->data;
    if (fd->is_extern) {
      parser_set_error(parser,
                       "Decorators cannot be applied to extern functions");
      ast_destroy_node(decl);
      return NULL;
    }
    fd->is_inline = decos.is_inline;
    fd->is_inline_contract = decos.is_inline_contract;
    fd->is_noinline = decos.is_noinline;
    fd->is_pure = decos.is_pure;
    fd->is_noalloc = decos.is_noalloc;
    fd->is_test = decos.is_test;
    fd->simd_mode = decos.simd_mode;
    return decl;
  }

  switch (parser->current_token.type) {
  case TOKEN_IMPORT:
    return parser_parse_import_declaration(parser);
  case TOKEN_EXTERN: {
    parser_advance(parser); // consume 'extern'
    if (parser->current_token.type == TOKEN_FUNCTION ||
        parser->current_token.type == TOKEN_FN) {
      ASTNode *decl = parser_parse_function_declaration(parser);
      if (decl && decl->data) {
        FunctionDeclaration *func_data = (FunctionDeclaration *)decl->data;
        if (func_data->body != NULL) {
          parser_set_error(parser, "Extern functions must not have a body");
          ast_destroy_node(decl);
          return NULL;
        }
        func_data->is_extern = 1;
      }
      return decl;
    }
    if (parser->current_token.type == TOKEN_VAR) {
      return parser_parse_extern_var_declaration(parser);
    }
    parser_set_error(parser, "Expected 'fn' or 'var' after 'extern'");
    return NULL;
  }
  case TOKEN_EXPORT: {
    parser_advance(parser); // consume 'export'
    ASTNode *decl = NULL;
    if (parser->current_token.type == TOKEN_FUNCTION ||
        parser->current_token.type == TOKEN_FN) {
      decl = parser_parse_function_declaration(parser);
      if (decl && decl->data) {
        ((FunctionDeclaration *)decl->data)->is_exported = 1;
      }
    } else if (parser->current_token.type == TOKEN_STRUCT) {
      decl = parser_parse_struct_declaration(parser);
      if (decl && decl->data) {
        ((StructDeclaration *)decl->data)->is_exported = 1;
      }
    } else if (parser->current_token.type == TOKEN_ENUM) {
      decl = parser_parse_enum_declaration(parser);
      if (decl && decl->data) {
        ((EnumDeclaration *)decl->data)->is_exported = 1;
      }
    } else if (parser->current_token.type == TOKEN_TRAIT) {
      decl = parser_parse_trait_declaration(parser);
      if (decl && decl->data) {
        ((TraitDeclaration *)decl->data)->is_exported = 1;
      }
    } else if (parser->current_token.type == TOKEN_VAR) {
      decl = parser_parse_var_declaration(parser);
      if (decl && decl->data) {
        ((VarDeclaration *)decl->data)->is_exported = 1;
      }
    } else if (parser->current_token.type == TOKEN_EXTERN) {
      parser_advance(parser); // consume 'extern'
      if (parser->current_token.type == TOKEN_FUNCTION ||
          parser->current_token.type == TOKEN_FN) {
        decl = parser_parse_function_declaration(parser);
        if (decl && decl->data) {
          FunctionDeclaration *func_data = (FunctionDeclaration *)decl->data;
          if (func_data->body != NULL) {
            parser_set_error(parser, "Extern functions must not have a body");
            ast_destroy_node(decl);
            return NULL;
          }
          func_data->is_extern = 1;
          func_data->is_exported = 1;
        }
      } else if (parser->current_token.type == TOKEN_VAR) {
        decl = parser_parse_extern_var_declaration(parser);
        if (decl && decl->data) {
          ((VarDeclaration *)decl->data)->is_exported = 1;
        }
      } else {
        parser_set_error(parser,
                         "Expected 'fn' or 'var' after 'export extern'");
        return NULL;
      }
    } else if (parser->current_token.type == TOKEN_AT) {
      parser_set_error(parser,
                       "Decorators must precede 'export' (write "
                       "'@inline export fn', not 'export @inline ...')");
      return NULL;
    } else {
      parser_set_error(parser, "Expected 'fn', 'var', 'struct', 'enum', "
                               "'trait', or 'extern' after 'export'");
      return NULL;
    }
    return decl;
  }
  case TOKEN_VAR:
  case TOKEN_CONST:
    return parser_parse_var_declaration(parser);
  case TOKEN_DEFER:
    return parser_parse_defer_statement(parser);
  case TOKEN_ERRDEFER:
    return parser_parse_errdefer_statement(parser);
  case TOKEN_FUNCTION:
  case TOKEN_FN:
  case TOKEN_KERNEL:
    return parser_parse_function_declaration(parser);
  case TOKEN_STRUCT:
    return parser_parse_struct_declaration(parser);
  case TOKEN_ENUM:
    return parser_parse_enum_declaration(parser);
  case TOKEN_TRAIT:
    return parser_parse_trait_declaration(parser);
  case TOKEN_IMPL:
    return parser_parse_impl_declaration(parser);
  default:
    // Try to parse as a statement instead
    return parser_parse_statement(parser);
  }
}

static int parser_is_assignment_target(ASTNode *target) {
  if (!target) {
    return 0;
  }

  if (target->type == AST_IDENTIFIER || target->type == AST_MEMBER_ACCESS ||
      target->type == AST_INDEX_EXPRESSION) {
    return 1;
  }

  if (target->type == AST_UNARY_EXPRESSION) {
    UnaryExpression *unary = (UnaryExpression *)target->data;
    return unary && unary->operator && strcmp(unary->operator, "*") == 0;
  }

  return 0;
}

static const char *parser_compound_assign_op(TokenType type) {
  switch (type) {
  case TOKEN_PLUS_EQUALS:    return "+";
  case TOKEN_MINUS_EQUALS:   return "-";
  case TOKEN_STAR_EQUALS:    return "*";
  case TOKEN_SLASH_EQUALS:   return "/";
  case TOKEN_PERCENT_EQUALS: return "%";
  case TOKEN_AMP_EQUALS:     return "&";
  case TOKEN_PIPE_EQUALS:    return "|";
  case TOKEN_CARET_EQUALS:   return "^";
  case TOKEN_LSHIFT_EQUALS:  return "<<";
  case TOKEN_RSHIFT_EQUALS:  return ">>";
  default:                   return NULL;
  }
}

int parser_is_assignment_token(TokenType type) {
  return type == TOKEN_EQUALS || parser_compound_assign_op(type) != NULL;
}

static ASTNode *parser_parse_assignment_from_target(Parser *parser,
                                                    ASTNode *target) {
  if (!parser || !target) {
    return NULL;
  }

  if (!parser_is_assignment_target(target)) {
    parser_set_error(parser, "Invalid assignment target");
    ast_destroy_node(target);
    return NULL;
  }

  TokenType assign_token = parser->current_token.type;
  const char *compound_op = parser_compound_assign_op(assign_token);

  parser_advance(parser); // consume '=' or compound assignment operator
  ASTNode *value = parser_parse_expression(parser);
  if (!value) {
    ast_destroy_node(target);
    return NULL;
  }

  // Desugar `target OP= value` into `target = target OP value`.
  if (compound_op) {
    ASTNode *target_clone = ast_clone_node(target);
    if (!target_clone) {
      ast_destroy_node(target);
      ast_destroy_node(value);
      parser_set_error(parser, "Out of memory cloning compound assignment target");
      return NULL;
    }
    ASTNode *combined = ast_create_binary_expression(target_clone, compound_op,
                                                     value, target->location);
    if (!combined) {
      ast_destroy_node(target);
      ast_destroy_node(target_clone);
      ast_destroy_node(value);
      return NULL;
    }
    value = combined;
  }

  if (target->type == AST_IDENTIFIER) {
    Identifier *id = (Identifier *)target->data;
    if (!id || !id->name) {
      ast_destroy_node(target);
      ast_destroy_node(value);
      parser_set_error(parser, "Invalid assignment target");
      return NULL;
    }

    ASTNode *assign = ast_create_assignment(id->name, value, target->location);
    ast_destroy_node(target);
    return assign;
  }

  return ast_create_field_assignment(target, value, target->location);
}

static ASTNode *parser_parse_dispatch_statement(Parser *parser);

ASTNode *parser_parse_statement(Parser *parser) {
  if (!parser)
    return NULL;

  // Vectorization attribute on a loop: `@simd` / `@simd!`.
  //   @simd  for i in 0..n { ... }   -> best-effort hint (warn if not vectorized)
  //   @simd! for i in 0..n { ... }   -> hard contract (compile error otherwise)
  // The attribute may sit in front of a label too: `@simd outer: for ...`.
  // Only `@simd` is meaningful on a loop; the other decorators are function-only.
  if (parser->current_token.type == TOKEN_AT) {
    ParsedDecorators decos;
    if (!parser_parse_decorator_chain(parser, &decos))
      return NULL;
    if (decos.is_inline || decos.is_noinline || decos.is_pure ||
        decos.is_noalloc) {
      parser_set_error(parser,
                       "'@inline', '@noinline', '@pure', and '@noalloc' apply "
                       "to a function, not a loop");
      return NULL;
    }
    if (decos.simd_mode == SIMD_ATTR_NONE) {
      parser_set_error(parser, "Expected a '@simd' decorator before a loop");
      return NULL;
    }

    ASTNode *loop = parser_parse_statement(parser);
    if (!loop)
      return NULL;
    if (loop->type == AST_FOR_STATEMENT) {
      ((ForStatement *)loop->data)->simd_mode = decos.simd_mode;
    } else if (loop->type == AST_WHILE_STATEMENT) {
      ((WhileStatement *)loop->data)->simd_mode = decos.simd_mode;
    } else {
      parser_set_error(parser,
                       "'@simd' must be applied to a 'for' or 'while' loop");
      ast_destroy_node(loop);
      return NULL;
    }
    return loop;
  }

  // Labeled loop: IDENT ':' (while | for)
  if (parser->current_token.type == TOKEN_IDENTIFIER &&
      parser->peek_token.type == TOKEN_COLON) {
    Token after_colon = lexer_peek_token(parser->lexer);
    if (after_colon.type == TOKEN_WHILE || after_colon.type == TOKEN_FOR) {
      char *label = strdup(parser->current_token.value);
      parser_advance(parser); // consume IDENT
      parser_advance(parser); // consume ':'

      ASTNode *loop = NULL;
      if (parser->current_token.type == TOKEN_WHILE) {
        loop = parser_parse_while_statement(parser);
        if (loop && loop->data) {
          WhileStatement *w = (WhileStatement *)loop->data;
          w->label = label;
          label = NULL;
        }
      } else if (parser->current_token.type == TOKEN_FOR) {
        loop = parser_parse_for_statement(parser);
        if (loop && loop->data) {
          ForStatement *f = (ForStatement *)loop->data;
          f->label = label;
          label = NULL;
        }
      }

      free(label);
      return loop;
    }
  }

  switch (parser->current_token.type) {
  case TOKEN_EXTERN:
    return parser_parse_declaration(parser);
  case TOKEN_VAR:
  case TOKEN_CONST:
    return parser_parse_var_declaration(parser);
  case TOKEN_RETURN:
    return parser_parse_return_statement(parser);
  case TOKEN_IF:
    return parser_parse_if_statement(parser);
  case TOKEN_WHILE:
    return parser_parse_while_statement(parser);
  case TOKEN_FOR:
    return parser_parse_for_statement(parser);
  case TOKEN_DISPATCH:
    return parser_parse_dispatch_statement(parser);
  case TOKEN_SWITCH:
    return parser_parse_switch_statement(parser);
  case TOKEN_MATCH:
    return parser_parse_match_statement(parser);
  case TOKEN_BREAK:
    return parser_parse_break_statement(parser);
  case TOKEN_CONTINUE:
    return parser_parse_continue_statement(parser);
  case TOKEN_DEFER:
    return parser_parse_defer_statement(parser);
  case TOKEN_ERRDEFER:
    return parser_parse_errdefer_statement(parser);
  case TOKEN_ASM:
    return parser_parse_inline_asm(parser);
  case TOKEN_LBRACE:
    return parser_parse_block(parser);
  default:
    break;
  }

  ASTNode *expr = parser_parse_expression(parser);
  if (!expr) {
    return NULL;
  }

  if (parser_is_assignment_token(parser->current_token.type)) {
    return parser_parse_assignment_from_target(parser, expr);
  }

  return expr;
}

static ASTNode *parser_parse_defer_or_errdefer(Parser *parser,
                                               TokenType token_type) {
  if (!parser) {
    return NULL;
  }

  SourceLocation location = parser_current_location(parser);
  const char *keyword =
      (token_type == TOKEN_DEFER) ? "defer" : "errdefer";

  if (!parser_expect(parser, token_type)) {
    return NULL;
  }

  if (parser->current_token.type == TOKEN_SEMICOLON ||
      parser->current_token.type == TOKEN_NEWLINE) {
    char msg[PARSER_ERROR_BUF_SIZE];
    snprintf(msg, sizeof(msg), "Expected statement after '%s'", keyword);
    parser_set_error(parser, msg);
    return NULL;
  }

  ASTNode *stmt = parser_parse_statement(parser);
  if (!stmt) {
    if (!parser->has_error) {
      char msg[PARSER_ERROR_BUF_SIZE];
      snprintf(msg, sizeof(msg), "Expected statement after '%s'", keyword);
      parser_set_error(parser, msg);
    }
    return NULL;
  }

  parser_expect_statement_end(parser);
  if (token_type == TOKEN_DEFER) {
    return ast_create_defer_statement(stmt, location);
  }
  return ast_create_errdefer_statement(stmt, location);
}

ASTNode *parser_parse_defer_statement(Parser *parser) {
  return parser_parse_defer_or_errdefer(parser, TOKEN_DEFER);
}

ASTNode *parser_parse_errdefer_statement(Parser *parser) {
  return parser_parse_defer_or_errdefer(parser, TOKEN_ERRDEFER);
}

ASTNode *parser_parse_expression(Parser *parser) {
  if (!parser)
    return NULL;

  return parser_parse_binary_expression(parser, 0);
}

int parser_is_identifier_like(TokenType type) {
  // Check if token can be used as an identifier in expression context
  return type == TOKEN_IDENTIFIER ||
         // x86 mnemonics can be used as function names
         (type >= TOKEN_MOV && type <= TOKEN_SYSCALL) ||
         // x86 registers can be used as identifiers in high-level context
         (type >= TOKEN_EAX && type <= TOKEN_R15);
}

int parser_is_type_keyword(TokenType type) {
  // Check if token is a built-in type keyword
  return (type >= TOKEN_INT8 && type <= TOKEN_STRING_TYPE);
}

static void parser_free_string_array(char **values, size_t count) {
  if (!values) {
    return;
  }

  for (size_t i = 0; i < count; i++) {
    free(values[i]);
  }
  free(values);
}

static void parser_free_type_param_list(char **params, char **traits,
                                        size_t count) {
  parser_free_string_array(params, count);
  parser_free_string_array(traits, count);
}

static char *parser_parse_qualified_name(Parser *parser, const char *expected);

static int parser_append_type_param_bound(char **bounds,
                                          const char *trait_name) {
  char *combined = NULL;
  size_t len = 0;

  if (!bounds || !trait_name) {
    return 0;
  }

  if (!*bounds) {
    *bounds = strdup(trait_name);
    return *bounds != NULL;
  }

  len = strlen(*bounds) + 1 + strlen(trait_name) + 1;
  combined = malloc(len);
  if (!combined) {
    return 0;
  }

  snprintf(combined, len, "%s+%s", *bounds, trait_name);
  free(*bounds);
  *bounds = combined;
  return 1;
}

static int parser_find_type_param_index(char **params, size_t count,
                                        const char *name, size_t *out_index) {
  if (!params || !name || !out_index) {
    return 0;
  }

  for (size_t i = 0; i < count; i++) {
    if (params[i] && strcmp(params[i], name) == 0) {
      *out_index = i;
      return 1;
    }
  }

  return 0;
}

static int parser_parse_bound_list(Parser *parser, char **out_bounds) {
  if (!parser || !out_bounds) {
    return 0;
  }

  do {
    char *trait_name =
        parser_parse_qualified_name(parser, "Expected trait name in bound");
    if (!trait_name) {
      return 0;
    }

    if (!parser_append_type_param_bound(out_bounds, trait_name)) {
      free(trait_name);
      parser_set_error(parser, "Out of memory while parsing trait bounds");
      return 0;
    }
    free(trait_name);

    if (parser->current_token.type != TOKEN_PLUS) {
      break;
    }
    parser_advance(parser);
  } while (1);

  return 1;
}

static int parser_parse_where_clause(Parser *parser, char **type_params,
                                     char **type_param_traits,
                                     size_t type_param_count) {
  if (!parser || parser->current_token.type != TOKEN_WHERE) {
    return 1;
  }

  parser_advance(parser); // consume 'where'

  do {
    size_t param_index = 0;
    char *param_name = NULL;

    if (!parser_is_identifier_like(parser->current_token.type)) {
      parser_set_error(parser, "Expected type parameter name in where clause");
      return 0;
    }

    param_name = strdup(parser->current_token.value);
    if (!param_name) {
      parser_set_error(parser, "Out of memory while parsing where clause");
      return 0;
    }
    parser_advance(parser);

    if (!parser_find_type_param_index(type_params, type_param_count, param_name,
                                      &param_index)) {
      parser_set_error(parser,
                       "Where clause references unknown type parameter");
      free(param_name);
      return 0;
    }
    free(param_name);

    if (!parser_expect(parser, TOKEN_COLON)) {
      return 0;
    }

    if (!parser_parse_bound_list(parser, &type_param_traits[param_index])) {
      return 0;
    }

    if (parser->current_token.type != TOKEN_COMMA) {
      break;
    }
    parser_advance(parser);
  } while (1);

  return 1;
}

static char *parser_parse_qualified_name(Parser *parser, const char *expected) {
  char *name = NULL;

  if (!parser || !expected) {
    return NULL;
  }

  if (!parser_is_identifier_like(parser->current_token.type) &&
      !parser_is_type_keyword(parser->current_token.type)) {
    parser_set_error(parser, expected);
    return NULL;
  }

  name = strdup(parser->current_token.value);
  if (!name) {
    return NULL;
  }
  parser_advance(parser);

  while (parser->current_token.type == TOKEN_DOT) {
    char *qualified_name = NULL;
    size_t qualified_len = 0;

    parser_advance(parser);
    if (!parser_is_identifier_like(parser->current_token.type) &&
        !parser_is_type_keyword(parser->current_token.type)) {
      parser_set_error(parser, expected);
      free(name);
      return NULL;
    }

    qualified_len = strlen(name) + 1 + strlen(parser->current_token.value) + 1;
    qualified_name = malloc(qualified_len);
    if (!qualified_name) {
      free(name);
      return NULL;
    }

    snprintf(qualified_name, qualified_len, "%s.%s", name,
             parser->current_token.value);
    free(name);
    name = qualified_name;
    parser_advance(parser);
  }

  return name;
}

static char **parser_parse_type_param_list(Parser *parser, char ***out_traits,
                                           size_t *out_count) {
  *out_count = 0;
  *out_traits = NULL;
  if (parser->current_token.type != TOKEN_LESS_THAN)
    return NULL;
  parser_advance(parser); // consume '<'

  char **params = NULL;
  char **traits = NULL;
  size_t count = 0;

  while (parser->current_token.type != TOKEN_GREATER_THAN &&
         parser->current_token.type != TOKEN_EOF) {
    if (!parser_is_identifier_like(parser->current_token.type)) {
      parser_set_error(parser, "Expected type parameter name");
      parser_free_type_param_list(params, traits, count);
      return NULL;
    }
    params = realloc(params, (count + 1) * sizeof(char *));
    traits = realloc(traits, (count + 1) * sizeof(char *));
    params[count] = strdup(parser->current_token.value);
    traits[count] = NULL;
    count++;
    parser_advance(parser);

    if (parser->current_token.type == TOKEN_COLON) {
      parser_advance(parser);
      if (!parser_parse_bound_list(parser, &traits[count - 1])) {
        parser_free_type_param_list(params, traits, count);
        return NULL;
      }
    }

    if (parser->current_token.type == TOKEN_COMMA) {
      parser_advance(parser);
    } else if (parser->current_token.type != TOKEN_GREATER_THAN) {
      parser_set_error(parser, "Expected ',' or '>' in type parameter list");
      parser_free_type_param_list(params, traits, count);
      return NULL;
    }
  }

  if (!parser_expect(parser, TOKEN_GREATER_THAN)) {
    parser_free_type_param_list(params, traits, count);
    return NULL;
  }

  *out_count = count;
  *out_traits = traits;
  return params;
}

static char *parser_parse_type_annotation(Parser *parser) {
  if (!parser)
    return NULL;

  char *type_name = NULL;

  /* Function pointer type: fn(param_types) -> return_type (thin), or
   * Fn(param_types) -> return_type (a stateful closure type). */
  int is_closure_fn = parser_is_identifier_like(parser->current_token.type) &&
                      parser->current_token.value &&
                      strcmp(parser->current_token.value, "Fn") == 0 &&
                      parser->peek_token.type == TOKEN_LPAREN;
  if (parser->current_token.type == TOKEN_FN || is_closure_fn) {
    parser_advance(parser); /* consume 'fn' or 'Fn' */
    if (!parser_expect(parser, TOKEN_LPAREN)) {
      parser_set_error(parser,
                       "Expected '(' after 'fn' in function pointer type");
      return NULL;
    }
    char *params_buf = malloc(1024);
    if (!params_buf)
      return NULL;
    size_t params_len = 0;
    size_t params_cap = 1024;
    params_buf[0] = '\0';
    int first = 1;
    while (parser->current_token.type != TOKEN_RPAREN &&
           parser->current_token.type != TOKEN_EOF && !parser->has_error) {
      if (!first) {
        if (parser->current_token.type != TOKEN_COMMA) {
          parser_set_error(
              parser, "Expected ',' or ')' in function pointer parameter list");
          free(params_buf);
          return NULL;
        }
        parser_advance(parser); /* consume ',' */
        if (params_len + 1 >= params_cap) {
          params_cap *= 2;
          params_buf = realloc(params_buf, params_cap);
          if (!params_buf)
            return NULL;
        }
        params_buf[params_len++] = ',';
        params_buf[params_len] = '\0';
      }
      first = 0;
      char *param = parser_parse_type_annotation(parser);
      if (!param) {
        if (!parser->has_error)
          parser_set_error(parser,
                           "Expected type in function pointer parameter list");
        free(params_buf);
        return NULL;
      }
      size_t plen = strlen(param);
      while (params_len + plen + 1 >= params_cap) {
        params_cap *= 2;
        params_buf = realloc(params_buf, params_cap);
        if (!params_buf) {
          free(param);
          free(params_buf);
          return NULL;
        }
      }
      memcpy(params_buf + params_len, param, plen + 1);
      params_len += plen;
      free(param);
    }
    if (!parser_expect(parser, TOKEN_RPAREN)) {
      free(params_buf);
      return NULL;
    }
    if (!parser_expect(parser, TOKEN_ARROW)) {
      parser_set_error(parser,
                       "Expected '->' after ')' in function pointer type");
      free(params_buf);
      return NULL;
    }
    char *ret = parser_parse_type_annotation(parser);
    if (!ret) {
      if (!parser->has_error)
        parser_set_error(
            parser, "Expected return type after '->' in function pointer type");
      free(params_buf);
      return NULL;
    }
    size_t fn_len = 4 + params_len + 3 + strlen(ret) +
                    1; /* "fn(" + params + ")->" + ret + NUL */
    type_name = malloc(fn_len);
    if (!type_name) {
      free(params_buf);
      free(ret);
      return NULL;
    }
    snprintf(type_name, fn_len, is_closure_fn ? "Fn(%s)->%s" : "fn(%s)->%s",
             params_buf, ret);
    free(params_buf);
    free(ret);
    /* Continue to allow * and [] suffixes (e.g. fn()->int32* is nonsensical but
     * we handle it) */
  } else if (!parser_is_type_keyword(parser->current_token.type) &&
             !parser_is_identifier_like(parser->current_token.type)) {
    return NULL;
  } else {
    type_name = strdup(parser->current_token.value);
    parser_advance(parser);
    if (!type_name)
      return NULL;
  }

  while (parser->current_token.type == TOKEN_DOT) {
    char *qualified_type = NULL;
    size_t qualified_len = 0;

    parser_advance(parser); // consume '.'
    if (!parser_is_identifier_like(parser->current_token.type) &&
        !parser_is_type_keyword(parser->current_token.type)) {
      parser_set_error(parser, "Expected type name after '.'");
      free(type_name);
      return NULL;
    }

    qualified_len =
        strlen(type_name) + 1 + strlen(parser->current_token.value) + 1;
    qualified_type = malloc(qualified_len);
    if (!qualified_type) {
      free(type_name);
      return NULL;
    }

    snprintf(qualified_type, qualified_len, "%s.%s", type_name,
             parser->current_token.value);
    free(type_name);
    type_name = qualified_type;
    parser_advance(parser);
  }

  if (parser->current_token.type == TOKEN_LESS_THAN) {
    parser_advance(parser); // consume '<'

    char *args_buf = malloc(1024);
    size_t args_len = 0;
    size_t args_cap = 1024;
    args_buf[0] = '\0';

    int first = 1;
    while (parser->current_token.type != TOKEN_GREATER_THAN &&
           parser->current_token.type != TOKEN_EOF && !parser->has_error) {
      if (!first) {
        if (parser->current_token.type != TOKEN_COMMA) {
          parser_set_error(parser, "Expected ',' or '>' in type argument list");
          free(args_buf);
          free(type_name);
          return NULL;
        }
        parser_advance(parser); // consume ','

        if (args_len + 1 >= args_cap) {
          args_cap *= 2;
          char *new_args_buf = realloc(args_buf, args_cap);
          if (!new_args_buf) {
            free(args_buf);
            free(type_name);
            return NULL;
          }
          args_buf = new_args_buf;
        }
        args_buf[args_len++] = ',';
        args_buf[args_len] = '\0';
      }
      first = 0;

      char *arg = parser_parse_type_annotation(parser);
      if (!arg) {
        if (!parser->has_error)
          parser_set_error(parser, "Expected type in type argument list");
        free(args_buf);
        free(type_name);
        return NULL;
      }

      size_t arg_len = strlen(arg);
      while (args_len + arg_len + 1 >= args_cap) {
        args_cap *= 2;
        char *new_args_buf = realloc(args_buf, args_cap);
        if (!new_args_buf) {
          free(arg);
          free(args_buf);
          free(type_name);
          return NULL;
        }
        args_buf = new_args_buf;
      }
      memcpy(args_buf + args_len, arg, arg_len);
      args_len += arg_len;
      args_buf[args_len] = '\0';
      free(arg);
    }

    if (!parser_expect(parser, TOKEN_GREATER_THAN)) {
      free(args_buf);
      free(type_name);
      return NULL;
    }

    size_t full_len = strlen(type_name) + 1 + args_len + 1 + 1;
    char *full_type = malloc(full_len);
    snprintf(full_type, full_len, "%s<%s>", type_name, args_buf);
    free(type_name);
    free(args_buf);
    type_name = full_type;
  }

  while (parser->current_token.type == TOKEN_MULTIPLY) {
    size_t next_len = strlen(type_name) + 2;
    char *next_type = malloc(next_len);
    if (!next_type) {
      free(type_name);
      return NULL;
    }

    snprintf(next_type, next_len, "%s*", type_name);
    free(type_name);
    type_name = next_type;
    parser_advance(parser); // consume '*'
  }

  if (parser->current_token.type != TOKEN_LBRACKET) {
    return type_name;
  }

  parser_advance(parser); // consume '['

  if (parser->current_token.type != TOKEN_NUMBER) {
    free(type_name);
    parser_set_error(parser, "Expected array size after '['");
    return NULL;
  }

  char *size_text = strdup(parser->current_token.value);
  parser_advance(parser);

  if (!size_text) {
    free(type_name);
    return NULL;
  }

  if (!parser_expect(parser, TOKEN_RBRACKET)) {
    free(type_name);
    free(size_text);
    return NULL;
  }

  size_t full_len = strlen(type_name) + strlen(size_text) + 3;
  char *full_type = malloc(full_len);
  if (!full_type) {
    free(type_name);
    free(size_text);
    return NULL;
  }

  snprintf(full_type, full_len, "%s[%s]", type_name, size_text);
  free(type_name);
  free(size_text);
  return full_type;
}

static int parser_literal_radix_hint(const char *value) {
  if (!value || value[0] != '0' || value[1] == '\0') {
    return 10;
  }
  if (value[1] == 'x' || value[1] == 'X') {
    return 16;
  }
  if (value[1] == 'b' || value[1] == 'B') {
    return 2;
  }
  /* Leading zeros without 0x/0b prefix are still decimal (e.g. "007"). */
  return 10;
}

/*
 * Parses integer TOKEN_NUMBER lexeme text into long long bits and radix.
 * Handles decimal, hexadecimal (strtoull 16), and binary (0b) digits only.
 */
static int parser_parse_integer_literal_string(
    Parser *parser, const char *value, long long *out_value,
    unsigned char *out_radix) {
  if (!parser || !value || !out_value || !out_radix) {
    return 0;
  }

  int hint = parser_literal_radix_hint(value);

  errno = 0;
  char *end = NULL;

  if (hint == 16) {
    unsigned long long u = strtoull(value, &end, 16);
    if (end == value || *end != '\0') {
      parser_set_error(parser, "Invalid hexadecimal literal");
      return 0;
    }
    if (errno == ERANGE) {
      parser_set_error(parser, "Hexadecimal literal is out of range");
      return 0;
    }
    *out_radix = 16;
    *out_value = (long long)u;
    return 1;
  }

  if (hint == 2) {
    const char *p = value + 2;
    if (*p == '\0') {
      parser_set_error(parser, "Invalid binary literal");
      return 0;
    }
    unsigned long long u = 0;
    while (*p) {
      if (*p != '0' && *p != '1') {
        parser_set_error(parser, "Invalid binary literal");
        return 0;
      }
      if (u > (ULLONG_MAX >> 1)) {
        parser_set_error(parser, "Binary literal is out of range");
        return 0;
      }
      u = u * 2 + (unsigned long long)(*p - '0');
      p++;
    }
    *out_radix = 2;
    *out_value = (long long)u;
    return 1;
  }

  long long s = strtoll(value, &end, 10);
  if (end == value || *end != '\0') {
    parser_set_error(parser, "Invalid integer literal");
    return 0;
  }
  if (errno == ERANGE) {
    parser_set_error(parser, "Decimal integer literal is out of range");
    return 0;
  }
  *out_radix = 10;
  *out_value = s;
  return 1;
}

static ASTNode *parser_parse_for_initializer(Parser *parser) {
  if (!parser)
    return NULL;

  if (parser->current_token.type == TOKEN_SEMICOLON) {
    return NULL;
  }

  if (parser->current_token.type == TOKEN_VAR) {
    return parser_parse_var_declaration(parser);
  }

  ASTNode *expr = parser_parse_expression(parser);
  if (!expr)
    return NULL;

  if (parser_is_assignment_token(parser->current_token.type)) {
    return parser_parse_assignment_from_target(parser, expr);
  }

  return expr;
}

static int parser_identifier_name_is(ASTNode *expr, const char *name) {
  if (!expr || expr->type != AST_IDENTIFIER || !name) {
    return 0;
  }

  Identifier *id = (Identifier *)expr->data;
  return id && id->name && strcmp(id->name, name) == 0;
}

static int parser_parse_parameter_list(Parser *parser, char ***out_names,
                                       char ***out_types, size_t *out_count);

/* Anonymous function (lambda) expression: `fn(params) [-> ret] { body }`. In
 * expression position `fn` always begins a lambda; the type spelling
 * `fn(...)->R` is parsed only in type positions by parser_parse_type_annotation,
 * and named declarations only at the top level via parse_declaration. The node
 * is an AST_LAMBDA_EXPRESSION carrying a FunctionDeclaration payload with a NULL
 * name; the closure-conversion pass lifts it to a real top-level function. */
static ASTNode *parser_parse_lambda_expression(Parser *parser) {
  SourceLocation location = parser_current_location(parser);
  parser_advance(parser); // consume 'fn'

  if (!parser_expect(parser, TOKEN_LPAREN)) {
    return NULL;
  }

  char **param_names = NULL;
  char **param_types = NULL;
  size_t param_count = 0;
  if (!parser_parse_parameter_list(parser, &param_names, &param_types,
                                   &param_count)) {
    return NULL;
  }

  if (!parser_expect(parser, TOKEN_RPAREN)) {
    for (size_t i = 0; i < param_count; i++) {
      free(param_names[i]);
      free(param_types[i]);
    }
    free(param_names);
    free(param_types);
    return NULL;
  }

  char *return_type = NULL;
  if (parser->current_token.type == TOKEN_ARROW ||
      parser->current_token.type == TOKEN_COLON) {
    parser_advance(parser);
    return_type = parser_parse_type_annotation(parser);
    if (!return_type) {
      if (!parser->has_error) {
        parser_set_error(parser, "Expected return type after '->' in lambda");
      }
      for (size_t i = 0; i < param_count; i++) {
        free(param_names[i]);
        free(param_types[i]);
      }
      free(param_names);
      free(param_types);
      return NULL;
    }
  } else {
    return_type = strdup("void");
  }

  if (parser->current_token.type != TOKEN_LBRACE) {
    parser_set_error(parser, "Expected '{' to begin lambda body");
    for (size_t i = 0; i < param_count; i++) {
      free(param_names[i]);
      free(param_types[i]);
    }
    free(param_names);
    free(param_types);
    free(return_type);
    return NULL;
  }

  ASTNode *body = parser_parse_block(parser);
  if (!body) {
    for (size_t i = 0; i < param_count; i++) {
      free(param_names[i]);
      free(param_types[i]);
    }
    free(param_names);
    free(param_types);
    free(return_type);
    return NULL;
  }

  ASTNode *node = ast_create_function_declaration(
      NULL, param_names, param_types, param_count, return_type, body, location);
  for (size_t i = 0; i < param_count; i++) {
    free(param_names[i]);
    free(param_types[i]);
  }
  free(param_names);
  free(param_types);
  free(return_type);
  if (!node) {
    ast_destroy_node(body);
    return NULL;
  }
  node->type = AST_LAMBDA_EXPRESSION;
  return node;
}

ASTNode *parser_parse_primary_expression(Parser *parser) {
  if (!parser)
    return NULL;

  SourceLocation location = parser_current_location(parser);

  if (parser_is_identifier_like(parser->current_token.type)) {
    ASTNode *result =
        ast_create_identifier(parser->current_token.value, location);
    parser_advance(parser);
    return result;
  }

  switch (parser->current_token.type) {
  case TOKEN_NUMBER: {
    // Check if it's a float or integer
    char *value = parser->current_token.value;
    ASTNode *result;

    if (strchr(value, '.')) {
      double float_val = atof(value);
      result = ast_create_float_literal(float_val, location);
    } else {
      long long int_val = 0;
      unsigned char int_radix = 10;
      if (!parser_parse_integer_literal_string(parser, value, &int_val,
                                               &int_radix)) {
        return NULL;
      }
      result = ast_create_number_literal(int_val, location, int_radix);
      if (!result) {
        return NULL;
      }
    }

    parser_advance(parser);
    return result;
  }
  case TOKEN_STRING: {
    ASTNode *result =
        ast_create_string_literal(parser->current_token.value, location);
    parser_advance(parser);
    return result;
  }
  case TOKEN_IMPORT_STR: {
    parser_advance(parser); // consume 'import_str'
    if (parser->current_token.type != TOKEN_STRING) {
      parser_set_error(parser, "Expected string literal after 'import_str'");
      return NULL;
    }
    char *file_path = strdup(parser->current_token.value);
    parser_advance(parser); // consume the string
    ASTNode *node = ast_create_import_str(file_path, location);
    free(file_path);
    return node;
  }
  case TOKEN_LPAREN: {
    parser_advance(parser); // consume '('
    ASTNode *expr = parser_parse_expression(parser);
    if (!parser_expect(parser, TOKEN_RPAREN)) {
      ast_destroy_node(expr);
      return NULL;
    }
    return expr;
  }
  case TOKEN_FN:
  case TOKEN_FUNCTION:
    return parser_parse_lambda_expression(parser);
  case TOKEN_MATCH:
    return parser_parse_match_expression(parser);
  case TOKEN_THIS: {
    parser_advance(parser);
    return ast_create_identifier("this", location);
  }
  case TOKEN_NEW: {
    parser_advance(parser); // Built-in memory alloc handling
    if (!parser_is_identifier_like(parser->current_token.type) &&
        !parser_is_type_keyword(parser->current_token.type) &&
        parser->current_token.type != TOKEN_FN) {
      parser_set_error(parser, "Expected type name after 'new'");
      return NULL;
    }
    char *type_name = parser_parse_type_annotation(parser);
    if (!type_name) {
      return NULL;
    }

    ASTNode *new_expr = ast_create_new_expression(type_name, location);
    free(type_name);
    return new_expr;
  }
  default:
    parser_set_error(parser, "Expected primary expression");
    return NULL;
  }
}

typedef struct {
  size_t lexer_position;
  size_t lexer_line;
  size_t lexer_column;
  size_t lexer_continuation_depth;
  Token current_token;
  Token peek_token;
  int has_error;
  int had_error;
  size_t error_count;
  char *error_message;
  ErrorReporter *error_reporter;
} ParserSavedState;

static ParserSavedState parser_save_state(Parser *parser) {
  ParserSavedState state;
  state.lexer_position = parser->lexer->position;
  state.lexer_line = parser->lexer->line;
  state.lexer_column = parser->lexer->column;
  state.lexer_continuation_depth = parser->lexer->continuation_depth;
  state.current_token = token_clone(&parser->current_token);
  state.peek_token = token_clone(&parser->peek_token);
  state.has_error = parser->has_error;
  state.had_error = parser->had_error;
  state.error_count = parser->error_count;
  state.error_message =
      parser->error_message ? strdup(parser->error_message) : NULL;
  state.error_reporter = parser->error_reporter;
  return state;
}

static void parser_restore_state(Parser *parser,
                                 const ParserSavedState *state) {
  parser->lexer->position = state->lexer_position;
  parser->lexer->line = state->lexer_line;
  parser->lexer->column = state->lexer_column;
  parser->lexer->continuation_depth = state->lexer_continuation_depth;

  token_destroy(&parser->current_token);
  parser->current_token = token_clone(&state->current_token);
  token_destroy(&parser->peek_token);
  parser->peek_token = token_clone(&state->peek_token);

  parser->has_error = state->has_error;
  parser->had_error = state->had_error;
  parser->error_count = state->error_count;
  parser->error_reporter = state->error_reporter;
  free(parser->error_message);
  parser->error_message =
      state->error_message ? strdup(state->error_message) : NULL;
}

static void parser_discard_saved_state(ParserSavedState *state) {
  token_destroy(&state->current_token);
  token_destroy(&state->peek_token);
  free(state->error_message);
}

ASTNode *parser_parse_cast_expression(Parser *parser) {
  if (!parser || parser->current_token.type != TOKEN_LPAREN)
    return NULL;

  // Only try if the next token could start a type name
  if (!parser_is_type_keyword(parser->peek_token.type) &&
      !parser_is_identifier_like(parser->peek_token.type) &&
      parser->peek_token.type != TOKEN_FN) {
    return NULL;
  }

  ParserSavedState saved = parser_save_state(parser);
  parser->error_message = NULL;
  parser->error_reporter = NULL;

  SourceLocation location = parser_current_location(parser);

  parser_advance(parser); // consume '('

  // Remember whether the parenthesized type begins with a built-in type
  // keyword (int32, int64, float64, ...). The parser keeps no registry of
  // user-defined type names, so this is its only reliable signal that the
  // parenthesized token is genuinely a type rather than a value. We use it
  // below to disambiguate `(name) <op> x` where <op> is both a unary and a
  // binary operator (`&`, `*`, `+`, `-`).
  int type_starts_with_keyword =
      parser_is_type_keyword(parser->current_token.type);

  char *type_name = parser_parse_type_annotation(parser);
  if (!type_name || parser->has_error ||
      parser->current_token.type != TOKEN_RPAREN) {
    free(type_name);
    parser_restore_state(parser, &saved);
    parser_discard_saved_state(&saved);
    return NULL;
  }

  parser->error_reporter = saved.error_reporter;

  parser_advance(parser); // consume ')'

  // A bare single identifier (no built-in keyword, and no pointer/array/
  // generic/qualified structure such as `*`, `[`, `<` or `.`) gives the parser
  // no reason to believe it names a type. `(MyStruct*)`, `(Vec<T>)`,
  // `(mod.Type)` and the like are structurally types and stay casts.
  int looks_like_type =
      type_starts_with_keyword || strpbrk(type_name, "*[<.(") != NULL;

  // Check if it's a grouped expression instead
  int is_binary = parser_is_binary_operator(parser->current_token.type);
  int is_unary = parser_is_unary_operator(parser->current_token.type);
  if (parser->current_token.type == TOKEN_RPAREN ||
      parser->current_token.type == TOKEN_COMMA ||
      parser->current_token.type == TOKEN_SEMICOLON ||
      parser->current_token.type == TOKEN_EOF ||
      (is_binary && !is_unary) ||
      // Ambiguous prefix operator (`&`/`*`/`+`/`-`) after a token that does not
      // look like a type: treat `(name) <op> x` as a parenthesized expression
      // followed by a binary operator, not a cast of a unary expression.
      (is_binary && is_unary && !looks_like_type)) {
    free(type_name);
    parser_restore_state(parser, &saved);
    parser_discard_saved_state(&saved);
    return NULL;
  }

  parser_discard_saved_state(&saved);
  parser->has_error = 0;
  free(parser->error_message);
  parser->error_message = NULL;

  ASTNode *operand = parser_parse_unary_expression(parser);
  if (!operand) {
    free(type_name);
    return NULL;
  }

  ASTNode *cast_expr = ast_create_cast_expression(type_name, operand, location);
  free(type_name);
  return cast_expr;
}

ASTNode *parser_parse_unary_expression(Parser *parser) {
  if (!parser)
    return NULL;

  SourceLocation location = parser_current_location(parser);

  if (parser->current_token.type == TOKEN_LPAREN) {
    ASTNode *cast = parser_parse_cast_expression(parser);
    if (cast) {
      return cast;
    }
  }

  if (parser_is_unary_operator(parser->current_token.type)) {
    const char *operator = parser->current_token.value;
    parser_advance(parser);

    ASTNode *operand = parser_parse_unary_expression(parser);
    if (!operand) {
      return NULL;
    }

    return ast_create_unary_expression(operator, operand, location);
  }

  return parser_parse_postfix_expression(parser);
}

static int parser_try_parse_generic_call_type_args(Parser *parser,
                                                   char ***out_type_args,
                                                   size_t *out_type_arg_count) {
  *out_type_args = NULL;
  *out_type_arg_count = 0;

  if (parser->current_token.type != TOKEN_LESS_THAN)
    return 0;

  ParserSavedState saved = parser_save_state(parser);
  parser->error_message = NULL;

  parser_advance(parser); // consume '<'

  char **type_args = NULL;
  size_t type_arg_count = 0;
  int success = 1;

  while (parser->current_token.type != TOKEN_GREATER_THAN &&
         parser->current_token.type != TOKEN_EOF) {
    if (type_arg_count > 0) {
      if (parser->current_token.type != TOKEN_COMMA) {
        success = 0;
        break;
      }
      parser_advance(parser);
    }

    char *arg = parser_parse_type_annotation(parser);
    if (!arg || parser->has_error) {
      free(arg);
      success = 0;
      break;
    }

    type_args = realloc(type_args, (type_arg_count + 1) * sizeof(char *));
    type_args[type_arg_count++] = arg;
  }

  if (success && parser->current_token.type == TOKEN_GREATER_THAN) {
    parser_advance(parser); // consume '>'
    if (parser->current_token.type == TOKEN_LPAREN) {
      *out_type_args = type_args;
      *out_type_arg_count = type_arg_count;
      parser_discard_saved_state(&saved);
      parser->has_error = 0;
      free(parser->error_message);
      parser->error_message = NULL;
      return 1;
    }
  }

  for (size_t i = 0; i < type_arg_count; i++)
    free(type_args[i]);
  free(type_args);

  parser_restore_state(parser, &saved);
  parser_discard_saved_state(&saved);

  return 0;
}

// Kernel index built-ins: maps `<obj>.<axis>` to its GPU sreg intrinsic
// link-name (gpu_tid_x etc.), or NULL if not a built-in. Mirrors CUDA:
//   thread.x     -> gpu_tid_x     (threadIdx.x)
//   block.x      -> gpu_ctaid_x   (blockIdx.x)
//   block_dim.x  -> gpu_ntid_x    (blockDim.x)
//   grid_dim.x   -> gpu_nctaid_x  (gridDim.x)
// Only consulted in --emit-ptx (gpu_mode) compiles. Returns a pointer into a
// static buffer (single-threaded parse; copied immediately by the caller).
static const char *parser_gpu_index_intrinsic(const char *obj,
                                              const char *axis) {
  if (!obj || !axis || axis[0] == 0 || axis[1] != 0) {
    return NULL;
  }
  char a = axis[0];
  if (a != 'x' && a != 'y' && a != 'z') {
    return NULL;
  }
  const char *base = NULL;
  if (strcmp(obj, "thread") == 0) {
    base = "gpu_tid_";
  } else if (strcmp(obj, "block") == 0) {
    base = "gpu_ctaid_";
  } else if (strcmp(obj, "block_dim") == 0) {
    base = "gpu_ntid_";
  } else if (strcmp(obj, "grid_dim") == 0) {
    base = "gpu_nctaid_";
  } else {
    return NULL;
  }
  static char buf[24];
  snprintf(buf, sizeof(buf), "%s%c", base, a);
  return buf;
}

ASTNode *parser_parse_postfix_expression(Parser *parser) {
  if (!parser)
    return NULL;

  ASTNode *expr = parser_parse_primary_expression(parser);
  if (!expr)
    return NULL;

  while (1) {
    SourceLocation location = parser_current_location(parser);

    if (expr->type == AST_IDENTIFIER &&
        parser->current_token.type == TOKEN_LESS_THAN) {
      char **call_type_args = NULL;
      size_t call_type_arg_count = 0;
      if (parser_try_parse_generic_call_type_args(parser, &call_type_args,
                                                  &call_type_arg_count)) {
        parser_advance(parser); // consume '('

        ASTNode **arguments = NULL;
        size_t arg_count = 0;

        if (parser->current_token.type != TOKEN_RPAREN) {
          do {
            ASTNode *arg = parser_parse_expression(parser);
            if (!arg)
              break;
            arguments = realloc(arguments, (arg_count + 1) * sizeof(ASTNode *));
            arguments[arg_count++] = arg;
            if (parser->current_token.type == TOKEN_COMMA) {
              parser_advance(parser);
            } else if (parser->current_token.type == TOKEN_RPAREN) {
              break;
            } else {
              parser_set_error(parser, "Expected ',' or ')' in argument list");
              break;
            }
          } while (1);
        }

        if (!parser_expect(parser, TOKEN_RPAREN)) {
          for (size_t i = 0; i < arg_count; i++)
            ast_destroy_node(arguments[i]);
          free(arguments);
          for (size_t i = 0; i < call_type_arg_count; i++)
            free(call_type_args[i]);
          free(call_type_args);
          ast_destroy_node(expr);
          return NULL;
        }

        Identifier *id_data = (Identifier *)expr->data;
        char *func_name = strdup(id_data->name);
        ast_destroy_node(expr);

        expr = ast_create_call_expression(func_name, arguments, arg_count,
                                          location);
        if (expr && expr->data) {
          CallExpression *ce = (CallExpression *)expr->data;
          ce->type_args = call_type_args;
          ce->type_arg_count = call_type_arg_count;
        } else {
          for (size_t i = 0; i < call_type_arg_count; i++)
            free(call_type_args[i]);
          free(call_type_args);
        }
        free(func_name);
        continue;
      }
    }

    if (parser->current_token.type == TOKEN_LPAREN) {
      // Function call or method call
      parser_advance(parser); // consume '('

      // Parse arguments first (common to both cases)
      ASTNode **arguments = NULL;
      size_t arg_count = 0;

      if (parser_identifier_name_is(expr, "sizeof")) {
        SourceLocation type_location = parser_current_location(parser);
        if (parser->current_token.type == TOKEN_RPAREN) {
          parser_set_error(parser, "Expected type name in sizeof");
          ast_destroy_node(expr);
          return NULL;
        }

        char *type_name = parser_parse_type_annotation(parser);
        if (!type_name) {
          if (!parser->has_error) {
            parser_set_error(parser, "Expected type name in sizeof");
          }
          ast_destroy_node(expr);
          return NULL;
        }

        ASTNode *type_arg = ast_create_identifier(type_name, type_location);
        free(type_name);
        if (!type_arg) {
          ast_destroy_node(expr);
          return NULL;
        }

        arguments = malloc(sizeof(ASTNode *));
        if (!arguments) {
          ast_destroy_node(type_arg);
          ast_destroy_node(expr);
          parser_set_error(parser, "Out of memory parsing sizeof");
          return NULL;
        }
        arguments[0] = type_arg;
        arg_count = 1;

        if (parser->current_token.type == TOKEN_COMMA) {
          parser_set_error(parser, "sizeof expects exactly one type argument");
          ast_destroy_node(type_arg);
          free(arguments);
          ast_destroy_node(expr);
          return NULL;
        }
      } else if (parser->current_token.type != TOKEN_RPAREN) {
        do {
          ASTNode *arg = parser_parse_expression(parser);
          if (!arg)
            break;

          arguments = realloc(arguments, (arg_count + 1) * sizeof(ASTNode *));
          arguments[arg_count] = arg;
          arg_count++;

          if (parser->current_token.type == TOKEN_COMMA) {
            parser_advance(parser);
          } else if (parser->current_token.type == TOKEN_RPAREN) {
            break;
          } else {
            parser_set_error(parser, "Expected ',' or ')' in argument list");
            break;
          }
        } while (1);
      }

      if (!parser_expect(parser, TOKEN_RPAREN)) {
        for (size_t i = 0; i < arg_count; i++) {
          ast_destroy_node(arguments[i]);
        }
        free(arguments);
        ast_destroy_node(expr);
        return NULL;
      }

      if (expr->type == AST_MEMBER_ACCESS) {
        // Method call: obj.method(args)
        MemberAccess *access = (MemberAccess *)expr->data;
        char *method_name = strdup(access->member);
        ASTNode *object = access->object;
        // Detach the object from the member access so it's not double-freed
        access->object = NULL;
        free(expr->children);
        expr->children = NULL;
        expr->child_count = 0;
        ast_destroy_node(expr);

        expr = ast_create_method_call(object, method_name, arguments, arg_count,
                                      location);
        free(method_name);
      } else if (expr->type == AST_IDENTIFIER) {
        // Regular function call (or function pointer variable - type checker
        // validates)
        Identifier *id_data = (Identifier *)expr->data;
        char *func_name = strdup(id_data->name);
        ast_destroy_node(expr);

        expr = ast_create_call_expression(func_name, arguments, arg_count,
                                          location);
        free(func_name);
      } else {
        // (expr)(args) / f(...)(args): call through the value the expression
        // produces - a thin function pointer or a closure. The node owns the
        // callee expression and the argument nodes; only the array is ours.
        ASTNode *fp = ast_create_func_ptr_call(expr, arguments, arg_count,
                                               location);
        free(arguments);
        if (!fp) {
          return NULL;
        }
        expr = fp;
      }

    } else if (parser->current_token.type == TOKEN_DOT) {
      // Member access
      parser_advance(parser); // consume '.'

      if (parser->current_token.type != TOKEN_IDENTIFIER) {
        parser_set_error(parser, "Expected member name after '.'");
        ast_destroy_node(expr);
        return NULL;
      }

      char *member = strdup(parser->current_token.value);
      parser_advance(parser);

      // GPU kernel index built-ins: `thread.x` etc. desugar to a call to the
      // corresponding gpu_* intrinsic (only in --emit-ptx compiles).
      const char *gpu_intr = NULL;
      if (parser->gpu_mode && expr->type == AST_IDENTIFIER && expr->data) {
        gpu_intr =
            parser_gpu_index_intrinsic(((Identifier *)expr->data)->name, member);
      }
      if (gpu_intr) {
        ASTNode *call = ast_create_call_expression(gpu_intr, NULL, 0, location);
        ast_destroy_node(expr);
        free(member);
        if (!call) {
          return NULL;
        }
        expr = call;
      } else {
        expr = ast_create_member_access(expr, member, location);
        free(member);
      }

    } else if (parser->current_token.type == TOKEN_ARROW) {
      // Pointer member access: p->field == (*p).field
      parser_advance(parser); // consume '->'

      if (parser->current_token.type != TOKEN_IDENTIFIER) {
        parser_set_error(parser, "Expected member name after '->'");
        ast_destroy_node(expr);
        return NULL;
      }

      char *member = strdup(parser->current_token.value);
      parser_advance(parser);

      ASTNode *deref = ast_create_unary_expression("*", expr, location);
      if (!deref) {
        free(member);
        ast_destroy_node(expr);
        return NULL;
      }

      expr = ast_create_member_access(deref, member, location);
      free(member);

    } else if (parser->current_token.type == TOKEN_LBRACKET) {
      parser_advance(parser); // consume '['

      ASTNode *index_expr = parser_parse_expression(parser);
      if (!index_expr) {
        ast_destroy_node(expr);
        return NULL;
      }

      if (!parser_expect(parser, TOKEN_RBRACKET)) {
        ast_destroy_node(index_expr);
        ast_destroy_node(expr);
        return NULL;
      }

      expr = ast_create_array_index_expression(expr, index_expr, location);

    } else {
      break;
    }
  }

  return expr;
}

ASTNode *parser_parse_binary_expression(Parser *parser, int min_precedence) {
  if (!parser)
    return NULL;

  ASTNode *left = parser_parse_unary_expression(parser);
  if (!left)
    return NULL;

  while (parser_is_binary_operator(parser->current_token.type)) {
    SourceLocation location = parser_current_location(parser);
    int precedence = parser_get_operator_precedence(parser->current_token.type);
    if (precedence < min_precedence)
      break;

    char *operator = strdup(parser->current_token.value);
    parser_advance(parser);

    ASTNode *right = parser_parse_binary_expression(parser, precedence + 1);
    if (!right) {
      free(operator);
      ast_destroy_node(left);
      return NULL;
    }

    left = ast_create_binary_expression(left, operator, right, location);
    free(operator);
  }

  return left;
}

// Parse an optional platform guard on an import: `import "..." if windows;`
// (or `if linux`). On success writes a heap-allocated platform name to
// *out_guard (or NULL when there is no guard) and returns 1; returns 0 on error.
static int parser_parse_import_guard(Parser *parser, char **out_guard) {
  *out_guard = NULL;
  if (parser->current_token.type != TOKEN_IF) {
    return 1;
  }
  parser_advance(parser); // consume 'if'
  if (!parser_is_identifier_like(parser->current_token.type) ||
      !parser->current_token.value) {
    parser_set_error(parser,
                     "Expected platform name after 'if' in import guard");
    return 0;
  }
  const char *name = parser->current_token.value;
  if (strcmp(name, "windows") != 0 && strcmp(name, "linux") != 0) {
    parser_set_error(parser,
                     "Import guard platform must be 'windows' or 'linux'");
    return 0;
  }
  *out_guard = strdup(name);
  parser_advance(parser); // consume platform name
  if (!*out_guard) {
    parser_set_error(parser, "Out of memory parsing import guard");
    return 0;
  }
  return 1;
}

ASTNode *parser_parse_import_declaration(Parser *parser) {
  if (!parser)
    return NULL;

  SourceLocation location = parser_current_location(parser);
  parser_advance(parser); // consume 'import'

  // Selective import: import { a, b } from "mod"
  if (parser->current_token.type == TOKEN_LBRACE) {
    parser_advance(parser); // consume '{'

    char **selected = NULL;
    size_t selected_count = 0;
    size_t selected_capacity = 0;

    while (parser->current_token.type != TOKEN_RBRACE &&
           parser->current_token.type != TOKEN_EOF) {
      if (!parser_is_identifier_like(parser->current_token.type)) {
        parser_set_error(parser, "Expected identifier in import list");
        for (size_t i = 0; i < selected_count; i++) free(selected[i]);
        free(selected);
        return NULL;
      }

      if (selected_count >= selected_capacity) {
        size_t new_cap = selected_capacity == 0 ? 8 : selected_capacity * 2;
        char **grown = realloc(selected, new_cap * sizeof(char *));
        if (!grown) {
          for (size_t i = 0; i < selected_count; i++) free(selected[i]);
          free(selected);
          parser_set_error(parser, "Out of memory parsing import list");
          return NULL;
        }
        selected = grown;
        selected_capacity = new_cap;
      }

      selected[selected_count++] = strdup(parser->current_token.value);
      parser_advance(parser); // consume name

      if (parser->current_token.type == TOKEN_COMMA) {
        parser_advance(parser); // consume ','
      } else {
        break;
      }
    }

    if (parser->current_token.type != TOKEN_RBRACE) {
      parser_set_error(parser, "Expected '}' after import list");
      for (size_t i = 0; i < selected_count; i++) free(selected[i]);
      free(selected);
      return NULL;
    }
    parser_advance(parser); // consume '}'

    // expect 'from'
    if (!parser_is_identifier_like(parser->current_token.type) ||
        !parser->current_token.value ||
        strcmp(parser->current_token.value, "from") != 0) {
      parser_set_error(parser, "Expected 'from' after import list");
      for (size_t i = 0; i < selected_count; i++) free(selected[i]);
      free(selected);
      return NULL;
    }
    parser_advance(parser); // consume 'from'

    if (parser->current_token.type != TOKEN_STRING) {
      parser_set_error(parser, "Expected string literal after 'from'");
      for (size_t i = 0; i < selected_count; i++) free(selected[i]);
      free(selected);
      return NULL;
    }

    char *module_name = strdup(parser->current_token.value);
    parser_advance(parser); // consume string

    char *guard = NULL;
    if (!parser_parse_import_guard(parser, &guard)) {
      free(module_name);
      for (size_t i = 0; i < selected_count; i++) free(selected[i]);
      free(selected);
      return NULL;
    }

    if (!parser_expect_statement_end(parser)) {
      free(guard);
      free(module_name);
      for (size_t i = 0; i < selected_count; i++) free(selected[i]);
      free(selected);
      return NULL;
    }

    ASTNode *node = ast_create_import_declaration(
        module_name, NULL, (const char **)selected, selected_count, location);
    if (node && node->data && guard) {
      ((ImportDeclaration *)node->data)->platform_guard = guard; // transfer
    } else {
      free(guard);
    }
    free(module_name);
    for (size_t i = 0; i < selected_count; i++) free(selected[i]);
    free(selected);
    return node;
  }

  // Plain import or namespaced import
  if (parser->current_token.type != TOKEN_STRING) {
    parser_set_error(parser, "Expected string literal after 'import'");
    return NULL;
  }

  char *module_name = strdup(parser->current_token.value);
  char *namespace_alias = NULL;
  parser_advance(parser); // consume string

  if (parser_is_identifier_like(parser->current_token.type) &&
      parser->current_token.value &&
      strcmp(parser->current_token.value, "as") == 0) {
    parser_advance(parser); // consume 'as'
    if (!parser_is_identifier_like(parser->current_token.type)) {
      free(module_name);
      parser_set_error(parser, "Expected namespace alias after 'as'");
      return NULL;
    }
    namespace_alias = strdup(parser->current_token.value);
    parser_advance(parser); // consume alias
  }

  char *guard = NULL;
  if (!parser_parse_import_guard(parser, &guard)) {
    free(module_name);
    free(namespace_alias);
    return NULL;
  }

  if (!parser_expect_statement_end(parser)) {
    free(guard);
    free(module_name);
    free(namespace_alias);
    return NULL;
  }

  ASTNode *node =
      ast_create_import_declaration(module_name, namespace_alias, NULL, 0, location);
  if (node && node->data && guard) {
    ((ImportDeclaration *)node->data)->platform_guard = guard; // transfer
  } else {
    free(guard);
  }
  free(module_name);
  free(namespace_alias);

  return node;
}

static ASTNode *parser_parse_extern_var_declaration(Parser *parser) {
  if (!parser) {
    return NULL;
  }

  SourceLocation location = parser_current_location(parser);
  if (!parser_expect(parser, TOKEN_VAR)) {
    return NULL;
  }

  if (!parser_is_identifier_like(parser->current_token.type)) {
    parser_set_error(parser, "Expected variable name after 'extern var'");
    return NULL;
  }

  char *var_name = strdup(parser->current_token.value);
  parser_advance(parser);
  if (!var_name) {
    parser_set_error(parser, "Memory allocation failed");
    return NULL;
  }

  if (parser->current_token.type != TOKEN_COLON) {
    parser_set_error(parser,
                     "Extern variable declarations require an explicit type");
    free(var_name);
    return NULL;
  }
  parser_advance(parser); // consume ':'

  char *type_name = parser_parse_type_annotation(parser);
  if (!type_name) {
    free(var_name);
    parser_set_error(parser, "Extern variable declarations require a type");
    return NULL;
  }

  char *link_name = NULL;
  if (parser->current_token.type == TOKEN_EQUALS) {
    parser_advance(parser); // consume '='
    if (parser->current_token.type != TOKEN_STRING) {
      parser_set_error(
          parser,
          "Extern variable declarations cannot have an initializer; expected "
          "string literal link name after '='");
      free(var_name);
      free(type_name);
      return NULL;
    }
    link_name = strdup(parser->current_token.value);
    parser_advance(parser);
    if (!link_name) {
      parser_set_error(parser, "Memory allocation failed for link name");
      free(var_name);
      free(type_name);
      return NULL;
    }
  }

  if (!parser_expect_statement_end(parser)) {
    free(var_name);
    free(type_name);
    free(link_name);
    return NULL;
  }

  ASTNode *var_decl =
      ast_create_var_declaration(var_name, type_name, NULL, location);
  free(var_name);
  free(type_name);
  if (!var_decl || !var_decl->data) {
    free(link_name);
    return var_decl;
  }

  VarDeclaration *var_data = (VarDeclaration *)var_decl->data;
  var_data->is_extern = 1;
  if (link_name) {
    var_data->link_name = strdup(link_name);
    if (!var_data->link_name) {
      free(link_name);
      ast_destroy_node(var_decl);
      parser_set_error(parser, "Memory allocation failed for link name");
      return NULL;
    }
  }
  free(link_name);

  return var_decl;
}

ASTNode *parser_parse_var_declaration(Parser *parser) {
  if (!parser)
    return NULL;

  SourceLocation location = parser_current_location(parser);
  // Expect 'var' or 'const' keyword
  int is_const = (parser->current_token.type == TOKEN_CONST);
  if (is_const) {
    parser_advance(parser); // consume 'const'
  } else if (!parser_expect(parser, TOKEN_VAR)) {
    return NULL;
  }

  // Expect identifier
  if (!parser_is_identifier_like(parser->current_token.type)) {
    parser_set_error(parser,
                     is_const ? "Expected identifier after 'const'"
                              : "Expected identifier after 'var'");
    return NULL;
  }

  char *var_name = strdup(parser->current_token.value);
  parser_advance(parser);

  char *type_name = NULL;
  ASTNode *initializer = NULL;

  // Optional type annotation: ': type'
  if (parser->current_token.type == TOKEN_COLON) {
    parser_advance(parser); // consume ':'

    type_name = parser_parse_type_annotation(parser);
    if (!type_name) {
      if (!parser->has_error) {
        parser_set_error(parser, "Expected type after ':'");
      }
      free(var_name);
      return NULL;
    }
  }

  // Optional initializer: '= expression'
  if (parser->current_token.type == TOKEN_EQUALS) {
    parser_advance(parser); // consume '='

    initializer = parser_parse_expression(parser);
    if (!initializer) {
      free(var_name);
      free(type_name);
      return NULL;
    }
  }

  // A constant must always have a value to fold at compile time.
  if (is_const && !initializer) {
    parser_set_error(parser, "Constant declaration requires an initializer");
    free(var_name);
    free(type_name);
    return NULL;
  }

  // For type inference, if no type is specified but there's an initializer,
  // we'll leave type_name as NULL and let the semantic analyzer infer it
  if (!type_name && !initializer) {
    parser_set_error(parser, "Variable declaration must have either a type "
                             "annotation or an initializer");
    free(var_name);
    return NULL;
  }

  // Expect a newline or semicolon to end the declaration
  parser_expect_statement_end(parser);

  ASTNode *var_decl =
      ast_create_var_declaration(var_name, type_name, initializer, location);
  if (var_decl && var_decl->data) {
    ((VarDeclaration *)var_decl->data)->is_const = is_const;
  }

  free(var_name);
  free(type_name);

  return var_decl;
}

static int parser_parse_parameter_list(Parser *parser, char ***out_names,
                                       char ***out_types,
                                       size_t *out_count) {
  char **param_names = NULL;
  char **param_types = NULL;
  size_t param_count = 0;

  if (parser->current_token.type != TOKEN_RPAREN) {
    do {
      if (!parser_is_identifier_like(parser->current_token.type)) {
        parser_set_error(parser, "Expected parameter name");
        goto fail;
      }

      param_names = realloc(param_names, (param_count + 1) * sizeof(char *));
      param_types = realloc(param_types, (param_count + 1) * sizeof(char *));

      param_names[param_count] = strdup(parser->current_token.value);
      parser_advance(parser);

      if (!parser_expect(parser, TOKEN_COLON)) {
        free(param_names[param_count]);
        goto fail;
      }

      param_types[param_count] = parser_parse_type_annotation(parser);
      if (!param_types[param_count]) {
        if (!parser->has_error) {
          parser_set_error(parser, "Expected parameter type");
        }
        free(param_names[param_count]);
        goto fail;
      }
      param_count++;

      if (parser->current_token.type == TOKEN_COMMA) {
        parser_advance(parser);
      } else if (parser->current_token.type == TOKEN_RPAREN) {
        break;
      } else {
        parser_set_error(parser, "Expected ',' or ')' in parameter list");
        goto fail;
      }
    } while (1);
  }

  *out_names = param_names;
  *out_types = param_types;
  *out_count = param_count;
  return 1;

fail:
  for (size_t i = 0; i < param_count; i++) {
    free(param_names[i]);
    free(param_types[i]);
  }
  free(param_names);
  free(param_types);
  return 0;
}

ASTNode *parser_parse_function_declaration(Parser *parser) {
  if (!parser)
    return NULL;

  SourceLocation location = parser_current_location(parser);
  if (parser->current_token.type != TOKEN_FUNCTION &&
      parser->current_token.type != TOKEN_FN &&
      parser->current_token.type != TOKEN_KERNEL) {
    parser_set_error(parser, "Expected 'fn' or 'kernel'");
    return NULL;
  }
  // `kernel` is the GPU-facing spelling of a top-level function; it parses
  // identically and is emitted as a PTX .entry by --emit-ptx.
  parser_advance(parser);

  // Expect function name
  if (!parser_is_identifier_like(parser->current_token.type)) {
    parser_set_error(parser, "Expected function name after 'fn'");
    return NULL;
  }

  char *func_name = strdup(parser->current_token.value);
  parser_advance(parser);

  char **func_type_params = NULL;
  char **func_type_param_traits = NULL;
  size_t func_type_param_count = 0;
  if (parser->current_token.type == TOKEN_LESS_THAN) {
    func_type_params = parser_parse_type_param_list(
        parser, &func_type_param_traits, &func_type_param_count);
    if (!func_type_params && parser->has_error) {
      free(func_name);
      return NULL;
    }
  }

  // Expect '('
  if (!parser_expect(parser, TOKEN_LPAREN)) {
    parser_free_type_param_list(func_type_params, func_type_param_traits,
                                func_type_param_count);
    free(func_name);
    return NULL;
  }

  char **param_names = NULL;
  char **param_types = NULL;
  size_t param_count = 0;

  if (!parser_parse_parameter_list(parser, &param_names, &param_types,
                                   &param_count)) {
    parser_free_type_param_list(func_type_params, func_type_param_traits,
                                func_type_param_count);
    free(func_name);
    return NULL;
  }

  // Expect ')'
  if (!parser_expect(parser, TOKEN_RPAREN)) {
    for (size_t i = 0; i < param_count; i++) {
      free(param_names[i]);
      free(param_types[i]);
    }
    free(param_names);
    free(param_types);
    parser_free_type_param_list(func_type_params, func_type_param_traits,
                                func_type_param_count);
    free(func_name);
    return NULL;
  }

  // Optional return type: '-> type' (or ': type' for compatibility)
  char *return_type = NULL;
  char *link_name = NULL;
  if (parser->current_token.type == TOKEN_ARROW ||
      parser->current_token.type == TOKEN_COLON) {
    parser_advance(parser); // consume return separator

    return_type = parser_parse_type_annotation(parser);
    if (!return_type) {
      if (!parser->has_error) {
        parser_set_error(parser, "Expected return type after return separator");
      }
      // Clean up
      for (size_t i = 0; i < param_count; i++) {
        free(param_names[i]);
        free(param_types[i]);
      }
      free(param_names);
      free(param_types);
      parser_free_type_param_list(func_type_params, func_type_param_traits,
                                  func_type_param_count);
      free(func_name);
      free(link_name);
      return NULL;
    }
  }

  if (parser->current_token.type == TOKEN_WHERE &&
      !parser_parse_where_clause(parser, func_type_params,
                                 func_type_param_traits,
                                 func_type_param_count)) {
    for (size_t i = 0; i < param_count; i++) {
      free(param_names[i]);
      free(param_types[i]);
    }
    free(param_names);
    free(param_types);
    parser_free_type_param_list(func_type_params, func_type_param_traits,
                                func_type_param_count);
    free(func_name);
    free(return_type);
    free(link_name);
    return NULL;
  }

  if (parser->current_token.type == TOKEN_EQUALS) {
    parser_advance(parser); // consume '='
    if (parser->current_token.type != TOKEN_STRING) {
      parser_set_error(parser, "Expected string literal link name after '='");
      for (size_t i = 0; i < param_count; i++) {
        free(param_names[i]);
        free(param_types[i]);
      }
      free(param_names);
      free(param_types);
      parser_free_type_param_list(func_type_params, func_type_param_traits,
                                  func_type_param_count);
      free(func_name);
      free(return_type);
      return NULL;
    }
    link_name = strdup(parser->current_token.value);
    parser_advance(parser);
    if (!link_name) {
      parser_set_error(parser, "Memory allocation failed for link name");
      for (size_t i = 0; i < param_count; i++) {
        free(param_names[i]);
        free(param_types[i]);
      }
      free(param_names);
      free(param_types);
      free(func_name);
      free(return_type);
      return NULL;
    }
  }

  // Parse function body (block) or allow forward declaration terminator
  ASTNode *body = NULL;
  if (parser->current_token.type == TOKEN_LBRACE) {
    body = parser_parse_block(parser);
    if (!body && parser->has_error) {
      // Clean up
      for (size_t i = 0; i < param_count; i++) {
        free(param_names[i]);
        free(param_types[i]);
      }
      free(param_names);
      free(param_types);
      parser_free_type_param_list(func_type_params, func_type_param_traits,
                                  func_type_param_count);
      free(func_name);
      free(return_type);
      free(link_name);
      return NULL;
    }
  } else if (parser->current_token.type == TOKEN_SEMICOLON ||
             parser->current_token.type == TOKEN_NEWLINE) {
    parser_expect_statement_end(parser);
  } else {
    parser_set_error(parser,
                     "Expected function body ('{') or declaration terminator");
    // Clean up
    for (size_t i = 0; i < param_count; i++) {
      free(param_names[i]);
      free(param_types[i]);
    }
    free(param_names);
    free(param_types);
    parser_free_type_param_list(func_type_params, func_type_param_traits,
                                func_type_param_count);
    free(func_name);
    free(return_type);
    free(link_name);
    return NULL;
  }

  ASTNode *func_decl =
      ast_create_function_declaration(func_name, param_names, param_types,
                                      param_count, return_type, body, location);
  if (func_decl && func_decl->data) {
    FunctionDeclaration *func_data = (FunctionDeclaration *)func_decl->data;
    if (link_name) {
      func_data->link_name = strdup(link_name);
      if (!func_data->link_name) {
        ast_destroy_node(func_decl);
        func_decl = NULL;
        parser_set_error(parser, "Memory allocation failed for link name");
      }
    }
  }
  if (func_decl && func_decl->data && func_type_param_count > 0) {
    FunctionDeclaration *fd = (FunctionDeclaration *)func_decl->data;
    fd->type_params = malloc(func_type_param_count * sizeof(char *));
    fd->type_param_traits = malloc(func_type_param_count * sizeof(char *));
    fd->type_param_count = func_type_param_count;
    for (size_t i = 0; i < func_type_param_count; i++) {
      fd->type_params[i] = (char *)string_intern(func_type_params[i]);
      fd->type_param_traits[i] = func_type_param_traits[i]
                                     ? (char *)string_intern(
                                           func_type_param_traits[i])
                                     : NULL;
    }
  }
  parser_free_type_param_list(func_type_params, func_type_param_traits,
                              func_type_param_count);

  // Clean up temporary strings
  free(func_name);
  free(return_type);
  free(link_name);
  for (size_t i = 0; i < param_count; i++) {
    free(param_names[i]);
    free(param_types[i]);
  }
  free(param_names);
  free(param_types);

  return func_decl;
}

ASTNode *parser_parse_trait_declaration(Parser *parser) {
  if (!parser) {
    return NULL;
  }

  SourceLocation location = parser_current_location(parser);
  if (!parser_expect(parser, TOKEN_TRAIT)) {
    return NULL;
  }

  if (!parser_is_identifier_like(parser->current_token.type)) {
    parser_set_error(parser, "Expected trait name after 'trait'");
    return NULL;
  }

  char *trait_name = strdup(parser->current_token.value);
  ASTNode *trait_decl = NULL;
  ASTNode **methods = NULL;
  size_t method_count = 0;
  parser_advance(parser);

  if (!trait_name) {
    return NULL;
  }

  if (parser->current_token.type == TOKEN_LBRACE) {
    parser_advance(parser);
    while (parser->current_token.type != TOKEN_RBRACE &&
           parser->current_token.type != TOKEN_EOF && !parser->has_error) {
      if (parser->current_token.type == TOKEN_NEWLINE ||
          parser->current_token.type == TOKEN_SEMICOLON) {
        parser_advance(parser);
        continue;
      }
      if (parser->current_token.type != TOKEN_FUNCTION &&
          parser->current_token.type != TOKEN_FN) {
        parser_set_error(parser,
                         "Expected function signature in trait declaration");
        goto trait_cleanup;
      }
      ASTNode *method = parser_parse_function_declaration(parser);
      if (!method) {
        goto trait_cleanup;
      }
      FunctionDeclaration *method_decl = (FunctionDeclaration *)method->data;
      if (method_decl && method_decl->body) {
        parser_set_error(parser, "Trait method declarations must not have a body");
        ast_destroy_node(method);
        goto trait_cleanup;
      }
      ASTNode **grown = realloc(methods, (method_count + 1) * sizeof(ASTNode *));
      if (!grown) {
        parser_set_error(parser, "Out of memory while parsing trait methods");
        ast_destroy_node(method);
        goto trait_cleanup;
      }
      methods = grown;
      methods[method_count++] = method;
    }
    if (!parser_expect(parser, TOKEN_RBRACE)) {
      goto trait_cleanup;
    }
  } else if (!parser_expect_statement_end(parser)) {
    goto trait_cleanup;
  }

  trait_decl = ast_create_trait_declaration(trait_name, location);
  if (trait_decl && trait_decl->data && method_count > 0) {
    TraitDeclaration *data = (TraitDeclaration *)trait_decl->data;
    data->methods = malloc(method_count * sizeof(ASTNode *));
    if (!data->methods) {
      parser_set_error(parser, "Out of memory while parsing trait methods");
      ast_destroy_node(trait_decl);
      trait_decl = NULL;
      goto trait_cleanup;
    }
    data->method_count = method_count;
    for (size_t i = 0; i < method_count; i++) {
      data->methods[i] = methods[i];
      ast_add_child(trait_decl, methods[i]);
      methods[i] = NULL;
    }
  }

trait_cleanup:
  for (size_t i = 0; i < method_count; i++) {
    if (methods[i]) {
      ast_destroy_node(methods[i]);
    }
  }
  free(methods);
  free(trait_name);
  return trait_decl;
}

ASTNode *parser_parse_impl_declaration(Parser *parser) {
  if (!parser) {
    return NULL;
  }

  SourceLocation location = parser_current_location(parser);
  char *trait_name = NULL;
  char *for_type_name = NULL;
  ASTNode *impl_decl = NULL;
  ASTNode **methods = NULL;
  size_t method_count = 0;

  if (!parser_expect(parser, TOKEN_IMPL)) {
    return NULL;
  }

  trait_name =
      parser_parse_qualified_name(parser, "Expected trait name after 'impl'");
  if (!trait_name) {
    return NULL;
  }

  if (!parser_expect(parser, TOKEN_FOR)) {
    free(trait_name);
    parser_set_error(parser, "Expected 'for' after trait name in impl");
    return NULL;
  }

  for_type_name = parser_parse_type_annotation(parser);
  if (!for_type_name) {
    if (!parser->has_error) {
      parser_set_error(parser, "Expected type name after 'for' in impl");
    }
    free(trait_name);
    return NULL;
  }

  if (parser->current_token.type == TOKEN_LBRACE) {
    parser_advance(parser);
    while (parser->current_token.type != TOKEN_RBRACE &&
           parser->current_token.type != TOKEN_EOF && !parser->has_error) {
      if (parser->current_token.type == TOKEN_NEWLINE ||
          parser->current_token.type == TOKEN_SEMICOLON) {
        parser_advance(parser);
        continue;
      }
      if (parser->current_token.type != TOKEN_FUNCTION &&
          parser->current_token.type != TOKEN_FN) {
        parser_set_error(parser, "Expected function declaration in impl block");
        goto impl_cleanup;
      }
      ASTNode *method = parser_parse_function_declaration(parser);
      if (!method) {
        goto impl_cleanup;
      }
      FunctionDeclaration *method_decl = (FunctionDeclaration *)method->data;
      if (method_decl && !method_decl->body) {
        parser_set_error(parser, "Impl method declarations must have a body");
        ast_destroy_node(method);
        goto impl_cleanup;
      }
      ASTNode **grown = realloc(methods, (method_count + 1) * sizeof(ASTNode *));
      if (!grown) {
        parser_set_error(parser, "Out of memory while parsing impl methods");
        ast_destroy_node(method);
        goto impl_cleanup;
      }
      methods = grown;
      methods[method_count++] = method;
    }
    if (!parser_expect(parser, TOKEN_RBRACE)) {
      goto impl_cleanup;
    }
  } else if (!parser_expect_statement_end(parser)) {
    goto impl_cleanup;
  }

  impl_decl = ast_create_impl_declaration(trait_name, for_type_name, location);
  if (impl_decl && impl_decl->data && method_count > 0) {
    ImplDeclaration *data = (ImplDeclaration *)impl_decl->data;
    data->methods = malloc(method_count * sizeof(ASTNode *));
    if (!data->methods) {
      parser_set_error(parser, "Out of memory while parsing impl methods");
      ast_destroy_node(impl_decl);
      impl_decl = NULL;
      goto impl_cleanup;
    }
    data->method_count = method_count;
    for (size_t i = 0; i < method_count; i++) {
      data->methods[i] = methods[i];
      ast_add_child(impl_decl, methods[i]);
      methods[i] = NULL;
    }
  }

impl_cleanup:
  for (size_t i = 0; i < method_count; i++) {
    if (methods[i]) {
      ast_destroy_node(methods[i]);
    }
  }
  free(methods);
  free(trait_name);
  free(for_type_name);
  return impl_decl;
}

ASTNode *parser_parse_enum_declaration(Parser *parser) {
  if (!parser)
    return NULL;

  SourceLocation location = parser_current_location(parser);
  if (!parser_expect(parser, TOKEN_ENUM))
    return NULL;

  if (!parser_is_identifier_like(parser->current_token.type)) {
    parser_set_error(parser, "Expected enum name after 'enum'");
    return NULL;
  }
  char *enum_name = strdup(parser->current_token.value);
  parser_advance(parser);

  // Optional generic type parameters: enum Option<T> { ... }
  char **type_params = NULL;
  size_t type_param_count = 0;
  if (parser->current_token.type == TOKEN_LESS_THAN) {
    parser_advance(parser); // consume '<'
    while (parser->current_token.type != TOKEN_GREATER_THAN &&
           parser->current_token.type != TOKEN_EOF && !parser->has_error) {
      if (parser->current_token.type == TOKEN_COMMA ||
          parser->current_token.type == TOKEN_NEWLINE) {
        parser_advance(parser);
        continue;
      }
      if (!parser_is_identifier_like(parser->current_token.type) &&
          !parser_is_type_keyword(parser->current_token.type)) {
        parser_set_error(parser, "Expected type parameter name");
        break;
      }
      char **new_tp =
          realloc(type_params, (type_param_count + 1) * sizeof(char *));
      if (!new_tp) {
        parser_set_error(parser, "Out of memory in enum type parameters");
        break;
      }
      type_params = new_tp;
      type_params[type_param_count++] = strdup(parser->current_token.value);
      parser_advance(parser);
    }
    if (!parser->has_error && !parser_expect(parser, TOKEN_GREATER_THAN)) {
      for (size_t i = 0; i < type_param_count; i++) free(type_params[i]);
      free(type_params);
      free(enum_name);
      return NULL;
    }
  }

  if (!parser_expect(parser, TOKEN_LBRACE)) {
    for (size_t i = 0; i < type_param_count; i++) free(type_params[i]);
    free(type_params);
    free(enum_name);
    return NULL;
  }

  EnumVariant *variants = NULL;
  size_t variant_count = 0;

  while (parser->current_token.type != TOKEN_RBRACE &&
         parser->current_token.type != TOKEN_EOF && !parser->has_error) {
    if (parser->current_token.type == TOKEN_NEWLINE ||
        parser->current_token.type == TOKEN_COMMA) {
      parser_advance(parser);
      continue;
    }

    if (!parser_is_identifier_like(parser->current_token.type)) {
      parser_set_error(parser, "Expected enum variant name");
      break;
    }
    char *variant_name = strdup(parser->current_token.value);
    parser_advance(parser);

    // Optional payload type: Some(T)
    char *payload_type = NULL;
    if (parser->current_token.type == TOKEN_LPAREN) {
      parser_advance(parser); // consume '('
      if (!parser_is_identifier_like(parser->current_token.type) &&
          !parser_is_type_keyword(parser->current_token.type)) {
        parser_set_error(parser, "Expected payload type in variant");
        free(variant_name);
        break;
      }
      payload_type = parser_parse_type_annotation(parser);
      if (!payload_type) {
        parser_set_error(parser, "Expected payload type in variant");
        free(variant_name);
        break;
      }
      if (!parser_expect(parser, TOKEN_RPAREN)) {
        free(payload_type);
        free(variant_name);
        break;
      }
    }

    ASTNode *value = NULL;
    if (parser->current_token.type == TOKEN_EQUALS) {
      parser_advance(parser);
      value = parser_parse_expression(parser);
      if (!value) {
        if (!parser->has_error)
          parser_set_error(parser, "Expected expression after '='");
        free(payload_type);
        free(variant_name);
        break;
      }
    }

    EnumVariant *new_v =
        realloc(variants, (variant_count + 1) * sizeof(EnumVariant));
    if (!new_v) {
      parser_set_error(parser, "Out of memory");
      free(payload_type);
      free(variant_name);
      break;
    }
    variants = new_v;
    variants[variant_count].name = variant_name;
    variants[variant_count].payload_type = payload_type;
    variants[variant_count].value = value;
    variant_count++;

    if (parser->current_token.type == TOKEN_COMMA ||
        parser->current_token.type == TOKEN_NEWLINE) {
      parser_advance(parser);
    }
  }

  if (parser->has_error) {
    for (size_t i = 0; i < variant_count; i++) {
      free(variants[i].name);
      free(variants[i].payload_type);
      ast_destroy_node(variants[i].value);
    }
    free(variants);
    for (size_t i = 0; i < type_param_count; i++) free(type_params[i]);
    free(type_params);
    free(enum_name);
    return NULL;
  }

  if (!parser_expect(parser, TOKEN_RBRACE)) {
    for (size_t i = 0; i < variant_count; i++) {
      free(variants[i].name);
      free(variants[i].payload_type);
      ast_destroy_node(variants[i].value);
    }
    free(variants);
    for (size_t i = 0; i < type_param_count; i++) free(type_params[i]);
    free(type_params);
    free(enum_name);
    return NULL;
  }

  ASTNode *node =
      ast_create_enum_declaration(enum_name, variants, variant_count, location);

  if (node) {
    EnumDeclaration *decl = (EnumDeclaration *)node->data;
    if (decl && type_param_count > 0) {
      decl->type_params = type_params;
      decl->type_param_count = type_param_count;
      type_params = NULL; // ownership transferred
      type_param_count = 0;
    }
  }

  free(enum_name);
  for (size_t i = 0; i < variant_count; i++) {
    free(variants[i].name);
    free(variants[i].payload_type);
  }
  free(variants);
  for (size_t i = 0; i < type_param_count; i++) free(type_params[i]);
  free(type_params);

  return node;
}

ASTNode *parser_parse_struct_declaration(Parser *parser) {
  if (!parser)
    return NULL;

  SourceLocation location = parser_current_location(parser);
  // Expect 'struct' keyword
  if (!parser_expect(parser, TOKEN_STRUCT)) {
    return NULL;
  }

  // Expect struct name
  if (!parser_is_identifier_like(parser->current_token.type)) {
    parser_set_error(parser, "Expected struct name after 'struct'");
    return NULL;
  }

  char *struct_name = strdup(parser->current_token.value);
  parser_advance(parser);

  char **type_params = NULL;
  char **type_param_traits = NULL;
  size_t type_param_count = 0;
  if (parser->current_token.type == TOKEN_LESS_THAN) {
    type_params = parser_parse_type_param_list(parser, &type_param_traits,
                                               &type_param_count);
    if (!type_params && parser->has_error) {
      free(struct_name);
      return NULL;
    }
  }

  if (parser->current_token.type == TOKEN_WHERE &&
      !parser_parse_where_clause(parser, type_params, type_param_traits,
                                 type_param_count)) {
    parser_free_type_param_list(type_params, type_param_traits,
                                type_param_count);
    free(struct_name);
    return NULL;
  }

  // Expect '{'
  if (!parser_expect(parser, TOKEN_LBRACE)) {
    parser_free_type_param_list(type_params, type_param_traits,
                                type_param_count);
    free(struct_name);
    return NULL;
  }

  // Parse fields and methods
  char **field_names = NULL;
  char **field_types = NULL;
  size_t field_count = 0;
  ASTNode **methods = NULL;
  size_t method_count = 0;

  while (parser->current_token.type != TOKEN_RBRACE &&
         parser->current_token.type != TOKEN_EOF && !parser->has_error) {

    // Allow blank lines and redundant separators inside struct bodies.
    if (parser->current_token.type == TOKEN_NEWLINE ||
        parser->current_token.type == TOKEN_SEMICOLON) {
      parser_advance(parser);
      continue;
    }

    if (parser->current_token.type == TOKEN_METHOD) {
      // Parse method declaration
      ASTNode *method = parser_parse_method_declaration(parser);
      if (method) {
        methods = realloc(methods, (method_count + 1) * sizeof(ASTNode *));
        methods[method_count] = method;
        method_count++;
      } else if (parser->has_error) {
        // Clean up and return
        for (size_t i = 0; i < field_count; i++) {
          free(field_names[i]);
          free(field_types[i]);
        }
        free(field_names);
        free(field_types);
        for (size_t i = 0; i < method_count; i++) {
          ast_destroy_node(methods[i]);
        }
        free(methods);
        parser_free_type_param_list(type_params, type_param_traits,
                                    type_param_count);
        free(struct_name);
        return NULL;
      }
    } else {
      // Parse field declaration: name: type;
      if (!parser_is_identifier_like(parser->current_token.type)) {
        parser_set_error(parser, "Expected field name or method declaration");
        // Clean up
        for (size_t i = 0; i < field_count; i++) {
          free(field_names[i]);
          free(field_types[i]);
        }
        free(field_names);
        free(field_types);
        for (size_t i = 0; i < method_count; i++) {
          ast_destroy_node(methods[i]);
        }
        free(methods);
        parser_free_type_param_list(type_params, type_param_traits,
                                    type_param_count);
        free(struct_name);
        return NULL;
      }

      // Reallocate field arrays
      field_names = realloc(field_names, (field_count + 1) * sizeof(char *));
      field_types = realloc(field_types, (field_count + 1) * sizeof(char *));

      field_names[field_count] = strdup(parser->current_token.value);
      parser_advance(parser);

      // Expect ':'
      if (!parser_expect(parser, TOKEN_COLON)) {
        // Clean up
        for (size_t i = 0; i <= field_count; i++) {
          free(field_names[i]);
          if (i < field_count)
            free(field_types[i]);
        }
        free(field_names);
        free(field_types);
        for (size_t i = 0; i < method_count; i++) {
          ast_destroy_node(methods[i]);
        }
        free(methods);
        parser_free_type_param_list(type_params, type_param_traits,
                                    type_param_count);
        free(struct_name);
        return NULL;
      }

      // Parse field type
      field_types[field_count] = parser_parse_type_annotation(parser);
      if (!field_types[field_count]) {
        if (!parser->has_error) {
          parser_set_error(parser, "Expected field type");
        }
        // Clean up
        for (size_t i = 0; i <= field_count; i++) {
          free(field_names[i]);
          if (i < field_count)
            free(field_types[i]);
        }
        free(field_names);
        free(field_types);
        for (size_t i = 0; i < method_count; i++) {
          ast_destroy_node(methods[i]);
        }
        free(methods);
        parser_free_type_param_list(type_params, type_param_traits,
                                    type_param_count);
        free(struct_name);
        return NULL;
      }
      field_count++;

      // Expect a newline or semicolon to end the declaration
      parser_expect_statement_end(parser);
    }
  }

  // Expect '}'
  if (!parser_expect(parser, TOKEN_RBRACE)) {
    // Clean up
    for (size_t i = 0; i < field_count; i++) {
      free(field_names[i]);
      free(field_types[i]);
    }
    free(field_names);
    free(field_types);
    for (size_t i = 0; i < method_count; i++) {
      ast_destroy_node(methods[i]);
    }
    free(methods);
    parser_free_type_param_list(type_params, type_param_traits,
                                type_param_count);
    free(struct_name);
    return NULL;
  }

  ASTNode *struct_decl = ast_create_struct_declaration(
      struct_name, field_names, field_types, field_count, methods, method_count,
      location);

  if (struct_decl && struct_decl->data && type_param_count > 0) {
    StructDeclaration *sd = (StructDeclaration *)struct_decl->data;
    sd->type_params = malloc(type_param_count * sizeof(char *));
    sd->type_param_traits = malloc(type_param_count * sizeof(char *));
    sd->type_param_count = type_param_count;
    for (size_t i = 0; i < type_param_count; i++) {
      sd->type_params[i] = (char *)string_intern(type_params[i]);
      sd->type_param_traits[i] =
          type_param_traits[i] ? (char *)string_intern(type_param_traits[i])
                               : NULL;
    }
  }
  parser_free_type_param_list(type_params, type_param_traits, type_param_count);

  free(struct_name);
  for (size_t i = 0; i < field_count; i++) {
    free(field_names[i]);
    free(field_types[i]);
  }
  free(field_names);
  free(field_types);
  // Note: methods array is now owned by the AST node, don't free it

  return struct_decl;
}

ASTNode *parser_parse_inline_asm(Parser *parser) {
  if (!parser)
    return NULL;

  SourceLocation location = parser_current_location(parser);
  // Expect 'asm' keyword
  if (!parser_expect(parser, TOKEN_ASM)) {
    return NULL;
  }

  // Expect '{'
  if (!parser_expect(parser, TOKEN_LBRACE)) {
    return NULL;
  }

  // Collect all tokens until we hit the closing '}'
  // We need to preserve the exact assembly code as a string
  char *assembly_code = malloc(1024); // Start with 1KB buffer
  size_t buffer_size = 1024;
  size_t code_length = 0;

  if (!assembly_code) {
    parser_set_error(parser, "Memory allocation failed");
    return NULL;
  }

  assembly_code[0] = '\0';

  size_t last_token_line = 0;

  while (parser->current_token.type != TOKEN_RBRACE &&
         parser->current_token.type != TOKEN_EOF && !parser->has_error) {

    // Add the token value to our assembly code string
    if (parser->current_token.value) {
      size_t token_len = strlen(parser->current_token.value);

      // Check if we need to expand the buffer
      if (code_length + token_len + 2 >=
          buffer_size) { // +2 for separator and null terminator
        buffer_size *= 2;
        assembly_code = realloc(assembly_code, buffer_size);
        if (!assembly_code) {
          parser_set_error(parser, "Memory allocation failed");
          return NULL;
        }
      }

      // Add a separator before the token if we already have content
      if (code_length > 0) {
        if (last_token_line != 0 &&
            parser->current_token.line > last_token_line) {
          assembly_code[code_length] = '\n';
        } else {
          assembly_code[code_length] = ' ';
        }
        code_length++;
      }

      // Copy the token value
      strcpy(assembly_code + code_length, parser->current_token.value);
      code_length += token_len;
    }

    last_token_line = parser->current_token.line;
    parser_advance(parser);
  }

  // Expect '}'
  if (!parser_expect(parser, TOKEN_RBRACE)) {
    free(assembly_code);
    return NULL;
  }

  // Create inline assembly node
  ASTNode *inline_asm = ast_create_inline_asm(assembly_code, location);

  free(assembly_code);
  return inline_asm;
}

ASTNode *parser_parse_return_statement(Parser *parser) {
  if (!parser)
    return NULL;

  SourceLocation location = parser_current_location(parser);
  parser_advance(parser); // consume 'return'

  ASTNode *value = NULL;
  if (parser->current_token.type != TOKEN_SEMICOLON &&
      parser->current_token.type != TOKEN_NEWLINE) {
    value = parser_parse_expression(parser);
  }

  ASTNode *return_stmt = ast_create_node(AST_RETURN_STATEMENT, location);
  if (return_stmt && value) {
    ReturnStatement *ret_data = malloc(sizeof(ReturnStatement));
    ret_data->value = value;
    return_stmt->data = ret_data;
    ast_add_child(return_stmt, value);
  }

  return return_stmt;
}

ASTNode *parser_parse_if_statement(Parser *parser) {
  if (!parser)
    return NULL;

  SourceLocation location = parser_current_location(parser);

  if (!parser_expect(parser, TOKEN_IF)) {
    return NULL;
  }

  if (!parser_expect(parser, TOKEN_LPAREN)) {
    parser_refine_error(parser, "Expected '(' after 'if'");
    return NULL;
  }

  ASTNode *condition = parser_parse_expression(parser);
  if (!condition) {
    return NULL;
  }

  if (!parser_expect(parser, TOKEN_RPAREN)) {
    parser_set_error(parser, "Expected ')' after if condition");
    ast_destroy_node(condition);
    return NULL;
  }

  ASTNode *then_branch = (parser->current_token.type == TOKEN_LBRACE)
                             ? parser_parse_block(parser)
                             : parser_parse_statement(parser);
  if (!then_branch) {
    ast_destroy_node(condition);
    return NULL;
  }

  while (parser->current_token.type == TOKEN_NEWLINE) {
    parser_advance(parser);
  }

  ElseIfClause *else_ifs = NULL;
  size_t else_if_count = 0;
  ASTNode *else_branch = NULL;

  while (parser->current_token.type == TOKEN_ELSE) {
    if (parser->peek_token.type == TOKEN_IF) {
      parser_advance(parser); // consume 'else'
      parser_advance(parser); // consume 'if'

      if (!parser_expect(parser, TOKEN_LPAREN)) {
        parser_set_error(parser, "Expected '(' after 'else if'");
        goto cleanup;
      }

      ASTNode *elif_cond = parser_parse_expression(parser);
      if (!elif_cond)
        goto cleanup;

      if (!parser_expect(parser, TOKEN_RPAREN)) {
        parser_set_error(parser, "Expected ')' after else if condition");
        ast_destroy_node(elif_cond);
        goto cleanup;
      }

      ASTNode *elif_body = (parser->current_token.type == TOKEN_LBRACE)
                               ? parser_parse_block(parser)
                               : parser_parse_statement(parser);
      if (!elif_body) {
        ast_destroy_node(elif_cond);
        goto cleanup;
      }

      else_ifs = realloc(else_ifs, (else_if_count + 1) * sizeof(ElseIfClause));
      else_ifs[else_if_count].condition = elif_cond;
      else_ifs[else_if_count].body = elif_body;
      else_if_count++;

      while (parser->current_token.type == TOKEN_NEWLINE) {
        parser_advance(parser);
      }
    } else {
      parser_advance(parser); // consume 'else'
      else_branch = (parser->current_token.type == TOKEN_LBRACE)
                        ? parser_parse_block(parser)
                        : parser_parse_statement(parser);
      if (!else_branch)
        goto cleanup;
      break;
    }
  }

  ASTNode *if_node = ast_create_node(AST_IF_STATEMENT, location);
  if (!if_node)
    goto cleanup;

  IfStatement *if_data = malloc(sizeof(IfStatement));
  if (!if_data) {
    ast_destroy_node(if_node);
    goto cleanup;
  }

  if_data->condition = condition;
  if_data->then_branch = then_branch;
  if_data->else_ifs = else_ifs;
  if_data->else_if_count = else_if_count;
  if_data->else_branch = else_branch;
  if_node->data = if_data;

  ast_add_child(if_node, condition);
  ast_add_child(if_node, then_branch);
  for (size_t i = 0; i < else_if_count; i++) {
    ast_add_child(if_node, else_ifs[i].condition);
    ast_add_child(if_node, else_ifs[i].body);
  }
  if (else_branch)
    ast_add_child(if_node, else_branch);

  return if_node;

cleanup:
  ast_destroy_node(condition);
  ast_destroy_node(then_branch);
  for (size_t i = 0; i < else_if_count; i++) {
    ast_destroy_node(else_ifs[i].condition);
    ast_destroy_node(else_ifs[i].body);
  }
  free(else_ifs);
  if (else_branch)
    ast_destroy_node(else_branch);
  return NULL;
}

ASTNode *parser_parse_while_statement(Parser *parser) {
  if (!parser)
    return NULL;

  SourceLocation location = parser_current_location(parser);

  if (!parser_expect(parser, TOKEN_WHILE)) {
    return NULL;
  }

  if (!parser_expect(parser, TOKEN_LPAREN)) {
    parser_refine_error(parser, "Expected '(' after 'while'");
    return NULL;
  }

  ASTNode *condition = parser_parse_expression(parser);
  if (!condition) {
    return NULL;
  }

  if (!parser_expect(parser, TOKEN_RPAREN)) {
    parser_set_error(parser, "Expected ')' after while condition");
    ast_destroy_node(condition);
    return NULL;
  }

  ASTNode *body = (parser->current_token.type == TOKEN_LBRACE)
                      ? parser_parse_block(parser)
                      : parser_parse_statement(parser);
  if (!body) {
    ast_destroy_node(condition);
    return NULL;
  }

  ASTNode *while_node = ast_create_node(AST_WHILE_STATEMENT, location);
  if (!while_node) {
    ast_destroy_node(condition);
    ast_destroy_node(body);
    return NULL;
  }

  WhileStatement *while_data = malloc(sizeof(WhileStatement));
  if (!while_data) {
    ast_destroy_node(while_node);
    ast_destroy_node(condition);
    ast_destroy_node(body);
    return NULL;
  }

  while_data->condition = condition;
  while_data->body = body;
  while_data->label = NULL;
  while_data->simd_mode = SIMD_ATTR_NONE;
  while_node->data = while_data;

  ast_add_child(while_node, condition);
  ast_add_child(while_node, body);

  return while_node;
}

static ASTNode *parser_parse_break_or_continue(Parser *parser,
                                               TokenType token_type) {
  if (!parser)
    return NULL;

  SourceLocation location = parser_current_location(parser);
  if (!parser_expect(parser, token_type)) {
    return NULL;
  }

  char *target_label = NULL;
  if (parser->current_token.type == TOKEN_IDENTIFIER) {
    target_label = strdup(parser->current_token.value);
    parser_advance(parser);
  }

  parser_expect_statement_end(parser);
  ASTNode *node = (token_type == TOKEN_BREAK)
                      ? ast_create_labeled_break_statement(target_label, location)
                      : ast_create_labeled_continue_statement(target_label, location);
  free(target_label);
  return node;
}

ASTNode *parser_parse_break_statement(Parser *parser) {
  return parser_parse_break_or_continue(parser, TOKEN_BREAK);
}

ASTNode *parser_parse_continue_statement(Parser *parser) {
  return parser_parse_break_or_continue(parser, TOKEN_CONTINUE);
}

// Range-based for: `for i [: type] in start ..|..= end { body }`.
// Desugars at parse time into a classic counted ForStatement so every
// downstream stage (type checker, IR lowering, the counted-loop vectorizer)
// sees the exact shape it already handles:
//
//   for i in lo..hi   =>  var i = lo;  i <  hi;  i = i + 1
//   for i in lo..=hi  =>  var i = lo;  i <= hi;  i = i + 1
//
// The `start` bound is evaluated once (the loop initializer); the `end` bound
// is re-evaluated in the condition each iteration, matching Mettle's C-style
// `for`/`while` semantics. Hoist a call-valued bound yourself if that matters.
// `in` is a contextual keyword (a plain identifier elsewhere), so adding this
// form breaks no existing program that uses `in` as a name.
static ASTNode *parser_parse_range_for(Parser *parser, SourceLocation location) {
  if (!parser_is_identifier_like(parser->current_token.type)) {
    parser_set_error(parser, "Expected '(' or a loop variable after 'for'");
    return NULL;
  }
  char *var_name = strdup(parser->current_token.value);
  parser_advance(parser);

  // Optional type annotation: `for i: int64 in ...`. Without one, the variable
  // type is inferred from the start bound (same rule as `var i = <expr>`).
  char *type_name = NULL;
  if (parser->current_token.type == TOKEN_COLON) {
    parser_advance(parser);
    type_name = parser_parse_type_annotation(parser);
    if (!type_name) {
      if (!parser->has_error)
        parser_set_error(parser, "Expected type after ':'");
      free(var_name);
      return NULL;
    }
  }

  // Contextual `in`.
  if (!parser_is_identifier_like(parser->current_token.type) ||
      strcmp(parser->current_token.value, "in") != 0) {
    parser_set_error(parser, "Expected 'in' in range-based for loop");
    free(var_name);
    free(type_name);
    return NULL;
  }
  parser_advance(parser);

  ASTNode *start = parser_parse_expression(parser);
  if (!start) {
    free(var_name);
    free(type_name);
    return NULL;
  }

  if (parser->current_token.type != TOKEN_DOT_DOT) {
    parser_set_error(parser, "Expected '..' or '..=' in range-based for loop");
    free(var_name);
    free(type_name);
    ast_destroy_node(start);
    return NULL;
  }
  parser_advance(parser); // consume '..'
  int inclusive = 0;
  if (parser->current_token.type == TOKEN_EQUALS) {
    inclusive = 1;
    parser_advance(parser); // consume '=' of '..='
  }

  ASTNode *end = parser_parse_expression(parser);
  if (!end) {
    free(var_name);
    free(type_name);
    ast_destroy_node(start);
    return NULL;
  }

  ASTNode *body = (parser->current_token.type == TOKEN_LBRACE)
                      ? parser_parse_block(parser)
                      : parser_parse_statement(parser);
  if (!body) {
    free(var_name);
    free(type_name);
    ast_destroy_node(start);
    ast_destroy_node(end);
    return NULL;
  }

  // initializer: var <name>[: type] = <start>
  ASTNode *initializer =
      ast_create_var_declaration(var_name, type_name, start, location);
  // The loop counter's type is structural (it takes the type of its bound), so
  // it is exempt from the "explicit type required" rule when no `: type` given.
  if (initializer && !type_name) {
    ((VarDeclaration *)initializer->data)->structural_type = 1;
  }
  // condition: <name> <  <end>   (exclusive)
  //            <name> <= <end>   (inclusive)
  ASTNode *condition = ast_create_binary_expression(
      ast_create_identifier(var_name, location), inclusive ? "<=" : "<", end,
      location);
  // increment: <name> = <name> + 1
  ASTNode *step_value = ast_create_binary_expression(
      ast_create_identifier(var_name, location), "+",
      ast_create_number_literal(1, location, 10), location);
  ASTNode *increment = ast_create_assignment(var_name, step_value, location);

  free(var_name);
  free(type_name);

  if (!initializer || !condition || !increment) {
    if (initializer)
      ast_destroy_node(initializer);
    if (condition)
      ast_destroy_node(condition);
    if (increment)
      ast_destroy_node(increment);
    ast_destroy_node(body);
    parser_set_error(parser, "Out of memory desugaring range-based for loop");
    return NULL;
  }

  return ast_create_for_statement(initializer, condition, increment, body,
                                  location);
}

static void parser_block_add(ASTNode *block, ASTNode *stmt) {
  if (!block || !stmt) {
    return;
  }
  Program *bd = (Program *)block->data;
  ASTNode **grown =
      realloc(bd->declarations, (bd->declaration_count + 1) * sizeof(ASTNode *));
  if (!grown) {
    return;
  }
  bd->declarations = grown;
  bd->declarations[bd->declaration_count++] = stmt;
  ast_add_child(block, stmt);
}

// GPU kernel launch: `dispatch K[grid, block](a0, a1, ...)`.
// Desugars (launch-only) at parse time into a block that marshals the args into
// a CUDA-style void** params array and calls std/gpu's gpu_launch:
//   { var __a0 = a0; ...; var __p: int64[N];
//     __p[0] = (int64)&__a0; ...;
//     gpu_launch(K, grid, block, &__p[0], N); }
// Each arg lives in a naturally-typed (inferred) local, so &__ai yields a
// correctly-typed pointer and the kernel reads the right bytes -- no manual
// per-arg type juggling. Device memory stays explicit (the user's call).
static ASTNode *parser_parse_dispatch_statement(Parser *parser) {
  SourceLocation loc = parser_current_location(parser);
  parser_advance(parser); // consume 'dispatch'

  if (parser->current_token.type != TOKEN_IDENTIFIER) {
    parser_set_error(parser,
                     "Expected a kernel handle (identifier) after 'dispatch'");
    return NULL;
  }
  char *kernel_name = strdup(parser->current_token.value);
  parser_advance(parser);

  ASTNode *grid = NULL;
  ASTNode *block = NULL;
  ASTNode *args[64];
  size_t nargs = 0;

#define DISP_FAIL()                                                            \
  do {                                                                         \
    free(kernel_name);                                                         \
    if (grid)                                                                  \
      ast_destroy_node(grid);                                                  \
    if (block)                                                                 \
      ast_destroy_node(block);                                                 \
    for (size_t _i = 0; _i < nargs; _i++)                                      \
      ast_destroy_node(args[_i]);                                              \
    return NULL;                                                               \
  } while (0)

  // [ grid , block ]
  if (!parser_expect(parser, TOKEN_LBRACKET)) {
    parser_set_error(parser, "Expected '[grid, block]' after the dispatch kernel");
    DISP_FAIL();
  }
  grid = parser_parse_expression(parser);
  if (!grid || !parser_expect(parser, TOKEN_COMMA)) {
    if (grid && !parser->has_error)
      parser_set_error(parser, "Expected ',' between grid and block in dispatch");
    DISP_FAIL();
  }
  block = parser_parse_expression(parser);
  if (!block || !parser_expect(parser, TOKEN_RBRACKET)) {
    DISP_FAIL();
  }

  // ( args )
  if (!parser_expect(parser, TOKEN_LPAREN)) {
    parser_set_error(parser, "Expected '(' before dispatch arguments");
    DISP_FAIL();
  }
  if (parser->current_token.type != TOKEN_RPAREN) {
    while (1) {
      if (nargs >= 64) {
        parser_set_error(parser, "too many dispatch arguments (max 64)");
        DISP_FAIL();
      }
      ASTNode *a = parser_parse_expression(parser);
      if (!a) {
        DISP_FAIL();
      }
      args[nargs++] = a;
      if (parser->current_token.type == TOKEN_COMMA) {
        parser_advance(parser);
        continue;
      }
      break;
    }
  }
  if (!parser_expect(parser, TOKEN_RPAREN)) {
    DISP_FAIL();
  }
#undef DISP_FAIL

  int uid = parser->dispatch_counter++;
  char pname[32];
  snprintf(pname, sizeof(pname), "__mdsp%d_p", uid);

  ASTNode *blk = ast_create_program();
  if (!blk) {
    goto oom;
  }

  // var __mdsp<uid>_a<i> = arg_i;   (type is structural: it is the arg's type)
  for (size_t i = 0; i < nargs; i++) {
    char an[32];
    snprintf(an, sizeof(an), "__mdsp%d_a%zu", uid, i);
    ASTNode *arg_decl = ast_create_var_declaration(an, NULL, args[i], loc);
    if (arg_decl && arg_decl->data) {
      ((VarDeclaration *)arg_decl->data)->structural_type = 1;
    }
    parser_block_add(blk, arg_decl);
    args[i] = NULL; // ownership moved into the var decl
  }

  if (nargs > 0) {
    // var __mdsp<uid>_p: int64[N];
    char ptype[24];
    snprintf(ptype, sizeof(ptype), "int64[%zu]", nargs);
    parser_block_add(blk, ast_create_var_declaration(pname, ptype, NULL, loc));
    // __mdsp<uid>_p[i] = (int64)&__mdsp<uid>_a<i>;
    for (size_t i = 0; i < nargs; i++) {
      char an[32];
      snprintf(an, sizeof(an), "__mdsp%d_a%zu", uid, i);
      ASTNode *target = ast_create_array_index_expression(
          ast_create_identifier(pname, loc), ast_create_number_literal((long long)i, loc, 10), loc);
      ASTNode *addr =
          ast_create_unary_expression("&", ast_create_identifier(an, loc), loc);
      ASTNode *castv = ast_create_cast_expression("int64", addr, loc);
      parser_block_add(blk, ast_create_field_assignment(target, castv, loc));
    }
  }

  // gpu_launch(K, grid, block, &__p[0] | (int64*)0, N);
  ASTNode *params_ptr;
  if (nargs > 0) {
    params_ptr = ast_create_unary_expression(
        "&",
        ast_create_array_index_expression(
            ast_create_identifier(pname, loc),
            ast_create_number_literal(0, loc, 10), loc),
        loc);
  } else {
    params_ptr = ast_create_cast_expression(
        "int64*", ast_create_number_literal(0, loc, 10), loc);
  }
  ASTNode *call_args[5];
  call_args[0] = ast_create_identifier(kernel_name, loc);
  call_args[1] = grid;
  call_args[2] = block;
  call_args[3] = params_ptr;
  call_args[4] = ast_create_number_literal((long long)nargs, loc, 10);
  parser_block_add(blk,
                   ast_create_call_expression("gpu_launch", call_args, 5, loc));

  free(kernel_name);
  return blk;

oom:
  free(kernel_name);
  if (grid)
    ast_destroy_node(grid);
  if (block)
    ast_destroy_node(block);
  for (size_t i = 0; i < nargs; i++)
    if (args[i])
      ast_destroy_node(args[i]);
  parser_set_error(parser, "Out of memory desugaring dispatch");
  return NULL;
}

ASTNode *parser_parse_for_statement(Parser *parser) {
  if (!parser)
    return NULL;

  SourceLocation location = parser_current_location(parser);
  if (!parser_expect(parser, TOKEN_FOR)) {
    return NULL;
  }

  // The classic C-style form always opens with '('. Anything else starts a
  // range-based loop: `for i in lo..hi { ... }`.
  if (parser->current_token.type != TOKEN_LPAREN) {
    return parser_parse_range_for(parser, location);
  }

  if (!parser_expect(parser, TOKEN_LPAREN)) {
    parser_set_error(parser, "Expected '(' after 'for'");
    return NULL;
  }

  ASTNode *initializer = parser_parse_for_initializer(parser);
  if (parser->has_error) {
    if (initializer)
      ast_destroy_node(initializer);
    return NULL;
  }

  if (parser->current_token.type == TOKEN_SEMICOLON) {
    parser_advance(parser);
  } else if (!initializer || initializer->type != AST_VAR_DECLARATION) {
    parser_set_error(parser, "Expected ';' after for-loop initializer");
    if (initializer)
      ast_destroy_node(initializer);
    return NULL;
  }

  ASTNode *condition = NULL;
  if (parser->current_token.type != TOKEN_SEMICOLON) {
    condition = parser_parse_expression(parser);
    if (!condition) {
      if (initializer)
        ast_destroy_node(initializer);
      return NULL;
    }
  }

  if (!parser_expect(parser, TOKEN_SEMICOLON)) {
    if (initializer)
      ast_destroy_node(initializer);
    if (condition)
      ast_destroy_node(condition);
    return NULL;
  }

  ASTNode *increment = NULL;
  if (parser->current_token.type != TOKEN_RPAREN) {
    ASTNode *expr = parser_parse_expression(parser);
    if (!expr) {
      if (initializer)
        ast_destroy_node(initializer);
      if (condition)
        ast_destroy_node(condition);
      return NULL;
    }

    if (parser_is_assignment_token(parser->current_token.type)) {
      increment = parser_parse_assignment_from_target(parser, expr);
      if (!increment) {
        if (initializer)
          ast_destroy_node(initializer);
        if (condition)
          ast_destroy_node(condition);
        return NULL;
      }
    } else {
      increment = expr;
    }
  }

  if (!parser_expect(parser, TOKEN_RPAREN)) {
    if (initializer)
      ast_destroy_node(initializer);
    if (condition)
      ast_destroy_node(condition);
    if (increment)
      ast_destroy_node(increment);
    return NULL;
  }

  ASTNode *body = (parser->current_token.type == TOKEN_LBRACE)
                      ? parser_parse_block(parser)
                      : parser_parse_statement(parser);
  if (!body) {
    if (initializer)
      ast_destroy_node(initializer);
    if (condition)
      ast_destroy_node(condition);
    if (increment)
      ast_destroy_node(increment);
    return NULL;
  }

  return ast_create_for_statement(initializer, condition, increment, body,
                                  location);
}

ASTNode *parser_parse_switch_statement(Parser *parser) {
  if (!parser)
    return NULL;

  SourceLocation location = parser_current_location(parser);
  if (!parser_expect(parser, TOKEN_SWITCH)) {
    return NULL;
  }

  if (!parser_expect(parser, TOKEN_LPAREN)) {
    parser_set_error(parser, "Expected '(' after 'switch'");
    return NULL;
  }

  ASTNode *expression = parser_parse_expression(parser);
  if (!expression) {
    return NULL;
  }

  if (!parser_expect(parser, TOKEN_RPAREN)) {
    ast_destroy_node(expression);
    return NULL;
  }

  if (!parser_expect(parser, TOKEN_LBRACE)) {
    ast_destroy_node(expression);
    return NULL;
  }

  ASTNode **cases = NULL;
  size_t case_count = 0;
  int seen_default = 0;

  while (parser->current_token.type != TOKEN_RBRACE &&
         parser->current_token.type != TOKEN_EOF) {
    while (parser->current_token.type == TOKEN_NEWLINE ||
           parser->current_token.type == TOKEN_SEMICOLON) {
      parser_advance(parser);
    }

    if (parser->current_token.type == TOKEN_RBRACE) {
      break;
    }

    int is_default = 0;
    ASTNode *case_value = NULL;
    ASTNode *case_high = NULL;
    SourceLocation case_loc = parser_current_location(parser);

    if (parser->current_token.type == TOKEN_CASE) {
      parser_advance(parser);
      case_value = parser_parse_expression(parser);
      if (!case_value) {
        parser_set_error(parser, "Expected constant expression after 'case'");
        break;
      }
      // Range case: `case lo..hi:` matches any value in [lo, hi].
      if (parser->current_token.type == TOKEN_DOT_DOT) {
        parser_advance(parser);
        case_high = parser_parse_expression(parser);
        if (!case_high) {
          parser_set_error(parser,
                           "Expected upper bound expression after '..'");
          ast_destroy_node(case_value);
          break;
        }
      }
    } else if (parser->current_token.type == TOKEN_DEFAULT) {
      if (seen_default) {
        parser_set_error(parser, "Only one default case is allowed");
        break;
      }
      seen_default = 1;
      is_default = 1;
      parser_advance(parser);
    } else {
      parser_set_error(parser, "Expected 'case' or 'default' in switch");
      break;
    }

    if (!parser_expect(parser, TOKEN_COLON)) {
      if (case_value)
        ast_destroy_node(case_value);
      if (case_high)
        ast_destroy_node(case_high);
      break;
    }

    ASTNode *case_body = ast_create_program();
    if (!case_body) {
      if (case_value)
        ast_destroy_node(case_value);
      if (case_high)
        ast_destroy_node(case_high);
      parser_set_error(parser, "Memory allocation failed for switch case");
      break;
    }

    Program *body_prog = (Program *)case_body->data;
    while (parser->current_token.type != TOKEN_EOF &&
           parser->current_token.type != TOKEN_CASE &&
           parser->current_token.type != TOKEN_DEFAULT &&
           parser->current_token.type != TOKEN_RBRACE) {
      if (parser->current_token.type == TOKEN_NEWLINE ||
          parser->current_token.type == TOKEN_SEMICOLON) {
        parser_advance(parser);
        continue;
      }

      ASTNode *stmt = parser_parse_statement(parser);
      if (!stmt) {
        ast_destroy_node(case_body);
        if (case_value)
          ast_destroy_node(case_value);
        if (case_high)
          ast_destroy_node(case_high);
        for (size_t i = 0; i < case_count; i++)
          ast_destroy_node(cases[i]);
        free(cases);
        ast_destroy_node(expression);
        return NULL;
      }

      body_prog->declarations =
          realloc(body_prog->declarations,
                  (body_prog->declaration_count + 1) * sizeof(ASTNode *));
      body_prog->declarations[body_prog->declaration_count++] = stmt;
      ast_add_child(case_body, stmt);
    }

    ASTNode *case_node =
        ast_create_case_clause(case_value, case_body, is_default, case_loc);
    if (!case_node) {
      ast_destroy_node(case_body);
      if (case_value)
        ast_destroy_node(case_value);
      if (case_high)
        ast_destroy_node(case_high);
      parser_set_error(parser, "Failed to create switch case clause");
      break;
    }

    if (case_high) {
      ((CaseClause *)case_node->data)->value_high = case_high;
      ast_add_child(case_node, case_high);
    }

    cases = realloc(cases, (case_count + 1) * sizeof(ASTNode *));
    cases[case_count++] = case_node;
  }

  if (!parser_expect(parser, TOKEN_RBRACE)) {
    for (size_t i = 0; i < case_count; i++)
      ast_destroy_node(cases[i]);
    free(cases);
    ast_destroy_node(expression);
    return NULL;
  }

  ASTNode *switch_node =
      ast_create_switch_statement(expression, cases, case_count, location);
  free(cases);
  return switch_node;
}

// Parse: match (expr) {
//   case VariantName(binding): { body }   (statement form; body is a block)
//   case VariantName: { body }
//   default: { body }
// }
// In expression form (is_expression=1) each arm body is a value-yielding
// expression instead of a block:
//   match (expr) { case Some(v): v + 1, default: 0 }
static ASTNode *parser_parse_match_core(Parser *parser, int is_expression) {
  if (!parser)
    return NULL;

  SourceLocation location = parser_current_location(parser);
  if (!parser_expect(parser, TOKEN_MATCH))
    return NULL;

  if (!parser_expect(parser, TOKEN_LPAREN)) {
    parser_refine_error(parser, "Expected '(' after 'match'");
    return NULL;
  }
  ASTNode *expr = parser_parse_expression(parser);
  if (!expr)
    return NULL;
  if (!parser_expect(parser, TOKEN_RPAREN)) {
    ast_destroy_node(expr);
    return NULL;
  }
  if (!parser_expect(parser, TOKEN_LBRACE)) {
    ast_destroy_node(expr);
    return NULL;
  }

  MatchArm *arms = NULL;
  size_t arm_count = 0;
  int seen_default = 0;

  while (parser->current_token.type != TOKEN_RBRACE &&
         parser->current_token.type != TOKEN_EOF && !parser->has_error) {
    while (parser->current_token.type == TOKEN_NEWLINE ||
           parser->current_token.type == TOKEN_SEMICOLON)
      parser_advance(parser);
    if (parser->current_token.type == TOKEN_RBRACE)
      break;

    MatchArm arm = {0};

    if (parser->current_token.type == TOKEN_DEFAULT) {
      if (seen_default) {
        parser_set_error(parser, "Only one default arm is allowed in match");
        break;
      }
      seen_default = 1;
      arm.is_default = 1;
      arm.variant_name = strdup("default");
      parser_advance(parser);
    } else if (parser->current_token.type == TOKEN_CASE) {
      parser_advance(parser); // consume 'case'
      if (!parser_is_identifier_like(parser->current_token.type)) {
        parser_set_error(parser, "Expected variant name after 'case' in match");
        break;
      }
      arm.variant_name = strdup(parser->current_token.value);
      parser_advance(parser);

      // Optional binding: case Some(v)
      if (parser->current_token.type == TOKEN_LPAREN) {
        parser_advance(parser); // consume '('
        if (!parser_is_identifier_like(parser->current_token.type)) {
          parser_set_error(parser, "Expected binding name in match arm");
          free(arm.variant_name);
          break;
        }
        arm.binding_name = strdup(parser->current_token.value);
        parser_advance(parser);
        if (!parser_expect(parser, TOKEN_RPAREN)) {
          free(arm.binding_name);
          free(arm.variant_name);
          break;
        }
      }
    } else {
      parser_set_error(parser, "Expected 'case' or 'default' in match");
      break;
    }

    if (!parser_expect(parser, TOKEN_COLON)) {
      free(arm.binding_name);
      free(arm.variant_name);
      break;
    }

    if (is_expression) {
      arm.body = parser_parse_expression(parser);
      if (!arm.body) {
        if (!parser->has_error)
          parser_set_error(parser,
                           "Expected value expression in match arm");
        free(arm.binding_name);
        free(arm.variant_name);
        break;
      }
    } else {
      arm.body = parser_parse_block(parser);
      if (!arm.body) {
        if (!parser->has_error)
          parser_set_error(parser, "Expected '{' block body in match arm");
        free(arm.binding_name);
        free(arm.variant_name);
        break;
      }
    }

    MatchArm *new_arms = realloc(arms, (arm_count + 1) * sizeof(MatchArm));
    if (!new_arms) {
      parser_set_error(parser, "Out of memory in match statement");
      ast_destroy_node(arm.body);
      free(arm.binding_name);
      free(arm.variant_name);
      break;
    }
    arms = new_arms;
    arms[arm_count++] = arm;

    // Expression-form arms may be separated by a comma.
    if (is_expression && parser->current_token.type == TOKEN_COMMA)
      parser_advance(parser);

    while (parser->current_token.type == TOKEN_NEWLINE ||
           parser->current_token.type == TOKEN_SEMICOLON)
      parser_advance(parser);
  }

  if (parser->has_error) {
    for (size_t i = 0; i < arm_count; i++) {
      free(arms[i].variant_name);
      free(arms[i].binding_name);
      ast_destroy_node(arms[i].body);
    }
    free(arms);
    ast_destroy_node(expr);
    return NULL;
  }

  if (!parser_expect(parser, TOKEN_RBRACE)) {
    for (size_t i = 0; i < arm_count; i++) {
      free(arms[i].variant_name);
      free(arms[i].binding_name);
      ast_destroy_node(arms[i].body);
    }
    free(arms);
    ast_destroy_node(expr);
    return NULL;
  }

  ASTNode *node =
      is_expression
          ? ast_create_match_expression(expr, arms, arm_count, location)
          : ast_create_match_statement(expr, arms, arm_count, location);
  for (size_t i = 0; i < arm_count; i++) {
    free(arms[i].variant_name);
    free(arms[i].binding_name);
    // body nodes now owned by the match node
  }
  free(arms);
  return node;
}

ASTNode *parser_parse_match_statement(Parser *parser) {
  return parser_parse_match_core(parser, 0);
}

ASTNode *parser_parse_match_expression(Parser *parser) {
  return parser_parse_match_core(parser, 1);
}

ASTNode *parser_parse_block(Parser *parser) {
  if (!parser)
    return NULL;

  // Expect '{'
  if (!parser_expect(parser, TOKEN_LBRACE)) {
    return NULL;
  }

  // Create a block node (we'll use a program node to hold statements)
  ASTNode *block = ast_create_program();
  if (!block)
    return NULL;

  Program *block_data = (Program *)block->data;

  // Parse statements until we hit '}'
  while (parser->current_token.type != TOKEN_RBRACE &&
         parser->current_token.type != TOKEN_EOF) {

    if (parser->has_error) {
      break;
    }

    // Skip empty statements/newlines
    if (parser->current_token.type == TOKEN_NEWLINE ||
        parser->current_token.type == TOKEN_SEMICOLON) {
      parser_advance(parser);
      continue;
    }

    ASTNode *stmt = parser_parse_statement(parser);
    if (stmt) {
      // Add to block's statements array
      block_data->declarations =
          realloc(block_data->declarations,
                  (block_data->declaration_count + 1) * sizeof(ASTNode *));
      if (block_data->declarations) {
        block_data->declarations[block_data->declaration_count] = stmt;
        block_data->declaration_count++;
        ast_add_child(block, stmt);
      }

      // Attempt to consume optional statement end (semicolon or newline)
      // for statements that didn't already consume it.
      if (stmt->type != AST_DEFER_STATEMENT &&
          (parser->current_token.type == TOKEN_SEMICOLON ||
           parser->current_token.type == TOKEN_NEWLINE)) {
        parser_expect_statement_end(parser);
      }
    } else if (parser->has_error) {
      break;
    } else {
      parser_set_error(parser, "Failed to parse statement in block");
      break;
    }
  }

  // Expect '}'
  if (!parser_expect(parser, TOKEN_RBRACE)) {
    ast_destroy_node(block);
    return NULL;
  }

  return block;
}

ASTNode *parser_parse_method_declaration(Parser *parser) {
  if (!parser)
    return NULL;

  SourceLocation location = parser_current_location(parser);
  // Expect 'method' keyword
  if (!parser_expect(parser, TOKEN_METHOD)) {
    return NULL;
  }

  // Expect method name
  if (!parser_is_identifier_like(parser->current_token.type)) {
    parser_set_error(parser, "Expected method name after 'method'");
    return NULL;
  }

  char *method_name = strdup(parser->current_token.value);
  parser_advance(parser);

  // Expect '('
  if (!parser_expect(parser, TOKEN_LPAREN)) {
    free(method_name);
    return NULL;
  }

  char **param_names = NULL;
  char **param_types = NULL;
  size_t param_count = 0;

  if (!parser_parse_parameter_list(parser, &param_names, &param_types,
                                   &param_count)) {
    free(method_name);
    return NULL;
  }

  // Expect ')'
  if (!parser_expect(parser, TOKEN_RPAREN)) {
    for (size_t i = 0; i < param_count; i++) {
      free(param_names[i]);
      free(param_types[i]);
    }
    free(param_names);
    free(param_types);
    free(method_name);
    return NULL;
  }

  // Optional return type: '-> type' (or ': type' for compatibility)
  char *return_type = NULL;
  if (parser->current_token.type == TOKEN_ARROW ||
      parser->current_token.type == TOKEN_COLON) {
    parser_advance(parser); // consume return separator

    return_type = parser_parse_type_annotation(parser);
    if (!return_type) {
      if (!parser->has_error) {
        parser_set_error(parser, "Expected return type after return separator");
      }
      for (size_t i = 0; i < param_count; i++) {
        free(param_names[i]);
        free(param_types[i]);
      }
      free(param_names);
      free(param_types);
      free(method_name);
      return NULL;
    }
  }

  // Parse method body (block)
  ASTNode *body = NULL;
  if (parser->current_token.type == TOKEN_LBRACE) {
    body = parser_parse_block(parser);
    if (!body && parser->has_error) {
      // Clean up
      for (size_t i = 0; i < param_count; i++) {
        free(param_names[i]);
        free(param_types[i]);
      }
      free(param_names);
      free(param_types);
      free(method_name);
      free(return_type);
      return NULL;
    }
  } else {
    parser_set_error(parser, "Expected method body ('{')");
    // Clean up
    for (size_t i = 0; i < param_count; i++) {
      free(param_names[i]);
      free(param_types[i]);
    }
    free(param_names);
    free(param_types);
    free(method_name);
    free(return_type);
    return NULL;
  }

  // Create method declaration node (we'll use AST_METHOD_DECLARATION type)
  ASTNode *method_decl = ast_create_node(AST_METHOD_DECLARATION, location);
  if (!method_decl) {
    // Clean up
    for (size_t i = 0; i < param_count; i++) {
      free(param_names[i]);
      free(param_types[i]);
    }
    free(param_names);
    free(param_types);
    free(method_name);
    free(return_type);
    if (body)
      ast_destroy_node(body);
    return NULL;
  }

  // Create method declaration data (reuse FunctionDeclaration structure)
  FunctionDeclaration *method_data = malloc(sizeof(FunctionDeclaration));
  if (!method_data) {
    // Clean up
    for (size_t i = 0; i < param_count; i++) {
      free(param_names[i]);
      free(param_types[i]);
    }
    free(param_names);
    free(param_types);
    free(method_name);
    free(return_type);
    if (body)
      ast_destroy_node(body);
    ast_destroy_node(method_decl);
    return NULL;
  }
  memset(method_data, 0, sizeof(FunctionDeclaration));

  method_data->name = (char *)string_intern(method_name);
  method_data->return_type =
      return_type ? (char *)string_intern(return_type) : NULL;
  method_data->parameter_count = param_count;
  method_data->body = body;

  if (param_count > 0) {
    method_data->parameter_names = malloc(param_count * sizeof(char *));
    method_data->parameter_types = malloc(param_count * sizeof(char *));

    for (size_t i = 0; i < param_count; i++) {
      method_data->parameter_names[i] = (char *)string_intern(param_names[i]);
      method_data->parameter_types[i] = (char *)string_intern(param_types[i]);
    }
  } else {
    method_data->parameter_names = NULL;
    method_data->parameter_types = NULL;
  }

  method_decl->data = method_data;

  if (body) {
    ast_add_child(method_decl, body);
  }

  // Clean up temporary strings
  free(method_name);
  free(return_type);
  for (size_t i = 0; i < param_count; i++) {
    free(param_names[i]);
    free(param_types[i]);
  }
  free(param_names);
  free(param_types);

  return method_decl;
}
