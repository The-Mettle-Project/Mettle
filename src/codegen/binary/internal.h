#ifndef CODEGEN_BINARY_INTERNAL_H
#define CODEGEN_BINARY_INTERNAL_H

#include "codegen/code_generator_internal.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
  const char **names;
  size_t *indices;
  size_t capacity;
} BinaryLabelIndex;

int binary_label_index_build(const IRFunction *function,
                             BinaryLabelIndex *index);
size_t binary_label_index_find(const BinaryLabelIndex *index, const char *name);
void binary_label_index_destroy(BinaryLabelIndex *index);

int cg_time_enabled(void);
double cg_time_begin(void);
void cg_time_end(const char *name, double started);
void cg_time_report(void);

#define BINARY_TEXT_SECTION_ALIGNMENT 16
#define BINARY_FUNCTION_STACK_SLOT_SIZE 8

#define BINARY_SAFETY_GRANULE 16
#define BINARY_WIN64_REGISTER_ARG_COUNT 4
#define BINARY_WIN64_SHADOW_SPACE_SIZE 32
#define BINARY_STACK_PAGE_SIZE 4096

typedef enum {
  BINARY_GP_RAX = 0,
  BINARY_GP_RCX = 1,
  BINARY_GP_RDX = 2,
  BINARY_GP_RBX = 3,
  BINARY_GP_RSP = 4,
  BINARY_GP_RBP = 5,
  BINARY_GP_RSI = 6,
  BINARY_GP_RDI = 7,
  BINARY_GP_R11 = 11,
  BINARY_GP_R8 = 8,
  BINARY_GP_R9 = 9,
  BINARY_GP_R10 = 10,
  BINARY_GP_R12 = 12,
  BINARY_GP_R13 = 13,
  BINARY_GP_R14 = 14,
  BINARY_GP_R15 = 15,
} BinaryGpRegister;

#define BINARY_GP_STORE_VALUE BINARY_GP_RCX

typedef enum {
  BINARY_XMM0 = 0,
  BINARY_XMM1 = 1,
  BINARY_XMM2 = 2,
  BINARY_XMM3 = 3,
  BINARY_XMM4 = 4,
  BINARY_XMM5 = 5,
  BINARY_XMM6 = 6,
  BINARY_XMM7 = 7,
  BINARY_XMM8 = 8,
  BINARY_XMM9 = 9,
  BINARY_XMM10 = 10,
  BINARY_XMM11 = 11,
  BINARY_XMM12 = 12,
  BINARY_XMM13 = 13,
  BINARY_XMM14 = 14,
  BINARY_XMM15 = 15,
} BinaryXmmRegister;

typedef enum {
  BINARY_EIGHTBYTE_NONE = 0,
  BINARY_EIGHTBYTE_INTEGER,
  BINARY_EIGHTBYTE_SSE,
} BinaryEightbyteClass;

typedef struct {
  int in_memory;
  size_t size;
  size_t eightbyte_count;
  BinaryEightbyteClass classes[2];
  size_t first_slot;
} BinarySysvAggregate;

typedef struct {
  unsigned char *data;
  size_t size;
  size_t capacity;
} BinaryCodeBuffer;

typedef struct {
  char *name;
  int offset;
} BinaryNamedSlot;

typedef struct {
  BinaryNamedSlot *items;
  size_t count;
  size_t capacity;
  size_t *slots;
  size_t slot_count;
} BinaryNamedSlotTable;

typedef struct {
  char *name;
  size_t offset;
} BinaryLabelEntry;

typedef struct {
  BinaryLabelEntry *items;
  size_t count;
  size_t capacity;
  size_t *slots;
  size_t slot_count;
} BinaryLabelTable;

typedef struct {
  char *name;
  size_t displacement_offset;
} BinaryLabelFixup;

typedef struct {
  BinaryLabelFixup *items;
  size_t count;
  size_t capacity;
} BinaryLabelFixupTable;

typedef struct {
  char *symbol_name;
  size_t displacement_offset;
} BinaryCallRelocation;

typedef struct {
  BinaryCallRelocation *items;
  size_t count;
  size_t capacity;
} BinaryCallRelocationTable;

typedef struct {
  char *symbol_name;
  size_t offset;
  int kind;
  int32_t addend;
} BinaryAsmRelocation;

typedef struct {
  BinaryAsmRelocation *items;
  size_t count;
  size_t capacity;
} BinaryAsmRelocationTable;

typedef struct {
  size_t *items;
  size_t count;
  size_t capacity;
} BinaryOffsetTable;

typedef struct {
  const char *name;
  const char *target;
} BinarySymbolAliasEntry;

typedef struct {
  BinarySymbolAliasEntry *items;
  size_t count;
  size_t capacity;
  size_t *slots;
  size_t slot_count;
} BinarySymbolAliasTable;

typedef struct {
  char *name;
  size_t offset;
} BinaryDebugLabelExport;

typedef struct {
  BinaryDebugLabelExport *items;
  size_t count;
  size_t capacity;
} BinaryDebugLabelExportTable;

typedef struct {
  const char *name;
  const char *decl_type;
  MtlcType *symbol_type;
  MtlcType *temp_type;
} BinaryOperandTypeEntry;

typedef struct {
  BinaryOperandTypeEntry *items;
  size_t count;
  size_t capacity;
  size_t *buckets;
  size_t bucket_count;
  int built;
} BinaryOperandTypeIndex;

typedef struct {
  const IROperand *operand;
  int base_register;
  int displacement;
} BinaryMarshaledOperand;

#define BINARY_MAX_MARSHALED_OPERANDS 16

typedef struct {
  BinaryCodeBuffer code;
  BinaryNamedSlotTable parameter_slots;
  BinaryNamedSlotTable local_slots;
  BinaryNamedSlotTable temp_slots;
  BinaryNamedSlotTable string_symbols;
  BinaryNamedSlotTable cstring_symbols;
  BinaryNamedSlotTable float64_symbols;
  BinaryNamedSlotTable address_taken_symbols;
  BinaryNamedSlotTable register_symbols;
  BinaryNamedSlotTable register_global_symbols;
  BinarySymbolAliasTable symbol_aliases;
  BinaryLabelTable labels;
  BinaryLabelFixupTable label_fixups;
  BinaryCallRelocationTable call_relocations;
  BinaryAsmRelocationTable asm_relocations;
  BinaryOffsetTable return_fixups;
  BinaryGpRegister saved_registers[8];
  int saved_register_offsets[8];
  size_t saved_register_count;
  BinaryXmmRegister saved_xmm_registers[8];
  int saved_xmm_offsets[8];
  size_t saved_xmm_count;
  int raw_frame_size;
  int frame_size;
  int omit_frame_pointer;
  int wants_wide_loop_alignment;
  int return_float_bits;
  int returns_indirect;
  size_t indirect_return_size;
  int returns_sysv_registers;
  BinarySysvAggregate sysv_return_class;
  int *indirect_return_slot_offsets;
  size_t indirect_return_slot_count;
  size_t indirect_return_slot_capacity;
  size_t indirect_return_slot_cursor;
  int *incoming_aggregate_offsets;
  size_t incoming_aggregate_count;
  char **indirect_temp_names;
  size_t *indirect_temp_sizes;
  size_t indirect_temp_count;
  size_t indirect_temp_capacity;
  IRFunction *ir_function;
  const char *function_name;
  BinaryOperandTypeIndex operand_types;
  char *runtime_end_label;
  BinaryDebugLabelExportTable debug_export_labels;
  BinaryMarshaledOperand marshaled_operands[BINARY_MAX_MARSHALED_OPERANDS];
  size_t marshaled_operand_count;
} BinaryFunctionContext;

typedef struct {
  char *name;
  uint64_t bits;
  long long int_value;
  double float_value;
  int is_float;
  int can_inline_load;
} BinaryGlobalConstEntry;

typedef struct {
  BinaryGlobalConstEntry *items;
  size_t count;
  size_t capacity;
  size_t *slots;
  size_t slot_count;
} BinaryGlobalConstTable;
typedef struct {
  long long int_value;
  double float_value;
  int is_float;
} BinaryNumericConstant;
typedef struct {
  const char *name;
  IRFunction *function;
} BinaryIRFunctionSlot;

typedef struct {
  BinaryIRFunctionSlot *slots;
  size_t slot_count;
  const IRProgram *program;
  size_t function_count;
} BinaryIRFunctionIndex;

extern const BinaryGpRegister BINARY_WIN64_INT_PARAM_REGISTERS[];
extern const BinaryXmmRegister BINARY_WIN64_FLOAT_PARAM_REGISTERS[];

typedef struct {
  const BinaryGpRegister *int_param_registers;
  size_t int_param_count;
  const BinaryXmmRegister *float_param_registers;
  size_t float_param_count;
  int shadow_space_size;
  BinaryGpRegister indirect_return_register;
  int counts_classes_separately;
} BinaryAbi;

const BinaryAbi *code_generator_binary_active_abi(void);
void code_generator_binary_select_abi(BinaryTargetFormat format);
void code_generator_binary_describe_abi(const BinaryGpRegister *int_regs,
                                        size_t int_count,
                                        const BinaryXmmRegister *float_regs,
                                        size_t float_count, int shadow_space,
                                        BinaryGpRegister indirect_return,
                                        int separate_classes);

int code_generator_binary_classify_sysv_aggregate(const MtlcType *type,
                                                  BinarySysvAggregate *out);

int code_generator_binary_function_is_abi_public(CodeGenerator *generator,
                                                 const char *name);

typedef enum {
  BINARY_ARG_IN_GP_REGISTER,
  BINARY_ARG_IN_XMM_REGISTER,
  BINARY_ARG_ON_STACK,
} BinaryArgLocationKind;

typedef struct {
  BinaryArgLocationKind kind;
  BinaryGpRegister gp_register;
  BinaryXmmRegister xmm_register;
  int stack_offset;
} BinaryArgLocation;

int code_generator_binary_compute_arg_layout(const BinaryAbi *abi,
                                              const int *is_float, size_t count,
                                              BinaryArgLocation *locations_out,
                                              int *stack_bytes_out);

int code_generator_binary_compute_arg_layout_ex(const BinaryAbi *abi,
                                                const int *is_float,
                                                const int *force_stack,
                                                const size_t *stack_slots,
                                                size_t count,
                                                BinaryArgLocation *locations_out,
                                                int *stack_bytes_out);

extern BinaryGlobalConstTable g_binary_global_consts;
extern BinaryIRFunctionIndex g_binary_ir_function_index;

BinaryLabelEntry *binary_label_table_get(BinaryLabelTable *table, const char *name);
size_t *code_generator_binary_build_loop_weights( const IRFunction *function);
const MtlcType *code_generator_binary_get_operand_type(CodeGenerator *generator, const IROperand *operand);
const MtlcType *code_generator_binary_get_operand_type_in_context( CodeGenerator *generator, BinaryFunctionContext *context, const IROperand *operand);
const MtlcType *code_generator_binary_get_resolved_type(CodeGenerator *generator, const char *type_name, int allow_void);
IRFunction *code_generator_find_ir_function_binary(CodeGenerator *generator, const char *name);
int binary_align_up_int(int value, int alignment, int *result_out);
int binary_call_relocation_table_add(BinaryCallRelocationTable *table, const char *symbol_name, size_t displacement_offset);
void binary_call_relocation_table_destroy( BinaryCallRelocationTable *table);
int binary_code_buffer_append_bytes(BinaryCodeBuffer *buffer, const void *data, size_t size);
int binary_code_buffer_append_u32(BinaryCodeBuffer *buffer, uint32_t value);
int binary_code_buffer_append_u64(BinaryCodeBuffer *buffer, uint64_t value);
int binary_code_buffer_append_u8(BinaryCodeBuffer *buffer, unsigned char value);
void binary_code_buffer_destroy(BinaryCodeBuffer *buffer);
int binary_code_buffer_reserve(BinaryCodeBuffer *buffer, size_t minimum_capacity);
int binary_emit_add_reg_imm32(BinaryCodeBuffer *buffer, BinaryGpRegister reg, uint32_t immediate);
int binary_emit_add_rsp_imm32(BinaryCodeBuffer *buffer, uint32_t immediate);
int binary_emit_addsd_xmm_xmm(BinaryCodeBuffer *buffer, BinaryXmmRegister destination, BinaryXmmRegister source);
int binary_emit_addss_xmm_xmm(BinaryCodeBuffer *buffer, BinaryXmmRegister destination, BinaryXmmRegister source);
int binary_emit_alu_reg8_reg8(BinaryCodeBuffer *buffer, unsigned char opcode, BinaryGpRegister destination, BinaryGpRegister source);
int binary_emit_alu_reg_imm32(BinaryCodeBuffer *buffer, unsigned char subopcode, BinaryGpRegister reg, uint32_t immediate);
int binary_emit_alu_reg_reg(BinaryCodeBuffer *buffer, unsigned char opcode, BinaryGpRegister destination, BinaryGpRegister source);
int binary_emit_alu_reg_mem(BinaryCodeBuffer *buffer, unsigned char opcode, BinaryGpRegister destination, BinaryGpRegister base, int displacement, int width);
int binary_emit_alu_reg_reg32(BinaryCodeBuffer *buffer, unsigned char opcode, BinaryGpRegister destination, BinaryGpRegister source);
int binary_emit_alu_reg_imm_w32(BinaryCodeBuffer *buffer, unsigned char subopcode, BinaryGpRegister reg, uint32_t immediate);
int binary_emit_unary_reg32(BinaryCodeBuffer *buffer, unsigned char subopcode, BinaryGpRegister reg);
int binary_emit_neg_reg32(BinaryCodeBuffer *buffer, BinaryGpRegister reg);
int binary_emit_not_reg32(BinaryCodeBuffer *buffer, BinaryGpRegister reg);
int binary_emit_alu_rsp_imm32(BinaryCodeBuffer *buffer, unsigned char subopcode, uint32_t immediate);
int binary_emit_and_reg_imm32(BinaryCodeBuffer *buffer, BinaryGpRegister reg, uint32_t immediate);
int binary_emit_call_placeholder(BinaryCodeBuffer *buffer, size_t *displacement_offset_out);
int binary_emit_call_reg(BinaryCodeBuffer *buffer, BinaryGpRegister reg);
int binary_emit_jmp_reg(BinaryCodeBuffer *buffer, BinaryGpRegister reg);
int binary_emit_cmovcc_reg_reg(BinaryCodeBuffer *buffer, unsigned char opcode, BinaryGpRegister destination, BinaryGpRegister source);
int binary_emit_cmp_reg_imm32(BinaryCodeBuffer *buffer, BinaryGpRegister reg, uint32_t immediate);
int binary_emit_cmp_reg_reg(BinaryCodeBuffer *buffer, BinaryGpRegister lhs, BinaryGpRegister rhs);
int binary_emit_cmp_reg_reg32(BinaryCodeBuffer *buffer, BinaryGpRegister lhs, BinaryGpRegister rhs);
int binary_emit_cmp_reg_imm_w32(BinaryCodeBuffer *buffer, BinaryGpRegister reg, uint32_t immediate);
int binary_emit_cqo(BinaryCodeBuffer *buffer);
int binary_emit_cvtsd2ss_xmm_xmm(BinaryCodeBuffer *buffer, BinaryXmmRegister destination, BinaryXmmRegister source);
int binary_emit_cvtsi2sd_xmm_reg(BinaryCodeBuffer *buffer, BinaryXmmRegister destination, BinaryGpRegister source);
int binary_emit_cvtsi2ss_xmm_reg(BinaryCodeBuffer *buffer, BinaryXmmRegister destination, BinaryGpRegister source);
int binary_emit_cvtss2sd_xmm_xmm(BinaryCodeBuffer *buffer, BinaryXmmRegister destination, BinaryXmmRegister source);
int binary_emit_cvttsd2si_reg_xmm(BinaryCodeBuffer *buffer, BinaryGpRegister destination, BinaryXmmRegister source);
int binary_emit_cvttss2si_reg_xmm(BinaryCodeBuffer *buffer, BinaryGpRegister destination, BinaryXmmRegister source);
int binary_emit_divsd_xmm_xmm(BinaryCodeBuffer *buffer, BinaryXmmRegister destination, BinaryXmmRegister source);
int binary_emit_divss_xmm_xmm(BinaryCodeBuffer *buffer, BinaryXmmRegister destination, BinaryXmmRegister source);
int binary_emit_frame_allocation(BinaryCodeBuffer *code, int frame_size);
int binary_emit_idiv_reg(BinaryCodeBuffer *buffer, BinaryGpRegister divisor);
int binary_emit_div_reg(BinaryCodeBuffer *buffer, BinaryGpRegister divisor);
int binary_emit_mul_reg(BinaryCodeBuffer *buffer, BinaryGpRegister src);
int binary_emit_imul_reg(BinaryCodeBuffer *buffer, BinaryGpRegister src);
int binary_emit_imul_reg_reg(BinaryCodeBuffer *buffer, BinaryGpRegister destination, BinaryGpRegister source);
int binary_emit_bt_reg_reg(BinaryCodeBuffer *buffer, BinaryGpRegister base, BinaryGpRegister offset);
int binary_emit_imul_reg_reg_imm32(BinaryCodeBuffer *buffer, BinaryGpRegister destination, BinaryGpRegister source, uint32_t immediate);
int binary_emit_imul_reg_reg32(BinaryCodeBuffer *buffer, BinaryGpRegister destination, BinaryGpRegister source);
int binary_emit_imul_reg_reg_imm32_scratch(BinaryCodeBuffer *buffer, BinaryGpRegister destination, BinaryGpRegister source, uint32_t immediate, int have_scratch, BinaryGpRegister scratch);
int binary_emit_imul_reg_reg_imm32_scratch_w32(BinaryCodeBuffer *buffer, BinaryGpRegister destination, BinaryGpRegister source, uint32_t immediate, int have_scratch, BinaryGpRegister scratch);
int binary_emit_imul_reg_reg_small_imm(BinaryCodeBuffer *buffer, BinaryGpRegister destination, BinaryGpRegister source, int32_t immediate);
int binary_emit_jcc_placeholder(BinaryCodeBuffer *buffer, unsigned char condition_opcode, size_t *displacement_offset_out);
int binary_emit_jmp_placeholder(BinaryCodeBuffer *buffer, size_t *displacement_offset_out);
int binary_emit_lea_reg_base_index_scale_disp( BinaryCodeBuffer *buffer, BinaryGpRegister destination, BinaryGpRegister base, BinaryGpRegister index, int scale, int displacement);
int binary_emit_lea_reg_mem(BinaryCodeBuffer *buffer, BinaryGpRegister destination, BinaryGpRegister base, int displacement);
int binary_emit_lea32_reg_mem(BinaryCodeBuffer *buffer, BinaryGpRegister destination, BinaryGpRegister base, int displacement);
int binary_emit_lea32_reg_base_index_scale_disp(BinaryCodeBuffer *buffer, BinaryGpRegister destination, BinaryGpRegister base, BinaryGpRegister index, int scale, int displacement);
int binary_emit_syscall(BinaryCodeBuffer *buffer);
int binary_emit_lea_reg_rip_placeholder(BinaryCodeBuffer *buffer, BinaryGpRegister destination, size_t *displacement_offset_out);
int binary_emit_memory_access(BinaryCodeBuffer *buffer, unsigned char opcode, BinaryGpRegister reg, BinaryGpRegister base, int displacement);
int binary_emit_memory_access_ex(BinaryCodeBuffer *buffer, int operand_size_prefix, int rex_w, unsigned char opcode1, int has_opcode2, unsigned char opcode2, BinaryGpRegister reg, BinaryGpRegister base, int displacement);
int binary_emit_prefetcht0_mem(BinaryCodeBuffer *buffer, BinaryGpRegister base, int displacement);
int binary_emit_memory_access_sib(BinaryCodeBuffer *buffer, int operand_size_prefix, int rex_w, unsigned char opcode1, int has_opcode2, unsigned char opcode2, BinaryGpRegister reg, BinaryGpRegister base, BinaryGpRegister index, int scale, int displacement);
int binary_emit_memory_access_sib_forced(BinaryCodeBuffer *buffer, int operand_size_prefix, int rex_w, unsigned char opcode1, int has_opcode2, unsigned char opcode2, BinaryGpRegister reg, BinaryGpRegister base, BinaryGpRegister index, int scale, int displacement);
int binary_emit_memory_access_ex_forced(BinaryCodeBuffer *buffer, int operand_size_prefix, int rex_w, unsigned char opcode1, int has_opcode2, unsigned char opcode2, BinaryGpRegister reg, BinaryGpRegister base, int displacement);
#define BINARY_LOOP_ALIGN 16u
#define BINARY_LOOP_ALIGN_MAX_PAD 11u
#define BINARY_LOOP_ALIGN_TIGHT_MAX_PAD 15u
#define BINARY_LOOP_TIGHT_MIR_INSTRUCTIONS 20u
#define BINARY_FUNCTION_LINE_ALIGN 64u
#define BINARY_LOOP_ALIGN_BIG 32u
#define BINARY_LOOP_ALIGN_BIG_MAX_PAD 31u
#define BINARY_LOOP_BIG_MIR_INSTRUCTIONS 32u
#define BINARY_JUMP_TARGET_ALIGN 16u
#define BINARY_LOOP_BIG_IR_INSTRUCTIONS 10u

int binary_emit_align_code(BinaryCodeBuffer *buffer, size_t boundary, size_t max_pad);
int binary_emit_mov_mem_imm32(BinaryCodeBuffer *buffer, BinaryGpRegister base, int displacement, int32_t immediate);
int binary_emit_mov_mem_imm_width(BinaryCodeBuffer *buffer, BinaryGpRegister base, int has_index, BinaryGpRegister index, int scale, int displacement, long long value, int width);
int binary_emit_cmp_mem_imm_width(BinaryCodeBuffer *buffer, BinaryGpRegister base, int has_index, BinaryGpRegister index, int scale, int displacement, long long value, int width);
int binary_emit_mov_mem_reg(BinaryCodeBuffer *buffer, BinaryGpRegister base, int displacement, BinaryGpRegister source);
int binary_emit_mov_mem_reg16(BinaryCodeBuffer *buffer, BinaryGpRegister base, int displacement, BinaryGpRegister source);
int binary_emit_mov_mem_reg32(BinaryCodeBuffer *buffer, BinaryGpRegister base, int displacement, BinaryGpRegister source);
int binary_emit_mov_mem_reg8(BinaryCodeBuffer *buffer, BinaryGpRegister base, int displacement, BinaryGpRegister source);
int binary_emit_mov_mem_rip_reg(BinaryCodeBuffer *buffer, BinaryGpRegister source, size_t *displacement_offset_out);
int binary_emit_mov_mem_rip_reg16(BinaryCodeBuffer *buffer, BinaryGpRegister source, size_t *displacement_offset_out);
int binary_emit_mov_mem_rip_reg32(BinaryCodeBuffer *buffer, BinaryGpRegister source, size_t *displacement_offset_out);
int binary_emit_mov_mem_rip_reg8(BinaryCodeBuffer *buffer, BinaryGpRegister source, size_t *displacement_offset_out);
int binary_emit_mov_reg32_rip_mem(BinaryCodeBuffer *buffer, BinaryGpRegister destination, size_t *displacement_offset_out);
int binary_emit_mov_reg_imm32_zero_extend(BinaryCodeBuffer *buffer, BinaryGpRegister destination, uint32_t immediate);
int binary_emit_mov_reg_imm64(BinaryCodeBuffer *buffer, BinaryGpRegister destination, uint64_t immediate);
int binary_emit_mov_reg_mem(BinaryCodeBuffer *buffer, BinaryGpRegister destination, BinaryGpRegister base, int displacement);
int binary_emit_mov_reg_mem32(BinaryCodeBuffer *buffer, BinaryGpRegister destination, BinaryGpRegister base, int displacement);
int binary_emit_mov_reg_reg(BinaryCodeBuffer *buffer, BinaryGpRegister destination, BinaryGpRegister source);
int binary_emit_mov_reg_reg32(BinaryCodeBuffer *buffer, BinaryGpRegister destination, BinaryGpRegister source);
int binary_emit_movzx_reg_reg32(BinaryCodeBuffer *buffer, BinaryGpRegister destination, BinaryGpRegister source);
int binary_emit_mov_reg_rip_mem(BinaryCodeBuffer *buffer, BinaryGpRegister destination, size_t *displacement_offset_out);
int binary_emit_movd_reg_xmm(BinaryCodeBuffer *buffer, BinaryGpRegister destination, BinaryXmmRegister source);
int binary_emit_movd_xmm_reg(BinaryCodeBuffer *buffer, BinaryXmmRegister destination, BinaryGpRegister source);
int binary_emit_movq_reg_xmm(BinaryCodeBuffer *buffer, BinaryGpRegister destination, BinaryXmmRegister source);
int binary_emit_movq_xmm_reg(BinaryCodeBuffer *buffer, BinaryXmmRegister destination, BinaryGpRegister source);
int binary_emit_movsx_reg_reg16(BinaryCodeBuffer *buffer, BinaryGpRegister destination, BinaryGpRegister source);
int binary_emit_movsx_reg_reg8(BinaryCodeBuffer *buffer, BinaryGpRegister destination, BinaryGpRegister source);
int binary_emit_movsxd_rax_eax(BinaryCodeBuffer *buffer);
int binary_emit_movsxd_reg_reg32(BinaryCodeBuffer *buffer, BinaryGpRegister destination, BinaryGpRegister source);
int binary_emit_movzx_reg_reg8(BinaryCodeBuffer *buffer, BinaryGpRegister destination, BinaryGpRegister source);
int binary_emit_movzx_reg_reg16(BinaryCodeBuffer *buffer, BinaryGpRegister destination, BinaryGpRegister source);
int binary_emit_movzx_eax_al(BinaryCodeBuffer *buffer);
int binary_emit_movzx_reg_mem16(BinaryCodeBuffer *buffer, BinaryGpRegister destination, BinaryGpRegister base, int displacement);
int binary_emit_movzx_reg_mem8(BinaryCodeBuffer *buffer, BinaryGpRegister destination, BinaryGpRegister base, int displacement);
int binary_emit_movsx_reg_mem8(BinaryCodeBuffer *buffer, BinaryGpRegister destination, BinaryGpRegister base, int displacement);
int binary_emit_movsx_reg_mem16(BinaryCodeBuffer *buffer, BinaryGpRegister destination, BinaryGpRegister base, int displacement);
int binary_emit_movsxd_reg_mem(BinaryCodeBuffer *buffer, BinaryGpRegister destination, BinaryGpRegister base, int displacement);
int binary_emit_movzx_reg_rip_mem16(BinaryCodeBuffer *buffer, BinaryGpRegister destination, size_t *displacement_offset_out);
int binary_emit_movzx_reg_rip_mem8(BinaryCodeBuffer *buffer, BinaryGpRegister destination, size_t *displacement_offset_out);
int binary_emit_mulsd_xmm_xmm(BinaryCodeBuffer *buffer, BinaryXmmRegister destination, BinaryXmmRegister source);
int binary_emit_mulss_xmm_xmm(BinaryCodeBuffer *buffer, BinaryXmmRegister destination, BinaryXmmRegister source);
int binary_emit_neg_reg(BinaryCodeBuffer *buffer, BinaryGpRegister reg);
int binary_emit_idiv_wrapping(BinaryCodeBuffer *buffer, BinaryGpRegister divisor);
int binary_emit_not_reg(BinaryCodeBuffer *buffer, BinaryGpRegister reg);
int binary_emit_or_reg_imm32(BinaryCodeBuffer *buffer, BinaryGpRegister reg, uint32_t immediate);
int binary_emit_pop_reg(BinaryCodeBuffer *buffer, BinaryGpRegister reg);
int binary_emit_push_reg(BinaryCodeBuffer *buffer, BinaryGpRegister reg);
int binary_emit_pxor_xmm_xmm(BinaryCodeBuffer *buffer, BinaryXmmRegister destination, BinaryXmmRegister source);

uint64_t binary_semantics_float_sign_mask(int float_bits);
int binary_semantics_condition_code(const char *op, int is_unsigned,
                                    unsigned char *out);
int binary_semantics_is_comparison(const char *op);
int binary_emit_ret(BinaryCodeBuffer *buffer);
int binary_emit_rex(BinaryCodeBuffer *buffer, int w, int r, int x, int b);
int binary_emit_rip_relative_access_ex( BinaryCodeBuffer *buffer, int operand_size_prefix, int rex_w, unsigned char opcode1, int has_opcode2, unsigned char opcode2, BinaryGpRegister reg, size_t *displacement_offset_out);
int binary_emit_setcc_reg8(BinaryCodeBuffer *buffer, unsigned char condition_opcode, BinaryGpRegister reg);
int binary_emit_shift_reg_cl(BinaryCodeBuffer *buffer, unsigned char subopcode, BinaryGpRegister reg);
int binary_emit_shift_reg_imm8(BinaryCodeBuffer *buffer, unsigned char subopcode, BinaryGpRegister reg, unsigned char immediate);
int binary_emit_shift_reg_imm8_32(BinaryCodeBuffer *buffer, unsigned char subopcode, BinaryGpRegister reg, unsigned char immediate);
int binary_emit_shift_reg_cl_32(BinaryCodeBuffer *buffer, unsigned char subopcode, BinaryGpRegister reg);
int binary_emit_sse_reg_reg(BinaryCodeBuffer *buffer, unsigned char mandatory_prefix, int rex_w, unsigned char opcode1, unsigned char opcode2, BinaryXmmRegister destination, BinaryXmmRegister source);
int binary_emit_sub_reg_imm32(BinaryCodeBuffer *buffer, BinaryGpRegister reg, uint32_t immediate);
int binary_emit_sub_rsp_imm32(BinaryCodeBuffer *buffer, uint32_t immediate);
int binary_emit_subsd_xmm_xmm(BinaryCodeBuffer *buffer, BinaryXmmRegister destination, BinaryXmmRegister source);
int binary_emit_subss_xmm_xmm(BinaryCodeBuffer *buffer, BinaryXmmRegister destination, BinaryXmmRegister source);
int binary_emit_test_reg_reg(BinaryCodeBuffer *buffer, BinaryGpRegister reg);
int binary_emit_test_reg_imm32(BinaryCodeBuffer *buffer, BinaryGpRegister reg, uint32_t immediate);
int binary_emit_ucomisd_xmm_xmm(BinaryCodeBuffer *buffer, BinaryXmmRegister lhs, BinaryXmmRegister rhs);
int binary_emit_ucomiss_xmm_xmm(BinaryCodeBuffer *buffer, BinaryXmmRegister lhs, BinaryXmmRegister rhs);
int binary_emit_unary_reg(BinaryCodeBuffer *buffer, unsigned char subopcode, BinaryGpRegister reg);
int binary_emit_xor_reg_imm32(BinaryCodeBuffer *buffer, BinaryGpRegister reg, uint32_t immediate);
int binary_emit_xor_reg_reg32(BinaryCodeBuffer *buffer, BinaryGpRegister reg);
int code_generator_binary_emitter_error( CodeGenerator *generator, BinaryEmitter *emitter, const char *fallback);
void binary_function_context_destroy(BinaryFunctionContext *context);
int code_generator_binary_emit_unsigned_int_to_float(BinaryFunctionContext *context, int float_bits, BinaryXmmRegister destination, BinaryGpRegister source, BinaryGpRegister work, BinaryGpRegister odd);
int code_generator_binary_emit_float_to_unsigned_int(BinaryFunctionContext *context, int float_bits, BinaryGpRegister destination, BinaryXmmRegister source, BinaryGpRegister work, BinaryXmmRegister scratch);
int binary_function_context_patch_rel32(BinaryFunctionContext *context, size_t displacement_offset, size_t target_offset);
uint64_t binary_global_const_bits(long long int_value, double float_value, int is_float);
int binary_global_const_table_add(const char *name, long long int_value, double float_value, int is_float, int can_inline_load);
int binary_global_const_table_get(const char *name, uint64_t *value_out);
int binary_global_const_table_rebuild(size_t needed_count);
void binary_global_const_table_reset(void);
int binary_immediate_positive_power_of_two_i32(int32_t value, unsigned char *shift_out);
int binary_ir_function_index_ensure(const IRProgram *program);
void binary_ir_function_index_insert(BinaryIRFunctionIndex *index, IRFunction *function);
void binary_ir_function_index_reset(void);
int binary_label_fixup_table_add(BinaryLabelFixupTable *table, const char *name, size_t displacement_offset);
void binary_label_fixup_table_destroy(BinaryLabelFixupTable *table);
int binary_label_table_define(BinaryLabelTable *table, const char *name, size_t offset);
void binary_label_table_destroy(BinaryLabelTable *table);
int binary_named_slot_table_add(BinaryNamedSlotTable *table, const char *name, int offset);
int binary_named_slot_table_set(BinaryNamedSlotTable *table, const char *name, int offset);
void binary_named_slot_table_destroy(BinaryNamedSlotTable *table);
int binary_named_slot_table_get_offset(const BinaryNamedSlotTable *table, const char *name);
void binary_offset_table_destroy(BinaryOffsetTable *table);
int binary_symbol_alias_table_add(BinarySymbolAliasTable *table, const char *name, const char *target);
void binary_symbol_alias_table_destroy(BinarySymbolAliasTable *table);
const char * binary_symbol_alias_table_get(const BinarySymbolAliasTable *table, const char *name);
int code_generator_binary_collect_global_constants(CodeGenerator *generator);
int code_generator_binary_collect_symbol_aliases( CodeGenerator *generator, BinaryFunctionContext *context, IRFunction *ir_function);
int code_generator_binary_context_add_saved_register( BinaryFunctionContext *context, BinaryGpRegister reg);
int code_generator_binary_context_add_saved_xmm_register( BinaryFunctionContext *context, BinaryXmmRegister reg);
int code_generator_binary_declare_external_symbol( CodeGenerator *generator, const char *symbol_name);
int code_generator_binary_emit_count_word_starts( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_emit_cstring_literal_address( CodeGenerator *generator, BinaryFunctionContext *context, const char *value, BinaryGpRegister target_register);
int code_generator_binary_emit_destination_store( CodeGenerator *generator, BinaryFunctionContext *context, const IROperand *destination, BinaryGpRegister source_register);
int code_generator_binary_emit_global_string_variable( CodeGenerator *generator, const char *link_name, const char *value, size_t value_length);
int code_generator_binary_emit_global_symbol_load( CodeGenerator *generator, BinaryFunctionContext *context, const char *symbol_name, const MtlcType *type, int declare_external, BinaryGpRegister target_register);
int code_generator_binary_emit_global_symbol_store( CodeGenerator *generator, BinaryFunctionContext *context, const char *symbol_name, const MtlcType *type, int declare_external, BinaryGpRegister source_register);
int code_generator_binary_emit_load_from_address( CodeGenerator *generator, BinaryFunctionContext *context, BinaryGpRegister address_register, int size, BinaryGpRegister target_register);
int code_generator_binary_emit_local_string_store( CodeGenerator *generator, BinaryFunctionContext *context, int offset, BinaryGpRegister source_register);
int code_generator_binary_emit_simd_copy(CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_emit_memcpy_inline( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_emit_operand_load( CodeGenerator *generator, BinaryFunctionContext *context, const IROperand *operand, BinaryGpRegister target_register);
int code_generator_binary_emit_profile_enter(CodeGenerator *generator,
                                             BinaryFunctionContext *context,
                                             uint32_t fn_id);
int code_generator_binary_emit_profile_op(CodeGenerator *generator,
                                          BinaryFunctionContext *context,
                                          uint32_t op_class,
                                          uint64_t amount);
int code_generator_binary_emit_profile_exit(CodeGenerator *generator,
                                            BinaryFunctionContext *context);
int code_generator_binary_emit_promoted_global_loads(CodeGenerator *generator,
                                                     BinaryFunctionContext *context);
int code_generator_binary_emit_promoted_global_stores(CodeGenerator *generator,
                                                      BinaryFunctionContext *context);
int code_generator_binary_emit_profile_tables(CodeGenerator *generator);
int code_generator_binary_emit_dwarf_debug_sections(CodeGenerator *generator);
int code_generator_binary_emit_runtime_debug_tables(CodeGenerator *generator);
int code_generator_binary_emit_crash_startup(CodeGenerator *generator);
int code_generator_binary_emit_elf_runtime_hooks(CodeGenerator *generator);
int code_generator_binary_emit_runtime_location_marker(
    CodeGenerator *generator, BinaryFunctionContext *context,
    size_t source_line, size_t source_column, const char *filename);
int code_generator_binary_record_debug_label_export(
    BinaryFunctionContext *context, const char *name, size_t offset);
int code_generator_binary_export_debug_symbols(
    CodeGenerator *generator, BinaryFunctionContext *context,
    size_t text_section, size_t function_offset, size_t end_offset);
int code_generator_binary_emit_rep_movsb( CodeGenerator *generator, BinaryFunctionContext *context, BinaryGpRegister src_addr_reg, BinaryGpRegister dst_addr_reg, size_t size);
int code_generator_binary_emit_rep_movsq( CodeGenerator *generator, BinaryFunctionContext *context, BinaryGpRegister src_addr_reg, BinaryGpRegister dst_addr_reg, size_t qword_count);
int code_generator_binary_emit_runtime_trap_call( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_emit_simd_clamp_i32( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_emit_simd_dot_i32( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_emit_simd_dot_i8( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_emit_simd_exp_f32( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_emit_simd_silu_f32_inline(BinaryCodeBuffer *b, int has_mul);
int code_generator_binary_emit_simd_slp_mac_i32_loop(BinaryCodeBuffer *b, long long K);
int code_generator_binary_emit_simd_slp_mac_i8_loop(BinaryCodeBuffer *b, long long K);
int code_generator_binary_emit_vzeroupper(BinaryCodeBuffer *b);
int code_generator_binary_emit_simd_insertion_sort_i32( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_emit_simd_reverse_copy_i32( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_emit_lower_bound_i32( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_emit_simd_scale_i32( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_emit_simd_sum_i32( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_emit_simd_lcg_u32( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_emit_simd_sum_u8( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_emit_simd_byte_map( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_emit_simd_fill_splat(BinaryCodeBuffer *b, long long size);
int code_generator_binary_emit_simd_fill_loop_mode0(BinaryCodeBuffer *b, long long size);
int code_generator_binary_emit_simd_fill_loop_bytewalk(BinaryCodeBuffer *b, long long size, int mode);
int code_generator_binary_emit_simd_affine_map_f32_loop(BinaryCodeBuffer *b, int b_is_one, int b_is_zero, int c_is_zero);
int code_generator_binary_emit_simd_affine_map_f32_inline(BinaryCodeBuffer *b, unsigned a_bits, unsigned b_bits, unsigned c_bits, int b_is_one, int b_is_zero, int c_is_zero, int a_runtime);
int code_generator_binary_emit_simd_affine_map_f64_loop(BinaryCodeBuffer *b, int b_is_one, int b_is_zero, int c_is_zero);
int code_generator_binary_emit_simd_affine_map_f64_inline(BinaryCodeBuffer *b, unsigned long long a_bits, unsigned long long b_bits, unsigned long long c_bits, int b_is_one, int b_is_zero, int c_is_zero, int a_runtime);
int code_generator_binary_emit_prefix_sum_i32( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_emit_simd_minmax_i32( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_emit_simd_sum_f64( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_emit_simd_sum_f32( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_emit_simd_dot_f64( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_emit_simd_dot_f32( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_emit_simd_multidot_f32( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_emit_simd_affine_map_f64( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_emit_simd_affine_map_f32( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_emit_simd_i2f_reduce_f64( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_emit_simd_vloop_f64( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction, int operands_marshaled);
int code_generator_binary_emit_simd_vloop_unmarshaled( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_vloop_collect_dist(const IRInstruction *in, int is_reduce, const char *names[4], const IROperand *srcs[4], int *n_out);
int code_generator_binary_emit_simd_find( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_emit_simd_outer_lane_f64( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_emit_store_to_address( CodeGenerator *generator, BinaryFunctionContext *context, BinaryGpRegister address_register, int size, BinaryGpRegister source_register);
int code_generator_binary_emit_string_literal_value_address( CodeGenerator *generator, BinaryFunctionContext *context, const char *value, size_t value_length, BinaryGpRegister target_register);
int code_generator_binary_emit_string_symbol_load( CodeGenerator *generator, BinaryFunctionContext *context, const char *symbol_name, const CgSym *symbol, BinaryGpRegister target_register);
int code_generator_binary_emit_symbol_address( CodeGenerator *generator, BinaryFunctionContext *context, const char *symbol_name, int declare_external, BinaryGpRegister target_register);
int code_generator_binary_function_can_promote_rsi_rdi( CodeGenerator *generator, IRFunction *function, const MtlcType *return_type);
int code_generator_binary_function_has_calls(const IRFunction *function);
size_t code_generator_binary_function_symbol_score( const BinaryFunctionContext *context, const IRFunction *function, const char *name, const size_t *loop_weights);
int code_generator_binary_get_access_size(CodeGenerator *generator, BinaryFunctionContext *context, const IROperand *size_operand);
int code_generator_binary_get_local_offset(BinaryFunctionContext *context, const char *name);
int code_generator_binary_get_parameter_offset( BinaryFunctionContext *context, const char *name);
int binary_asm_relocation_table_add(BinaryAsmRelocationTable *table, const char *symbol_name, size_t offset, int kind, int32_t addend);
void binary_asm_relocation_table_destroy(BinaryAsmRelocationTable *table);
int code_generator_binary_assemble_text(CodeGenerator *generator, BinaryFunctionContext *context, const char *text, int bits, int allow_bits_directive, const SourceLocation *location, int *final_bits_out);
int code_generator_binary_emit_inline_asm(CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int ir_function_has_inline_asm(const IRFunction *function);
int code_generator_emit_binary_naked_function(CodeGenerator *generator, IRFunction *ir_function, BinaryFunctionContext *context);
int code_generator_binary_check_interrupt_signature(CodeGenerator *generator, IRFunction *ir_function);
int code_generator_binary_emit_interrupt_entry(CodeGenerator *generator, IRFunction *ir_function, BinaryFunctionContext *context);
int code_generator_binary_emit_interrupt_exit(CodeGenerator *generator, IRFunction *ir_function, BinaryFunctionContext *context);
int code_generator_emit_binary_function_x86_16(CodeGenerator *generator, IRFunction *ir_function, BinaryFunctionContext *context);
int code_generator_binary_get_symbol_offset(BinaryFunctionContext *context, const char *name);
const CgSym *code_generator_binary_value_symbol(CodeGenerator *generator, BinaryFunctionContext *context, const char *name);
int code_generator_binary_get_temp_offset(BinaryFunctionContext *context, const char *name);
int code_generator_binary_global_is_written(IRProgram *ir_program, const char *name);
int code_generator_binary_gp_register_is_win64_nonvolatile( BinaryGpRegister reg);
int code_generator_binary_immediate_fits_signed_32(long long value);
int code_generator_binary_bitwise_imm_wants_operand_size_32(
    CodeGenerator *generator, BinaryFunctionContext *context, const char *op,
    const IROperand *value, long long immediate);
int code_generator_binary_instruction_result_float_bits( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_instruction_result_is_float64( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_instruction_writes_dest(IROpcode op);
int code_generator_binary_is_marked_float64_symbol( const BinaryFunctionContext *context, const char *name);
int code_generator_binary_load_needs_sign_extend( CodeGenerator *generator, BinaryFunctionContext *context, const IROperand *destination, int load_size);
int code_generator_binary_mark_float_symbol( BinaryFunctionContext *context, const char *name, int bits);
int code_generator_binary_marked_symbol_float_bits( const BinaryFunctionContext *context, const char *name);
int code_generator_binary_named_type_float_bits(CodeGenerator *generator, const char *type_name);
int code_generator_binary_operand_float_bits( CodeGenerator *generator, BinaryFunctionContext *context, const IROperand *operand);
int code_generator_binary_operand_is_known_float64( CodeGenerator *generator, BinaryFunctionContext *context, const IROperand *operand);
int code_generator_binary_operand_mentions_symbol( const IROperand *operand, const char *name);
int code_generator_binary_operand_mentions_symbol_or_alias( const BinaryFunctionContext *context, const IROperand *operand, const char *name);
int code_generator_binary_prepare_function_context( CodeGenerator *generator, IRFunction *ir_function, BinaryFunctionContext *context);
int binary_function_local_is_safety_described(const IRFunction *function,
                                              const char *name);
void binary_operand_type_index_destroy(BinaryOperandTypeIndex *ix);
int code_generator_binary_promote_hot_symbols( CodeGenerator *generator, BinaryFunctionContext *context, IRFunction *ir_function);
int code_generator_binary_resolve_fixups(CodeGenerator *generator, BinaryFunctionContext *context, size_t return_offset);
int code_generator_binary_resolved_type_float_bits(const MtlcType *type);
int code_generator_binary_resolved_type_is_abi_supported(const MtlcType *type, int allow_void);
int code_generator_binary_resolved_type_is_float64(const MtlcType *type);
int code_generator_binary_resolved_type_is_signed_integer(const MtlcType *type);
int code_generator_binary_resolved_type_is_stack_scalar(const MtlcType *type);
int code_generator_binary_resolved_type_is_supported(const MtlcType *type, int allow_void);
int code_generator_binary_resolved_type_scalar_size(const MtlcType *type);
int code_generator_binary_symbol_already_promoted( BinaryFunctionContext *context, const char *name);
int code_generator_binary_symbol_assigned_register( CodeGenerator *generator, BinaryFunctionContext *context, const char *name, BinaryGpRegister *register_out);
int code_generator_binary_symbol_is_scalar_accessible( CodeGenerator *generator, const char *name);
int code_generator_binary_type_scalar_width(const MtlcType *type);
int code_generator_binary_emit_temp_stack_load( CodeGenerator *generator, BinaryFunctionContext *context, int stack_offset, BinaryGpRegister target_register, const MtlcType *type);
int code_generator_binary_emit_reg_reg_move( BinaryCodeBuffer *buffer, BinaryGpRegister destination, BinaryGpRegister source, const MtlcType *type);
int code_generator_binary_emit_symbol_stack_load( CodeGenerator *generator, BinaryFunctionContext *context, const MtlcType *type, int stack_offset, BinaryGpRegister target_register);
int code_generator_binary_emit_symbol_stack_store( CodeGenerator *generator, BinaryFunctionContext *context, const MtlcType *type, int stack_offset, BinaryGpRegister source_register);
size_t code_generator_binary_symbol_write_count( const IRFunction *function, const char *name);
int code_generator_binary_type_is_abi_supported(CodeGenerator *generator, const char *type_name, int allow_void);
int code_generator_binary_type_is_cstring(const MtlcType *type);
int code_generator_binary_type_is_direct_aggregate(const MtlcType *type);
int code_generator_binary_type_is_gp_promotable(const MtlcType *type);
int code_generator_binary_type_is_string(MtlcType *type);
const MtlcType *code_generator_binary_indirect_callee_type( CodeGenerator *generator, BinaryFunctionContext *context, const IRInstruction *instruction);
int code_generator_binary_validate_signature(CodeGenerator *generator, IRFunction *ir_function);
int code_generator_declare_binary_externs(CodeGenerator *generator);
int code_generator_emit_binary_function(CodeGenerator *generator,
                                        IRFunction *ir_function);
int code_generator_emit_binary_global_variable(CodeGenerator *generator, const IRModuleSymbol *sym);
int code_generator_generate_program_binary_object(CodeGenerator *generator);
int simd_emit_xmm_mem_disp(BinaryCodeBuffer *b, unsigned char opcode, int xmm, int gpr, int displacement);
int simd_emit_prefixed_xmm_mem_disp(BinaryCodeBuffer *b, unsigned char prefix, unsigned char opcode, int xmm, int gpr, int displacement);
int simd_movdqu_mem_xmm_disp(BinaryCodeBuffer *b, int gpr, int displacement, int xmm);
int simd_movdqu_xmm_mem_disp(BinaryCodeBuffer *b, int xmm, int gpr, int displacement);
int wcs_accumulate_xmm0_i32_to_rax(BinaryCodeBuffer *b);
int wcs_add_reg_reg64(BinaryCodeBuffer *b, int dst, int src);
int wcs_addsub_reg_imm8(BinaryCodeBuffer *b, int gpr, int is_sub, unsigned char imm);
int wcs_and_reg_reg(BinaryCodeBuffer *b, int dst, int src);
int wcs_cmp_reg_imm32(BinaryCodeBuffer *b, int gpr, uint32_t imm);
int wcs_cmp_reg_imm8(BinaryCodeBuffer *b, int gpr, unsigned char imm);
int wcs_cmp_reg_reg32(BinaryCodeBuffer *b, int dst, int src);
int wcs_jcc(BinaryCodeBuffer *b, unsigned char cc, size_t *disp_off);
int wcs_mov_reg_imm32(BinaryCodeBuffer *b, int gpr, uint32_t imm);
int wcs_mov_reg_reg32(BinaryCodeBuffer *b, int dst, int src);
int wcs_movd_reg_xmm(BinaryCodeBuffer *b, int gpr, int xmm);
int wcs_movd_xmm_reg(BinaryCodeBuffer *b, int xmm, int gpr);
int wcs_avx_vcvtph2ps_xmm(BinaryCodeBuffer *b, int dst, int src);
int wcs_avx_vcvtps2ph_xmm(BinaryCodeBuffer *b, int dst, int src, unsigned char imm);
int wcs_movdqu_xmm_rcx(BinaryCodeBuffer *b, int xmm);
int wcs_movzx_reg_byte_rcx(BinaryCodeBuffer *b, int gpr);
int wcs_not_reg(BinaryCodeBuffer *b, int gpr);
int wcs_or_reg_reg(BinaryCodeBuffer *b, int dst, int src);
int wcs_paddd(BinaryCodeBuffer *b, int dst, int src);
int wcs_paddq(BinaryCodeBuffer *b, int dst, int src);
int wcs_patch_here(BinaryCodeBuffer *b, size_t disp_off);
int wcs_patch_both_here(BinaryCodeBuffer *b, size_t first, size_t second);
int wcs_patch_to(BinaryCodeBuffer *b, size_t disp_off, size_t target);
int wcs_pmaxsd(BinaryCodeBuffer *b, int dst, int src);
int wcs_pminsd(BinaryCodeBuffer *b, int dst, int src);
int wcs_pmovmskb(BinaryCodeBuffer *b, int gpr, int xmm);
int wcs_popcnt(BinaryCodeBuffer *b, int dst, int src);
int wcs_popcnt_sized(BinaryCodeBuffer *b, int dst, int src, int wide);
int wcs_pshufd(BinaryCodeBuffer *b, int dst, int src, unsigned char imm);
int wcs_shift_reg_imm(BinaryCodeBuffer *b, int gpr, int is_shr, unsigned char imm);
int wcs_sse_66(BinaryCodeBuffer *b, unsigned char op, int dst, int src);
int wcs_sse_66_38(BinaryCodeBuffer *b, unsigned char op, int dst, int src);
int wcs_sub_reg_reg32(BinaryCodeBuffer *b, int dst, int src);
int wcs_sub_reg_reg64(BinaryCodeBuffer *b, int dst, int src);
int wcs_test_reg_reg32(BinaryCodeBuffer *b, int gpr);
int wcs_xor_self32(BinaryCodeBuffer *b, int gpr);

#endif
