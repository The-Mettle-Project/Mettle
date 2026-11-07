#include "codegen/binary/internal.h"
#include "codegen/binary/simd_internal.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int simd_clamped_count(BinaryCodeBuffer *b) {
  return binary_emit_mov_reg_reg(b, BINARY_GP_R9, BINARY_GP_R8) &&
         binary_emit_shift_reg_imm8(b, 7, BINARY_GP_R9, 63) &&
         binary_code_buffer_append_u8(b, 0x49) &&
         binary_code_buffer_append_u8(b, 0xF7) &&
         binary_code_buffer_append_u8(b, 0xD1) &&
         binary_code_buffer_append_u8(b, 0x4D) &&
         binary_code_buffer_append_u8(b, 0x21) &&
         binary_code_buffer_append_u8(b, 0xC1);
}

int code_generator_binary_emit_simd_sum_f64(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  BinaryCodeBuffer *b = NULL;
  size_t loop_top = 0;
  size_t j_done = 0;
  size_t j_vec2 = 0;
  size_t j_vec = 0;
  size_t j_scalar = 0;

  if (!generator || !context || !instruction) {
    return 0;
  }
  b = &context->code;

  if (!code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->dest,
                                               BINARY_GP_RAX) ||
      !wcs_vex_movq_xmm_reg(b, BINARY_XMM3, BINARY_GP_RAX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->lhs,
                                               BINARY_GP_RCX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->rhs,
                                               BINARY_GP_R8) ||
      !simd_clamped_count(b) ||
      !binary_emit_shift_reg_imm8(b, 4, BINARY_GP_R9, 3) ||
      !wcs_add_reg_reg64(b, BINARY_GP_R9, BINARY_GP_RCX) ||
      !wcs_avx_vpxor_ymm(b, 2, 2, 2) ||
      !wcs_avx_vpxor_ymm(b, 4, 4, 4)) {
    return 0;
  }

  loop_top = b->size;
  if (!binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R9) ||
      !wcs_jcc(b, 0x83 , &j_done) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_R10, BINARY_GP_R9) ||
      !binary_emit_alu_reg_reg(b, 0x29, BINARY_GP_R10, BINARY_GP_RCX) ||
      !wcs_cmp_reg_imm32(b, BINARY_GP_R10, 64) ||
      !wcs_jcc(b, 0x83 , &j_vec2) ||
      !wcs_cmp_reg_imm32(b, BINARY_GP_R10, 32) ||
      !wcs_jcc(b, 0x83 , &j_vec) ||
      !wcs_jcc(b, 0, &j_scalar)) {
    return 0;
  }

  if (!wcs_patch_here(b, j_vec2) ||
      !wcs_avx_vmovups_ymm_mem(b, 0, BINARY_GP_RCX, 0) ||
      !wcs_avx_vmovups_ymm_mem(b, 1, BINARY_GP_RCX, 32) ||
      !wcs_avx_vaddpd_ymm(b, 2, 2, 0) ||
      !wcs_avx_vaddpd_ymm(b, 4, 4, 1) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 64)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, loop_top)) {
      return 0;
    }
  }

  if (!wcs_patch_here(b, j_vec) ||
      !wcs_avx_vmovups_ymm_mem(b, 0, BINARY_GP_RCX, 0) ||
      !wcs_avx_vaddpd_ymm(b, 2, 2, 0) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 32)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, loop_top)) {
      return 0;
    }
  }

  if (!wcs_patch_here(b, j_scalar) ||
      !wcs_vex_movsd_xmm_mem(b, 0, BINARY_GP_RCX, 0) ||
      !wcs_vex_addsd_xmm_xmm(b, BINARY_XMM3, BINARY_XMM0) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 8)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, loop_top)) {
      return 0;
    }
  }

  if (!wcs_patch_here(b, j_done) ||
      !wcs_avx_vaddpd_ymm(b, 2, 2, 4) ||
      !wcs_reduce_pd_acc_to_rax(b)) {
    return 0;
  }
  return code_generator_binary_emit_destination_store(generator, context,
                                                      &instruction->dest,
                                                      BINARY_GP_RAX);
}

int code_generator_binary_emit_simd_sum_f32(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  BinaryCodeBuffer *b = NULL;
  size_t loop_top = 0;
  size_t j_done = 0;
  size_t j_vec2 = 0;
  size_t j_vec = 0;
  size_t j_scalar = 0;

  if (!generator || !context || !instruction) {
    return 0;
  }
  b = &context->code;

  if (!code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->dest,
                                               BINARY_GP_RAX) ||
      !wcs_vex_movd_xmm_reg(b, BINARY_XMM3, BINARY_GP_RAX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->lhs,
                                               BINARY_GP_RCX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->rhs,
                                               BINARY_GP_R8) ||
      !simd_clamped_count(b) ||
      !binary_emit_shift_reg_imm8(b, 4, BINARY_GP_R9, 2) ||
      !wcs_add_reg_reg64(b, BINARY_GP_R9, BINARY_GP_RCX) ||
      !wcs_avx_vpxor_ymm(b, 2, 2, 2) ||
      !wcs_avx_vpxor_ymm(b, 4, 4, 4)) {
    return 0;
  }

  loop_top = b->size;
  if (!binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R9) ||
      !wcs_jcc(b, 0x83 , &j_done) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_R10, BINARY_GP_R9) ||
      !binary_emit_alu_reg_reg(b, 0x29, BINARY_GP_R10, BINARY_GP_RCX) ||
      !wcs_cmp_reg_imm32(b, BINARY_GP_R10, 64) ||
      !wcs_jcc(b, 0x83 , &j_vec2) ||
      !wcs_cmp_reg_imm32(b, BINARY_GP_R10, 32) ||
      !wcs_jcc(b, 0x83 , &j_vec) ||
      !wcs_jcc(b, 0, &j_scalar)) {
    return 0;
  }

  if (!wcs_patch_here(b, j_vec2) ||
      !wcs_avx_vmovups_ymm_mem(b, 0, BINARY_GP_RCX, 0) ||
      !wcs_avx_vmovups_ymm_mem(b, 1, BINARY_GP_RCX, 32) ||
      !wcs_avx_vaddps_ymm(b, 2, 2, 0) ||
      !wcs_avx_vaddps_ymm(b, 4, 4, 1) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 64)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, loop_top)) {
      return 0;
    }
  }

  if (!wcs_patch_here(b, j_vec) ||
      !wcs_avx_vmovups_ymm_mem(b, 0, BINARY_GP_RCX, 0) ||
      !wcs_avx_vaddps_ymm(b, 2, 2, 0) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 32)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, loop_top)) {
      return 0;
    }
  }

  if (!wcs_patch_here(b, j_scalar) ||
      !wcs_vex_movss_xmm_mem(b, 0, BINARY_GP_RCX, 0) ||
      !wcs_vex_addss_xmm_xmm(b, BINARY_XMM3, BINARY_XMM0) ||
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
      !wcs_avx_vaddps_ymm(b, 2, 2, 4) ||
      !wcs_reduce_ps_acc_to_rax(b)) {
    return 0;
  }
  return code_generator_binary_emit_destination_store(generator, context,
                                                      &instruction->dest,
                                                      BINARY_GP_RAX);
}

static int dot_f64_load(BinaryCodeBuffer *b, int reg, int base, int disp,
                        int widen) {
  if (!widen) {
    return wcs_avx_vmovups_ymm_mem(b, reg, base, disp);
  }
  return wcs_vex3(b, 1, 0, 1, 0, reg, base, 0) &&
         binary_code_buffer_append_u8(b, 0x5A) &&
         wcs_avx_modrm_mem_disp(b, reg, base, disp);
}

static int dot_f64_load_scalar(BinaryCodeBuffer *b, int reg, int base,
                               int widen) {
  if (!widen) {
    return wcs_vex_movsd_xmm_mem(b, reg, base, 0);
  }
  return wcs_vex3(b, 1, 2, 0, 0, reg, base, reg) &&
         binary_code_buffer_append_u8(b, 0x5A) &&
         wcs_avx_modrm_mem_disp(b, reg, base, 0);
}

static int dot_f64_step(BinaryCodeBuffer *b, int acc, int disp, int widen) {
  return dot_f64_load(b, 0, BINARY_GP_RCX, disp, widen) &&
         dot_f64_load(b, 1, BINARY_GP_RDX, disp, widen) &&
         wcs_avx_vfmadd231pd_ymm(b, acc, 0, 1);
}

static int dot_f64_advance(BinaryCodeBuffer *b, size_t loop_top,
                           unsigned char bytes) {
  size_t j_back = 0;
  return wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, bytes) &&
         wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, bytes) &&
         wcs_jcc(b, 0, &j_back) && wcs_patch_to(b, j_back, loop_top);
}

static int dot_f64_widens(const IRInstruction *instruction) {
  return instruction->argument_count > 1 &&
         instruction->arguments[1].kind == IR_OPERAND_INT &&
         instruction->arguments[1].int_value == 1;
}

int code_generator_binary_emit_simd_dot_f64(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  BinaryCodeBuffer *b = NULL;
  size_t loop_top = 0;
  size_t j_done = 0;
  size_t j_vec2 = 0;
  size_t j_vec = 0;
  size_t j_scalar = 0;

  if (!generator || !context || !instruction ||
      instruction->argument_count < 1 || !instruction->arguments) {
    code_generator_set_error(generator, "Malformed simd_dot_f64");
    return 0;
  }
  b = &context->code;
  int widen = dot_f64_widens(instruction);
  int size = widen ? 4 : 8;

  if (!code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->dest,
                                               BINARY_GP_RAX) ||
      !wcs_vex_movq_xmm_reg(b, BINARY_XMM3, BINARY_GP_RAX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->lhs,
                                               BINARY_GP_RCX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->rhs,
                                               BINARY_GP_RDX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->arguments[0],
                                               BINARY_GP_R8) ||
      !simd_clamped_count(b) ||
      !binary_emit_shift_reg_imm8(b, 4, BINARY_GP_R9, widen ? 2 : 3) ||
      !wcs_add_reg_reg64(b, BINARY_GP_R9, BINARY_GP_RCX) ||
      !wcs_avx_vpxor_ymm(b, 2, 2, 2) ||
      !wcs_avx_vpxor_ymm(b, 4, 4, 4)) {
    return 0;
  }

  loop_top = b->size;
  if (!binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R9) ||
      !wcs_jcc(b, 0x83 , &j_done) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_R10, BINARY_GP_R9) ||
      !binary_emit_alu_reg_reg(b, 0x29, BINARY_GP_R10, BINARY_GP_RCX) ||
      !wcs_cmp_reg_imm32(b, BINARY_GP_R10, (uint32_t)(8 * size)) ||
      !wcs_jcc(b, 0x83 , &j_vec2) ||
      !wcs_cmp_reg_imm32(b, BINARY_GP_R10, (uint32_t)(4 * size)) ||
      !wcs_jcc(b, 0x83 , &j_vec) ||
      !wcs_jcc(b, 0, &j_scalar)) {
    return 0;
  }

  if (!wcs_patch_here(b, j_vec2) || !dot_f64_step(b, 2, 0, widen) ||
      !dot_f64_step(b, 4, 4 * size, widen) ||
      !dot_f64_advance(b, loop_top, (unsigned char)(8 * size)) ||
      !wcs_patch_here(b, j_vec) || !dot_f64_step(b, 2, 0, widen) ||
      !dot_f64_advance(b, loop_top, (unsigned char)(4 * size)) ||
      !wcs_patch_here(b, j_scalar) ||
      !dot_f64_load_scalar(b, 0, BINARY_GP_RCX, widen) ||
      !dot_f64_load_scalar(b, 1, BINARY_GP_RDX, widen) ||
      !wcs_fmadd231sd(b, 3, 0, 1) ||
      !dot_f64_advance(b, loop_top, (unsigned char)size)) {
    return 0;
  }

  if (!wcs_patch_here(b, j_done) ||
      !wcs_avx_vaddpd_ymm(b, 2, 2, 4) ||
      !wcs_reduce_pd_acc_to_rax(b)) {
    return 0;
  }
  return code_generator_binary_emit_destination_store(generator, context,
                                                      &instruction->dest,
                                                      BINARY_GP_RAX);
}

static int wcs_affine_f64_scalar_step(BinaryCodeBuffer *b, int b_is_one,
                                      int b_is_zero, int c_is_zero) {
  if (!wcs_vex_movsd_xmm_mem(b, 0, BINARY_GP_RCX, 0) ||
      !wcs_vex_movsd_xmm_mem(b, 1, BINARY_GP_RDX, 0)) {
    return 0;
  }
  if (b_is_one && c_is_zero) {
    if (!wcs_fmadd231sd(b, 1, 0, 4) ||
        !wcs_vex_movsd_mem_xmm(b, BINARY_GP_RDX, 0, BINARY_XMM1)) {
      return 0;
    }
  } else {
    if (!wcs_vex_mulsd_xmm_xmm(b, BINARY_XMM0, BINARY_XMM4)) {
      return 0;
    }
    if (!b_is_zero && !wcs_fmadd231sd(b, 0, 5, 1)) {
      return 0;
    }
    if (!c_is_zero && !wcs_vex_addsd_xmm_xmm(b, BINARY_XMM0, BINARY_XMM3)) {
      return 0;
    }
    if (!wcs_vex_movsd_mem_xmm(b, BINARY_GP_RDX, 0, BINARY_XMM0)) {
      return 0;
    }
  }
  return wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 8) &&
         wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 8);
}

static int wcs_affine_alias_guard(BinaryCodeBuffer *b, size_t *j_alias) {
  size_t j_same = 0, j_after = 0;
  if (!binary_emit_cmp_reg_reg(b, BINARY_GP_RDX, BINARY_GP_RCX) ||
      !wcs_jcc(b, 0x84 , &j_same) ||
      !binary_emit_cmp_reg_reg(b, BINARY_GP_RDX, BINARY_GP_R9) ||
      !wcs_jcc(b, 0x83 , &j_after)) {
    return 0;
  }
  if (!binary_emit_mov_reg_reg(b, BINARY_GP_R11, BINARY_GP_RDX) ||
      !wcs_add_reg_reg64(b, BINARY_GP_R11, BINARY_GP_R9) ||
      !binary_emit_alu_reg_reg(b, 0x29, BINARY_GP_R11, BINARY_GP_RCX) ||
      !binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R11) ||
      !wcs_jcc(b, 0x82 , j_alias)) {
    return 0;
  }
  return wcs_patch_here(b, j_same) && wcs_patch_here(b, j_after);
}

int code_generator_binary_emit_simd_affine_map_f64_loop(BinaryCodeBuffer *b,
                                                        int b_is_one,
                                                        int b_is_zero,
                                                        int c_is_zero) {
  size_t loop_top = 0, j_done = 0, j_scalar = 0;
  size_t j_alias = 0;
  if (!wcs_affine_alias_guard(b, &j_alias)) {
    return 0;
  }

  if (b_is_one && c_is_zero) {
    size_t j_skip64 = 0, top64 = 0;
    if (!binary_emit_mov_reg_reg(b, BINARY_GP_R11, BINARY_GP_R9) ||
        !binary_emit_alu_reg_reg(b, 0x29, BINARY_GP_R11, BINARY_GP_RCX) ||
        !wcs_shift_reg_imm(b, BINARY_GP_R11, 1 , 6) ||
        !wcs_shift_reg_imm(b, BINARY_GP_R11, 0 , 6) ||
        !wcs_add_reg_reg64(b, BINARY_GP_R11, BINARY_GP_RCX)) {
      return 0;
    }
    if (!binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R11) ||
        !wcs_jcc(b, 0x83 , &j_skip64)) {
      return 0;
    }
    top64 = b->size;
    if (!wcs_avx_vmovups_ymm_mem(b, 0, BINARY_GP_RCX, 0) ||
        !wcs_avx_vmovups_ymm_mem(b, 1, BINARY_GP_RDX, 0) ||
        !wcs_avx_vfmadd231pd_ymm(b, 1, 0, 4) ||
        !wcs_avx_vmovups_mem_ymm(b, BINARY_GP_RDX, 0, 1) ||
        !wcs_avx_vmovups_ymm_mem(b, 2, BINARY_GP_RCX, 32) ||
        !wcs_avx_vmovups_ymm_mem(b, 3, BINARY_GP_RDX, 32) ||
        !wcs_avx_vfmadd231pd_ymm(b, 3, 2, 4) ||
        !wcs_avx_vmovups_mem_ymm(b, BINARY_GP_RDX, 32, 3)) {
      return 0;
    }
    if (!wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 64) ||
        !wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 64) ||
        !binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R11)) {
      return 0;
    }
    {
      size_t j_back = 0;
      if (!wcs_jcc(b, 0x82 , &j_back) ||
          !wcs_patch_to(b, j_back, top64)) {
        return 0;
      }
    }
    if (!wcs_patch_here(b, j_skip64)) {
      return 0;
    }
  }

  if (!binary_emit_mov_reg_reg(b, BINARY_GP_R11, BINARY_GP_R9) ||
      !binary_emit_alu_reg_reg(b, 0x29, BINARY_GP_R11, BINARY_GP_RCX) ||
      !wcs_shift_reg_imm(b, BINARY_GP_R11, 1 , 5) ||
      !wcs_shift_reg_imm(b, BINARY_GP_R11, 0 , 5) ||
      !wcs_add_reg_reg64(b, BINARY_GP_R11, BINARY_GP_RCX)) {
    return 0;
  }

  if (!binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R11) ||
      !wcs_jcc(b, 0x83 , &j_scalar)) {
    return 0;
  }
  loop_top = b->size;
  if (!wcs_avx_vmovups_ymm_mem(b, 0, BINARY_GP_RCX, 0) ||
      !wcs_avx_vmovups_ymm_mem(b, 1, BINARY_GP_RDX, 0)) {
    return 0;
  }
  if (b_is_one && c_is_zero) {
    if (!wcs_avx_vfmadd231pd_ymm(b, 1, 0, 4) ||
        !wcs_avx_vmovups_mem_ymm(b, BINARY_GP_RDX, 0, 1)) {
      return 0;
    }
  } else {
    if (!wcs_avx_vmulpd_ymm(b, 0, 0, 4)) {
      return 0;
    }
    if (!b_is_zero && !wcs_avx_vfmadd231pd_ymm(b, 0, 5, 1)) {
      return 0;
    }
    if (!c_is_zero && !wcs_avx_vaddpd_ymm(b, 0, 0, 3)) {
      return 0;
    }
    if (!wcs_avx_vmovups_mem_ymm(b, BINARY_GP_RDX, 0, 0)) {
      return 0;
    }
  }
  if (!wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 32) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 32) ||
      !binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R11)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0x82 , &j_back) ||
        !wcs_patch_to(b, j_back, loop_top)) {
      return 0;
    }
  }

  if (!wcs_patch_here(b, j_scalar) || !wcs_patch_here(b, j_alias) ||
      !binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R9) ||
      !wcs_jcc(b, 0x83 , &j_done)) {
    return 0;
  }
  {
    size_t scalar_top = b->size;
    if (!wcs_affine_f64_scalar_step(b, b_is_one, b_is_zero, c_is_zero) ||
        !binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R9)) {
      return 0;
    }
    {
      size_t j_back = 0;
      if (!wcs_jcc(b, 0x82 , &j_back) ||
          !wcs_patch_to(b, j_back, scalar_top)) {
        return 0;
      }
    }
  }

  return wcs_patch_here(b, j_done) && wcs_avx_vzeroupper(b);
}

int code_generator_binary_emit_simd_affine_map_f64_inline(
    BinaryCodeBuffer *b, unsigned long long a_bits, unsigned long long b_bits,
    unsigned long long c_bits, int b_is_one, int b_is_zero, int c_is_zero,
    int a_runtime) {
  if (a_runtime) {
    if (!wcs_avx_vbroadcastsd_ymm_xmm(b, 4, 4)) {
      return 0;
    }
  } else if (!binary_emit_mov_reg_imm64(b, BINARY_GP_RAX, (long long)a_bits) ||
             !wcs_vex_movq_xmm_reg(b, BINARY_XMM4, BINARY_GP_RAX) ||
             !wcs_avx_vbroadcastsd_ymm_xmm(b, 4, 4)) {
    return 0;
  }
  if (!binary_emit_mov_reg_imm64(b, BINARY_GP_RAX, (long long)b_bits) ||
      !wcs_vex_movq_xmm_reg(b, BINARY_XMM5, BINARY_GP_RAX) ||
      !wcs_avx_vbroadcastsd_ymm_xmm(b, 5, 5) ||
      !binary_emit_mov_reg_imm64(b, BINARY_GP_RAX, (long long)c_bits) ||
      !wcs_vex_movq_xmm_reg(b, BINARY_XMM3, BINARY_GP_RAX) ||
      !wcs_avx_vbroadcastsd_ymm_xmm(b, 3, 3) ||
      !simd_clamped_count(b) ||
      !binary_emit_shift_reg_imm8(b, 4, BINARY_GP_R9, 3) ||
      !wcs_add_reg_reg64(b, BINARY_GP_R9, BINARY_GP_RCX)) {
    return 0;
  }
  return code_generator_binary_emit_simd_affine_map_f64_loop(b, b_is_one,
                                                             b_is_zero,
                                                             c_is_zero);
}

int code_generator_binary_emit_simd_affine_map_f64(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  BinaryCodeBuffer *b = NULL;

  if (!generator || !context || !instruction ||
      instruction->argument_count < 4 || !instruction->arguments) {
    code_generator_set_error(generator, "Malformed simd_affine_map_f64");
    return 0;
  }
  b = &context->code;

  int b_is_one = instruction->arguments[2].kind == IR_OPERAND_FLOAT &&
                 instruction->arguments[2].float_value == 1.0;
  int b_is_zero = instruction->arguments[2].kind == IR_OPERAND_FLOAT &&
                  instruction->arguments[2].float_value == 0.0;
  int c_is_zero = instruction->arguments[3].kind == IR_OPERAND_FLOAT &&
                  instruction->arguments[3].float_value == 0.0;

  if (!code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->lhs,
                                               BINARY_GP_RCX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->rhs,
                                               BINARY_GP_RDX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->arguments[0],
                                               BINARY_GP_R8) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->arguments[1],
                                               BINARY_GP_RAX) ||
      !wcs_vex_movq_xmm_reg(b, BINARY_XMM4, BINARY_GP_RAX) ||
      !wcs_avx_vbroadcastsd_ymm_xmm(b, 4, 4) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->arguments[2],
                                               BINARY_GP_RAX) ||
      !wcs_vex_movq_xmm_reg(b, BINARY_XMM5, BINARY_GP_RAX) ||
      !wcs_avx_vbroadcastsd_ymm_xmm(b, 5, 5) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->arguments[3],
                                               BINARY_GP_RAX) ||
      !wcs_vex_movq_xmm_reg(b, BINARY_XMM3, BINARY_GP_RAX) ||
      !wcs_avx_vbroadcastsd_ymm_xmm(b, 3, 3) ||
      !simd_clamped_count(b) ||
      !binary_emit_shift_reg_imm8(b, 4, BINARY_GP_R9, 3) ||
      !wcs_add_reg_reg64(b, BINARY_GP_R9, BINARY_GP_RCX)) {
    return 0;
  }

  return code_generator_binary_emit_simd_affine_map_f64_loop(b, b_is_one,
                                                             b_is_zero,
                                                             c_is_zero);
}

int code_generator_binary_emit_simd_affine_map_f32_loop(BinaryCodeBuffer *b,
                                                        int b_is_one,
                                                        int b_is_zero,
                                                        int c_is_zero) {
  size_t loop_top = 0, j_done = 0, j_scalar = 0;
  size_t j_alias = 0;
  if (!wcs_affine_alias_guard(b, &j_alias)) {
    return 0;
  }

  if (!binary_emit_mov_reg_reg(b, BINARY_GP_R11, BINARY_GP_R9) ||
      !binary_emit_alu_reg_reg(b, 0x29, BINARY_GP_R11, BINARY_GP_RCX) ||
      !wcs_shift_reg_imm(b, BINARY_GP_R11, 1 , 5) ||
      !wcs_shift_reg_imm(b, BINARY_GP_R11, 0 , 5) ||
      !wcs_add_reg_reg64(b, BINARY_GP_R11, BINARY_GP_RCX)) {
    return 0;
  }

  if (!binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R11) ||
      !wcs_jcc(b, 0x83 , &j_scalar)) {
    return 0;
  }
  loop_top = b->size;
  if (!wcs_avx_vmovups_ymm_mem(b, 0, BINARY_GP_RCX, 0) ||
      !wcs_avx_vmovups_ymm_mem(b, 1, BINARY_GP_RDX, 0)) {
    return 0;
  }
  if (b_is_one && c_is_zero) {
    if (!wcs_avx_vfmadd231ps_ymm(b, 1, 0, 4) ||
        !wcs_avx_vmovups_mem_ymm(b, BINARY_GP_RDX, 0, 1)) {
      return 0;
    }
  } else {
    if (!wcs_avx_vmulps_ymm(b, 0, 0, 4)) {
      return 0;
    }
    if (!b_is_zero && !wcs_avx_vfmadd231ps_ymm(b, 0, 5, 1)) {
      return 0;
    }
    if (!c_is_zero && !wcs_avx_vaddps_ymm(b, 0, 0, 3)) {
      return 0;
    }
    if (!wcs_avx_vmovups_mem_ymm(b, BINARY_GP_RDX, 0, 0)) {
      return 0;
    }
  }
  if (!wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 32) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 32) ||
      !binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R11)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0x82 , &j_back) ||
        !wcs_patch_to(b, j_back, loop_top)) {
      return 0;
    }
  }

  if (!wcs_patch_here(b, j_scalar) || !wcs_patch_here(b, j_alias) ||
      !binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R9) ||
      !wcs_jcc(b, 0x83 , &j_done)) {
    return 0;
  }
  {
    size_t scalar_top = b->size;
    if (!wcs_vex_movss_xmm_mem(b, 0, BINARY_GP_RCX, 0) ||
        !wcs_vex_movss_xmm_mem(b, 1, BINARY_GP_RDX, 0)) {
      return 0;
    }
    if (b_is_one && c_is_zero) {
      if (!wcs_fmadd231ss(b, 1, 0, 4) ||
          !wcs_vex_movss_mem_xmm(b, BINARY_GP_RDX, 0, BINARY_XMM1)) {
        return 0;
      }
    } else if (!wcs_vex_mulss_xmm_xmm(b, BINARY_XMM0, BINARY_XMM4) ||
               (!b_is_zero && !wcs_fmadd231ss(b, 0, 5, 1)) ||
               (!c_is_zero &&
                !wcs_vex_addss_xmm_xmm(b, BINARY_XMM0, BINARY_XMM3)) ||
               !wcs_vex_movss_mem_xmm(b, BINARY_GP_RDX, 0, BINARY_XMM0)) {
      return 0;
    }
    if (!wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 4) ||
        !wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 4) ||
        !binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R9)) {
      return 0;
    }
    {
      size_t j_back = 0;
      if (!wcs_jcc(b, 0x82 , &j_back) ||
          !wcs_patch_to(b, j_back, scalar_top)) {
        return 0;
      }
    }
  }
  return wcs_patch_here(b, j_done) && wcs_avx_vzeroupper(b);
}

int code_generator_binary_emit_simd_affine_map_f32_inline(
    BinaryCodeBuffer *b, unsigned a_bits, unsigned b_bits, unsigned c_bits,
    int b_is_one, int b_is_zero, int c_is_zero, int a_runtime) {
  if (a_runtime) {
    if (!wcs_avx_vpbroadcastd_ymm(b, 4, 4)) {
      return 0;
    }
  } else if (!binary_emit_mov_reg_imm64(b, BINARY_GP_RAX, a_bits) ||
             !wcs_vex_movd_xmm_reg(b, 4, BINARY_GP_RAX) ||
             !wcs_avx_vpbroadcastd_ymm(b, 4, 4)) {
    return 0;
  }
  if (!binary_emit_mov_reg_imm64(b, BINARY_GP_RAX, b_bits) ||
      !wcs_vex_movd_xmm_reg(b, 5, BINARY_GP_RAX) ||
      !wcs_avx_vpbroadcastd_ymm(b, 5, 5) ||
      !binary_emit_mov_reg_imm64(b, BINARY_GP_RAX, c_bits) ||
      !wcs_vex_movd_xmm_reg(b, 3, BINARY_GP_RAX) ||
      !wcs_avx_vpbroadcastd_ymm(b, 3, 3) ||
      !simd_clamped_count(b) ||
      !binary_emit_shift_reg_imm8(b, 4, BINARY_GP_R9, 2) ||
      !wcs_add_reg_reg64(b, BINARY_GP_R9, BINARY_GP_RCX)) {
    return 0;
  }
  return code_generator_binary_emit_simd_affine_map_f32_loop(b, b_is_one,
                                                             b_is_zero,
                                                             c_is_zero);
}

int code_generator_binary_emit_simd_affine_map_f32(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  BinaryCodeBuffer *b = NULL;

  if (!generator || !context || !instruction ||
      instruction->argument_count < 4 || !instruction->arguments) {
    code_generator_set_error(generator, "Malformed simd_affine_map_f32");
    return 0;
  }
  b = &context->code;

  int b_is_one = instruction->arguments[2].kind == IR_OPERAND_FLOAT &&
                 instruction->arguments[2].float_value == 1.0;
  int b_is_zero = instruction->arguments[2].kind == IR_OPERAND_FLOAT &&
                  instruction->arguments[2].float_value == 0.0;
  int c_is_zero = instruction->arguments[3].kind == IR_OPERAND_FLOAT &&
                  instruction->arguments[3].float_value == 0.0;

  if (!code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->lhs,
                                               BINARY_GP_RCX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->rhs,
                                               BINARY_GP_RDX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->arguments[0],
                                               BINARY_GP_R8) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->arguments[1],
                                               BINARY_GP_RAX) ||
      !wcs_vex_movd_xmm_reg(b, 4, BINARY_GP_RAX) ||
      !wcs_avx_vpbroadcastd_ymm(b, 4, 4) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->arguments[2],
                                               BINARY_GP_RAX) ||
      !wcs_vex_movd_xmm_reg(b, 5, BINARY_GP_RAX) ||
      !wcs_avx_vpbroadcastd_ymm(b, 5, 5) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->arguments[3],
                                               BINARY_GP_RAX) ||
      !wcs_vex_movd_xmm_reg(b, 3, BINARY_GP_RAX) ||
      !wcs_avx_vpbroadcastd_ymm(b, 3, 3) ||
      !simd_clamped_count(b) ||
      !binary_emit_shift_reg_imm8(b, 4, BINARY_GP_R9, 2) ||
      !wcs_add_reg_reg64(b, BINARY_GP_R9, BINARY_GP_RCX)) {
    return 0;
  }

  return code_generator_binary_emit_simd_affine_map_f32_loop(b, b_is_one,
                                                             b_is_zero,
                                                             c_is_zero);
}

#define VLOOP_K_LOAD 0
#define VLOOP_K_IOTA 1
#define VLOOP_K_CONST 2
#define VLOOP_K_ADD 3
#define VLOOP_K_SUB 4
#define VLOOP_K_MUL 5
#define VLOOP_K_DIV 6
#define VLOOP_K_SCALAR 7
#define VLOOP_K_AND 8
#define VLOOP_K_OR 9
#define VLOOP_K_XOR 10
#define VLOOP_K_SHL 11
#define VLOOP_K_SAR 12
#define VLOOP_K_SHR 13
#define VLOOP_K_MIN 14
#define VLOOP_K_MAX 15
#define VLOOP_K_CMPGT 16
#define VLOOP_K_PAIR 17
#define VLOOP_K_SELECT 18
#define VLOOP_K_CMPEQ 19
#define VLOOP_KERNEL_REGS 4
#define VLOOP_KERNEL_MAX_NODES 48
#define VLOOP_KERNEL_MAX_BASES 4
#define VLOOP_KERNEL_POOL_MAX 6

int code_generator_vloop_pool_size(int elem8, int has_iota) {
  return 4 + (elem8 ? 0 : 1) + (has_iota ? 0 : 1);
}

static int vloop_kernel_tag_is_leaf(int tag) {
  return tag == VLOOP_K_LOAD || tag == VLOOP_K_IOTA || tag == VLOOP_K_CONST ||
         tag == VLOOP_K_SCALAR;
}

int code_generator_vloop_collect_dist(const IRInstruction *in, int is_reduce,
                                      const char *names[4],
                                      const IROperand *srcs[4], int *n_out) {
  int n_arrays = (int)in->arguments[1].int_value;
  int n = 0;
  if (!is_reduce) {
    names[0] = in->dest.name;
    srcs[0] = &in->dest;
    n = 1;
  }
  for (int k = 0; k < n_arrays; k++) {
    const IROperand *as = &in->arguments[7 + k];
    const char *nm = as->name;
    int found = 0;
    for (int j = 0; j < n; j++)
      if (names[j] && nm && strcmp(names[j], nm) == 0) { found = 1; break; }
    if (!found) {
      if (n >= VLOOP_KERNEL_MAX_BASES) return -1;
      names[n] = nm;
      srcs[n] = as;
      n++;
    }
  }
  *n_out = n;
  return 0;
}

int code_generator_binary_emit_simd_vloop_unmarshaled(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  return code_generator_binary_emit_simd_vloop_f64(generator, context,
                                                   instruction, 0);
}

enum { VLOOP_IOTA_CONST = 5, VLOOP_BYTE_MASK = 3 };

static const BinaryGpRegister VLOOP_GP[VLOOP_KERNEL_MAX_BASES] = {
    BINARY_GP_RCX, BINARY_GP_RDX, BINARY_GP_R8, BINARY_GP_R9};

typedef struct {
  CodeGenerator *generator;
  BinaryFunctionContext *context;
  const IRInstruction *instruction;
  BinaryCodeBuffer *b;
  const IROperand *args;
  size_t scalars_off;
  size_t nodes_off;
  size_t consts_off;
  int operands_marshaled;
  int i32;
  int f32;
  int elem8;
  int elem8_unsigned;
  int lanes;
  int elem_bytes;
  int vec_stride;
  int n_nodes;
  int n_consts;
  int n_scalars;
  int n_dist;
  int is_reduce;
  int is_minmax;
  int is_max;
  int has_iota;
  int pool_n;
  int pool_src[VLOOP_KERNEL_POOL_MAX];
  int arr_reg[VLOOP_KERNEL_MAX_BASES];
  int dst_reg;
  uint32_t cbytes;
  const IROperand *dist_src[VLOOP_KERNEL_MAX_BASES];
} VloopKernel;

typedef struct {
  int pool[VLOOP_KERNEL_POOL_MAX];
  int nfree;
  int vstk[VLOOP_KERNEL_MAX_NODES];
  int nv;
} VloopStack;

static int vloop_build_pool(VloopKernel *k, int depth) {
  static const int kPoolReduce[3] = {0, 1, 4};
  int map_pool[VLOOP_KERNEL_POOL_MAX] = {0, 1, 2, 4, 0, 0};
  int map_pool_n = 4;
  int i = 0;
  if (!k->elem8) {
    map_pool[map_pool_n++] = VLOOP_BYTE_MASK;
  }
  if (!k->has_iota) {
    map_pool[map_pool_n++] = VLOOP_IOTA_CONST;
  }
  k->pool_n = k->is_reduce ? 3 : map_pool_n;
  for (i = 0; i < k->pool_n; i++) {
    k->pool_src[i] = k->is_reduce ? kPoolReduce[i] : map_pool[i];
  }
  if (k->pool_n !=
          (k->is_reduce ? 3
                        : code_generator_vloop_pool_size(k->elem8,
                                                         k->has_iota)) ||
      depth > k->pool_n) {
    code_generator_set_error(k->generator, "simd_vloop depth over pool");
    return 0;
  }
  return 1;
}

static int vloop_decode_shape(VloopKernel *k) {
  const IRInstruction *instruction = k->instruction;
  k->i32 = (instruction->op == IR_OP_SIMD_VLOOP_I32);
  if (!k->i32 && instruction->float_bits != 32 &&
      instruction->float_bits != 64) {
    code_generator_set_error(k->generator, "simd_vloop bad float width");
    return 0;
  }
  k->elem8 = k->i32 && instruction->float_bits == 8;
  k->elem8_unsigned = k->elem8 && instruction->is_unsigned;
  if (k->i32 && instruction->float_bits != 32 &&
      instruction->float_bits != 8) {
    code_generator_set_error(k->generator, "simd_vloop bad int element width");
    return 0;
  }
  k->b = &k->context->code;
  k->f32 = k->i32 || (instruction->float_bits == 32);
  k->lanes = k->f32 ? 8 : 4;
  k->elem_bytes = k->elem8 ? 1 : (k->f32 ? 4 : 8);
  k->vec_stride = k->elem_bytes * k->lanes;
  return 1;
}

static int vloop_decode(VloopKernel *k, CodeGenerator *generator,
                        BinaryFunctionContext *context,
                        const IRInstruction *instruction,
                        int operands_marshaled) {
  const char *dist_name[VLOOP_KERNEL_MAX_BASES];
  const IROperand *args = NULL;
  long long reduce_op = 0;
  size_t expect = 0;
  int n_arrays = 0;
  int n_nodes = 0;
  int root = 0;
  int depth = 0;
  int i = 0;

  if (!generator || !context || !instruction ||
      instruction->argument_count < 7 || !instruction->arguments ||
      instruction->dest.kind != IR_OPERAND_SYMBOL) {
    code_generator_set_error(generator, "Malformed simd_vloop");
    return 0;
  }
  k->generator = generator;
  k->context = context;
  k->instruction = instruction;
  k->operands_marshaled = operands_marshaled;
  if (!vloop_decode_shape(k)) {
    return 0;
  }

  args = instruction->arguments;
  k->args = args;
  reduce_op = args[0].int_value;
  n_arrays = (int)args[1].int_value;
  n_nodes = (int)args[2].int_value;
  root = (int)args[3].int_value;
  k->n_nodes = n_nodes;
  k->n_consts = (int)args[4].int_value;
  k->n_scalars = (int)args[5].int_value;
  depth = (int)args[6].int_value;
  k->is_minmax = (reduce_op == 2 || reduce_op == 3);
  k->is_max = (reduce_op == 2);
  k->is_reduce = (reduce_op == 1) || k->is_minmax;
  expect = (size_t)(7 + n_arrays + k->n_scalars + 3 * n_nodes + k->n_consts);
  if ((reduce_op < 0 || reduce_op > 3) || n_arrays < 0 ||
      n_arrays > VLOOP_KERNEL_MAX_BASES || n_nodes <= 0 ||
      n_nodes > VLOOP_KERNEL_MAX_NODES || k->n_consts < 0 ||
      k->n_scalars < 0 || root < 0 || root >= n_nodes ||
      instruction->argument_count != expect) {
    code_generator_set_error(generator, "Bad simd_vloop encoding");
    return 0;
  }
  k->scalars_off = (size_t)(7 + n_arrays);
  k->nodes_off = k->scalars_off + (size_t)k->n_scalars;
  k->consts_off = k->nodes_off + (size_t)(3 * n_nodes);
  k->cbytes = (uint32_t)(32 * (k->n_consts + k->n_scalars));

  if (code_generator_vloop_collect_dist(instruction, k->is_reduce, dist_name,
                                        k->dist_src, &k->n_dist) < 0) {
    code_generator_set_error(generator, "simd_vloop_f64 too many bases");
    return 0;
  }
  for (i = 0; i < n_nodes; i++) {
    if ((int)args[k->nodes_off + 3 * i].int_value == VLOOP_K_IOTA) {
      k->has_iota = 1;
    }
  }
  if (!vloop_build_pool(k, depth)) {
    return 0;
  }
  for (i = 0; i < n_arrays; i++) {
    const char *nm = args[7 + i].name;
    int found = 0;
    for (int j = 0; j < k->n_dist; j++) {
      if (dist_name[j] && nm && strcmp(dist_name[j], nm) == 0) {
        found = j;
        break;
      }
    }
    k->arr_reg[i] = VLOOP_GP[found];
  }
  k->dst_reg = VLOOP_GP[0];
  return 1;
}

static int vloop_emit_bases(VloopKernel *k) {
  BinaryCodeBuffer *b = k->b;
  int j = 0;
  if (k->cbytes && !binary_emit_sub_rsp_imm32(b, k->cbytes)) {
    return 0;
  }
  if (k->operands_marshaled) {
    return binary_emit_mov_reg_reg(b, BINARY_GP_R10, VLOOP_GP[k->n_dist]);
  }
  for (j = 0; j < k->n_dist; j++) {
    if (!code_generator_binary_emit_operand_load(k->generator, k->context,
                                                 k->dist_src[j],
                                                 VLOOP_GP[j])) {
      return 0;
    }
  }
  return code_generator_binary_emit_operand_load(
      k->generator, k->context, &k->instruction->lhs, BINARY_GP_R10);
}

static uint64_t vloop_const_bits(const VloopKernel *k, const IROperand *op) {
  uint64_t bits = 0;
  if (k->i32) {
    uint32_t iv = (uint32_t)(uint64_t)op->int_value;
    return (uint64_t)iv | ((uint64_t)iv << 32);
  }
  if (k->f32) {
    float fv = (float)op->float_value;
    uint32_t fb = 0;
    memcpy(&fb, &fv, sizeof(fb));
    return (uint64_t)fb | ((uint64_t)fb << 32);
  }
  {
    double dv = op->float_value;
    memcpy(&bits, &dv, sizeof(bits));
  }
  return bits;
}

static int vloop_emit_slots(VloopKernel *k) {
  BinaryCodeBuffer *b = k->b;
  int c = 0;
  int s = 0;
  for (c = 0; c < k->n_consts; c++) {
    uint64_t bits = vloop_const_bits(k, &k->args[k->consts_off + c]);
    if (!binary_emit_mov_reg_imm64(b, BINARY_GP_RAX, bits) ||
        !wcs_vex_movq_xmm_reg(b, BINARY_XMM0, BINARY_GP_RAX) ||
        !wcs_avx_vbroadcastsd_ymm_xmm(b, 0, 0) ||
        !wcs_avx_vmovups_mem_ymm(b, BINARY_GP_RSP, 32 * c, 0)) {
      return 0;
    }
  }
  for (s = 0; s < k->n_scalars; s++) {
    if (!code_generator_binary_emit_operand_load(
            k->generator, k->context, &k->args[k->scalars_off + s],
            BINARY_GP_RAX)) {
      return 0;
    }
    if (k->f32) {
      if (!wcs_broadcast_i32_to_ymm(b, 0, BINARY_GP_RAX)) {
        return 0;
      }
    } else if (!wcs_vex_movq_xmm_reg(b, BINARY_XMM0, BINARY_GP_RAX) ||
               !wcs_avx_vbroadcastsd_ymm_xmm(b, 0, 0)) {
      return 0;
    }
    if (!wcs_avx_vmovups_mem_ymm(b, BINARY_GP_RSP, 32 * (k->n_consts + s),
                                 0)) {
      return 0;
    }
  }
  return 1;
}

static int vloop_emit_iota_vector(VloopKernel *k) {
  BinaryCodeBuffer *b = k->b;
  if (!k->has_iota) {
    return 1;
  }
  if (k->f32) {
    return binary_emit_mov_reg_imm64(b, BINARY_GP_RAX,
                                     0x0000000100000000ULL) &&
           wcs_vex_movq_xmm_reg(b, BINARY_XMM0, BINARY_GP_RAX) &&
           binary_emit_mov_reg_imm64(b, BINARY_GP_RAX,
                                     0x0000000300000002ULL) &&
           wcs_vex_movq_xmm_reg(b, BINARY_XMM2, BINARY_GP_RAX) &&
           wcs_avx_vpunpcklqdq_xmm(b, 0, 0, 2) &&
           binary_emit_mov_reg_imm64(b, BINARY_GP_RAX,
                                     0x0000000500000004ULL) &&
           wcs_vex_movq_xmm_reg(b, BINARY_XMM1, BINARY_GP_RAX) &&
           binary_emit_mov_reg_imm64(b, BINARY_GP_RAX,
                                     0x0000000700000006ULL) &&
           wcs_vex_movq_xmm_reg(b, BINARY_XMM2, BINARY_GP_RAX) &&
           wcs_avx_vpunpcklqdq_xmm(b, 1, 1, 2) &&
           wcs_avx_vperm2i128(b, VLOOP_IOTA_CONST, 0, 1, 0x20);
  }
  return binary_emit_mov_reg_imm64(b, BINARY_GP_RAX, 0x0000000100000000ULL) &&
         wcs_vex_movq_xmm_reg(b, BINARY_XMM0, BINARY_GP_RAX) &&
         binary_emit_mov_reg_imm64(b, BINARY_GP_RAX, 0x0000000300000002ULL) &&
         wcs_vex_movq_xmm_reg(b, BINARY_XMM1, BINARY_GP_RAX) &&
         wcs_avx_vpunpcklqdq_xmm(b, VLOOP_IOTA_CONST, 0, 1);
}

static int vloop_emit_accumulator(VloopKernel *k) {
  BinaryCodeBuffer *b = k->b;
  if (k->elem8 && !k->is_reduce &&
      (!binary_emit_mov_reg_imm64(b, BINARY_GP_RAX, 0x000000FF000000FFULL) ||
       !wcs_vex_movq_xmm_reg(b, BINARY_XMM0, BINARY_GP_RAX) ||
       !wcs_avx_vbroadcastsd_ymm_xmm(b, VLOOP_BYTE_MASK, 0))) {
    return 0;
  }
  if (k->is_minmax) {
    if (!code_generator_binary_emit_operand_load(k->generator, k->context,
                                                 &k->instruction->dest,
                                                 BINARY_GP_RAX)) {
      return 0;
    }
    if (k->f32) {
      return wcs_broadcast_i32_to_ymm(b, 2, BINARY_GP_RAX);
    }
    return wcs_vex_movq_xmm_reg(b, BINARY_XMM2, BINARY_GP_RAX) &&
           wcs_avx_vbroadcastsd_ymm_xmm(b, 2, 2);
  }
  if (!k->is_reduce) {
    return 1;
  }
  if (k->i32) {
    return wcs_avx_vpxor_ymm(b, 2, 2, 2);
  }
  return code_generator_binary_emit_operand_load(k->generator, k->context,
                                                 &k->instruction->dest,
                                                 BINARY_GP_RAX) &&
         (k->f32 ? wcs_vex_movd_xmm_reg(b, BINARY_XMM3, BINARY_GP_RAX)
                 : wcs_vex_movq_xmm_reg(b, BINARY_XMM3, BINARY_GP_RAX)) &&
         wcs_avx_vpxor_ymm(b, 2, 2, 2);
}

static int vloop_emit_prologue(VloopKernel *k) {
  if (!vloop_emit_bases(k)) {
    return 0;
  }
  if (k->has_iota && !binary_emit_mov_reg_imm64(k->b, BINARY_GP_R11, 0)) {
    return 0;
  }
  return vloop_emit_slots(k) && vloop_emit_iota_vector(k) &&
         vloop_emit_accumulator(k);
}

static int vloop_emit_overlap_guards(VloopKernel *k, size_t *j_overlap,
                                     int *n_overlap) {
  BinaryCodeBuffer *b = k->b;
  int shift = 0;
  int j = 0;
  if (k->is_reduce) {
    return 1;
  }
  while ((1 << shift) < k->elem_bytes) {
    shift++;
  }
  for (j = 1; j < k->n_dist; j++) {
    for (int dir = 0; dir < 2; dir++) {
      int lo = dir ? (int)VLOOP_GP[j] : (int)k->dst_reg;
      int hi = dir ? (int)k->dst_reg : (int)VLOOP_GP[j];
      if (!binary_emit_mov_reg_reg(b, BINARY_GP_RAX, (BinaryGpRegister)hi) ||
          !wcs_sub_reg_reg64(b, BINARY_GP_RAX, lo)) {
        return 0;
      }
      if (shift &&
          !wcs_shift_reg_imm(b, BINARY_GP_RAX, 1, (unsigned char)shift)) {
        return 0;
      }
      if (!wcs_cmp_reg_reg64(b, BINARY_GP_RAX, BINARY_GP_R10) ||
          !wcs_jcc(b, 0x82, &j_overlap[(*n_overlap)++])) {
        return 0;
      }
    }
  }
  return 1;
}

static void vloop_stack_reset(const VloopKernel *k, VloopStack *st) {
  int i = 0;
  st->nfree = k->pool_n;
  st->nv = 0;
  for (i = 0; i < k->pool_n; i++) {
    st->pool[i] = k->pool_src[i];
  }
}

static int vloop_shift_emit(VloopKernel *k, int ra, int tag, int op1) {
  BinaryCodeBuffer *b = k->b;
  if (tag == VLOOP_K_SHL) {
    return wcs_avx_vpslld_ymm_imm(b, ra, ra, (unsigned char)op1);
  }
  if (tag == VLOOP_K_SAR) {
    return wcs_avx_vpsrad_ymm_imm(b, ra, ra, (unsigned char)op1);
  }
  return wcs_avx_vpsrld_ymm_imm(b, ra, ra, (unsigned char)op1);
}

static int vloop_select_emit(VloopKernel *k, VloopStack *st) {
  BinaryCodeBuffer *b = k->b;
  int relse = st->vstk[--st->nv];
  int rthen = st->vstk[--st->nv];
  int rmask = st->vstk[--st->nv];
  if (!wcs_avx_vpxor_ymm(b, rthen, rthen, relse) ||
      !wcs_avx_vpand_ymm(b, rthen, rthen, rmask) ||
      !wcs_avx_vpxor_ymm(b, rthen, rthen, relse)) {
    return 0;
  }
  st->pool[st->nfree++] = relse;
  st->pool[st->nfree++] = rmask;
  st->vstk[st->nv++] = rthen;
  return 1;
}

static int vloop_tag_is_shift(int tag) {
  return tag == VLOOP_K_SHL || tag == VLOOP_K_SAR || tag == VLOOP_K_SHR;
}

static int vloop_vec_leaf(VloopKernel *k, VloopStack *st, int tag, int op0) {
  BinaryCodeBuffer *b = k->b;
  int R = 0;
  int ok = 0;
  if (st->nfree <= 0) {
    code_generator_set_error(k->generator, "vloop reg budget");
    return 0;
  }
  R = st->pool[--st->nfree];
  if (tag == VLOOP_K_LOAD) {
    ok = k->elem8
             ? (k->elem8_unsigned
                    ? wcs_avx_vpmovzxbd_ymm_mem(b, R, k->arr_reg[op0], 0)
                    : wcs_avx_vpmovsxbd_ymm_mem(b, R, k->arr_reg[op0], 0))
             : wcs_avx_vmovups_ymm_mem(b, R, k->arr_reg[op0], 0);
  } else if (tag == VLOOP_K_CONST) {
    ok = wcs_avx_vmovups_ymm_mem(b, R, BINARY_GP_RSP, 32 * op0);
  } else if (tag == VLOOP_K_SCALAR) {
    ok = wcs_avx_vmovups_ymm_mem(b, R, BINARY_GP_RSP,
                                 32 * (k->n_consts + op0));
  } else {
    ok = wcs_broadcast_i32_to_ymm(b, R, BINARY_GP_R11) &&
         wcs_avx_vpaddd_ymm(b, R, R, VLOOP_IOTA_CONST) &&
         (k->i32 ? 1
                 : (k->f32 ? wcs_avx_vcvtdq2ps_ymm(b, R, R)
                           : wcs_avx_vcvtdq2pd_ymm_xmm(b, R, R)));
  }
  if (!ok) {
    return 0;
  }
  st->vstk[st->nv++] = R;
  return 1;
}

static int vloop_vec_binop(VloopKernel *k, int tag, int ra, int rb) {
  BinaryCodeBuffer *b = k->b;
  switch (tag) {
  case VLOOP_K_ADD:
    return k->i32 ? wcs_avx_vpaddd_ymm(b, ra, ra, rb)
                  : (k->f32 ? wcs_avx_vaddps_ymm(b, ra, ra, rb)
                            : wcs_avx_vaddpd_ymm(b, ra, ra, rb));
  case VLOOP_K_SUB:
    return k->i32 ? wcs_avx_vpsubd_ymm(b, ra, ra, rb)
                  : (k->f32 ? wcs_avx_vsubps_ymm(b, ra, ra, rb)
                            : wcs_avx_vsubpd_ymm(b, ra, ra, rb));
  case VLOOP_K_MUL:
    return k->i32 ? wcs_avx_vpmulld_ymm(b, ra, ra, rb)
                  : (k->f32 ? wcs_avx_vmulps_ymm(b, ra, ra, rb)
                            : wcs_avx_vmulpd_ymm(b, ra, ra, rb));
  case VLOOP_K_DIV:
    if (k->i32) {
      code_generator_set_error(k->generator, "vloop int div");
      return 0;
    }
    return k->f32 ? wcs_avx_vdivps_ymm(b, ra, ra, rb)
                  : wcs_avx_vdivpd_ymm(b, ra, ra, rb);
  case VLOOP_K_AND:
  case VLOOP_K_OR:
  case VLOOP_K_XOR:
    if (!k->i32) {
      code_generator_set_error(k->generator, "vloop float bitop");
      return 0;
    }
    return tag == VLOOP_K_AND
               ? wcs_avx_vpand_ymm(b, ra, ra, rb)
               : (tag == VLOOP_K_OR ? wcs_avx_vpor_ymm(b, ra, ra, rb)
                                    : wcs_avx_vpxor_ymm(b, ra, ra, rb));
  case VLOOP_K_MIN:
  case VLOOP_K_MAX:
  case VLOOP_K_CMPGT:
  case VLOOP_K_CMPEQ:
    if (!k->i32) {
      code_generator_set_error(k->generator, "vloop float compare");
      return 0;
    }
    return tag == VLOOP_K_MIN
               ? wcs_avx_vpminsd_ymm(b, ra, ra, rb)
               : tag == VLOOP_K_MAX
                     ? wcs_avx_vpmaxsd_ymm(b, ra, ra, rb)
                     : tag == VLOOP_K_CMPGT
                           ? wcs_avx_vpcmpgtd_ymm(b, ra, ra, rb)
                           : wcs_avx_vpcmpeqd_ymm(b, ra, ra, rb);
  default:
    code_generator_set_error(k->generator, "vloop op");
    return 0;
  }
}

static int vloop_vec_node(VloopKernel *k, VloopStack *st, int index) {
  const IROperand *node = &k->args[k->nodes_off + 3 * index];
  int tag = (int)node[0].int_value;
  int ra = 0;
  int rb = 0;
  if (vloop_kernel_tag_is_leaf(tag)) {
    return vloop_vec_leaf(k, st, tag, (int)node[1].int_value);
  }
  if (vloop_tag_is_shift(tag)) {
    if (st->nv < 1 || !k->i32) {
      code_generator_set_error(k->generator, "vloop shift");
      return 0;
    }
    return vloop_shift_emit(k, st->vstk[st->nv - 1], tag,
                            (int)node[2].int_value);
  }
  if (tag == VLOOP_K_PAIR) {
    if (st->nv < 2) {
      code_generator_set_error(k->generator, "vloop pair");
      return 0;
    }
    return 1;
  }
  if (tag == VLOOP_K_SELECT) {
    if (st->nv < 3 || !k->i32) {
      code_generator_set_error(k->generator, "vloop select");
      return 0;
    }
    return vloop_select_emit(k, st);
  }
  if (st->nv < 2) {
    code_generator_set_error(k->generator, "vloop stack");
    return 0;
  }
  rb = st->vstk[--st->nv];
  ra = st->vstk[--st->nv];
  if (!vloop_vec_binop(k, tag, ra, rb)) {
    return 0;
  }
  st->pool[st->nfree++] = rb;
  st->vstk[st->nv++] = ra;
  return 1;
}

static int vloop_vec_store(VloopKernel *k, int R) {
  BinaryCodeBuffer *b = k->b;
  if (k->is_minmax) {
    return k->i32
               ? (k->is_max ? wcs_avx_vpmaxsd_ymm(b, 2, R, 2)
                            : wcs_avx_vpminsd_ymm(b, 2, R, 2))
               : (k->f32 ? (k->is_max ? wcs_avx_vmaxps_ymm(b, 2, R, 2)
                                      : wcs_avx_vminps_ymm(b, 2, R, 2))
                         : (k->is_max ? wcs_avx_vmaxpd_ymm(b, 2, R, 2)
                                      : wcs_avx_vminpd_ymm(b, 2, R, 2)));
  }
  if (k->is_reduce) {
    return k->i32 ? wcs_avx_vpaddd_ymm(b, 2, 2, R)
                  : (k->f32 ? wcs_avx_vaddps_ymm(b, 2, 2, R)
                            : wcs_avx_vaddpd_ymm(b, 2, 2, R));
  }
  if (k->elem8) {
    return wcs_avx_vpand_ymm(b, R, R, VLOOP_BYTE_MASK) &&
           wcs_avx_vpackusdw_ymm(b, R, R, R) &&
           wcs_avx_vpermq_ymm(b, R, R, 0x08) &&
           wcs_avx_vpackuswb_ymm(b, R, R, R) &&
           wcs_vex_movsd_mem_xmm(b, k->dst_reg, 0, R);
  }
  return wcs_avx_vmovups_mem_ymm(b, k->dst_reg, 0, R);
}

static int vloop_emit_vector_body(VloopKernel *k) {
  VloopStack st;
  int i = 0;
  vloop_stack_reset(k, &st);
  for (i = 0; i < k->n_nodes; i++) {
    if (!vloop_vec_node(k, &st, i)) {
      return 0;
    }
  }
  if (st.nv != 1) {
    code_generator_set_error(k->generator, "vloop root");
    return 0;
  }
  return vloop_vec_store(k, st.vstk[0]);
}

static int vloop_tail_leaf(VloopKernel *k, VloopStack *st, int tag, int op0) {
  BinaryCodeBuffer *b = k->b;
  int R = st->pool[--st->nfree];
  int ok = 0;
  if (tag == VLOOP_K_LOAD && k->elem8) {
    ok = (k->elem8_unsigned
              ? binary_emit_movzx_reg_mem8(
                    b, BINARY_GP_RAX, (BinaryGpRegister)k->arr_reg[op0], 0)
              : binary_emit_movsx_reg_mem8(
                    b, BINARY_GP_RAX, (BinaryGpRegister)k->arr_reg[op0], 0)) &&
         wcs_avx_vmovd_xmm_reg(b, R, BINARY_GP_RAX);
  } else if (tag == VLOOP_K_LOAD) {
    ok = k->i32 ? wcs_avx_vmovd_xmm_mem(b, R, k->arr_reg[op0], 0)
                : (k->f32 ? wcs_vex_movss_xmm_mem(b, R, k->arr_reg[op0], 0)
                          : wcs_vex_movsd_xmm_mem(b, R, k->arr_reg[op0], 0));
  } else if (tag == VLOOP_K_CONST || tag == VLOOP_K_SCALAR) {
    int disp = 32 * (tag == VLOOP_K_CONST ? op0 : k->n_consts + op0);
    ok = k->i32 ? wcs_avx_vmovd_xmm_mem(b, R, BINARY_GP_RSP, disp)
                : (k->f32 ? wcs_vex_movss_xmm_mem(b, R, BINARY_GP_RSP, disp)
                          : wcs_vex_movsd_xmm_mem(b, R, BINARY_GP_RSP, disp));
  } else {
    ok = k->i32 ? wcs_avx_vmovd_xmm_reg(b, R, BINARY_GP_R11)
                : (k->f32 ? wcs_vex_cvtsi2ss_xmm_reg(
                                b, (BinaryXmmRegister)R, BINARY_GP_R11)
                          : wcs_vex_cvtsi2sd_xmm_reg(
                                b, (BinaryXmmRegister)R, BINARY_GP_R11));
  }
  if (!ok) {
    return 0;
  }
  st->vstk[st->nv++] = R;
  return 1;
}

static int vloop_tail_int_binop(VloopKernel *k, int tag, int ra, int rb) {
  BinaryCodeBuffer *b = k->b;
  switch (tag) {
  case VLOOP_K_ADD: return wcs_avx_vpaddd_ymm(b, ra, ra, rb);
  case VLOOP_K_SUB: return wcs_avx_vpsubd_ymm(b, ra, ra, rb);
  case VLOOP_K_MUL: return wcs_avx_vpmulld_ymm(b, ra, ra, rb);
  case VLOOP_K_AND: return wcs_avx_vpand_ymm(b, ra, ra, rb);
  case VLOOP_K_OR: return wcs_avx_vpor_ymm(b, ra, ra, rb);
  case VLOOP_K_XOR: return wcs_avx_vpxor_ymm(b, ra, ra, rb);
  case VLOOP_K_MIN: return wcs_avx_vpminsd_ymm(b, ra, ra, rb);
  case VLOOP_K_MAX: return wcs_avx_vpmaxsd_ymm(b, ra, ra, rb);
  case VLOOP_K_CMPGT: return wcs_avx_vpcmpgtd_ymm(b, ra, ra, rb);
  case VLOOP_K_CMPEQ: return wcs_avx_vpcmpeqd_ymm(b, ra, ra, rb);
  default: return 0;
  }
}

static int vloop_tail_float_binop(VloopKernel *k, int tag, int ra, int rb) {
  BinaryCodeBuffer *b = k->b;
  BinaryXmmRegister A = (BinaryXmmRegister)ra;
  BinaryXmmRegister B = (BinaryXmmRegister)rb;
  switch (tag) {
  case VLOOP_K_ADD:
    return k->f32 ? wcs_vex_addss_xmm_xmm(b, A, B)
                  : wcs_vex_addsd_xmm_xmm(b, A, B);
  case VLOOP_K_SUB:
    return k->f32 ? wcs_vex_subss_xmm_xmm(b, A, B)
                  : wcs_vex_subsd_xmm_xmm(b, A, B);
  case VLOOP_K_MUL:
    return k->f32 ? wcs_vex_mulss_xmm_xmm(b, A, B)
                  : wcs_vex_mulsd_xmm_xmm(b, A, B);
  case VLOOP_K_DIV:
    return k->f32 ? wcs_vex_divss_xmm_xmm(b, A, B)
                  : wcs_vex_divsd_xmm_xmm(b, A, B);
  default: return 0;
  }
}

static int vloop_tail_node(VloopKernel *k, VloopStack *st, int index) {
  const IROperand *node = &k->args[k->nodes_off + 3 * index];
  int tag = (int)node[0].int_value;
  int ra = 0;
  int rb = 0;
  if (vloop_kernel_tag_is_leaf(tag)) {
    return vloop_tail_leaf(k, st, tag, (int)node[1].int_value);
  }
  if (vloop_tag_is_shift(tag)) {
    return k->i32 && vloop_shift_emit(k, st->vstk[st->nv - 1], tag,
                                      (int)node[2].int_value);
  }
  if (tag == VLOOP_K_PAIR) {
    return st->nv >= 2;
  }
  if (tag == VLOOP_K_SELECT) {
    if (st->nv < 3 || !k->i32) {
      return 0;
    }
    return vloop_select_emit(k, st);
  }
  rb = st->vstk[--st->nv];
  ra = st->vstk[--st->nv];
  if (!(k->i32 ? vloop_tail_int_binop(k, tag, ra, rb)
               : vloop_tail_float_binop(k, tag, ra, rb))) {
    return 0;
  }
  st->pool[st->nfree++] = rb;
  st->vstk[st->nv++] = ra;
  return 1;
}

static int vloop_tail_store(VloopKernel *k, int R) {
  BinaryCodeBuffer *b = k->b;
  if (k->is_minmax) {
    if (!(k->f32 ? wcs_avx_vpbroadcastd_ymm(b, R, R)
                 : wcs_avx_vbroadcastsd_ymm_xmm(b, R, R))) {
      return 0;
    }
    return k->i32
               ? (k->is_max ? wcs_avx_vpmaxsd_ymm(b, 2, R, 2)
                            : wcs_avx_vpminsd_ymm(b, 2, R, 2))
               : (k->f32 ? (k->is_max ? wcs_avx_vmaxps_ymm(b, 2, R, 2)
                                      : wcs_avx_vminps_ymm(b, 2, R, 2))
                         : (k->is_max ? wcs_avx_vmaxpd_ymm(b, 2, R, 2)
                                      : wcs_avx_vminpd_ymm(b, 2, R, 2)));
  }
  if (k->is_reduce) {
    if (k->i32) {
      return wcs_avx_vpaddd_ymm(b, 2, 2, R);
    }
    return k->f32
               ? wcs_vex_addss_xmm_xmm(b, BINARY_XMM3, (BinaryXmmRegister)R)
               : wcs_vex_addsd_xmm_xmm(b, BINARY_XMM3,
                                           (BinaryXmmRegister)R);
  }
  if (k->elem8) {
    return wcs_avx_vpextrb_mem_xmm(b, k->dst_reg, 0, R);
  }
  return k->i32 ? wcs_avx_vmovd_mem_xmm(b, k->dst_reg, 0, R)
                : (k->f32 ? wcs_vex_movss_mem_xmm(b, k->dst_reg, 0, R)
                          : wcs_vex_movsd_mem_xmm(b, k->dst_reg, 0, R));
}

static int vloop_emit_tail_body(VloopKernel *k) {
  VloopStack st;
  int i = 0;
  vloop_stack_reset(k, &st);
  for (i = 0; i < k->n_nodes; i++) {
    if (!vloop_tail_node(k, &st, i)) {
      return 0;
    }
  }
  return vloop_tail_store(k, st.vstk[0]);
}

static int vloop_emit_advance(VloopKernel *k, int stride, int step) {
  BinaryCodeBuffer *b = k->b;
  int j = 0;
  for (j = 0; j < k->n_dist; j++) {
    if (!wcs_addsub_reg_imm8(b, VLOOP_GP[j], 0, stride)) {
      return 0;
    }
  }
  if (k->has_iota && !wcs_addsub_reg_imm8(b, BINARY_GP_R11, 0, step)) {
    return 0;
  }
  return wcs_addsub_reg_imm8(b, BINARY_GP_R10, 1, step);
}

static int vloop_emit_reduce_result(VloopKernel *k) {
  BinaryCodeBuffer *b = k->b;
  if (!k->is_minmax && k->i32 &&
      !code_generator_binary_emit_operand_load(k->generator, k->context,
                                               &k->instruction->dest,
                                               BINARY_GP_RAX)) {
    return 0;
  }
  if (k->is_minmax) {
    if (!(k->i32 ? wcs_reduce_ymm_i32_minmax_to_rax(b, k->is_max)
                 : (k->f32 ? wcs_reduce_ps_minmax_to_rax(b, k->is_max)
                           : wcs_reduce_pd_minmax_to_rax(b, k->is_max)))) {
      return 0;
    }
  } else if (!(k->i32 ? wcs_reduce_ymm_i32_sum_to_rax(b, 2)
                      : (k->f32 ? wcs_reduce_ps_acc_to_rax(b)
                                : wcs_reduce_pd_acc_to_rax(b)))) {
    return 0;
  }
  if (k->i32 &&
      !(k->instruction->is_unsigned
            ? wcs_mov_reg_reg32(b, BINARY_GP_RAX, BINARY_GP_RAX)
            : binary_emit_movsxd_reg_reg32(b, BINARY_GP_RAX,
                                           BINARY_GP_RAX))) {
    return 0;
  }
  if (k->cbytes && !binary_emit_add_rsp_imm32(b, k->cbytes)) {
    return 0;
  }
  return code_generator_binary_emit_destination_store(
      k->generator, k->context, &k->instruction->dest, BINARY_GP_RAX);
}

int code_generator_binary_emit_simd_vloop_f64(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction, int operands_marshaled) {
  VloopKernel k = {0};
  BinaryCodeBuffer *b = NULL;
  size_t j_overlap[2 * VLOOP_KERNEL_MAX_BASES];
  size_t vec_top = 0;
  size_t tail_top = 0;
  size_t j_tail = 0;
  size_t j_done = 0;
  size_t j_back = 0;
  int n_overlap = 0;
  int j = 0;

  if (!vloop_decode(&k, generator, context, instruction, operands_marshaled)) {
    return 0;
  }
  b = k.b;
  if (!vloop_emit_prologue(&k) ||
      !vloop_emit_overlap_guards(&k, j_overlap, &n_overlap)) {
    return 0;
  }

  vec_top = b->size;
  if (!wcs_cmp_reg_imm8(b, BINARY_GP_R10, k.lanes) ||
      !wcs_jcc(b, 0x8C, &j_tail) || !vloop_emit_vector_body(&k) ||
      !vloop_emit_advance(&k, k.vec_stride, k.lanes) ||
      !wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, vec_top) ||
      !wcs_patch_here(b, j_tail)) {
    return 0;
  }
  for (j = 0; j < n_overlap; j++) {
    if (!wcs_patch_here(b, j_overlap[j])) {
      return 0;
    }
  }

  tail_top = b->size;
  j_back = 0;
  if (!wcs_cmp_reg_imm8(b, BINARY_GP_R10, 0) || !wcs_jcc(b, 0x8E, &j_done) ||
      !vloop_emit_tail_body(&k) || !vloop_emit_advance(&k, k.elem_bytes, 1) ||
      !wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, tail_top) ||
      !wcs_patch_here(b, j_done)) {
    return 0;
  }

  if (k.is_reduce) {
    return vloop_emit_reduce_result(&k);
  }
  if (!wcs_avx_vzeroupper(b)) {
    return 0;
  }
  if (k.cbytes && !binary_emit_add_rsp_imm32(b, k.cbytes)) {
    return 0;
  }
  return 1;
}

#define OLK_U_AND 1
#define OLK_U_OR 2
#define OLK_U_XOR 3
#define OLK_U_ADD 4
#define OLK_U_SUB 5
#define OLK_U_MUL 6
#define OLK_U_SHL 7
#define OLK_U_SHR 8
#define OLK_U_CVT 9
#define OLK_U_FADD 10
#define OLK_U_FSUB 11
#define OLK_U_FMUL 12
#define OLK_U_FDIV 13
#define OLK_C_ADD 0
#define OLK_C_SUB 1
#define OLK_C_MUL 2
#define OLK_C_DIV 3

static int ol_emit_uniform_prog(CodeGenerator *gen, BinaryCodeBuffer *b,
                                const IROperand *args, size_t prog_off,
                                int n_micro, int n_fconst,
                                BinaryGpRegister val_gpr, BinaryGpRegister work_gpr,
                                int res_xmm, int ctmp_xmm) {
  if (!binary_emit_mov_reg_reg(b, work_gpr, val_gpr)) {
    return 0;
  }
  int in_float = 0;
  for (int m = 0; m < n_micro; m++) {
    int mop = (int)args[prog_off + 2 * m].int_value;
    long long mimm = args[prog_off + 2 * m + 1].int_value;
    int ok = 1;
    switch (mop) {
    case OLK_U_AND:
    case OLK_U_OR:
    case OLK_U_XOR:
    case OLK_U_ADD:
    case OLK_U_SUB:
    case OLK_U_MUL: {
      unsigned char alu = 0;
      if (mop == OLK_U_ADD) alu = 0x01;
      else if (mop == OLK_U_SUB) alu = 0x29;
      else if (mop == OLK_U_AND) alu = 0x21;
      else if (mop == OLK_U_OR) alu = 0x09;
      else if (mop == OLK_U_XOR) alu = 0x31;
      ok = binary_emit_mov_reg_imm64(b, BINARY_GP_RAX, (uint64_t)mimm);
      if (ok && mop == OLK_U_MUL) {
        ok = binary_emit_imul_reg_reg(b, work_gpr, BINARY_GP_RAX);
      } else if (ok) {
        ok = binary_emit_alu_reg_reg(b, alu, work_gpr, BINARY_GP_RAX);
      }
      break;
    }
    case OLK_U_SHL: ok = wcs_shift_reg_imm(b, work_gpr, 0, (unsigned char)mimm); break;
    case OLK_U_SHR: ok = wcs_shift_reg_imm(b, work_gpr, 1, (unsigned char)mimm); break;
    case OLK_U_CVT:
      ok = wcs_avx_vcvtsi2sd(b, res_xmm, res_xmm, work_gpr);
      in_float = 1;
      break;
    case OLK_U_FADD:
    case OLK_U_FSUB:
    case OLK_U_FMUL:
    case OLK_U_FDIV: {
      if (mimm < 0 || mimm >= n_fconst) {
        code_generator_set_error(gen, "vloop outer: unif fconst idx");
        return 0;
      }
      ok = wcs_avx_vmovsd_xmm_mem(b, ctmp_xmm, BINARY_GP_RSP, (int)(32 * mimm));
      if (ok) {
        if (mop == OLK_U_FADD) ok = wcs_avx_vaddsd(b, res_xmm, res_xmm, ctmp_xmm);
        else if (mop == OLK_U_FSUB) ok = wcs_avx_vsubsd(b, res_xmm, res_xmm, ctmp_xmm);
        else if (mop == OLK_U_FMUL) ok = wcs_avx_vmulsd(b, res_xmm, res_xmm, ctmp_xmm);
        else ok = wcs_avx_vdivsd(b, res_xmm, res_xmm, ctmp_xmm);
      }
      break;
    }
    default:
      code_generator_set_error(gen, "vloop outer: bad micro op");
      return 0;
    }
    if (!ok) {
      return 0;
    }
  }
  if (!in_float) {
    code_generator_set_error(gen, "vloop outer: uniform never cast");
    return 0;
  }
  return 1;
}

int code_generator_binary_emit_simd_outer_lane_f64(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  BinaryCodeBuffer *b = NULL;
  if (!generator || !context || !instruction || instruction->argument_count < 8 ||
      !instruction->arguments || instruction->dest.kind != IR_OPERAND_SYMBOL) {
    code_generator_set_error(generator, "Malformed simd_outer_lane_f64");
    return 0;
  }
  b = &context->code;
  const IROperand *args = instruction->arguments;
  int inner_cmp = (int)args[0].int_value;
  long long istep = args[1].int_value;
  int n_chain = (int)args[2].int_value;
  int n_unif = (int)args[3].int_value;
  int n_fconst = (int)args[4].int_value;
  long long i0 = args[5].int_value;
  int init_mode = (int)args[6].int_value;
  double iacc_init = args[7].float_value;
  if (n_chain <= 0 || n_chain > 8 || n_unif < 0 || n_unif > 8 || n_fconst < 0 ||
      n_fconst > 16 || istep <= 0 || (init_mode != 0 && init_mode != 1)) {
    code_generator_set_error(generator, "Bad simd_outer_lane_f64 encoding");
    return 0;
  }

  const int K = 3;
  const int kAcc[3] = {0, 1, 2};
  const int GROUP = 4 * K;

  size_t chain_off = 8;
  size_t off = chain_off + (size_t)(4 * n_chain);
  size_t unif_start[8];
  int unif_nmicro[8];
  for (int u = 0; u < n_unif; u++) {
    if (off >= instruction->argument_count) {
      code_generator_set_error(generator, "vloop outer: uniform overrun");
      return 0;
    }
    int nm = (int)args[off].int_value;
    if (nm < 0 || nm > 16) {
      code_generator_set_error(generator, "vloop outer: bad micro count");
      return 0;
    }
    unif_nmicro[u] = nm;
    unif_start[u] = off + 1;
    off += 1 + (size_t)(2 * nm);
  }
  size_t init_start = 0;
  int init_nmicro = 0;
  if (init_mode == 1) {
    if (off >= instruction->argument_count) {
      code_generator_set_error(generator, "vloop outer: seed overrun");
      return 0;
    }
    init_nmicro = (int)args[off].int_value;
    if (init_nmicro < 0 || init_nmicro > 16) {
      code_generator_set_error(generator, "vloop outer: bad seed micro count");
      return 0;
    }
    init_start = off + 1;
    off += 1 + (size_t)(2 * init_nmicro);
  }
  size_t fconst_off = off;
  if (fconst_off + (size_t)n_fconst != instruction->argument_count) {
    code_generator_set_error(generator, "vloop outer: arg length mismatch");
    return 0;
  }

  uint64_t init_bits = 0;
  memcpy(&init_bits, &iacc_init, sizeof(init_bits));
  int total_off = 32 * n_fconst;
  int seedarr_off = total_off + 16;
  uint32_t cbytes =
      (uint32_t)(seedarr_off + (init_mode == 1 ? 32 * K : 0));
  if (!binary_emit_sub_rsp_imm32(b, cbytes)) {
    return 0;
  }
  if (!code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->dest,
                                               BINARY_GP_RAX) ||
      !wcs_vex_movq_xmm_reg(b, BINARY_XMM4, BINARY_GP_RAX) ||
      !wcs_vex_movsd_mem_xmm(b, BINARY_GP_RSP, total_off, 4) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->lhs,
                                               BINARY_GP_R9) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->rhs,
                                               BINARY_GP_R8)) {
    return 0;
  }
  for (int c = 0; c < n_fconst; c++) {
    double dv = args[fconst_off + c].float_value;
    uint64_t bits = 0;
    memcpy(&bits, &dv, sizeof(bits));
    if (!binary_emit_mov_reg_imm64(b, BINARY_GP_RAX, bits) ||
        !wcs_vex_movq_xmm_reg(b, BINARY_XMM0, BINARY_GP_RAX) ||
        !wcs_avx_vbroadcastsd_ymm_xmm(b, 0, 0) ||
        !wcs_avx_vmovups_mem_ymm(b, BINARY_GP_RSP, 32 * c, 0)) {
      return 0;
    }
  }
  if (!binary_emit_mov_reg_imm64(b, BINARY_GP_RDX, 0)) {
    return 0;
  }

  size_t outer_top = b->size;
  size_t j_outer_end = 0;
  if (!binary_emit_cmp_reg_reg(b, BINARY_GP_RDX, BINARY_GP_R9) ||
      !wcs_jcc(b, 0x8D , &j_outer_end)) {
    return 0;
  }
  if (init_mode == 0) {
    if (!binary_emit_mov_reg_imm64(b, BINARY_GP_RAX, init_bits) ||
        !wcs_vex_movq_xmm_reg(b, BINARY_XMM4, BINARY_GP_RAX)) {
      return 0;
    }
    for (int k = 0; k < K; k++) {
      if (!wcs_avx_vbroadcastsd_ymm_xmm(b, kAcc[k], 4)) {
        return 0;
      }
    }
  } else {
    for (int g = 0; g < GROUP; g++) {
      if (!binary_emit_mov_reg_reg(b, BINARY_GP_R11, BINARY_GP_RDX) ||
          !wcs_addsub_reg_imm8(b, BINARY_GP_R11, 0, (unsigned char)g) ||
          !ol_emit_uniform_prog(generator, b, args, init_start, init_nmicro,
                                n_fconst, BINARY_GP_R11, BINARY_GP_R10, 4, 5) ||
          !wcs_avx_vmovsd_mem_xmm(b, BINARY_GP_RSP, seedarr_off + 8 * g, 4)) {
        return 0;
      }
    }
    for (int k = 0; k < K; k++) {
      if (!wcs_avx_vmovups_ymm_mem(b, kAcc[k], BINARY_GP_RSP,
                                   seedarr_off + 32 * k)) {
        return 0;
      }
    }
  }
  if (!binary_emit_mov_reg_imm64(b, BINARY_GP_RCX, (uint64_t)i0)) {
    return 0;
  }

  size_t inner_top = b->size;
  size_t j_inner_end = 0;
  if (!binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R8) ||
      !wcs_jcc(b, inner_cmp ? 0x8F  : 0x8D , &j_inner_end)) {
    return 0;
  }
  for (int s = 0; s < n_chain; s++) {
    int c_op = (int)args[chain_off + 4 * s].int_value;
    int side = (int)args[chain_off + 4 * s + 1].int_value;
    int kind = (int)args[chain_off + 4 * s + 2].int_value;
    int idx = (int)args[chain_off + 4 * s + 3].int_value;
    if (kind == 0) {
      if (idx < 0 || idx >= n_fconst ||
          !wcs_avx_vmovups_ymm_mem(b, 3, BINARY_GP_RSP, 32 * idx)) {
        code_generator_set_error(generator, "vloop outer: const idx");
        return 0;
      }
    } else {
      if (idx < 0 || idx >= n_unif ||
          !ol_emit_uniform_prog(generator, b, args, unif_start[idx],
                                unif_nmicro[idx], n_fconst, BINARY_GP_RCX,
                                BINARY_GP_R10, 4, 5) ||
          !wcs_avx_vbroadcastsd_ymm_xmm(b, 3, 4)) {
        if (idx < 0 || idx >= n_unif) {
          code_generator_set_error(generator, "vloop outer: unif idx");
        }
        return 0;
      }
    }
    for (int k = 0; k < K; k++) {
      int a = kAcc[k];
      int ok = 0;
      if (c_op == OLK_C_ADD) ok = wcs_avx_vaddpd_ymm(b, a, a, 3);
      else if (c_op == OLK_C_MUL) ok = wcs_avx_vmulpd_ymm(b, a, a, 3);
      else if (c_op == OLK_C_SUB)
        ok = side ? wcs_avx_vsubpd_ymm(b, a, 3, a) : wcs_avx_vsubpd_ymm(b, a, a, 3);
      else if (c_op == OLK_C_DIV)
        ok = side ? wcs_avx_vdivpd_ymm(b, a, 3, a) : wcs_avx_vdivpd_ymm(b, a, a, 3);
      else { code_generator_set_error(generator, "vloop outer: chain op"); return 0; }
      if (!ok) {
        return 0;
      }
    }
  }
  if (!wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, (unsigned char)istep)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, inner_top)) {
      return 0;
    }
  }

  if (!wcs_patch_here(b, j_inner_end)) {
    return 0;
  }
  if (!wcs_avx_vmovsd_xmm_mem(b, 5, BINARY_GP_RSP, total_off)) {
    return 0;
  }
  if (init_mode == 0) {
    if (!binary_emit_mov_reg_reg(b, BINARY_GP_R11, BINARY_GP_R9) ||
        !binary_emit_alu_reg_reg(b, 0x29 , BINARY_GP_R11, BINARY_GP_RDX) ||
        !binary_emit_mov_reg_imm64(b, BINARY_GP_RAX, (uint64_t)GROUP) ||
        !wcs_cmp_reg_imm32(b, BINARY_GP_R11, (uint32_t)GROUP) ||
        !binary_emit_cmovcc_reg_reg(b, 0x4F , BINARY_GP_R11,
                                    BINARY_GP_RAX)) {
      return 0;
    }
    size_t add_top = b->size;
    if (!wcs_avx_vaddsd(b, 5, 5, 0) ||
        !wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 1) ||
        !wcs_addsub_reg_imm8(b, BINARY_GP_R11, 1, 1)) {
      return 0;
    }
    size_t j_add_back = 0;
    if (!wcs_jcc(b, 0x85 , &j_add_back) ||
        !wcs_patch_to(b, j_add_back, add_top)) {
      return 0;
    }
  } else {
    for (int g = 0; g < GROUP; g++) {
      int k = g / 4, j = g % 4;
      int acc = kAcc[k];
      size_t j_skip = 0;
      if (!binary_emit_mov_reg_reg(b, BINARY_GP_R11, BINARY_GP_RDX) ||
          !wcs_addsub_reg_imm8(b, BINARY_GP_R11, 0, (unsigned char)g) ||
          !binary_emit_cmp_reg_reg(b, BINARY_GP_R11, BINARY_GP_R9) ||
          !wcs_jcc(b, 0x8D , &j_skip)) {
        return 0;
      }
      int ok = 1;
      if (j == 0) {
        ok = wcs_avx_vaddsd(b, 5, 5, acc);
      } else if (j == 1) {
        ok = wcs_avx_vunpckhpd_xmm(b, 4, acc, acc) && wcs_avx_vaddsd(b, 5, 5, 4);
      } else if (j == 2) {
        ok = wcs_avx_vextractf128(b, 4, acc, 1) && wcs_avx_vaddsd(b, 5, 5, 4);
      } else {
        ok = wcs_avx_vextractf128(b, 4, acc, 1) &&
             wcs_avx_vunpckhpd_xmm(b, 4, 4, 4) && wcs_avx_vaddsd(b, 5, 5, 4);
      }
      if (!ok || !wcs_patch_here(b, j_skip)) {
        return 0;
      }
    }
    if (!wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, (unsigned char)GROUP)) {
      return 0;
    }
  }
  if (!wcs_avx_vmovsd_mem_xmm(b, BINARY_GP_RSP, total_off, 5)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, outer_top)) {
      return 0;
    }
  }

  if (!wcs_patch_here(b, j_outer_end) || !wcs_avx_vzeroupper(b)) {
    return 0;
  }
  if (!wcs_vex_movsd_xmm_mem(b, 0, BINARY_GP_RSP, total_off) ||
      !wcs_vex_movq_reg_xmm(b, BINARY_GP_RAX, BINARY_XMM0)) {
    return 0;
  }
  if (!binary_emit_add_rsp_imm32(b, cbytes)) {
    return 0;
  }
  return code_generator_binary_emit_destination_store(generator, context,
                                                      &instruction->dest,
                                                      BINARY_GP_RAX);
}

int code_generator_binary_emit_simd_i2f_reduce_f64(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  BinaryCodeBuffer *b = NULL;
  long long bound = 0;
  long long vec_end = 0;
  size_t nsteps = 0;
  size_t vec_top = 0;
  size_t rem_top = 0;
  size_t j_after_vec = 0;
  size_t j_done = 0;
  size_t back = 0;

  if (!generator || !context || !instruction || instruction->argument_count < 3 ||
      (instruction->argument_count % 2) == 0 || !instruction->arguments) {
    code_generator_set_error(generator, "Malformed simd_i2f_reduce_f64");
    return 0;
  }
  b = &context->code;
  bound = instruction->arguments[0].int_value;
  vec_end = bound & ~3LL;
  nsteps = (instruction->argument_count - 1) / 2;
  uint32_t const_bytes = (uint32_t)(32 * nsteps);

  if (!wcs_avx_vpxor_ymm(b, 2, 2, 2) || !wcs_avx_vpxor_ymm(b, 3, 3, 3) ||
      !binary_emit_mov_reg_imm64(b, BINARY_GP_RAX, 0x0000000100000000ULL) ||
      !wcs_vex_movq_xmm_reg(b, BINARY_XMM0, BINARY_GP_RAX) ||
      !binary_emit_mov_reg_imm64(b, BINARY_GP_RAX, 0x0000000300000002ULL) ||
      !wcs_vex_movq_xmm_reg(b, BINARY_XMM1, BINARY_GP_RAX) ||
      !wcs_avx_vpunpcklqdq_xmm(b, 5, 0, 1) ||
      !binary_emit_mov_reg_imm64(b, BINARY_GP_RCX, 0) ||
      !binary_emit_mov_reg_imm64(b, BINARY_GP_R8, (uint64_t)vec_end) ||
      !binary_emit_mov_reg_imm64(b, BINARY_GP_R9, (uint64_t)bound)) {
    return 0;
  }

  if (!binary_emit_sub_rsp_imm32(b, const_bytes) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_R11, BINARY_GP_RSP)) {
    return 0;
  }
  for (size_t s = 0; s < nsteps; s++) {
    double kd = instruction->arguments[2 + 2 * s].float_value;
    uint64_t kbits = 0;
    memcpy(&kbits, &kd, sizeof(kbits));
    if (!binary_emit_mov_reg_imm64(b, BINARY_GP_RAX, kbits) ||
        !wcs_vex_movq_xmm_reg(b, BINARY_XMM1, BINARY_GP_RAX) ||
        !wcs_avx_vbroadcastsd_ymm_xmm(b, 1, 1) ||
        !wcs_avx_vmovups_mem_ymm(b, BINARY_GP_R11, (int)(32 * s), 1)) {
      return 0;
    }
  }

  vec_top = b->size;
  if (!binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R8) ||
      !wcs_jcc(b, 0x8D , &j_after_vec) ||
      !wcs_broadcast_i32_to_ymm(b, 1, BINARY_GP_RCX) ||
      !wcs_avx_vpaddd_ymm(b, 1, 1, 5) ||
      !wcs_avx_vcvtdq2pd_ymm_xmm(b, 0, 1)) {
    return 0;
  }
  for (size_t s = 0; s < nsteps; s++) {
    int op = (int)instruction->arguments[1 + 2 * s].int_value;
    if (!wcs_avx_vmovups_ymm_mem(b, 1, BINARY_GP_R11, (int)(32 * s))) {
      return 0;
    }
    int ok = 0;
    switch (op) {
    case 0: ok = wcs_avx_vmulpd_ymm(b, 0, 0, 1); break;
    case 1: ok = wcs_avx_vaddpd_ymm(b, 0, 0, 1); break;
    case 2: ok = wcs_avx_vsubpd_ymm(b, 0, 0, 1); break;
    case 3: ok = wcs_avx_vsubpd_ymm(b, 0, 1, 0); break;
    case 4: ok = wcs_avx_vdivpd_ymm(b, 0, 0, 1); break;
    default: code_generator_set_error(generator, "bad i2f step op"); return 0;
    }
    if (!ok) {
      return 0;
    }
  }
  if (!wcs_avx_vcvttpd2dq_xmm_ymm(b, 1, 0) ||
      !wcs_avx_vcvtdq2pd_ymm_xmm(b, 0, 1) || !wcs_avx_vaddpd_ymm(b, 2, 2, 0) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 4) ||
      !wcs_jcc(b, 0, &back) || !wcs_patch_to(b, back, vec_top)) {
    return 0;
  }

  if (!wcs_patch_here(b, j_after_vec)) {
    return 0;
  }
  rem_top = b->size;
  if (!binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R9) ||
      !wcs_jcc(b, 0x8D , &j_done) ||
      !wcs_vex_cvtsi2sd_xmm_reg(b, BINARY_XMM0, BINARY_GP_RCX)) {
    return 0;
  }
  for (size_t s = 0; s < nsteps; s++) {
    int op = (int)instruction->arguments[1 + 2 * s].int_value;
    if (!wcs_vex_movsd_xmm_mem(b, 1, BINARY_GP_R11, (int)(32 * s))) {
      return 0;
    }
    int ok = 0;
    switch (op) {
    case 0: ok = wcs_vex_mulsd_xmm_xmm(b, BINARY_XMM0, BINARY_XMM1); break;
    case 1: ok = wcs_vex_addsd_xmm_xmm(b, BINARY_XMM0, BINARY_XMM1); break;
    case 2: ok = wcs_vex_subsd_xmm_xmm(b, BINARY_XMM0, BINARY_XMM1); break;
    case 3:
      ok = wcs_vex_subsd_xmm_xmm(b, BINARY_XMM1, BINARY_XMM0) &&
           wcs_vex_movsd_xmm_xmm(b, 0, 1);
      break;
    case 4: ok = wcs_vex_divsd_xmm_xmm(b, BINARY_XMM0, BINARY_XMM1); break;
    default: code_generator_set_error(generator, "bad i2f step op"); return 0;
    }
    if (!ok) {
      return 0;
    }
  }
  if (!wcs_vex_cvttsd2si_reg_xmm(b, BINARY_GP_RAX, BINARY_XMM0) ||
      !wcs_vex_cvtsi2sd_xmm_reg(b, BINARY_XMM0, BINARY_GP_RAX) ||
      !wcs_vex_addsd_xmm_xmm(b, BINARY_XMM3, BINARY_XMM0) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 1) ||
      !wcs_jcc(b, 0, &back) || !wcs_patch_to(b, back, rem_top)) {
    return 0;
  }

  if (!wcs_patch_here(b, j_done) ||
      !binary_emit_add_rsp_imm32(b, const_bytes)  ||
      !wcs_reduce_pd_acc_to_rax(b) ||
      !wcs_vex_movq_xmm_reg(b, BINARY_XMM0, BINARY_GP_RAX) ||
      !wcs_vex_cvttsd2si_reg_xmm(b, BINARY_GP_R10, BINARY_XMM0) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->dest,
                                               BINARY_GP_RAX) ||
      !wcs_add_reg_reg64(b, BINARY_GP_RAX, BINARY_GP_R10)) {
    return 0;
  }
  return code_generator_binary_emit_destination_store(generator, context,
                                                      &instruction->dest,
                                                      BINARY_GP_RAX);
}

int code_generator_binary_emit_simd_dot_f32(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  BinaryCodeBuffer *b = NULL;
  size_t loop_top = 0;
  size_t j_done = 0;
  size_t j_vec2 = 0;
  size_t j_vec = 0;
  size_t j_scalar = 0;

  if (!generator || !context || !instruction ||
      instruction->argument_count < 1 || !instruction->arguments) {
    code_generator_set_error(generator, "Malformed simd_dot_f32");
    return 0;
  }
  b = &context->code;

  if (!code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->dest,
                                               BINARY_GP_RAX) ||
      !wcs_vex_movd_xmm_reg(b, BINARY_XMM3, BINARY_GP_RAX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->lhs,
                                               BINARY_GP_RCX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->rhs,
                                               BINARY_GP_RDX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->arguments[0],
                                               BINARY_GP_R8) ||
      !simd_clamped_count(b) ||
      !binary_emit_shift_reg_imm8(b, 4, BINARY_GP_R9, 2) ||
      !wcs_add_reg_reg64(b, BINARY_GP_R9, BINARY_GP_RCX) ||
      !wcs_avx_vpxor_ymm(b, 2, 2, 2) ||
      !wcs_avx_vpxor_ymm(b, 4, 4, 4)) {
    return 0;
  }

  loop_top = b->size;
  if (!binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R9) ||
      !wcs_jcc(b, 0x83 , &j_done) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_R10, BINARY_GP_R9) ||
      !binary_emit_alu_reg_reg(b, 0x29, BINARY_GP_R10, BINARY_GP_RCX) ||
      !wcs_cmp_reg_imm32(b, BINARY_GP_R10, 64) ||
      !wcs_jcc(b, 0x83 , &j_vec2) ||
      !wcs_cmp_reg_imm32(b, BINARY_GP_R10, 32) ||
      !wcs_jcc(b, 0x83 , &j_vec) ||
      !wcs_jcc(b, 0, &j_scalar)) {
    return 0;
  }

  if (!wcs_patch_here(b, j_vec2) ||
      !wcs_avx_vmovups_ymm_mem(b, 0, BINARY_GP_RCX, 0) ||
      !wcs_avx_vmovups_ymm_mem(b, 1, BINARY_GP_RDX, 0) ||
      !wcs_avx_vfmadd231ps_ymm(b, 2, 0, 1) ||
      !wcs_avx_vmovups_ymm_mem(b, 0, BINARY_GP_RCX, 32) ||
      !wcs_avx_vmovups_ymm_mem(b, 1, BINARY_GP_RDX, 32) ||
      !wcs_avx_vfmadd231ps_ymm(b, 4, 0, 1) ||
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

  if (!wcs_patch_here(b, j_vec) ||
      !wcs_avx_vmovups_ymm_mem(b, 0, BINARY_GP_RCX, 0) ||
      !wcs_avx_vmovups_ymm_mem(b, 1, BINARY_GP_RDX, 0) ||
      !wcs_avx_vfmadd231ps_ymm(b, 2, 0, 1) ||
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
      !wcs_vex_movss_xmm_mem(b, 0, BINARY_GP_RCX, 0) ||
      !wcs_vex_movss_xmm_mem(b, 1, BINARY_GP_RDX, 0) ||
      !wcs_fmadd231ss(b, 3, 0, 1) ||
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
      !wcs_avx_vaddps_ymm(b, 2, 2, 4) ||
      !wcs_reduce_ps_acc_to_rax(b)) {
    return 0;
  }
  return code_generator_binary_emit_destination_store(generator, context,
                                                      &instruction->dest,
                                                      BINARY_GP_RAX);
}

#define MULTIDOT_SAVE_BYTES 48
#define MULTIDOT_COUNT_DISP 0
#define MULTIDOT_SUMS_DISP 16

static const BinaryGpRegister kMultidotBases[6] = {
    BINARY_GP_RCX, BINARY_GP_RDX, BINARY_GP_R8,
    BINARY_GP_R9,  BINARY_GP_R10, BINARY_GP_R11};

static int multidot_rsp_adjust(BinaryCodeBuffer *b, int sub) {
  return binary_code_buffer_append_u8(b, 0x48) &&
         binary_code_buffer_append_u8(b, 0x81) &&
         binary_code_buffer_append_u8(b, sub ? 0xEC : 0xC4) &&
         binary_code_buffer_append_u32(b, MULTIDOT_SAVE_BYTES);
}

static int multidot_and_r12_imm8(BinaryCodeBuffer *b, signed char mask) {
  return binary_code_buffer_append_u8(b, 0x49) &&
         binary_code_buffer_append_u8(b, 0x83) &&
         binary_code_buffer_append_u8(b, 0xE4) &&
         binary_code_buffer_append_u8(b, (unsigned char)mask);
}

static int multidot_slot(const BinaryFunctionContext *context,
                         const IROperand *operand, BinaryGpRegister *base,
                         int *disp) {
  for (size_t i = 0; i < context->marshaled_operand_count; i++) {
    if (context->marshaled_operands[i].operand == operand) {
      *base = (BinaryGpRegister)context->marshaled_operands[i].base_register;
      *disp = context->marshaled_operands[i].displacement;
      if (*base == BINARY_GP_RSP) {
        *disp += MULTIDOT_SAVE_BYTES;
      }
      return 1;
    }
  }
  return 0;
}

static int multidot_step(BinaryCodeBuffer *b, int k_count, int d_count,
                         const int *pa, const int *pb, int acc_base,
                         int disp) {
  for (int d = 0; d < d_count; d++) {
    if (!wcs_vex_movups_ymm_sib(b, 8 + d, kMultidotBases[d], BINARY_GP_RAX,
                                disp)) {
      return 0;
    }
  }
  for (int k = 0; k < k_count; k++) {
    if (!wcs_avx_vfmadd231ps_ymm(b, acc_base + k, 8 + pa[k], 8 + pb[k])) {
      return 0;
    }
  }
  return 1;
}

typedef struct {
  int k_count;
  int d_count;
  int unroll;
  int stride;
  int pa[8];
  int pb[8];
} MultidotShape;

static int multidot_decode(const IRInstruction *instruction,
                           MultidotShape *shape) {
  if (!instruction->arguments || instruction->argument_count < 3) {
    return 0;
  }
  const IROperand *args = instruction->arguments;
  shape->k_count = (int)args[0].int_value;
  shape->d_count = (int)args[1].int_value;
  if (shape->k_count < 2 || shape->k_count > 8 || shape->d_count < 1 ||
      shape->d_count > 6 ||
      instruction->argument_count !=
          (size_t)(3 + 3 * shape->k_count + shape->d_count)) {
    return 0;
  }
  int at = 3 + shape->k_count + shape->d_count;
  for (int k = 0; k < shape->k_count; k++) {
    shape->pa[k] = (int)args[at + 2 * k].int_value;
    shape->pb[k] = (int)args[at + 2 * k + 1].int_value;
    if (shape->pa[k] < 0 || shape->pa[k] >= shape->d_count ||
        shape->pb[k] < 0 || shape->pb[k] >= shape->d_count) {
      return 0;
    }
  }
  shape->unroll = shape->k_count <= 4 ? 2 : 1;
  shape->stride = 8 * shape->unroll;
  return 1;
}

static int multidot_emit_setup(CodeGenerator *generator,
                               BinaryFunctionContext *context,
                               const IROperand *args,
                               const MultidotShape *s) {
  BinaryCodeBuffer *b = &context->code;
  if (!code_generator_binary_emit_operand_load(generator, context, &args[2],
                                               BINARY_GP_RAX) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_R12, BINARY_GP_RAX) ||
      !binary_emit_shift_reg_imm8(b, 7, BINARY_GP_R12, 63) ||
      !binary_code_buffer_append_u8(b, 0x49) ||
      !binary_code_buffer_append_u8(b, 0xF7) ||
      !binary_code_buffer_append_u8(b, 0xD4) ||
      !binary_emit_alu_reg_reg(b, 0x21, BINARY_GP_R12, BINARY_GP_RAX)) {
    return 0;
  }
  for (int d = 0; d < s->d_count; d++) {
    if (!code_generator_binary_emit_operand_load(
            generator, context, &args[3 + s->k_count + d], kMultidotBases[d])) {
      return 0;
    }
  }
  if (!multidot_rsp_adjust(b, 1) ||
      !binary_emit_mov_mem_reg(b, BINARY_GP_RSP, MULTIDOT_COUNT_DISP,
                               BINARY_GP_R12)) {
    return 0;
  }
  for (int a = 0; a < s->k_count * s->unroll; a++) {
    if (!wcs_avx_vpxor_ymm(b, a, a, a)) {
      return 0;
    }
  }
  return binary_emit_alu_reg_reg(b, 0x31, BINARY_GP_RAX, BINARY_GP_RAX) &&
         multidot_and_r12_imm8(b, (signed char)-s->stride);
}

static int multidot_emit_vector_loops(BinaryCodeBuffer *b,
                                      const MultidotShape *s) {
  size_t loop_top = b->size;
  size_t j_vec_done = 0;
  size_t j_back = 0;
  if (!binary_emit_cmp_reg_reg(b, BINARY_GP_RAX, BINARY_GP_R12) ||
      !wcs_jcc(b, 0x83, &j_vec_done)) {
    return 0;
  }
  for (int u = 0; u < s->unroll; u++) {
    if (!multidot_step(b, s->k_count, s->d_count, s->pa, s->pb,
                       u * s->k_count, 32 * u)) {
      return 0;
    }
  }
  if (!wcs_addsub_reg_imm8(b, BINARY_GP_RAX, 0, (unsigned char)s->stride) ||
      !wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, loop_top) ||
      !wcs_patch_here(b, j_vec_done)) {
    return 0;
  }
  if (s->unroll != 2) {
    return 1;
  }
  size_t j_eight_done = 0;
  for (int k = 0; k < s->k_count; k++) {
    if (!wcs_avx_vaddps_ymm(b, k, k, s->k_count + k)) {
      return 0;
    }
  }
  return binary_emit_mov_reg_mem(b, BINARY_GP_R12, BINARY_GP_RSP,
                                 MULTIDOT_COUNT_DISP) &&
         multidot_and_r12_imm8(b, (signed char)-8) &&
         binary_emit_cmp_reg_reg(b, BINARY_GP_RAX, BINARY_GP_R12) &&
         wcs_jcc(b, 0x83, &j_eight_done) &&
         multidot_step(b, s->k_count, s->d_count, s->pa, s->pb, 0, 0) &&
         wcs_addsub_reg_imm8(b, BINARY_GP_RAX, 0, 8) &&
         wcs_patch_here(b, j_eight_done);
}

static int multidot_emit_reduce(BinaryCodeBuffer *b, const MultidotShape *s) {
  enum { ZERO = 15 };
  int acc[8];
  for (int j = 0; j < 8; j++) {
    acc[j] = j < s->k_count ? j : ZERO;
  }
  if (!wcs_avx_vpxor_ymm(b, ZERO, ZERO, ZERO)) {
    return 0;
  }
  for (int h = 0; h < 4; h++) {
    if (!wcs_avx_vhaddps_ymm(b, 8 + h, acc[2 * h], acc[2 * h + 1])) {
      return 0;
    }
  }
  if (!wcs_avx_vhaddps_ymm(b, 8, 8, 9) || !wcs_avx_vhaddps_ymm(b, 9, 10, 11) ||
      !wcs_avx_vperm2f128(b, 10, 8, 9, 0x20) ||
      !wcs_avx_vperm2f128(b, 11, 8, 9, 0x31) ||
      !wcs_avx_vaddps_ymm(b, 10, 10, 11) ||
      !wcs_avx_vmovups_mem_ymm(b, BINARY_GP_RSP, MULTIDOT_SUMS_DISP, 10)) {
    return 0;
  }
  for (int k = 0; k < s->k_count; k++) {
    if (!wcs_avx_vpxor_ymm(b, k, k, k)) {
      return 0;
    }
  }
  return 1;
}

static int multidot_emit_scalar_tail(BinaryCodeBuffer *b,
                                     const MultidotShape *s) {
  if (!multidot_emit_reduce(b, s) ||
      !binary_emit_mov_reg_mem(b, BINARY_GP_R12, BINARY_GP_RSP,
                               MULTIDOT_COUNT_DISP)) {
    return 0;
  }
  size_t tail_top = b->size;
  size_t j_tail_done = 0;
  size_t j_back = 0;
  if (!binary_emit_cmp_reg_reg(b, BINARY_GP_RAX, BINARY_GP_R12) ||
      !wcs_jcc(b, 0x83, &j_tail_done)) {
    return 0;
  }
  for (int d = 0; d < s->d_count; d++) {
    if (!wcs_vex_movss_xmm_sib(b, 8 + d, kMultidotBases[d], BINARY_GP_RAX, 0)) {
      return 0;
    }
  }
  for (int k = 0; k < s->k_count; k++) {
    if (!wcs_fmadd231ss(b, k, 8 + s->pa[k], 8 + s->pb[k])) {
      return 0;
    }
  }
  return wcs_addsub_reg_imm8(b, BINARY_GP_RAX, 0, 1) &&
         wcs_jcc(b, 0, &j_back) && wcs_patch_to(b, j_back, tail_top) &&
         wcs_patch_here(b, j_tail_done);
}

static int multidot_emit_accumulate(CodeGenerator *generator,
                                    BinaryFunctionContext *context,
                                    const IROperand *args,
                                    const MultidotShape *s) {
  BinaryCodeBuffer *b = &context->code;
  for (int k = 0; k < s->k_count; k++) {
    BinaryGpRegister base = BINARY_GP_RSP;
    int disp = 0;
    if (!multidot_slot(context, &args[3 + k], &base, &disp)) {
      code_generator_set_error(generator,
                               "simd_multidot_f32 accumulator is not staged");
      return 0;
    }
    if (!wcs_vex_addss_xmm_mem(b, k, k, BINARY_GP_RSP,
                               MULTIDOT_SUMS_DISP + 4 * k) ||
        !wcs_vex_addss_xmm_mem(b, k, k, base, disp) ||
        !wcs_vex_movss_mem_xmm(b, base, disp, k)) {
      return 0;
    }
  }
  return 1;
}

int code_generator_binary_emit_simd_multidot_f32(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  MultidotShape shape;
  if (!generator || !context || !instruction ||
      !multidot_decode(instruction, &shape)) {
    code_generator_set_error(generator, "Malformed simd_multidot_f32");
    return 0;
  }
  BinaryCodeBuffer *b = &context->code;
  return multidot_emit_setup(generator, context, instruction->arguments,
                             &shape) &&
         multidot_emit_vector_loops(b, &shape) &&
         multidot_emit_scalar_tail(b, &shape) &&
         multidot_emit_accumulate(generator, context, instruction->arguments,
                                  &shape) &&
         multidot_rsp_adjust(b, 0) && wcs_avx_vzeroupper(b);
}

static uint32_t exp_f32_bits(float f) {
  uint32_t u;
  memcpy(&u, &f, 4);
  return u;
}

#define EXP_POOL_OFF 32
#define EXP_C(i) (EXP_POOL_OFF + (i) * 4)
static const float exp_pool_consts[13] = {
    88.3762626647949f,    -88.3762626647949f,  1.44269504088896341f,
    0.5f,                 0.693359375f,        -2.12194440e-4f,
    1.9875691500E-4f,     1.3981999507E-3f,    8.3334519073E-3f,
    4.1665795894E-2f,     1.6666665459E-1f,    5.0000001201E-1f,
    1.0f};

static int exp_compute(BinaryCodeBuffer *b) {
  if (!wcs_avx_vbroadcastss_ymm_mem(b, 4, BINARY_GP_RSP, EXP_C(0)) ||
      !wcs_avx_vminps_ymm(b, 0, 0, 4) ||
      !wcs_avx_vbroadcastss_ymm_mem(b, 4, BINARY_GP_RSP, EXP_C(1)) ||
      !wcs_avx_vmaxps_ymm(b, 0, 0, 4)) {
    return 0;
  }
  if (!wcs_avx_vbroadcastss_ymm_mem(b, 1, BINARY_GP_RSP, EXP_C(2)) ||
      !wcs_avx_vmulps_ymm(b, 1, 0, 1) ||
      !wcs_avx_vbroadcastss_ymm_mem(b, 4, BINARY_GP_RSP, EXP_C(3)) ||
      !wcs_avx_vaddps_ymm(b, 1, 1, 4) ||
      !wcs_avx_vroundps_ymm(b, 1, 1, 1 )) {
    return 0;
  }
  if (!wcs_avx_vbroadcastss_ymm_mem(b, 4, BINARY_GP_RSP, EXP_C(4)) ||
      !wcs_avx_vmulps_ymm(b, 5, 1, 4) || !wcs_avx_vsubps_ymm(b, 0, 0, 5) ||
      !wcs_avx_vbroadcastss_ymm_mem(b, 4, BINARY_GP_RSP, EXP_C(5)) ||
      !wcs_avx_vmulps_ymm(b, 5, 1, 4) || !wcs_avx_vsubps_ymm(b, 0, 0, 5)) {
    return 0;
  }
  if (!wcs_avx_vbroadcastss_ymm_mem(b, 2, BINARY_GP_RSP, EXP_C(6)) ||
      !wcs_avx_vmulps_ymm(b, 2, 2, 0) ||
      !wcs_avx_vbroadcastss_ymm_mem(b, 4, BINARY_GP_RSP, EXP_C(7)) ||
      !wcs_avx_vaddps_ymm(b, 2, 2, 4)) {
    return 0;
  }
  for (int i = 8; i <= 11; i++) {
    if (!wcs_avx_vmulps_ymm(b, 2, 2, 0) ||
        !wcs_avx_vbroadcastss_ymm_mem(b, 4, BINARY_GP_RSP, EXP_C(i)) ||
        !wcs_avx_vaddps_ymm(b, 2, 2, 4)) {
      return 0;
    }
  }
  if (!wcs_avx_vmulps_ymm(b, 3, 0, 0) || !wcs_avx_vmulps_ymm(b, 2, 2, 3) ||
      !wcs_avx_vaddps_ymm(b, 2, 2, 0) ||
      !wcs_avx_vbroadcastss_ymm_mem(b, 4, BINARY_GP_RSP, EXP_C(12)) ||
      !wcs_avx_vaddps_ymm(b, 2, 2, 4)) {
    return 0;
  }
  if (!wcs_avx_vcvttps2dq_ymm(b, 1, 1) ||
      !wcs_avx_vbroadcastss_ymm_mem(b, 4, BINARY_GP_RSP, EXP_C(13)) ||
      !wcs_avx_vpaddd_ymm(b, 1, 1, 4) || !wcs_avx_vpslld_ymm_imm(b, 1, 1, 23) ||
      !wcs_avx_vmulps_ymm(b, 2, 2, 1)) {
    return 0;
  }
  return 1;
}

static int exp_fill_pool(BinaryCodeBuffer *b) {
  for (int i = 0; i < 14; i++) {
    uint32_t bits = (i == 13) ? 127u : exp_f32_bits(exp_pool_consts[i]);
    if (!binary_emit_mov_reg_imm32_zero_extend(b, BINARY_GP_RAX, bits) ||
        !binary_emit_mov_mem_reg32(b, BINARY_GP_RSP, EXP_C(i), BINARY_GP_RAX)) {
      return 0;
    }
  }
  return 1;
}

static int simd_tail_count_to_r8(BinaryCodeBuffer *b, size_t *tail) {
  return binary_emit_mov_reg_reg(b, BINARY_GP_R10, BINARY_GP_R9) &&
         wcs_addsub_reg_imm8(b, BINARY_GP_R10, 0 , 32) &&
         wcs_sub_reg_reg64(b, BINARY_GP_R10, BINARY_GP_RCX) &&
         binary_emit_shift_reg_imm8(b, 5 , BINARY_GP_R10, 2) &&
         binary_emit_mov_reg_reg(b, BINARY_GP_R8, BINARY_GP_R10) &&
         wcs_jcc(b, 0, tail);
}

static int simd_exp_step(BinaryCodeBuffer *b, size_t *done_main,
                         size_t *noclamp) {
  return wcs_avx_vmovups_ymm_mem(b, 0, BINARY_GP_RCX, 0) && exp_compute(b) &&
         wcs_avx_vmovups_mem_ymm(b, BINARY_GP_RCX, 0, 2) &&
         binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R9) &&
         wcs_jcc(b, 0x83 , done_main) &&
         wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 32) &&
         binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R9) &&
         wcs_jcc(b, 0x86 , noclamp);
}

int code_generator_binary_emit_simd_exp_f32(CodeGenerator *generator,
                                            BinaryFunctionContext *context,
                                            const IRInstruction *instruction) {
  BinaryCodeBuffer *b = NULL;
  size_t loop_top = 0, done_main = 0, small = 0, fin = 0, noclamp = 0;
  size_t tail = 0;
  if (!generator || !context || !instruction ||
      instruction->argument_count < 1 || !instruction->arguments) {
    code_generator_set_error(generator, "Malformed simd_exp_f32");
    return 0;
  }
  b = &context->code;

  if (!code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->dest,
                                               BINARY_GP_RCX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->arguments[0],
                                               BINARY_GP_R8) ||
      !simd_clamped_count(b) ||
      !binary_emit_shift_reg_imm8(b, 4, BINARY_GP_R9, 2) ||
      !wcs_add_reg_reg64(b, BINARY_GP_R9, BINARY_GP_RCX)) {
    return 0;
  }
  if (!binary_emit_sub_rsp_imm32(b, 96) || !exp_fill_pool(b)) {
    return 0;
  }
  if (!wcs_cmp_reg_imm32(b, BINARY_GP_R8, 8) ||
      !wcs_jcc(b, 0x82 , &small)) {
    return 0;
  }
  if (!wcs_addsub_reg_imm8(b, BINARY_GP_R9, 1 , 32)) {
    return 0;
  }
  loop_top = b->size;
  if (!simd_exp_step(b, &done_main, &noclamp)) {
    return 0;
  }
  if (!simd_tail_count_to_r8(b, &tail)) {
    return 0;
  }
  if (!wcs_patch_here(b, noclamp)) {
    return 0;
  }
  {
    size_t jb = 0;
    if (!wcs_jcc(b, 0, &jb) || !wcs_patch_to(b, jb, loop_top)) {
      return 0;
    }
  }
  if (!wcs_patch_here(b, done_main) || !wcs_jcc(b, 0, &fin)) {
    return 0;
  }

  if (!wcs_patch_here(b, small) || !wcs_patch_here(b, tail) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_R9, BINARY_GP_RCX) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_R11, BINARY_GP_RSP) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_R10, BINARY_GP_R8)) {
    return 0;
  }
  {
    size_t ci = b->size, di = 0;
    if (!binary_emit_test_reg_reg(b, BINARY_GP_R10) ||
        !wcs_jcc(b, 0x84 , &di) ||
        !binary_emit_mov_reg_mem32(b, BINARY_GP_RAX, BINARY_GP_RCX, 0) ||
        !binary_emit_mov_mem_reg32(b, BINARY_GP_R11, 0, BINARY_GP_RAX) ||
        !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 4) ||
        !wcs_addsub_reg_imm8(b, BINARY_GP_R11, 0, 4) ||
        !binary_emit_sub_reg_imm32(b, BINARY_GP_R10, 1)) {
      return 0;
    }
    size_t jb = 0;
    if (!wcs_jcc(b, 0, &jb) || !wcs_patch_to(b, jb, ci) ||
        !wcs_patch_here(b, di)) {
      return 0;
    }
  }
  if (!wcs_avx_vmovups_ymm_mem(b, 0, BINARY_GP_RSP, 0) || !exp_compute(b) ||
      !wcs_avx_vmovups_mem_ymm(b, BINARY_GP_RSP, 0, 2) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_R11, BINARY_GP_RSP) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_R10, BINARY_GP_R8)) {
    return 0;
  }
  {
    size_t co = b->size, dout = 0;
    if (!binary_emit_test_reg_reg(b, BINARY_GP_R10) ||
        !wcs_jcc(b, 0x84 , &dout) ||
        !binary_emit_mov_reg_mem32(b, BINARY_GP_RAX, BINARY_GP_R11, 0) ||
        !binary_emit_mov_mem_reg32(b, BINARY_GP_R9, 0, BINARY_GP_RAX) ||
        !wcs_addsub_reg_imm8(b, BINARY_GP_R9, 0, 4) ||
        !wcs_addsub_reg_imm8(b, BINARY_GP_R11, 0, 4) ||
        !binary_emit_sub_reg_imm32(b, BINARY_GP_R10, 1)) {
      return 0;
    }
    size_t jb = 0;
    if (!wcs_jcc(b, 0, &jb) || !wcs_patch_to(b, jb, co) ||
        !wcs_patch_here(b, dout)) {
      return 0;
    }
  }
  if (!wcs_patch_here(b, fin)) {
    return 0;
  }
  if (!binary_emit_add_rsp_imm32(b, 96)) {
    return 0;
  }
  return wcs_avx_vzeroupper(b);
}

#define SILU_USCRATCH 88

static int silu_vec(BinaryCodeBuffer *b, int has_mul) {
  if (!wcs_avx_vpxor_ymm(b, 0, 0, 0) ||
      !wcs_avx_vsubps_ymm(b, 0, 0, 6) ||
      !exp_compute(b) ||
      !wcs_avx_vbroadcastss_ymm_mem(b, 4, BINARY_GP_RSP, EXP_C(12)) ||
      !wcs_avx_vaddps_ymm(b, 2, 2, 4) ||
      !wcs_avx_vdivps_ymm(b, 6, 6, 2)) {
    return 0;
  }
  if (has_mul && !wcs_avx_vmulps_ymm(b, 6, 6, 7)) {
    return 0;
  }
  return 1;
}

int code_generator_binary_emit_simd_silu_f32_inline(BinaryCodeBuffer *b,
                                                    int has_mul) {
  size_t loop_top = 0, done_main = 0, small = 0, fin = 0, noclamp = 0;
  size_t tail = 0;
  if (!simd_clamped_count(b) ||
      !binary_emit_shift_reg_imm8(b, 4, BINARY_GP_R9, 2) ||
      !wcs_add_reg_reg64(b, BINARY_GP_R9, BINARY_GP_RCX)) {
    return 0;
  }
  if (!binary_emit_sub_rsp_imm32(b, 128) || !exp_fill_pool(b)) {
    return 0;
  }
  if (!wcs_cmp_reg_imm32(b, BINARY_GP_R8, 8) ||
      !wcs_jcc(b, 0x82 , &small)) {
    return 0;
  }
  if (!wcs_addsub_reg_imm8(b, BINARY_GP_R9, 1 , 32)) {
    return 0;
  }
  loop_top = b->size;
  if (!wcs_avx_vmovups_ymm_mem(b, 6, BINARY_GP_RCX, 0)) {
    return 0;
  }
  if (has_mul && !wcs_avx_vmovups_ymm_mem(b, 7, BINARY_GP_RDX, 0)) {
    return 0;
  }
  if (!silu_vec(b, has_mul) ||
      !wcs_avx_vmovups_mem_ymm(b, BINARY_GP_RCX, 0, 6) ||
      !binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R9) ||
      !wcs_jcc(b, 0x83 , &done_main) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 32)) {
    return 0;
  }
  if (has_mul && !wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 32)) {
    return 0;
  }
  if (!binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R9) ||
      !wcs_jcc(b, 0x86 , &noclamp)) {
    return 0;
  }
  if (!simd_tail_count_to_r8(b, &tail)) {
    return 0;
  }
  if (!wcs_patch_here(b, noclamp)) {
    return 0;
  }
  {
    size_t jb = 0;
    if (!wcs_jcc(b, 0, &jb) || !wcs_patch_to(b, jb, loop_top)) {
      return 0;
    }
  }
  if (!wcs_patch_here(b, done_main) || !wcs_jcc(b, 0, &fin)) {
    return 0;
  }

  if (!wcs_patch_here(b, small) || !wcs_patch_here(b, tail) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_R9, BINARY_GP_RCX)) {
    return 0;
  }
  if (!binary_emit_mov_reg_reg(b, BINARY_GP_R11, BINARY_GP_RSP) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_R10, BINARY_GP_R8)) {
    return 0;
  }
  {
    size_t ci = b->size, di = 0;
    if (!binary_emit_test_reg_reg(b, BINARY_GP_R10) ||
        !wcs_jcc(b, 0x84 , &di) ||
        !binary_emit_mov_reg_mem32(b, BINARY_GP_RAX, BINARY_GP_RCX, 0) ||
        !binary_emit_mov_mem_reg32(b, BINARY_GP_R11, 0, BINARY_GP_RAX)) {
      return 0;
    }
    if (has_mul &&
        (!binary_emit_mov_reg_mem32(b, BINARY_GP_RAX, BINARY_GP_RDX, 0) ||
         !binary_emit_mov_mem_reg32(b, BINARY_GP_R11, SILU_USCRATCH,
                                    BINARY_GP_RAX) ||
         !wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 4))) {
      return 0;
    }
    if (!wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 4) ||
        !wcs_addsub_reg_imm8(b, BINARY_GP_R11, 0, 4) ||
        !binary_emit_sub_reg_imm32(b, BINARY_GP_R10, 1)) {
      return 0;
    }
    size_t jb = 0;
    if (!wcs_jcc(b, 0, &jb) || !wcs_patch_to(b, jb, ci) ||
        !wcs_patch_here(b, di)) {
      return 0;
    }
  }
  if (!wcs_avx_vmovups_ymm_mem(b, 6, BINARY_GP_RSP, 0)) {
    return 0;
  }
  if (has_mul &&
      !wcs_avx_vmovups_ymm_mem(b, 7, BINARY_GP_RSP, SILU_USCRATCH)) {
    return 0;
  }
  if (!silu_vec(b, has_mul) ||
      !wcs_avx_vmovups_mem_ymm(b, BINARY_GP_RSP, 0, 6)) {
    return 0;
  }
  if (!binary_emit_mov_reg_reg(b, BINARY_GP_R11, BINARY_GP_RSP) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_R10, BINARY_GP_R8)) {
    return 0;
  }
  {
    size_t co = b->size, dout = 0;
    if (!binary_emit_test_reg_reg(b, BINARY_GP_R10) ||
        !wcs_jcc(b, 0x84 , &dout) ||
        !binary_emit_mov_reg_mem32(b, BINARY_GP_RAX, BINARY_GP_R11, 0) ||
        !binary_emit_mov_mem_reg32(b, BINARY_GP_R9, 0, BINARY_GP_RAX) ||
        !wcs_addsub_reg_imm8(b, BINARY_GP_R9, 0, 4) ||
        !wcs_addsub_reg_imm8(b, BINARY_GP_R11, 0, 4) ||
        !binary_emit_sub_reg_imm32(b, BINARY_GP_R10, 1)) {
      return 0;
    }
    size_t jb = 0;
    if (!wcs_jcc(b, 0, &jb) || !wcs_patch_to(b, jb, co) ||
        !wcs_patch_here(b, dout)) {
      return 0;
    }
  }
  if (!wcs_patch_here(b, fin) || !binary_emit_add_rsp_imm32(b, 128)) {
    return 0;
  }
  return wcs_avx_vzeroupper(b);
}
