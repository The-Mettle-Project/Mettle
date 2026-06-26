#include "codegen/binary/arm64.h"

#define R5(x) ((uint32_t)((x) & 0x1F))

Arm64Cond arm64_invert_cond(Arm64Cond c) {
  /* The architecture pairs conditions by toggling bit 0 (EQ<->NE, GE<->LT,
   * GT<->LE, HI<->LS, CS<->CC, MI<->PL, VS<->VC). AL/NV have no useful
   * inverse but toggling keeps the table total. */
  return (Arm64Cond)((int)c ^ 1);
}

uint32_t arm64_nop(void) { return 0xD503201Fu; }

uint32_t arm64_mov_reg(int is64, Arm64Reg rd, Arm64Reg rm) {
  uint32_t base = is64 ? 0xAA0003E0u : 0x2A0003E0u; /* ORR Rd, ZR, Rm */
  return base | (R5(rm) << 16) | R5(rd);
}

/* mov to/from SP == ADD rd, rn, #0 (reg 31 = SP in the add-immediate slot). */
uint32_t arm64_mov_sp(Arm64Reg rd, Arm64Reg rn) {
  return arm64_add_imm(1, rd, rn, 0, 0);
}

/* Move wide (immediate): sf|opc|100101|hw(2)|imm16|Rd. */
uint32_t arm64_movz(int is64, Arm64Reg rd, uint16_t imm16, int hw) {
  uint32_t base = is64 ? 0xD2800000u : 0x52800000u; /* opc=10 */
  return base | (((uint32_t)hw & 3u) << 21) | ((uint32_t)imm16 << 5) | R5(rd);
}
uint32_t arm64_movn(int is64, Arm64Reg rd, uint16_t imm16, int hw) {
  uint32_t base = is64 ? 0x92800000u : 0x12800000u; /* opc=00 */
  return base | (((uint32_t)hw & 3u) << 21) | ((uint32_t)imm16 << 5) | R5(rd);
}
uint32_t arm64_movk(int is64, Arm64Reg rd, uint16_t imm16, int hw) {
  uint32_t base = is64 ? 0xF2800000u : 0x72800000u; /* opc=11 */
  return base | (((uint32_t)hw & 3u) << 21) | ((uint32_t)imm16 << 5) | R5(rd);
}

/* Arithmetic (shifted register): sf|op|S|01011|shift|0|Rm|imm6|Rn|Rd. */
uint32_t arm64_add_reg(int is64, Arm64Reg rd, Arm64Reg rn, Arm64Reg rm) {
  uint32_t base = is64 ? 0x8B000000u : 0x0B000000u;
  return base | (R5(rm) << 16) | (R5(rn) << 5) | R5(rd);
}
uint32_t arm64_sub_reg(int is64, Arm64Reg rd, Arm64Reg rn, Arm64Reg rm) {
  uint32_t base = is64 ? 0xCB000000u : 0x4B000000u;
  return base | (R5(rm) << 16) | (R5(rn) << 5) | R5(rd);
}
/* Logical (shifted register). */
uint32_t arm64_and_reg(int is64, Arm64Reg rd, Arm64Reg rn, Arm64Reg rm) {
  uint32_t base = is64 ? 0x8A000000u : 0x0A000000u;
  return base | (R5(rm) << 16) | (R5(rn) << 5) | R5(rd);
}
uint32_t arm64_orr_reg(int is64, Arm64Reg rd, Arm64Reg rn, Arm64Reg rm) {
  uint32_t base = is64 ? 0xAA000000u : 0x2A000000u;
  return base | (R5(rm) << 16) | (R5(rn) << 5) | R5(rd);
}
uint32_t arm64_eor_reg(int is64, Arm64Reg rd, Arm64Reg rn, Arm64Reg rm) {
  uint32_t base = is64 ? 0xCA000000u : 0x4A000000u;
  return base | (R5(rm) << 16) | (R5(rn) << 5) | R5(rd);
}
uint32_t arm64_neg(int is64, Arm64Reg rd, Arm64Reg rm) {
  return arm64_sub_reg(is64, rd, ARM64_XZR, rm);
}
/* MVN == ORN Xd, XZR, Xm (ORR with N=1, bit 21 set). */
uint32_t arm64_mvn(int is64, Arm64Reg rd, Arm64Reg rm) {
  uint32_t base = is64 ? 0xAA2003E0u : 0x2A2003E0u;
  return base | (R5(rm) << 16) | R5(rd);
}

/* Add/subtract (immediate): sf|op|S|100010|sh|imm12|Rn|Rd. */
uint32_t arm64_add_imm(int is64, Arm64Reg rd, Arm64Reg rn, uint32_t imm12,
                       int shift12) {
  uint32_t base = is64 ? 0x91000000u : 0x11000000u;
  return base | (shift12 ? (1u << 22) : 0u) | ((imm12 & 0xFFFu) << 10) |
         (R5(rn) << 5) | R5(rd);
}
uint32_t arm64_sub_imm(int is64, Arm64Reg rd, Arm64Reg rn, uint32_t imm12,
                       int shift12) {
  uint32_t base = is64 ? 0xD1000000u : 0x51000000u;
  return base | (shift12 ? (1u << 22) : 0u) | ((imm12 & 0xFFFu) << 10) |
         (R5(rn) << 5) | R5(rd);
}

/* CMP rn,rm == SUBS XZR,rn,rm (arithmetic shifted reg, op=1,S=1, Rd=31). */
uint32_t arm64_cmp_reg(int is64, Arm64Reg rn, Arm64Reg rm) {
  uint32_t base = is64 ? 0xEB00001Fu : 0x6B00001Fu;
  return base | (R5(rm) << 16) | (R5(rn) << 5);
}
/* CMP rn,#imm == SUBS XZR,rn,#imm (add/sub imm, op=1,S=1, Rd=31). */
uint32_t arm64_cmp_imm(int is64, Arm64Reg rn, uint32_t imm12, int shift12) {
  uint32_t base = is64 ? 0xF100001Fu : 0x7100001Fu;
  return base | (shift12 ? (1u << 22) : 0u) | ((imm12 & 0xFFFu) << 10) |
         (R5(rn) << 5);
}
/* TST rn,rm == ANDS XZR,rn,rm (logical shifted register, opc=11, Rd=31). */
uint32_t arm64_tst(int is64, Arm64Reg rn, Arm64Reg rm) {
  uint32_t base = is64 ? 0xEA00001Fu : 0x6A00001Fu;
  return base | (R5(rm) << 16) | (R5(rn) << 5);
}

/* Extends are UBFM/SBFM aliases: UXTB Wd,Wn=UBFM Wd,Wn,#0,#7; SXTB Xd,Wn=
 * SBFM Xd,Xn,#0,#7; widths 7/15/31 select byte/half/word. */
uint32_t arm64_uxtb(Arm64Reg rd, Arm64Reg rn) {
  return 0x53000000u | (7u << 10) | (R5(rn) << 5) | R5(rd);
}
uint32_t arm64_uxth(Arm64Reg rd, Arm64Reg rn) {
  return 0x53000000u | (15u << 10) | (R5(rn) << 5) | R5(rd);
}
uint32_t arm64_sxtb(Arm64Reg rd, Arm64Reg rn) {
  return 0x93400000u | (7u << 10) | (R5(rn) << 5) | R5(rd);
}
uint32_t arm64_sxth(Arm64Reg rd, Arm64Reg rn) {
  return 0x93400000u | (15u << 10) | (R5(rn) << 5) | R5(rd);
}
uint32_t arm64_sxtw(Arm64Reg rd, Arm64Reg rn) {
  return 0x93400000u | (31u << 10) | (R5(rn) << 5) | R5(rd);
}

/* Data-processing (3 source): sf|00|11011|op31|Rm|o0|Ra|Rn|Rd.
 * MADD o0=0, MSUB o0=1 (bit 15). */
uint32_t arm64_madd(int is64, Arm64Reg rd, Arm64Reg rn, Arm64Reg rm,
                    Arm64Reg ra) {
  uint32_t base = is64 ? 0x9B000000u : 0x1B000000u;
  return base | (R5(rm) << 16) | (R5(ra) << 10) | (R5(rn) << 5) | R5(rd);
}
uint32_t arm64_msub(int is64, Arm64Reg rd, Arm64Reg rn, Arm64Reg rm,
                    Arm64Reg ra) {
  uint32_t base = is64 ? 0x9B008000u : 0x1B008000u;
  return base | (R5(rm) << 16) | (R5(ra) << 10) | (R5(rn) << 5) | R5(rd);
}
uint32_t arm64_mul(int is64, Arm64Reg rd, Arm64Reg rn, Arm64Reg rm) {
  return arm64_madd(is64, rd, rn, rm, ARM64_XZR);
}
/* Data-processing (2 source): sf|0|S|11010110|Rm|opcode|Rn|Rd. */
uint32_t arm64_sdiv(int is64, Arm64Reg rd, Arm64Reg rn, Arm64Reg rm) {
  uint32_t base = is64 ? 0x9AC00C00u : 0x1AC00C00u; /* opcode=000011 */
  return base | (R5(rm) << 16) | (R5(rn) << 5) | R5(rd);
}
uint32_t arm64_udiv(int is64, Arm64Reg rd, Arm64Reg rn, Arm64Reg rm) {
  uint32_t base = is64 ? 0x9AC00800u : 0x1AC00800u; /* opcode=000010 */
  return base | (R5(rm) << 16) | (R5(rn) << 5) | R5(rd);
}

/* ---- variable + immediate shifts ---------------------------------------- */

uint32_t arm64_lslv(int is64, Arm64Reg rd, Arm64Reg rn, Arm64Reg rm) {
  uint32_t base = is64 ? 0x9AC02000u : 0x1AC02000u; /* opcode=001000 */
  return base | (R5(rm) << 16) | (R5(rn) << 5) | R5(rd);
}
uint32_t arm64_lsrv(int is64, Arm64Reg rd, Arm64Reg rn, Arm64Reg rm) {
  uint32_t base = is64 ? 0x9AC02400u : 0x1AC02400u; /* opcode=001001 */
  return base | (R5(rm) << 16) | (R5(rn) << 5) | R5(rd);
}
uint32_t arm64_asrv(int is64, Arm64Reg rd, Arm64Reg rn, Arm64Reg rm) {
  uint32_t base = is64 ? 0x9AC02800u : 0x1AC02800u; /* opcode=001010 */
  return base | (R5(rm) << 16) | (R5(rn) << 5) | R5(rd);
}

/* Bitfield move: sf|opc|100110|N|immr|imms|Rn|Rd. UBFM opc=10, SBFM opc=00;
 * for 64-bit forms N=1 (bit 22). The immediate shift aliases:
 *   LSL #s = UBFM Rd,Rn,#(-s MOD width),#(width-1-s)
 *   LSR #s = UBFM Rd,Rn,#s,#(width-1)
 *   ASR #s = SBFM Rd,Rn,#s,#(width-1)  */
static uint32_t arm64_bfm(int is64, int sbfm, Arm64Reg rd, Arm64Reg rn,
                          int immr, int imms) {
  uint32_t base;
  int mask = is64 ? 63 : 31;
  if (sbfm) {
    base = is64 ? 0x93400000u : 0x13000000u;
  } else {
    base = is64 ? 0xD3400000u : 0x53000000u;
  }
  return base | (((uint32_t)(immr & mask)) << 16) |
         (((uint32_t)(imms & mask)) << 10) | (R5(rn) << 5) | R5(rd);
}
uint32_t arm64_lsl_imm(int is64, Arm64Reg rd, Arm64Reg rn, int shift) {
  int width = is64 ? 64 : 32;
  int immr = (-shift) & (width - 1);
  int imms = width - 1 - shift;
  return arm64_bfm(is64, 0, rd, rn, immr, imms);
}
uint32_t arm64_lsr_imm(int is64, Arm64Reg rd, Arm64Reg rn, int shift) {
  int width = is64 ? 64 : 32;
  return arm64_bfm(is64, 0, rd, rn, shift, width - 1);
}
uint32_t arm64_asr_imm(int is64, Arm64Reg rd, Arm64Reg rn, int shift) {
  int width = is64 ? 64 : 32;
  return arm64_bfm(is64, 1, rd, rn, shift, width - 1);
}


/* Conditional select: sf|op|S|11010100|Rm|cond|op2|Rn|Rd.
 * CSEL op2=00; CSINC op2=01 (bit 10). */
uint32_t arm64_csel(int is64, Arm64Reg rd, Arm64Reg rn, Arm64Reg rm,
                    Arm64Cond cond) {
  uint32_t base = is64 ? 0x9A800000u : 0x1A800000u;
  return base | (R5(rm) << 16) | (((uint32_t)cond & 0xF) << 12) |
         (R5(rn) << 5) | R5(rd);
}
/* CSET Rd,cond == CSINC Rd,ZR,ZR,invert(cond). */
uint32_t arm64_cset(int is64, Arm64Reg rd, Arm64Cond cond) {
  uint32_t base = is64 ? 0x9A800400u : 0x1A800400u; /* CSINC, Rm=Rn=ZR below */
  Arm64Cond inv = arm64_invert_cond(cond);
  return base | (R5(ARM64_XZR) << 16) | (((uint32_t)inv & 0xF) << 12) |
         (R5(ARM64_XZR) << 5) | R5(rd);
}

uint32_t arm64_str_imm(int is64, Arm64Reg rt, Arm64Reg rn, int offset_bytes) {
  uint32_t base = is64 ? 0xF9000000u : 0xB9000000u;
  uint32_t scaled = (uint32_t)(offset_bytes / (is64 ? 8 : 4)) & 0xFFFu;
  return base | (scaled << 10) | (R5(rn) << 5) | R5(rt);
}
uint32_t arm64_ldr_imm(int is64, Arm64Reg rt, Arm64Reg rn, int offset_bytes) {
  uint32_t base = is64 ? 0xF9400000u : 0xB9400000u;
  uint32_t scaled = (uint32_t)(offset_bytes / (is64 ? 8 : 4)) & 0xFFFu;
  return base | (scaled << 10) | (R5(rn) << 5) | R5(rt);
}

uint32_t arm64_stp_pre(int is64, Arm64Reg rt, Arm64Reg rt2, Arm64Reg rn,
                       int offset_bytes) {
  uint32_t base = is64 ? 0xA9800000u : 0x29800000u;
  int scale = is64 ? 8 : 4;
  uint32_t imm7 = (uint32_t)((offset_bytes / scale) & 0x7F);
  return base | (imm7 << 15) | (R5(rt2) << 10) | (R5(rn) << 5) | R5(rt);
}
uint32_t arm64_ldp_post(int is64, Arm64Reg rt, Arm64Reg rt2, Arm64Reg rn,
                        int offset_bytes) {
  uint32_t base = is64 ? 0xA8C00000u : 0x28C00000u;
  int scale = is64 ? 8 : 4;
  uint32_t imm7 = (uint32_t)((offset_bytes / scale) & 0x7F);
  return base | (imm7 << 15) | (R5(rt2) << 10) | (R5(rn) << 5) | R5(rt);
}

uint32_t arm64_b(int offset_bytes) {
  return 0x14000000u | ((uint32_t)(offset_bytes >> 2) & 0x03FFFFFFu);
}
uint32_t arm64_bl(int offset_bytes) {
  return 0x94000000u | ((uint32_t)(offset_bytes >> 2) & 0x03FFFFFFu);
}
uint32_t arm64_bcond(Arm64Cond cond, int offset_bytes) {
  uint32_t imm19 = (uint32_t)(offset_bytes >> 2) & 0x7FFFFu;
  return 0x54000000u | (imm19 << 5) | ((uint32_t)cond & 0xFu);
}
uint32_t arm64_cbz(int is64, Arm64Reg rt, int offset_bytes) {
  uint32_t base = is64 ? 0xB4000000u : 0x34000000u;
  uint32_t imm19 = (uint32_t)(offset_bytes >> 2) & 0x7FFFFu;
  return base | (imm19 << 5) | R5(rt);
}
uint32_t arm64_cbnz(int is64, Arm64Reg rt, int offset_bytes) {
  uint32_t base = is64 ? 0xB5000000u : 0x35000000u;
  uint32_t imm19 = (uint32_t)(offset_bytes >> 2) & 0x7FFFFu;
  return base | (imm19 << 5) | R5(rt);
}
uint32_t arm64_br(Arm64Reg rn) { return 0xD61F0000u | (R5(rn) << 5); }
uint32_t arm64_blr(Arm64Reg rn) { return 0xD63F0000u | (R5(rn) << 5); }
uint32_t arm64_ret(Arm64Reg rn) { return 0xD65F0000u | (R5(rn) << 5); }
