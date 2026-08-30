#include "x86_asm.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define X86_ASM_MAX_OPERANDS 3

typedef enum {
  AT_END = 0,
  AT_IDENT,
  AT_NUMBER,
  AT_STRING,
  AT_PUNCT,
  AT_NEWLINE,
  AT_BINDING
} AsmTokenType;

typedef struct {
  AsmTokenType type;
  char text[192];
  long long number;
  size_t length;
  int line;
  char punct;
} AsmToken;

typedef struct {
  const char *source;
  size_t position;
  int line;
  AsmToken current;
  AsmToken lookahead;
  int has_lookahead;
  char error[256];
  int error_line;
  int failed;
} AsmLexer;

typedef struct {
  char *name;
  size_t offset;
} AsmLabel;

typedef struct {
  char *name;
  size_t offset;
  int bytes;
  int pc_relative;
  long long addend;
  size_t next_instruction_offset;
  int line;
  int branch_ordinal;
} AsmPatch;

typedef struct {
  unsigned char *code;
  size_t size;
  size_t capacity;
  AsmLabel *labels;
  size_t label_count;
  size_t label_capacity;
  AsmPatch *patches;
  size_t patch_count;
  size_t patch_capacity;
  char **declared;
  size_t declared_count;
  size_t declared_capacity;
  int bits;
  const X86AsmConfig *config;
  AsmLexer *lexer;
  int segment_override;
  int lock_prefix;
  int rep_prefix;
  unsigned char *short_hints;
  size_t short_hint_count;
  int branch_ordinal;
} AsmState;

static const struct {
  const char *name;
  int class_id;
  int number;
  int bytes;
  int high_byte;
} ASM_REGISTERS[] = {
    {"al", X86_ASM_REG_GP, 0, 1, 0},    {"cl", X86_ASM_REG_GP, 1, 1, 0},
    {"dl", X86_ASM_REG_GP, 2, 1, 0},    {"bl", X86_ASM_REG_GP, 3, 1, 0},
    {"ah", X86_ASM_REG_GP, 4, 1, 1},    {"ch", X86_ASM_REG_GP, 5, 1, 1},
    {"dh", X86_ASM_REG_GP, 6, 1, 1},    {"bh", X86_ASM_REG_GP, 7, 1, 1},
    {"spl", X86_ASM_REG_GP, 4, 1, 0},   {"bpl", X86_ASM_REG_GP, 5, 1, 0},
    {"sil", X86_ASM_REG_GP, 6, 1, 0},   {"dil", X86_ASM_REG_GP, 7, 1, 0},
    {"r8b", X86_ASM_REG_GP, 8, 1, 0},   {"r9b", X86_ASM_REG_GP, 9, 1, 0},
    {"r10b", X86_ASM_REG_GP, 10, 1, 0}, {"r11b", X86_ASM_REG_GP, 11, 1, 0},
    {"r12b", X86_ASM_REG_GP, 12, 1, 0}, {"r13b", X86_ASM_REG_GP, 13, 1, 0},
    {"r14b", X86_ASM_REG_GP, 14, 1, 0}, {"r15b", X86_ASM_REG_GP, 15, 1, 0},

    {"ax", X86_ASM_REG_GP, 0, 2, 0},    {"cx", X86_ASM_REG_GP, 1, 2, 0},
    {"dx", X86_ASM_REG_GP, 2, 2, 0},    {"bx", X86_ASM_REG_GP, 3, 2, 0},
    {"sp", X86_ASM_REG_GP, 4, 2, 0},    {"bp", X86_ASM_REG_GP, 5, 2, 0},
    {"si", X86_ASM_REG_GP, 6, 2, 0},    {"di", X86_ASM_REG_GP, 7, 2, 0},
    {"r8w", X86_ASM_REG_GP, 8, 2, 0},   {"r9w", X86_ASM_REG_GP, 9, 2, 0},
    {"r10w", X86_ASM_REG_GP, 10, 2, 0}, {"r11w", X86_ASM_REG_GP, 11, 2, 0},
    {"r12w", X86_ASM_REG_GP, 12, 2, 0}, {"r13w", X86_ASM_REG_GP, 13, 2, 0},
    {"r14w", X86_ASM_REG_GP, 14, 2, 0}, {"r15w", X86_ASM_REG_GP, 15, 2, 0},

    {"eax", X86_ASM_REG_GP, 0, 4, 0},   {"ecx", X86_ASM_REG_GP, 1, 4, 0},
    {"edx", X86_ASM_REG_GP, 2, 4, 0},   {"ebx", X86_ASM_REG_GP, 3, 4, 0},
    {"esp", X86_ASM_REG_GP, 4, 4, 0},   {"ebp", X86_ASM_REG_GP, 5, 4, 0},
    {"esi", X86_ASM_REG_GP, 6, 4, 0},   {"edi", X86_ASM_REG_GP, 7, 4, 0},
    {"r8d", X86_ASM_REG_GP, 8, 4, 0},   {"r9d", X86_ASM_REG_GP, 9, 4, 0},
    {"r10d", X86_ASM_REG_GP, 10, 4, 0}, {"r11d", X86_ASM_REG_GP, 11, 4, 0},
    {"r12d", X86_ASM_REG_GP, 12, 4, 0}, {"r13d", X86_ASM_REG_GP, 13, 4, 0},
    {"r14d", X86_ASM_REG_GP, 14, 4, 0}, {"r15d", X86_ASM_REG_GP, 15, 4, 0},

    {"rax", X86_ASM_REG_GP, 0, 8, 0},   {"rcx", X86_ASM_REG_GP, 1, 8, 0},
    {"rdx", X86_ASM_REG_GP, 2, 8, 0},   {"rbx", X86_ASM_REG_GP, 3, 8, 0},
    {"rsp", X86_ASM_REG_GP, 4, 8, 0},   {"rbp", X86_ASM_REG_GP, 5, 8, 0},
    {"rsi", X86_ASM_REG_GP, 6, 8, 0},   {"rdi", X86_ASM_REG_GP, 7, 8, 0},
    {"r8", X86_ASM_REG_GP, 8, 8, 0},    {"r9", X86_ASM_REG_GP, 9, 8, 0},
    {"r10", X86_ASM_REG_GP, 10, 8, 0},  {"r11", X86_ASM_REG_GP, 11, 8, 0},
    {"r12", X86_ASM_REG_GP, 12, 8, 0},  {"r13", X86_ASM_REG_GP, 13, 8, 0},
    {"r14", X86_ASM_REG_GP, 14, 8, 0},  {"r15", X86_ASM_REG_GP, 15, 8, 0},

    {"es", X86_ASM_REG_SEG, 0, 2, 0},   {"cs", X86_ASM_REG_SEG, 1, 2, 0},
    {"ss", X86_ASM_REG_SEG, 2, 2, 0},   {"ds", X86_ASM_REG_SEG, 3, 2, 0},
    {"fs", X86_ASM_REG_SEG, 4, 2, 0},   {"gs", X86_ASM_REG_SEG, 5, 2, 0},

    {"cr0", X86_ASM_REG_CR, 0, 8, 0},   {"cr2", X86_ASM_REG_CR, 2, 8, 0},
    {"cr3", X86_ASM_REG_CR, 3, 8, 0},   {"cr4", X86_ASM_REG_CR, 4, 8, 0},
    {"cr8", X86_ASM_REG_CR, 8, 8, 0},

    {"dr0", X86_ASM_REG_DR, 0, 8, 0},   {"dr1", X86_ASM_REG_DR, 1, 8, 0},
    {"dr2", X86_ASM_REG_DR, 2, 8, 0},   {"dr3", X86_ASM_REG_DR, 3, 8, 0},
    {"dr6", X86_ASM_REG_DR, 6, 8, 0},   {"dr7", X86_ASM_REG_DR, 7, 8, 0},

    {"xmm0", X86_ASM_REG_XMM, 0, 16, 0},   {"xmm1", X86_ASM_REG_XMM, 1, 16, 0},
    {"xmm2", X86_ASM_REG_XMM, 2, 16, 0},   {"xmm3", X86_ASM_REG_XMM, 3, 16, 0},
    {"xmm4", X86_ASM_REG_XMM, 4, 16, 0},   {"xmm5", X86_ASM_REG_XMM, 5, 16, 0},
    {"xmm6", X86_ASM_REG_XMM, 6, 16, 0},   {"xmm7", X86_ASM_REG_XMM, 7, 16, 0},
    {"xmm8", X86_ASM_REG_XMM, 8, 16, 0},   {"xmm9", X86_ASM_REG_XMM, 9, 16, 0},
    {"xmm10", X86_ASM_REG_XMM, 10, 16, 0}, {"xmm11", X86_ASM_REG_XMM, 11, 16, 0},
    {"xmm12", X86_ASM_REG_XMM, 12, 16, 0}, {"xmm13", X86_ASM_REG_XMM, 13, 16, 0},
    {"xmm14", X86_ASM_REG_XMM, 14, 16, 0}, {"xmm15", X86_ASM_REG_XMM, 15, 16, 0},

    {"rip", X86_ASM_REG_RIP, 0, 8, 0},  {"eip", X86_ASM_REG_RIP, 0, 4, 0},
};

static const struct {
  const char *name;
  int code;
} ASM_CONDITIONS[] = {
    {"o", 0},   {"no", 1},  {"b", 2},   {"c", 2},   {"nae", 2}, {"ae", 3},
    {"nb", 3},  {"nc", 3},  {"e", 4},   {"z", 4},   {"ne", 5},  {"nz", 5},
    {"be", 6},  {"na", 6},  {"a", 7},   {"nbe", 7}, {"s", 8},   {"ns", 9},
    {"p", 10},  {"pe", 10}, {"np", 11}, {"po", 11}, {"l", 12},  {"nge", 12},
    {"ge", 13}, {"nl", 13}, {"le", 14}, {"ng", 14}, {"g", 15},  {"nle", 15},
};

int x86_asm_lookup_register(const char *name, int *out_class, int *out_reg,
                            int *out_bytes, int *out_high_byte) {
  size_t i;
  if (!name) {
    return 0;
  }
  for (i = 0; i < sizeof(ASM_REGISTERS) / sizeof(ASM_REGISTERS[0]); i++) {
    if (strcmp(ASM_REGISTERS[i].name, name) != 0) {
      continue;
    }
    if (out_class) {
      *out_class = ASM_REGISTERS[i].class_id;
    }
    if (out_reg) {
      *out_reg = ASM_REGISTERS[i].number;
    }
    if (out_bytes) {
      *out_bytes = ASM_REGISTERS[i].bytes;
    }
    if (out_high_byte) {
      *out_high_byte = ASM_REGISTERS[i].high_byte;
    }
    return 1;
  }
  return 0;
}

static int asm_condition_code(const char *suffix) {
  size_t i;
  for (i = 0; i < sizeof(ASM_CONDITIONS) / sizeof(ASM_CONDITIONS[0]); i++) {
    if (strcmp(ASM_CONDITIONS[i].name, suffix) == 0) {
      return ASM_CONDITIONS[i].code;
    }
  }
  return -1;
}

static void asm_lexer_fail(AsmLexer *lexer, int line, const char *message) {
  if (lexer->failed) {
    return;
  }
  lexer->failed = 1;
  lexer->error_line = line;
  snprintf(lexer->error, sizeof(lexer->error), "%s", message);
}

static void asm_fail(AsmState *state, int line, const char *format, ...) {
  va_list arguments;
  if (state->lexer->failed) {
    return;
  }
  state->lexer->failed = 1;
  state->lexer->error_line = line;
  va_start(arguments, format);
  vsnprintf(state->lexer->error, sizeof(state->lexer->error), format,
            arguments);
  va_end(arguments);
}

static int asm_is_ident_start(int c) {
  return isalpha(c) || c == '_' || c == '.' || c == '$' || c == '@';
}

static int asm_is_ident_char(int c) {
  return isalnum(c) || c == '_' || c == '.' || c == '$' || c == '@';
}

static void asm_skip_trivia(AsmLexer *lexer) {
  const char *source = lexer->source;
  for (;;) {
    char c = source[lexer->position];
    if (c == ' ' || c == '\t' || c == '\r') {
      lexer->position++;
      continue;
    }
    if (c == ';' || c == '#' ||
        (c == '/' && source[lexer->position + 1] == '/')) {
      while (source[lexer->position] && source[lexer->position] != '\n') {
        lexer->position++;
      }
      continue;
    }
    if (c == '/' && source[lexer->position + 1] == '*') {
      lexer->position += 2;
      while (source[lexer->position] &&
             !(source[lexer->position] == '*' &&
               source[lexer->position + 1] == '/')) {
        if (source[lexer->position] == '\n') {
          lexer->line++;
        }
        lexer->position++;
      }
      if (source[lexer->position]) {
        lexer->position += 2;
      }
      continue;
    }
    return;
  }
}

static void asm_scan_binding(AsmLexer *lexer, AsmToken *token) {
  const char *source = lexer->source;
  size_t length = 0;
  lexer->position++;
  while (source[lexer->position] && source[lexer->position] != '}' &&
         source[lexer->position] != '\n') {
    if (length + 1 < sizeof(token->text)) {
      token->text[length++] = source[lexer->position];
    }
    lexer->position++;
  }
  token->text[length] = '\0';
  if (source[lexer->position] != '}') {
    asm_lexer_fail(lexer, token->line, "unterminated `{` operand binding");
    token->type = AT_END;
    return;
  }
  lexer->position++;
  while (length > 0 && isspace((unsigned char)token->text[length - 1])) {
    token->text[--length] = '\0';
  }
  token->type = AT_BINDING;
}

static char asm_scan_escape(AsmLexer *lexer) {
  char escaped = lexer->source[lexer->position + 1];
  lexer->position += 2;
  switch (escaped) {
  case 'n': return '\n';
  case 't': return '\t';
  case 'r': return '\r';
  case '0': return '\0';
  case 'e': return 27;
  default: return escaped;
  }
}

static void asm_scan_string(AsmLexer *lexer, AsmToken *token) {
  const char *source = lexer->source;
  char quote = source[lexer->position];
  size_t length = 0;
  size_t i;
  lexer->position++;
  while (source[lexer->position] && source[lexer->position] != quote) {
    char c = source[lexer->position];
    if (c == '\\' && source[lexer->position + 1]) {
      c = asm_scan_escape(lexer);
    } else {
      if (c == '\n') {
        lexer->line++;
      }
      lexer->position++;
    }
    if (length + 1 < sizeof(token->text)) {
      token->text[length++] = c;
    }
  }
  if (source[lexer->position] != quote) {
    asm_lexer_fail(lexer, token->line, "unterminated string in asm block");
    token->type = AT_END;
    return;
  }
  lexer->position++;
  token->text[length] = '\0';
  token->length = length;
  if (length >= 1 && length <= 8) {
    token->number = 0;
    for (i = 0; i < length; i++) {
      token->number |= ((long long)(unsigned char)token->text[i]) << (8 * i);
    }
  }
  token->type = AT_STRING;
}

static int asm_scan_number_base(AsmLexer *lexer) {
  const char *source = lexer->source;
  char marker;
  if (source[lexer->position] != '0') {
    return 10;
  }
  marker = source[lexer->position + 1];
  if (marker == 'x' || marker == 'X') {
    lexer->position += 2;
    return 16;
  }
  if (marker == 'b' || marker == 'B') {
    lexer->position += 2;
    return 2;
  }
  if (marker == 'o' || marker == 'O') {
    lexer->position += 2;
    return 8;
  }
  return 10;
}

static void asm_scan_number(AsmLexer *lexer, AsmToken *token) {
  const char *source = lexer->source;
  char buffer[128];
  size_t length = 0;
  int base = asm_scan_number_base(lexer);
  char *end = NULL;
  unsigned long long value;
  while (isalnum((unsigned char)source[lexer->position]) ||
         source[lexer->position] == '_') {
    if (source[lexer->position] != '_' && length + 1 < sizeof(buffer)) {
      buffer[length++] = source[lexer->position];
    }
    lexer->position++;
  }
  buffer[length] = '\0';
  value = strtoull(buffer, &end, base);
  if (!end || *end != '\0') {
    asm_lexer_fail(lexer, token->line, "malformed number in asm block");
    token->type = AT_END;
    return;
  }
  token->number = (long long)value;
  token->type = AT_NUMBER;
  snprintf(token->text, sizeof(token->text), "%s", buffer);
}

static void asm_scan_identifier(AsmLexer *lexer, AsmToken *token) {
  const char *source = lexer->source;
  size_t length = 0;
  while (asm_is_ident_char((unsigned char)source[lexer->position])) {
    if (length + 1 < sizeof(token->text)) {
      token->text[length++] = source[lexer->position];
    }
    lexer->position++;
  }
  token->text[length] = '\0';
  token->length = length;
  token->type = AT_IDENT;
}

static AsmToken asm_scan_token(AsmLexer *lexer) {
  AsmToken token;
  const char *source = lexer->source;
  char c;
  memset(&token, 0, sizeof(token));

  asm_skip_trivia(lexer);
  token.line = lexer->line;
  c = source[lexer->position];

  if (c == '\0') {
    token.type = AT_END;
    return token;
  }
  if (c == '\n') {
    lexer->position++;
    lexer->line++;
    token.type = AT_NEWLINE;
    return token;
  }
  if (c == '{') {
    asm_scan_binding(lexer, &token);
    return token;
  }
  if (c == '\'' || c == '"') {
    asm_scan_string(lexer, &token);
    return token;
  }
  if (isdigit((unsigned char)c)) {
    asm_scan_number(lexer, &token);
    return token;
  }
  if (asm_is_ident_start((unsigned char)c)) {
    asm_scan_identifier(lexer, &token);
    return token;
  }

  token.type = AT_PUNCT;
  token.punct = c;
  token.text[0] = c;
  token.text[1] = '\0';
  lexer->position++;
  return token;
}

static void asm_advance(AsmLexer *lexer) {
  if (lexer->has_lookahead) {
    lexer->current = lexer->lookahead;
    lexer->has_lookahead = 0;
    return;
  }
  lexer->current = asm_scan_token(lexer);
}

static const AsmToken *asm_peek(AsmLexer *lexer) {
  if (!lexer->has_lookahead) {
    lexer->lookahead = asm_scan_token(lexer);
    lexer->has_lookahead = 1;
  }
  return &lexer->lookahead;
}

static void asm_lowercase(char *text) {
  while (*text) {
    *text = (char)tolower((unsigned char)*text);
    text++;
  }
}

static int asm_reserve(AsmState *state, size_t extra) {
  size_t capacity;
  unsigned char *grown;
  if (state->size + extra <= state->capacity) {
    return 1;
  }
  capacity = state->capacity ? state->capacity : 256;
  while (capacity < state->size + extra) {
    capacity *= 2;
  }
  grown = (unsigned char *)realloc(state->code, capacity);
  if (!grown) {
    return 0;
  }
  state->code = grown;
  state->capacity = capacity;
  return 1;
}

static int asm_byte(AsmState *state, unsigned int value) {
  if (!asm_reserve(state, 1)) {
    return 0;
  }
  state->code[state->size++] = (unsigned char)(value & 0xFF);
  return 1;
}

static int asm_value(AsmState *state, long long value, int bytes) {
  int i;
  if (!asm_reserve(state, (size_t)bytes)) {
    return 0;
  }
  for (i = 0; i < bytes; i++) {
    state->code[state->size++] =
        (unsigned char)((unsigned long long)value >> (8 * i)) & 0xFFu;
  }
  return 1;
}

static int asm_declare_label(AsmState *state, const char *name) {
  if (state->declared_count == state->declared_capacity) {
    size_t capacity = state->declared_capacity ? state->declared_capacity * 2 : 8;
    char **grown = (char **)realloc(state->declared, capacity * sizeof(char *));
    if (!grown) {
      return 0;
    }
    state->declared = grown;
    state->declared_capacity = capacity;
  }
  state->declared[state->declared_count] = strdup(name);
  if (!state->declared[state->declared_count]) {
    return 0;
  }
  state->declared_count++;
  return 1;
}

static int asm_is_declared_label(const AsmState *state, const char *name) {
  size_t i;
  for (i = 0; i < state->declared_count; i++) {
    if (strcmp(state->declared[i], name) == 0) {
      return 1;
    }
  }
  return 0;
}

static int asm_add_label(AsmState *state, const char *name, size_t offset) {
  if (state->label_count == state->label_capacity) {
    size_t capacity = state->label_capacity ? state->label_capacity * 2 : 8;
    AsmLabel *grown =
        (AsmLabel *)realloc(state->labels, capacity * sizeof(AsmLabel));
    if (!grown) {
      return 0;
    }
    state->labels = grown;
    state->label_capacity = capacity;
  }
  state->labels[state->label_count].name = strdup(name);
  if (!state->labels[state->label_count].name) {
    return 0;
  }
  state->labels[state->label_count].offset = offset;
  state->label_count++;
  return 1;
}

static int asm_find_label(const AsmState *state, const char *name,
                          size_t *out_offset) {
  size_t i;
  for (i = 0; i < state->label_count; i++) {
    if (strcmp(state->labels[i].name, name) == 0) {
      if (out_offset) {
        *out_offset = state->labels[i].offset;
      }
      return 1;
    }
  }
  return 0;
}

static int asm_patch(AsmState *state, const char *name, size_t offset,
                     int bytes, int pc_relative, long long addend, int line) {
  if (state->patch_count == state->patch_capacity) {
    size_t capacity = state->patch_capacity ? state->patch_capacity * 2 : 8;
    AsmPatch *grown =
        (AsmPatch *)realloc(state->patches, capacity * sizeof(AsmPatch));
    if (!grown) {
      return 0;
    }
    state->patches = grown;
    state->patch_capacity = capacity;
  }
  state->patches[state->patch_count].name = strdup(name);
  if (!state->patches[state->patch_count].name) {
    return 0;
  }
  state->patches[state->patch_count].offset = offset;
  state->patches[state->patch_count].bytes = bytes;
  state->patches[state->patch_count].pc_relative = pc_relative;
  state->patches[state->patch_count].addend = addend;
  state->patches[state->patch_count].next_instruction_offset = 0;
  state->patches[state->patch_count].line = line;
  state->patches[state->patch_count].branch_ordinal = -1;
  state->patch_count++;
  return 1;
}

static void asm_operand_release(X86AsmOperand *operand) {
  if (operand && operand->symbol) {
    free(operand->symbol);
    operand->symbol = NULL;
  }
}

static int asm_size_keyword(const char *text) {
  if (strcmp(text, "byte") == 0) {
    return 1;
  }
  if (strcmp(text, "word") == 0) {
    return 2;
  }
  if (strcmp(text, "dword") == 0) {
    return 4;
  }
  if (strcmp(text, "qword") == 0) {
    return 8;
  }
  if (strcmp(text, "xmmword") == 0 || strcmp(text, "oword") == 0) {
    return 16;
  }
  return 0;
}

static int asm_parse_memory(AsmState *state, X86AsmOperand *operand,
                            int size_bytes, int segment) {
  AsmLexer *lexer = state->lexer;
  int sign = 1;
  int expect_term = 1;

  operand->kind = X86_ASM_OPERAND_MEM;
  operand->mem_bytes = size_bytes;
  operand->segment = segment;
  operand->scale = 1;

  asm_advance(lexer);

  while (!(lexer->current.type == AT_PUNCT && lexer->current.punct == ']')) {
    if (lexer->current.type == AT_END || lexer->current.type == AT_NEWLINE) {
      asm_fail(state, lexer->current.line, "unterminated memory operand");
      return 0;
    }
    if (lexer->current.type == AT_PUNCT && lexer->current.punct == '+') {
      sign = 1;
      expect_term = 1;
      asm_advance(lexer);
      continue;
    }
    if (lexer->current.type == AT_PUNCT && lexer->current.punct == '-') {
      sign = -1;
      expect_term = 1;
      asm_advance(lexer);
      continue;
    }
    if (!expect_term) {
      asm_fail(state, lexer->current.line,
               "expected `+` or `-` between memory operand terms");
      return 0;
    }
    expect_term = 0;

    if (lexer->current.type == AT_NUMBER) {
      long long value = lexer->current.number;
      const AsmToken *next = asm_peek(lexer);
      if (next->type == AT_PUNCT && next->punct == '*') {
        char name[192];
        int reg_class = 0;
        int number = 0;
        int bytes = 0;
        asm_advance(lexer);
        asm_advance(lexer);
        if (lexer->current.type != AT_IDENT) {
          asm_fail(state, lexer->current.line,
                   "expected a register after `*` in a memory operand");
          return 0;
        }
        snprintf(name, sizeof(name), "%s", lexer->current.text);
        asm_lowercase(name);
        if (!x86_asm_lookup_register(name, &reg_class, &number, &bytes, NULL) ||
            reg_class != X86_ASM_REG_GP) {
          asm_fail(state, lexer->current.line,
                   "`%s` is not a general-purpose register", name);
          return 0;
        }
        operand->has_index = 1;
        operand->index = number;
        operand->scale = (int)value;
        operand->address_bytes = bytes;
        asm_advance(lexer);
        continue;
      }
      operand->disp += sign * value;
      asm_advance(lexer);
      continue;
    }

    if (lexer->current.type == AT_IDENT) {
      char name[192];
      int reg_class = 0;
      int number = 0;
      int bytes = 0;
      snprintf(name, sizeof(name), "%s", lexer->current.text);
      asm_lowercase(name);
      if (x86_asm_lookup_register(name, &reg_class, &number, &bytes, NULL)) {
        const AsmToken *next = NULL;
        if (reg_class == X86_ASM_REG_RIP) {
          operand->rip_relative = 1;
          asm_advance(lexer);
          continue;
        }
        if (reg_class != X86_ASM_REG_GP) {
          asm_fail(state, lexer->current.line,
                   "`%s` cannot appear inside a memory operand", name);
          return 0;
        }
        next = asm_peek(lexer);
        if (next->type == AT_PUNCT && next->punct == '*') {
          asm_advance(lexer);
          asm_advance(lexer);
          if (lexer->current.type != AT_NUMBER) {
            asm_fail(state, lexer->current.line,
                     "expected a scale (1, 2, 4 or 8) after `*`");
            return 0;
          }
          operand->has_index = 1;
          operand->index = number;
          operand->scale = (int)lexer->current.number;
          operand->address_bytes = bytes;
          asm_advance(lexer);
          continue;
        }
        if (!operand->has_base) {
          operand->has_base = 1;
          operand->base = number;
        } else if (!operand->has_index) {
          operand->has_index = 1;
          operand->index = number;
          operand->scale = 1;
        } else {
          asm_fail(state, lexer->current.line,
                   "a memory operand takes at most a base and an index");
          return 0;
        }
        operand->address_bytes = bytes;
        asm_advance(lexer);
        continue;
      }
      if (strcmp(name, "$") == 0) {
        operand->disp += sign * (long long)(state->config->origin + state->size);
        asm_advance(lexer);
        continue;
      }
      if (operand->symbol) {
        asm_fail(state, lexer->current.line,
                 "a memory operand takes at most one symbol");
        return 0;
      }
      operand->symbol = strdup(lexer->current.text);
      if (!operand->symbol) {
        asm_fail(state, lexer->current.line, "out of memory");
        return 0;
      }
      operand->symbol_is_label = asm_is_declared_label(state, operand->symbol);
      asm_advance(lexer);
      continue;
    }

    asm_fail(state, lexer->current.line, "unexpected `%s` in a memory operand",
             lexer->current.text);
    return 0;
  }

  asm_advance(lexer);

  if (operand->has_index && operand->scale != 1 && operand->scale != 2 &&
      operand->scale != 4 && operand->scale != 8) {
    asm_fail(state, lexer->current.line, "an index scale must be 1, 2, 4 or 8");
    return 0;
  }
  return 1;
}

static int asm_parse_operand(AsmState *state, X86AsmOperand *operand);

static int asm_parse_operand_prefixes(AsmState *state, X86AsmOperand *operand) {
  AsmLexer *lexer = state->lexer;
  int size_bytes = 0;
  for (;;) {
    char name[192];
    int keyword_size;
    if (lexer->current.type != AT_IDENT) {
      return size_bytes;
    }
    snprintf(name, sizeof(name), "%s", lexer->current.text);
    asm_lowercase(name);
    keyword_size = asm_size_keyword(name);
    if (keyword_size) {
      const AsmToken *next = asm_peek(lexer);
      int next_is_ptr = next->type == AT_IDENT;
      char peeked[192];
      if (next_is_ptr) {
        snprintf(peeked, sizeof(peeked), "%s", next->text);
        asm_lowercase(peeked);
        next_is_ptr = strcmp(peeked, "ptr") == 0;
      }
      if (next_is_ptr) {
        size_bytes = keyword_size;
        asm_advance(lexer);
        asm_advance(lexer);
        continue;
      }
      if (next->type == AT_PUNCT && next->punct == '[') {
        size_bytes = keyword_size;
        asm_advance(lexer);
        continue;
      }
    }
    if (strcmp(name, "short") == 0 || strcmp(name, "near") == 0) {
      operand->explicit_short = strcmp(name, "short") == 0;
      asm_advance(lexer);
      continue;
    }
    if (strcmp(name, "ptr") == 0 || strcmp(name, "offset") == 0) {
      asm_advance(lexer);
      continue;
    }
    return size_bytes;
  }
}

static int asm_parse_register_operand(AsmState *state, X86AsmOperand *operand,
                                      int size_bytes, int *handled) {
  AsmLexer *lexer = state->lexer;
  char name[192];
  int reg_class = 0;
  int number = 0;
  int bytes = 0;
  int high = 0;
  const AsmToken *next;

  *handled = 0;
  snprintf(name, sizeof(name), "%s", lexer->current.text);
  asm_lowercase(name);
  if (!x86_asm_lookup_register(name, &reg_class, &number, &bytes, &high)) {
    return 0;
  }
  *handled = 1;
  next = asm_peek(lexer);
  if (reg_class == X86_ASM_REG_SEG && next->type == AT_PUNCT &&
      next->punct == ':') {
    asm_advance(lexer);
    asm_advance(lexer);
    if (!(lexer->current.type == AT_PUNCT && lexer->current.punct == '[')) {
      asm_fail(state, lexer->current.line,
               "expected `[` after a segment override");
      return 0;
    }
    return asm_parse_memory(state, operand, size_bytes, number + 1);
  }
  operand->kind = X86_ASM_OPERAND_REG;
  operand->reg_class = reg_class;
  operand->reg = number;
  operand->reg_bytes = bytes;
  operand->high_byte = high;
  asm_advance(lexer);
  return 1;
}

static int asm_parse_binding_operand(AsmState *state, X86AsmOperand *operand,
                                     int size_bytes) {
  AsmLexer *lexer = state->lexer;
  char error[192];
  X86AsmOperand bound;
  memset(&bound, 0, sizeof(bound));
  if (!state->config->resolve_binding) {
    asm_fail(state, lexer->current.line,
             "`{%s}` operand bindings are not available in this context",
             lexer->current.text);
    return 0;
  }
  error[0] = '\0';
  if (!state->config->resolve_binding(state->config->binding_context,
                                      lexer->current.text, &bound, error,
                                      sizeof(error))) {
    asm_fail(state, lexer->current.line, "%s",
             error[0] ? error : "unknown operand binding");
    return 0;
  }
  if (size_bytes && bound.kind == X86_ASM_OPERAND_MEM) {
    bound.mem_bytes = size_bytes;
  }
  *operand = bound;
  asm_advance(lexer);
  return 1;
}

static void asm_parse_immediate_sign(AsmLexer *lexer, int *sign) {
  for (;;) {
    if (lexer->current.type == AT_PUNCT && lexer->current.punct == '-') {
      *sign = -*sign;
      asm_advance(lexer);
      continue;
    }
    if (lexer->current.type == AT_PUNCT && lexer->current.punct == '+') {
      asm_advance(lexer);
      continue;
    }
    return;
  }
}

static int asm_parse_immediate_symbol(AsmState *state, X86AsmOperand *operand,
                                      int sign) {
  AsmLexer *lexer = state->lexer;
  if (strcmp(lexer->current.text, "$") == 0) {
    operand->imm += sign * (long long)(state->config->origin + state->size);
    asm_advance(lexer);
    return 1;
  }
  if (strcmp(lexer->current.text, "$$") == 0) {
    operand->imm += sign * (long long)state->config->origin;
    asm_advance(lexer);
    return 1;
  }
  if (operand->symbol) {
    asm_fail(state, lexer->current.line, "an operand takes at most one symbol");
    return 0;
  }
  operand->symbol = strdup(lexer->current.text);
  if (!operand->symbol) {
    asm_fail(state, lexer->current.line, "out of memory");
    return 0;
  }
  operand->symbol_is_label = asm_is_declared_label(state, operand->symbol);
  asm_advance(lexer);
  return 1;
}

static int asm_parse_far_offset(AsmState *state, X86AsmOperand *operand) {
  AsmLexer *lexer = state->lexer;
  X86AsmOperand offset_operand;
  if (operand->symbol) {
    asm_fail(state, lexer->current.line,
             "a far pointer's selector must be a constant");
    return 0;
  }
  asm_advance(lexer);
  if (!asm_parse_operand(state, &offset_operand)) {
    return 0;
  }
  operand->far_segment = operand->imm;
  operand->imm = offset_operand.imm;
  operand->symbol = offset_operand.symbol;
  operand->symbol_is_label = offset_operand.symbol_is_label;
  operand->kind = X86_ASM_OPERAND_FAR;
  return 1;
}

static int asm_parse_immediate_operand(AsmState *state,
                                       X86AsmOperand *operand) {
  AsmLexer *lexer = state->lexer;
  int sign = 1;
  int have_value = 0;

  operand->kind = X86_ASM_OPERAND_IMM;
  asm_parse_immediate_sign(lexer, &sign);
  for (;;) {
    if (lexer->current.type == AT_NUMBER ||
        lexer->current.type == AT_STRING) {
      if (lexer->current.type == AT_STRING && lexer->current.length > 8) {
        asm_fail(state, lexer->current.line,
                 "a string longer than 8 bytes is not a constant");
        return 0;
      }
      operand->imm += sign * lexer->current.number;
      asm_advance(lexer);
    } else if (lexer->current.type == AT_IDENT) {
      if (!asm_parse_immediate_symbol(state, operand, sign)) {
        return 0;
      }
    } else {
      break;
    }
    have_value = 1;
    if (lexer->current.type == AT_PUNCT &&
        (lexer->current.punct == '+' || lexer->current.punct == '-')) {
      sign = lexer->current.punct == '-' ? -1 : 1;
      asm_advance(lexer);
      continue;
    }
    break;
  }
  if (!have_value) {
    asm_fail(state, lexer->current.line, "expected an operand");
    return 0;
  }
  if (lexer->current.type == AT_PUNCT && lexer->current.punct == ':') {
    return asm_parse_far_offset(state, operand);
  }
  return 1;
}

static int asm_parse_operand(AsmState *state, X86AsmOperand *operand) {
  AsmLexer *lexer = state->lexer;
  int size_bytes;

  memset(operand, 0, sizeof(*operand));
  operand->scale = 1;
  size_bytes = asm_parse_operand_prefixes(state, operand);

  if (lexer->current.type == AT_IDENT) {
    int handled = 0;
    int parsed = asm_parse_register_operand(state, operand, size_bytes,
                                            &handled);
    if (handled) {
      return parsed;
    }
  }
  if (lexer->current.type == AT_PUNCT && lexer->current.punct == '[') {
    return asm_parse_memory(state, operand, size_bytes, 0);
  }
  if (lexer->current.type == AT_BINDING) {
    return asm_parse_binding_operand(state, operand, size_bytes);
  }
  return asm_parse_immediate_operand(state, operand);
}

static int asm_address_bytes(const AsmState *state,
                             const X86AsmOperand *operand) {
  if (operand->address_bytes) {
    return operand->address_bytes;
  }
  return state->bits / 8;
}

static int asm_fits_signed(long long value, int bytes) {
  long long low;
  long long high;
  if (bytes >= 8) {
    return 1;
  }
  low = -(1LL << (bytes * 8 - 1));
  high = (1LL << (bytes * 8 - 1)) - 1;
  return value >= low && value <= high;
}

typedef struct {
  int base;
  int index;
  int field;
} AsmModRm16;

static const AsmModRm16 ASM_MODRM16_PAIR[] = {
    {3, 6, 0}, {3, 7, 1}, {5, 6, 2}, {5, 7, 3},
    {6, 3, 0}, {7, 3, 1}, {6, 5, 2}, {7, 5, 3}};

static const AsmModRm16 ASM_MODRM16_SINGLE[] = {
    {6, 0, 4}, {7, 0, 5}, {5, 0, 6}, {3, 0, 7}};

static int asm_rex_flags(const X86AsmOperand *rm, int reg_field,
                         int reg_field_is_gp, int reg_high_byte,
                         int operand_bytes, int force_rex_w, int *needs_rex,
                         int *forbids_rex) {
  int rex = 0;
  *needs_rex = 0;
  *forbids_rex = 0;
  if (reg_high_byte) {
    *forbids_rex = 1;
  }
  if (reg_field_is_gp && operand_bytes == 1 && !reg_high_byte &&
      reg_field >= 4 && reg_field <= 7) {
    *needs_rex = 1;
  }
  if (rm && rm->kind == X86_ASM_OPERAND_REG) {
    if (rm->high_byte) {
      *forbids_rex = 1;
    } else if (rm->reg_class == X86_ASM_REG_GP && rm->reg_bytes == 1 &&
               rm->reg >= 4 && rm->reg <= 7) {
      *needs_rex = 1;
    }
  }
  if (operand_bytes == 8 || force_rex_w) {
    rex |= 0x08;
  }
  if (reg_field >= 8) {
    rex |= 0x04;
  }
  if (rm && rm->kind == X86_ASM_OPERAND_REG && rm->reg >= 8) {
    rex |= 0x01;
  }
  if (rm && rm->kind == X86_ASM_OPERAND_MEM) {
    if (rm->has_base && rm->base >= 8) {
      rex |= 0x01;
    }
    if (rm->has_index && rm->index >= 8) {
      rex |= 0x02;
    }
  }
  return rex;
}

static int asm_emit_prefixes(AsmState *state, int line,
                             const X86AsmOperand *rm, int address_bytes,
                             int operand_bytes, int mandatory_prefix) {
  if (state->lock_prefix && !asm_byte(state, 0xF0)) {
    return 0;
  }
  if (state->rep_prefix && !asm_byte(state, (unsigned)state->rep_prefix)) {
    return 0;
  }

  if (rm && rm->kind == X86_ASM_OPERAND_MEM && rm->segment) {
    static const unsigned char segment_prefixes[6] = {0x26, 0x2E, 0x36,
                                                      0x3E, 0x64, 0x65};
    if (!asm_byte(state, segment_prefixes[rm->segment - 1])) {
      return 0;
    }
  }

  if (rm && rm->kind == X86_ASM_OPERAND_MEM &&
      address_bytes != state->bits / 8) {
    if (state->bits == 64 && address_bytes == 2) {
      asm_fail(state, line, "16-bit addressing is not available in 64-bit code");
      return 0;
    }
    if (!asm_byte(state, 0x67)) {
      return 0;
    }
  }

  if (mandatory_prefix && !asm_byte(state, (unsigned)mandatory_prefix)) {
    return 0;
  }

  if (state->bits == 16) {
    if (operand_bytes == 4 && !asm_byte(state, 0x66)) {
      return 0;
    }
    if (operand_bytes == 8) {
      asm_fail(state, line, "64-bit operands are not available in 16-bit code");
      return 0;
    }
    return 1;
  }
  if (operand_bytes == 2 && mandatory_prefix != 0x66 &&
      !asm_byte(state, 0x66)) {
    return 0;
  }
  if (operand_bytes == 8 && state->bits != 64) {
    asm_fail(state, line, "64-bit operands are not available in 32-bit code");
    return 0;
  }
  return 1;
}

static int asm_emit_displacement(AsmState *state, int line,
                                 const X86AsmOperand *rm, long long
                                 displacement, int bytes, int pc_relative) {
  if (rm->symbol) {
    if (!asm_patch(state, rm->symbol, state->size, bytes, pc_relative,
                   displacement, line)) {
      return 0;
    }
    return asm_value(state, 0, bytes);
  }
  return asm_value(state, displacement, bytes);
}

static int asm_modrm16_field(const X86AsmOperand *rm) {
  size_t i;
  if (rm->has_base && rm->has_index) {
    for (i = 0; i < sizeof(ASM_MODRM16_PAIR) / sizeof(ASM_MODRM16_PAIR[0]);
         i++) {
      if (rm->base == ASM_MODRM16_PAIR[i].base &&
          rm->index == ASM_MODRM16_PAIR[i].index) {
        return ASM_MODRM16_PAIR[i].field;
      }
    }
    return -1;
  }
  for (i = 0; i < sizeof(ASM_MODRM16_SINGLE) / sizeof(ASM_MODRM16_SINGLE[0]);
       i++) {
    if ((rm->has_base ? rm->base : rm->index) ==
        ASM_MODRM16_SINGLE[i].base) {
      return ASM_MODRM16_SINGLE[i].field;
    }
  }
  return -1;
}

static int asm_emit_modrm16(AsmState *state, int line, int reg_field,
                            const X86AsmOperand *rm) {
  int rm_field;
  int mod;
  long long displacement = rm->disp;
  if (rm->rip_relative) {
    asm_fail(state, line, "rip-relative addressing needs 64-bit code");
    return 0;
  }
  if (!rm->has_base && !rm->has_index) {
    if (!asm_byte(state, ((reg_field & 7) << 3) | 6)) {
      return 0;
    }
    return asm_emit_displacement(state, line, rm, displacement, 2, 0);
  }
  if (rm->has_base && rm->has_index && rm->scale != 1) {
    asm_fail(state, line, "16-bit addressing has no scaled index");
    return 0;
  }
  rm_field = asm_modrm16_field(rm);
  if (rm_field < 0) {
    asm_fail(state, line, "that base/index combination has no 16-bit encoding");
    return 0;
  }
  if (rm->symbol) {
    mod = 2;
  } else if (displacement == 0 && rm_field != 6) {
    mod = 0;
  } else if (asm_fits_signed(displacement, 1)) {
    mod = 1;
  } else {
    mod = 2;
  }
  if (!asm_byte(state,
                (unsigned)((mod << 6) | ((reg_field & 7) << 3) | rm_field))) {
    return 0;
  }
  if (mod == 1) {
    return asm_value(state, displacement, 1);
  }
  if (mod == 2) {
    return asm_emit_displacement(state, line, rm, displacement, 2, 0);
  }
  return 1;
}

static int asm_emit_sib(AsmState *state, int line, const X86AsmOperand *rm) {
  int scale_bits;
  int index_field = rm->has_index ? (rm->index & 7) : 4;
  int base_field = rm->has_base ? (rm->base & 7) : 5;
  switch (rm->scale) {
  case 1: scale_bits = 0; break;
  case 2: scale_bits = 1; break;
  case 4: scale_bits = 2; break;
  case 8: scale_bits = 3; break;
  default:
    asm_fail(state, line, "an index scale must be 1, 2, 4 or 8");
    return 0;
  }
  return asm_byte(state, (unsigned)((scale_bits << 6) | (index_field << 3) |
                                    base_field));
}

static int asm_emit_modrm32(AsmState *state, int line, int reg_field,
                            const X86AsmOperand *rm) {
  int mod;
  int rm_field;
  int need_sib = 0;
  int displacement_bytes = 0;
  long long displacement = rm->disp;

  if (rm->rip_relative ||
      (state->bits == 64 && rm->symbol && !rm->has_base && !rm->has_index)) {
    if (state->bits != 64) {
      asm_fail(state, line, "rip-relative addressing needs 64-bit code");
      return 0;
    }
    if (!asm_byte(state, (unsigned)(((reg_field & 7) << 3) | 5))) {
      return 0;
    }
    return asm_emit_displacement(state, line, rm, displacement, 4, 1);
  }

  if (!rm->has_base && !rm->has_index) {
    if (state->bits == 64) {
      if (!asm_byte(state, (unsigned)(((reg_field & 7) << 3) | 4))) {
        return 0;
      }
      if (!asm_byte(state, 0x25)) {
        return 0;
      }
    } else if (!asm_byte(state, (unsigned)(((reg_field & 7) << 3) | 5))) {
      return 0;
    }
    return asm_emit_displacement(state, line, rm, displacement, 4, 0);
  }

  if (rm->has_index || (rm->has_base && (rm->base & 7) == 4)) {
    need_sib = 1;
  }
  if (rm->has_index && (rm->index & 7) == 4 && rm->scale == 1 &&
      !rm->has_base) {
    asm_fail(state, line, "rsp cannot be a scaled index");
    return 0;
  }

  if (!rm->has_base) {
    mod = 0;
    displacement_bytes = 4;
  } else if (rm->symbol) {
    mod = 2;
    displacement_bytes = 4;
  } else if (displacement == 0 && (rm->base & 7) != 5) {
    mod = 0;
    displacement_bytes = 0;
  } else if (asm_fits_signed(displacement, 1)) {
    mod = 1;
    displacement_bytes = 1;
  } else {
    mod = 2;
    displacement_bytes = 4;
  }

  rm_field = need_sib ? 4 : (rm->base & 7);
  if (!asm_byte(state,
                (unsigned)((mod << 6) | ((reg_field & 7) << 3) | rm_field))) {
    return 0;
  }
  if (need_sib && !asm_emit_sib(state, line, rm)) {
    return 0;
  }
  if (displacement_bytes == 1) {
    return asm_value(state, displacement, 1);
  }
  if (displacement_bytes == 4) {
    return asm_emit_displacement(state, line, rm, displacement, 4, 0);
  }
  return 1;
}

static int asm_emit_instruction(AsmState *state, int line,
                                const unsigned char *opcode, int opcode_length,
                                int reg_field, int reg_field_is_gp,
                                int reg_high_byte, const X86AsmOperand *rm,
                                int operand_bytes, int mandatory_prefix,
                                int force_rex_w) {
  int rex;
  int needs_rex;
  int forbids_rex;
  int address_bytes = state->bits / 8;
  int i;

  if (rm && rm->kind == X86_ASM_OPERAND_MEM) {
    address_bytes = asm_address_bytes(state, rm);
  }

  rex = asm_rex_flags(rm, reg_field, reg_field_is_gp, reg_high_byte,
                      operand_bytes, force_rex_w, &needs_rex, &forbids_rex);

  if (!asm_emit_prefixes(state, line, rm, address_bytes, operand_bytes,
                         mandatory_prefix)) {
    return 0;
  }

  if (rex || needs_rex) {
    if (state->bits != 64) {
      asm_fail(state, line,
               "registers r8-r15, spl/bpl/sil/dil and 64-bit operands need "
               "64-bit code");
      return 0;
    }
    if (forbids_rex) {
      asm_fail(state, line,
               "ah/ch/dh/bh cannot be combined with an operand that needs a "
               "REX prefix");
      return 0;
    }
    if (!asm_byte(state, 0x40 | rex)) {
      return 0;
    }
  }

  for (i = 0; i < opcode_length; i++) {
    if (!asm_byte(state, opcode[i])) {
      return 0;
    }
  }

  if (!rm) {
    return 1;
  }
  if (rm->kind == X86_ASM_OPERAND_REG) {
    return asm_byte(state, 0xC0 | ((reg_field & 7) << 3) | (rm->reg & 7));
  }
  if (address_bytes == 2) {
    return asm_emit_modrm16(state, line, reg_field, rm);
  }
  return asm_emit_modrm32(state, line, reg_field, rm);
}

static int asm_emit_immediate(AsmState *state, int line,
                              const X86AsmOperand *operand, int bytes) {
  if (operand->symbol) {
    if (!asm_patch(state, operand->symbol, state->size, bytes, 0, operand->imm,
                   line)) {
      return 0;
    }
    return asm_value(state, 0, bytes);
  }
  return asm_value(state, operand->imm, bytes);
}

static int asm_immediate_bytes(int operand_bytes) {
  if (operand_bytes == 1) {
    return 1;
  }
  if (operand_bytes == 2) {
    return 2;
  }
  return 4;
}

static int asm_infer_operand_bytes(AsmState *state, int line,
                                   const X86AsmOperand *operands, int count) {
  int i;
  for (i = 0; i < count; i++) {
    if (operands[i].kind == X86_ASM_OPERAND_REG &&
        operands[i].reg_class == X86_ASM_REG_GP) {
      return operands[i].reg_bytes;
    }
  }
  for (i = 0; i < count; i++) {
    if (operands[i].kind == X86_ASM_OPERAND_MEM && operands[i].mem_bytes) {
      return operands[i].mem_bytes;
    }
  }
  asm_fail(state, line,
           "operand size is ambiguous; write `byte`, `word`, `dword` or "
           "`qword` before the memory operand");
  return 0;
}

static int asm_alu(AsmState *state, int line, int index,
                   X86AsmOperand *operands, int count) {
  int operand_bytes;
  unsigned char opcode[2];
  if (count != 2) {
    asm_fail(state, line, "this instruction takes two operands");
    return 0;
  }
  operand_bytes = asm_infer_operand_bytes(state, line, operands, count);
  if (!operand_bytes) {
    return 0;
  }

  if (operands[1].kind == X86_ASM_OPERAND_IMM) {
    int immediate_bytes = asm_immediate_bytes(operand_bytes);
    if (operand_bytes != 1 && !operands[1].symbol &&
        asm_fits_signed(operands[1].imm, 1)) {
      opcode[0] = 0x83;
      if (!asm_emit_instruction(state, line, opcode, 1, index, 0, 0,
                                &operands[0], operand_bytes, 0, 0)) {
        return 0;
      }
      return asm_emit_immediate(state, line, &operands[1], 1);
    }
    if (operands[0].kind == X86_ASM_OPERAND_REG && operands[0].reg == 0 &&
        operands[0].reg_class == X86_ASM_REG_GP) {
      opcode[0] = (unsigned char)(index * 8 + (operand_bytes == 1 ? 0x04 : 0x05));
      if (!asm_emit_instruction(state, line, opcode, 1, 0, 0, 0, NULL,
                                operand_bytes, 0, 0)) {
        return 0;
      }
      return asm_emit_immediate(state, line, &operands[1], immediate_bytes);
    }
    opcode[0] = (unsigned char)(operand_bytes == 1 ? 0x80 : 0x81);
    if (!asm_emit_instruction(state, line, opcode, 1, index, 0, 0, &operands[0],
                              operand_bytes, 0, 0)) {
      return 0;
    }
    return asm_emit_immediate(state, line, &operands[1], immediate_bytes);
  }

  if (operands[1].kind == X86_ASM_OPERAND_REG) {
    opcode[0] = (unsigned char)(index * 8 + (operand_bytes == 1 ? 0x00 : 0x01));
    return asm_emit_instruction(state, line, opcode, 1, operands[1].reg, 1,
                                operands[1].high_byte, &operands[0],
                                operand_bytes, 0, 0);
  }
  if (operands[0].kind == X86_ASM_OPERAND_REG) {
    opcode[0] = (unsigned char)(index * 8 + (operand_bytes == 1 ? 0x02 : 0x03));
    return asm_emit_instruction(state, line, opcode, 1, operands[0].reg, 1,
                                operands[0].high_byte, &operands[1],
                                operand_bytes, 0, 0);
  }
  asm_fail(state, line, "at least one operand must be a register");
  return 0;
}

static int asm_emit_opcode_plus_register(AsmState *state, int line,
                                         unsigned int base_opcode,
                                         const X86AsmOperand *reg_operand,
                                         int operand_bytes) {
  int rex = 0;
  if (state->bits == 16) {
    if (operand_bytes == 4 && !asm_byte(state, 0x66)) {
      return 0;
    }
    if (operand_bytes == 8) {
      asm_fail(state, line, "64-bit operands are not available in 16-bit code");
      return 0;
    }
  } else {
    if (operand_bytes == 2 && !asm_byte(state, 0x66)) {
      return 0;
    }
    if (operand_bytes == 8 && state->bits != 64) {
      asm_fail(state, line, "64-bit operands are not available in 32-bit code");
      return 0;
    }
  }
  if (operand_bytes == 8) {
    rex |= 0x08;
  }
  if (reg_operand->reg >= 8) {
    rex |= 0x01;
  }
  if (operand_bytes == 1 && !reg_operand->high_byte && reg_operand->reg >= 4 &&
      reg_operand->reg <= 7) {
    rex |= 0x00;
    if (state->bits != 64) {
      asm_fail(state, line, "spl/bpl/sil/dil need 64-bit code");
      return 0;
    }
    if (!asm_byte(state, 0x40)) {
      return 0;
    }
    return asm_byte(state, base_opcode + (unsigned)(reg_operand->reg & 7));
  }
  if (rex) {
    if (state->bits != 64) {
      asm_fail(state, line,
               "registers r8-r15 and 64-bit operands need 64-bit code");
      return 0;
    }
    if (!asm_byte(state, 0x40u | (unsigned)rex)) {
      return 0;
    }
  }
  return asm_byte(state, base_opcode + (unsigned)(reg_operand->reg & 7));
}

static int asm_mov(AsmState *state, int line, X86AsmOperand *operands,
                   int count) {
  int operand_bytes;
  unsigned char opcode[3];

  if (count != 2) {
    asm_fail(state, line, "`mov` takes two operands");
    return 0;
  }

  if (operands[0].kind == X86_ASM_OPERAND_REG &&
      (operands[0].reg_class == X86_ASM_REG_CR ||
       operands[0].reg_class == X86_ASM_REG_DR)) {
    if (operands[1].kind != X86_ASM_OPERAND_REG ||
        operands[1].reg_class != X86_ASM_REG_GP) {
      asm_fail(state, line, "a control or debug register loads from a register");
      return 0;
    }
    opcode[0] = 0x0F;
    opcode[1] = (unsigned char)(operands[0].reg_class == X86_ASM_REG_CR ? 0x22
                                                                        : 0x23);
    return asm_emit_instruction(state, line, opcode, 2, operands[0].reg, 0, 0,
                                &operands[1], state->bits == 64 ? 4 : 4, 0, 0);
  }
  if (operands[1].kind == X86_ASM_OPERAND_REG &&
      (operands[1].reg_class == X86_ASM_REG_CR ||
       operands[1].reg_class == X86_ASM_REG_DR)) {
    if (operands[0].kind != X86_ASM_OPERAND_REG ||
        operands[0].reg_class != X86_ASM_REG_GP) {
      asm_fail(state, line, "a control or debug register stores to a register");
      return 0;
    }
    opcode[0] = 0x0F;
    opcode[1] = (unsigned char)(operands[1].reg_class == X86_ASM_REG_CR ? 0x20
                                                                        : 0x21);
    return asm_emit_instruction(state, line, opcode, 2, operands[1].reg, 0, 0,
                                &operands[0], 4, 0, 0);
  }

  if (operands[0].kind == X86_ASM_OPERAND_REG &&
      operands[0].reg_class == X86_ASM_REG_SEG) {
    opcode[0] = 0x8E;
    return asm_emit_instruction(state, line, opcode, 1, operands[0].reg, 0, 0,
                                &operands[1], 2, 0, 0);
  }
  if (operands[1].kind == X86_ASM_OPERAND_REG &&
      operands[1].reg_class == X86_ASM_REG_SEG) {
    opcode[0] = 0x8C;
    return asm_emit_instruction(state, line, opcode, 1, operands[1].reg, 0, 0,
                                &operands[0], 2, 0, 0);
  }

  operand_bytes = asm_infer_operand_bytes(state, line, operands, count);
  if (!operand_bytes) {
    return 0;
  }

  if (operands[1].kind == X86_ASM_OPERAND_IMM ||
      operands[1].kind == X86_ASM_OPERAND_FAR) {
    if (operands[0].kind == X86_ASM_OPERAND_REG) {
      int immediate_bytes = operand_bytes;
      if (operand_bytes == 8 && !operands[1].symbol &&
          asm_fits_signed(operands[1].imm, 4)) {
        opcode[0] = 0xC7;
        if (!asm_emit_instruction(state, line, opcode, 1, 0, 0, 0, &operands[0],
                                  operand_bytes, 0, 0)) {
          return 0;
        }
        return asm_emit_immediate(state, line, &operands[1], 4);
      }
      if (!asm_emit_opcode_plus_register(
              state, line, operand_bytes == 1 ? 0xB0 : 0xB8, &operands[0],
              operand_bytes)) {
        return 0;
      }
      return asm_emit_immediate(state, line, &operands[1], immediate_bytes);
    }
    opcode[0] = (unsigned char)(operand_bytes == 1 ? 0xC6 : 0xC7);
    if (!asm_emit_instruction(state, line, opcode, 1, 0, 0, 0, &operands[0],
                              operand_bytes, 0, 0)) {
      return 0;
    }
    return asm_emit_immediate(state, line, &operands[1],
                              asm_immediate_bytes(operand_bytes));
  }

  if (operands[1].kind == X86_ASM_OPERAND_REG) {
    opcode[0] = (unsigned char)(operand_bytes == 1 ? 0x88 : 0x89);
    return asm_emit_instruction(state, line, opcode, 1, operands[1].reg, 1,
                                operands[1].high_byte, &operands[0],
                                operand_bytes, 0, 0);
  }
  if (operands[0].kind == X86_ASM_OPERAND_REG) {
    opcode[0] = (unsigned char)(operand_bytes == 1 ? 0x8A : 0x8B);
    return asm_emit_instruction(state, line, opcode, 1, operands[0].reg, 1,
                                operands[0].high_byte, &operands[1],
                                operand_bytes, 0, 0);
  }
  asm_fail(state, line, "`mov` needs at least one register operand");
  return 0;
}

static int asm_shift(AsmState *state, int line, int digit,
                     X86AsmOperand *operands, int count) {
  int operand_bytes;
  unsigned char opcode[1];
  if (count != 2) {
    asm_fail(state, line, "a shift takes a destination and a count");
    return 0;
  }
  operand_bytes = asm_infer_operand_bytes(state, line, operands, 1);
  if (!operand_bytes) {
    return 0;
  }
  if (operands[1].kind == X86_ASM_OPERAND_REG) {
    if (operands[1].reg != 1 || operands[1].reg_bytes != 1) {
      asm_fail(state, line, "a variable shift count must be `cl`");
      return 0;
    }
    opcode[0] = (unsigned char)(operand_bytes == 1 ? 0xD2 : 0xD3);
    return asm_emit_instruction(state, line, opcode, 1, digit, 0, 0,
                                &operands[0], operand_bytes, 0, 0);
  }
  if (operands[1].kind != X86_ASM_OPERAND_IMM) {
    asm_fail(state, line, "a shift count must be `cl` or a constant");
    return 0;
  }
  if (!operands[1].symbol && operands[1].imm == 1) {
    opcode[0] = (unsigned char)(operand_bytes == 1 ? 0xD0 : 0xD1);
    return asm_emit_instruction(state, line, opcode, 1, digit, 0, 0,
                                &operands[0], operand_bytes, 0, 0);
  }
  opcode[0] = (unsigned char)(operand_bytes == 1 ? 0xC0 : 0xC1);
  if (!asm_emit_instruction(state, line, opcode, 1, digit, 0, 0, &operands[0],
                            operand_bytes, 0, 0)) {
    return 0;
  }
  return asm_emit_immediate(state, line, &operands[1], 1);
}

static int asm_group3(AsmState *state, int line, int digit,
                      X86AsmOperand *operands, int count) {
  int operand_bytes;
  unsigned char opcode[1];
  if (count != 1) {
    asm_fail(state, line, "this instruction takes one operand");
    return 0;
  }
  operand_bytes = asm_infer_operand_bytes(state, line, operands, 1);
  if (!operand_bytes) {
    return 0;
  }
  opcode[0] = (unsigned char)(operand_bytes == 1 ? 0xF6 : 0xF7);
  return asm_emit_instruction(state, line, opcode, 1, digit, 0, 0, &operands[0],
                              operand_bytes, 0, 0);
}

static int asm_increment(AsmState *state, int line, int digit,
                         X86AsmOperand *operands, int count) {
  int operand_bytes;
  unsigned char opcode[1];
  if (count != 1) {
    asm_fail(state, line, "this instruction takes one operand");
    return 0;
  }
  operand_bytes = asm_infer_operand_bytes(state, line, operands, 1);
  if (!operand_bytes) {
    return 0;
  }
  if (state->bits != 64 && operands[0].kind == X86_ASM_OPERAND_REG &&
      operand_bytes != 1) {
    opcode[0] = (unsigned char)((digit == 0 ? 0x40 : 0x48) + operands[0].reg);
    return asm_emit_instruction(state, line, opcode, 1, 0, 0, 0, NULL,
                                operand_bytes, 0, 0);
  }
  opcode[0] = (unsigned char)(operand_bytes == 1 ? 0xFE : 0xFF);
  return asm_emit_instruction(state, line, opcode, 1, digit, 0, 0, &operands[0],
                              operand_bytes, 0, 0);
}

static int asm_push_pop(AsmState *state, int line, int is_push,
                        X86AsmOperand *operands, int count) {
  unsigned char opcode[2];
  int stack_bytes = state->bits == 16 ? 2 : (state->bits == 32 ? 4 : 8);
  if (count != 1) {
    asm_fail(state, line, "`push`/`pop` take one operand");
    return 0;
  }
  if (operands[0].kind == X86_ASM_OPERAND_REG &&
      operands[0].reg_class == X86_ASM_REG_SEG) {
    static const unsigned char push_codes[6] = {0x06, 0x0E, 0x16, 0x1E, 0, 0};
    static const unsigned char pop_codes[6] = {0x07, 0, 0x17, 0x1F, 0, 0};
    int segment = operands[0].reg;
    if (segment >= 4) {
      opcode[0] = 0x0F;
      opcode[1] = (unsigned char)(segment == 4 ? (is_push ? 0xA0 : 0xA1)
                                               : (is_push ? 0xA8 : 0xA9));
      return asm_emit_instruction(state, line, opcode, 2, 0, 0, 0, NULL, 0, 0,
                                  0);
    }
    if (state->bits == 64) {
      asm_fail(state, line,
               "pushing or popping this segment register is invalid in 64-bit "
               "code");
      return 0;
    }
    opcode[0] = is_push ? push_codes[segment] : pop_codes[segment];
    if (!opcode[0]) {
      asm_fail(state, line, "that segment register cannot be popped");
      return 0;
    }
    return asm_emit_instruction(state, line, opcode, 1, 0, 0, 0, NULL, 0, 0, 0);
  }

  if (operands[0].kind == X86_ASM_OPERAND_REG) {
    if (operands[0].reg_bytes != stack_bytes && operands[0].reg_bytes != 2) {
      asm_fail(state, line, "`push`/`pop` need a stack-sized register");
      return 0;
    }
    if (operands[0].reg_bytes == 2 && state->bits != 16 &&
        !asm_byte(state, 0x66)) {
      return 0;
    }
    if (operands[0].reg_bytes == 4 && state->bits == 16 &&
        !asm_byte(state, 0x66)) {
      return 0;
    }
    if (operands[0].reg >= 8) {
      if (state->bits != 64) {
        asm_fail(state, line, "registers r8-r15 need 64-bit code");
        return 0;
      }
      if (!asm_byte(state, 0x41)) {
        return 0;
      }
    }
    opcode[0] = (unsigned char)((is_push ? 0x50 : 0x58) + (operands[0].reg & 7));
    return asm_byte(state, opcode[0]);
  }

  if (is_push && operands[0].kind == X86_ASM_OPERAND_IMM) {
    if (!operands[0].symbol && asm_fits_signed(operands[0].imm, 1)) {
      if (!asm_byte(state, 0x6A)) {
        return 0;
      }
      return asm_emit_immediate(state, line, &operands[0], 1);
    }
    if (!asm_byte(state, 0x68)) {
      return 0;
    }
    return asm_emit_immediate(state, line, &operands[0],
                              state->bits == 16 ? 2 : 4);
  }

  if (operands[0].kind == X86_ASM_OPERAND_MEM) {
    opcode[0] = (unsigned char)(is_push ? 0xFF : 0x8F);
    return asm_emit_instruction(state, line, opcode, 1, is_push ? 6 : 0, 0, 0,
                                &operands[0], state->bits == 16 ? 2 : 0, 0, 0);
  }

  asm_fail(state, line, "unsupported `push`/`pop` operand");
  return 0;
}

static int asm_branch(AsmState *state, int line, int opcode_short,
                      const unsigned char *opcode_near, int near_length,
                      X86AsmOperand *operands, int count) {
  int displacement_bytes = state->bits == 16 ? 2 : 4;
  if (count != 1) {
    asm_fail(state, line, "a branch takes one target");
    return 0;
  }
  if (operands[0].kind == X86_ASM_OPERAND_MEM ||
      (operands[0].kind == X86_ASM_OPERAND_REG)) {
    asm_fail(state, line, "this branch cannot take a register or memory target");
    return 0;
  }
  int ordinal = -1;
  int take_short = operands[0].explicit_short || opcode_near == NULL;
  if (!take_short && opcode_short >= 0 && operands[0].symbol) {
    ordinal = state->branch_ordinal++;
    if (state->short_hints && (size_t)ordinal < state->short_hint_count &&
        state->short_hints[ordinal]) {
      take_short = 1;
    }
  }
  if (take_short) {
    if (opcode_short < 0) {
      asm_fail(state, line, "this branch has no short form");
      return 0;
    }
    if (!asm_byte(state, (unsigned)opcode_short)) {
      return 0;
    }
    if (operands[0].symbol) {
      if (!asm_patch(state, operands[0].symbol, state->size, 1, 1,
                     operands[0].imm, line)) {
        return 0;
      }
      state->patches[state->patch_count - 1].branch_ordinal = ordinal;
      return asm_value(state, 0, 1);
    }
    return asm_value(state, operands[0].imm, 1);
  }
  {
    int i;
    for (i = 0; i < near_length; i++) {
      if (!asm_byte(state, opcode_near[i])) {
        return 0;
      }
    }
  }
  if (operands[0].symbol) {
    if (!asm_patch(state, operands[0].symbol, state->size, displacement_bytes,
                   1, operands[0].imm, line)) {
      return 0;
    }
    state->patches[state->patch_count - 1].branch_ordinal = ordinal;
    return asm_value(state, 0, displacement_bytes);
  }
  return asm_value(state, operands[0].imm, displacement_bytes);
}

static int asm_string_operation(AsmState *state, int line, const char *name,
                                unsigned char byte_opcode,
                                unsigned char wide_opcode, int width) {
  unsigned char opcode[1];
  (void)name;
  if (width == 1) {
    opcode[0] = byte_opcode;
    return asm_emit_instruction(state, line, opcode, 1, 0, 0, 0, NULL, 0, 0, 0);
  }
  opcode[0] = wide_opcode;
  return asm_emit_instruction(state, line, opcode, 1, 0, 0, 0, NULL, width, 0,
                              0);
}

typedef struct {
  const char *name;
  unsigned char bytes[4];
  int length;
} AsmSimpleInstruction;

static const AsmSimpleInstruction ASM_SIMPLE[] = {
    {"nop", {0x90}, 1},        {"hlt", {0xF4}, 1},
    {"cli", {0xFA}, 1},        {"sti", {0xFB}, 1},
    {"cld", {0xFC}, 1},        {"std", {0xFD}, 1},
    {"clc", {0xF8}, 1},        {"stc", {0xF9}, 1},
    {"cmc", {0xF5}, 1},        {"leave", {0xC9}, 1},
    {"int3", {0xCC}, 1},       {"into", {0xCE}, 1},
    {"sahf", {0x9E}, 1},       {"lahf", {0x9F}, 1},
    {"xlatb", {0xD7}, 1},      {"ud2", {0x0F, 0x0B}, 2},
    {"syscall", {0x0F, 0x05}, 2},  {"sysret", {0x0F, 0x07}, 2},
    {"sysenter", {0x0F, 0x34}, 2}, {"sysexit", {0x0F, 0x35}, 2},
    {"cpuid", {0x0F, 0xA2}, 2},    {"rdtsc", {0x0F, 0x31}, 2},
    {"rdmsr", {0x0F, 0x32}, 2},    {"wrmsr", {0x0F, 0x30}, 2},
    {"rdpmc", {0x0F, 0x33}, 2},    {"clts", {0x0F, 0x06}, 2},
    {"invd", {0x0F, 0x08}, 2},     {"wbinvd", {0x0F, 0x09}, 2},
    {"rdtscp", {0x0F, 0x01, 0xF9}, 3},
    {"swapgs", {0x0F, 0x01, 0xF8}, 3},
    {"xgetbv", {0x0F, 0x01, 0xD0}, 3},
    {"xsetbv", {0x0F, 0x01, 0xD1}, 3},
    {"monitor", {0x0F, 0x01, 0xC8}, 3},
    {"mwait", {0x0F, 0x01, 0xC9}, 3},
    {"vmcall", {0x0F, 0x01, 0xC1}, 3},
    {"mfence", {0x0F, 0xAE, 0xF0}, 3},
    {"lfence", {0x0F, 0xAE, 0xE8}, 3},
    {"sfence", {0x0F, 0xAE, 0xF8}, 3},
    {"pause", {0xF3, 0x90}, 2},
    {"emms", {0x0F, 0x77}, 2},
    {"endbr64", {0xF3, 0x0F, 0x1E, 0xFA}, 4},
    {"endbr32", {0xF3, 0x0F, 0x1E, 0xFB}, 4},
};

typedef struct {
  const char *name;
  unsigned char prefix;
  unsigned char opcode;
  int store_direction;
} AsmSseInstruction;

static const AsmSseInstruction ASM_SSE[] = {
    {"movups", 0x00, 0x10, 0},  {"movupd", 0x66, 0x10, 0},
    {"movss", 0xF3, 0x10, 0},   {"movsd", 0xF2, 0x10, 0},
    {"movaps", 0x00, 0x28, 0},  {"movapd", 0x66, 0x28, 0},
    {"movdqa", 0x66, 0x6F, 0},  {"movdqu", 0xF3, 0x6F, 0},
    {"addps", 0x00, 0x58, 0},   {"addpd", 0x66, 0x58, 0},
    {"addss", 0xF3, 0x58, 0},   {"addsd", 0xF2, 0x58, 0},
    {"mulps", 0x00, 0x59, 0},   {"mulpd", 0x66, 0x59, 0},
    {"mulss", 0xF3, 0x59, 0},   {"mulsd", 0xF2, 0x59, 0},
    {"subps", 0x00, 0x5C, 0},   {"subpd", 0x66, 0x5C, 0},
    {"subss", 0xF3, 0x5C, 0},   {"subsd", 0xF2, 0x5C, 0},
    {"divps", 0x00, 0x5E, 0},   {"divpd", 0x66, 0x5E, 0},
    {"divss", 0xF3, 0x5E, 0},   {"divsd", 0xF2, 0x5E, 0},
    {"minss", 0xF3, 0x5D, 0},   {"minsd", 0xF2, 0x5D, 0},
    {"maxss", 0xF3, 0x5F, 0},   {"maxsd", 0xF2, 0x5F, 0},
    {"sqrtss", 0xF3, 0x51, 0},  {"sqrtsd", 0xF2, 0x51, 0},
    {"sqrtps", 0x00, 0x51, 0},  {"sqrtpd", 0x66, 0x51, 0},
    {"xorps", 0x00, 0x57, 0},   {"xorpd", 0x66, 0x57, 0},
    {"andps", 0x00, 0x54, 0},   {"andpd", 0x66, 0x54, 0},
    {"orps", 0x00, 0x56, 0},    {"orpd", 0x66, 0x56, 0},
    {"pxor", 0x66, 0xEF, 0},    {"pand", 0x66, 0xDB, 0},
    {"por", 0x66, 0xEB, 0},     {"paddb", 0x66, 0xFC, 0},
    {"paddw", 0x66, 0xFD, 0},   {"paddd", 0x66, 0xFE, 0},
    {"paddq", 0x66, 0xD4, 0},   {"psubb", 0x66, 0xF8, 0},
    {"psubw", 0x66, 0xF9, 0},   {"psubd", 0x66, 0xFA, 0},
    {"ucomiss", 0x00, 0x2E, 0}, {"ucomisd", 0x66, 0x2E, 0},
    {"comiss", 0x00, 0x2F, 0},  {"comisd", 0x66, 0x2F, 0},
};

static int asm_directive_data(AsmState *state, int line, int width);

static int asm_encode_mnemonic(AsmState *state, int line, const char *mnemonic,
                               X86AsmOperand *operands, int count);

static int asm_parse_operand_list(AsmState *state, X86AsmOperand *operands,
                                  int *count) {
  AsmLexer *lexer = state->lexer;
  *count = 0;
  if (lexer->current.type == AT_NEWLINE || lexer->current.type == AT_END) {
    return 1;
  }
  for (;;) {
    if (*count == X86_ASM_MAX_OPERANDS) {
      asm_fail(state, lexer->current.line, "too many operands");
      return 0;
    }
    if (!asm_parse_operand(state, &operands[*count])) {
      return 0;
    }
    (*count)++;
    if (lexer->current.type == AT_PUNCT && lexer->current.punct == ',') {
      asm_advance(lexer);
      continue;
    }
    break;
  }
  if (lexer->current.type != AT_NEWLINE && lexer->current.type != AT_END) {
    asm_fail(state, lexer->current.line, "unexpected `%s` after the operands",
             lexer->current.text);
    return 0;
  }
  return 1;
}

static int asm_statement(AsmState *state);

static int asm_directive_data(AsmState *state, int line, int width) {
  AsmLexer *lexer = state->lexer;
  for (;;) {
    if (lexer->current.type == AT_STRING) {
      size_t i;
      for (i = 0; i < lexer->current.length; i++) {
        if (!asm_value(state, (unsigned char)lexer->current.text[i], width)) {
          return 0;
        }
      }
      asm_advance(lexer);
    } else {
      X86AsmOperand operand;
      if (!asm_parse_operand(state, &operand)) {
        return 0;
      }
      if (operand.kind != X86_ASM_OPERAND_IMM) {
        asm_operand_release(&operand);
        asm_fail(state, line, "a data directive takes constants or strings");
        return 0;
      }
      if (operand.symbol) {
        if (!asm_patch(state, operand.symbol, state->size, width, 0,
                       operand.imm, line)) {
          asm_operand_release(&operand);
          return 0;
        }
        if (!asm_value(state, 0, width)) {
          asm_operand_release(&operand);
          return 0;
        }
      } else if (!asm_value(state, operand.imm, width)) {
        asm_operand_release(&operand);
        return 0;
      }
      asm_operand_release(&operand);
    }
    if (lexer->current.type == AT_PUNCT && lexer->current.punct == ',') {
      asm_advance(lexer);
      continue;
    }
    break;
  }
  return 1;
}

static int asm_far_branch(AsmState *state, int line, int is_call,
                          X86AsmOperand *operands, int count) {
  int offset_bytes = state->bits == 16 ? 2 : 4;
  if (count != 1 || operands[0].kind != X86_ASM_OPERAND_FAR) {
    asm_fail(state, line, "a far branch takes `selector:offset`");
    return 0;
  }
  if (state->bits == 64) {
    asm_fail(state, line, "a direct far branch is invalid in 64-bit code");
    return 0;
  }
  if (!asm_byte(state, is_call ? 0x9A : 0xEA)) {
    return 0;
  }
  if (operands[0].symbol) {
    if (!asm_patch(state, operands[0].symbol, state->size, offset_bytes, 0,
                   operands[0].imm, line)) {
      return 0;
    }
    if (!asm_value(state, 0, offset_bytes)) {
      return 0;
    }
  } else if (!asm_value(state, operands[0].imm, offset_bytes)) {
    return 0;
  }
  return asm_value(state, operands[0].far_segment, 2);
}


typedef struct {
  const char *name;
  int digit;
} AsmDigitInstruction;

static const AsmDigitInstruction ASM_GROUP3[] = {
    {"not", 2}, {"neg", 3}, {"mul", 4}, {"div", 6}, {"idiv", 7}};

static const AsmDigitInstruction ASM_BIT_TEST[] = {
    {"bt", 4}, {"bts", 5}, {"btr", 6}, {"btc", 7}};

static const AsmDigitInstruction ASM_DESCRIPTOR[] = {
    {"sgdt", 0}, {"sidt", 1}, {"lgdt", 2},   {"lidt", 3},
    {"smsw", 4}, {"lmsw", 6}, {"invlpg", 7}};

static const AsmDigitInstruction ASM_SEGMENT_REGISTER[] = {
    {"sldt", 0}, {"str", 1},  {"lldt", 2},
    {"ltr", 3},  {"verr", 4}, {"verw", 5}};

static const AsmDigitInstruction ASM_SAVE_STATE[] = {
    {"fxsave", 0}, {"fxrstor", 1}, {"xsave", 4}, {"xrstor", 5}, {"clflush", 7}};

static const AsmDigitInstruction ASM_PREFETCH_HINT[] = {
    {"nta", 0}, {"t0", 1}, {"t1", 2}, {"t2", 3}};

typedef struct {
  const char *name;
  unsigned char opcode;
  int width;
} AsmWidthInstruction;

static const AsmWidthInstruction ASM_WIDTH[] = {
    {"cbw", 0x98, 16},  {"cwde", 0x98, 32},  {"cdqe", 0x98, 64},
    {"cwd", 0x99, 16},  {"cdq", 0x99, 32},   {"cqo", 0x99, 64},
    {"iret", 0xCF, 16}, {"iretw", 0xCF, 16}, {"iretd", 0xCF, 32},
    {"iretq", 0xCF, 64}};

typedef struct {
  const char *name;
  unsigned char opcode;
} AsmCounterBranch;

static const AsmCounterBranch ASM_COUNTER_BRANCH[] = {
    {"loop", 0xE2},   {"loope", 0xE1}, {"loopz", 0xE1},  {"loopne", 0xE0},
    {"loopnz", 0xE0}, {"jcxz", 0xE3},  {"jecxz", 0xE3},  {"jrcxz", 0xE3}};

typedef struct {
  const char *name;
  unsigned char opcode;
  unsigned char prefix;
} AsmBitScan;

static const AsmBitScan ASM_BIT_SCAN[] = {
    {"bsf", 0xBC, 0},      {"bsr", 0xBD, 0}, {"popcnt", 0xB8, 0xF3},
    {"lzcnt", 0xBD, 0xF3}, {"tzcnt", 0xBC, 0xF3}};

typedef struct {
  const char *name;
  unsigned char byte_opcode;
  unsigned char wide_opcode;
  int width;
  int bare_only;
} AsmStringInstruction;

static const AsmStringInstruction ASM_STRING[] = {
    {"movsb", 0xA4, 0xA5, 1, 0}, {"movsw", 0xA4, 0xA5, 2, 0},
    {"movsd", 0xA4, 0xA5, 4, 1}, {"movsq", 0xA4, 0xA5, 8, 0},
    {"stosb", 0xAA, 0xAB, 1, 0}, {"stosw", 0xAA, 0xAB, 2, 0},
    {"stosd", 0xAA, 0xAB, 4, 0}, {"stosq", 0xAA, 0xAB, 8, 0},
    {"lodsb", 0xAC, 0xAD, 1, 0}, {"lodsw", 0xAC, 0xAD, 2, 0},
    {"lodsd", 0xAC, 0xAD, 4, 0}, {"lodsq", 0xAC, 0xAD, 8, 0},
    {"scasb", 0xAE, 0xAF, 1, 0}, {"scasw", 0xAE, 0xAF, 2, 0},
    {"scasd", 0xAE, 0xAF, 4, 0}, {"scasq", 0xAE, 0xAF, 8, 0},
    {"cmpsb", 0xA6, 0xA7, 1, 0}, {"cmpsw", 0xA6, 0xA7, 2, 0},
    {"cmpsd", 0xA6, 0xA7, 4, 1}, {"cmpsq", 0xA6, 0xA7, 8, 0},
    {"insb", 0x6C, 0x6D, 1, 0},  {"insw", 0x6C, 0x6D, 2, 0},
    {"insd", 0x6C, 0x6D, 4, 0},  {"outsb", 0x6E, 0x6F, 1, 0},
    {"outsw", 0x6E, 0x6F, 2, 0}, {"outsd", 0x6E, 0x6F, 4, 0}};

static int asm_encode_width_op(AsmState *state, int line, const char *mnemonic,
                               unsigned char opcode, int width) {
  if (width == 64) {
    if (state->bits != 64) {
      asm_fail(state, line, "`%s` needs 64-bit code", mnemonic);
      return 0;
    }
    if (!asm_byte(state, 0x48)) {
      return 0;
    }
    return asm_byte(state, opcode);
  }
  if ((width == 16) != (state->bits == 16) && !asm_byte(state, 0x66)) {
    return 0;
  }
  return asm_byte(state, opcode);
}

static int asm_encode_one_operand_digit(AsmState *state, int line,
                                        const char *mnemonic, int digit,
                                        unsigned char second, int operand_bytes,
                                        X86AsmOperand *operands, int count) {
  unsigned char opcode[2];
  if (count != 1) {
    asm_fail(state, line, "`%s` takes one operand", mnemonic);
    return 0;
  }
  opcode[0] = 0x0F;
  opcode[1] = second;
  return asm_emit_instruction(state, line, opcode, 2, digit, 0, 0, &operands[0],
                              operand_bytes, 0, 0);
}

static int asm_encode_port(AsmState *state, int line, int is_in,
                           const char *mnemonic, X86AsmOperand *operands,
                           int count) {
  const X86AsmOperand *port = is_in ? &operands[1] : &operands[0];
  const X86AsmOperand *accumulator = is_in ? &operands[0] : &operands[1];
  if (count != 2 || accumulator->kind != X86_ASM_OPERAND_REG ||
      accumulator->reg != 0) {
    asm_fail(state, line, "`%s` uses al, ax or eax", mnemonic);
    return 0;
  }
  if (accumulator->reg_bytes == 2 && state->bits != 16 &&
      !asm_byte(state, 0x66)) {
    return 0;
  }
  if (accumulator->reg_bytes == 4 && state->bits == 16 &&
      !asm_byte(state, 0x66)) {
    return 0;
  }
  if (port->kind == X86_ASM_OPERAND_REG) {
    if (port->reg != 2 || port->reg_bytes != 2) {
      asm_fail(state, line, "a variable port must be `dx`");
      return 0;
    }
    return asm_byte(state, (unsigned)((is_in ? 0xEC : 0xEE) +
                                      (accumulator->reg_bytes == 1 ? 0 : 1)));
  }
  if (!asm_byte(state, (unsigned)((is_in ? 0xE4 : 0xE6) +
                                  (accumulator->reg_bytes == 1 ? 0 : 1)))) {
    return 0;
  }
  return asm_emit_immediate(state, line, port, 1);
}

static int asm_encode_memory_digit(AsmState *state, int line,
                                   const char *mnemonic, int digit,
                                   unsigned char second,
                                   X86AsmOperand *operands, int count) {
  unsigned char opcode[2];
  if (count != 1 || operands[0].kind != X86_ASM_OPERAND_MEM) {
    asm_fail(state, line, "`%s` takes a memory operand", mnemonic);
    return 0;
  }
  opcode[0] = 0x0F;
  opcode[1] = second;
  return asm_emit_instruction(state, line, opcode, 2, digit, 0, 0, &operands[0],
                              0, 0, 0);
}

static int asm_encode_arithmetic(AsmState *state, int line,
                         const char *mnemonic,
                         X86AsmOperand *operands, int count,
                         int *handled) {
  static const char *ALU_NAMES[8] = {"add", "or",  "adc", "sbb",
                                     "and", "sub", "xor", "cmp"};
  static const char *SHIFT_NAMES[8] = {"rol", "ror", "rcl", "rcr",
                                       "shl", "shr", NULL,  "sar"};
  unsigned char opcode[4];
  size_t i;
  *handled = 1;

  for (i = 0; i < 8; i++) {
    if (strcmp(mnemonic, ALU_NAMES[i]) == 0) {
      return asm_alu(state, line, (int)i, operands, count);
    }
  }
  for (i = 0; i < 8; i++) {
    if (SHIFT_NAMES[i] && strcmp(mnemonic, SHIFT_NAMES[i]) == 0) {
      return asm_shift(state, line, (int)i, operands, count);
    }
  }
  if (strcmp(mnemonic, "sal") == 0) {
    return asm_shift(state, line, 4, operands, count);
  }

  for (i = 0; i < sizeof(ASM_GROUP3) / sizeof(ASM_GROUP3[0]); i++) {
    if (strcmp(mnemonic, ASM_GROUP3[i].name) == 0) {
      return asm_group3(state, line, ASM_GROUP3[i].digit, operands, count);
    }
  }

  if (strcmp(mnemonic, "imul") == 0) {
    if (count == 1) {
      return asm_group3(state, line, 5, operands, count);
    }
    if (count == 2) {
      if (operands[0].kind != X86_ASM_OPERAND_REG) {
        asm_fail(state, line, "`imul` needs a register destination");
        return 0;
      }
      opcode[0] = 0x0F;
      opcode[1] = 0xAF;
      return asm_emit_instruction(state, line, opcode, 2, operands[0].reg, 0, 0,
                                  &operands[1], operands[0].reg_bytes, 0, 0);
    }
    if (count == 3) {
      if (operands[0].kind != X86_ASM_OPERAND_REG ||
          operands[2].kind != X86_ASM_OPERAND_IMM) {
        asm_fail(state, line,
                 "three-operand `imul` takes a register, an operand and a "
                 "constant");
        return 0;
      }
      if (!operands[2].symbol && asm_fits_signed(operands[2].imm, 1)) {
        opcode[0] = 0x6B;
        if (!asm_emit_instruction(state, line, opcode, 1, operands[0].reg, 0, 0,
                                  &operands[1], operands[0].reg_bytes, 0, 0)) {
          return 0;
        }
        return asm_emit_immediate(state, line, &operands[2], 1);
      }
      opcode[0] = 0x69;
      if (!asm_emit_instruction(state, line, opcode, 1, operands[0].reg, 0, 0,
                                &operands[1], operands[0].reg_bytes, 0, 0)) {
        return 0;
      }
      return asm_emit_immediate(state, line, &operands[2],
                                asm_immediate_bytes(operands[0].reg_bytes));
    }
    asm_fail(state, line, "`imul` takes one, two or three operands");
    return 0;
  }

  if (strcmp(mnemonic, "inc") == 0) {
    return asm_increment(state, line, 0, operands, count);
  }
  if (strcmp(mnemonic, "dec") == 0) {
    return asm_increment(state, line, 1, operands, count);
  }
  *handled = 0;
  return 0;
}

static int asm_encode_move(AsmState *state, int line,
                         const char *mnemonic,
                         X86AsmOperand *operands, int count,
                         int *handled) {
  unsigned char opcode[4];
  *handled = 1;

  if (strcmp(mnemonic, "mov") == 0) {
    return asm_mov(state, line, operands, count);
  }
  if (strcmp(mnemonic, "movabs") == 0) {
    int operand_bytes;
    if (count != 2 || operands[0].kind != X86_ASM_OPERAND_REG) {
      asm_fail(state, line, "`movabs` takes a register and a 64-bit constant");
      return 0;
    }
    operand_bytes = operands[0].reg_bytes;
    if (!asm_emit_opcode_plus_register(state, line, 0xB8, &operands[0],
                                       operand_bytes)) {
      return 0;
    }
    return asm_emit_immediate(state, line, &operands[1], operand_bytes);
  }

  if (strcmp(mnemonic, "lea") == 0) {
    if (count != 2 || operands[0].kind != X86_ASM_OPERAND_REG ||
        operands[1].kind != X86_ASM_OPERAND_MEM) {
      asm_fail(state, line, "`lea` takes a register and a memory operand");
      return 0;
    }
    opcode[0] = 0x8D;
    return asm_emit_instruction(state, line, opcode, 1, operands[0].reg, 0, 0,
                                &operands[1], operands[0].reg_bytes, 0, 0);
  }

  if (strcmp(mnemonic, "movzx") == 0 || strcmp(mnemonic, "movsx") == 0) {
    int source_bytes;
    if (count != 2 || operands[0].kind != X86_ASM_OPERAND_REG) {
      asm_fail(state, line, "`%s` takes a register destination", mnemonic);
      return 0;
    }
    if (operands[1].kind == X86_ASM_OPERAND_REG) {
      source_bytes = operands[1].reg_bytes;
    } else if (operands[1].mem_bytes) {
      source_bytes = operands[1].mem_bytes;
    } else {
      asm_fail(state, line,
               "`%s` needs `byte` or `word` before the memory operand",
               mnemonic);
      return 0;
    }
    if (source_bytes != 1 && source_bytes != 2) {
      asm_fail(state, line, "`%s` widens from a byte or a word", mnemonic);
      return 0;
    }
    opcode[0] = 0x0F;
    opcode[1] = (unsigned char)((mnemonic[3] == 'z' ? 0xB6 : 0xBE) +
                                (source_bytes == 2 ? 1 : 0));
    return asm_emit_instruction(state, line, opcode, 2, operands[0].reg, 0, 0,
                                &operands[1], operands[0].reg_bytes, 0, 0);
  }
  if (strcmp(mnemonic, "movsxd") == 0) {
    if (count != 2 || operands[0].kind != X86_ASM_OPERAND_REG) {
      asm_fail(state, line, "`movsxd` takes a register destination");
      return 0;
    }
    opcode[0] = 0x63;
    return asm_emit_instruction(state, line, opcode, 1, operands[0].reg, 0, 0,
                                &operands[1], operands[0].reg_bytes, 0, 0);
  }

  if (strcmp(mnemonic, "movbe") == 0) {
    int to_register;
    if (count != 2) {
      asm_fail(state, line, "`movbe` takes two operands");
      return 0;
    }
    to_register = operands[0].kind == X86_ASM_OPERAND_REG;
    if (to_register == (operands[1].kind == X86_ASM_OPERAND_REG)) {
      asm_fail(state, line, "`movbe` moves between a register and memory");
      return 0;
    }
    opcode[0] = 0x0F;
    opcode[1] = 0x38;
    opcode[2] = (unsigned char)(to_register ? 0xF0 : 0xF1);
    return asm_emit_instruction(
        state, line, opcode, 3, to_register ? operands[0].reg : operands[1].reg,
        0, 0, to_register ? &operands[1] : &operands[0],
        to_register ? operands[0].reg_bytes : operands[1].reg_bytes, 0, 0);
  }
  *handled = 0;
  return 0;
}

static int asm_encode_test_exchange(AsmState *state, int line,
                         const char *mnemonic,
                         X86AsmOperand *operands, int count,
                         int *handled) {
  unsigned char opcode[4];
  *handled = 1;

  if (strcmp(mnemonic, "test") == 0) {
    int operand_bytes;
    if (count != 2) {
      asm_fail(state, line, "`test` takes two operands");
      return 0;
    }
    operand_bytes = asm_infer_operand_bytes(state, line, operands, count);
    if (!operand_bytes) {
      return 0;
    }
    if (operands[1].kind == X86_ASM_OPERAND_IMM) {
      if (operands[0].kind == X86_ASM_OPERAND_REG && operands[0].reg == 0 &&
          operands[0].reg_class == X86_ASM_REG_GP && !operands[0].high_byte) {
        opcode[0] = (unsigned char)(operand_bytes == 1 ? 0xA8 : 0xA9);
        if (!asm_emit_instruction(state, line, opcode, 1, 0, 0, 0, NULL,
                                  operand_bytes, 0, 0)) {
          return 0;
        }
        return asm_emit_immediate(state, line, &operands[1],
                                  asm_immediate_bytes(operand_bytes));
      }
      opcode[0] = (unsigned char)(operand_bytes == 1 ? 0xF6 : 0xF7);
      if (!asm_emit_instruction(state, line, opcode, 1, 0, 0, 0, &operands[0],
                                operand_bytes, 0, 0)) {
        return 0;
      }
      return asm_emit_immediate(state, line, &operands[1],
                                asm_immediate_bytes(operand_bytes));
    }
    if (operands[1].kind == X86_ASM_OPERAND_REG) {
      opcode[0] = (unsigned char)(operand_bytes == 1 ? 0x84 : 0x85);
      return asm_emit_instruction(state, line, opcode, 1, operands[1].reg, 1,
                                  operands[1].high_byte, &operands[0],
                                  operand_bytes, 0, 0);
    }
    if (operands[0].kind == X86_ASM_OPERAND_REG) {
      opcode[0] = (unsigned char)(operand_bytes == 1 ? 0x84 : 0x85);
      return asm_emit_instruction(state, line, opcode, 1, operands[0].reg, 1,
                                  operands[0].high_byte, &operands[1],
                                  operand_bytes, 0, 0);
    }
    asm_fail(state, line, "`test` needs a register operand");
    return 0;
  }

  if (strcmp(mnemonic, "xchg") == 0) {
    int operand_bytes;
    if (count != 2) {
      asm_fail(state, line, "`xchg` takes two operands");
      return 0;
    }
    operand_bytes = asm_infer_operand_bytes(state, line, operands, count);
    if (!operand_bytes) {
      return 0;
    }
    if (operands[0].kind == X86_ASM_OPERAND_REG &&
        operands[1].kind == X86_ASM_OPERAND_REG) {
      opcode[0] = (unsigned char)(operand_bytes == 1 ? 0x86 : 0x87);
      return asm_emit_instruction(state, line, opcode, 1, operands[0].reg, 1,
                                  operands[0].high_byte, &operands[1],
                                  operand_bytes, 0, 0);
    }
    opcode[0] = (unsigned char)(operand_bytes == 1 ? 0x86 : 0x87);
    if (operands[0].kind == X86_ASM_OPERAND_REG) {
      return asm_emit_instruction(state, line, opcode, 1, operands[0].reg, 1,
                                  operands[0].high_byte, &operands[1],
                                  operand_bytes, 0, 0);
    }
    if (operands[1].kind == X86_ASM_OPERAND_REG) {
      return asm_emit_instruction(state, line, opcode, 1, operands[1].reg, 1,
                                  operands[1].high_byte, &operands[0],
                                  operand_bytes, 0, 0);
    }
    asm_fail(state, line, "`xchg` needs a register operand");
    return 0;
  }
  *handled = 0;
  return 0;
}

static int asm_encode_stack(AsmState *state, int line,
                         const char *mnemonic,
                         X86AsmOperand *operands, int count,
                         int *handled) {
  *handled = 1;

  if (strcmp(mnemonic, "push") == 0) {
    return asm_push_pop(state, line, 1, operands, count);
  }
  if (strcmp(mnemonic, "pop") == 0) {
    return asm_push_pop(state, line, 0, operands, count);
  }
  if (strncmp(mnemonic, "pushf", 5) == 0 || strncmp(mnemonic, "popf", 4) == 0) {
    int is_push = mnemonic[1] == 'u';
    const char *suffix = mnemonic + (is_push ? 5 : 4);
    int operand_bits = suffix[0] == 'd'   ? 32
                       : suffix[0] == 'q' ? 64
                       : suffix[0] == '\0' ? 16
                                           : 0;
    if (!operand_bits) {
      asm_fail(state, line, "unknown instruction `%s`", mnemonic);
      return 0;
    }
    if (operand_bits == 32 && state->bits == 64) {
      asm_fail(state, line, "`%s` is invalid in 64-bit code", mnemonic);
      return 0;
    }
    if (operand_bits == 64 && state->bits != 64) {
      asm_fail(state, line, "`%s` needs 64-bit code", mnemonic);
      return 0;
    }
    if ((operand_bits == 16 && state->bits != 16) ||
        (operand_bits == 32 && state->bits == 16)) {
      if (!asm_byte(state, 0x66)) {
        return 0;
      }
    }
    return asm_byte(state, is_push ? 0x9C : 0x9D);
  }
  if (strcmp(mnemonic, "pusha") == 0 || strcmp(mnemonic, "pushad") == 0) {
    if (state->bits == 64) {
      asm_fail(state, line, "`pusha` is invalid in 64-bit code");
      return 0;
    }
    return asm_byte(state, 0x60);
  }
  if (strcmp(mnemonic, "popa") == 0 || strcmp(mnemonic, "popad") == 0) {
    if (state->bits == 64) {
      asm_fail(state, line, "`popa` is invalid in 64-bit code");
      return 0;
    }
    return asm_byte(state, 0x61);
  }

  if (strcmp(mnemonic, "enter") == 0) {
    if (count != 2) {
      asm_fail(state, line, "`enter` takes a frame size and a nesting level");
      return 0;
    }
    if (!asm_byte(state, 0xC8)) {
      return 0;
    }
    if (!asm_emit_immediate(state, line, &operands[0], 2)) {
      return 0;
    }
    return asm_emit_immediate(state, line, &operands[1], 1);
  }
  *handled = 0;
  return 0;
}

static int asm_encode_width(AsmState *state, int line,
                         const char *mnemonic,
                         X86AsmOperand *operands, int count,
                         int *handled) {
  size_t i;
  *handled = 1;

  for (i = 0; i < sizeof(ASM_WIDTH) / sizeof(ASM_WIDTH[0]); i++) {
    if (strcmp(mnemonic, ASM_WIDTH[i].name) == 0) {
      return asm_encode_width_op(state, line, mnemonic, ASM_WIDTH[i].opcode,
                                 ASM_WIDTH[i].width);
    }
  }

  if (strcmp(mnemonic, "ret") == 0 || strcmp(mnemonic, "retn") == 0) {
    if (count == 0) {
      return asm_byte(state, 0xC3);
    }
    if (!asm_byte(state, 0xC2)) {
      return 0;
    }
    return asm_emit_immediate(state, line, &operands[0], 2);
  }
  if (strcmp(mnemonic, "retf") == 0) {
    if (count == 0) {
      return asm_byte(state, 0xCB);
    }
    if (!asm_byte(state, 0xCA)) {
      return 0;
    }
    return asm_emit_immediate(state, line, &operands[0], 2);
  }

  if (strcmp(mnemonic, "int") == 0) {
    if (count != 1 || operands[0].kind != X86_ASM_OPERAND_IMM) {
      asm_fail(state, line, "`int` takes a constant vector");
      return 0;
    }
    if (!operands[0].symbol && operands[0].imm == 3) {
      return asm_byte(state, 0xCC);
    }
    if (!asm_byte(state, 0xCD)) {
      return 0;
    }
    return asm_emit_immediate(state, line, &operands[0], 1);
  }
  *handled = 0;
  return 0;
}

static int asm_encode_branch(AsmState *state, int line,
                         const char *mnemonic,
                         X86AsmOperand *operands, int count,
                         int *handled) {
  unsigned char opcode[4];
  size_t i;
  *handled = 1;

  if (strcmp(mnemonic, "jmp") == 0) {
    if (count == 1 && operands[0].kind == X86_ASM_OPERAND_FAR) {
      return asm_far_branch(state, line, 0, operands, count);
    }
    if (count == 1 && (operands[0].kind == X86_ASM_OPERAND_REG ||
                       operands[0].kind == X86_ASM_OPERAND_MEM)) {
      opcode[0] = 0xFF;
      return asm_emit_instruction(state, line, opcode, 1, 4, 0, 0, &operands[0],
                                  state->bits == 16 ? 2 : 0, 0, 0);
    }
    opcode[0] = 0xE9;
    return asm_branch(state, line, 0xEB, opcode, 1, operands, count);
  }
  if (strcmp(mnemonic, "call") == 0) {
    if (count == 1 && operands[0].kind == X86_ASM_OPERAND_FAR) {
      return asm_far_branch(state, line, 1, operands, count);
    }
    if (count == 1 && (operands[0].kind == X86_ASM_OPERAND_REG ||
                       operands[0].kind == X86_ASM_OPERAND_MEM)) {
      opcode[0] = 0xFF;
      return asm_emit_instruction(state, line, opcode, 1, 2, 0, 0, &operands[0],
                                  state->bits == 16 ? 2 : 0, 0, 0);
    }
    opcode[0] = 0xE8;
    return asm_branch(state, line, -1, opcode, 1, operands, count);
  }

  for (i = 0; i < sizeof(ASM_COUNTER_BRANCH) / sizeof(ASM_COUNTER_BRANCH[0]);
       i++) {
    if (strcmp(mnemonic, ASM_COUNTER_BRANCH[i].name) == 0) {
      return asm_branch(state, line, ASM_COUNTER_BRANCH[i].opcode, NULL, 0,
                        operands, count);
    }
  }

  if (mnemonic[0] == 'j') {
    int condition = asm_condition_code(mnemonic + 1);
    if (condition >= 0) {
      opcode[0] = 0x0F;
      opcode[1] = (unsigned char)(0x80 + condition);
      return asm_branch(state, line, 0x70 + condition, opcode, 2, operands,
                        count);
    }
  }
  if (strncmp(mnemonic, "set", 3) == 0) {
    int condition = asm_condition_code(mnemonic + 3);
    if (condition >= 0) {
      if (count != 1) {
        asm_fail(state, line, "`%s` takes one 8-bit operand", mnemonic);
        return 0;
      }
      opcode[0] = 0x0F;
      opcode[1] = (unsigned char)(0x90 + condition);
      return asm_emit_instruction(state, line, opcode, 2, 0, 0, 0, &operands[0],
                                  1, 0, 0);
    }
  }
  if (strncmp(mnemonic, "cmov", 4) == 0) {
    int condition = asm_condition_code(mnemonic + 4);
    if (condition >= 0) {
      if (count != 2 || operands[0].kind != X86_ASM_OPERAND_REG) {
        asm_fail(state, line, "`%s` takes a register and an operand", mnemonic);
        return 0;
      }
      opcode[0] = 0x0F;
      opcode[1] = (unsigned char)(0x40 + condition);
      return asm_emit_instruction(state, line, opcode, 2, operands[0].reg, 0, 0,
                                  &operands[1], operands[0].reg_bytes, 0, 0);
    }
  }
  *handled = 0;
  return 0;
}

static int asm_encode_bit(AsmState *state, int line,
                         const char *mnemonic,
                         X86AsmOperand *operands, int count,
                         int *handled) {
  unsigned char opcode[4];
  size_t i;
  *handled = 1;

  for (i = 0; i < sizeof(ASM_BIT_SCAN) / sizeof(ASM_BIT_SCAN[0]); i++) {
    if (strcmp(mnemonic, ASM_BIT_SCAN[i].name) != 0) {
      continue;
    }
    if (count != 2 || operands[0].kind != X86_ASM_OPERAND_REG) {
      asm_fail(state, line, "`%s` takes a register and an operand", mnemonic);
      return 0;
    }
    opcode[0] = 0x0F;
    opcode[1] = ASM_BIT_SCAN[i].opcode;
    return asm_emit_instruction(state, line, opcode, 2, operands[0].reg, 0, 0,
                                &operands[1], operands[0].reg_bytes,
                                ASM_BIT_SCAN[i].prefix, 0);
  }

  for (i = 0; i < sizeof(ASM_BIT_TEST) / sizeof(ASM_BIT_TEST[0]); i++) {
    int digit;
    int operand_bytes;
    if (strcmp(mnemonic, ASM_BIT_TEST[i].name) != 0) {
      continue;
    }
    digit = ASM_BIT_TEST[i].digit;
    if (count != 2) {
      asm_fail(state, line, "`%s` takes two operands", mnemonic);
      return 0;
    }
    operand_bytes = asm_infer_operand_bytes(state, line, operands, count);
    if (!operand_bytes) {
      return 0;
    }
    opcode[0] = 0x0F;
    if (operands[1].kind == X86_ASM_OPERAND_IMM) {
      opcode[1] = 0xBA;
      if (!asm_emit_instruction(state, line, opcode, 2, digit, 0, 0,
                                &operands[0], operand_bytes, 0, 0)) {
        return 0;
      }
      return asm_emit_immediate(state, line, &operands[1], 1);
    }
    opcode[1] = (unsigned char)(0xA3 + (digit - 4) * 8);
    return asm_emit_instruction(state, line, opcode, 2, operands[1].reg, 0, 0,
                                &operands[0], operand_bytes, 0, 0);
  }

  if (strcmp(mnemonic, "bswap") == 0) {
    int rex = 0;
    if (count != 1 || operands[0].kind != X86_ASM_OPERAND_REG ||
        operands[0].reg_class != X86_ASM_REG_GP ||
        (operands[0].reg_bytes != 4 && operands[0].reg_bytes != 8)) {
      asm_fail(state, line, "`bswap` takes a 32- or 64-bit register");
      return 0;
    }
    if (operands[0].reg_bytes == 8) {
      rex |= 0x48;
    }
    if (operands[0].reg >= 8) {
      rex |= 0x41;
    }
    if (rex && state->bits != 64) {
      asm_fail(state, line, "`bswap` of that register needs 64-bit code");
      return 0;
    }
    if (rex && !asm_byte(state, (unsigned char)rex)) {
      return 0;
    }
    if (!asm_byte(state, 0x0F)) {
      return 0;
    }
    return asm_byte(state, (unsigned char)(0xC8 + (operands[0].reg & 7)));
  }

  if (strcmp(mnemonic, "shld") == 0 || strcmp(mnemonic, "shrd") == 0) {
    int is_left = mnemonic[2] == 'l';
    if (count != 3 || operands[1].kind != X86_ASM_OPERAND_REG) {
      asm_fail(state, line,
               "`%s` takes an operand, a register and a count", mnemonic);
      return 0;
    }
    opcode[0] = 0x0F;
    if (operands[2].kind == X86_ASM_OPERAND_IMM) {
      opcode[1] = (unsigned char)(is_left ? 0xA4 : 0xAC);
      if (!asm_emit_instruction(state, line, opcode, 2, operands[1].reg, 1,
                                operands[1].high_byte, &operands[0],
                                operands[1].reg_bytes, 0, 0)) {
        return 0;
      }
      return asm_emit_immediate(state, line, &operands[2], 1);
    }
    if (operands[2].kind != X86_ASM_OPERAND_REG || operands[2].reg != 1 ||
        operands[2].reg_bytes != 1) {
      asm_fail(state, line, "`%s` counts by an immediate or by cl", mnemonic);
      return 0;
    }
    opcode[1] = (unsigned char)(is_left ? 0xA5 : 0xAD);
    return asm_emit_instruction(state, line, opcode, 2, operands[1].reg, 1,
                                operands[1].high_byte, &operands[0],
                                operands[1].reg_bytes, 0, 0);
  }
  *handled = 0;
  return 0;
}

static int asm_encode_atomic(AsmState *state, int line,
                         const char *mnemonic,
                         X86AsmOperand *operands, int count,
                         int *handled) {
  unsigned char opcode[4];
  *handled = 1;

  if (strcmp(mnemonic, "xadd") == 0 || strcmp(mnemonic, "cmpxchg") == 0) {
    int operand_bytes;
    if (count != 2 || operands[1].kind != X86_ASM_OPERAND_REG) {
      asm_fail(state, line, "`%s` takes an operand and a register", mnemonic);
      return 0;
    }
    operand_bytes = asm_infer_operand_bytes(state, line, operands, count);
    if (!operand_bytes) {
      return 0;
    }
    opcode[0] = 0x0F;
    if (strcmp(mnemonic, "xadd") == 0) {
      opcode[1] = (unsigned char)(operand_bytes == 1 ? 0xC0 : 0xC1);
    } else {
      opcode[1] = (unsigned char)(operand_bytes == 1 ? 0xB0 : 0xB1);
    }
    return asm_emit_instruction(state, line, opcode, 2, operands[1].reg, 1,
                                operands[1].high_byte, &operands[0],
                                operand_bytes, 0, 0);
  }

  if (strcmp(mnemonic, "cmpxchg16b") == 0 ||
      strcmp(mnemonic, "cmpxchg8b") == 0) {
    int wide = strcmp(mnemonic, "cmpxchg16b") == 0;
    if (count != 1 || operands[0].kind != X86_ASM_OPERAND_MEM) {
      asm_fail(state, line, "`%s` takes a memory operand", mnemonic);
      return 0;
    }
    if (wide && state->bits != 64) {
      asm_fail(state, line, "`cmpxchg16b` needs 64-bit code");
      return 0;
    }
    opcode[0] = 0x0F;
    opcode[1] = 0xC7;
    return asm_emit_instruction(state, line, opcode, 2, 1, 0, 0, &operands[0],
                                0, 0, wide);
  }
  *handled = 0;
  return 0;
}

static int asm_encode_system(AsmState *state, int line,
                         const char *mnemonic,
                         X86AsmOperand *operands, int count,
                         int *handled) {
  unsigned char opcode[4];
  size_t i;
  *handled = 1;

  if (strcmp(mnemonic, "in") == 0 || strcmp(mnemonic, "out") == 0) {
    return asm_encode_port(state, line, strcmp(mnemonic, "in") == 0, mnemonic,
                           operands, count);
  }

  for (i = 0; i < sizeof(ASM_DESCRIPTOR) / sizeof(ASM_DESCRIPTOR[0]); i++) {
    if (strcmp(mnemonic, ASM_DESCRIPTOR[i].name) == 0) {
      return asm_encode_one_operand_digit(state, line, mnemonic,
                                          ASM_DESCRIPTOR[i].digit, 0x01, 0,
                                          operands, count);
    }
  }

  for (i = 0;
       i < sizeof(ASM_SEGMENT_REGISTER) / sizeof(ASM_SEGMENT_REGISTER[0]);
       i++) {
    if (strcmp(mnemonic, ASM_SEGMENT_REGISTER[i].name) == 0) {
      return asm_encode_one_operand_digit(state, line, mnemonic,
                                          ASM_SEGMENT_REGISTER[i].digit, 0x00,
                                          2, operands, count);
    }
  }

  for (i = 0; i < sizeof(ASM_SAVE_STATE) / sizeof(ASM_SAVE_STATE[0]); i++) {
    if (strcmp(mnemonic, ASM_SAVE_STATE[i].name) == 0) {
      return asm_encode_memory_digit(state, line, mnemonic,
                                     ASM_SAVE_STATE[i].digit, 0xAE, operands,
                                     count);
    }
  }

  if (strncmp(mnemonic, "prefetch", 8) == 0) {
    const char *hint = mnemonic + 8;
    for (i = 0; i < sizeof(ASM_PREFETCH_HINT) / sizeof(ASM_PREFETCH_HINT[0]);
         i++) {
      if (strcmp(hint, ASM_PREFETCH_HINT[i].name) == 0) {
        return asm_encode_memory_digit(state, line, mnemonic,
                                       ASM_PREFETCH_HINT[i].digit, 0x18,
                                       operands, count);
      }
    }
    asm_fail(state, line, "unknown instruction `%s`", mnemonic);
    return 0;
  }

  if (strcmp(mnemonic, "arpl") == 0) {
    if (count != 2 || operands[1].kind != X86_ASM_OPERAND_REG ||
        operands[1].reg_bytes != 2) {
      asm_fail(state, line, "`arpl` takes an operand and a 16-bit register");
      return 0;
    }
    if (state->bits == 64) {
      asm_fail(state, line, "`arpl` is invalid in 64-bit code");
      return 0;
    }
    opcode[0] = 0x63;
    return asm_emit_instruction(state, line, opcode, 1, operands[1].reg, 1,
                                operands[1].high_byte, &operands[0], 2, 0, 0);
  }

  if (strcmp(mnemonic, "lar") == 0 || strcmp(mnemonic, "lsl") == 0) {
    if (count != 2 || operands[0].kind != X86_ASM_OPERAND_REG) {
      asm_fail(state, line, "`%s` takes a register and an operand", mnemonic);
      return 0;
    }
    opcode[0] = 0x0F;
    opcode[1] = (unsigned char)(strcmp(mnemonic, "lar") == 0 ? 0x02 : 0x03);
    return asm_emit_instruction(state, line, opcode, 2, operands[0].reg, 0, 0,
                                &operands[1], operands[0].reg_bytes, 0, 0);
  }
  *handled = 0;
  return 0;
}

static int asm_encode_string(AsmState *state, int line,
                         const char *mnemonic,
                         X86AsmOperand *operands, int count,
                         int *handled) {
  size_t i;
  *handled = 1;

  (void)operands;

  for (i = 0; i < sizeof(ASM_STRING) / sizeof(ASM_STRING[0]); i++) {
    if (strcmp(mnemonic, ASM_STRING[i].name) != 0) {
      continue;
    }
    if (ASM_STRING[i].bare_only && count != 0) {
      break;
    }
    return asm_string_operation(state, line, mnemonic,
                                ASM_STRING[i].byte_opcode,
                                ASM_STRING[i].wide_opcode, ASM_STRING[i].width);
  }

  *handled = 0;
  return 0;
}

static int asm_encode_sse(AsmState *state, int line,
                         const char *mnemonic,
                         X86AsmOperand *operands, int count,
                         int *handled) {
  unsigned char opcode[4];
  size_t i;
  *handled = 1;

  if (strcmp(mnemonic, "movd") == 0 || strcmp(mnemonic, "movq") == 0) {
    int wide = strcmp(mnemonic, "movq") == 0;
    if (count != 2) {
      asm_fail(state, line, "`%s` takes two operands", mnemonic);
      return 0;
    }
    opcode[0] = 0x0F;
    if (operands[0].kind == X86_ASM_OPERAND_REG &&
        operands[0].reg_class == X86_ASM_REG_XMM) {
      opcode[1] = 0x6E;
      return asm_emit_instruction(state, line, opcode, 2, operands[0].reg, 0, 0,
                                  &operands[1], wide ? 8 : 4, 0x66, 0);
    }
    opcode[1] = 0x7E;
    if (operands[1].kind != X86_ASM_OPERAND_REG ||
        operands[1].reg_class != X86_ASM_REG_XMM) {
      asm_fail(state, line, "`%s` needs an xmm register operand", mnemonic);
      return 0;
    }
    return asm_emit_instruction(state, line, opcode, 2, operands[1].reg, 0, 0,
                                &operands[0], wide ? 8 : 4, 0x66, 0);
  }

  for (i = 0; i < sizeof(ASM_SSE) / sizeof(ASM_SSE[0]); i++) {
    if (strcmp(mnemonic, ASM_SSE[i].name) != 0) {
      continue;
    }
    if (count != 2) {
      asm_fail(state, line, "`%s` takes two operands", mnemonic);
      return 0;
    }
    opcode[0] = 0x0F;
    if (operands[0].kind == X86_ASM_OPERAND_REG &&
        operands[0].reg_class == X86_ASM_REG_XMM) {
      opcode[1] = ASM_SSE[i].opcode;
      return asm_emit_instruction(state, line, opcode, 2, operands[0].reg, 0, 0,
                                  &operands[1], 0, ASM_SSE[i].prefix, 0);
    }
    if (operands[1].kind == X86_ASM_OPERAND_REG &&
        operands[1].reg_class == X86_ASM_REG_XMM) {
      unsigned char store = ASM_SSE[i].opcode;
      if (store == 0x10 || store == 0x28) {
        store = (unsigned char)(store + 1);
      } else if (store == 0x6F) {
        store = 0x7F;
      } else {
        asm_fail(state, line, "`%s` cannot store to memory", mnemonic);
        return 0;
      }
      opcode[1] = store;
      return asm_emit_instruction(state, line, opcode, 2, operands[1].reg, 0, 0,
                                  &operands[0], 0, ASM_SSE[i].prefix, 0);
    }
    asm_fail(state, line, "`%s` needs an xmm register operand", mnemonic);
    return 0;
  }

  if (strcmp(mnemonic, "cvtsi2ss") == 0 || strcmp(mnemonic, "cvtsi2sd") == 0) {
    if (count != 2 || operands[0].kind != X86_ASM_OPERAND_REG) {
      asm_fail(state, line, "`%s` takes an xmm register destination", mnemonic);
      return 0;
    }
    opcode[0] = 0x0F;
    opcode[1] = 0x2A;
    return asm_emit_instruction(
        state, line, opcode, 2, operands[0].reg, 0, 0, &operands[1],
        operands[1].kind == X86_ASM_OPERAND_REG ? operands[1].reg_bytes
                                                : operands[1].mem_bytes,
        mnemonic[7] == 's' ? 0xF3 : 0xF2, 0);
  }
  if (strcmp(mnemonic, "cvttss2si") == 0 ||
      strcmp(mnemonic, "cvttsd2si") == 0) {
    if (count != 2 || operands[0].kind != X86_ASM_OPERAND_REG) {
      asm_fail(state, line, "`%s` takes a register destination", mnemonic);
      return 0;
    }
    opcode[0] = 0x0F;
    opcode[1] = 0x2C;
    return asm_emit_instruction(state, line, opcode, 2, operands[0].reg, 0, 0,
                                &operands[1], operands[0].reg_bytes,
                                mnemonic[4] == 's' ? 0xF3 : 0xF2, 0);
  }
  *handled = 0;
  return 0;
}

static int asm_encode_simple(AsmState *state, int line,
                         const char *mnemonic,
                         X86AsmOperand *operands, int count,
                         int *handled) {
  unsigned char opcode[4];
  size_t i;
  *handled = 1;

  if (strcmp(mnemonic, "nop") == 0 && count == 1) {
    opcode[0] = 0x0F;
    opcode[1] = 0x1F;
    return asm_emit_instruction(state, line, opcode, 2, 0, 0, 0, &operands[0],
                                0, 0, 0);
  }

  for (i = 0; i < sizeof(ASM_SIMPLE) / sizeof(ASM_SIMPLE[0]); i++) {
    if (strcmp(mnemonic, ASM_SIMPLE[i].name) == 0) {
      int j;
      if (count != 0) {
        asm_fail(state, line, "`%s` takes no operands", mnemonic);
        return 0;
      }
      for (j = 0; j < ASM_SIMPLE[i].length; j++) {
        if (!asm_byte(state, ASM_SIMPLE[i].bytes[j])) {
          return 0;
        }
      }
      return 1;
    }
  }
  *handled = 0;
  return 0;
}

static int asm_encode_mnemonic(AsmState *state, int line, const char *mnemonic,
                               X86AsmOperand *operands, int count) {
  static int (*const GROUPS[])(AsmState *, int, const char *, X86AsmOperand *,
                               int, int *) = {
      asm_encode_arithmetic, asm_encode_move,   asm_encode_test_exchange,
      asm_encode_stack,      asm_encode_width,  asm_encode_branch,
      asm_encode_bit,        asm_encode_atomic, asm_encode_system,
      asm_encode_string,     asm_encode_sse,    asm_encode_simple};
  size_t i;

  for (i = 0; i < sizeof(GROUPS) / sizeof(GROUPS[0]); i++) {
    int handled = 0;
    int encoded = GROUPS[i](state, line, mnemonic, operands, count, &handled);
    if (handled) {
      return encoded;
    }
  }
  asm_fail(state, line, "unknown instruction `%s`", mnemonic);
  return 0;
}

static int asm_statement(AsmState *state);

static int asm_consume_labels(AsmState *state, int line) {
  AsmLexer *lexer = state->lexer;
  while (lexer->current.type == AT_IDENT) {
    const AsmToken *next = asm_peek(lexer);
    if (!(next->type == AT_PUNCT && next->punct == ':')) {
      return 1;
    }
    if (!asm_add_label(state, lexer->current.text, state->size)) {
      asm_fail(state, line, "out of memory");
      return 0;
    }
    asm_advance(lexer);
    asm_advance(lexer);
    while (lexer->current.type == AT_NEWLINE) {
      asm_advance(lexer);
    }
  }
  return 1;
}

static void asm_read_repeat_prefixes(AsmState *state, char *mnemonic,
                                     size_t size) {
  AsmLexer *lexer = state->lexer;
  for (;;) {
    snprintf(mnemonic, size, "%s", lexer->current.text);
    asm_lowercase(mnemonic);
    if (strcmp(mnemonic, "lock") == 0) {
      state->lock_prefix = 1;
      asm_advance(lexer);
      continue;
    }
    if (strcmp(mnemonic, "rep") == 0 || strcmp(mnemonic, "repe") == 0 ||
        strcmp(mnemonic, "repz") == 0) {
      state->rep_prefix = 0xF3;
      asm_advance(lexer);
      continue;
    }
    if (strcmp(mnemonic, "repne") == 0 || strcmp(mnemonic, "repnz") == 0) {
      state->rep_prefix = 0xF2;
      asm_advance(lexer);
      continue;
    }
    return;
  }
}

static int asm_directive_bits(AsmState *state, int line,
                              const char *mnemonic) {
  AsmLexer *lexer = state->lexer;
  int requested = 0;
  if (!state->config->allow_bits_directive) {
    asm_fail(state, line,
             "`bits` is only allowed in a `@naked` function's asm block");
    return 0;
  }
  if (strcmp(mnemonic, "bits") == 0) {
    asm_advance(lexer);
    if (lexer->current.type != AT_NUMBER) {
      asm_fail(state, line, "`bits` takes 16, 32 or 64");
      return 0;
    }
    requested = (int)lexer->current.number;
    asm_advance(lexer);
  } else {
    requested = atoi(mnemonic + 3);
    asm_advance(lexer);
  }
  if (requested != 16 && requested != 32 && requested != 64) {
    asm_fail(state, line, "`bits` takes 16, 32 or 64");
    return 0;
  }
  state->bits = requested;
  return 1;
}

static int asm_directive_reserve(AsmState *state, int line, int width) {
  X86AsmOperand operand;
  long long total;
  if (!asm_parse_operand(state, &operand)) {
    return 0;
  }
  if (operand.kind != X86_ASM_OPERAND_IMM || operand.symbol) {
    asm_operand_release(&operand);
    asm_fail(state, line, "a reservation takes a constant count");
    return 0;
  }
  total = operand.imm * width;
  asm_operand_release(&operand);
  while (total-- > 0) {
    if (!asm_byte(state, 0)) {
      return 0;
    }
  }
  return 1;
}

static int asm_directive_align(AsmState *state, int line) {
  X86AsmOperand operand;
  long long boundary;
  if (!asm_parse_operand(state, &operand)) {
    return 0;
  }
  boundary = operand.imm;
  asm_operand_release(&operand);
  if (boundary <= 0 || (boundary & (boundary - 1)) != 0) {
    asm_fail(state, line, "`align` takes a power of two");
    return 0;
  }
  while ((long long)((state->config->origin + state->size) %
                     (size_t)boundary)) {
    if (!asm_byte(state, 0x90)) {
      return 0;
    }
  }
  return 1;
}

static int asm_directive_times(AsmState *state, int line) {
  AsmLexer *lexer = state->lexer;
  X86AsmOperand operand;
  long long repetitions;
  size_t saved_position;
  int saved_line;
  AsmToken saved_current;
  AsmToken saved_lookahead;
  int saved_has_lookahead;
  if (!asm_parse_operand(state, &operand)) {
    return 0;
  }
  if (operand.kind != X86_ASM_OPERAND_IMM || operand.symbol) {
    asm_operand_release(&operand);
    asm_fail(state, line, "`times` takes a constant count");
    return 0;
  }
  repetitions = operand.imm;
  asm_operand_release(&operand);
  if (repetitions < 0) {
    asm_fail(state, line, "`times` count is negative");
    return 0;
  }
  saved_position = lexer->position;
  saved_line = lexer->line;
  saved_current = lexer->current;
  saved_lookahead = lexer->lookahead;
  saved_has_lookahead = lexer->has_lookahead;
  if (repetitions == 0) {
    while (lexer->current.type != AT_NEWLINE &&
           lexer->current.type != AT_END) {
      asm_advance(lexer);
    }
    return 1;
  }
  while (repetitions-- > 0) {
    lexer->position = saved_position;
    lexer->line = saved_line;
    lexer->current = saved_current;
    lexer->lookahead = saved_lookahead;
    lexer->has_lookahead = saved_has_lookahead;
    if (!asm_statement(state)) {
      return 0;
    }
  }
  return 1;
}

static int asm_statement_instruction(AsmState *state, int line,
                                     const char *mnemonic) {
  X86AsmOperand operands[X86_ASM_MAX_OPERANDS];
  int operand_count = 0;
  size_t patch_mark = state->patch_count;
  size_t patch;
  int result;
  int i;

  memset(operands, 0, sizeof(operands));
  if (!asm_parse_operand_list(state, operands, &operand_count)) {
    for (i = 0; i < X86_ASM_MAX_OPERANDS; i++) {
      asm_operand_release(&operands[i]);
    }
    return 0;
  }

  result = asm_encode_mnemonic(state, line, mnemonic, operands, operand_count);
  for (i = 0; i < X86_ASM_MAX_OPERANDS; i++) {
    asm_operand_release(&operands[i]);
  }
  if (!result) {
    return 0;
  }
  for (patch = patch_mark; patch < state->patch_count; patch++) {
    state->patches[patch].next_instruction_offset = state->size;
  }
  return 1;
}

static int asm_statement_body(AsmState *state, int line,
                              const char *mnemonic) {
  AsmLexer *lexer = state->lexer;

  if (strcmp(mnemonic, "bits") == 0 || strcmp(mnemonic, "use16") == 0 ||
      strcmp(mnemonic, "use32") == 0 || strcmp(mnemonic, "use64") == 0) {
    return asm_directive_bits(state, line, mnemonic);
  }
  if (strcmp(mnemonic, "db") == 0 || strcmp(mnemonic, "dw") == 0 ||
      strcmp(mnemonic, "dd") == 0 || strcmp(mnemonic, "dq") == 0) {
    int width = mnemonic[1] == 'b' ? 1 : (mnemonic[1] == 'w' ? 2
                                          : (mnemonic[1] == 'd' ? 4 : 8));
    asm_advance(lexer);
    return asm_directive_data(state, line, width);
  }
  if (strcmp(mnemonic, "resb") == 0 || strcmp(mnemonic, "resw") == 0 ||
      strcmp(mnemonic, "resd") == 0 || strcmp(mnemonic, "resq") == 0) {
    int width = mnemonic[3] == 'b' ? 1 : (mnemonic[3] == 'w' ? 2
                                          : (mnemonic[3] == 'd' ? 4 : 8));
    asm_advance(lexer);
    return asm_directive_reserve(state, line, width);
  }
  if (strcmp(mnemonic, "align") == 0) {
    asm_advance(lexer);
    return asm_directive_align(state, line);
  }
  if (strcmp(mnemonic, "times") == 0) {
    asm_advance(lexer);
    return asm_directive_times(state, line);
  }

  asm_advance(lexer);
  return asm_statement_instruction(state, line, mnemonic);
}

static int asm_statement(AsmState *state) {
  AsmLexer *lexer = state->lexer;
  char mnemonic[192];
  int line;

  state->lock_prefix = 0;
  state->rep_prefix = 0;

  while (lexer->current.type == AT_NEWLINE) {
    asm_advance(lexer);
  }
  if (lexer->current.type == AT_END) {
    return 1;
  }

  line = lexer->current.line;
  if (!asm_consume_labels(state, line)) {
    return 0;
  }

  if (lexer->current.type == AT_END) {
    return 1;
  }
  if (lexer->current.type == AT_NEWLINE) {
    asm_advance(lexer);
    return 1;
  }
  if (lexer->current.type != AT_IDENT) {
    asm_fail(state, line, "expected an instruction, found `%s`",
             lexer->current.text);
    return 0;
  }

  asm_read_repeat_prefixes(state, mnemonic, sizeof(mnemonic));
  return asm_statement_body(state, line, mnemonic);
}

static int asm_prescan_labels(AsmState *state, const char *text) {
  AsmLexer scanner;
  memset(&scanner, 0, sizeof(scanner));
  scanner.source = text;
  scanner.line = 1;
  asm_advance(&scanner);
  while (scanner.current.type != AT_END && !scanner.failed) {
    if (scanner.current.type == AT_IDENT) {
      const AsmToken *next = asm_peek(&scanner);
      if (next->type == AT_PUNCT && next->punct == ':') {
        if (!asm_declare_label(state, scanner.current.text)) {
          return 0;
        }
      }
    }
    asm_advance(&scanner);
  }
  return 1;
}

static int asm_resolve_patches(AsmState *state, X86AsmResult *result) {
  size_t i;
  for (i = 0; i < state->patch_count; i++) {
    AsmPatch *patch = &state->patches[i];
    size_t target;
    if (asm_find_label(state, patch->name, &target)) {
      long long value;
      if (patch->pc_relative) {
        value = (long long)target - (long long)patch->next_instruction_offset +
                patch->addend;
        if (!asm_fits_signed(value, patch->bytes)) {
          asm_fail(state, patch->line,
                   "branch to `%s` is out of range for a %d-byte displacement",
                   patch->name, patch->bytes);
          return 0;
        }
        {
          int b;
          for (b = 0; b < patch->bytes; b++) {
            state->code[patch->offset + (size_t)b] =
                (unsigned char)((unsigned long long)value >> (8 * b)) & 0xFFu;
          }
        }
        continue;
      }
      {
        X86AsmFixup *grown = (X86AsmFixup *)realloc(
            result->fixups, (result->fixup_count + 1) * sizeof(X86AsmFixup));
        if (!grown) {
          return 0;
        }
        result->fixups = grown;
        grown[result->fixup_count].symbol = strdup(patch->name);
        grown[result->fixup_count].offset = patch->offset;
        grown[result->fixup_count].bytes = patch->bytes;
        grown[result->fixup_count].kind = X86_ASM_FIXUP_ABSOLUTE;
        grown[result->fixup_count].addend =
            patch->addend + (long long)target;
        grown[result->fixup_count].next_instruction_offset =
            patch->next_instruction_offset;
        grown[result->fixup_count].block_local = 1;
        result->fixup_count++;
      }
      continue;
    }
    {
      X86AsmFixup *grown = (X86AsmFixup *)realloc(
          result->fixups, (result->fixup_count + 1) * sizeof(X86AsmFixup));
      if (!grown) {
        return 0;
      }
      result->fixups = grown;
      grown[result->fixup_count].symbol = strdup(patch->name);
      grown[result->fixup_count].offset = patch->offset;
      grown[result->fixup_count].bytes = patch->bytes;
      grown[result->fixup_count].kind = patch->pc_relative
                                            ? X86_ASM_FIXUP_PC_RELATIVE
                                            : X86_ASM_FIXUP_ABSOLUTE;
      grown[result->fixup_count].addend = patch->addend;
      grown[result->fixup_count].next_instruction_offset =
          patch->next_instruction_offset;
      grown[result->fixup_count].block_local = 0;
      result->fixup_count++;
    }
  }
  return 1;
}

static void asm_state_destroy(AsmState *state) {
  size_t i;
  for (i = 0; i < state->label_count; i++) {
    free(state->labels[i].name);
  }
  free(state->labels);
  for (i = 0; i < state->patch_count; i++) {
    free(state->patches[i].name);
  }
  free(state->patches);
  for (i = 0; i < state->declared_count; i++) {
    free(state->declared[i]);
  }
  free(state->declared);
  free(state->short_hints);
}

int x86_asm_assemble(const char *text, const X86AsmConfig *config,
                     X86AsmResult *result, char *error, size_t error_size,
                     int *error_line) {
  AsmState state;
  AsmLexer lexer;

  if (!text || !config || !result) {
    return 0;
  }

  memset(&state, 0, sizeof(state));
  memset(&lexer, 0, sizeof(lexer));
  memset(result, 0, sizeof(*result));

  lexer.source = text;
  lexer.line = 1;
  state.lexer = &lexer;
  state.config = config;
  state.bits = config->bits ? config->bits : 64;
  int relaxation_rounds = 0;

  if (!asm_prescan_labels(&state, text)) {
    snprintf(error, error_size, "out of memory");
    asm_state_destroy(&state);
    return 0;
  }

  for (;;) {
    size_t i;
    int hints_changed = 0;

    free(state.code);
    state.code = NULL;
    state.size = 0;
    state.capacity = 0;
    for (i = 0; i < state.label_count; i++) {
      free(state.labels[i].name);
    }
    state.label_count = 0;
    for (i = 0; i < state.patch_count; i++) {
      free(state.patches[i].name);
    }
    state.patch_count = 0;
    state.branch_ordinal = 0;
    state.bits = config->bits ? config->bits : 64;

    memset(&lexer, 0, sizeof(lexer));
    lexer.source = text;
    lexer.line = 1;
    asm_advance(&lexer);
    while (lexer.current.type != AT_END && !lexer.failed) {
      if (!asm_statement(&state)) {
        break;
      }
    }
    if (lexer.failed) {
      break;
    }

    if (relaxation_rounds++ >= 8) {
      break;
    }
    if ((size_t)state.branch_ordinal > state.short_hint_count) {
      unsigned char *grown = (unsigned char *)realloc(
          state.short_hints, (size_t)state.branch_ordinal);
      if (!grown) {
        break;
      }
      memset(grown + state.short_hint_count, 0,
             (size_t)state.branch_ordinal - state.short_hint_count);
      state.short_hints = grown;
      state.short_hint_count = (size_t)state.branch_ordinal;
    }
    for (i = 0; i < state.patch_count; i++) {
      const AsmPatch *patch = &state.patches[i];
      size_t target;
      long long displacement;
      if (patch->branch_ordinal < 0 || patch->bytes == 1 ||
          !asm_find_label(&state, patch->name, &target)) {
        continue;
      }
      displacement = (long long)target -
                     (long long)patch->next_instruction_offset + patch->addend;
      if (displacement < -128 || displacement > 127) {
        continue;
      }
      if (!state.short_hints[patch->branch_ordinal]) {
        state.short_hints[patch->branch_ordinal] = 1;
        hints_changed = 1;
      }
    }
    if (!hints_changed) {
      break;
    }
  }

  if (lexer.failed) {
    if (error && error_size) {
      snprintf(error, error_size, "%s", lexer.error);
    }
    if (error_line) {
      *error_line = lexer.error_line;
    }
    free(state.code);
    asm_state_destroy(&state);
    return 0;
  }

  result->code = state.code;
  result->size = state.size;
  result->final_bits = state.bits;

  if (!asm_resolve_patches(&state, result)) {
    if (error && error_size) {
      snprintf(error, error_size, "%s",
               lexer.error[0] ? lexer.error : "out of memory");
    }
    if (error_line) {
      *error_line = lexer.error_line;
    }
    x86_asm_result_destroy(result);
    asm_state_destroy(&state);
    return 0;
  }

  asm_state_destroy(&state);
  return 1;
}

void x86_asm_result_destroy(X86AsmResult *result) {
  size_t i;
  if (!result) {
    return;
  }
  free(result->code);
  for (i = 0; i < result->fixup_count; i++) {
    free(result->fixups[i].symbol);
  }
  free(result->fixups);
  memset(result, 0, sizeof(*result));
}
