#include "lexer.h"
#include "common.h"
#include "../string_intern.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef struct StringInternEntry {
  char *value;
  size_t length;
  struct StringInternEntry *next;
  struct StringInternEntry *ptr_next;
} StringInternEntry;

#define STRING_INTERN_INITIAL_BUCKETS 4096u

/* Both tables index the same entries: g_string_intern_buckets chains by content
 * hash (via entry->next) for interning lookups, g_string_intern_ptr_buckets
 * chains by pointer hash (via entry->ptr_next) for string_is_interned. They
 * share one bucket count and grow together so chains stay short -- with a fixed
 * 4096 buckets, a program with ~200k distinct identifiers gave ~50-long chains,
 * making both interning (parse) and string_is_interned (teardown) O(n^2). */
static StringInternEntry **g_string_intern_buckets = NULL;
static StringInternEntry **g_string_intern_ptr_buckets = NULL;
static size_t g_string_intern_bucket_count = 0;
static size_t g_string_intern_entry_count = 0;

static void token_set_lexeme(Token *token, const char *data, size_t length) {
  if (!token) {
    return;
  }
  token->lexeme.data = data;
  token->lexeme.length = length;
}

/* Assign a fixed operator/punctuation spelling without heap allocation. The
 * value points at a string literal with static lifetime, so it is flagged
 * interned: token_destroy will not free it and token_clone will not deep-copy
 * it. This avoids a malloc+free per operator token, which dominated lexing of
 * punctuation-heavy source. */
static void token_set_static_value(Token *token, const char *literal) {
  if (!token) {
    return;
  }
  token->value = (char *)literal;
  token->is_interned = 1;
}

/* Static, nul-terminated one-character strings for every byte value, so a
 * single-character operator token can borrow a stable spelling instead of
 * allocating a 2-byte buffer per token. Index by (unsigned char)c. */
static const char g_single_char_strings[256][2] = {
#define SCS1(n) {(char)(n), '\0'}
#define SCS4(n) SCS1(n), SCS1((n) + 1), SCS1((n) + 2), SCS1((n) + 3)
#define SCS16(n) SCS4(n), SCS4((n) + 4), SCS4((n) + 8), SCS4((n) + 12)
#define SCS64(n) SCS16(n), SCS16((n) + 16), SCS16((n) + 32), SCS16((n) + 48)
    SCS64(0), SCS64(64), SCS64(128), SCS64(192)
#undef SCS64
#undef SCS16
#undef SCS4
#undef SCS1
};

static void token_set_single_char_value(Token *token, char c) {
  token_set_static_value(token, g_single_char_strings[(unsigned char)c]);
}

static int string_intern_init(void) {
  if (g_string_intern_buckets && g_string_intern_ptr_buckets) {
    return 1;
  }

  g_string_intern_buckets =
      calloc(STRING_INTERN_INITIAL_BUCKETS, sizeof(StringInternEntry *));
  g_string_intern_ptr_buckets =
      calloc(STRING_INTERN_INITIAL_BUCKETS, sizeof(StringInternEntry *));
  if (!g_string_intern_buckets || !g_string_intern_ptr_buckets) {
    free(g_string_intern_buckets);
    free(g_string_intern_ptr_buckets);
    g_string_intern_buckets = NULL;
    g_string_intern_ptr_buckets = NULL;
    return 0;
  }
  g_string_intern_bucket_count = STRING_INTERN_INITIAL_BUCKETS;
  g_string_intern_entry_count = 0;

  return 1;
}

static size_t string_intern_hash_bytes(const char *value, size_t length) {
  size_t hash = (size_t)1469598103934665603ULL;
  for (size_t i = 0; i < length; i++) {
    hash ^= (unsigned char)value[i];
    hash *= (size_t)1099511628211ULL;
  }
  return hash;
}

static size_t string_intern_hash_ptr(const void *ptr) {
  uintptr_t value = (uintptr_t)ptr;
  value ^= value >> 33;
  value *= (uintptr_t)0xff51afd7ed558ccdULL;
  value ^= value >> 33;
  return (size_t)value;
}

/* Double both bucket arrays and rehash every entry into the new buckets. Keeps
 * average chain length ~constant as the number of interned strings grows. On
 * allocation failure the existing (smaller) tables are left intact and lookups
 * remain correct, just slower. */
static void string_intern_maybe_grow(void) {
  if (g_string_intern_entry_count <= (g_string_intern_bucket_count * 3) / 4) {
    return;
  }

  size_t new_count = g_string_intern_bucket_count * 2;
  StringInternEntry **new_content =
      calloc(new_count, sizeof(StringInternEntry *));
  StringInternEntry **new_ptr = calloc(new_count, sizeof(StringInternEntry *));
  if (!new_content || !new_ptr) {
    free(new_content);
    free(new_ptr);
    return; /* keep the old tables; correctness is unaffected */
  }

  /* Rehash via the content-bucket chains, which reach every entry exactly
   * once, fixing up both the content (next) and pointer (ptr_next) links. */
  for (size_t i = 0; i < g_string_intern_bucket_count; i++) {
    StringInternEntry *entry = g_string_intern_buckets[i];
    while (entry) {
      StringInternEntry *next = entry->next;

      size_t cb =
          string_intern_hash_bytes(entry->value, entry->length) & (new_count - 1);
      entry->next = new_content[cb];
      new_content[cb] = entry;

      size_t pb = string_intern_hash_ptr(entry->value) & (new_count - 1);
      entry->ptr_next = new_ptr[pb];
      new_ptr[pb] = entry;

      entry = next;
    }
  }

  free(g_string_intern_buckets);
  free(g_string_intern_ptr_buckets);
  g_string_intern_buckets = new_content;
  g_string_intern_ptr_buckets = new_ptr;
  g_string_intern_bucket_count = new_count;
}

const char *string_intern_n(const char *value, size_t length) {
  if (!value) {
    return NULL;
  }

  if (!string_intern_init()) {
    return NULL;
  }

  size_t hash = string_intern_hash_bytes(value, length);
  size_t bucket = hash & (g_string_intern_bucket_count - 1);
  StringInternEntry *entry = g_string_intern_buckets[bucket];
  while (entry) {
    if (entry->length == length && memcmp(entry->value, value, length) == 0) {
      return entry->value;
    }
    entry = entry->next;
  }

  char *copy = malloc(length + 1);
  if (!copy) {
    return NULL;
  }
  memcpy(copy, value, length);
  copy[length] = '\0';

  entry = malloc(sizeof(StringInternEntry));
  if (!entry) {
    free(copy);
    return NULL;
  }

  entry->value = copy;
  entry->length = length;
  entry->next = g_string_intern_buckets[bucket];
  g_string_intern_buckets[bucket] = entry;

  size_t ptr_bucket =
      string_intern_hash_ptr(copy) & (g_string_intern_bucket_count - 1);
  entry->ptr_next = g_string_intern_ptr_buckets[ptr_bucket];
  g_string_intern_ptr_buckets[ptr_bucket] = entry;

  g_string_intern_entry_count++;
  string_intern_maybe_grow();

  return copy;
}

const char *string_intern(const char *value) {
  if (!value) {
    return NULL;
  }
  return string_intern_n(value, strlen(value));
}

int string_is_interned(const char *value) {
  if (!value || !g_string_intern_ptr_buckets) {
    return 0;
  }

  size_t ptr_bucket =
      string_intern_hash_ptr(value) & (g_string_intern_bucket_count - 1);
  StringInternEntry *entry = g_string_intern_ptr_buckets[ptr_bucket];
  while (entry) {
    if (entry->value == value) {
      return 1;
    }
    entry = entry->ptr_next;
  }

  return 0;
}

void string_intern_clear(void) {
  if (!g_string_intern_buckets) {
    return;
  }

  for (size_t i = 0; i < g_string_intern_bucket_count; i++) {
    StringInternEntry *entry = g_string_intern_buckets[i];
    while (entry) {
      StringInternEntry *next = entry->next;
      free(entry->value);
      free(entry);
      entry = next;
    }
    g_string_intern_buckets[i] = NULL;
  }

  free(g_string_intern_buckets);
  free(g_string_intern_ptr_buckets);
  g_string_intern_buckets = NULL;
  g_string_intern_ptr_buckets = NULL;
  g_string_intern_bucket_count = 0;
  g_string_intern_entry_count = 0;
}

Lexer *lexer_create(const char *source) {
  Lexer *lexer = malloc(sizeof(Lexer));
  if (!lexer)
    return NULL;

  lexer->source = source;
  lexer->position = 0;
  /* Skip a UTF-8 BOM (EF BB BF) at the very start of the input; Windows
     editors (Notepad, PowerShell Set-Content -Encoding utf8) emit one. */
  if ((unsigned char)source[0] == 0xEF && (unsigned char)source[1] == 0xBB &&
      (unsigned char)source[2] == 0xBF) {
    lexer->position = 3;
  }
  lexer->line = 1;
  lexer->column = 1;
  lexer->length = strlen(source);
  lexer->error_message = NULL;
  lexer->has_error = 0;
  lexer->continuation_depth = 0;
  lexer->last_significant = TOKEN_NEWLINE;

  return lexer;
}

void lexer_destroy(Lexer *lexer) {
  if (lexer) {
    if (lexer->error_message) {
      free(lexer->error_message);
    }
    free(lexer);
  }
}

static Token lexer_skip_line_comment(Lexer *lexer) {
  lexer->position += 2;
  lexer->column += 2;
  while (lexer->position < lexer->length &&
         lexer->source[lexer->position] != '\n') {
    lexer->position++;
    lexer->column++;
  }
  return lexer_next_token(lexer);
}

static Token lexer_skip_block_comment(Lexer *lexer) {
  size_t start_line = lexer->line;
  size_t start_column = lexer->column;
  lexer->position += 2;
  lexer->column += 2;
  int depth = 1;
  while (lexer->position < lexer->length && depth > 0) {
    char c = lexer->source[lexer->position];
    if (c == '/' && lexer->position + 1 < lexer->length &&
        lexer->source[lexer->position + 1] == '*') {
      depth++;
      lexer->position += 2;
      lexer->column += 2;
    } else if (c == '*' && lexer->position + 1 < lexer->length &&
               lexer->source[lexer->position + 1] == '/') {
      depth--;
      lexer->position += 2;
      lexer->column += 2;
    } else if (c == '\n') {
      lexer->position++;
      lexer->line++;
      lexer->column = 1;
    } else {
      lexer->position++;
      lexer->column++;
    }
  }
  if (depth != 0) {
    Token token = {TOKEN_ERROR, NULL, {NULL, 0}, start_line, start_column, 0};
    token.value = strdup("Unterminated block comment");
    lexer_set_error(lexer, token.value);
    return token;
  }
  return lexer_next_token(lexer);
}

static Token lexer_lex_char_literal(Lexer *lexer) {
  Token token = {TOKEN_EOF, NULL, {NULL, 0}, lexer->line, lexer->column, 0};
  size_t literal_start = lexer->position;
  lexer->position++;
  lexer->column++;
  if (lexer->position >= lexer->length) {
    token.type = TOKEN_ERROR;
    token.value = strdup("Unterminated character literal");
    lexer_set_error(lexer, token.value);
    return token;
  }

  int value = 0;
  char ch = lexer->source[lexer->position];
  if (ch == '\\') {
    lexer->position++;
    lexer->column++;
    if (lexer->position >= lexer->length) {
      token.type = TOKEN_ERROR;
      token.value = strdup("Unterminated character literal");
      lexer_set_error(lexer, token.value);
      return token;
    }

    char esc = lexer->source[lexer->position];
    switch (esc) {
    case 'n':
      value = '\n';
      break;
    case 't':
      value = '\t';
      break;
    case 'r':
      value = '\r';
      break;
    case '\\':
      value = '\\';
      break;
    case '\'':
      value = '\'';
      break;
    case '0':
      value = '\0';
      break;
    default:
      token.type = TOKEN_ERROR;
      token.value = strdup("Invalid character escape sequence");
      lexer_set_error(lexer, token.value);
      return token;
    }
  } else {
    if (ch == '\n' || ch == '\r') {
      token.type = TOKEN_ERROR;
      token.value = strdup("Unterminated character literal");
      lexer_set_error(lexer, token.value);
      return token;
    }
    value = (unsigned char)ch;
  }

  lexer->position++;
  lexer->column++;
  if (lexer->position >= lexer->length ||
      lexer->source[lexer->position] != '\'') {
    token.type = TOKEN_ERROR;
    token.value =
        strdup("Character literal must contain exactly one character");
    lexer_set_error(lexer, token.value);
    return token;
  }

  lexer->position++;
  lexer->column++;

  token.type = TOKEN_NUMBER;
  token.value = malloc(16);
  if (!token.value) {
    token.type = TOKEN_ERROR;
    token.value = strdup("Memory allocation failed");
    lexer_set_error(lexer, token.value);
    return token;
  }
  snprintf(token.value, 16, "%d", value);
  token_set_lexeme(&token, &lexer->source[literal_start],
                   lexer->position - literal_start);
  return token;
}

static Token lexer_lex_number(Lexer *lexer) {
  Token token = {TOKEN_EOF, NULL, {NULL, 0}, lexer->line, lexer->column, 0};
  size_t start = lexer->position;
  char current = lexer->source[lexer->position];

  if (current == '0' && lexer->position + 1 < lexer->length &&
      (lexer->source[lexer->position + 1] == 'x' ||
       lexer->source[lexer->position + 1] == 'X')) {
    lexer->position += 2;
    lexer->column += 2;
    size_t digits_start = lexer->position;
    while (lexer->position < lexer->length &&
           isxdigit(lexer->source[lexer->position])) {
      lexer->position++;
      lexer->column++;
    }
    if (lexer->position == digits_start) {
      token.type = TOKEN_ERROR;
      token.value = strdup("Invalid hexadecimal literal");
      lexer_set_error(lexer, token.value);
      return token;
    }
  } else if (current == '0' && lexer->position + 1 < lexer->length &&
             (lexer->source[lexer->position + 1] == 'b' ||
              lexer->source[lexer->position + 1] == 'B')) {
    lexer->position += 2;
    lexer->column += 2;
    size_t digits_start = lexer->position;
    while (lexer->position < lexer->length &&
           (lexer->source[lexer->position] == '0' ||
            lexer->source[lexer->position] == '1')) {
      lexer->position++;
      lexer->column++;
    }
    if (lexer->position == digits_start) {
      token.type = TOKEN_ERROR;
      token.value = strdup("Invalid binary literal");
      lexer_set_error(lexer, token.value);
      return token;
    }
  } else {
    while (lexer->position < lexer->length &&
           (isdigit(lexer->source[lexer->position]) ||
            // A '.' is a decimal point only when it is not the start of a `..`
            // range operator, so `1..5` lexes as 1, .., 5 rather than `1.`.
            (lexer->source[lexer->position] == '.' &&
             !(lexer->position + 1 < lexer->length &&
               lexer->source[lexer->position + 1] == '.')))) {
      lexer->position++;
      lexer->column++;
    }

    // Scientific notation: 'e' or 'E', an optional sign, then at least one
    // digit. The lookahead has to confirm a digit before anything is consumed,
    // so a trailing 'e' that is not an exponent (`1.e` or an identifier butted
    // against a number) is left for the following token.
    if (lexer->position < lexer->length &&
        (lexer->source[lexer->position] == 'e' ||
         lexer->source[lexer->position] == 'E')) {
      size_t peek = lexer->position + 1;
      if (peek < lexer->length &&
          (lexer->source[peek] == '+' || lexer->source[peek] == '-')) {
        peek++;
      }
      if (peek < lexer->length &&
          isdigit((unsigned char)lexer->source[peek])) {
        size_t skip = peek - lexer->position;
        lexer->position += skip;
        lexer->column += skip;
        while (lexer->position < lexer->length &&
               isdigit((unsigned char)lexer->source[lexer->position])) {
          lexer->position++;
          lexer->column++;
        }
      }
    }
  }

  size_t length = lexer->position - start;
  token.type = TOKEN_NUMBER;
  token.value = malloc(length + 1);
  if (!token.value) {
    token.type = TOKEN_ERROR;
    token.value = strdup("Memory allocation failed");
    lexer_set_error(lexer, token.value);
    return token;
  }
  /* memcpy, not strncpy: the span is exactly `length` bytes of source text with
   * the terminator written below, so strncpy's scan-for-NUL and tail padding are
   * both wasted -- and its byte-at-a-time loop showed up at 2.7% of total
   * compile time on a numeral-heavy input. */
  memcpy(token.value, &lexer->source[start], length);
  token.value[length] = '\0';
  token_set_lexeme(&token, &lexer->source[start], length);
  return token;
}

#define LEX_WORD_MAXLEN 10

/* Keyword lookup.
 *
 * Every identifier used to walk a chain of 94 strcmp/strcasecmp calls, and an
 * identifier that is not a keyword -- much the commonest case -- paid the whole
 * chain. Replacing it cut the parse phase roughly in half on a 200k-line input
 * (326ms to 177ms).
 *
 * The words are grouped by length, so a lookup only ever compares candidates
 * that could match, and sorted within each group, so a binary search settles it
 * in a few comparisons. The two groups stay separate and are consulted in the
 * original order: language keywords match exactly, inline-assembly mnemonics and
 * register names match case-insensitively.
 *
 * Both tables are const, so there is no initialization step and no shared
 * mutable state. The sort order and the _span arrays are what make the search
 * work, and getting them wrong just makes a keyword stop being recognised, so
 * they are maintained by tools/gen_lexer_keywords.py: add an entry anywhere and
 * re-run it. `--check` verifies without writing.
 */
typedef struct {
  char word[LEX_WORD_MAXLEN + 1];
  int type; /* TokenType */
} LexWord;

/* parsed 45 exact, 49 case-insensitive; max length 10 */
static const LexWord g_lex_keywords[] = {
    {"fn", TOKEN_FN},
    {"if", TOKEN_IF},
    {"asm", TOKEN_ASM},
    {"for", TOKEN_FOR},
    {"new", TOKEN_NEW},
    {"var", TOKEN_VAR},
    {"case", TOKEN_CASE},
    {"else", TOKEN_ELSE},
    {"enum", TOKEN_ENUM},
    {"impl", TOKEN_IMPL},
    {"int8", TOKEN_INT8},
    {"this", TOKEN_THIS},
    {"break", TOKEN_BREAK},
    {"const", TOKEN_CONST},
    {"defer", TOKEN_DEFER},
    {"int16", TOKEN_INT16},
    {"int32", TOKEN_INT32},
    {"int64", TOKEN_INT64},
    {"match", TOKEN_MATCH},
    {"trait", TOKEN_TRAIT},
    {"uint8", TOKEN_UINT8},
    {"where", TOKEN_WHERE},
    {"while", TOKEN_WHILE},
    {"export", TOKEN_EXPORT},
    {"extern", TOKEN_EXTERN},
    {"import", TOKEN_IMPORT},
    {"kernel", TOKEN_KERNEL},
    {"method", TOKEN_METHOD},
    {"return", TOKEN_RETURN},
    {"string", TOKEN_STRING_TYPE},
    {"struct", TOKEN_STRUCT},
    {"switch", TOKEN_SWITCH},
    {"uint16", TOKEN_UINT16},
    {"uint32", TOKEN_UINT32},
    {"uint64", TOKEN_UINT64},
    {"barrier", TOKEN_BARRIER},
    {"default", TOKEN_DEFAULT},
    {"float32", TOKEN_FLOAT32},
    {"float64", TOKEN_FLOAT64},
    {"private", TOKEN_PRIVATE},
    {"continue", TOKEN_CONTINUE},
    {"dispatch", TOKEN_DISPATCH},
    {"errdefer", TOKEN_ERRDEFER},
    {"workgroup", TOKEN_WORKGROUP},
    {"import_str", TOKEN_IMPORT_STR},
};
static const unsigned char g_lex_keywords_span[] = {0, 0, 0, 2, 6, 12, 23, 35, 40, 43, 44, 45};
static const LexWord g_lex_asm_words[] = {
    {"je", TOKEN_JE},
    {"jg", TOKEN_JG},
    {"jl", TOKEN_JL},
    {"r8", TOKEN_R8},
    {"r9", TOKEN_R9},
    {"add", TOKEN_ADD},
    {"cmp", TOKEN_CMP},
    {"dec", TOKEN_DEC},
    {"div", TOKEN_DIV},
    {"eax", TOKEN_EAX},
    {"ebp", TOKEN_EBP},
    {"ebx", TOKEN_EBX},
    {"ecx", TOKEN_ECX},
    {"edi", TOKEN_EDI},
    {"edx", TOKEN_EDX},
    {"esi", TOKEN_ESI},
    {"esp", TOKEN_ESP},
    {"inc", TOKEN_INC},
    {"int", TOKEN_INT},
    {"jge", TOKEN_JGE},
    {"jle", TOKEN_JLE},
    {"jmp", TOKEN_JMP},
    {"jne", TOKEN_JNE},
    {"lea", TOKEN_LEA},
    {"mov", TOKEN_MOV},
    {"mul", TOKEN_MUL},
    {"nop", TOKEN_NOP},
    {"pop", TOKEN_POP},
    {"r10", TOKEN_R10},
    {"r11", TOKEN_R11},
    {"r12", TOKEN_R12},
    {"r13", TOKEN_R13},
    {"r14", TOKEN_R14},
    {"r15", TOKEN_R15},
    {"rax", TOKEN_RAX},
    {"rbp", TOKEN_RBP},
    {"rbx", TOKEN_RBX},
    {"rcx", TOKEN_RCX},
    {"rdi", TOKEN_RDI},
    {"rdx", TOKEN_RDX},
    {"ret", TOKEN_RET},
    {"rsi", TOKEN_RSI},
    {"rsp", TOKEN_RSP},
    {"sub", TOKEN_SUB},
    {"call", TOKEN_CALL},
    {"idiv", TOKEN_IDIV},
    {"imul", TOKEN_IMUL},
    {"push", TOKEN_PUSH},
    {"syscall", TOKEN_SYSCALL},
};
static const unsigned char g_lex_asm_words_span[] = {0, 0, 0, 5, 44, 48, 48, 48, 49, 49, 49, 49};

/* Compare `len` bytes, folding the input to lower case when `fold` is set. The
 * table entries are already lower case, so a folded comparison keeps the same
 * byte ordering the tables are sorted by. */
static int lex_word_cmp(const char *input, const char *entry, size_t len,
                        int fold) {
  for (size_t i = 0; i < len; i++) {
    unsigned char a = (unsigned char)input[i];
    unsigned char b = (unsigned char)entry[i];
    if (fold) {
      a = (unsigned char)tolower(a);
    }
    if (a != b) {
      return a < b ? -1 : 1;
    }
  }
  return 0;
}

/* Token type for `text` in one of the tables above, or TOKEN_IDENTIFIER. */
static TokenType lex_lookup_word(const LexWord *table,
                                 const unsigned char *span, const char *text,
                                 size_t len, int fold) {
  if (len == 0 || len > LEX_WORD_MAXLEN) {
    return TOKEN_IDENTIFIER;
  }
  size_t lo = span[len];
  size_t hi = span[len + 1];
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    int c = lex_word_cmp(text, table[mid].word, len, fold);
    if (c == 0) {
      return (TokenType)table[mid].type;
    }
    if (c < 0) {
      hi = mid;
    } else {
      lo = mid + 1;
    }
  }
  return TOKEN_IDENTIFIER;
}

static Token lexer_lex_identifier_or_keyword(Lexer *lexer) {
  Token token = {TOKEN_EOF, NULL, {NULL, 0}, lexer->line, lexer->column, 0};
  size_t start = lexer->position;
  while (lexer->position < lexer->length &&
         (isalnum(lexer->source[lexer->position]) ||
          lexer->source[lexer->position] == '_')) {
    lexer->position++;
    lexer->column++;
  }

  size_t length = lexer->position - start;
  token.value = (char *)string_intern_n(&lexer->source[start], length);
  if (!token.value) {
    token.type = TOKEN_ERROR;
    token.value = strdup("Memory allocation failed");
    lexer_set_error(lexer, token.value);
    return token;
  }
  token.is_interned = 1;
  token_set_lexeme(&token, &lexer->source[start], length);

  token.type = lex_lookup_word(g_lex_keywords, g_lex_keywords_span,
                               token.value, length, /*fold=*/0);
  if (token.type == TOKEN_IDENTIFIER) {
    token.type = lex_lookup_word(g_lex_asm_words, g_lex_asm_words_span,
                                 token.value, length, /*fold=*/1);
  }

  return token;
}

static int lexer_read_string_body(Lexer *lexer, char *buffer,
                                  size_t *buffer_pos_out,
                                  int interpolation_aware, int *bad_escape) {
  size_t buffer_pos = 0;
  size_t depth = 0;
  int in_nested = 0;

  while (lexer->position < lexer->length) {
    char c = lexer->source[lexer->position];

    if (c == '"' && !(interpolation_aware && depth > 0)) {
      break;
    }

    if (c == '\\' && lexer->position + 1 < lexer->length) {
      lexer->position++;
      lexer->column++;

      char escape_char = lexer->source[lexer->position];
      switch (escape_char) {
      case 'n':
        buffer[buffer_pos++] = '\n';
        break;
      case 't':
        buffer[buffer_pos++] = '\t';
        break;
      case 'r':
        buffer[buffer_pos++] = '\r';
        break;
      case '\\':
        buffer[buffer_pos++] = '\\';
        break;
      case '"':
        buffer[buffer_pos++] = '"';
        break;
      case '0':
        buffer[buffer_pos++] = '\0';
        break;
      default:
        if (bad_escape && *bad_escape == 0) {
          *bad_escape = (unsigned char)escape_char;
        }
        buffer[buffer_pos++] = '\\';
        buffer[buffer_pos++] = escape_char;
        break;
      }
      lexer->position++;
      lexer->column++;
      continue;
    }

    if (interpolation_aware) {
      if (!in_nested && depth == 0 && c == '{' &&
          lexer->position + 1 < lexer->length &&
          lexer->source[lexer->position + 1] == '{') {
        buffer[buffer_pos++] = '{';
        buffer[buffer_pos++] = '{';
        lexer->position += 2;
        lexer->column += 2;
        continue;
      }
      if (depth > 0 && c == '"') {
        in_nested = !in_nested;
      } else if (!in_nested) {
        if (c == '{') {
          depth++;
        } else if (c == '}' && depth > 0) {
          depth--;
        }
      }
    }

    buffer[buffer_pos++] = c;
    lexer->position++;
    lexer->column++;
  }

  *buffer_pos_out = buffer_pos;
  return lexer->position < lexer->length;
}

static Token lexer_lex_string_literal(Lexer *lexer) {
  Token token = {TOKEN_EOF, NULL, {NULL, 0}, lexer->line, lexer->column, 0};
  lexer->position++;
  lexer->column++;

  size_t buffer_size = lexer->length - lexer->position;
  char *buffer = malloc(buffer_size + 1);
  if (!buffer) {
    token.type = TOKEN_ERROR;
    token.value = strdup("Memory allocation failed");
    lexer_set_error(lexer, token.value);
    return token;
  }

  size_t body_position = lexer->position;
  size_t body_column = lexer->column;
  size_t buffer_pos = 0;
  int bad_escape = 0;

  if (!lexer_read_string_body(lexer, buffer, &buffer_pos, 1, &bad_escape)) {
    lexer->position = body_position;
    lexer->column = body_column;
    bad_escape = 0;
    lexer_read_string_body(lexer, buffer, &buffer_pos, 0, &bad_escape);
  }

  if (lexer->position >= lexer->length) {
    free(buffer);
    token.type = TOKEN_ERROR;
    token.value = strdup("Unterminated string literal");
    lexer_set_error(lexer, token.value);
    return token;
  }

  if (bad_escape != 0) {
    /* Step past the closing quote so the next token is whatever follows the
     * string: leaving the cursor on it re-lexes the body and reports a second,
     * invented "unterminated string literal". */
    lexer->position++;
    lexer->column++;
    free(buffer);
    token.type = TOKEN_ERROR;
    token.value = strdup("Invalid string escape sequence");
    lexer_set_error(lexer, token.value);
    return token;
  }

  buffer[buffer_pos] = '\0';
  token.type = TOKEN_STRING;
  token.value = buffer;
  token_set_lexeme(&token, token.value, buffer_pos);

  lexer->position++;
  lexer->column++;
  return token;
}

static int lexer_token_continues_line(TokenType type) {
  switch (type) {
  case TOKEN_PLUS:
  case TOKEN_MINUS:
  case TOKEN_DIVIDE:
  case TOKEN_PERCENT:
  case TOKEN_AMPERSAND:
  case TOKEN_PIPE:
  case TOKEN_CARET:
  case TOKEN_LSHIFT:
  case TOKEN_RSHIFT:
  case TOKEN_AND_AND:
  case TOKEN_OR_OR:
  case TOKEN_EQUALS_EQUALS:
  case TOKEN_NOT_EQUALS:
  case TOKEN_LESS_THAN:
  case TOKEN_LESS_EQUALS:
  case TOKEN_GREATER_EQUALS:
  case TOKEN_EQUALS:
  case TOKEN_PLUS_EQUALS:
  case TOKEN_MINUS_EQUALS:
  case TOKEN_STAR_EQUALS:
  case TOKEN_SLASH_EQUALS:
  case TOKEN_PERCENT_EQUALS:
  case TOKEN_AMP_EQUALS:
  case TOKEN_PIPE_EQUALS:
  case TOKEN_CARET_EQUALS:
  case TOKEN_LSHIFT_EQUALS:
  case TOKEN_RSHIFT_EQUALS:
  case TOKEN_ARROW:
  case TOKEN_DOT:
  case TOKEN_DOT_DOT:
    return 1;
  default:
    return 0;
  }
}

static Token lexer_scan_token(Lexer *lexer) {
  Token token = {TOKEN_EOF, NULL, {NULL, 0}, lexer->line, lexer->column, 0};

  while (lexer->position < lexer->length &&
         isspace(lexer->source[lexer->position])) {
    if (lexer->source[lexer->position] == '\n') {
      if (lexer->continuation_depth > 0 ||
          lexer_token_continues_line(lexer->last_significant)) {
        lexer->position++;
        lexer->line++;
        lexer->column = 1;
        continue;
      }
      token.type = TOKEN_NEWLINE;
      token.value = NULL;
      token_set_lexeme(&token, &lexer->source[lexer->position], 1);
      lexer->position++;
      lexer->line++;
      lexer->column = 1;
      return token;
    }
    lexer->column++;
    lexer->position++;
  }

  if (lexer->position >= lexer->length) {
    return token;
  }

  char current = lexer->source[lexer->position];
  token.line = lexer->line;
  token.column = lexer->column;

  if (current == '/' && lexer->position + 1 < lexer->length &&
      lexer->source[lexer->position + 1] == '/') {
    return lexer_skip_line_comment(lexer);
  }

  if (current == '/' && lexer->position + 1 < lexer->length &&
      lexer->source[lexer->position + 1] == '*') {
    return lexer_skip_block_comment(lexer);
  }

  // Single character tokens
  switch (current) {
  case ':':
    token.type = TOKEN_COLON;
    break;
  case ';':
    token.type = TOKEN_SEMICOLON;
    break;
  case '@':
    token.type = TOKEN_AT;
    break;
  case ',':
    token.type = TOKEN_COMMA;
    break;
  case '=':
    if (lexer->position + 1 < lexer->length &&
        lexer->source[lexer->position + 1] == '=') {
      token.type = TOKEN_EQUALS_EQUALS;
      token_set_static_value(&token, "==");
      token_set_lexeme(&token, &lexer->source[lexer->position], 2);
      lexer->position += 2;
      lexer->column += 2;
      return token;
    }
    token.type = TOKEN_EQUALS;
    break;
  case '!':
    if (lexer->position + 1 < lexer->length &&
        lexer->source[lexer->position + 1] == '=') {
      token.type = TOKEN_NOT_EQUALS;
      token_set_static_value(&token, "!=");
      token_set_lexeme(&token, &lexer->source[lexer->position], 2);
      lexer->position += 2;
      lexer->column += 2;
      return token;
    }
    token.type = TOKEN_NOT;
    break;
  case '<':
    if (lexer->position + 1 < lexer->length) {
      if (lexer->source[lexer->position + 1] == '=') {
        token.type = TOKEN_LESS_EQUALS;
        token_set_static_value(&token, "<=");
        token_set_lexeme(&token, &lexer->source[lexer->position], 2);
        lexer->position += 2;
        lexer->column += 2;
        return token;
      } else if (lexer->source[lexer->position + 1] == '<') {
        if (lexer->position + 2 < lexer->length &&
            lexer->source[lexer->position + 2] == '=') {
          token.type = TOKEN_LSHIFT_EQUALS;
          token_set_static_value(&token, "<<=");
          token_set_lexeme(&token, &lexer->source[lexer->position], 3);
          lexer->position += 3;
          lexer->column += 3;
          return token;
        }
        token.type = TOKEN_LSHIFT;
        token_set_static_value(&token, "<<");
        token_set_lexeme(&token, &lexer->source[lexer->position], 2);
        lexer->position += 2;
        lexer->column += 2;
        return token;
      }
    }
    token.type = TOKEN_LESS_THAN;
    break;
  case '>':
    if (lexer->position + 1 < lexer->length) {
      if (lexer->source[lexer->position + 1] == '=') {
        token.type = TOKEN_GREATER_EQUALS;
        token_set_static_value(&token, ">=");
        token_set_lexeme(&token, &lexer->source[lexer->position], 2);
        lexer->position += 2;
        lexer->column += 2;
        return token;
      } else if (lexer->source[lexer->position + 1] == '>') {
        if (lexer->position + 2 < lexer->length &&
            lexer->source[lexer->position + 2] == '=') {
          token.type = TOKEN_RSHIFT_EQUALS;
          token_set_static_value(&token, ">>=");
          token_set_lexeme(&token, &lexer->source[lexer->position], 3);
          lexer->position += 3;
          lexer->column += 3;
          return token;
        }
        token.type = TOKEN_RSHIFT;
        token_set_static_value(&token, ">>");
        token_set_lexeme(&token, &lexer->source[lexer->position], 2);
        lexer->position += 2;
        lexer->column += 2;
        return token;
      }
    }
    token.type = TOKEN_GREATER_THAN;
    break;
  case '(':
    token.type = TOKEN_LPAREN;
    lexer->continuation_depth++;
    break;
  case ')':
    token.type = TOKEN_RPAREN;
    if (lexer->continuation_depth > 0)
      lexer->continuation_depth--;
    break;
  case '{':
    token.type = TOKEN_LBRACE;
    break;
  case '}':
    token.type = TOKEN_RBRACE;
    break;
  case '[':
    token.type = TOKEN_LBRACKET;
    lexer->continuation_depth++;
    break;
  case ']':
    token.type = TOKEN_RBRACKET;
    if (lexer->continuation_depth > 0)
      lexer->continuation_depth--;
    break;
  case '+':
    if (lexer->position + 1 < lexer->length &&
        lexer->source[lexer->position + 1] == '=') {
      token.type = TOKEN_PLUS_EQUALS;
      token_set_static_value(&token, "+=");
      token_set_lexeme(&token, &lexer->source[lexer->position], 2);
      lexer->position += 2;
      lexer->column += 2;
      return token;
    }
    if (lexer->position + 1 < lexer->length &&
        lexer->source[lexer->position + 1] == '+') {
      token.type = TOKEN_PLUS_PLUS;
      token_set_static_value(&token, "++");
      token_set_lexeme(&token, &lexer->source[lexer->position], 2);
      lexer->position += 2;
      lexer->column += 2;
      return token;
    }
    token.type = TOKEN_PLUS;
    break;
  case '*':
    if (lexer->position + 1 < lexer->length &&
        lexer->source[lexer->position + 1] == '=') {
      token.type = TOKEN_STAR_EQUALS;
      token_set_static_value(&token, "*=");
      token_set_lexeme(&token, &lexer->source[lexer->position], 2);
      lexer->position += 2;
      lexer->column += 2;
      return token;
    }
    token.type = TOKEN_MULTIPLY;
    break;
  case '&':
    if (lexer->position + 1 < lexer->length &&
        lexer->source[lexer->position + 1] == '&') {
      token.type = TOKEN_AND_AND;
      token_set_static_value(&token, "&&");
      token_set_lexeme(&token, &lexer->source[lexer->position], 2);
      lexer->position += 2;
      lexer->column += 2;
      return token;
    }
    if (lexer->position + 1 < lexer->length &&
        lexer->source[lexer->position + 1] == '=') {
      token.type = TOKEN_AMP_EQUALS;
      token_set_static_value(&token, "&=");
      token_set_lexeme(&token, &lexer->source[lexer->position], 2);
      lexer->position += 2;
      lexer->column += 2;
      return token;
    }
    token.type = TOKEN_AMPERSAND;
    break;
  case '|':
    if (lexer->position + 1 < lexer->length &&
        lexer->source[lexer->position + 1] == '|') {
      token.type = TOKEN_OR_OR;
      token_set_static_value(&token, "||");
      token_set_lexeme(&token, &lexer->source[lexer->position], 2);
      lexer->position += 2;
      lexer->column += 2;
      return token;
    }
    if (lexer->position + 1 < lexer->length &&
        lexer->source[lexer->position + 1] == '=') {
      token.type = TOKEN_PIPE_EQUALS;
      token_set_static_value(&token, "|=");
      token_set_lexeme(&token, &lexer->source[lexer->position], 2);
      lexer->position += 2;
      lexer->column += 2;
      return token;
    }
    token.type = TOKEN_PIPE;
    break;
  case '^':
    if (lexer->position + 1 < lexer->length &&
        lexer->source[lexer->position + 1] == '=') {
      token.type = TOKEN_CARET_EQUALS;
      token_set_static_value(&token, "^=");
      token_set_lexeme(&token, &lexer->source[lexer->position], 2);
      lexer->position += 2;
      lexer->column += 2;
      return token;
    }
    token.type = TOKEN_CARET;
    break;
  case '~':
    token.type = TOKEN_TILDE;
    break;
  case '/':
    // Note: comments (//, /* */) are already handled above before this switch
    if (lexer->position + 1 < lexer->length &&
        lexer->source[lexer->position + 1] == '=') {
      token.type = TOKEN_SLASH_EQUALS;
      token_set_static_value(&token, "/=");
      token_set_lexeme(&token, &lexer->source[lexer->position], 2);
      lexer->position += 2;
      lexer->column += 2;
      return token;
    }
    token.type = TOKEN_DIVIDE;
    break;
  case '%':
    if (lexer->position + 1 < lexer->length &&
        lexer->source[lexer->position + 1] == '=') {
      token.type = TOKEN_PERCENT_EQUALS;
      token_set_static_value(&token, "%=");
      token_set_lexeme(&token, &lexer->source[lexer->position], 2);
      lexer->position += 2;
      lexer->column += 2;
      return token;
    }
    token.type = TOKEN_PERCENT;
    break;
  case '.':
    if (lexer->position + 1 < lexer->length &&
        lexer->source[lexer->position + 1] == '.') {
      token.type = TOKEN_DOT_DOT;
      token_set_static_value(&token, "..");
      token_set_lexeme(&token, &lexer->source[lexer->position], 2);
      lexer->position += 2;
      lexer->column += 2;
      return token;
    }
    token.type = TOKEN_DOT;
    break;
  default:
    token.type = TOKEN_ERROR;
  }

  if (token.type != TOKEN_ERROR) {
    token_set_single_char_value(&token, current);
    token_set_lexeme(&token, &lexer->source[lexer->position], 1);
    lexer->position++;
    lexer->column++;
    return token;
  }

  // Handle arrow operator
  if (current == '-' && lexer->position + 1 < lexer->length &&
      lexer->source[lexer->position + 1] == '>') {
    token.type = TOKEN_ARROW;
    token_set_static_value(&token, "->");
    token_set_lexeme(&token, &lexer->source[lexer->position], 2);
    lexer->position += 2;
    lexer->column += 2;
    return token;
  }

  if (current == '-' && lexer->position + 1 < lexer->length &&
      lexer->source[lexer->position + 1] == '-') {
    token.type = TOKEN_MINUS_MINUS;
    token_set_static_value(&token, "--");
    token_set_lexeme(&token, &lexer->source[lexer->position], 2);
    lexer->position += 2;
    lexer->column += 2;
    return token;
  }

  // Compound minus assignment
  if (current == '-' && lexer->position + 1 < lexer->length &&
      lexer->source[lexer->position + 1] == '=') {
    token.type = TOKEN_MINUS_EQUALS;
    token_set_static_value(&token, "-=");
    token_set_lexeme(&token, &lexer->source[lexer->position], 2);
    lexer->position += 2;
    lexer->column += 2;
    return token;
  }

  if (current == '-') {
    token.type = TOKEN_MINUS;
    token_set_single_char_value(&token, current);
    token_set_lexeme(&token, &lexer->source[lexer->position], 1);
    lexer->position++;
    lexer->column++;
    return token;
  }

  if (current == '\'') {
    return lexer_lex_char_literal(lexer);
  }

  if (isdigit(current) ||
      (current == '0' && lexer->position + 1 < lexer->length &&
       (lexer->source[lexer->position + 1] == 'x' ||
        lexer->source[lexer->position + 1] == 'X' ||
        lexer->source[lexer->position + 1] == 'b' ||
        lexer->source[lexer->position + 1] == 'B'))) {
    return lexer_lex_number(lexer);
  }

  if (isalpha(current) || current == '_') {
    return lexer_lex_identifier_or_keyword(lexer);
  }

  if (current == '"') {
    return lexer_lex_string_literal(lexer);
  }

  // Unknown character
  token.type = TOKEN_ERROR;
  token.value = malloc(32);
  if (token.value) {
    snprintf(token.value, 32, "Unknown character: %c", current);
    lexer_set_error(lexer, token.value);
    token_set_lexeme(&token, &lexer->source[lexer->position], 1);
  } else {
    token.value = strdup("Unknown character");
    lexer_set_error(lexer, "Unknown character");
    token_set_lexeme(&token, token.value, strlen(token.value));
  }
  lexer->position++;
  lexer->column++;

  return token;
}

Token lexer_next_token(Lexer *lexer) {
  Token token = lexer_scan_token(lexer);

  if (token.type != TOKEN_NEWLINE) {
    lexer->last_significant = token.type;
  }
  return token;
}

Token lexer_peek_token(Lexer *lexer) {
  size_t saved_position = lexer->position;
  size_t saved_line = lexer->line;
  size_t saved_column = lexer->column;
  size_t saved_continuation_depth = lexer->continuation_depth;
  TokenType saved_last_significant = lexer->last_significant;

  Token token = lexer_next_token(lexer);

  lexer->position = saved_position;
  lexer->line = saved_line;
  lexer->column = saved_column;
  lexer->continuation_depth = saved_continuation_depth;
  lexer->last_significant = saved_last_significant;

  return token;
}

void token_destroy(Token *token) {
  if (!token) {
    return;
  }

  if (token->value) {
    if (!token->is_interned) {
      free(token->value);
    }
    token->value = NULL;
    token->is_interned = 0;
  }
  token->lexeme.data = NULL;
  token->lexeme.length = 0;
}

Token token_clone(const Token *token) {
  Token clone = {TOKEN_EOF, NULL, {NULL, 0}, 0, 0, 0};
  if (!token) {
    return clone;
  }

  clone = *token;
  if (token->value && !token->is_interned) {
    size_t bytes = strlen(token->value);
    if (token->lexeme.data == token->value && token->lexeme.length > bytes) {
      bytes = token->lexeme.length;
    }
    clone.value = malloc(bytes + 1);
    if (clone.value) {
      memcpy(clone.value, token->value, bytes);
      clone.value[bytes] = '\0';
    }
    if (!clone.value) {
      clone.type = TOKEN_ERROR;
      clone.is_interned = 0;
      clone.lexeme.data = NULL;
      clone.lexeme.length = 0;
    } else if (token->lexeme.data == token->value) {
      clone.lexeme.data = clone.value;
    }
  }
  return clone;
}

// Error reporting functions
void lexer_set_error(Lexer *lexer, const char *message) {
  if (!lexer)
    return;

  if (lexer->error_message) {
    free(lexer->error_message);
  }

  if (message) {
    size_t msg_len = strlen(message);
    lexer->error_message =
        malloc(msg_len + 100); // Extra space for location info
    if (lexer->error_message) {
      snprintf(lexer->error_message, msg_len + 100,
               "Lexer error at line %lu, column %lu: %s",
               (unsigned long)lexer->line, (unsigned long)lexer->column,
               message);
    }
  } else {
    lexer->error_message = NULL;
  }

  lexer->has_error = (message != NULL);
}
