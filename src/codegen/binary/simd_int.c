#include "codegen/binary/internal.h"
#include "codegen/binary/simd_internal.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int code_generator_binary_emit_simd_sum_i32(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  BinaryCodeBuffer *b = NULL;
  size_t loop_top = 0;
  size_t j_done = 0;
  size_t j_vec = 0;
  size_t j_scalar = 0;

  if (!generator || !context || !instruction) {
    return 0;
  }
  b = &context->code;

  if (!code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->dest,
                                               BINARY_GP_RAX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->lhs,
                                               BINARY_GP_RCX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->rhs,
                                               BINARY_GP_R8)) {
    return 0;
  }
  if (!wcs_xor_self32(b, BINARY_GP_RDX) ||
      !wcs_avx_vpxor_ymm(b, 2, 2, 2)) {
    return 0;
  }

  loop_top = b->size;
  if (!wcs_cmp_reg_reg64(b, BINARY_GP_RDX, BINARY_GP_R8)) return 0;
  if (!wcs_jcc(b, 0x8D , &j_done)) return 0;

  if (!wcs_mov_reg_reg32(b, BINARY_GP_R9, BINARY_GP_R8) ||
      !wcs_sub_reg_reg32(b, BINARY_GP_R9, BINARY_GP_RDX) ||
      !wcs_cmp_reg_imm32(b, BINARY_GP_R9, 8) ||
      !wcs_jcc(b, 0x83 , &j_vec)) {
    return 0;
  }
  if (!wcs_jcc(b, 0, &j_scalar)) return 0;

  if (!wcs_patch_here(b, j_vec) ||
      !wcs_avx_vpmovsxdq_ymm_mem(b, 0, BINARY_GP_RCX, 0) ||
      !wcs_avx_vpaddq_ymm(b, 2, 2, 0) ||
      !wcs_avx_vpmovsxdq_ymm_mem(b, 1, BINARY_GP_RCX, 16) ||
      !wcs_avx_vpaddq_ymm(b, 2, 2, 1) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 32) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 8)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, loop_top)) {
      return 0;
    }
  }

  if (!wcs_patch_here(b, j_scalar)) return 0;
  if (!binary_emit_mov_reg_mem32(b, BINARY_GP_R10, BINARY_GP_RCX, 0) ||
      !binary_emit_movsxd_reg_reg32(b, BINARY_GP_R10, BINARY_GP_R10) ||
      !wcs_add_reg_reg64(b, BINARY_GP_RAX, BINARY_GP_R10) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 4) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 1)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, loop_top)) {
      return 0;
    }
  }

  if (!wcs_patch_here(b, j_done) ||
      !wcs_avx_vextracti128(b, 0, 2, 1) ||
      !wcs_avx_vzeroupper(b) ||
      !wcs_paddq(b, 2, 0) ||
      !binary_emit_movq_reg_xmm(b, BINARY_GP_R10, BINARY_XMM2) ||
      !wcs_add_reg_reg64(b, BINARY_GP_RAX, BINARY_GP_R10) ||
      !wcs_pshufd(b, 0, 2, 0xEE) ||
      !binary_emit_movq_reg_xmm(b, BINARY_GP_R10, BINARY_XMM0) ||
      !wcs_add_reg_reg64(b, BINARY_GP_RAX, BINARY_GP_R10)) {
    return 0;
  }

  return code_generator_binary_emit_destination_store(generator, context,
                                                      &instruction->dest,
                                                      BINARY_GP_RAX);
}

int code_generator_binary_emit_simd_lcg_u32(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  BinaryCodeBuffer *b = NULL;
  uint32_t A = 0, C = 0, MASK = 0;
  uint32_t P[9], Q[9];
  size_t loop_top = 0, j_done = 0, jb = 0, tail_top = 0, t_done = 0, tb = 0;

  if (!generator || !context || !instruction || instruction->argument_count < 3 ||
      !instruction->arguments) {
    code_generator_set_error(generator, "Malformed simd_lcg_u32");
    return 0;
  }
  b = &context->code;
  A = (uint32_t)instruction->arguments[0].int_value;
  C = (uint32_t)instruction->arguments[1].int_value;
  MASK = (uint32_t)instruction->arguments[2].int_value;
  P[0] = 1u;
  Q[0] = 0u;
  for (int j = 1; j <= 8; j++) {
    P[j] = P[j - 1] * A;
    Q[j] = Q[j - 1] * A + C;
  }

  if (!code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->dest,
                                               BINARY_GP_RAX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->rhs,
                                               BINARY_GP_RCX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->lhs,
                                               BINARY_GP_R8)) {
    return 0;
  }

  if (!binary_emit_sub_rsp_imm32(b, 128)) {
    return 0;
  }
  for (int x = 6; x <= 9; x++) {
    if (!simd_movdqu_mem_xmm_disp(b, BINARY_GP_RSP, 64 + 16 * (x - 6), x)) {
      return 0;
    }
  }
  for (int j = 0; j < 8; j++) {
    if (!binary_emit_mov_reg_imm32_zero_extend(b, BINARY_GP_R11, P[j + 1]) ||
        !binary_emit_mov_mem_reg32(b, BINARY_GP_RSP, 4 * j, BINARY_GP_R11) ||
        !binary_emit_mov_reg_imm32_zero_extend(b, BINARY_GP_R11, Q[j + 1]) ||
        !binary_emit_mov_mem_reg32(b, BINARY_GP_RSP, 32 + 4 * j,
                                   BINARY_GP_R11)) {
      return 0;
    }
  }

  if (!binary_emit_mov_reg_reg(b, BINARY_GP_R9, BINARY_GP_R8) ||
      !binary_emit_shift_reg_imm8(b, 5 , BINARY_GP_R9, 3) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_R10, BINARY_GP_R8) ||
      !binary_emit_and_reg_imm32(b, BINARY_GP_R10, 7)) {
    return 0;
  }

  if (!wcs_avx_vmovd_xmm_reg(b, 1, BINARY_GP_RCX) ||
      !wcs_avx_vpbroadcastd_ymm(b, 1, 1) ||
      !wcs_avx_vmovdqu_ymm_mem(b, 0, BINARY_GP_RSP, 0) ||
      !wcs_avx_vpmulld_ymm(b, 0, 1, 0) ||
      !wcs_avx_vmovdqu_ymm_mem(b, 2, BINARY_GP_RSP, 32) ||
      !wcs_avx_vpaddd_ymm(b, 0, 0, 2)) {
    return 0;
  }

  if (!binary_emit_mov_reg_imm32_zero_extend(b, BINARY_GP_R11, MASK) ||
      !wcs_avx_vmovd_xmm_reg(b, 3, BINARY_GP_R11) ||
      !wcs_avx_vpbroadcastd_ymm(b, 3, 3) ||
      !binary_emit_mov_reg_imm32_zero_extend(b, BINARY_GP_R11, P[8]) ||
      !wcs_avx_vmovd_xmm_reg(b, 4, BINARY_GP_R11) ||
      !wcs_avx_vpbroadcastd_ymm(b, 4, 4) ||
      !binary_emit_mov_reg_imm32_zero_extend(b, BINARY_GP_R11, Q[8]) ||
      !wcs_avx_vmovd_xmm_reg(b, 5, BINARY_GP_R11) ||
      !wcs_avx_vpbroadcastd_ymm(b, 5, 5)) {
    return 0;
  }

  if (!wcs_avx_vpxor_ymm(b, 6, 6, 6) || !wcs_avx_vpxor_ymm(b, 7, 7, 7)) {
    return 0;
  }

  loop_top = b->size;
  if (!wcs_cmp_reg_imm32(b, BINARY_GP_R9, 0) ||
      !wcs_jcc(b, 0x84 , &j_done)) {
    return 0;
  }
  if (!wcs_avx_vpand_ymm(b, 8, 0, 3) || !wcs_avx_vpmovsxdq_ymm_xmm(b, 9, 8) ||
      !wcs_avx_vpaddq_ymm(b, 6, 6, 9) || !wcs_avx_vextracti128(b, 8, 8, 1) ||
      !wcs_avx_vpmovsxdq_ymm_xmm(b, 9, 8) || !wcs_avx_vpaddq_ymm(b, 7, 7, 9)) {
    return 0;
  }
  if (!wcs_avx_vpmulld_ymm(b, 0, 0, 4) || !wcs_avx_vpaddd_ymm(b, 0, 0, 5)) {
    return 0;
  }
  if (!wcs_addsub_reg_imm8(b, BINARY_GP_R9, 1 , 1)) {
    return 0;
  }
  if (!wcs_jcc(b, 0, &jb) || !wcs_patch_to(b, jb, loop_top)) {
    return 0;
  }
  if (!wcs_patch_here(b, j_done)) {
    return 0;
  }

  if (!wcs_avx_vmovd_mem_xmm(b, BINARY_GP_RSP, 0, 0)) {
    return 0;
  }

  if (!wcs_avx_vpaddq_ymm(b, 6, 6, 7) || !wcs_avx_vextracti128(b, 1, 6, 1) ||
      !wcs_avx_vzeroupper(b) || !wcs_paddq(b, 6, 1) ||
      !binary_emit_movq_reg_xmm(b, BINARY_GP_R11, BINARY_XMM6) ||
      !wcs_add_reg_reg64(b, BINARY_GP_RAX, BINARY_GP_R11) ||
      !wcs_pshufd(b, 1, 6, 0xEE) ||
      !binary_emit_movq_reg_xmm(b, BINARY_GP_R11, BINARY_XMM1) ||
      !wcs_add_reg_reg64(b, BINARY_GP_RAX, BINARY_GP_R11)) {
    return 0;
  }

  if (!binary_emit_mov_reg_mem32(b, BINARY_GP_RCX, BINARY_GP_RSP, 0)) {
    return 0;
  }
  tail_top = b->size;
  if (!wcs_cmp_reg_imm32(b, BINARY_GP_R10, 0) ||
      !wcs_jcc(b, 0x84 , &t_done)) {
    return 0;
  }
  if (!wcs_mov_reg_reg32(b, BINARY_GP_RDX, BINARY_GP_RCX) ||
      !binary_emit_and_reg_imm32(b, BINARY_GP_RDX, MASK) ||
      !wcs_add_reg_reg64(b, BINARY_GP_RAX, BINARY_GP_RDX) ||
      !binary_emit_imul_reg_reg_imm32(b, BINARY_GP_RCX, BINARY_GP_RCX, A) ||
      !binary_emit_and_reg_imm32(b, BINARY_GP_RCX, 0xFFFFFFFFu) ||
      !binary_emit_add_reg_imm32(b, BINARY_GP_RCX, C) ||
      !binary_emit_and_reg_imm32(b, BINARY_GP_RCX, 0xFFFFFFFFu)) {
    return 0;
  }
  if (!wcs_addsub_reg_imm8(b, BINARY_GP_R10, 1 , 1)) {
    return 0;
  }
  if (!wcs_jcc(b, 0, &tb) || !wcs_patch_to(b, tb, tail_top)) {
    return 0;
  }
  if (!wcs_patch_here(b, t_done)) {
    return 0;
  }

  for (int x = 6; x <= 9; x++) {
    if (!simd_movdqu_xmm_mem_disp(b, x, BINARY_GP_RSP, 64 + 16 * (x - 6))) {
      return 0;
    }
  }
  if (!binary_emit_add_rsp_imm32(b, 128)) {
    return 0;
  }
  return code_generator_binary_emit_destination_store(generator, context,
                                                      &instruction->dest,
                                                      BINARY_GP_RAX);
}

int code_generator_binary_emit_simd_sum_u8(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  BinaryCodeBuffer *b = NULL;
  size_t loop_top = 0;
  size_t j_done = 0;
  size_t j_vec = 0;
  size_t j_scalar = 0;

  if (!generator || !context || !instruction) {
    return 0;
  }
  b = &context->code;

  if (!code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->dest,
                                               BINARY_GP_RAX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->lhs,
                                               BINARY_GP_RCX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->rhs,
                                               BINARY_GP_R8)) {
    return 0;
  }
  if (!wcs_xor_self32(b, BINARY_GP_RDX) ||
      !wcs_avx_vpxor_ymm(b, 2, 2, 2) ||
      !wcs_avx_vpxor_ymm(b, 3, 3, 3)) {
    return 0;
  }

  loop_top = b->size;
  if (!wcs_cmp_reg_reg64(b, BINARY_GP_RDX, BINARY_GP_R8)) return 0;
  if (!wcs_jcc(b, 0x8D , &j_done)) return 0;

  if (!wcs_mov_reg_reg32(b, BINARY_GP_R9, BINARY_GP_R8) ||
      !wcs_sub_reg_reg32(b, BINARY_GP_R9, BINARY_GP_RDX) ||
      !wcs_cmp_reg_imm32(b, BINARY_GP_R9, 32) ||
      !wcs_jcc(b, 0x83 , &j_vec)) {
    return 0;
  }
  if (!wcs_jcc(b, 0, &j_scalar)) return 0;

  if (!wcs_patch_here(b, j_vec) ||
      !wcs_avx_vmovdqu_ymm_mem(b, 0, BINARY_GP_RCX, 0) ||
      !wcs_avx_vpsadbw_ymm(b, 0, 0, 3) ||
      !wcs_avx_vpaddq_ymm(b, 2, 2, 0) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 32) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 32)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, loop_top)) {
      return 0;
    }
  }

  if (!wcs_patch_here(b, j_scalar)) return 0;
  if (!binary_emit_movzx_reg_mem8(b, BINARY_GP_R10, BINARY_GP_RCX, 0) ||
      !wcs_add_reg_reg64(b, BINARY_GP_RAX, BINARY_GP_R10) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 1) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 1)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, loop_top)) {
      return 0;
    }
  }

  if (!wcs_patch_here(b, j_done) ||
      !wcs_avx_vextracti128(b, 0, 2, 1) ||
      !wcs_avx_vzeroupper(b) ||
      !wcs_paddq(b, 2, 0) ||
      !binary_emit_movq_reg_xmm(b, BINARY_GP_R10, BINARY_XMM2) ||
      !wcs_add_reg_reg64(b, BINARY_GP_RAX, BINARY_GP_R10) ||
      !wcs_pshufd(b, 0, 2, 0xEE) ||
      !binary_emit_movq_reg_xmm(b, BINARY_GP_R10, BINARY_XMM0) ||
      !wcs_add_reg_reg64(b, BINARY_GP_RAX, BINARY_GP_R10)) {
    return 0;
  }

  return code_generator_binary_emit_destination_store(generator, context,
                                                      &instruction->dest,
                                                      BINARY_GP_RAX);
}

static int byte_map_emit_step_vec(BinaryCodeBuffer *b, int op, int bcast_off,
                                  int mask_off) {
  switch (op) {
  case IR_BYTE_MAP_ADD:
    return wcs_avx_vmovdqu_xmm_mem(b, 1, BINARY_GP_RSP, bcast_off) &&
           wcs_avx_vpaddb_xmm(b, 0, 0, 1);
  case IR_BYTE_MAP_SUB:
    return wcs_avx_vmovdqu_xmm_mem(b, 1, BINARY_GP_RSP, bcast_off) &&
           wcs_avx_vpsubb_xmm(b, 0, 0, 1);
  case IR_BYTE_MAP_XOR:
    return wcs_avx_vmovdqu_xmm_mem(b, 1, BINARY_GP_RSP, bcast_off) &&
           wcs_avx_vpxor_xmm(b, 0, 0, 1);
  case IR_BYTE_MAP_AND:
    return wcs_avx_vmovdqu_xmm_mem(b, 1, BINARY_GP_RSP, bcast_off) &&
           wcs_avx_vpand_xmm(b, 0, 0, 1);
  case IR_BYTE_MAP_OR:
    return wcs_avx_vmovdqu_xmm_mem(b, 1, BINARY_GP_RSP, bcast_off) &&
           wcs_avx_vpor_xmm(b, 0, 0, 1);
  case IR_BYTE_MAP_MUL:
    return wcs_avx_vpxor_xmm(b, 3, 3, 3) &&
           wcs_avx_vpunpcklbw_xmm(b, 1, 0, 3) &&
           wcs_avx_vpunpckhbw_xmm(b, 2, 0, 3) &&
           wcs_avx_vmovdqu_xmm_mem(b, 4, BINARY_GP_RSP, bcast_off) &&
           wcs_avx_vpmullw_xmm(b, 1, 1, 4) &&
           wcs_avx_vpmullw_xmm(b, 2, 2, 4) &&
           wcs_avx_vmovdqu_xmm_mem(b, 5, BINARY_GP_RSP, mask_off) &&
           wcs_avx_vpand_xmm(b, 1, 1, 5) &&
           wcs_avx_vpand_xmm(b, 2, 2, 5) &&
           wcs_avx_vpackuswb_xmm(b, 0, 1, 2);
  default:
    return 0;
  }
}

static int byte_map_emit_step_scalar(BinaryCodeBuffer *b, int op, int k) {
  switch (op) {
  case IR_BYTE_MAP_ADD:
    return binary_emit_add_reg_imm32(b, BINARY_GP_R10, (uint32_t)k);
  case IR_BYTE_MAP_SUB:
    return binary_emit_sub_reg_imm32(b, BINARY_GP_R10, (uint32_t)k);
  case IR_BYTE_MAP_XOR:
    return binary_emit_xor_reg_imm32(b, BINARY_GP_R10, (uint32_t)k);
  case IR_BYTE_MAP_AND:
    return binary_emit_and_reg_imm32(b, BINARY_GP_R10, (uint32_t)k);
  case IR_BYTE_MAP_OR:
    return binary_emit_or_reg_imm32(b, BINARY_GP_R10, (uint32_t)k);
  case IR_BYTE_MAP_MUL:
    return binary_emit_imul_reg_reg_imm32(b, BINARY_GP_R10, BINARY_GP_R10,
                                          (uint32_t)k);
  default:
    return 0;
  }
}

static int fill_emit_element_store(BinaryCodeBuffer *b, long long size) {
  switch (size) {
  case 1: return binary_emit_mov_mem_reg8(b, BINARY_GP_RCX, 0, BINARY_GP_RAX);
  case 2: return binary_emit_mov_mem_reg16(b, BINARY_GP_RCX, 0, BINARY_GP_RAX);
  case 4: return binary_emit_mov_mem_reg32(b, BINARY_GP_RCX, 0, BINARY_GP_RAX);
  case 8: return binary_emit_mov_mem_reg(b, BINARY_GP_RCX, 0, BINARY_GP_RAX);
  default: return 0;
  }
}

int code_generator_binary_emit_simd_fill_splat(BinaryCodeBuffer *b,
                                               long long size) {
  if (size == 1) {
    if (!binary_emit_and_reg_imm32(b, BINARY_GP_RAX, 0xFF) ||
        !binary_emit_imul_reg_reg_imm32(b, BINARY_GP_RAX, BINARY_GP_RAX,
                                        0x01010101u)) {
      return 0;
    }
  } else if (size == 2) {
    if (!binary_emit_and_reg_imm32(b, BINARY_GP_RAX, 0xFFFF) ||
        !binary_emit_imul_reg_reg_imm32(b, BINARY_GP_RAX, BINARY_GP_RAX,
                                        0x00010001u)) {
      return 0;
    }
  } else if (size == 4) {
    if (!wcs_mov_reg_reg32(b, BINARY_GP_RAX, BINARY_GP_RAX)) {
      return 0;
    }
  }
  if (size != 8) {
    if (!binary_emit_mov_reg_reg(b, BINARY_GP_R9, BINARY_GP_RAX) ||
        !binary_emit_shift_reg_imm8(b, 4 , BINARY_GP_RAX, 32) ||
        !wcs_add_reg_reg64(b, BINARY_GP_RAX, BINARY_GP_R9)) {
      return 0;
    }
  }
  if (!binary_emit_movq_xmm_reg(b, BINARY_XMM0, BINARY_GP_RAX) ||
      !wcs_avx_vpunpcklqdq_xmm(b, 0, 0, 0)) {
    return 0;
  }
  return 1;
}

int code_generator_binary_emit_simd_fill_loop_mode0(BinaryCodeBuffer *b,
                                                    long long size) {
  long long per_vec = 16 / size;
  size_t j_done_neg = 0, j_done = 0, j_vec = 0, j_scalar = 0;
  size_t loop_top = 0;
  if (!binary_emit_test_reg_reg(b, BINARY_GP_R8) ||
      !wcs_jcc(b, 0x8E , &j_done_neg) ||
      !wcs_xor_self32(b, BINARY_GP_RDX)) {
    return 0;
  }
  loop_top = b->size;
  if (!wcs_cmp_reg_reg64(b, BINARY_GP_RDX, BINARY_GP_R8) ||
      !wcs_jcc(b, 0x8D , &j_done)) {
    return 0;
  }
  if (!wcs_mov_reg_reg32(b, BINARY_GP_R9, BINARY_GP_R8) ||
      !wcs_sub_reg_reg32(b, BINARY_GP_R9, BINARY_GP_RDX) ||
      !wcs_cmp_reg_imm32(b, BINARY_GP_R9, (uint32_t)per_vec) ||
      !wcs_jcc(b, 0x83 , &j_vec) ||
      !wcs_jcc(b, 0, &j_scalar)) {
    return 0;
  }
  if (!wcs_patch_here(b, j_vec) ||
      !wcs_avx_vmovdqu_mem_xmm(b, BINARY_GP_RCX, 0, 0) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 16) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, (unsigned char)per_vec)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, loop_top)) {
      return 0;
    }
  }
  if (!wcs_patch_here(b, j_scalar) ||
      !fill_emit_element_store(b, size) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, (unsigned char)size) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 1)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, loop_top)) {
      return 0;
    }
  }
  if (!wcs_patch_here(b, j_done) || !wcs_patch_here(b, j_done_neg)) {
    return 0;
  }
  return 1;
}

int code_generator_binary_emit_simd_fill_loop_bytewalk(BinaryCodeBuffer *b,
                                                       long long size, int mode) {
  size_t j_done = 0, j_tail = 0;
  size_t loop16_top = 0, tail_top = 0;
  if (mode == 1 && !wcs_sub_reg_reg64(b, BINARY_GP_R8, BINARY_GP_RCX)) {
    return 0;
  }
  loop16_top = b->size;
  if (!binary_emit_cmp_reg_imm32(b, BINARY_GP_R8, 16) ||
      !wcs_jcc(b, 0x8C , &j_tail)) {
    return 0;
  }
  if (!wcs_avx_vmovdqu_mem_xmm(b, BINARY_GP_RCX, 0, 0) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 16) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_R8, 1, 16)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, loop16_top)) {
      return 0;
    }
  }
  if (!wcs_patch_here(b, j_tail)) {
    return 0;
  }
  tail_top = b->size;
  if (!binary_emit_test_reg_reg(b, BINARY_GP_R8) ||
      !wcs_jcc(b, 0x8E , &j_done)) {
    return 0;
  }
  if (!fill_emit_element_store(b, size) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, (unsigned char)size) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_R8, 1, (unsigned char)size)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, tail_top)) {
      return 0;
    }
  }
  if (!wcs_patch_here(b, j_done)) {
    return 0;
  }
  return 1;
}

int code_generator_binary_emit_simd_byte_map(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  BinaryCodeBuffer *b = NULL;
  size_t nsteps = 0;
  int mask_off = 0;
  uint32_t cbytes = 0;
  size_t loop_top = 0;
  size_t j_done = 0;
  size_t j_vec = 0;
  size_t j_scalar = 0;

  if (!generator || !context || !instruction ||
      instruction->argument_count == 0 ||
      instruction->argument_count % 2 != 0) {
    return 0;
  }
  b = &context->code;
  nsteps = instruction->argument_count / 2;
  mask_off = 16 * (int)nsteps;
  cbytes = (uint32_t)(16 * ((int)nsteps + 1));

  if (!binary_emit_sub_rsp_imm32(b, cbytes)) {
    return 0;
  }

  for (size_t s = 0; s < nsteps; s++) {
    int op = (int)instruction->arguments[2 * s].int_value;
    int k = (int)instruction->arguments[2 * s + 1].int_value;
    uint64_t splat;
    if (op == IR_BYTE_MAP_MUL) {
      uint64_t w = (uint64_t)(k & 0xFFFF);
      splat = w | (w << 16) | (w << 32) | (w << 48);
    } else {
      uint64_t byte = (uint64_t)(k & 0xFF);
      splat = byte * 0x0101010101010101ULL;
    }
    if (!binary_emit_mov_reg_imm64(b, BINARY_GP_RAX, splat) ||
        !binary_emit_movq_xmm_reg(b, BINARY_XMM0, BINARY_GP_RAX) ||
        !wcs_avx_vpunpcklqdq_xmm(b, 0, 0, 0) ||
        !wcs_avx_vmovdqu_mem_xmm(b, BINARY_GP_RSP, 16 * (int)s, 0)) {
      return 0;
    }
  }
  if (!binary_emit_mov_reg_imm64(b, BINARY_GP_RAX, 0x00FF00FF00FF00FFULL) ||
      !binary_emit_movq_xmm_reg(b, BINARY_XMM0, BINARY_GP_RAX) ||
      !wcs_avx_vpunpcklqdq_xmm(b, 0, 0, 0) ||
      !wcs_avx_vmovdqu_mem_xmm(b, BINARY_GP_RSP, mask_off, 0)) {
    return 0;
  }

  if (!code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->lhs,
                                               BINARY_GP_RCX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->rhs,
                                               BINARY_GP_R8) ||
      !wcs_xor_self32(b, BINARY_GP_RDX)) {
    return 0;
  }

  loop_top = b->size;
  if (!wcs_cmp_reg_reg64(b, BINARY_GP_RDX, BINARY_GP_R8) ||
      !wcs_jcc(b, 0x8D , &j_done)) {
    return 0;
  }
  if (!wcs_mov_reg_reg32(b, BINARY_GP_R9, BINARY_GP_R8) ||
      !wcs_sub_reg_reg32(b, BINARY_GP_R9, BINARY_GP_RDX) ||
      !wcs_cmp_reg_imm32(b, BINARY_GP_R9, 16) ||
      !wcs_jcc(b, 0x83 , &j_vec) ||
      !wcs_jcc(b, 0, &j_scalar)) {
    return 0;
  }

  if (!wcs_patch_here(b, j_vec) ||
      !wcs_avx_vmovdqu_xmm_mem(b, 0, BINARY_GP_RCX, 0)) {
    return 0;
  }
  for (size_t s = 0; s < nsteps; s++) {
    int op = (int)instruction->arguments[2 * s].int_value;
    if (!byte_map_emit_step_vec(b, op, 16 * (int)s, mask_off)) {
      return 0;
    }
  }
  if (!wcs_avx_vmovdqu_mem_xmm(b, BINARY_GP_RCX, 0, 0) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 16) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 16)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, loop_top)) {
      return 0;
    }
  }

  if (!wcs_patch_here(b, j_scalar) ||
      !binary_emit_movzx_reg_mem8(b, BINARY_GP_R10, BINARY_GP_RCX, 0)) {
    return 0;
  }
  for (size_t s = 0; s < nsteps; s++) {
    int op = (int)instruction->arguments[2 * s].int_value;
    int k = (int)instruction->arguments[2 * s + 1].int_value;
    if (!byte_map_emit_step_scalar(b, op, k)) {
      return 0;
    }
  }
  if (!binary_emit_mov_mem_reg8(b, BINARY_GP_RCX, 0, BINARY_GP_R10) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 1) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 1)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, loop_top)) {
      return 0;
    }
  }

  if (!wcs_patch_here(b, j_done) ||
      !binary_emit_add_rsp_imm32(b, cbytes)) {
    return 0;
  }
  return 1;
}

int code_generator_binary_emit_simd_insertion_sort_i32(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  BinaryCodeBuffer *b = NULL;
  size_t outer_loop = 0;
  size_t inner_loop = 0;
  size_t j_insert_from_bound = 0;
  size_t j_insert_from_le = 0;
  size_t j_done = 0;

  if (!generator || !context || !instruction ||
      instruction->dest.kind == IR_OPERAND_NONE ||
      instruction->rhs.kind == IR_OPERAND_NONE) {
    code_generator_set_error(generator, "Malformed simd_insertion_sort_i32");
    return 0;
  }
  b = &context->code;

  if (!code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->dest,
                                               BINARY_GP_RCX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->rhs,
                                               BINARY_GP_R8) ||
      !wcs_xor_self32(b, BINARY_GP_RDX) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 1)) {
    return 0;
  }

  outer_loop = b->size;
  if (!wcs_cmp_reg_reg64(b, BINARY_GP_RDX, BINARY_GP_R8) ||
      !wcs_jcc(b, 0x8D , &j_done)) {
    return 0;
  }

  if (!binary_emit_lea_reg_base_index_scale_disp(
          b, BINARY_GP_RAX, BINARY_GP_RCX, BINARY_GP_RDX, 4, 0) ||
      !binary_emit_mov_reg_mem32(b, BINARY_GP_R9, BINARY_GP_RAX, 0) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_R10, BINARY_GP_RAX) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_R10, 1, 4)) {
    return 0;
  }

  context->wants_wide_loop_alignment = 1;
  if (!binary_emit_align_code(b, BINARY_LOOP_ALIGN_BIG,
                              BINARY_LOOP_ALIGN_BIG_MAX_PAD)) {
    return 0;
  }
  inner_loop = b->size;
  if (!binary_emit_cmp_reg_reg(b, BINARY_GP_R10, BINARY_GP_RCX) ||
      !wcs_jcc(b, 0x82 , &j_insert_from_bound) ||
      !binary_emit_mov_reg_mem32(b, BINARY_GP_R11, BINARY_GP_R10, 0) ||
      !wcs_cmp_reg_reg32(b, BINARY_GP_R11, BINARY_GP_R9) ||
      !wcs_jcc(b, 0x8E , &j_insert_from_le) ||
      !binary_emit_mov_mem_reg32(b, BINARY_GP_R10, 4, BINARY_GP_R11) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_R10, 1, 4)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, inner_loop)) {
      return 0;
    }
  }

  if (!wcs_patch_here(b, j_insert_from_bound) ||
      !wcs_patch_here(b, j_insert_from_le) ||
      !binary_emit_mov_mem_reg32(b, BINARY_GP_R10, 4, BINARY_GP_R9) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 1)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, outer_loop)) {
      return 0;
    }
  }

  return wcs_patch_here(b, j_done);
}

int code_generator_binary_emit_lower_bound_i32(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  BinaryCodeBuffer *b = NULL;
  size_t loop_top = 0;
  size_t j_done = 0;
  size_t j_back = 0;

  if (!generator || !context || !instruction || instruction->argument_count < 1) {
    return 0;
  }
  b = &context->code;

  if (!binary_emit_push_reg(b, BINARY_GP_RSI)) {
    return 0;
  }

  if (!code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->dest,
                                               BINARY_GP_R10) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->rhs,
                                               BINARY_GP_R11) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->arguments[0],
                                               BINARY_GP_R9) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->lhs,
                                               BINARY_GP_RSI)) {
    return 0;
  }

  loop_top = b->size;
  if (!binary_emit_cmp_reg_reg(b, BINARY_GP_R10, BINARY_GP_R11) ||
      !wcs_jcc(b, 0x8D , &j_done) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_RAX, BINARY_GP_R11) ||
      !binary_emit_alu_reg_reg(b, 0x29, BINARY_GP_RAX, BINARY_GP_R10) ||
      !binary_emit_shift_reg_imm8(b, 7, BINARY_GP_RAX, 1) ||
      !binary_emit_alu_reg_reg(b, 0x01, BINARY_GP_RAX, BINARY_GP_R10) ||
      !binary_emit_lea_reg_base_index_scale_disp(
          b, BINARY_GP_RCX, BINARY_GP_RSI, BINARY_GP_RAX, 4, 0) ||
      !binary_emit_mov_reg_mem32(b, BINARY_GP_RCX, BINARY_GP_RCX, 0) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_RDX, BINARY_GP_RAX) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_R8, BINARY_GP_RAX) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_R8, 0, 1) ||
      !wcs_cmp_reg_reg32(b, BINARY_GP_RCX, BINARY_GP_R9) ||
      !binary_emit_cmovcc_reg_reg(b, 0x4C , BINARY_GP_R10,
                                  BINARY_GP_R8) ||
      !binary_emit_cmovcc_reg_reg(b, 0x4D , BINARY_GP_R11,
                                  BINARY_GP_RDX) ||
      !wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, loop_top)) {
    return 0;
  }
  j_back = 0;
  if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, loop_top) ||
      !wcs_patch_here(b, j_done) ||
      !code_generator_binary_emit_destination_store(generator, context,
                                                    &instruction->dest,
                                                    BINARY_GP_R10)) {
    return 0;
  }
  if (!binary_emit_pop_reg(b, BINARY_GP_RSI)) {
    return 0;
  }
  return 1;
}

int code_generator_binary_emit_simd_scale_i32(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  BinaryCodeBuffer *b = NULL;
  size_t loop_top = 0;
  size_t j_done = 0;
  size_t j_vec = 0;
  size_t j_scalar = 0;
  int32_t mul_imm = 0;
  int32_t add_imm = 0;

  if (!generator || !context || !instruction ||
      instruction->argument_count < 3) {
    return 0;
  }
  b = &context->code;
  mul_imm = (int32_t)instruction->arguments[1].int_value;
  add_imm = (int32_t)instruction->arguments[2].int_value;

  if (!code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->dest,
                                               BINARY_GP_RAX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->lhs,
                                               BINARY_GP_RCX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->rhs,
                                               BINARY_GP_RDX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->arguments[0],
                                               BINARY_GP_R11) ||
      !wcs_mov_reg_reg32(b, BINARY_GP_R8, BINARY_GP_R11) ||
      !wcs_shift_reg_imm(b, BINARY_GP_R8, 0, 2) ||
      !wcs_add_reg_reg64(b, BINARY_GP_R8, BINARY_GP_RCX) ||
      !wcs_mov_reg_imm32(b, BINARY_GP_R9, (uint32_t)mul_imm) ||
      !wcs_broadcast_i32_to_ymm(b, 4, BINARY_GP_R9) ||
      !wcs_mov_reg_imm32(b, BINARY_GP_R9, (uint32_t)add_imm) ||
      !wcs_broadcast_i32_to_ymm(b, 5, BINARY_GP_R9) ||
      !wcs_avx_vpxor_ymm(b, 2, 2, 2)) {
    return 0;
  }

  loop_top = b->size;
  if (!binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R8) ||
      !wcs_jcc(b, 0x83 , &j_done)) {
    return 0;
  }

  if (!binary_emit_mov_reg_reg(b, BINARY_GP_R9, BINARY_GP_R8) ||
      !binary_emit_alu_reg_reg(b, 0x29, BINARY_GP_R9, BINARY_GP_RCX) ||
      !wcs_cmp_reg_imm32(b, BINARY_GP_R9, 64) ||
      !wcs_jcc(b, 0x83 , &j_vec) ||
      !wcs_jcc(b, 0, &j_scalar)) {
    return 0;
  }

  if (!wcs_patch_here(b, j_vec) ||
      !wcs_avx_vmovdqu_ymm_mem(b, 0, BINARY_GP_RCX, 0) ||
      !wcs_avx_vpmulld_ymm(b, 0, 0, 4) ||
      !wcs_avx_vpaddd_ymm(b, 0, 0, 5) ||
      !wcs_avx_vmovdqu_mem_ymm(b, BINARY_GP_RDX, 0, 0) ||
      !wcs_avx_vpaddd_ymm(b, 2, 2, 0) ||
      !wcs_avx_vmovdqu_ymm_mem(b, 0, BINARY_GP_RCX, 32) ||
      !wcs_avx_vpmulld_ymm(b, 0, 0, 4) ||
      !wcs_avx_vpaddd_ymm(b, 0, 0, 5) ||
      !wcs_avx_vmovdqu_mem_ymm(b, BINARY_GP_RDX, 32, 0) ||
      !wcs_avx_vpaddd_ymm(b, 2, 2, 0) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 64) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 64)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, loop_top)) {
      return 0;
    }
  }

  if (!wcs_patch_here(b, j_scalar) ||
      !binary_emit_mov_reg_mem32(b, BINARY_GP_R10, BINARY_GP_RCX, 0) ||
      !binary_emit_imul_reg_reg_imm32(b, BINARY_GP_R10, BINARY_GP_R10,
                                      (uint32_t)mul_imm) ||
      !binary_emit_add_reg_imm32(b, BINARY_GP_R10, (uint32_t)add_imm) ||
      !binary_emit_mov_mem_reg32(b, BINARY_GP_RDX, 0, BINARY_GP_R10) ||
      !binary_emit_movsxd_reg_reg32(b, BINARY_GP_R10, BINARY_GP_R10) ||
      !wcs_add_reg_reg64(b, BINARY_GP_RAX, BINARY_GP_R10) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 4) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 4)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, loop_top)) {
      return 0;
    }
  }

  if (!wcs_patch_here(b, j_done) ||
      !wcs_reduce_ymm_i32_sum_to_rax(b, 2)) {
    return 0;
  }
  return code_generator_binary_emit_destination_store(generator, context,
                                                      &instruction->dest,
                                                      BINARY_GP_RAX);
}

static int copy_move_element(BinaryCodeBuffer *b, long long size) {
  if (size == 1) {
    return binary_emit_movzx_reg_mem8(b, BINARY_GP_RAX, BINARY_GP_RDX, 0) &&
           binary_emit_mov_mem_reg8(b, BINARY_GP_RCX, 0, BINARY_GP_RAX);
  }
  if (size == 2) {
    return binary_emit_movzx_reg_mem16(b, BINARY_GP_RAX, BINARY_GP_RDX, 0) &&
           binary_emit_mov_mem_reg16(b, BINARY_GP_RCX, 0, BINARY_GP_RAX);
  }
  if (size == 4) {
    return binary_emit_mov_reg_mem32(b, BINARY_GP_RAX, BINARY_GP_RDX, 0) &&
           binary_emit_mov_mem_reg32(b, BINARY_GP_RCX, 0, BINARY_GP_RAX);
  }
  return binary_emit_mov_reg_mem(b, BINARY_GP_RAX, BINARY_GP_RDX, 0) &&
         binary_emit_mov_mem_reg(b, BINARY_GP_RCX, 0, BINARY_GP_RAX);
}

static int copy_loop_back_to(BinaryCodeBuffer *b, size_t top) {
  size_t j_back = 0;

  return wcs_jcc(b, 0, &j_back) && wcs_patch_to(b, j_back, top);
}

static int copy_advance(BinaryCodeBuffer *b, BinaryGpRegister count,
                        unsigned char bytes, unsigned char decrement) {
  return wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, bytes) &&
         wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, bytes) &&
         wcs_addsub_reg_imm8(b, count, 1, decrement);
}

static int copy_emit_ymm_loop(BinaryCodeBuffer *b, size_t *j_tail) {
  size_t top = b->size;

  return wcs_cmp_reg_imm32(b, BINARY_GP_R9, 32) && wcs_jcc(b, 0x82, j_tail) &&
         wcs_avx_vmovdqu_ymm_mem(b, 0, BINARY_GP_RDX, 0) &&
         wcs_avx_vmovdqu_mem_ymm(b, BINARY_GP_RCX, 0, 0) &&
         copy_advance(b, BINARY_GP_R9, 32, 32) && copy_loop_back_to(b, top);
}

static int copy_emit_byte_tail(BinaryCodeBuffer *b, size_t *j_tail_done) {
  size_t top = b->size;

  return binary_emit_test_reg_reg(b, BINARY_GP_R9) &&
         wcs_jcc(b, 0x8E, j_tail_done) &&
         binary_emit_movzx_reg_mem8(b, BINARY_GP_RAX, BINARY_GP_RDX, 0) &&
         binary_emit_mov_mem_reg8(b, BINARY_GP_RCX, 0, BINARY_GP_RAX) &&
         copy_advance(b, BINARY_GP_R9, 1, 1) && copy_loop_back_to(b, top);
}

static int copy_emit_element_loop(BinaryCodeBuffer *b, long long size,
                                  size_t *j_scalar_done) {
  size_t top = b->size;

  return binary_emit_test_reg_reg(b, BINARY_GP_R8) &&
         wcs_jcc(b, 0x8E, j_scalar_done) && copy_move_element(b, size) &&
         copy_advance(b, BINARY_GP_R8, (unsigned char)size, 1) &&
         copy_loop_back_to(b, top);
}

static int copy_emit_wide_path(BinaryCodeBuffer *b, size_t *j_vec_join) {
  size_t j_tail = 0;
  size_t j_tail_done = 0;

  return copy_emit_ymm_loop(b, &j_tail) && wcs_patch_here(b, j_tail) &&
         wcs_avx_vzeroupper(b) && copy_emit_byte_tail(b, &j_tail_done) &&
         wcs_patch_here(b, j_tail_done) && wcs_jcc(b, 0, j_vec_join);
}

static int copy_emit_length_guard(BinaryCodeBuffer *b, unsigned char shift,
                                  size_t *j_empty) {
  if (!binary_emit_test_reg_reg(b, BINARY_GP_R8) ||
      !wcs_jcc(b, 0x8E, j_empty) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_R9, BINARY_GP_R8)) {
    return 0;
  }
  return !shift || binary_emit_shift_reg_imm8(b, 4, BINARY_GP_R9, shift);
}

static int copy_emit_overlap_guard(BinaryCodeBuffer *b, size_t *j_scalar) {
  return binary_emit_mov_reg_reg(b, BINARY_GP_RAX, BINARY_GP_RCX) &&
         binary_emit_alu_reg_reg(b, 0x29, BINARY_GP_RAX, BINARY_GP_RDX) &&
         binary_emit_cmp_reg_reg(b, BINARY_GP_RAX, BINARY_GP_R9) &&
         wcs_jcc(b, 0x82, j_scalar);
}

static int copy_load_arguments(CodeGenerator *generator,
                               BinaryFunctionContext *context,
                               const IRInstruction *instruction) {
  return code_generator_binary_emit_operand_load(generator, context,
                                                 &instruction->dest,
                                                 BINARY_GP_RCX) &&
         code_generator_binary_emit_operand_load(generator, context,
                                                 &instruction->lhs,
                                                 BINARY_GP_RDX) &&
         code_generator_binary_emit_operand_load(generator, context,
                                                 &instruction->rhs,
                                                 BINARY_GP_R8);
}

static unsigned char copy_element_shift(long long size) {
  return size == 1 ? 0 : size == 2 ? 1 : size == 4 ? 2 : 3;
}

int code_generator_binary_emit_simd_copy(CodeGenerator *generator,
                                         BinaryFunctionContext *context,
                                         const IRInstruction *instruction) {
  BinaryCodeBuffer *b = NULL;
  long long size = 0;
  size_t j_empty = 0;
  size_t j_scalar = 0;
  size_t j_scalar_done = 0;
  size_t j_vec_join = 0;

  if (!generator || !context || !instruction ||
      instruction->argument_count < 1 ||
      instruction->arguments[0].kind != IR_OPERAND_INT) {
    return 0;
  }
  size = instruction->arguments[0].int_value;
  if (size != 1 && size != 2 && size != 4 && size != 8) {
    return 0;
  }
  b = &context->code;
  if (!copy_load_arguments(generator, context, instruction) ||
      !copy_emit_length_guard(b, copy_element_shift(size), &j_empty) ||
      !copy_emit_overlap_guard(b, &j_scalar) ||
      !copy_emit_wide_path(b, &j_vec_join) ||
      !wcs_patch_here(b, j_scalar) ||
      !copy_emit_element_loop(b, size, &j_scalar_done)) {
    return 0;
  }
  return wcs_patch_here(b, j_scalar_done) && wcs_patch_here(b, j_vec_join) &&
         wcs_patch_here(b, j_empty);
}

int code_generator_binary_emit_simd_reverse_copy_i32(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  BinaryCodeBuffer *b = NULL;
  size_t loop_top = 0;
  size_t j_done = 0;
  size_t j_vec = 0;
  size_t j_scalar = 0;
  size_t j_short = 0;

  if (!generator || !context || !instruction ||
      instruction->argument_count < 1) {
    return 0;
  }
  b = &context->code;

  if (!code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->dest,
                                               BINARY_GP_RAX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->lhs,
                                               BINARY_GP_RCX) ||
      !binary_emit_mov_reg_reg(&context->code, BINARY_GP_R10, BINARY_GP_RCX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->rhs,
                                               BINARY_GP_RDX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->arguments[0],
                                               BINARY_GP_R11) ||
      !wcs_mov_reg_reg32(b, BINARY_GP_R9, BINARY_GP_R11) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_R9, 1, 1) ||
      !wcs_shift_reg_imm(b, BINARY_GP_R9, 0, 2) ||
      !wcs_add_reg_reg64(b, BINARY_GP_RCX, BINARY_GP_R9) ||
      !wcs_mov_reg_reg32(b, BINARY_GP_R8, BINARY_GP_R11) ||
      !wcs_shift_reg_imm(b, BINARY_GP_R8, 0, 2) ||
      !wcs_add_reg_reg64(b, BINARY_GP_R8, BINARY_GP_RDX)) {
    return 0;
  }
  if (!wcs_avx_vpxor_ymm(b, 2, 2, 2)) {
    return 0;
  }

  loop_top = b->size;
  if (!binary_emit_cmp_reg_reg(b, BINARY_GP_RDX, BINARY_GP_R8) ||
      !wcs_jcc(b, 0x83 , &j_done)) {
    return 0;
  }

  if (!binary_emit_mov_reg_reg(b, BINARY_GP_R9, BINARY_GP_R8) ||
      !binary_emit_alu_reg_reg(b, 0x29, BINARY_GP_R9, BINARY_GP_RDX) ||
      !wcs_cmp_reg_imm32(b, BINARY_GP_R9, 64) ||
      !wcs_jcc(b, 0x82 , &j_short) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_R9, BINARY_GP_RCX) ||
      !binary_emit_alu_reg_reg(b, 0x29, BINARY_GP_R9, BINARY_GP_R10) ||
      !wcs_cmp_reg_imm32(b, BINARY_GP_R9, 60) ||
      !wcs_jcc(b, 0x82 , &j_scalar) ||
      !wcs_jcc(b, 0, &j_vec)) {
    return 0;
  }

  if (!wcs_patch_here(b, j_vec) ||
      !wcs_avx_vmovdqu_ymm_mem(b, 0, BINARY_GP_RCX, -28) ||
      !wcs_avx_vpshufd_ymm(b, 0, 0, 0x1B) ||
      !wcs_avx_vperm2i128(b, 0, 0, 0, 0x01) ||
      !wcs_avx_vmovdqu_mem_ymm(b, BINARY_GP_RDX, 0, 0) ||
      !wcs_avx_vpaddd_ymm(b, 2, 2, 0) ||
      !wcs_avx_vmovdqu_ymm_mem(b, 0, BINARY_GP_RCX, -60) ||
      !wcs_avx_vpshufd_ymm(b, 0, 0, 0x1B) ||
      !wcs_avx_vperm2i128(b, 0, 0, 0, 0x01) ||
      !wcs_avx_vmovdqu_mem_ymm(b, BINARY_GP_RDX, 32, 0) ||
      !wcs_avx_vpaddd_ymm(b, 2, 2, 0) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 1, 64) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 64)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, loop_top)) {
      return 0;
    }
  }

  if (!wcs_patch_here(b, j_scalar) || !wcs_patch_here(b, j_short) ||
      !binary_emit_mov_reg_mem32(b, BINARY_GP_R9, BINARY_GP_RCX, 0) ||
      !binary_emit_mov_mem_reg32(b, BINARY_GP_RDX, 0, BINARY_GP_R9) ||
      !binary_emit_movsxd_reg_reg32(b, BINARY_GP_R9, BINARY_GP_R9) ||
      !wcs_add_reg_reg64(b, BINARY_GP_RAX, BINARY_GP_R9) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 1, 4) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 4)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, loop_top)) {
      return 0;
    }
  }

  if (!wcs_patch_here(b, j_done) ||
      !wcs_reduce_ymm_i32_sum_to_rax(b, 2)) {
    return 0;
  }
  return code_generator_binary_emit_destination_store(generator, context,
                                                      &instruction->dest,
                                                      BINARY_GP_RAX);
}

int code_generator_binary_emit_simd_clamp_i32(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  BinaryCodeBuffer *b = NULL;
  size_t loop_top = 0;
  size_t j_done = 0;
  size_t j_vec = 0;
  size_t j_scalar = 0;
  int32_t lo = 0;
  int32_t hi = 0;

  if (!generator || !context || !instruction ||
      instruction->argument_count < 3) {
    return 0;
  }
  b = &context->code;
  lo = (int32_t)instruction->arguments[1].int_value;
  hi = (int32_t)instruction->arguments[2].int_value;
  if (lo > hi) {
    return 0;
  }

  if (!code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->dest,
                                               BINARY_GP_RAX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->lhs,
                                               BINARY_GP_RCX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->rhs,
                                               BINARY_GP_RDX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->arguments[0],
                                               BINARY_GP_R11) ||
      !wcs_mov_reg_reg32(b, BINARY_GP_R8, BINARY_GP_R11) ||
      !wcs_shift_reg_imm(b, BINARY_GP_R8, 0, 2) ||
      !wcs_add_reg_reg64(b, BINARY_GP_R8, BINARY_GP_RCX) ||
      !wcs_mov_reg_imm32(b, BINARY_GP_R9, (uint32_t)lo) ||
      !wcs_broadcast_i32_to_ymm(b, 4, BINARY_GP_R9) ||
      !wcs_mov_reg_imm32(b, BINARY_GP_R9, (uint32_t)hi) ||
      !wcs_broadcast_i32_to_ymm(b, 5, BINARY_GP_R9) ||
      !wcs_avx_vpxor_ymm(b, 2, 2, 2)) {
    return 0;
  }

  loop_top = b->size;
  if (!binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R8) ||
      !wcs_jcc(b, 0x83 , &j_done)) {
    return 0;
  }

  if (!binary_emit_mov_reg_reg(b, BINARY_GP_R9, BINARY_GP_R8) ||
      !binary_emit_alu_reg_reg(b, 0x29, BINARY_GP_R9, BINARY_GP_RCX) ||
      !wcs_cmp_reg_imm32(b, BINARY_GP_R9, 32) ||
      !wcs_jcc(b, 0x83 , &j_vec) ||
      !wcs_jcc(b, 0, &j_scalar)) {
    return 0;
  }

  if (!wcs_patch_here(b, j_vec) ||
      !wcs_avx_vmovdqu_ymm_mem(b, 0, BINARY_GP_RCX, 0) ||
      !wcs_avx_vpmaxsd_ymm(b, 0, 0, 4) ||
      !wcs_avx_vpminsd_ymm(b, 0, 0, 5) ||
      !wcs_avx_vmovdqu_mem_ymm(b, BINARY_GP_RDX, 0, 0) ||
      !wcs_avx_vpaddd_ymm(b, 2, 2, 0) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 32) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 32)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, loop_top)) {
      return 0;
    }
  }

  if (!wcs_patch_here(b, j_scalar) ||
      !binary_emit_mov_reg_mem32(b, BINARY_GP_R10, BINARY_GP_RCX, 0)) {
    return 0;
  }
  {
    size_t j_not_lo = 0;
    size_t j_not_hi = 0;
    size_t j_clamp_done = 0;
    if (!wcs_cmp_reg_imm32(b, BINARY_GP_R10, lo) ||
        !wcs_jcc(b, 0x8D , &j_not_lo) ||
        !wcs_mov_reg_imm32(b, BINARY_GP_R10, (uint32_t)lo) ||
        !wcs_jcc(b, 0, &j_clamp_done)) {
      return 0;
    }
    if (!wcs_patch_here(b, j_not_lo) ||
        !wcs_cmp_reg_imm32(b, BINARY_GP_R10, hi) ||
        !wcs_jcc(b, 0x8E , &j_not_hi) ||
        !wcs_mov_reg_imm32(b, BINARY_GP_R10, (uint32_t)hi)) {
      return 0;
    }
    if (!wcs_patch_here(b, j_not_hi) ||
        !wcs_patch_here(b, j_clamp_done) ||
        !binary_emit_mov_mem_reg32(b, BINARY_GP_RDX, 0, BINARY_GP_R10) ||
        !binary_emit_movsxd_reg_reg32(b, BINARY_GP_R10, BINARY_GP_R10) ||
        !wcs_add_reg_reg64(b, BINARY_GP_RAX, BINARY_GP_R10) ||
        !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 4) ||
        !wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 4)) {
      return 0;
    }
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, loop_top)) {
      return 0;
    }
  }

  if (!wcs_patch_here(b, j_done) ||
      !wcs_reduce_ymm_i32_sum_to_rax(b, 2)) {
    return 0;
  }
  return code_generator_binary_emit_destination_store(generator, context,
                                                      &instruction->dest,
                                                      BINARY_GP_RAX);
}

static int simd_dot_i32_prologue(CodeGenerator *generator,
                                 BinaryFunctionContext *context,
                                 const IRInstruction *instruction) {
  BinaryCodeBuffer *b = &context->code;
  return code_generator_binary_emit_operand_load(generator, context,
                                                 &instruction->lhs,
                                                 BINARY_GP_RCX) &&
         code_generator_binary_emit_operand_load(generator, context,
                                                 &instruction->rhs,
                                                 BINARY_GP_RDX) &&
         code_generator_binary_emit_operand_load(generator, context,
                                                 &instruction->arguments[0],
                                                 BINARY_GP_R8) &&
         binary_emit_mov_reg_imm64(b, BINARY_GP_RAX, 0) &&
         wcs_avx_vpxor_ymm(b, 2, 2, 2) && wcs_avx_vpxor_ymm(b, 5, 5, 5) &&
         binary_emit_mov_reg_reg(b, BINARY_GP_R11, BINARY_GP_R8) &&
         binary_emit_shift_reg_imm8(b, 4, BINARY_GP_R11, 2) &&
         wcs_add_reg_reg64(b, BINARY_GP_R11, BINARY_GP_RCX);
}

static int simd_dot_i32_vector_step(BinaryCodeBuffer *b, int displacement) {
  return wcs_avx_vmovdqu_ymm_mem(b, 0, BINARY_GP_RCX, displacement) &&
         wcs_avx_vmovdqu_ymm_mem(b, 1, BINARY_GP_RDX, displacement) &&
         wcs_avx_vpmuldq_ymm(b, 3, 0, 1) &&
         wcs_avx_vpsrlq_ymm_imm(b, 0, 0, 32) &&
         wcs_avx_vpsrlq_ymm_imm(b, 1, 1, 32) &&
         wcs_avx_vpmuldq_ymm(b, 4, 0, 1) && wcs_avx_vpaddq_ymm(b, 2, 2, 3) &&
         wcs_avx_vpaddq_ymm(b, 5, 5, 4);
}

static int simd_dot_i32_vector_body(BinaryCodeBuffer *b) {
  int displacement = 0;
  for (displacement = 0; displacement < 128; displacement += 32) {
    if (!simd_dot_i32_vector_step(b, displacement)) {
      return 0;
    }
  }
  return wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 64) &&
         wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 64) &&
         wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 64) &&
         wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 64);
}

static int simd_dot_i32_scalar_step(BinaryCodeBuffer *b) {
  return binary_emit_mov_reg_mem32(b, BINARY_GP_R10, BINARY_GP_RCX, 0) &&
         binary_emit_movsxd_reg_reg32(b, BINARY_GP_R10, BINARY_GP_R10) &&
         binary_emit_mov_reg_mem32(b, BINARY_GP_R9, BINARY_GP_RDX, 0) &&
         binary_emit_movsxd_reg_reg32(b, BINARY_GP_R9, BINARY_GP_R9) &&
         binary_emit_imul_reg_reg(b, BINARY_GP_R10, BINARY_GP_R9) &&
         wcs_add_reg_reg64(b, BINARY_GP_RAX, BINARY_GP_R10) &&
         wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 4) &&
         wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 4);
}

static int simd_dot_i32_reduce(BinaryCodeBuffer *b) {
  return wcs_avx_vpaddq_ymm(b, 2, 2, 5) && wcs_avx_vextracti128(b, 3, 2, 1) &&
         wcs_avx_vzeroupper(b) && wcs_paddq(b, 2, 3) &&
         binary_emit_movq_reg_xmm(b, BINARY_GP_R10, BINARY_XMM2) &&
         wcs_add_reg_reg64(b, BINARY_GP_RAX, BINARY_GP_R10) &&
         wcs_pshufd(b, 3, 2, 0xEE) &&
         binary_emit_movq_reg_xmm(b, BINARY_GP_R10, BINARY_XMM3) &&
         wcs_add_reg_reg64(b, BINARY_GP_RAX, BINARY_GP_R10);
}

static int simd_loop_back_to(BinaryCodeBuffer *b, size_t target) {
  size_t j_back = 0;
  return wcs_jcc(b, 0, &j_back) && wcs_patch_to(b, j_back, target);
}

int code_generator_binary_emit_simd_dot_i32(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  BinaryCodeBuffer *b = NULL;
  size_t loop_top = 0;
  size_t j_done = 0;
  size_t j_vec = 0;
  size_t j_scalar = 0;

  if (!generator || !context || !instruction ||
      instruction->argument_count != 1 || !instruction->arguments) {
    code_generator_set_error(generator, "Malformed simd_dot_i32");
    return 0;
  }
  b = &context->code;

  if (!simd_dot_i32_prologue(generator, context, instruction)) {
    return 0;
  }

  loop_top = b->size;
  if (!binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R11) ||
      !wcs_jcc(b, 0x83, &j_done)) {
    return 0;
  }
  if (!binary_emit_mov_reg_reg(b, BINARY_GP_R9, BINARY_GP_R11) ||
      !binary_emit_alu_reg_reg(b, 0x29, BINARY_GP_R9, BINARY_GP_RCX) ||
      !wcs_cmp_reg_imm32(b, BINARY_GP_R9, 128) ||
      !wcs_jcc(b, 0x83, &j_vec) || !wcs_jcc(b, 0, &j_scalar)) {
    return 0;
  }

  if (!wcs_patch_here(b, j_vec) || !simd_dot_i32_vector_body(b) ||
      !simd_loop_back_to(b, loop_top)) {
    return 0;
  }
  if (!wcs_patch_here(b, j_scalar) || !simd_dot_i32_scalar_step(b) ||
      !simd_loop_back_to(b, loop_top)) {
    return 0;
  }
  if (!wcs_patch_here(b, j_done) || !simd_dot_i32_reduce(b)) {
    return 0;
  }
  return code_generator_binary_emit_destination_store(generator, context,
                                                      &instruction->dest,
                                                      BINARY_GP_RAX);
}

typedef int (*BinaryByteWiden)(BinaryCodeBuffer *, int, int, int);
typedef int (*BinaryByteLoad)(BinaryCodeBuffer *, BinaryGpRegister,
                              BinaryGpRegister, int);

static int binary_emit_dot_i8_body(BinaryCodeBuffer *b, BinaryByteWiden widen) {
  size_t loop_top = b->size;
  size_t after_main = 0;
  size_t j_back = 0;
  if (!binary_emit_mov_reg_reg(b, BINARY_GP_R9, BINARY_GP_R11) ||
      !binary_emit_alu_reg_reg(b, 0x29, BINARY_GP_R9, BINARY_GP_RCX) ||
      !wcs_cmp_reg_imm32(b, BINARY_GP_R9, 32) ||
      !wcs_jcc(b, 0x82 , &after_main) ||
      !widen(b, 0, BINARY_GP_RCX, 0) || !widen(b, 1, BINARY_GP_RDX, 0) ||
      !wcs_avx_vpmaddwd_ymm(b, 0, 0, 1) || !wcs_avx_vpaddd_ymm(b, 2, 2, 0) ||
      !widen(b, 0, BINARY_GP_RCX, 16) || !widen(b, 1, BINARY_GP_RDX, 16) ||
      !wcs_avx_vpmaddwd_ymm(b, 0, 0, 1) || !wcs_avx_vpaddd_ymm(b, 3, 3, 0) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 32) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 32) ||
      !wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, loop_top)) {
    return 0;
  }
  return wcs_patch_here(b, after_main) && wcs_avx_vpaddd_ymm(b, 2, 2, 3) &&
         wcs_avx_vextracti128(b, 3, 2, 1) && wcs_avx_vzeroupper(b) &&
         wcs_paddd(b, 2, 3) && wcs_pshufd(b, 3, 2, 0xEE) &&
         wcs_paddd(b, 2, 3) && wcs_pshufd(b, 3, 2, 0x55) &&
         wcs_paddd(b, 2, 3) &&
         binary_emit_movd_reg_xmm(b, BINARY_GP_RAX, BINARY_XMM2) &&
         binary_emit_movsxd_rax_eax(b);
}

static int binary_emit_dot_i8_tail(BinaryCodeBuffer *b,
                                   BinaryByteLoad tail_load) {
  size_t tail_top = b->size;
  size_t j_done = 0;
  size_t j_back = 0;
  if (!binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R11) ||
      !wcs_jcc(b, 0x83 , &j_done) ||
      !tail_load(b, BINARY_GP_R10, BINARY_GP_RCX, 0) ||
      !tail_load(b, BINARY_GP_R9, BINARY_GP_RDX, 0) ||
      !binary_emit_imul_reg_reg(b, BINARY_GP_R10, BINARY_GP_R9) ||
      !wcs_add_reg_reg64(b, BINARY_GP_RAX, BINARY_GP_R10) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 1) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 1) ||
      !wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, tail_top)) {
    return 0;
  }
  return wcs_patch_here(b, j_done);
}

int code_generator_binary_emit_simd_dot_i8(CodeGenerator *generator,
                                           BinaryFunctionContext *context,
                                           const IRInstruction *instruction) {
  BinaryCodeBuffer *b = NULL;
  BinaryByteWiden widen = NULL;
  BinaryByteLoad tail_load = NULL;

  if (!generator || !context || !instruction ||
      instruction->argument_count != 1 || !instruction->arguments) {
    code_generator_set_error(generator, "Malformed simd_dot_i8");
    return 0;
  }
  widen = instruction->is_unsigned ? wcs_avx_vpmovzxbw_ymm_mem
                                   : wcs_avx_vpmovsxbw_ymm_mem;
  tail_load = instruction->is_unsigned ? binary_emit_movzx_reg_mem8
                                       : binary_emit_movsx_reg_mem8;
  b = &context->code;

  if (!code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->lhs,
                                               BINARY_GP_RCX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->rhs,
                                               BINARY_GP_RDX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->arguments[0],
                                               BINARY_GP_R8) ||
      !wcs_avx_vpxor_ymm(b, 2, 2, 2) || !wcs_avx_vpxor_ymm(b, 3, 3, 3) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_R11, BINARY_GP_RCX) ||
      !wcs_add_reg_reg64(b, BINARY_GP_R11, BINARY_GP_R8)) {
    return 0;
  }

  if (!binary_emit_dot_i8_body(b, widen) ||
      !binary_emit_dot_i8_tail(b, tail_load)) {
    return 0;
  }
  return code_generator_binary_emit_destination_store(generator, context,
                                                      &instruction->dest,
                                                      BINARY_GP_RAX);
}

int code_generator_binary_emit_vzeroupper(BinaryCodeBuffer *b) {
  return wcs_avx_vzeroupper(b);
}

static int slp_mac_step(BinaryCodeBuffer *b, int wide, int a_disp, int acc_ymm) {
  return wcs_avx_vpbroadcastd_ymm_mem(b, 0, BINARY_GP_RCX, a_disp) &&
         (wide ? wcs_avx_vmovdqu_ymm_mem(b, 1, BINARY_GP_RDX, 0)
               : wcs_avx_vmovdqu_xmm_mem(b, 1, BINARY_GP_RDX, 0)) &&
         wcs_avx_vpmulld_ymm(b, 1, 0, 1) &&
         wcs_avx_vpaddd_ymm(b, acc_ymm, acc_ymm, 1) &&
         wcs_add_reg_reg64(b, BINARY_GP_RDX, BINARY_GP_RAX);
}

int code_generator_binary_emit_simd_slp_mac_i32_loop(BinaryCodeBuffer *b,
                                                      long long K) {
  size_t main_top = 0, after_main = 0, tail_top = 0, j_done = 0;
  int wide = (K == 8);
  if ((K != 4 && K != 8) || !b) {
    return 0;
  }
  if (!wcs_avx_vpxor_ymm(b, 2, 2, 2) || !wcs_avx_vpxor_ymm(b, 3, 3, 3) ||
      !wcs_avx_vpxor_ymm(b, 4, 4, 4) || !wcs_avx_vpxor_ymm(b, 5, 5, 5)) {
    return 0;
  }
  main_top = b->size;
  if (!binary_emit_cmp_reg_imm32(b, BINARY_GP_R9, 4) ||
      !wcs_jcc(b, 0x8C , &after_main) ||
      !slp_mac_step(b, wide, 0, 2) || !slp_mac_step(b, wide, 4, 3) ||
      !slp_mac_step(b, wide, 8, 4) || !slp_mac_step(b, wide, 12, 5) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 16) ||
      !binary_emit_sub_reg_imm32(b, BINARY_GP_R9, 4)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, main_top)) {
      return 0;
    }
  }
  if (!wcs_patch_here(b, after_main) || !wcs_avx_vpaddd_ymm(b, 2, 2, 3) ||
      !wcs_avx_vpaddd_ymm(b, 4, 4, 5) || !wcs_avx_vpaddd_ymm(b, 2, 2, 4)) {
    return 0;
  }
  tail_top = b->size;
  if (!binary_emit_cmp_reg_imm32(b, BINARY_GP_R9, 0) ||
      !wcs_jcc(b, 0x84 , &j_done) || !slp_mac_step(b, wide, 0, 2) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 4) ||
      !binary_emit_sub_reg_imm32(b, BINARY_GP_R9, 1)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, tail_top)) {
      return 0;
    }
  }
  if (!wcs_patch_here(b, j_done)) {
    return 0;
  }
  if (wide ? !wcs_avx_vmovdqu_mem_ymm(b, BINARY_GP_R8, 0, 2)
           : !wcs_avx_vmovdqu_mem_xmm(b, BINARY_GP_R8, 0, 2)) {
    return 0;
  }
  return 1;
}

static int slp_mac_i8_step(BinaryCodeBuffer *b, int wide, int a_disp,
                           int acc_ymm) {
  return binary_emit_movzx_reg_mem8(b, BINARY_GP_R10, BINARY_GP_RCX, a_disp) &&
         wcs_avx_vmovd_xmm_reg(b, 0, BINARY_GP_R10) &&
         wcs_avx_vpbroadcastd_ymm(b, 0, 0) &&
         (wide ? wcs_avx_vpmovzxbd_ymm_mem(b, 1, BINARY_GP_RDX, 0)
               : wcs_avx_vpmovzxbd_xmm_mem(b, 1, BINARY_GP_RDX, 0)) &&
         wcs_avx_vpmulld_ymm(b, 1, 0, 1) &&
         wcs_avx_vpaddd_ymm(b, acc_ymm, acc_ymm, 1) &&
         wcs_add_reg_reg64(b, BINARY_GP_RDX, BINARY_GP_RAX);
}

int code_generator_binary_emit_simd_slp_mac_i8_loop(BinaryCodeBuffer *b,
                                                     long long K) {
  size_t main_top = 0, after_main = 0, tail_top = 0, j_done = 0;
  int wide = (K == 8);
  if ((K != 4 && K != 8) || !b) {
    return 0;
  }
  if (!wcs_avx_vpxor_ymm(b, 2, 2, 2) || !wcs_avx_vpxor_ymm(b, 3, 3, 3) ||
      !wcs_avx_vpxor_ymm(b, 4, 4, 4) || !wcs_avx_vpxor_ymm(b, 5, 5, 5)) {
    return 0;
  }
  main_top = b->size;
  if (!binary_emit_cmp_reg_imm32(b, BINARY_GP_R9, 4) ||
      !wcs_jcc(b, 0x8C , &after_main)) {
    return 0;
  }
  if (!wide) {
    static const unsigned char splat[4] = {0x00, 0x55, 0xAA, 0xFF};
    const int acc[4] = {2, 3, 4, 5};
    if (!wcs_avx_vpmovzxbd_xmm_mem(b, 6, BINARY_GP_RCX, 0)) {
      return 0;
    }
    for (int j = 0; j < 4; j++) {
      if (!wcs_avx_vpshufd_ymm(b, 0, 6, splat[j]) ||
          !wcs_avx_vpmovzxbd_xmm_mem(b, 1, BINARY_GP_RDX, 0) ||
          !wcs_avx_vpmulld_ymm(b, 1, 0, 1) ||
          !wcs_avx_vpaddd_ymm(b, acc[j], acc[j], 1) ||
          !wcs_add_reg_reg64(b, BINARY_GP_RDX, BINARY_GP_RAX)) {
        return 0;
      }
    }
    if (!wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 4) ||
        !binary_emit_sub_reg_imm32(b, BINARY_GP_R9, 4)) {
      return 0;
    }
  } else if (!slp_mac_i8_step(b, wide, 0, 2) ||
             !slp_mac_i8_step(b, wide, 1, 3) ||
             !slp_mac_i8_step(b, wide, 2, 4) ||
             !slp_mac_i8_step(b, wide, 3, 5) ||
             !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 4) ||
             !binary_emit_sub_reg_imm32(b, BINARY_GP_R9, 4)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, main_top)) {
      return 0;
    }
  }
  if (!wcs_patch_here(b, after_main) || !wcs_avx_vpaddd_ymm(b, 2, 2, 3) ||
      !wcs_avx_vpaddd_ymm(b, 4, 4, 5) || !wcs_avx_vpaddd_ymm(b, 2, 2, 4)) {
    return 0;
  }
  tail_top = b->size;
  if (!binary_emit_cmp_reg_imm32(b, BINARY_GP_R9, 0) ||
      !wcs_jcc(b, 0x84 , &j_done) || !slp_mac_i8_step(b, wide, 0, 2) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 1) ||
      !binary_emit_sub_reg_imm32(b, BINARY_GP_R9, 1)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, tail_top)) {
      return 0;
    }
  }
  if (!wcs_patch_here(b, j_done)) {
    return 0;
  }
  if (wide ? !wcs_avx_vmovdqu_mem_ymm(b, BINARY_GP_R8, 0, 2)
           : !wcs_avx_vmovdqu_mem_xmm(b, BINARY_GP_R8, 0, 2)) {
    return 0;
  }
  return 1;
}

int code_generator_binary_emit_prefix_sum_i32(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  BinaryCodeBuffer *b = NULL;
  size_t loop_top = 0;
  size_t j_done = 0;

  if (!generator || !context || !instruction ||
      instruction->argument_count < 1) {
    return 0;
  }
  b = &context->code;

  if (!code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->dest,
                                               BINARY_GP_R8) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->lhs,
                                               BINARY_GP_RCX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->rhs,
                                               BINARY_GP_RDX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->arguments[0],
                                               BINARY_GP_R11) ||
      !wcs_mov_reg_reg32(b, BINARY_GP_R9, BINARY_GP_R11) ||
      !wcs_shift_reg_imm(b, BINARY_GP_R9, 0, 2) ||
      !wcs_add_reg_reg64(b, BINARY_GP_R9, BINARY_GP_RCX)) {
    return 0;
  }

  {
    size_t j_novec = 0;
    size_t j_too_close = 0;
    size_t j_dist_ok = 0;
    size_t vec_top = 0;
    if (!binary_emit_mov_reg_reg(b, BINARY_GP_R10, BINARY_GP_RDX) ||
        !binary_emit_alu_reg_reg(b, 0x29, BINARY_GP_R10, BINARY_GP_RCX) ||
        !binary_emit_test_reg_reg(b, BINARY_GP_R10) ||
        !wcs_jcc(b, 0x84 , &j_dist_ok) ||
        !binary_emit_cmp_reg_imm32(b, BINARY_GP_R10, 16) ||
        !wcs_jcc(b, 0x82 , &j_too_close)) {
      return 0;
    }
    if (!wcs_patch_here(b, j_dist_ok) ||
        !binary_emit_mov_reg_reg(b, BINARY_GP_R10, BINARY_GP_R9) ||
        !binary_emit_alu_reg_reg(b, 0x29, BINARY_GP_R10, BINARY_GP_RCX) ||
        !wcs_shift_reg_imm(b, BINARY_GP_R10, 1 , 4) ||
        !wcs_shift_reg_imm(b, BINARY_GP_R10, 0 , 4) ||
        !wcs_add_reg_reg64(b, BINARY_GP_R10, BINARY_GP_RCX)) {
      return 0;
    }
    if (!binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R10) ||
        !wcs_jcc(b, 0x83 , &j_novec)) {
      return 0;
    }
    if (!wcs_avx_vpxor_ymm(b, 5, 5, 5) ||
        !wcs_broadcast_i32_to_ymm(b, 2, BINARY_GP_R8)) {
      return 0;
    }
    vec_top = b->size;
    if (!wcs_avx_vmovdqu_xmm_mem(b, 0, BINARY_GP_RCX, 0) ||
        !wcs_avx_vpmovsxdq_ymm_mem(b, 4, BINARY_GP_RCX, 0) ||
        !wcs_avx_vpaddq_ymm(b, 5, 5, 4) ||
        !wcs_avx_vpslldq_xmm(b, 1, 0, 4) ||
        !wcs_avx_vpaddd_xmm(b, 0, 0, 1) ||
        !wcs_avx_vpslldq_xmm(b, 1, 0, 8) ||
        !wcs_avx_vpaddd_xmm(b, 0, 0, 1) ||
        !wcs_avx_vpaddd_xmm(b, 0, 0, 2) ||
        !wcs_avx_vmovdqu_mem_xmm(b, BINARY_GP_RDX, 0, 0) ||
        !wcs_avx_vpshufd_xmm(b, 2, 0, 0xFF) ||
        !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 16) ||
        !wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 16) ||
        !binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R10)) {
      return 0;
    }
    {
      size_t j_back = 0;
      if (!wcs_jcc(b, 0x82 , &j_back) ||
          !wcs_patch_to(b, j_back, vec_top)) {
        return 0;
      }
    }
    if (!wcs_avx_vextracti128(b, 4, 5, 1) ||
        !wcs_avx_vpaddq_xmm(b, 5, 5, 4) ||
        !binary_emit_movq_reg_xmm(b, BINARY_GP_R10, BINARY_XMM5) ||
        !wcs_avx_vpshufd_xmm(b, 5, 5, 0x4E) ||
        !binary_emit_movq_reg_xmm(b, BINARY_GP_R11, BINARY_XMM5) ||
        !wcs_add_reg_reg64(b, BINARY_GP_R8, BINARY_GP_R10) ||
        !wcs_add_reg_reg64(b, BINARY_GP_R8, BINARY_GP_R11) ||
        !wcs_avx_vzeroupper(b)) {
      return 0;
    }
    if (!wcs_patch_here(b, j_novec) || !wcs_patch_here(b, j_too_close)) {
      return 0;
    }
  }

  loop_top = b->size;
  if (!binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R9) ||
      !wcs_jcc(b, 0x83 , &j_done)) {
    return 0;
  }

  if (!binary_emit_mov_reg_mem32(b, BINARY_GP_R10, BINARY_GP_RCX, 0) ||
      !binary_emit_movsxd_reg_reg32(b, BINARY_GP_R10, BINARY_GP_R10) ||
      !wcs_add_reg_reg64(b, BINARY_GP_R8, BINARY_GP_R10) ||
      !binary_emit_mov_mem_reg32(b, BINARY_GP_RDX, 0, BINARY_GP_R8) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 4) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 4)) {
    return 0;
  }

  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, loop_top)) {
      return 0;
    }
  }

  if (!wcs_patch_here(b, j_done) ||
      !code_generator_binary_emit_destination_store(generator, context,
                                                    &instruction->dest,
                                                    BINARY_GP_R8)) {
    return 0;
  }
  return 1;
}

int code_generator_binary_emit_simd_minmax_i32(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  BinaryCodeBuffer *b = NULL;
  size_t loop_top = 0;
  size_t j_done = 0;
  size_t j_vec = 0;
  size_t j_scalar = 0;

  if (!generator || !context || !instruction ||
      instruction->argument_count < 1) {
    return 0;
  }
  b = &context->code;

  if (!code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->dest,
                                               BINARY_GP_R10) ||
      !code_generator_binary_emit_operand_load(
          generator, context, &instruction->arguments[0], BINARY_GP_R11) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->lhs,
                                               BINARY_GP_RCX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->rhs,
                                               BINARY_GP_RDX) ||
      !wcs_mov_reg_reg32(b, BINARY_GP_R8, BINARY_GP_RDX) ||
      !wcs_shift_reg_imm(b, BINARY_GP_R8, 0, 2) ||
      !wcs_add_reg_reg64(b, BINARY_GP_R8, BINARY_GP_RCX) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 4) ||
      !wcs_broadcast_i32_to_ymm(b, 4, BINARY_GP_R10) ||
      !wcs_broadcast_i32_to_ymm(b, 5, BINARY_GP_R11) ||
      !binary_emit_movsxd_reg_reg32(b, BINARY_GP_R10, BINARY_GP_R10) ||
      !binary_emit_movsxd_reg_reg32(b, BINARY_GP_R11, BINARY_GP_R11)) {
    return 0;
  }

  loop_top = b->size;
  if (!binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R8) ||
      !wcs_jcc(b, 0x83 , &j_done)) {
    return 0;
  }

  if (!binary_emit_mov_reg_reg(b, BINARY_GP_R9, BINARY_GP_R8) ||
      !binary_emit_alu_reg_reg(b, 0x29, BINARY_GP_R9, BINARY_GP_RCX) ||
      !wcs_cmp_reg_imm32(b, BINARY_GP_R9, 64) ||
      !wcs_jcc(b, 0x83 , &j_vec) ||
      !wcs_jcc(b, 0, &j_scalar)) {
    return 0;
  }

  if (!wcs_patch_here(b, j_vec) ||
      !wcs_avx_vmovdqu_ymm_mem(b, 0, BINARY_GP_RCX, 0) ||
      !wcs_avx_vpminsd_ymm(b, 4, 4, 0) ||
      !wcs_avx_vpmaxsd_ymm(b, 5, 5, 0) ||
      !wcs_avx_vmovdqu_ymm_mem(b, 0, BINARY_GP_RCX, 32) ||
      !wcs_avx_vpminsd_ymm(b, 4, 4, 0) ||
      !wcs_avx_vpmaxsd_ymm(b, 5, 5, 0) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 64)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, loop_top)) {
      return 0;
    }
  }

  if (!wcs_patch_here(b, j_scalar) ||
      !binary_emit_mov_reg_mem32(b, BINARY_GP_R14, BINARY_GP_RCX, 0) ||
      !binary_emit_movsxd_reg_reg32(b, BINARY_GP_R14, BINARY_GP_R14) ||
      !binary_emit_cmp_reg_reg(b, BINARY_GP_R14, BINARY_GP_R10) ||
      !binary_emit_cmovcc_reg_reg(b, 0x4C , BINARY_GP_R10,
                                  BINARY_GP_R14) ||
      !binary_emit_cmp_reg_reg(b, BINARY_GP_R14, BINARY_GP_R11) ||
      !binary_emit_cmovcc_reg_reg(b, 0x4F , BINARY_GP_R11,
                                  BINARY_GP_R14) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 4)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, loop_top)) {
      return 0;
    }
  }

  if (!wcs_patch_here(b, j_done) ||
      !wcs_avx_vextracti128(b, 0, 4, 1) ||
      !wcs_avx_vextracti128(b, 1, 5, 1) ||
      !wcs_avx_vzeroupper(b) ||
      !wcs_pminsd(b, 4, 0) ||
      !wcs_pmaxsd(b, 5, 1) ||
      !wcs_horizontal_pminsd_to_reg(b, 4, BINARY_GP_R10) ||
      !wcs_horizontal_pmaxsd_to_reg(b, 5, BINARY_GP_R11) ||
      !code_generator_binary_emit_destination_store(generator, context,
                                                    &instruction->dest,
                                                    BINARY_GP_R10) ||
      !code_generator_binary_emit_destination_store(
          generator, context, &instruction->arguments[0], BINARY_GP_R11)) {
    return 0;
  }
  return 1;
}

#define VFIND_EQ 0
#define VFIND_NE 1
#define VFIND_LT 2
#define VFIND_GT 3
#define VFIND_LE 4
#define VFIND_GE 5
#define VFIND_ASCII_IDENT_END 6

static unsigned char vfind_hit_cc(int pred) {
  switch (pred) {
  case VFIND_EQ: return 0x84;
  case VFIND_NE: return 0x85;
  case VFIND_LT: return 0x8C;
  case VFIND_GT: return 0x8F;
  case VFIND_LE: return 0x8E;
  default: return 0x8D;
  }
}

static int code_generator_binary_emit_simd_find_ascii_ident(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  BinaryCodeBuffer *b = &context->code;
  if (instruction->argument_count != 5 ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->lhs,
                                               BINARY_GP_R10) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->rhs,
                                               BINARY_GP_RDX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->arguments[4],
                                               BINARY_GP_R11) ||
      !wcs_add_reg_reg64(b, BINARY_GP_RDX, BINARY_GP_R11) ||
      !binary_emit_mov_reg_imm64(b, BINARY_GP_RAX,
                                 UINT64_C(0x7A615F5F5A413930)) ||
      !binary_emit_movq_xmm_reg(b, BINARY_XMM0, BINARY_GP_RAX)) {
    return 0;
  }

  size_t top = b->size;
  size_t to_done[4];
  size_t n_done = 0;
  size_t to_hit = 0;
  if (!wcs_cmp_reg_reg64(b, BINARY_GP_R11, BINARY_GP_R10) ||
      !wcs_jcc(b, 0x8D , &to_done[n_done++]) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_RAX, BINARY_GP_RDX) ||
      !binary_emit_and_reg_imm32(b, BINARY_GP_RAX, 4095) ||
      !wcs_cmp_reg_imm32(b, BINARY_GP_RAX, 4080) ||
      !wcs_jcc(b, 0x87 ,
               &to_done[n_done++]) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_RAX, BINARY_GP_R10) ||
      !wcs_sub_reg_reg64(b, BINARY_GP_RAX, BINARY_GP_R11) ||
      !wcs_cmp_reg_imm8(b, BINARY_GP_RAX, 16) ||
      !wcs_jcc(b, 0x8C ,
               &to_done[n_done++]) ||
      !wcs_sse42_pcmpistri_ranges_mem(b, BINARY_XMM0, BINARY_GP_RDX,
                                      0x14) ||
      !wcs_cmp_reg_imm8(b, BINARY_GP_RCX, 16) ||
      !wcs_jcc(b, 0x85 ,
               &to_hit) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 16) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_R11, 0, 16)) {
    return 0;
  }
  {
    size_t back = 0;
    if (!wcs_jcc(b, 0, &back) || !wcs_patch_to(b, back, top) ||
        !wcs_patch_here(b, to_hit) ||
        !wcs_add_reg_reg64(b, BINARY_GP_R11, BINARY_GP_RCX)) {
      return 0;
    }
  }
  for (size_t i = 0; i < n_done; i++) {
    if (!wcs_patch_here(b, to_done[i])) return 0;
  }
  if (!code_generator_binary_emit_destination_store(generator, context,
                                                      &instruction->dest,
                                                      BINARY_GP_R11)) {
    return 0;
  }
  return 1;
}

typedef struct {
  CodeGenerator *generator;
  BinaryFunctionContext *context;
  const IRInstruction *instruction;
  BinaryCodeBuffer *b;
  const IROperand *rhs;
  int pred;
  int u8;
  int rhs_kind;
  int lanes;
  int esz;
  int two_arrays;
  int invert_mask;
  uint32_t full_mask;
  size_t to_done[3];
  size_t n_done;
} SimdFind;

static int simd_find_decode(SimdFind *f, CodeGenerator *generator,
                            BinaryFunctionContext *context,
                            const IRInstruction *instruction) {
  const IROperand *args = instruction->arguments;
  f->generator = generator;
  f->context = context;
  f->instruction = instruction;
  f->b = &context->code;
  f->pred = (int)args[0].int_value;
  f->u8 = (int)args[1].int_value == 1;
  f->rhs_kind = (int)args[2].int_value;
  f->rhs = &args[3];
  if (instruction->argument_count != 4 ||
      instruction->rhs.kind != IR_OPERAND_SYMBOL || f->pred < VFIND_EQ ||
      f->pred > VFIND_GE) {
    code_generator_set_error(generator, "Bad simd_find encoding");
    return 0;
  }
  f->lanes = f->u8 ? 32 : 8;
  f->esz = f->u8 ? 1 : 4;
  f->two_arrays = (f->rhs_kind == 2);
  f->invert_mask = (f->pred == VFIND_NE || f->pred == VFIND_LE ||
                    f->pred == VFIND_GE);
  f->full_mask = f->u8 ? 0xFFFFFFFFu : 0xFFu;
  if (f->rhs_kind < 0 || f->rhs_kind > 2 ||
      (f->u8 && f->pred != VFIND_EQ && f->pred != VFIND_NE)) {
    code_generator_set_error(generator, "Bad simd_find encoding");
    return 0;
  }
  return 1;
}

static int simd_find_load_operands(SimdFind *f) {
  BinaryCodeBuffer *b = f->b;
  if (!code_generator_binary_emit_operand_load(f->generator, f->context,
                                               &f->instruction->lhs,
                                               BINARY_GP_R10) ||
      !code_generator_binary_emit_operand_load(f->generator, f->context,
                                               &f->instruction->rhs,
                                               BINARY_GP_RCX)) {
    return 0;
  }
  if (f->two_arrays) {
    return code_generator_binary_emit_operand_load(f->generator, f->context,
                                                   f->rhs, BINARY_GP_RDX);
  }
  if (f->rhs_kind == 0) {
    if (!binary_emit_mov_reg_imm64(b, BINARY_GP_R8,
                                   (uint64_t)f->rhs->int_value)) {
      return 0;
    }
  } else if (!code_generator_binary_emit_operand_load(f->generator, f->context,
                                                      f->rhs, BINARY_GP_R8)) {
    return 0;
  }
  return !f->u8 || binary_emit_movzx_reg_reg8(b, BINARY_GP_R8, BINARY_GP_R8);
}

static int simd_find_broadcast_needle(SimdFind *f) {
  BinaryCodeBuffer *b = f->b;
  if (f->two_arrays) {
    return 1;
  }
  if (f->u8) {
    return wcs_avx_vmovd_xmm_reg(b, 1, BINARY_GP_R8) &&
           wcs_avx_vpbroadcastb_ymm(b, 1, 1);
  }
  return wcs_broadcast_i32_to_ymm(b, 1, BINARY_GP_R8);
}

static int simd_find_load_element(SimdFind *f, BinaryGpRegister address,
                                  BinaryGpRegister into) {
  if (f->u8) {
    return wcs_movzx_reg_byte_mem(f->b, into, address);
  }
  return code_generator_binary_emit_load_from_address(f->generator, f->context,
                                                      address, 4, into);
}

static int simd_find_head_step(SimdFind *f) {
  BinaryCodeBuffer *b = f->b;
  if (!wcs_cmp_reg_reg64(b, BINARY_GP_R11, BINARY_GP_R10) ||
      !wcs_jcc(b, 0x8D, &f->to_done[f->n_done])) {
    return 0;
  }
  f->n_done++;
  if (!simd_find_load_element(f, BINARY_GP_RCX, BINARY_GP_R9)) {
    return 0;
  }
  if (f->two_arrays) {
    if (!simd_find_load_element(f, BINARY_GP_RDX, BINARY_GP_RAX) ||
        !wcs_cmp_reg_reg32(b, BINARY_GP_R9, BINARY_GP_RAX)) {
      return 0;
    }
  } else if (!wcs_cmp_reg_reg32(b, BINARY_GP_R9, BINARY_GP_R8)) {
    return 0;
  }
  if (!wcs_jcc(b, vfind_hit_cc(f->pred), &f->to_done[f->n_done])) {
    return 0;
  }
  f->n_done++;
  return wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, (unsigned char)f->esz) &&
         (!f->two_arrays ||
          wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, (unsigned char)f->esz)) &&
         wcs_addsub_reg_imm8(b, BINARY_GP_R11, 0, 1);
}

static int simd_find_compare_lanes(SimdFind *f) {
  BinaryCodeBuffer *b = f->b;
  int src2 = f->two_arrays ? 2 : 1;
  switch (f->pred) {
  case VFIND_EQ:
  case VFIND_NE:
    return f->u8 ? wcs_avx_vpcmpeqb_ymm(b, 0, 0, src2)
                 : wcs_avx_vpcmpeqd_ymm(b, 0, 0, src2);
  case VFIND_GT:
  case VFIND_LE:
    return wcs_avx_vpcmpgtd_ymm(b, 0, 0, src2);
  default:
    return wcs_avx_vpcmpgtd_ymm(b, 0, src2, 0);
  }
}

static int simd_find_vector_step(SimdFind *f, size_t *j_hit) {
  BinaryCodeBuffer *b = f->b;
  if (!binary_emit_mov_reg_reg(b, BINARY_GP_RAX, BINARY_GP_R10) ||
      !wcs_sub_reg_reg64(b, BINARY_GP_RAX, BINARY_GP_R11) ||
      !wcs_cmp_reg_imm8(b, BINARY_GP_RAX, (unsigned char)f->lanes) ||
      !wcs_jcc(b, 0x8C, &f->to_done[f->n_done])) {
    return 0;
  }
  f->n_done++;
  if (!wcs_avx_vmovups_ymm_mem(b, 0, BINARY_GP_RCX, 0)) {
    return 0;
  }
  if (f->two_arrays && !wcs_avx_vmovups_ymm_mem(b, 2, BINARY_GP_RDX, 0)) {
    return 0;
  }
  if (!simd_find_compare_lanes(f)) {
    return 0;
  }
  if (!(f->u8 ? wcs_avx_vpmovmskb_reg_ymm(b, BINARY_GP_RAX, 0)
              : wcs_avx_vmovmskps_reg_ymm(b, BINARY_GP_RAX, 0))) {
    return 0;
  }
  if (f->invert_mask && !wcs_xor_reg_imm32(b, BINARY_GP_RAX, f->full_mask)) {
    return 0;
  }
  if (!wcs_test_reg_reg32(b, BINARY_GP_RAX) || !wcs_jcc(b, 0x85, j_hit)) {
    return 0;
  }
  return wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 32) &&
         (!f->two_arrays || wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 32)) &&
         wcs_addsub_reg_imm8(b, BINARY_GP_R11, 0, (unsigned char)f->lanes);
}

static int simd_find_is_ascii_class(const IRInstruction *instruction) {
  return (int)instruction->arguments[0].int_value == VFIND_ASCII_IDENT_END;
}

int code_generator_binary_emit_simd_find(CodeGenerator *generator,
                                         BinaryFunctionContext *context,
                                         const IRInstruction *instruction) {
  SimdFind f = {0};
  BinaryCodeBuffer *b = NULL;
  size_t head_top = 0;
  size_t vec_top = 0;
  size_t j_vec = 0;
  size_t j_hit = 0;
  size_t k = 0;

  if (!generator || !context || !instruction ||
      (instruction->argument_count != 4 && instruction->argument_count != 5) ||
      !instruction->arguments ||
      instruction->dest.kind != IR_OPERAND_SYMBOL ||
      (instruction->rhs.kind != IR_OPERAND_SYMBOL &&
       instruction->rhs.kind != IR_OPERAND_TEMP)) {
    code_generator_set_error(generator, "Malformed simd_find");
    return 0;
  }
  if (simd_find_is_ascii_class(instruction)) {
    const IROperand *args = instruction->arguments;
    if ((int)args[1].int_value != 1 || (int)args[2].int_value != 0 ||
        instruction->argument_count != 5 || args[3].kind != IR_OPERAND_INT) {
      code_generator_set_error(generator, "Bad ASCII class simd_find encoding");
      return 0;
    }
    return code_generator_binary_emit_simd_find_ascii_ident(generator, context,
                                                            instruction);
  }
  if (!simd_find_decode(&f, generator, context, instruction)) {
    return 0;
  }
  b = f.b;
  if (!simd_find_load_operands(&f) || !simd_find_broadcast_needle(&f) ||
      !binary_emit_mov_reg_imm64(b, BINARY_GP_R11, 0)) {
    return 0;
  }

  head_top = b->size;
  if (!binary_emit_mov_reg_imm64(b, BINARY_GP_RAX, 31) ||
      !wcs_and_reg_reg(b, BINARY_GP_RAX, BINARY_GP_RCX) ||
      !wcs_jcc(b, 0x84, &j_vec) || !simd_find_head_step(&f) ||
      !simd_loop_back_to(b, head_top) || !wcs_patch_here(b, j_vec)) {
    return 0;
  }

  vec_top = b->size;
  if (!simd_find_vector_step(&f, &j_hit) ||
      !simd_loop_back_to(b, vec_top) || !wcs_patch_here(b, j_hit) ||
      !wcs_bsf_reg_reg32(b, BINARY_GP_RAX, BINARY_GP_RAX) ||
      !wcs_add_reg_reg64(b, BINARY_GP_R11, BINARY_GP_RAX)) {
    return 0;
  }
  for (k = 0; k < f.n_done; k++) {
    if (!wcs_patch_here(b, f.to_done[k])) {
      return 0;
    }
  }
  if (!wcs_avx_vzeroupper(b)) {
    return 0;
  }
  return code_generator_binary_emit_destination_store(generator, context,
                                                      &instruction->dest,
                                                      BINARY_GP_R11);
}
