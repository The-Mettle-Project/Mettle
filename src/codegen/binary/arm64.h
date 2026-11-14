#ifndef CODEGEN_BINARY_ARM64_H
#define CODEGEN_BINARY_ARM64_H

#include <stdint.h>

typedef enum {
  ARM64_X0 = 0, ARM64_X1, ARM64_X2, ARM64_X3, ARM64_X4, ARM64_X5,
  ARM64_X6, ARM64_X7, ARM64_X8, ARM64_X9, ARM64_X10, ARM64_X11,
  ARM64_X12, ARM64_X13, ARM64_X14, ARM64_X15, ARM64_X16, ARM64_X17,
  ARM64_X18, ARM64_X19, ARM64_X20, ARM64_X21, ARM64_X22, ARM64_X23,
  ARM64_X24, ARM64_X25, ARM64_X26, ARM64_X27, ARM64_X28,
  ARM64_X29 = 29,
  ARM64_X30 = 30,
  ARM64_XZR = 31,
  ARM64_SP = 31
} Arm64Reg;

typedef enum {
  ARM64_EQ = 0, ARM64_NE = 1, ARM64_CS = 2, ARM64_CC = 3,
  ARM64_MI = 4, ARM64_PL = 5, ARM64_VS = 6, ARM64_VC = 7,
  ARM64_HI = 8, ARM64_LS = 9, ARM64_GE = 10, ARM64_LT = 11,
  ARM64_GT = 12, ARM64_LE = 13, ARM64_AL = 14, ARM64_NV = 15
} Arm64Cond;

Arm64Cond arm64_invert_cond(Arm64Cond c);

uint32_t arm64_nop(void);
uint32_t arm64_mov_reg(int is64, Arm64Reg rd, Arm64Reg rm);
uint32_t arm64_mov_sp(Arm64Reg rd, Arm64Reg rn);
uint32_t arm64_movz(int is64, Arm64Reg rd, uint16_t imm16, int hw);
uint32_t arm64_movn(int is64, Arm64Reg rd, uint16_t imm16, int hw);
uint32_t arm64_movk(int is64, Arm64Reg rd, uint16_t imm16, int hw);

uint32_t arm64_add_reg(int is64, Arm64Reg rd, Arm64Reg rn, Arm64Reg rm);
uint32_t arm64_sub_reg(int is64, Arm64Reg rd, Arm64Reg rn, Arm64Reg rm);
uint32_t arm64_and_reg(int is64, Arm64Reg rd, Arm64Reg rn, Arm64Reg rm);
uint32_t arm64_orr_reg(int is64, Arm64Reg rd, Arm64Reg rn, Arm64Reg rm);
uint32_t arm64_eor_reg(int is64, Arm64Reg rd, Arm64Reg rn, Arm64Reg rm);
uint32_t arm64_neg(int is64, Arm64Reg rd, Arm64Reg rm);
uint32_t arm64_mvn(int is64, Arm64Reg rd, Arm64Reg rm);

uint32_t arm64_add_imm(int is64, Arm64Reg rd, Arm64Reg rn, uint32_t imm12,
                       int shift12);
uint32_t arm64_sub_imm(int is64, Arm64Reg rd, Arm64Reg rn, uint32_t imm12,
                       int shift12);

uint32_t arm64_cmp_reg(int is64, Arm64Reg rn, Arm64Reg rm);
uint32_t arm64_cmp_imm(int is64, Arm64Reg rn, uint32_t imm12, int shift12);
uint32_t arm64_tst(int is64, Arm64Reg rn, Arm64Reg rm);

uint32_t arm64_uxtb(Arm64Reg rd, Arm64Reg rn);
uint32_t arm64_uxth(Arm64Reg rd, Arm64Reg rn);
uint32_t arm64_sxtb(Arm64Reg rd, Arm64Reg rn);
uint32_t arm64_sxth(Arm64Reg rd, Arm64Reg rn);
uint32_t arm64_sxtw(Arm64Reg rd, Arm64Reg rn);

uint32_t arm64_madd(int is64, Arm64Reg rd, Arm64Reg rn, Arm64Reg rm,
                    Arm64Reg ra);
uint32_t arm64_msub(int is64, Arm64Reg rd, Arm64Reg rn, Arm64Reg rm,
                    Arm64Reg ra);
uint32_t arm64_mul(int is64, Arm64Reg rd, Arm64Reg rn, Arm64Reg rm);
uint32_t arm64_smulh(Arm64Reg rd, Arm64Reg rn, Arm64Reg rm);
int arm64_logical_imm(uint64_t value, uint32_t *fields);
uint32_t arm64_and_imm(Arm64Reg rd, Arm64Reg rn, uint32_t fields);
uint32_t arm64_orr_imm(Arm64Reg rd, Arm64Reg rn, uint32_t fields);
uint32_t arm64_eor_imm(Arm64Reg rd, Arm64Reg rn, uint32_t fields);
uint32_t arm64_umulh(Arm64Reg rd, Arm64Reg rn, Arm64Reg rm);
uint32_t arm64_sdiv(int is64, Arm64Reg rd, Arm64Reg rn, Arm64Reg rm);
uint32_t arm64_udiv(int is64, Arm64Reg rd, Arm64Reg rn, Arm64Reg rm);

uint32_t arm64_lslv(int is64, Arm64Reg rd, Arm64Reg rn, Arm64Reg rm);
uint32_t arm64_lsrv(int is64, Arm64Reg rd, Arm64Reg rn, Arm64Reg rm);
uint32_t arm64_asrv(int is64, Arm64Reg rd, Arm64Reg rn, Arm64Reg rm);
uint32_t arm64_lsl_imm(int is64, Arm64Reg rd, Arm64Reg rn, int shift);
uint32_t arm64_lsr_imm(int is64, Arm64Reg rd, Arm64Reg rn, int shift);
uint32_t arm64_asr_imm(int is64, Arm64Reg rd, Arm64Reg rn, int shift);

uint32_t arm64_csel(int is64, Arm64Reg rd, Arm64Reg rn, Arm64Reg rm,
                    Arm64Cond cond);
uint32_t arm64_cset(int is64, Arm64Reg rd, Arm64Cond cond);

uint32_t arm64_ldr_imm(int is64, Arm64Reg rt, Arm64Reg rn, int offset_bytes);
uint32_t arm64_str_imm(int is64, Arm64Reg rt, Arm64Reg rn, int offset_bytes);
uint32_t arm64_strb_imm(Arm64Reg rt, Arm64Reg rn, int offset);
uint32_t arm64_ldrb_imm(Arm64Reg rt, Arm64Reg rn, int offset);
uint32_t arm64_strh_imm(Arm64Reg rt, Arm64Reg rn, int offset_bytes);
uint32_t arm64_ldrh_imm(Arm64Reg rt, Arm64Reg rn, int offset_bytes);

uint32_t arm64_fadd(int is_double, int fd, int fn, int fm);
uint32_t arm64_fsub(int is_double, int fd, int fn, int fm);
uint32_t arm64_fmul(int is_double, int fd, int fn, int fm);
uint32_t arm64_fdiv(int is_double, int fd, int fn, int fm);
uint32_t arm64_fneg(int is_double, int fd, int fn);
uint32_t arm64_fsqrt(int is_double, int fd, int fn);
uint32_t arm64_fcmp(int is_double, int fn, int fm);
uint32_t arm64_scvtf(int is_double, int fd, Arm64Reg xn);
uint32_t arm64_fcvtzs(int is_double, Arm64Reg xd, int fn);
uint32_t arm64_ucvtf(int is_double, int fd, Arm64Reg xn);
uint32_t arm64_fcvtzu(int is_double, Arm64Reg xd, int fn);
uint32_t arm64_fcvt(int to_double, int fd, int fn);
uint32_t arm64_fcvt_h2s(int sd, int hn);
uint32_t arm64_fcvt_s2h(int hd, int sn);
uint32_t arm64_fmov_gp(int is_double, int fd, Arm64Reg xn);
uint32_t arm64_fmov_to_gp(int is_double, Arm64Reg xd, int fn);
uint32_t arm64_fmov_reg(int is_double, int fd, int fn);
uint32_t arm64_ldr_fp(int is_double, int ft, Arm64Reg rn, int offset_bytes);
uint32_t arm64_str_fp(int is_double, int ft, Arm64Reg rn, int offset_bytes);
uint32_t arm64_stp_pre(int is64, Arm64Reg rt, Arm64Reg rt2, Arm64Reg rn,
                       int offset_bytes);
uint32_t arm64_ldp_post(int is64, Arm64Reg rt, Arm64Reg rt2, Arm64Reg rn,
                        int offset_bytes);

uint32_t arm64_b(int offset_bytes);
uint32_t arm64_bl(int offset_bytes);
uint32_t arm64_bcond(Arm64Cond cond, int offset_bytes);
uint32_t arm64_cbz(int is64, Arm64Reg rt, int offset_bytes);
uint32_t arm64_cbnz(int is64, Arm64Reg rt, int offset_bytes);
uint32_t arm64_br(Arm64Reg rn);
uint32_t arm64_blr(Arm64Reg rn);
uint32_t arm64_ret(Arm64Reg rn);

typedef enum {
  ARM64_DIS_UNKNOWN = 0,
  ARM64_DIS_NOP, ARM64_DIS_MOV, ARM64_DIS_MOVZ, ARM64_DIS_MOVN, ARM64_DIS_MOVK,
  ARM64_DIS_ADD_REG, ARM64_DIS_SUB_REG, ARM64_DIS_AND_REG, ARM64_DIS_ORR_REG,
  ARM64_DIS_EOR_REG, ARM64_DIS_ADD_IMM, ARM64_DIS_SUB_IMM, ARM64_DIS_SUBS_REG,
  ARM64_DIS_SUBS_IMM, ARM64_DIS_MADD, ARM64_DIS_MSUB, ARM64_DIS_SDIV,
  ARM64_DIS_UDIV, ARM64_DIS_LSLV, ARM64_DIS_LSRV, ARM64_DIS_ASRV,
  ARM64_DIS_UBFM, ARM64_DIS_SBFM, ARM64_DIS_CSEL, ARM64_DIS_CSINC,
  ARM64_DIS_LDR_IMM, ARM64_DIS_STR_IMM, ARM64_DIS_STP, ARM64_DIS_LDP,
  ARM64_DIS_B, ARM64_DIS_BL, ARM64_DIS_BCOND, ARM64_DIS_CBZ, ARM64_DIS_CBNZ,
  ARM64_DIS_BR, ARM64_DIS_BLR, ARM64_DIS_RET
} Arm64DisOp;

typedef struct {
  Arm64DisOp op;
  int is64;
  int rd, rn, rm, rt, rt2, ra;
  long long imm;
  int hw;
  int cond;
  int immr, imms;
} Arm64Inst;

Arm64Inst arm64_decode(uint32_t word);

typedef enum {
  ARM64_ARG_IN_GP_REGISTER = 0,
  ARM64_ARG_IN_VEC_REGISTER = 1,
  ARM64_ARG_ON_STACK = 2
} Arm64ArgKind;

typedef struct {
  Arm64ArgKind kind;
  Arm64Reg reg;
  int stack_offset;
} Arm64ArgLocation;

typedef struct {
  const Arm64Reg *gp_arg_regs;
  int gp_arg_count;
  const Arm64Reg *vec_arg_regs;
  int vec_arg_count;
  Arm64Reg indirect_result_reg;
  int stack_slot_bytes;

  const Arm64Reg *gp_callee_saved;
  int gp_callee_saved_count;
  const Arm64Reg *gp_temps;
  int gp_temps_count;
  const Arm64Reg *vec_volatile;
  int vec_volatile_count;
  const Arm64Reg *vec_callee_saved;
  int vec_callee_saved_count;

  Arm64Reg fp, lr, sp;
  Arm64Reg scratch0, scratch1;
  Arm64Reg platform;
} Arm64Abi;

const Arm64Abi *arm64_aapcs64(void);
int arm64_abi_set_gp_args(const Arm64Reg *regs, int count);
void arm64_abi_reset(void);

int arm64_reg_is_callee_saved(Arm64Reg r);
int arm64_reg_is_volatile(Arm64Reg r);
int arm64_reg_arg_index(Arm64Reg r);
int arm64_reg_is_allocatable(Arm64Reg r);

int arm64_compute_arg_layout_packed(const int *is_float,
                                    const int *stack_sizes, int count,
                                    Arm64ArgLocation *out,
                                    int *stack_bytes_out);
int arm64_compute_arg_layout(const int *is_float, int count,
                             Arm64ArgLocation *out, int *stack_bytes_out);

#endif
