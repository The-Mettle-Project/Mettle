#ifndef MTLC_X86_ASM_H
#define MTLC_X86_ASM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  X86_ASM_OPERAND_NONE = 0,
  X86_ASM_OPERAND_REG,
  X86_ASM_OPERAND_IMM,
  X86_ASM_OPERAND_MEM,
  X86_ASM_OPERAND_FAR
} X86AsmOperandKind;

typedef enum {
  X86_ASM_REG_GP = 0,
  X86_ASM_REG_SEG,
  X86_ASM_REG_CR,
  X86_ASM_REG_DR,
  X86_ASM_REG_XMM,
  X86_ASM_REG_RIP
} X86AsmRegClass;

typedef struct {
  X86AsmOperandKind kind;
  int reg_class;
  int reg;
  int reg_bytes;
  int high_byte;
  long long imm;
  char *symbol;
  int symbol_is_label;
  int has_base;
  int base;
  int has_index;
  int index;
  int scale;
  long long disp;
  int rip_relative;
  int mem_bytes;
  int address_bytes;
  int segment;
  int explicit_short;
  long long far_segment;
} X86AsmOperand;

typedef enum {
  X86_ASM_FIXUP_ABSOLUTE = 0,
  X86_ASM_FIXUP_PC_RELATIVE
} X86AsmFixupKind;

typedef struct {
  char *symbol;
  size_t offset;
  int bytes;
  X86AsmFixupKind kind;
  long long addend;
  size_t next_instruction_offset;
  int block_local;
} X86AsmFixup;

typedef int (*X86AsmBindingResolver)(void *context, const char *name,
                                     X86AsmOperand *out, char *error,
                                     size_t error_size);

typedef struct {
  int bits;
  size_t origin;
  X86AsmBindingResolver resolve_binding;
  void *binding_context;
  int allow_bits_directive;
} X86AsmConfig;

typedef struct {
  unsigned char *code;
  size_t size;
  X86AsmFixup *fixups;
  size_t fixup_count;
  int final_bits;
} X86AsmResult;

int x86_asm_assemble(const char *text, const X86AsmConfig *config,
                     X86AsmResult *result, char *error, size_t error_size,
                     int *error_line);

void x86_asm_result_destroy(X86AsmResult *result);

int x86_asm_lookup_register(const char *name, int *out_class, int *out_reg,
                            int *out_bytes, int *out_high_byte);

#ifdef __cplusplus
}
#endif

#endif
