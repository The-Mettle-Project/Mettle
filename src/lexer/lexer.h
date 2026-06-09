#ifndef LEXER_H
#define LEXER_H

#include <stddef.h>

typedef enum {
  TOKEN_EOF,
  TOKEN_IDENTIFIER,
  TOKEN_NUMBER,
  TOKEN_STRING,

  // Enhanced syntax keywords
  TOKEN_IMPORT,
  TOKEN_IMPORT_STR,
  TOKEN_EXTERN,
  TOKEN_EXPORT,
  TOKEN_VAR,
  TOKEN_CONST,
  TOKEN_FUNCTION,
  TOKEN_STRUCT,
  TOKEN_ENUM,
  TOKEN_METHOD,
  TOKEN_RETURN,
  TOKEN_IF,
  TOKEN_ELSE,
  TOKEN_WHILE,
  TOKEN_FOR,
  TOKEN_SWITCH,
  TOKEN_CASE,
  TOKEN_DEFAULT,
  TOKEN_BREAK,
  TOKEN_CONTINUE,
  TOKEN_DEFER,
  TOKEN_ERRDEFER,
  TOKEN_ASM,
  TOKEN_THIS,
  TOKEN_NEW,
  TOKEN_TRAIT,
  TOKEN_IMPL,
  TOKEN_WHERE,
  TOKEN_FN,
  TOKEN_MATCH,
  TOKEN_KERNEL,
  TOKEN_DISPATCH,

  // Type keywords
  TOKEN_INT8,
  TOKEN_INT16,
  TOKEN_INT32,
  TOKEN_INT64,
  TOKEN_UINT8,
  TOKEN_UINT16,
  TOKEN_UINT32,
  TOKEN_UINT64,
  TOKEN_FLOAT32,
  TOKEN_FLOAT64,
  TOKEN_STRING_TYPE,

  // Operators and punctuation
  TOKEN_COLON,
  TOKEN_SEMICOLON,
  TOKEN_COMMA,
  TOKEN_EQUALS,
  TOKEN_PLUS_EQUALS,
  TOKEN_MINUS_EQUALS,
  TOKEN_STAR_EQUALS,
  TOKEN_SLASH_EQUALS,
  TOKEN_PERCENT_EQUALS,
  TOKEN_AMP_EQUALS,
  TOKEN_PIPE_EQUALS,
  TOKEN_CARET_EQUALS,
  TOKEN_LSHIFT_EQUALS,
  TOKEN_RSHIFT_EQUALS,
  TOKEN_EQUALS_EQUALS,
  TOKEN_NOT_EQUALS,
  TOKEN_LESS_THAN,
  TOKEN_GREATER_THAN,
  TOKEN_LESS_EQUALS,
  TOKEN_GREATER_EQUALS,
  TOKEN_ARROW,
  TOKEN_LPAREN,
  TOKEN_RPAREN,
  TOKEN_LBRACE,
  TOKEN_RBRACE,
  TOKEN_LBRACKET,
  TOKEN_RBRACKET,
  TOKEN_PLUS,
  TOKEN_MINUS,
  TOKEN_MULTIPLY,
  TOKEN_AMPERSAND,
  TOKEN_PIPE,
  TOKEN_CARET,
  TOKEN_LSHIFT,
  TOKEN_RSHIFT,
  TOKEN_TILDE,
  TOKEN_AND_AND,
  TOKEN_OR_OR,
  TOKEN_DIVIDE,
  TOKEN_PERCENT,
  TOKEN_NOT,
  TOKEN_DOT,
  TOKEN_DOT_DOT,
  TOKEN_AT,
  TOKEN_NEWLINE,

  // Common x86 mnemonics (for backward compatibility)
  TOKEN_MOV,
  TOKEN_ADD,
  TOKEN_SUB,
  TOKEN_MUL,
  TOKEN_DIV,
  TOKEN_IMUL,
  TOKEN_IDIV,
  TOKEN_INC,
  TOKEN_DEC,
  TOKEN_CMP,
  TOKEN_JMP,
  TOKEN_JE,
  TOKEN_JNE,
  TOKEN_JL,
  TOKEN_JLE,
  TOKEN_JG,
  TOKEN_JGE,
  TOKEN_CALL,
  TOKEN_RET,
  TOKEN_PUSH,
  TOKEN_POP,
  TOKEN_LEA,
  TOKEN_NOP,
  TOKEN_INT,
  TOKEN_SYSCALL,

  // x86 registers
  TOKEN_EAX,
  TOKEN_EBX,
  TOKEN_ECX,
  TOKEN_EDX,
  TOKEN_ESI,
  TOKEN_EDI,
  TOKEN_ESP,
  TOKEN_EBP,
  TOKEN_RAX,
  TOKEN_RBX,
  TOKEN_RCX,
  TOKEN_RDX,
  TOKEN_RSI,
  TOKEN_RDI,
  TOKEN_RSP,
  TOKEN_RBP,
  TOKEN_R8,
  TOKEN_R9,
  TOKEN_R10,
  TOKEN_R11,
  TOKEN_R12,
  TOKEN_R13,
  TOKEN_R14,
  TOKEN_R15,

  TOKEN_ERROR
} TokenType;

typedef struct {
  const char *data;
  size_t length;
} StringView;

typedef struct {
  TokenType type;
  char *value;
  StringView lexeme;
  size_t line;
  size_t column;
  unsigned char is_interned;
} Token;

typedef struct {
  const char *source;
  size_t position;
  size_t line;
  size_t column;
  size_t length;
  char *error_message;
  int has_error;
  size_t continuation_depth;
} Lexer;

// Function declarations
Lexer *lexer_create(const char *source);
void lexer_destroy(Lexer *lexer);
Token lexer_next_token(Lexer *lexer);
Token lexer_peek_token(Lexer *lexer);
void token_destroy(Token *token);
Token token_clone(const Token *token);

// Error reporting functions
void lexer_set_error(Lexer *lexer, const char *message);

#endif // LEXER_H
