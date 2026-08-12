#include "codegen/binary/arm64.h"

static long long sign_extend(uint32_t value, int bits) {
  uint64_t m = (uint64_t)1 << (bits - 1);
  uint64_t v = (uint64_t)value & (((uint64_t)1 << bits) - 1u);
  return (long long)((v ^ m) - m);
}

Arm64Inst arm64_decode(uint32_t w) {
  Arm64Inst i;
  i.op = ARM64_DIS_UNKNOWN;
  i.is64 = (int)(w >> 31);
  i.rd = (int)(w & 0x1F);
  i.rn = (int)((w >> 5) & 0x1F);
  i.rm = (int)((w >> 16) & 0x1F);
  i.rt = (int)(w & 0x1F);
  i.rt2 = (int)((w >> 10) & 0x1F);
  i.ra = (int)((w >> 10) & 0x1F);
  i.imm = 0;
  i.hw = (int)((w >> 21) & 3);
  i.cond = 0;
  i.immr = (int)((w >> 16) & 0x3F);
  i.imms = (int)((w >> 10) & 0x3F);

  /* exact fixed-word forms first */
  if (w == 0xD503201Fu) {
    i.op = ARM64_DIS_NOP;
    return i;
  }
  /* unconditional branch (register): 0xD6.F.0000 with Rn in bits 9-5 */
  if ((w & 0xFFFFFC1Fu) == 0xD61F0000u) { i.op = ARM64_DIS_BR; return i; }
  if ((w & 0xFFFFFC1Fu) == 0xD63F0000u) { i.op = ARM64_DIS_BLR; return i; }
  if ((w & 0xFFFFFC1Fu) == 0xD65F0000u) { i.op = ARM64_DIS_RET; return i; }

  /* move wide (immediate): bits 28-23 == 100101 */
  if (((w >> 23) & 0x3F) == 0x25) {
    int opc = (int)((w >> 29) & 3);
    i.imm = (long long)((w >> 5) & 0xFFFF);
    i.op = (opc == 0) ? ARM64_DIS_MOVN
                      : (opc == 2 ? ARM64_DIS_MOVZ : ARM64_DIS_MOVK);
    return i;
  }

  /* add/subtract (immediate): bits 28-23 == 100010 */
  if (((w >> 23) & 0x3F) == 0x22) {
    int op = (int)((w >> 30) & 1);
    int s = (int)((w >> 29) & 1);
    int sh = (int)((w >> 22) & 1);
    i.imm = (long long)((w >> 10) & 0xFFF) << (sh ? 12 : 0);
    if (s) {
      i.op = ARM64_DIS_SUBS_IMM; /* CMP imm == SUBS XZR */
    } else {
      i.op = op ? ARM64_DIS_SUB_IMM : ARM64_DIS_ADD_IMM;
    }
    return i;
  }

  /* bitfield move: bits 28-23 == 100110 */
  if (((w >> 23) & 0x3F) == 0x26) {
    int opc = (int)((w >> 29) & 3);
    i.op = (opc == 0) ? ARM64_DIS_SBFM : ARM64_DIS_UBFM;
    return i;
  }

  /* logical (shifted register): bits 28-24 == 01010 */
  if (((w >> 24) & 0x1F) == 0x0A) {
    int opc = (int)((w >> 29) & 3);
    int n = (int)((w >> 21) & 1);
    if (opc == 1 && n == 0) {
      i.op = (i.rn == 31) ? ARM64_DIS_MOV : ARM64_DIS_ORR_REG;
    } else if (opc == 0) {
      i.op = ARM64_DIS_AND_REG;
    } else if (opc == 2) {
      i.op = ARM64_DIS_EOR_REG;
    } else {
      i.op = ARM64_DIS_ORR_REG; /* ORN (MVN) also lands here */
    }
    return i;
  }

  /* arithmetic (shifted register): bits 28-24 == 01011 */
  if (((w >> 24) & 0x1F) == 0x0B) {
    int op = (int)((w >> 30) & 1);
    int s = (int)((w >> 29) & 1);
    if (s) {
      i.op = ARM64_DIS_SUBS_REG; /* CMP reg == SUBS XZR */
    } else {
      i.op = op ? ARM64_DIS_SUB_REG : ARM64_DIS_ADD_REG;
    }
    return i;
  }

  /* data-processing (3 source): bits 28-24 == 11011, bits 30-29 == 00 */
  if (((w >> 24) & 0x1F) == 0x1B && ((w >> 29) & 3) == 0) {
    int o0 = (int)((w >> 15) & 1);
    i.op = o0 ? ARM64_DIS_MSUB : ARM64_DIS_MADD;
    return i;
  }

  /* data-processing (2 source): bits 28-21 == 11010110 */
  if (((w >> 21) & 0xFF) == 0xD6) {
    int opcode = (int)((w >> 10) & 0x3F);
    switch (opcode) {
      case 0x02: i.op = ARM64_DIS_UDIV; break;
      case 0x03: i.op = ARM64_DIS_SDIV; break;
      case 0x08: i.op = ARM64_DIS_LSLV; break;
      case 0x09: i.op = ARM64_DIS_LSRV; break;
      case 0x0A: i.op = ARM64_DIS_ASRV; break;
      default: i.op = ARM64_DIS_UNKNOWN; break;
    }
    return i;
  }

  /* conditional select: bits 28-21 == 11010100 */
  if (((w >> 21) & 0xFF) == 0xD4) {
    int op2 = (int)((w >> 10) & 3);
    i.cond = (int)((w >> 12) & 0xF);
    i.op = (op2 == 1) ? ARM64_DIS_CSINC : ARM64_DIS_CSEL;
    return i;
  }

  /* load/store register (unsigned immediate): bits 29-27 == 111, 26 == 0,
   * 25-24 == 01 */
  if (((w >> 27) & 0x7) == 0x7 && ((w >> 26) & 1) == 0 &&
      ((w >> 24) & 0x3) == 0x1) {
    int size = (int)((w >> 30) & 3);
    int opc = (int)((w >> 22) & 3);
    int scale = (size == 3) ? 8 : 4;
    i.is64 = (size == 3);
    i.imm = (long long)((w >> 10) & 0xFFF) * scale;
    i.op = (opc == 0) ? ARM64_DIS_STR_IMM : ARM64_DIS_LDR_IMM;
    return i;
  }

  /* load/store pair: bits 29-27 == 101, 26 == 0 */
  if (((w >> 27) & 0x7) == 0x5 && ((w >> 26) & 1) == 0) {
    int l = (int)((w >> 22) & 1);
    int opc = (int)((w >> 30) & 3);
    int scale = (opc == 2) ? 8 : 4;
    i.is64 = (opc == 2);
    i.rt2 = (int)((w >> 10) & 0x1F);
    i.imm = sign_extend((w >> 15) & 0x7F, 7) * scale;
    i.op = l ? ARM64_DIS_LDP : ARM64_DIS_STP;
    return i;
  }

  /* compare and branch (CBZ/CBNZ): bits 30-25 == 011010 */
  if (((w >> 25) & 0x3F) == 0x1A) {
    i.imm = sign_extend((w >> 5) & 0x7FFFF, 19) * 4;
    i.op = ((w >> 24) & 1) ? ARM64_DIS_CBNZ : ARM64_DIS_CBZ;
    return i;
  }

  /* conditional branch (B.cond): bits 31-24 == 01010100 */
  if (((w >> 24) & 0xFF) == 0x54) {
    i.cond = (int)(w & 0xF);
    i.imm = sign_extend((w >> 5) & 0x7FFFF, 19) * 4;
    i.op = ARM64_DIS_BCOND;
    return i;
  }

  /* unconditional branch (immediate): bits 31-26 == 000101 (B) / 100101 (BL) */
  if (((w >> 26) & 0x3F) == 0x05) {
    i.imm = sign_extend(w & 0x03FFFFFFu, 26) * 4;
    i.op = ARM64_DIS_B;
    return i;
  }
  if (((w >> 26) & 0x3F) == 0x25) {
    i.imm = sign_extend(w & 0x03FFFFFFu, 26) * 4;
    i.op = ARM64_DIS_BL;
    return i;
  }

  return i;
}
