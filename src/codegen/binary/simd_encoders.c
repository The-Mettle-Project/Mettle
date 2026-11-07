#include "codegen/binary/internal.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "codegen/binary/simd_internal.h"

int wcs_sse_66(BinaryCodeBuffer *b, unsigned char op,
                      int dst, int src) {
  return binary_code_buffer_append_u8(b, 0x66) &&
         binary_code_buffer_append_u8(b, 0x0F) &&
         binary_code_buffer_append_u8(b, op) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (src & 7)));
}

int wcs_movdqu_xmm_rcx(BinaryCodeBuffer *b, int xmm) {
  return binary_code_buffer_append_u8(b, 0xF3) &&
         binary_code_buffer_append_u8(b, 0x0F) &&
         binary_code_buffer_append_u8(b, 0x6F) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0x00 | ((xmm & 7) << 3) | 0x01));
}

int wcs_movd_xmm_reg(BinaryCodeBuffer *b, int xmm, int gpr) {
  return binary_code_buffer_append_u8(b, 0x66) &&
         binary_emit_rex(b, 0, xmm >> 3, 0, gpr >> 3) &&
         binary_code_buffer_append_u8(b, 0x0F) &&
         binary_code_buffer_append_u8(b, 0x6E) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((xmm & 7) << 3) | (gpr & 7)));
}

int wcs_pshufd(BinaryCodeBuffer *b, int dst, int src,
                      unsigned char imm) {
  return binary_code_buffer_append_u8(b, 0x66) &&
         binary_code_buffer_append_u8(b, 0x0F) &&
         binary_code_buffer_append_u8(b, 0x70) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (src & 7))) &&
         binary_code_buffer_append_u8(b, imm);
}

int wcs_pmovmskb(BinaryCodeBuffer *b, int gpr, int xmm) {
  return binary_code_buffer_append_u8(b, 0x66) &&
         binary_emit_rex(b, 0, gpr >> 3, 0, xmm >> 3) &&
         binary_code_buffer_append_u8(b, 0x0F) &&
         binary_code_buffer_append_u8(b, 0xD7) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((gpr & 7) << 3) | (xmm & 7)));
}

int wcs_popcnt(BinaryCodeBuffer *b, int dst, int src) {
  return wcs_popcnt_sized(b, dst, src, 0);
}

int wcs_popcnt_sized(BinaryCodeBuffer *b, int dst, int src, int wide) {
  return binary_code_buffer_append_u8(b, 0xF3) &&
         binary_emit_rex(b, wide ? 1 : 0, dst >> 3, 0, src >> 3) &&
         binary_code_buffer_append_u8(b, 0x0F) &&
         binary_code_buffer_append_u8(b, 0xB8) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (src & 7)));
}

int wcs_movzx_reg_byte_rcx(BinaryCodeBuffer *b, int gpr) {
  return binary_emit_rex(b, 0, gpr >> 3, 0, 0) &&
         binary_code_buffer_append_u8(b, 0x0F) &&
         binary_code_buffer_append_u8(b, 0xB6) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0x00 | ((gpr & 7) << 3) | 0x01));
}

int wcs_shift_reg_imm(BinaryCodeBuffer *b, int gpr, int is_shr,
                             unsigned char imm) {
  return binary_emit_rex(b, 0, 0, 0, gpr >> 3) &&
         binary_code_buffer_append_u8(b, 0xC1) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((is_shr ? 5 : 4) << 3) |
                                (gpr & 7))) &&
         binary_code_buffer_append_u8(b, imm);
}

int wcs_or_reg_reg(BinaryCodeBuffer *b, int dst, int src) {
  return binary_emit_rex(b, 0, src >> 3, 0, dst >> 3) &&
         binary_code_buffer_append_u8(b, 0x09) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((src & 7) << 3) | (dst & 7)));
}

int wcs_sub_reg_reg64(BinaryCodeBuffer *b, int dst, int src) {
  return binary_emit_rex(b, 1, src >> 3, 0, dst >> 3) &&
         binary_code_buffer_append_u8(b, 0x29) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((src & 7) << 3) | (dst & 7)));
}

int wcs_not_reg(BinaryCodeBuffer *b, int gpr) {
  return binary_emit_rex(b, 0, 0, 0, gpr >> 3) &&
         binary_code_buffer_append_u8(b, 0xF7) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | (2 << 3) | (gpr & 7)));
}

int wcs_and_reg_reg(BinaryCodeBuffer *b, int dst, int src) {
  return binary_emit_rex(b, 0, dst >> 3, 0, src >> 3) &&
         binary_code_buffer_append_u8(b, 0x23) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (src & 7)));
}

int wcs_mov_reg_reg32(BinaryCodeBuffer *b, int dst, int src) {
  return binary_emit_rex(b, 0, src >> 3, 0, dst >> 3) &&
         binary_code_buffer_append_u8(b, 0x89) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((src & 7) << 3) | (dst & 7)));
}

int wcs_mov_reg_imm32(BinaryCodeBuffer *b, int gpr, uint32_t imm) {
  return binary_emit_rex(b, 0, 0, 0, gpr >> 3) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xB8 + (gpr & 7))) &&
         binary_code_buffer_append_u32(b, imm);
}

int wcs_add_reg_reg64(BinaryCodeBuffer *b, int dst, int src) {
  return binary_emit_rex(b, 1, src >> 3, 0, dst >> 3) &&
         binary_code_buffer_append_u8(b, 0x01) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((src & 7) << 3) | (dst & 7)));
}

int wcs_addsub_reg_imm8(BinaryCodeBuffer *b, int gpr, int is_sub,
                               unsigned char imm) {
  return binary_emit_rex(b, 1, 0, 0, gpr >> 3) &&
         binary_code_buffer_append_u8(b, 0x83) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((is_sub ? 5 : 0) << 3) |
                                (gpr & 7))) &&
         binary_code_buffer_append_u8(b, imm);
}

int wcs_cmp_reg_imm8(BinaryCodeBuffer *b, int gpr, unsigned char imm) {
  return binary_emit_rex(b, 1, 0, 0, gpr >> 3) &&
         binary_code_buffer_append_u8(b, 0x83) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | (7 << 3) | (gpr & 7))) &&
         binary_code_buffer_append_u8(b, imm);
}

int wcs_cmp_reg_imm32(BinaryCodeBuffer *b, int gpr, uint32_t imm) {
  return binary_emit_rex(b, 0, 0, 0, gpr >> 3) &&
         binary_code_buffer_append_u8(b, 0x81) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | (7 << 3) | (gpr & 7))) &&
         binary_code_buffer_append_u32(b, imm);
}

int wcs_test_reg_reg32(BinaryCodeBuffer *b, int gpr) {
  return binary_emit_rex(b, 0, gpr >> 3, 0, gpr >> 3) &&
         binary_code_buffer_append_u8(b, 0x85) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((gpr & 7) << 3) | (gpr & 7)));
}

int wcs_xor_self32(BinaryCodeBuffer *b, int gpr) {
  return binary_emit_rex(b, 0, gpr >> 3, 0, gpr >> 3) &&
         binary_code_buffer_append_u8(b, 0x31) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((gpr & 7) << 3) | (gpr & 7)));
}

int wcs_jcc(BinaryCodeBuffer *b, unsigned char cc, size_t *disp_off) {
  if (cc == 0) {
    if (!binary_code_buffer_append_u8(b, 0xE9)) return 0;
  } else {
    if (!binary_code_buffer_append_u8(b, 0x0F) ||
        !binary_code_buffer_append_u8(b, cc))
      return 0;
  }
  *disp_off = b->size;
  return binary_code_buffer_append_u32(b, 0);
}

int wcs_patch_here(BinaryCodeBuffer *b, size_t disp_off) {
  long long delta =
      (long long)b->size - (long long)(disp_off + 4);
  if (delta < INT32_MIN || delta > INT32_MAX) return 0;
  int32_t d = (int32_t)delta;
  memcpy(b->data + disp_off, &d, 4);
  return 1;
}

int wcs_patch_both_here(BinaryCodeBuffer *b, size_t first, size_t second) {
  return wcs_patch_here(b, first) && wcs_patch_here(b, second);
}

int wcs_patch_to(BinaryCodeBuffer *b, size_t disp_off,
                        size_t target) {
  long long delta = (long long)target - (long long)(disp_off + 4);
  if (delta < INT32_MIN || delta > INT32_MAX) return 0;
  int32_t d = (int32_t)delta;
  memcpy(b->data + disp_off, &d, 4);
  return 1;
}

int wcs_cmp_reg_reg32(BinaryCodeBuffer *b, int dst, int src) {
  return binary_emit_rex(b, 0, src >> 3, 0, dst >> 3) &&
         binary_code_buffer_append_u8(b, 0x39) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((src & 7) << 3) | (dst & 7)));
}

int simd_emit_prefixed_xmm_mem_disp(BinaryCodeBuffer *b, unsigned char prefix,
                                    unsigned char opcode, int xmm, int gpr,
                                    int displacement) {
  if (!b) {
    return 0;
  }

  int use_disp8 = displacement >= -128 && displacement <= 127;
  unsigned char rm = (unsigned char)(gpr & 7);
  int needs_sib = rm == (BINARY_GP_RSP & 7);
  int needs_base_disp = rm == (BINARY_GP_RBP & 7);
  unsigned char mod =
      (displacement == 0 && !needs_base_disp) ? 0 : (use_disp8 ? 1 : 2);
  unsigned char modrm =
      (unsigned char)((mod << 6) | ((xmm & 7) << 3) |
                      (needs_sib ? 4 : rm));

  if (!binary_code_buffer_append_u8(b, prefix) ||
      !binary_emit_rex(b, 0, xmm >> 3, 0, gpr >> 3) ||
      !binary_code_buffer_append_u8(b, 0x0F) ||
      !binary_code_buffer_append_u8(b, opcode) ||
      !binary_code_buffer_append_u8(b, modrm)) {
    return 0;
  }
  if (needs_sib) {
    unsigned char sib = (unsigned char)((0 << 6) | (4 << 3) | (gpr & 7));
    if (!binary_code_buffer_append_u8(b, sib)) {
      return 0;
    }
  }
  if (mod == 1) {
    return binary_code_buffer_append_u8(b, (unsigned char)(int8_t)displacement);
  }
  if (mod == 2) {
    return binary_code_buffer_append_u32(b, (uint32_t)(int32_t)displacement);
  }
  return 1;
}

int simd_emit_xmm_mem_disp(BinaryCodeBuffer *b, unsigned char opcode,
                                  int xmm, int gpr, int displacement) {
  return simd_emit_prefixed_xmm_mem_disp(b, 0xF3, opcode, xmm, gpr,
                                         displacement);
}

int simd_movdqu_xmm_mem_disp(BinaryCodeBuffer *b, int xmm, int gpr,
                                    int displacement) {
  return simd_emit_xmm_mem_disp(b, 0x6F, xmm, gpr, displacement);
}

int simd_movdqu_mem_xmm_disp(BinaryCodeBuffer *b, int gpr,
                                    int displacement, int xmm) {
  return simd_emit_xmm_mem_disp(b, 0x7F, xmm, gpr, displacement);
}

int wcs_movd_reg_xmm(BinaryCodeBuffer *b, int gpr, int xmm) {
  return binary_code_buffer_append_u8(b, 0x66) &&
         binary_emit_rex(b, 0, xmm >> 3, 0, gpr >> 3) &&
         binary_code_buffer_append_u8(b, 0x0F) &&
         binary_code_buffer_append_u8(b, 0x7E) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((xmm & 7) << 3) | (gpr & 7)));
}

int wcs_sub_reg_reg32(BinaryCodeBuffer *b, int dst, int src) {
  return binary_emit_rex(b, 0, src >> 3, 0, dst >> 3) &&
         binary_code_buffer_append_u8(b, 0x29) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((src & 7) << 3) | (dst & 7)));
}

int wcs_paddd(BinaryCodeBuffer *b, int dst, int src) {
  return wcs_sse_66(b, 0xFE, dst, src);
}

int wcs_sse_66_38(BinaryCodeBuffer *b, unsigned char op, int dst,
                         int src) {
  return binary_code_buffer_append_u8(b, 0x66) &&
         binary_code_buffer_append_u8(b, 0x0F) &&
         binary_code_buffer_append_u8(b, 0x38) &&
         binary_code_buffer_append_u8(b, op) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (src & 7)));
}

int wcs_pminsd(BinaryCodeBuffer *b, int dst, int src) {
  return wcs_sse_66_38(b, 0x39, dst, src);
}

int wcs_pmaxsd(BinaryCodeBuffer *b, int dst, int src) {
  return wcs_sse_66_38(b, 0x3D, dst, src);
}

int wcs_accumulate_xmm0_i32_to_rax(BinaryCodeBuffer *b) {
  if (!wcs_sse_66(b, 0x6F, 1, 0) || !wcs_pshufd(b, 1, 0, 0xEE) ||
      !wcs_paddd(b, 0, 1) || !wcs_pshufd(b, 1, 0, 0x01) ||
      !wcs_paddd(b, 0, 1) || !wcs_movd_reg_xmm(b, BINARY_GP_R10, 0) ||
      !binary_emit_movsxd_reg_reg32(b, BINARY_GP_R10, BINARY_GP_R10) ||
      !wcs_add_reg_reg64(b, BINARY_GP_RAX, BINARY_GP_R10)) {
    return 0;
  }
  return 1;
}

int wcs_paddq(BinaryCodeBuffer *b, int dst, int src) {
  return wcs_sse_66(b, 0xD4, dst, src);
}

int wcs_vex3(BinaryCodeBuffer *b, int map, int pp, int len256, int w,
                    int reg, int rm, int vvvv) {
  unsigned char b2 = (unsigned char)((((~(reg >> 3)) & 1) << 7) |
                                     (1 << 6) |
                                     (((~(rm >> 3)) & 1) << 5) |
                                     (map & 0x1F));
  unsigned char b3 = (unsigned char)(((w & 1) << 7) |
                                     (((~vvvv) & 0x0F) << 3) |
                                     ((len256 & 1) << 2) | (pp & 3));
  return binary_code_buffer_append_u8(b, 0xC4) &&
         binary_code_buffer_append_u8(b, b2) &&
         binary_code_buffer_append_u8(b, b3);
}

int wcs_avx_modrm_mem_disp(BinaryCodeBuffer *b, int reg, int base,
                                  int displacement) {
  int use_disp8 = displacement >= -128 && displacement <= 127;
  unsigned char base_low = (unsigned char)(base & 7);
  unsigned char mod = 0;
  unsigned char rm = base_low;
  if (displacement != 0 || base_low == 5) {
    mod = use_disp8 ? 1 : 2;
  }
  if (base_low == 4) {
    rm = 4;
  }
  if (!binary_code_buffer_append_u8(
          b, (unsigned char)((mod << 6) | ((reg & 7) << 3) | rm))) {
    return 0;
  }
  if (base_low == 4 &&
      !binary_code_buffer_append_u8(
          b, (unsigned char)((0 << 6) | (4 << 3) | base_low))) {
    return 0;
  }
  if (mod == 1) {
    return binary_code_buffer_append_u8(b,
                                        (unsigned char)(int8_t)displacement);
  }
  if (mod == 2 || (mod == 0 && base_low == 5)) {
    return binary_code_buffer_append_u32(b, (uint32_t)(int32_t)displacement);
  }
  return 1;
}

int wcs_avx_vpmovsxdq_ymm_mem(BinaryCodeBuffer *b, int dst, int base,
                                     int disp) {
  return wcs_vex3(b, 2, 1, 1, 0, dst, base, 0) &&
         binary_code_buffer_append_u8(b, 0x25) &&
         wcs_avx_modrm_mem_disp(b, dst, base, disp);
}

int wcs_avx_vmovups_mem_ymm(BinaryCodeBuffer *b, int base, int disp,
                                   int src) {
  return wcs_vex3(b, 1, 0, 1, 0, src, base, 0) &&
         binary_code_buffer_append_u8(b, 0x11) &&
         wcs_avx_modrm_mem_disp(b, src, base, disp);
}

int wcs_avx_vbroadcastsd_ymm_xmm(BinaryCodeBuffer *b, int dst,
                                        int src_xmm) {
  return wcs_vex3(b, 2, 1, 1, 0, dst, src_xmm, 0) &&
         binary_code_buffer_append_u8(b, 0x19) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (src_xmm & 7)));
}

int wcs_avx_vmovdqu_ymm_mem(BinaryCodeBuffer *b, int dst, int base,
                                   int displacement) {
  return wcs_vex3(b, 1, 2, 1, 0, dst, base, 0) &&
         binary_code_buffer_append_u8(b, 0x6F) &&
         wcs_avx_modrm_mem_disp(b, dst, base, displacement);
}

int wcs_avx_vpaddq_ymm(BinaryCodeBuffer *b, int dst, int src1,
                              int src2) {
  return wcs_vex3(b, 1, 1, 1, 0, dst, src2, src1) &&
         binary_code_buffer_append_u8(b, 0xD4) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (src2 & 7)));
}

int wcs_avx_vpxor_ymm(BinaryCodeBuffer *b, int dst, int src1,
                             int src2) {
  return wcs_vex3(b, 1, 1, 1, 0, dst, src2, src1) &&
         binary_code_buffer_append_u8(b, 0xEF) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (src2 & 7)));
}

int wcs_avx_vpsadbw_ymm(BinaryCodeBuffer *b, int dst, int src1, int src2) {
  return wcs_vex3(b, 1, 1, 1, 0, dst, src2, src1) &&
         binary_code_buffer_append_u8(b, 0xF6) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (src2 & 7)));
}

static int wcs_avx128_66_0f_rr(BinaryCodeBuffer *b, unsigned char opcode,
                               int dst, int src1, int src2) {
  return wcs_vex3(b, 1, 1, 0, 0, dst, src2, src1) &&
         binary_code_buffer_append_u8(b, opcode) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (src2 & 7)));
}

int wcs_avx_vpaddb_xmm(BinaryCodeBuffer *b, int d, int s1, int s2) {
  return wcs_avx128_66_0f_rr(b, 0xFC, d, s1, s2);
}
int wcs_avx_vpsubb_xmm(BinaryCodeBuffer *b, int d, int s1, int s2) {
  return wcs_avx128_66_0f_rr(b, 0xF8, d, s1, s2);
}
int wcs_avx_vpand_xmm(BinaryCodeBuffer *b, int d, int s1, int s2) {
  return wcs_avx128_66_0f_rr(b, 0xDB, d, s1, s2);
}
int wcs_avx_vpor_xmm(BinaryCodeBuffer *b, int d, int s1, int s2) {
  return wcs_avx128_66_0f_rr(b, 0xEB, d, s1, s2);
}
int wcs_avx_vpxor_xmm(BinaryCodeBuffer *b, int d, int s1, int s2) {
  return wcs_avx128_66_0f_rr(b, 0xEF, d, s1, s2);
}
int wcs_avx_vpmullw_xmm(BinaryCodeBuffer *b, int d, int s1, int s2) {
  return wcs_avx128_66_0f_rr(b, 0xD5, d, s1, s2);
}
int wcs_avx_vpunpcklbw_xmm(BinaryCodeBuffer *b, int d, int s1, int s2) {
  return wcs_avx128_66_0f_rr(b, 0x60, d, s1, s2);
}
int wcs_avx_vpunpckhbw_xmm(BinaryCodeBuffer *b, int d, int s1, int s2) {
  return wcs_avx128_66_0f_rr(b, 0x68, d, s1, s2);
}
int wcs_avx_vpackuswb_xmm(BinaryCodeBuffer *b, int d, int s1, int s2) {
  return wcs_avx128_66_0f_rr(b, 0x67, d, s1, s2);
}
int wcs_avx_vpaddd_xmm(BinaryCodeBuffer *b, int d, int s1, int s2) {
  return wcs_avx128_66_0f_rr(b, 0xFE, d, s1, s2);
}
int wcs_avx_vpaddq_xmm(BinaryCodeBuffer *b, int d, int s1, int s2) {
  return wcs_avx128_66_0f_rr(b, 0xD4, d, s1, s2);
}
int wcs_avx_vpshufd_xmm(BinaryCodeBuffer *b, int dst, int src,
                        unsigned char imm) {
  return wcs_vex3(b, 1, 1, 0, 0, dst, src, 0) &&
         binary_code_buffer_append_u8(b, 0x70) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (src & 7))) &&
         binary_code_buffer_append_u8(b, imm);
}
int wcs_avx_vpslldq_xmm(BinaryCodeBuffer *b, int dst, int src,
                        unsigned char imm) {
  return wcs_vex3(b, 1, 1, 0, 0, 7, src, dst) &&
         binary_code_buffer_append_u8(b, 0x73) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | (7 << 3) | (src & 7))) &&
         binary_code_buffer_append_u8(b, imm);
}

int wcs_avx_vmovdqu_xmm_mem(BinaryCodeBuffer *b, int dst, int base, int disp) {
  return wcs_vex3(b, 1, 2, 0, 0, dst, base, 0) &&
         binary_code_buffer_append_u8(b, 0x6F) &&
         wcs_avx_modrm_mem_disp(b, dst, base, disp);
}
int wcs_avx_vmovdqu_mem_xmm(BinaryCodeBuffer *b, int base, int disp, int src) {
  return wcs_vex3(b, 1, 2, 0, 0, src, base, 0) &&
         binary_code_buffer_append_u8(b, 0x7F) &&
         wcs_avx_modrm_mem_disp(b, src, base, disp);
}

int wcs_avx_vpmuldq_ymm(BinaryCodeBuffer *b, int dst, int src1,
                               int src2) {
  return wcs_vex3(b, 2, 1, 1, 0, dst, src2, src1) &&
         binary_code_buffer_append_u8(b, 0x28) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (src2 & 7)));
}

int wcs_avx_vpsrlq_ymm_imm(BinaryCodeBuffer *b, int dst, int src,
                                  unsigned char imm) {
  return wcs_vex3(b, 1, 1, 1, 0, 2, dst, src) &&
         binary_code_buffer_append_u8(b, 0x73) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | (2 << 3) | (dst & 7))) &&
         binary_code_buffer_append_u8(b, imm);
}

int wcs_avx_vextracti128(BinaryCodeBuffer *b, int dst_xmm, int src_ymm,
                                unsigned char lane) {
  return wcs_vex3(b, 3, 1, 1, 0, src_ymm, dst_xmm, 0) &&
         binary_code_buffer_append_u8(b, 0x39) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((src_ymm & 7) << 3) |
                                (dst_xmm & 7))) &&
         binary_code_buffer_append_u8(b, lane);
}

int wcs_avx_vzeroupper(BinaryCodeBuffer *b) {
  return binary_code_buffer_append_u8(b, 0xC5) &&
         binary_code_buffer_append_u8(b, 0xF8) &&
         binary_code_buffer_append_u8(b, 0x77);
}

int wcs_sse_0f(BinaryCodeBuffer *b, unsigned char op, int dst, int src) {
  return binary_code_buffer_append_u8(b, 0x0F) &&
         binary_code_buffer_append_u8(b, op) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (src & 7)));
}

int wcs_sse_f2(BinaryCodeBuffer *b, unsigned char op, int dst, int src) {
  return binary_code_buffer_append_u8(b, 0xF2) &&
         binary_code_buffer_append_u8(b, 0x0F) &&
         binary_code_buffer_append_u8(b, op) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (src & 7)));
}

int wcs_avx_vpd_ymm(BinaryCodeBuffer *b, unsigned char op, int dst,
                           int src1, int src2) {
  return wcs_vex3(b, 1, 1, 1, 0, dst, src2, src1) &&
         binary_code_buffer_append_u8(b, op) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (src2 & 7)));
}

int wcs_avx_vps_ymm(BinaryCodeBuffer *b, unsigned char op, int dst,
                           int src1, int src2) {
  return wcs_vex3(b, 1, 0, 1, 0, dst, src2, src1) &&
         binary_code_buffer_append_u8(b, op) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (src2 & 7)));
}

int wcs_avx_vaddpd_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2) {
  return wcs_avx_vpd_ymm(b, 0x58, dst, s1, s2);
}
int wcs_avx_vmulpd_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2) {
  return wcs_avx_vpd_ymm(b, 0x59, dst, s1, s2);
}
int wcs_avx_vsubpd_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2) {
  return wcs_avx_vpd_ymm(b, 0x5C, dst, s1, s2);
}
int wcs_avx_vdivpd_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2) {
  return wcs_avx_vpd_ymm(b, 0x5E, dst, s1, s2);
}
int wcs_avx_vminpd_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2) {
  return wcs_avx_vpd_ymm(b, 0x5D, dst, s1, s2);
}
int wcs_avx_vmaxpd_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2) {
  return wcs_avx_vpd_ymm(b, 0x5F, dst, s1, s2);
}

int wcs_avx_vcvtdq2pd_ymm_xmm(BinaryCodeBuffer *b, int dst, int src) {
  return wcs_vex3(b, 1, 2, 1, 0, dst, src, 0) &&
         binary_code_buffer_append_u8(b, 0xE6) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (src & 7)));
}

int wcs_avx_vcvttpd2dq_xmm_ymm(BinaryCodeBuffer *b, int dst, int src) {
  return wcs_vex3(b, 1, 1, 1, 0, dst, src, 0) &&
         binary_code_buffer_append_u8(b, 0xE6) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (src & 7)));
}

int wcs_avx_vpmovsxdq_ymm_xmm(BinaryCodeBuffer *b, int dst, int src) {
  return wcs_vex3(b, 2, 1, 1, 0, dst, src, 0) &&
         binary_code_buffer_append_u8(b, 0x25) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (src & 7)));
}

int wcs_avx_vpunpcklqdq_xmm(BinaryCodeBuffer *b, int dst, int src1, int src2) {
  return wcs_vex3(b, 1, 1, 0, 0, dst, src2, src1) &&
         binary_code_buffer_append_u8(b, 0x6C) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (src2 & 7)));
}

static int wcs_avx_vsd(BinaryCodeBuffer *b, unsigned char op, int dst, int s1,
                       int s2) {
  return wcs_vex3(b, 1, 3, 0, 0, dst, s2, s1) &&
         binary_code_buffer_append_u8(b, op) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (s2 & 7)));
}
int wcs_avx_vaddsd(BinaryCodeBuffer *b, int dst, int s1, int s2) {
  return wcs_avx_vsd(b, 0x58, dst, s1, s2);
}
int wcs_avx_vsubsd(BinaryCodeBuffer *b, int dst, int s1, int s2) {
  return wcs_avx_vsd(b, 0x5C, dst, s1, s2);
}
int wcs_avx_vmulsd(BinaryCodeBuffer *b, int dst, int s1, int s2) {
  return wcs_avx_vsd(b, 0x59, dst, s1, s2);
}
int wcs_avx_vdivsd(BinaryCodeBuffer *b, int dst, int s1, int s2) {
  return wcs_avx_vsd(b, 0x5E, dst, s1, s2);
}
int wcs_avx_vcvtsi2sd(BinaryCodeBuffer *b, int dst, int s1, int gpr) {
  return wcs_vex3(b, 1, 3, 0, 1, dst, gpr, s1) &&
         binary_code_buffer_append_u8(b, 0x2A) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (gpr & 7)));
}
int wcs_avx_vmovsd_xmm_mem(BinaryCodeBuffer *b, int dst, int base, int disp) {
  return wcs_vex3(b, 1, 3, 0, 0, dst, base, 0) &&
         binary_code_buffer_append_u8(b, 0x10) &&
         wcs_avx_modrm_mem_disp(b, dst, base, disp);
}
int wcs_avx_vmovsd_mem_xmm(BinaryCodeBuffer *b, int base, int disp, int src) {
  return wcs_vex3(b, 1, 3, 0, 0, src, base, 0) &&
         binary_code_buffer_append_u8(b, 0x11) &&
         wcs_avx_modrm_mem_disp(b, src, base, disp);
}
int wcs_vex_movss_xmm_mem(BinaryCodeBuffer *b, int xmm, int gpr, int disp) {
  return wcs_vex3(b, 1, 2, 0, 0, xmm, gpr, 0) &&
         binary_code_buffer_append_u8(b, 0x10) &&
         wcs_avx_modrm_mem_disp(b, xmm, gpr, disp);
}
int wcs_vex_movss_mem_xmm(BinaryCodeBuffer *b, int gpr, int disp, int xmm) {
  return wcs_vex3(b, 1, 2, 0, 0, xmm, gpr, 0) &&
         binary_code_buffer_append_u8(b, 0x11) &&
         wcs_avx_modrm_mem_disp(b, xmm, gpr, disp);
}
int wcs_vex_movsd_xmm_mem(BinaryCodeBuffer *b, int xmm, int gpr, int disp) {
  return wcs_avx_vmovsd_xmm_mem(b, xmm, gpr, disp);
}
int wcs_vex_movsd_mem_xmm(BinaryCodeBuffer *b, int gpr, int disp, int xmm) {
  return wcs_avx_vmovsd_mem_xmm(b, gpr, disp, xmm);
}
static int wcs_vex_scalar_rr(BinaryCodeBuffer *b, int pp, unsigned char op,
                             int dst, int src) {
  return wcs_vex3(b, 1, pp, 0, 0, dst, src, dst) &&
         binary_code_buffer_append_u8(b, op) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (src & 7)));
}
int wcs_vex_addss_xmm_xmm(BinaryCodeBuffer *b, int dst, int src) {
  return wcs_vex_scalar_rr(b, 2, 0x58, dst, src);
}
int wcs_vex_mulss_xmm_xmm(BinaryCodeBuffer *b, int dst, int src) {
  return wcs_vex_scalar_rr(b, 2, 0x59, dst, src);
}
int wcs_vex_subss_xmm_xmm(BinaryCodeBuffer *b, int dst, int src) {
  return wcs_vex_scalar_rr(b, 2, 0x5C, dst, src);
}
int wcs_vex_divss_xmm_xmm(BinaryCodeBuffer *b, int dst, int src) {
  return wcs_vex_scalar_rr(b, 2, 0x5E, dst, src);
}
int wcs_vex_addsd_xmm_xmm(BinaryCodeBuffer *b, int dst, int src) {
  return wcs_vex_scalar_rr(b, 3, 0x58, dst, src);
}
int wcs_vex_mulsd_xmm_xmm(BinaryCodeBuffer *b, int dst, int src) {
  return wcs_vex_scalar_rr(b, 3, 0x59, dst, src);
}
int wcs_vex_subsd_xmm_xmm(BinaryCodeBuffer *b, int dst, int src) {
  return wcs_vex_scalar_rr(b, 3, 0x5C, dst, src);
}
int wcs_vex_divsd_xmm_xmm(BinaryCodeBuffer *b, int dst, int src) {
  return wcs_vex_scalar_rr(b, 3, 0x5E, dst, src);
}
int wcs_vex_movsd_xmm_xmm(BinaryCodeBuffer *b, int dst, int src) {
  return wcs_vex_scalar_rr(b, 3, 0x10, dst, src);
}
int wcs_vex_cvtsi2sd_xmm_reg(BinaryCodeBuffer *b, int dst, int gpr) {
  return wcs_vex3(b, 1, 3, 0, 1, dst, gpr, dst) &&
         binary_code_buffer_append_u8(b, 0x2A) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (gpr & 7)));
}
int wcs_vex_cvtsi2ss_xmm_reg(BinaryCodeBuffer *b, int dst, int gpr) {
  return wcs_vex3(b, 1, 2, 0, 1, dst, gpr, dst) &&
         binary_code_buffer_append_u8(b, 0x2A) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (gpr & 7)));
}
static int wcs_vex_gpr_move(BinaryCodeBuffer *b, unsigned char op, int w,
                            int xmm, int gpr) {
  return wcs_vex3(b, 1, 1, 0, w, xmm, gpr, 0) &&
         binary_code_buffer_append_u8(b, op) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((xmm & 7) << 3) | (gpr & 7)));
}
int wcs_vex_movd_xmm_reg(BinaryCodeBuffer *b, int xmm, int gpr) {
  return wcs_vex_gpr_move(b, 0x6E, 0, xmm, gpr);
}
int wcs_vex_movq_xmm_reg(BinaryCodeBuffer *b, int xmm, int gpr) {
  return wcs_vex_gpr_move(b, 0x6E, 1, xmm, gpr);
}
int wcs_vex_movd_reg_xmm(BinaryCodeBuffer *b, int gpr, int xmm) {
  return wcs_vex_gpr_move(b, 0x7E, 0, xmm, gpr);
}
int wcs_vex_movq_reg_xmm(BinaryCodeBuffer *b, int gpr, int xmm) {
  return wcs_vex_gpr_move(b, 0x7E, 1, xmm, gpr);
}
static int wcs_vex_sib4(BinaryCodeBuffer *b, int reg, int base, int index,
                        int disp) {
  unsigned char base_low = (unsigned char)(base & 7);
  unsigned char mod = 0;
  int use_disp8 = disp >= -128 && disp <= 127;
  if (disp != 0 || base_low == 5) {
    mod = use_disp8 ? 1 : 2;
  }
  if (!binary_code_buffer_append_u8(
          b, (unsigned char)((mod << 6) | ((reg & 7) << 3) | 4)) ||
      !binary_code_buffer_append_u8(
          b, (unsigned char)((2 << 6) | ((index & 7) << 3) | base_low))) {
    return 0;
  }
  if (mod == 1) {
    return binary_code_buffer_append_u8(b, (unsigned char)(signed char)disp);
  }
  if (mod == 2) {
    return binary_code_buffer_append_u32(b, (uint32_t)disp);
  }
  return 1;
}
int wcs_vex_movups_ymm_sib(BinaryCodeBuffer *b, int dst, int base, int index,
                           int disp) {
  return (index & 8) == 0 && wcs_vex3(b, 1, 0, 1, 0, dst, base, 0) &&
         binary_code_buffer_append_u8(b, 0x10) &&
         wcs_vex_sib4(b, dst, base, index, disp);
}
int wcs_vex_movss_xmm_sib(BinaryCodeBuffer *b, int dst, int base, int index,
                          int disp) {
  return (index & 8) == 0 && wcs_vex3(b, 1, 2, 0, 0, dst, base, 0) &&
         binary_code_buffer_append_u8(b, 0x10) &&
         wcs_vex_sib4(b, dst, base, index, disp);
}
int wcs_vex_movups_xmm_mem(BinaryCodeBuffer *b, int dst, int base, int disp) {
  return wcs_vex3(b, 1, 0, 0, 0, dst, base, 0) &&
         binary_code_buffer_append_u8(b, 0x10) &&
         wcs_avx_modrm_mem_disp(b, dst, base, disp);
}
int wcs_vex_movups_mem_xmm(BinaryCodeBuffer *b, int base, int disp, int src) {
  return wcs_vex3(b, 1, 0, 0, 0, src, base, 0) &&
         binary_code_buffer_append_u8(b, 0x11) &&
         wcs_avx_modrm_mem_disp(b, src, base, disp);
}
static int wcs_vex_rrr128(BinaryCodeBuffer *b, int pp, unsigned char op,
                          int dst, int s1, int s2) {
  return wcs_vex3(b, 1, pp, 0, 0, dst, s2, s1) &&
         binary_code_buffer_append_u8(b, op) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (s2 & 7)));
}
int wcs_vex_addps_xmm(BinaryCodeBuffer *b, int dst, int s1, int s2) {
  return wcs_vex_rrr128(b, 0, 0x58, dst, s1, s2);
}
int wcs_vex_haddps_xmm(BinaryCodeBuffer *b, int dst, int s1, int s2) {
  return wcs_vex_rrr128(b, 3, 0x7C, dst, s1, s2);
}
int wcs_vex_addss_rrr(BinaryCodeBuffer *b, int dst, int s1, int s2) {
  return wcs_vex_rrr128(b, 2, 0x58, dst, s1, s2);
}
int wcs_vex_cvttsd2si_reg_xmm(BinaryCodeBuffer *b, int gpr, int xmm) {
  return wcs_vex3(b, 1, 3, 0, 1, gpr, xmm, 0) &&
         binary_code_buffer_append_u8(b, 0x2C) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((gpr & 7) << 3) | (xmm & 7)));
}
int wcs_avx_vunpckhpd_xmm(BinaryCodeBuffer *b, int dst, int s1, int s2) {
  return wcs_vex3(b, 1, 1, 0, 0, dst, s2, s1) &&
         binary_code_buffer_append_u8(b, 0x15) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (s2 & 7)));
}

int wcs_avx_vaddps_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2) {
  return wcs_avx_vps_ymm(b, 0x58, dst, s1, s2);
}
int wcs_avx_vmulps_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2) {
  return wcs_avx_vps_ymm(b, 0x59, dst, s1, s2);
}
int wcs_avx_vsubps_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2) {
  return wcs_avx_vps_ymm(b, 0x5C, dst, s1, s2);
}
int wcs_avx_vdivps_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2) {
  return wcs_avx_vps_ymm(b, 0x5E, dst, s1, s2);
}
int wcs_avx_vminps_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2) {
  return wcs_avx_vps_ymm(b, 0x5D, dst, s1, s2);
}
int wcs_avx_vmaxps_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2) {
  return wcs_avx_vps_ymm(b, 0x5F, dst, s1, s2);
}

int wcs_avx_vroundps_ymm(BinaryCodeBuffer *b, int dst, int src,
                         unsigned char imm) {
  return wcs_vex3(b, 3, 1, 1, 0, dst, src, 0) &&
         binary_code_buffer_append_u8(b, 0x08) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (src & 7))) &&
         binary_code_buffer_append_u8(b, imm);
}

int wcs_avx_vcvttps2dq_ymm(BinaryCodeBuffer *b, int dst, int src) {
  return wcs_vex3(b, 1, 2, 1, 0, dst, src, 0) &&
         binary_code_buffer_append_u8(b, 0x5B) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (src & 7)));
}

int wcs_avx_vcvtdq2ps_ymm(BinaryCodeBuffer *b, int dst, int src) {
  return wcs_vex3(b, 1, 0, 1, 0, dst, src, 0) &&
         binary_code_buffer_append_u8(b, 0x5B) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (src & 7)));
}

int wcs_avx_vcvtph2ps_xmm(BinaryCodeBuffer *b, int dst, int src) {
  return wcs_vex3(b, 2, 1, 0, 0, dst, src, 0) &&
         binary_code_buffer_append_u8(b, 0x13) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (src & 7)));
}

int wcs_avx_vcvtps2ph_xmm(BinaryCodeBuffer *b, int dst, int src,
                           unsigned char imm) {
  return wcs_vex3(b, 3, 1, 0, 0, src, dst, 0) &&
         binary_code_buffer_append_u8(b, 0x1D) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((src & 7) << 3) | (dst & 7))) &&
         binary_code_buffer_append_u8(b, imm);
}

int wcs_avx_vpslld_ymm_imm(BinaryCodeBuffer *b, int dst, int src,
                           unsigned char imm) {
  return wcs_vex3(b, 1, 1, 1, 0, 6, dst, src) &&
         binary_code_buffer_append_u8(b, 0x72) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | (6 << 3) | (dst & 7))) &&
         binary_code_buffer_append_u8(b, imm);
}

int wcs_avx_vpsrld_ymm_imm(BinaryCodeBuffer *b, int dst, int src,
                           unsigned char imm) {
  return wcs_vex3(b, 1, 1, 1, 0, 2, dst, src) &&
         binary_code_buffer_append_u8(b, 0x72) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | (2 << 3) | (dst & 7))) &&
         binary_code_buffer_append_u8(b, imm);
}

int wcs_avx_vpsrad_ymm_imm(BinaryCodeBuffer *b, int dst, int src,
                           unsigned char imm) {
  return wcs_vex3(b, 1, 1, 1, 0, 4, dst, src) &&
         binary_code_buffer_append_u8(b, 0x72) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | (4 << 3) | (dst & 7))) &&
         binary_code_buffer_append_u8(b, imm);
}

int wcs_avx_vmovups_ymm_mem(BinaryCodeBuffer *b, int dst, int base,
                                    int disp) {
  return wcs_vex3(b, 1, 0, 1, 0, dst, base, 0) &&
         binary_code_buffer_append_u8(b, 0x10) &&
         wcs_avx_modrm_mem_disp(b, dst, base, disp);
}

int wcs_avx_vextractf128(BinaryCodeBuffer *b, int dst_xmm, int src_ymm,
                                unsigned char lane) {
  return wcs_vex3(b, 3, 1, 1, 0, src_ymm, dst_xmm, 0) &&
         binary_code_buffer_append_u8(b, 0x19) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((src_ymm & 7) << 3) | (dst_xmm & 7))) &&
         binary_code_buffer_append_u8(b, lane);
}

int wcs_movsd_xmm_mem(BinaryCodeBuffer *b, int xmm, int gpr, int disp) {
  return simd_emit_prefixed_xmm_mem_disp(b, 0xF2, 0x10, xmm, gpr, disp);
}
int wcs_movsd_mem_xmm(BinaryCodeBuffer *b, int gpr, int disp, int xmm) {
  return simd_emit_prefixed_xmm_mem_disp(b, 0xF2, 0x11, xmm, gpr, disp);
}
int wcs_movss_xmm_mem(BinaryCodeBuffer *b, int xmm, int gpr, int disp) {
  return simd_emit_prefixed_xmm_mem_disp(b, 0xF3, 0x10, xmm, gpr, disp);
}
int wcs_movss_mem_xmm(BinaryCodeBuffer *b, int gpr, int disp, int xmm) {
  return simd_emit_prefixed_xmm_mem_disp(b, 0xF3, 0x11, xmm, gpr, disp);
}

int wcs_reduce_pd_acc_to_rax(BinaryCodeBuffer *b) {
  return wcs_avx_vextractf128(b, 0, 2, 1) && wcs_avx_vzeroupper(b) &&
         wcs_sse_66(b, 0x58, 2, 0) &&
         wcs_sse_66(b, 0x7C, 2, 2) &&
         binary_emit_addsd_xmm_xmm(b, BINARY_XMM3, BINARY_XMM2) &&
         binary_emit_movq_reg_xmm(b, BINARY_GP_RAX, BINARY_XMM3);
}

int wcs_reduce_ps_acc_to_rax(BinaryCodeBuffer *b) {
  return wcs_avx_vextractf128(b, 0, 2, 1) && wcs_avx_vzeroupper(b) &&
         wcs_sse_0f(b, 0x58, 2, 0) &&
         wcs_sse_f2(b, 0x7C, 2, 2) &&
         wcs_sse_f2(b, 0x7C, 2, 2) &&
         binary_emit_addss_xmm_xmm(b, BINARY_XMM3, BINARY_XMM2) &&
         wcs_movd_reg_xmm(b, BINARY_GP_RAX, BINARY_XMM3);
}

int wcs_reduce_pd_minmax_to_rax(BinaryCodeBuffer *b, int is_max) {
  unsigned char op = is_max ? 0x5F : 0x5D;
  return wcs_avx_vextractf128(b, 0, 2, 1) && wcs_avx_vzeroupper(b) &&
         wcs_sse_66(b, op, 2, 0) &&
         wcs_pshufd(b, 0, 2, 0xEE) &&
         wcs_sse_66(b, op, 2, 0) &&
         binary_emit_movq_reg_xmm(b, BINARY_GP_RAX, BINARY_XMM2);
}

int wcs_reduce_ps_minmax_to_rax(BinaryCodeBuffer *b, int is_max) {
  unsigned char op = is_max ? 0x5F : 0x5D;
  return wcs_avx_vextractf128(b, 0, 2, 1) && wcs_avx_vzeroupper(b) &&
         wcs_sse_0f(b, op, 2, 0) &&
         wcs_pshufd(b, 0, 2, 0xEE) &&
         wcs_sse_0f(b, op, 2, 0) &&
         wcs_pshufd(b, 0, 2, 0x01) &&
         wcs_sse_0f(b, op, 2, 0) &&
         wcs_movd_reg_xmm(b, BINARY_GP_RAX, BINARY_XMM2);
}

int wcs_reduce_ymm_i32_minmax_to_rax(BinaryCodeBuffer *b, int is_max) {
  int (*fold)(BinaryCodeBuffer *, int, int) = is_max ? wcs_pmaxsd : wcs_pminsd;
  return wcs_avx_vextracti128(b, 0, 2, 1) && wcs_avx_vzeroupper(b) &&
         fold(b, 2, 0) &&
         wcs_pshufd(b, 0, 2, 0xEE) && fold(b, 2, 0) &&
         wcs_pshufd(b, 0, 2, 0x01) && fold(b, 2, 0) &&
         wcs_movd_reg_xmm(b, BINARY_GP_RAX, BINARY_XMM2) &&
         binary_emit_movsxd_reg_reg32(b, BINARY_GP_RAX, BINARY_GP_RAX);
}

int wcs_horizontal_pminsd_to_reg(BinaryCodeBuffer *b, int xmm, int gpr) {
  return wcs_pshufd(b, 1, xmm, 0xEE) &&
         wcs_pminsd(b, xmm, 1) &&
         wcs_pshufd(b, 1, xmm, 0x01) &&
         wcs_pminsd(b, xmm, 1) &&
         wcs_movd_reg_xmm(b, BINARY_GP_R9, xmm) &&
         binary_emit_movsxd_reg_reg32(b, BINARY_GP_R9, BINARY_GP_R9) &&
         binary_emit_cmp_reg_reg(b, BINARY_GP_R9, gpr) &&
         binary_emit_cmovcc_reg_reg(b, 0x4C , gpr, BINARY_GP_R9);
}

int wcs_horizontal_pmaxsd_to_reg(BinaryCodeBuffer *b, int xmm, int gpr) {
  return wcs_pshufd(b, 1, xmm, 0xEE) &&
         wcs_pmaxsd(b, xmm, 1) &&
         wcs_pshufd(b, 1, xmm, 0x01) &&
         wcs_pmaxsd(b, xmm, 1) &&
         wcs_movd_reg_xmm(b, BINARY_GP_R9, xmm) &&
         binary_emit_movsxd_reg_reg32(b, BINARY_GP_R9, BINARY_GP_R9) &&
         binary_emit_cmp_reg_reg(b, BINARY_GP_R9, gpr) &&
         binary_emit_cmovcc_reg_reg(b, 0x4F , gpr, BINARY_GP_R9);
}

int wcs_avx_0f38_ymm(BinaryCodeBuffer *b, unsigned char op, int dst,
                            int src1, int src2) {
  return wcs_vex3(b, 2, 1, 1, 0, dst, src2, src1) &&
         binary_code_buffer_append_u8(b, op) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (src2 & 7)));
}
int wcs_avx_vpminsd_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2) {
  return wcs_avx_0f38_ymm(b, 0x39, dst, s1, s2);
}
int wcs_avx_vpmaxsd_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2) {
  return wcs_avx_0f38_ymm(b, 0x3D, dst, s1, s2);
}
int wcs_avx_vpmulld_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2) {
  return wcs_avx_0f38_ymm(b, 0x40, dst, s1, s2);
}
int wcs_avx_vpshufd_ymm(BinaryCodeBuffer *b, int dst, int src,
                               unsigned char imm) {
  return wcs_vex3(b, 1, 1, 1, 0, dst, src, 0) &&
         binary_code_buffer_append_u8(b, 0x70) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (src & 7))) &&
         binary_code_buffer_append_u8(b, imm);
}
int wcs_avx_vperm2i128(BinaryCodeBuffer *b, int dst, int s1, int s2,
                              unsigned char imm) {
  return wcs_vex3(b, 3, 1, 1, 0, dst, s2, s1) &&
         binary_code_buffer_append_u8(b, 0x46) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (s2 & 7))) &&
         binary_code_buffer_append_u8(b, imm);
}
int wcs_avx_vpaddd_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2) {
  return wcs_vex3(b, 1, 1, 1, 0, dst, s2, s1) &&
         binary_code_buffer_append_u8(b, 0xFE) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (s2 & 7)));
}
int wcs_avx_vmovdqu_mem_ymm(BinaryCodeBuffer *b, int base,
                                   int displacement, int src) {
  return wcs_vex3(b, 1, 2, 1, 0, src, base, 0) &&
         binary_code_buffer_append_u8(b, 0x7F) &&
         wcs_avx_modrm_mem_disp(b, src, base, displacement);
}
int wcs_avx_vpbroadcastd_ymm(BinaryCodeBuffer *b, int dst, int src_xmm) {
  return wcs_vex3(b, 2, 1, 1, 0, dst, src_xmm, 0) &&
         binary_code_buffer_append_u8(b, 0x58) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (src_xmm & 7)));
}

int wcs_avx_vpbroadcastd_ymm_mem(BinaryCodeBuffer *b, int dst, int base,
                                 int displacement) {
  return wcs_vex3(b, 2, 1, 1, 0, dst, base, 0) &&
         binary_code_buffer_append_u8(b, 0x58) &&
         wcs_avx_modrm_mem_disp(b, dst, base, displacement);
}

int wcs_avx_vbroadcastss_ymm_mem(BinaryCodeBuffer *b, int dst, int base,
                                 int displacement) {
  return wcs_vex3(b, 2, 1, 1, 0, dst, base, 0) &&
         binary_code_buffer_append_u8(b, 0x18) &&
         wcs_avx_modrm_mem_disp(b, dst, base, displacement);
}

int wcs_avx_vpmaddwd_ymm(BinaryCodeBuffer *b, int dst, int src1, int src2) {
  return wcs_vex3(b, 1, 1, 1, 0, dst, src2, src1) &&
         binary_code_buffer_append_u8(b, 0xF5) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (src2 & 7)));
}

int wcs_avx_vpmovsxbw_ymm_mem(BinaryCodeBuffer *b, int dst, int base,
                              int displacement) {
  return wcs_vex3(b, 2, 1, 1, 0, dst, base, 0) &&
         binary_code_buffer_append_u8(b, 0x20) &&
         wcs_avx_modrm_mem_disp(b, dst, base, displacement);
}

int wcs_avx_vpmovzxbw_ymm_mem(BinaryCodeBuffer *b, int dst, int base,
                              int displacement) {
  return wcs_vex3(b, 2, 1, 1, 0, dst, base, 0) &&
         binary_code_buffer_append_u8(b, 0x30) &&
         wcs_avx_modrm_mem_disp(b, dst, base, displacement);
}

int wcs_avx_vpmovzxbd_xmm_mem(BinaryCodeBuffer *b, int dst, int base,
                              int displacement) {
  return wcs_vex3(b, 2, 1, 0, 0, dst, base, 0) &&
         binary_code_buffer_append_u8(b, 0x31) &&
         wcs_avx_modrm_mem_disp(b, dst, base, displacement);
}

int wcs_avx_vpmovzxbd_ymm_mem(BinaryCodeBuffer *b, int dst, int base,
                              int displacement) {
  return wcs_vex3(b, 2, 1, 1, 0, dst, base, 0) &&
         binary_code_buffer_append_u8(b, 0x31) &&
         wcs_avx_modrm_mem_disp(b, dst, base, displacement);
}

int wcs_avx_vpmovsxbd_ymm_mem(BinaryCodeBuffer *b, int dst, int base,
                              int displacement) {
  return wcs_vex3(b, 2, 1, 1, 0, dst, base, 0) &&
         binary_code_buffer_append_u8(b, 0x21) &&
         wcs_avx_modrm_mem_disp(b, dst, base, displacement);
}

int wcs_avx_vpackusdw_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2) {
  return wcs_avx_0f38_ymm(b, 0x2B, dst, s1, s2);
}

int wcs_avx_vpackuswb_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2) {
  return wcs_avx_vpd_ymm(b, 0x67, dst, s1, s2);
}

int wcs_avx_vpextrb_mem_xmm(BinaryCodeBuffer *b, int base, int displacement,
                            int xmm) {
  return wcs_vex3(b, 3, 1, 0, 0, xmm, base, 0) &&
         binary_code_buffer_append_u8(b, 0x14) &&
         wcs_avx_modrm_mem_disp(b, xmm, base, displacement) &&
         binary_code_buffer_append_u8(b, 0x00);
}

int wcs_avx_vpermq_ymm(BinaryCodeBuffer *b, int dst, int src,
                       unsigned char imm) {
  return wcs_vex3(b, 3, 1, 1, 1, dst, src, 0) &&
         binary_code_buffer_append_u8(b, 0x00) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (src & 7))) &&
         binary_code_buffer_append_u8(b, imm);
}

int wcs_avx_vmovd_xmm_reg(BinaryCodeBuffer *b, int xmm, int gpr) {
  return wcs_vex3(b, 1, 1, 0, 0, xmm, gpr, 0) &&
         binary_code_buffer_append_u8(b, 0x6E) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((xmm & 7) << 3) | (gpr & 7)));
}

int wcs_avx_vmovd_xmm_mem(BinaryCodeBuffer *b, int dst, int base, int disp) {
  return wcs_vex3(b, 1, 1, 0, 0, dst, base, 0) &&
         binary_code_buffer_append_u8(b, 0x6E) &&
         wcs_avx_modrm_mem_disp(b, dst, base, disp);
}

int wcs_avx_vmovd_mem_xmm(BinaryCodeBuffer *b, int base, int disp, int src) {
  return wcs_vex3(b, 1, 1, 0, 0, src, base, 0) &&
         binary_code_buffer_append_u8(b, 0x7E) &&
         wcs_avx_modrm_mem_disp(b, src, base, disp);
}

int wcs_avx_vpsubd_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2) {
  return wcs_vex3(b, 1, 1, 1, 0, dst, s2, s1) &&
         binary_code_buffer_append_u8(b, 0xFA) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (s2 & 7)));
}

int wcs_avx_vpand_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2) {
  return wcs_vex3(b, 1, 1, 1, 0, dst, s2, s1) &&
         binary_code_buffer_append_u8(b, 0xDB) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (s2 & 7)));
}

int wcs_avx_vpor_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2) {
  return wcs_vex3(b, 1, 1, 1, 0, dst, s2, s1) &&
         binary_code_buffer_append_u8(b, 0xEB) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (s2 & 7)));
}

int wcs_cmp_reg_reg64(BinaryCodeBuffer *b, int dst, int src) {
  return binary_emit_rex(b, 1, src >> 3, 0, dst >> 3) &&
         binary_code_buffer_append_u8(b, 0x39) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((src & 7) << 3) | (dst & 7)));
}

int wcs_xor_reg_imm32(BinaryCodeBuffer *b, int gpr, uint32_t imm) {
  return binary_emit_rex(b, 0, 0, 0, gpr >> 3) &&
         binary_code_buffer_append_u8(b, 0x81) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | (6 << 3) | (gpr & 7))) &&
         binary_code_buffer_append_u32(b, imm);
}

int wcs_bsf_reg_reg32(BinaryCodeBuffer *b, int dst, int src) {
  return binary_emit_rex(b, 0, dst >> 3, 0, src >> 3) &&
         binary_code_buffer_append_u8(b, 0x0F) &&
         binary_code_buffer_append_u8(b, 0xBC) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (src & 7)));
}

int wcs_movzx_reg_byte_mem(BinaryCodeBuffer *b, int gpr, int base) {
  return binary_emit_rex(b, 0, gpr >> 3, 0, base >> 3) &&
         binary_code_buffer_append_u8(b, 0x0F) &&
         binary_code_buffer_append_u8(b, 0xB6) &&
         wcs_avx_modrm_mem_disp(b, gpr, base, 0);
}

int wcs_avx_vpcmpeqd_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2) {
  return wcs_vex3(b, 1, 1, 1, 0, dst, s2, s1) &&
         binary_code_buffer_append_u8(b, 0x76) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (s2 & 7)));
}

int wcs_avx_vpcmpgtd_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2) {
  return wcs_vex3(b, 1, 1, 1, 0, dst, s2, s1) &&
         binary_code_buffer_append_u8(b, 0x66) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (s2 & 7)));
}

int wcs_avx_vpcmpeqb_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2) {
  return wcs_vex3(b, 1, 1, 1, 0, dst, s2, s1) &&
         binary_code_buffer_append_u8(b, 0x74) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (s2 & 7)));
}

int wcs_avx_vpbroadcastb_ymm(BinaryCodeBuffer *b, int dst, int src_xmm) {
  return wcs_vex3(b, 2, 1, 1, 0, dst, src_xmm, 0) &&
         binary_code_buffer_append_u8(b, 0x78) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (src_xmm & 7)));
}

int wcs_avx_vmovmskps_reg_ymm(BinaryCodeBuffer *b, int gpr, int ymm) {
  return wcs_vex3(b, 1, 0, 1, 0, gpr, ymm, 0) &&
         binary_code_buffer_append_u8(b, 0x50) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((gpr & 7) << 3) | (ymm & 7)));
}

int wcs_avx_vpmovmskb_reg_ymm(BinaryCodeBuffer *b, int gpr, int ymm) {
  return wcs_vex3(b, 1, 1, 1, 0, gpr, ymm, 0) &&
         binary_code_buffer_append_u8(b, 0xD7) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((gpr & 7) << 3) | (ymm & 7)));
}

int wcs_sse42_pcmpistri_ranges_mem(BinaryCodeBuffer *b, int ranges_xmm,
                                   int base, unsigned char control) {
  if ((base & 7) == 4) return 0;
  return binary_code_buffer_append_u8(b, 0x66) &&
         binary_emit_rex(b, 0, ranges_xmm >> 3, 0, base >> 3) &&
         binary_code_buffer_append_u8(b, 0x0F) &&
         binary_code_buffer_append_u8(b, 0x3A) &&
         binary_code_buffer_append_u8(b, 0x63) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(((ranges_xmm & 7) << 3) | (base & 7))) &&
         binary_code_buffer_append_u8(b, control);
}

int wcs_broadcast_i32_to_ymm(BinaryCodeBuffer *b, int ymm, int gpr) {
  return wcs_avx_vmovd_xmm_reg(b, ymm, gpr) &&
         wcs_avx_vpbroadcastd_ymm(b, ymm, ymm);
}

int wcs_reduce_ymm_i32_sum_to_rax(BinaryCodeBuffer *b, int src) {
  return wcs_avx_vextracti128(b, 0, src, 1) && wcs_avx_vzeroupper(b) &&
         wcs_paddd(b, src, 0) && wcs_sse_66(b, 0x6F, 0, src) &&
         wcs_accumulate_xmm0_i32_to_rax(b);
}

int wcs_avx_vfmadd231pd_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2) {
  return wcs_vex3(b, 2, 1, 1, 1, dst, s2, s1) &&
         binary_code_buffer_append_u8(b, 0xB8) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (s2 & 7)));
}
int wcs_avx_vfmadd231ps_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2) {
  return wcs_vex3(b, 2, 1, 1, 0, dst, s2, s1) &&
         binary_code_buffer_append_u8(b, 0xB8) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (s2 & 7)));
}
int wcs_fmadd231sd(BinaryCodeBuffer *b, int dst, int s1, int s2) {
  return wcs_vex3(b, 2, 1, 0, 1, dst, s2, s1) &&
         binary_code_buffer_append_u8(b, 0xB9) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (s2 & 7)));
}
int wcs_fmadd231ss(BinaryCodeBuffer *b, int dst, int s1, int s2) {
  return wcs_vex3(b, 2, 1, 0, 0, dst, s2, s1) &&
         binary_code_buffer_append_u8(b, 0xB9) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (s2 & 7)));
}
