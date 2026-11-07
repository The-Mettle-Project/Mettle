#ifndef METTLE_X86_DISASM_H
#define METTLE_X86_DISASM_H

#include <stddef.h>

typedef struct {
  int length;
  char text[128];
  char mnemonic[24];
  int is_vector;
  int is_branch;
} X86DisasmInsn;

int x86_disasm_one(const unsigned char *bytes, size_t length,
                   long long address, X86DisasmInsn *out);

#endif
