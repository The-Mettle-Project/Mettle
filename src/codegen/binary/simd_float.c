#include "codegen/binary/internal.h"
#include "codegen/binary/simd_internal.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Floating-point SIMD kernels (AVX2 + FMA3 horizontal sums, dot products, affine maps). Encoders live in simd_encoders.c; see simd_internal.h. */

/* Horizontal sum of base[0..len-1] doubles, ADDED to dest's prior value.
 * dest = float64 sum symbol, lhs = base pointer, rhs = element count. */
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

  /* rax=prior bits, rcx=walk, r9=end, r10=bytes-remaining scratch.
   * xmm3=scalar running total, ymm2/ymm4=packed accumulators, ymm0/1=scratch. */
  if (!code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->dest,
                                               BINARY_GP_RAX) ||
      !binary_emit_movq_xmm_reg(b, BINARY_XMM3, BINARY_GP_RAX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->lhs,
                                               BINARY_GP_RCX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->rhs,
                                               BINARY_GP_R8) ||
      !wcs_mov_reg_reg32(b, BINARY_GP_R9, BINARY_GP_R8) ||
      !wcs_shift_reg_imm(b, BINARY_GP_R9, 0, 3) ||
      !wcs_add_reg_reg64(b, BINARY_GP_R9, BINARY_GP_RCX) ||
      !wcs_avx_vpxor_ymm(b, 2, 2, 2) ||
      !wcs_avx_vpxor_ymm(b, 4, 4, 4)) {
    return 0;
  }

  loop_top = b->size;
  if (!binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R9) ||
      !wcs_jcc(b, 0x83 /* jae */, &j_done) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_R10, BINARY_GP_R9) ||
      !binary_emit_alu_reg_reg(b, 0x29, BINARY_GP_R10, BINARY_GP_RCX) ||
      !wcs_cmp_reg_imm32(b, BINARY_GP_R10, 64) ||
      !wcs_jcc(b, 0x83 /* jae */, &j_vec2) ||
      !wcs_cmp_reg_imm32(b, BINARY_GP_R10, 32) ||
      !wcs_jcc(b, 0x83 /* jae */, &j_vec) ||
      !wcs_jcc(b, 0, &j_scalar)) {
    return 0;
  }

  /* Two-accumulator unroll: 8 doubles/iter summed into the independent ymm2 and
   * ymm4 chains so the ~4-cycle vaddpd latency overlaps instead of serializing
   * a single accumulator. The 32-byte and scalar tiers below mop up the < 64B
   * remainder once the loop re-dispatches. */
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
      !wcs_movsd_xmm_mem(b, 0, BINARY_GP_RCX, 0) ||
      !binary_emit_addsd_xmm_xmm(b, BINARY_XMM3, BINARY_XMM0) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 8)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, loop_top)) {
      return 0;
    }
  }

  /* Fold the second accumulator in before the horizontal reduce. */
  if (!wcs_patch_here(b, j_done) ||
      !wcs_avx_vaddpd_ymm(b, 2, 2, 4) ||
      !wcs_reduce_pd_acc_to_rax(b)) {
    return 0;
  }
  return code_generator_binary_emit_destination_store(generator, context,
                                                      &instruction->dest,
                                                      BINARY_GP_RAX);
}

/* Horizontal sum of base[0..len-1] floats, ADDED to dest's prior value.
 * dest = float32 sum symbol, lhs = base pointer, rhs = element count. */
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
      !binary_emit_movd_xmm_reg(b, BINARY_XMM3, BINARY_GP_RAX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->lhs,
                                               BINARY_GP_RCX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->rhs,
                                               BINARY_GP_R8) ||
      !wcs_mov_reg_reg32(b, BINARY_GP_R9, BINARY_GP_R8) ||
      !wcs_shift_reg_imm(b, BINARY_GP_R9, 0, 2) ||
      !wcs_add_reg_reg64(b, BINARY_GP_R9, BINARY_GP_RCX) ||
      !wcs_avx_vpxor_ymm(b, 2, 2, 2) ||
      !wcs_avx_vpxor_ymm(b, 4, 4, 4)) {
    return 0;
  }

  loop_top = b->size;
  if (!binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R9) ||
      !wcs_jcc(b, 0x83 /* jae */, &j_done) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_R10, BINARY_GP_R9) ||
      !binary_emit_alu_reg_reg(b, 0x29, BINARY_GP_R10, BINARY_GP_RCX) ||
      !wcs_cmp_reg_imm32(b, BINARY_GP_R10, 64) ||
      !wcs_jcc(b, 0x83 /* jae */, &j_vec2) ||
      !wcs_cmp_reg_imm32(b, BINARY_GP_R10, 32) ||
      !wcs_jcc(b, 0x83 /* jae */, &j_vec) ||
      !wcs_jcc(b, 0, &j_scalar)) {
    return 0;
  }

  /* Two-accumulator unroll: 16 floats/iter into the independent ymm2 and ymm4
   * chains; the 32-byte and scalar tiers handle the < 64B remainder. */
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
      !wcs_movss_xmm_mem(b, 0, BINARY_GP_RCX, 0) ||
      !binary_emit_addss_xmm_xmm(b, BINARY_XMM3, BINARY_XMM0) ||
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

/* Float64 dot product of a[0..n-1]*b[0..n-1], ADDED to dest's prior value.
 * dest = float64 sum, lhs = a, rhs = b, arguments[0] = element count. */
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

  /* rcx=a walk, rdx=b walk, r9=a_end, r10=scratch, rax=prior/result.
   * xmm3=scalar total, ymm2/ymm4=packed FMA accumulators, ymm0/ymm1=scratch. */
  if (!code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->dest,
                                               BINARY_GP_RAX) ||
      !binary_emit_movq_xmm_reg(b, BINARY_XMM3, BINARY_GP_RAX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->lhs,
                                               BINARY_GP_RCX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->rhs,
                                               BINARY_GP_RDX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->arguments[0],
                                               BINARY_GP_R8) ||
      !wcs_mov_reg_reg32(b, BINARY_GP_R9, BINARY_GP_R8) ||
      !wcs_shift_reg_imm(b, BINARY_GP_R9, 0, 3) ||
      !wcs_add_reg_reg64(b, BINARY_GP_R9, BINARY_GP_RCX) ||
      !wcs_avx_vpxor_ymm(b, 2, 2, 2) ||
      !wcs_avx_vpxor_ymm(b, 4, 4, 4)) {
    return 0;
  }

  loop_top = b->size;
  if (!binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R9) ||
      !wcs_jcc(b, 0x83 /* jae */, &j_done) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_R10, BINARY_GP_R9) ||
      !binary_emit_alu_reg_reg(b, 0x29, BINARY_GP_R10, BINARY_GP_RCX) ||
      !wcs_cmp_reg_imm32(b, BINARY_GP_R10, 64) ||
      !wcs_jcc(b, 0x83 /* jae */, &j_vec2) ||
      !wcs_cmp_reg_imm32(b, BINARY_GP_R10, 32) ||
      !wcs_jcc(b, 0x83 /* jae */, &j_vec) ||
      !wcs_jcc(b, 0, &j_scalar)) {
    return 0;
  }

  /* Two-accumulator FMA unroll: 8 elements/iter. Each vfmadd231pd folds a*b
   * into its accumulator in a single rounding step, and the two chains (ymm2,
   * ymm4) run independently so the FMA latency is hidden. */
  if (!wcs_patch_here(b, j_vec2) ||
      !wcs_avx_vmovups_ymm_mem(b, 0, BINARY_GP_RCX, 0) ||
      !wcs_avx_vmovups_ymm_mem(b, 1, BINARY_GP_RDX, 0) ||
      !wcs_avx_vfmadd231pd_ymm(b, 2, 0, 1) ||
      !wcs_avx_vmovups_ymm_mem(b, 0, BINARY_GP_RCX, 32) ||
      !wcs_avx_vmovups_ymm_mem(b, 1, BINARY_GP_RDX, 32) ||
      !wcs_avx_vfmadd231pd_ymm(b, 4, 0, 1) ||
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
      !wcs_avx_vfmadd231pd_ymm(b, 2, 0, 1) ||
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
      !wcs_movsd_xmm_mem(b, 0, BINARY_GP_RCX, 0) ||
      !wcs_movsd_xmm_mem(b, 1, BINARY_GP_RDX, 0) ||
      !wcs_fmadd231sd(b, 3, 0, 1) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 8) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 8)) {
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

/* Float64 affine map: dst[i] = a * src[i] + b * dst[i] + c.
 * lhs=src, rhs=dst, arguments[0]=count, [1]=a, [2]=b, [3]=c. */
int code_generator_binary_emit_simd_affine_map_f64(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  BinaryCodeBuffer *b = NULL;
  size_t loop_top = 0;
  size_t j_done = 0;
  size_t j_vec = 0;
  size_t j_scalar = 0;

  if (!generator || !context || !instruction ||
      instruction->argument_count < 4 || !instruction->arguments) {
    code_generator_set_error(generator, "Malformed simd_affine_map_f64");
    return 0;
  }
  b = &context->code;

  /* Identity-fold the affine coefficients when they are compile-time constants:
   * b == 1 means the dst term is just +dst[i] (no scale), and c == 0 means no
   * bias add. The common saxpy form `dst = a*src + dst` (b==1, c==0) then
   * collapses to a single fused multiply-add per vector instead of mul+fma+add. */
  int b_is_one = instruction->arguments[2].kind == IR_OPERAND_FLOAT &&
                 instruction->arguments[2].float_value == 1.0;
  int c_is_zero = instruction->arguments[3].kind == IR_OPERAND_FLOAT &&
                  instruction->arguments[3].float_value == 0.0;

  /* rcx=src walk, rdx=dst walk, r9=src_end, r10=bytes remaining.
   * ymm4=a, ymm5=b, ymm3=c; ymm0/ymm1 are vector scratch. */
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
      !binary_emit_movq_xmm_reg(b, BINARY_XMM4, BINARY_GP_RAX) ||
      !wcs_avx_vbroadcastsd_ymm_xmm(b, 4, 4) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->arguments[2],
                                               BINARY_GP_RAX) ||
      !binary_emit_movq_xmm_reg(b, BINARY_XMM5, BINARY_GP_RAX) ||
      !wcs_avx_vbroadcastsd_ymm_xmm(b, 5, 5) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->arguments[3],
                                               BINARY_GP_RAX) ||
      !binary_emit_movq_xmm_reg(b, BINARY_XMM3, BINARY_GP_RAX) ||
      !wcs_avx_vbroadcastsd_ymm_xmm(b, 3, 3) ||
      !wcs_mov_reg_reg32(b, BINARY_GP_R9, BINARY_GP_R8) ||
      !wcs_shift_reg_imm(b, BINARY_GP_R9, 0, 3) ||
      !wcs_add_reg_reg64(b, BINARY_GP_R9, BINARY_GP_RCX)) {
    return 0;
  }

  loop_top = b->size;
  if (!binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R9) ||
      !wcs_jcc(b, 0x83 /* jae */, &j_done) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_R10, BINARY_GP_R9) ||
      !binary_emit_alu_reg_reg(b, 0x29, BINARY_GP_R10, BINARY_GP_RCX) ||
      !wcs_cmp_reg_imm32(b, BINARY_GP_R10, 32) ||
      !wcs_jcc(b, 0x83 /* jae */, &j_vec) ||
      !wcs_jcc(b, 0, &j_scalar)) {
    return 0;
  }

  if (!wcs_patch_here(b, j_vec) ||
      !wcs_avx_vmovups_ymm_mem(b, 0, BINARY_GP_RCX, 0) ||
      !wcs_avx_vmovups_ymm_mem(b, 1, BINARY_GP_RDX, 0)) {
    return 0;
  }
  if (b_is_one && c_is_zero) {
    /* dst += a*src (one fma into the dst vector). */
    if (!wcs_avx_vfmadd231pd_ymm(b, 1, 0, 4) ||
        !wcs_avx_vmovups_mem_ymm(b, BINARY_GP_RDX, 0, 1)) {
      return 0;
    }
  } else {
    if (!wcs_avx_vmulpd_ymm(b, 0, 0, 4) ||
        !wcs_avx_vfmadd231pd_ymm(b, 0, 5, 1)) {
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
      !wcs_movsd_xmm_mem(b, 0, BINARY_GP_RCX, 0) ||
      !wcs_movsd_xmm_mem(b, 1, BINARY_GP_RDX, 0)) {
    return 0;
  }
  if (b_is_one && c_is_zero) {
    if (!wcs_fmadd231sd(b, 1, 0, 4) ||
        !wcs_movsd_mem_xmm(b, BINARY_GP_RDX, 0, BINARY_XMM1)) {
      return 0;
    }
  } else {
    if (!binary_emit_mulsd_xmm_xmm(b, BINARY_XMM0, BINARY_XMM4) ||
        !wcs_fmadd231sd(b, 0, 5, 1)) {
      return 0;
    }
    if (!c_is_zero && !binary_emit_addsd_xmm_xmm(b, BINARY_XMM0, BINARY_XMM3)) {
      return 0;
    }
    if (!wcs_movsd_mem_xmm(b, BINARY_GP_RDX, 0, BINARY_XMM0)) {
      return 0;
    }
  }
  if (!wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 8) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 8)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, loop_top)) {
      return 0;
    }
  }

  return wcs_patch_here(b, j_done) && wcs_avx_vzeroupper(b);
}

/* Float32 affine map: dst[i] = a * src[i] + b * dst[i] + c.
 * lhs=src, rhs=dst, arguments[0]=count, [1]=a, [2]=b, [3]=c. */
int code_generator_binary_emit_simd_affine_map_f32(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  BinaryCodeBuffer *b = NULL;
  size_t loop_top = 0;
  size_t j_done = 0;
  size_t j_vec = 0;
  size_t j_scalar = 0;

  if (!generator || !context || !instruction ||
      instruction->argument_count < 4 || !instruction->arguments) {
    code_generator_set_error(generator, "Malformed simd_affine_map_f32");
    return 0;
  }
  b = &context->code;

  /* See the f64 variant: fold b==1 (no dst scale) and c==0 (no bias); the saxpy
   * form b==1,c==0 collapses mul+fma+add into a single fused multiply-add. */
  int b_is_one = instruction->arguments[2].kind == IR_OPERAND_FLOAT &&
                 instruction->arguments[2].float_value == 1.0;
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
      !wcs_movd_xmm_reg(b, 4, BINARY_GP_RAX) ||
      !wcs_avx_vpbroadcastd_ymm(b, 4, 4) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->arguments[2],
                                               BINARY_GP_RAX) ||
      !wcs_movd_xmm_reg(b, 5, BINARY_GP_RAX) ||
      !wcs_avx_vpbroadcastd_ymm(b, 5, 5) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->arguments[3],
                                               BINARY_GP_RAX) ||
      !wcs_movd_xmm_reg(b, 3, BINARY_GP_RAX) ||
      !wcs_avx_vpbroadcastd_ymm(b, 3, 3) ||
      !wcs_mov_reg_reg32(b, BINARY_GP_R9, BINARY_GP_R8) ||
      !wcs_shift_reg_imm(b, BINARY_GP_R9, 0, 2) ||
      !wcs_add_reg_reg64(b, BINARY_GP_R9, BINARY_GP_RCX)) {
    return 0;
  }

  loop_top = b->size;
  if (!binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R9) ||
      !wcs_jcc(b, 0x83 /* jae */, &j_done) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_R10, BINARY_GP_R9) ||
      !binary_emit_alu_reg_reg(b, 0x29, BINARY_GP_R10, BINARY_GP_RCX) ||
      !wcs_cmp_reg_imm32(b, BINARY_GP_R10, 32) ||
      !wcs_jcc(b, 0x83 /* jae */, &j_vec) ||
      !wcs_jcc(b, 0, &j_scalar)) {
    return 0;
  }

  if (!wcs_patch_here(b, j_vec) ||
      !wcs_avx_vmovups_ymm_mem(b, 0, BINARY_GP_RCX, 0) ||
      !wcs_avx_vmovups_ymm_mem(b, 1, BINARY_GP_RDX, 0)) {
    return 0;
  }
  if (b_is_one && c_is_zero) {
    if (!wcs_avx_vfmadd231ps_ymm(b, 1, 0, 4) ||
        !wcs_avx_vmovups_mem_ymm(b, BINARY_GP_RDX, 0, 1)) {
      return 0;
    }
  } else {
    if (!wcs_avx_vmulps_ymm(b, 0, 0, 4) ||
        !wcs_avx_vfmadd231ps_ymm(b, 0, 5, 1)) {
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
      !wcs_movss_xmm_mem(b, 0, BINARY_GP_RCX, 0) ||
      !wcs_movss_xmm_mem(b, 1, BINARY_GP_RDX, 0)) {
    return 0;
  }
  if (b_is_one && c_is_zero) {
    if (!wcs_fmadd231ss(b, 1, 0, 4) ||
        !wcs_movss_mem_xmm(b, BINARY_GP_RDX, 0, BINARY_XMM1)) {
      return 0;
    }
  } else if (!binary_emit_mulss_xmm_xmm(b, BINARY_XMM0, BINARY_XMM4) ||
             !wcs_fmadd231ss(b, 0, 5, 1) ||
             (!c_is_zero &&
              !binary_emit_addss_xmm_xmm(b, BINARY_XMM0, BINARY_XMM3)) ||
             !wcs_movss_mem_xmm(b, BINARY_GP_RDX, 0, BINARY_XMM0)) {
    return 0;
  }
  if (!wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 4) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 4)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, loop_top)) {
      return 0;
    }
  }

  return wcs_patch_here(b, j_done) && wcs_avx_vzeroupper(b);
}

/* General auto-vectorized loop kernel (IR_OP_SIMD_VLOOP_F64). Decodes the
 * serialized float64 body DAG (see the opcode doc in ir.h) and emits a packed
 * AVX2 loop + scalar remainder. A2: element-wise maps out[i] = DAG(a_k[i],
 * (float64)i, consts). Replays the DAG over f64x4 lanes via a stack-machine of
 * up to VLOOP_KERNEL_REGS ymm registers; constants are broadcast once to a stack
 * array and re-read; array bases walk by 32B (vector) / 8B (scalar). */
#define VLOOP_K_LOAD 0
#define VLOOP_K_IOTA 1
#define VLOOP_K_CONST 2
#define VLOOP_K_ADD 3
#define VLOOP_K_SUB 4
#define VLOOP_K_MUL 5
#define VLOOP_K_DIV 6
#define VLOOP_KERNEL_REGS 4
#define VLOOP_KERNEL_MAX_NODES 48
#define VLOOP_KERNEL_MAX_BASES 4

int code_generator_binary_emit_simd_vloop_f64(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  BinaryCodeBuffer *b = NULL;
  /* ymm node-eval pools (volatile). Maps may use ymm0,1,2,4 (4-deep). '+'
   * reductions reserve ymm2 = packed accumulator and xmm3 = scalar/prior
   * accumulator, so their node pool is ymm0,1,4 (3-deep). ymm5 = iota const. */
  static const int kPoolMap[4] = {0, 1, 2, 4};
  static const int kPoolReduce[3] = {0, 1, 4};
  static const BinaryGpRegister kGp[VLOOP_KERNEL_MAX_BASES] = {
      BINARY_GP_RCX, BINARY_GP_RDX, BINARY_GP_R8, BINARY_GP_R9};
  const int IOTA_CONST = 5;

  if (!generator || !context || !instruction || instruction->argument_count < 6 ||
      !instruction->arguments || instruction->dest.kind != IR_OPERAND_SYMBOL) {
    code_generator_set_error(generator, "Malformed simd_vloop_f64");
    return 0;
  }
  b = &context->code;

  const IROperand *args = instruction->arguments;
  long long reduce_op = args[0].int_value;
  int n_arrays = (int)args[1].int_value;
  int n_nodes = (int)args[2].int_value;
  int root = (int)args[3].int_value;
  int n_consts = (int)args[4].int_value;
  int depth = (int)args[5].int_value;
  int is_reduce = (reduce_op == 1);
  const int *kPool = is_reduce ? kPoolReduce : kPoolMap;
  int pool_n = is_reduce ? 3 : 4;
  size_t expect = (size_t)(6 + n_arrays + 3 * n_nodes + n_consts);
  if ((reduce_op != 0 && reduce_op != 1) || n_arrays < 0 ||
      n_arrays > VLOOP_KERNEL_MAX_BASES || n_nodes <= 0 ||
      n_nodes > VLOOP_KERNEL_MAX_NODES || n_consts < 0 || depth > pool_n ||
      root < 0 || root >= n_nodes || instruction->argument_count != expect) {
    code_generator_set_error(generator, "Bad simd_vloop_f64 encoding");
    return 0;
  }

  size_t nodes_off = (size_t)(6 + n_arrays);
  size_t consts_off = nodes_off + (size_t)(3 * n_nodes);

  /* Assign a GP walking-pointer register to each distinct base symbol (the
   * destination plus each loaded array not equal to it). arr_reg[k] = the GP
   * register holding loaded array k's current element address. */
  const char *dist_name[VLOOP_KERNEL_MAX_BASES];
  const IROperand *dist_src[VLOOP_KERNEL_MAX_BASES];
  int n_dist = 0;
  /* For a map, dest is the stored array base (a walking pointer). For a '+'
   * reduction, dest is the scalar accumulator (handled via xmm3/RAX), not a
   * base pointer, so it is not seeded here. */
  if (!is_reduce) {
    dist_name[0] = instruction->dest.name;
    dist_src[0] = &instruction->dest;
    n_dist = 1;
  }
  int arr_reg[VLOOP_KERNEL_MAX_BASES];
  int has_iota = 0;
  for (int i = 0; i < n_nodes; i++) {
    if ((int)args[nodes_off + 3 * i].int_value == VLOOP_K_IOTA) {
      has_iota = 1;
    }
  }
  for (int k = 0; k < n_arrays; k++) {
    const IROperand *as = &args[nodes_off - n_arrays + k]; /* array base k */
    const char *nm = as->name;
    int found = -1;
    for (int j = 0; j < n_dist; j++) {
      if (dist_name[j] && nm && strcmp(dist_name[j], nm) == 0) {
        found = j;
        break;
      }
    }
    if (found >= 0) {
      arr_reg[k] = kGp[found];
    } else {
      if (n_dist >= VLOOP_KERNEL_MAX_BASES) {
        code_generator_set_error(generator, "simd_vloop_f64 too many bases");
        return 0;
      }
      dist_name[n_dist] = nm;
      dist_src[n_dist] = as;
      arr_reg[k] = kGp[n_dist];
      n_dist++;
    }
  }
  int dst_reg = kGp[0];
  /* r10 = element count (decrements), r11 = i (only if IOTA). rax = scratch. */

  /* Stack scratch for broadcast constants: 32 bytes each. */
  uint32_t cbytes = (uint32_t)(32 * n_consts);
  if (cbytes && !binary_emit_sub_rsp_imm32(b, cbytes)) {
    return 0;
  }

  /* Load base pointers and the element count. */
  for (int j = 0; j < n_dist; j++) {
    if (!code_generator_binary_emit_operand_load(generator, context,
                                                 dist_src[j], kGp[j])) {
      return 0;
    }
  }
  if (!code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->lhs,
                                               BINARY_GP_R10)) {
    return 0;
  }
  if (has_iota && !binary_emit_mov_reg_imm64(b, BINARY_GP_R11, 0)) {
    return 0;
  }

  /* Broadcast each constant once into its stack slot. */
  for (int c = 0; c < n_consts; c++) {
    double dv = args[consts_off + c].float_value;
    uint64_t bits = 0;
    memcpy(&bits, &dv, sizeof(bits));
    if (!binary_emit_mov_reg_imm64(b, BINARY_GP_RAX, bits) ||
        !binary_emit_movq_xmm_reg(b, BINARY_XMM0, BINARY_GP_RAX) ||
        !wcs_avx_vbroadcastsd_ymm_xmm(b, 0, 0) ||
        !wcs_avx_vmovups_mem_ymm(b, BINARY_GP_RSP, 32 * c, 0)) {
      return 0;
    }
  }
  /* iota constant [0,1,2,3] in ymm5 (int32 lanes), if needed. */
  if (has_iota) {
    if (!binary_emit_mov_reg_imm64(b, BINARY_GP_RAX, 0x0000000100000000ULL) ||
        !binary_emit_movq_xmm_reg(b, BINARY_XMM0, BINARY_GP_RAX) ||
        !binary_emit_mov_reg_imm64(b, BINARY_GP_RAX, 0x0000000300000002ULL) ||
        !binary_emit_movq_xmm_reg(b, BINARY_XMM1, BINARY_GP_RAX) ||
        !wcs_avx_vpunpcklqdq_xmm(b, IOTA_CONST, 0, 1)) {
      return 0;
    }
  }

  /* Reduction: xmm3 = prior accumulator value, ymm2 = packed accumulator = 0. */
  if (is_reduce) {
    if (!code_generator_binary_emit_operand_load(generator, context,
                                                 &instruction->dest,
                                                 BINARY_GP_RAX) ||
        !binary_emit_movq_xmm_reg(b, BINARY_XMM3, BINARY_GP_RAX) ||
        !wcs_avx_vpxor_ymm(b, 2, 2, 2)) {
      return 0;
    }
  }

  /* ---- vector loop: while (count >= 4) ---- */
  size_t vec_top = b->size;
  size_t j_tail = 0;
  if (!wcs_cmp_reg_imm8(b, BINARY_GP_R10, 4) || !wcs_jcc(b, 0x82 /* jb */, &j_tail)) {
    return 0;
  }
  {
    int pool[4];
    int nfree = pool_n;
    for (int i = 0; i < pool_n; i++) {
      pool[i] = kPool[i];
    }
    int vstk[VLOOP_KERNEL_MAX_NODES];
    int nv = 0;
    for (int i = 0; i < n_nodes; i++) {
      int tag = (int)args[nodes_off + 3 * i].int_value;
      int op0 = (int)args[nodes_off + 3 * i + 1].int_value;
      int op1 = (int)args[nodes_off + 3 * i + 2].int_value;
      if (tag <= VLOOP_K_CONST) {
        if (nfree <= 0) {
          code_generator_set_error(generator, "vloop reg budget");
          return 0;
        }
        int R = pool[--nfree];
        int ok = 0;
        if (tag == VLOOP_K_LOAD) {
          ok = wcs_avx_vmovups_ymm_mem(b, R, arr_reg[op0], 0);
        } else if (tag == VLOOP_K_CONST) {
          ok = wcs_avx_vmovups_ymm_mem(b, R, BINARY_GP_RSP, 32 * op0);
        } else { /* IOTA: R = (f64)[i, i+1, i+2, i+3] */
          ok = wcs_broadcast_i32_to_ymm(b, R, BINARY_GP_R11) &&
               wcs_avx_vpaddd_ymm(b, R, R, IOTA_CONST) &&
               wcs_avx_vcvtdq2pd_ymm_xmm(b, R, R);
        }
        if (!ok) {
          return 0;
        }
        vstk[nv++] = R;
      } else {
        if (nv < 2) {
          code_generator_set_error(generator, "vloop stack");
          return 0;
        }
        int rb = vstk[--nv];
        int ra = vstk[--nv];
        int ok = 0;
        switch (tag) {
        case VLOOP_K_ADD: ok = wcs_avx_vaddpd_ymm(b, ra, ra, rb); break;
        case VLOOP_K_SUB: ok = wcs_avx_vsubpd_ymm(b, ra, ra, rb); break;
        case VLOOP_K_MUL: ok = wcs_avx_vmulpd_ymm(b, ra, ra, rb); break;
        case VLOOP_K_DIV: ok = wcs_avx_vdivpd_ymm(b, ra, ra, rb); break;
        default: code_generator_set_error(generator, "vloop op"); return 0;
        }
        if (!ok) {
          return 0;
        }
        pool[nfree++] = rb;
        vstk[nv++] = ra;
        (void)op0;
        (void)op1;
      }
    }
    if (nv != 1) {
      code_generator_set_error(generator, "vloop root");
      return 0;
    }
    if (is_reduce) {
      if (!wcs_avx_vaddpd_ymm(b, 2, 2, vstk[0])) { /* acc += lanes */
        return 0;
      }
    } else if (!wcs_avx_vmovups_mem_ymm(b, dst_reg, 0, vstk[0])) {
      return 0;
    }
  }
  for (int j = 0; j < n_dist; j++) {
    if (!wcs_addsub_reg_imm8(b, kGp[j], 0, 32)) {
      return 0;
    }
  }
  if (has_iota && !wcs_addsub_reg_imm8(b, BINARY_GP_R11, 0, 4)) {
    return 0;
  }
  if (!wcs_addsub_reg_imm8(b, BINARY_GP_R10, 1, 4)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, vec_top)) {
      return 0;
    }
  }

  /* ---- scalar tail: while (count != 0) ---- */
  if (!wcs_patch_here(b, j_tail)) {
    return 0;
  }
  size_t tail_top = b->size;
  size_t j_done = 0;
  if (!wcs_cmp_reg_imm8(b, BINARY_GP_R10, 0) || !wcs_jcc(b, 0x84 /* je */, &j_done)) {
    return 0;
  }
  {
    int pool[4];
    int nfree = pool_n;
    for (int i = 0; i < pool_n; i++) {
      pool[i] = kPool[i];
    }
    int vstk[VLOOP_KERNEL_MAX_NODES];
    int nv = 0;
    for (int i = 0; i < n_nodes; i++) {
      int tag = (int)args[nodes_off + 3 * i].int_value;
      int op0 = (int)args[nodes_off + 3 * i + 1].int_value;
      if (tag <= VLOOP_K_CONST) {
        int R = pool[--nfree];
        int ok = 0;
        if (tag == VLOOP_K_LOAD) {
          ok = wcs_movsd_xmm_mem(b, R, arr_reg[op0], 0);
        } else if (tag == VLOOP_K_CONST) {
          ok = wcs_movsd_xmm_mem(b, R, BINARY_GP_RSP, 32 * op0);
        } else { /* IOTA scalar: (f64)i */
          ok = binary_emit_cvtsi2sd_xmm_reg(b, (BinaryXmmRegister)R,
                                            BINARY_GP_R11);
        }
        if (!ok) {
          return 0;
        }
        vstk[nv++] = R;
      } else {
        int rb = vstk[--nv];
        int ra = vstk[--nv];
        int ok = 0;
        switch (tag) {
        case VLOOP_K_ADD:
          ok = binary_emit_addsd_xmm_xmm(b, (BinaryXmmRegister)ra,
                                         (BinaryXmmRegister)rb);
          break;
        case VLOOP_K_SUB:
          ok = binary_emit_subsd_xmm_xmm(b, (BinaryXmmRegister)ra,
                                         (BinaryXmmRegister)rb);
          break;
        case VLOOP_K_MUL:
          ok = binary_emit_mulsd_xmm_xmm(b, (BinaryXmmRegister)ra,
                                         (BinaryXmmRegister)rb);
          break;
        case VLOOP_K_DIV:
          ok = binary_emit_divsd_xmm_xmm(b, (BinaryXmmRegister)ra,
                                         (BinaryXmmRegister)rb);
          break;
        default: return 0;
        }
        if (!ok) {
          return 0;
        }
        pool[nfree++] = rb;
        vstk[nv++] = ra;
      }
    }
    if (is_reduce) {
      if (!binary_emit_addsd_xmm_xmm(b, BINARY_XMM3, (BinaryXmmRegister)vstk[0])) {
        return 0;
      }
    } else if (!wcs_movsd_mem_xmm(b, dst_reg, 0, vstk[0])) {
      return 0;
    }
  }
  for (int j = 0; j < n_dist; j++) {
    if (!wcs_addsub_reg_imm8(b, kGp[j], 0, 8)) {
      return 0;
    }
  }
  if (has_iota && !wcs_addsub_reg_imm8(b, BINARY_GP_R11, 0, 1)) {
    return 0;
  }
  if (!wcs_addsub_reg_imm8(b, BINARY_GP_R10, 1, 1)) {
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
  if (is_reduce) {
    /* Fold ymm2 + xmm3 -> RAX (a double's bits) and store to the accumulator.
     * wcs_reduce_pd_acc_to_rax does its own vextractf128 + vzeroupper, so it
     * MUST run before any standalone vzeroupper (which would zero ymm2's upper
     * lanes and drop two accumulator lanes). */
    if (!wcs_reduce_pd_acc_to_rax(b)) {
      return 0;
    }
    if (cbytes && !binary_emit_add_rsp_imm32(b, cbytes)) {
      return 0;
    }
    return code_generator_binary_emit_destination_store(generator, context,
                                                        &instruction->dest,
                                                        BINARY_GP_RAX);
  }
  if (!wcs_avx_vzeroupper(b)) {
    return 0;
  }
  if (cbytes && !binary_emit_add_rsp_imm32(b, cbytes)) {
    return 0;
  }
  return 1;
}

/* Outer-loop lane vectorizer (IR_OP_SIMD_OUTER_LANE_F64). Runs 4 outer-loop
 * iterations of an outer-invariant inner serial recurrence in lockstep f64x4
 * lanes (genuinely running all the inner work 4-wide to hide the recurrence
 * latency), then accumulates the lane-identical result into the total with
 * exact scalar adds (bit-identical to the scalar loop). See the recognizer
 * ir_outer_vectorize_pass for the arguments[] layout. */
/* uniform micro-ops (mirror of the recognizer's OL_U_*). */
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

/* Emit a uniform-of-base program (micro-ops at args[prog_off..]) over the integer
 * value in val_gpr, leaving the float result in res_xmm. All ops VEX-encoded to
 * avoid AVX<->SSE transitions. Uses work_gpr + ctmp_xmm + RAX as scratch; float
 * consts are read from the stack array at [rsp + 32*idx]. */
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
  int inner_cmp = (int)args[0].int_value;  /* 0:< 1:<= */
  long long istep = args[1].int_value;
  int n_chain = (int)args[2].int_value;
  int n_unif = (int)args[3].int_value;
  int n_fconst = (int)args[4].int_value;
  long long i0 = args[5].int_value;
  int init_mode = (int)args[6].int_value;     /* 0: const seed; 1: seed(p) */
  double iacc_init = args[7].float_value;
  if (n_chain <= 0 || n_chain > 8 || n_unif < 0 || n_unif > 8 || n_fconst < 0 ||
      n_fconst > 16 || istep <= 0 || (init_mode != 0 && init_mode != 1)) {
    code_generator_set_error(generator, "Bad simd_outer_lane_f64 encoding");
    return 0;
  }

  /* K independent lane-accumulator chains run in lockstep so the divide unit
   * stays saturated (a single serial divpd chain is latency-bound; K>=2 makes it
   * throughput-bound). Each chain covers 4 outer iterations, so a super-group
   * covers 4*K. init_mode 0 -> all lanes identical (lane0 fast path). init_mode 1
   * -> each lane seeded from its own outer index p (lanes diverge), summed by
   * per-lane extraction in p order (still bit-exact). K=3 fits volatile xmm0-5:
   * acc=ymm0/1/2, term=ymm3, uniform-scalar=xmm4, uniform-const=xmm5; total and
   * the per-lane seed array live in stack slots. */
  const int K = 3;
  const int kAcc[3] = {0, 1, 2};
  const int GROUP = 4 * K;

  /* Locate the sections of arguments[]. */
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

  /* Stack frame: [0 .. 32*n_fconst) broadcast const array; then an 8-byte running
   * total slot; then (init_mode 1 only) a 32*K seed-vector scratch array. */
  uint64_t init_bits = 0;
  memcpy(&init_bits, &iacc_init, sizeof(init_bits));
  int total_off = 32 * n_fconst;
  int seedarr_off = total_off + 16;
  uint32_t cbytes =
      (uint32_t)(seedarr_off + (init_mode == 1 ? 32 * K : 0)); /* 16-aligned */
  if (!binary_emit_sub_rsp_imm32(b, cbytes)) {
    return 0;
  }
  /* total = prior accumulator value (dest) -> stack slot. */
  if (!code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->dest,
                                               BINARY_GP_RAX) ||
      !binary_emit_movq_xmm_reg(b, BINARY_XMM4, BINARY_GP_RAX) ||
      !wcs_movsd_mem_xmm(b, BINARY_GP_RSP, total_off, 4) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->lhs,
                                               BINARY_GP_R9) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->rhs,
                                               BINARY_GP_R8)) {
    return 0;
  }
  /* Broadcast each fconst to its stack slot. */
  for (int c = 0; c < n_fconst; c++) {
    double dv = args[fconst_off + c].float_value;
    uint64_t bits = 0;
    memcpy(&bits, &dv, sizeof(bits));
    if (!binary_emit_mov_reg_imm64(b, BINARY_GP_RAX, bits) ||
        !binary_emit_movq_xmm_reg(b, BINARY_XMM0, BINARY_GP_RAX) ||
        !wcs_avx_vbroadcastsd_ymm_xmm(b, 0, 0) ||
        !wcs_avx_vmovups_mem_ymm(b, BINARY_GP_RSP, 32 * c, 0)) {
      return 0;
    }
  }
  /* rdx = 0 (outer counter). */
  if (!binary_emit_mov_reg_imm64(b, BINARY_GP_RDX, 0)) {
    return 0;
  }

  size_t outer_top = b->size;
  size_t j_outer_end = 0;
  if (!binary_emit_cmp_reg_reg(b, BINARY_GP_RDX, BINARY_GP_R9) ||
      !wcs_jcc(b, 0x8D /* jge */, &j_outer_end)) {
    return 0;
  }
  /* Seed the K lane accumulators. */
  if (init_mode == 0) {
    /* identical: every lane = broadcast(const init). */
    if (!binary_emit_mov_reg_imm64(b, BINARY_GP_RAX, init_bits) ||
        !binary_emit_movq_xmm_reg(b, BINARY_XMM4, BINARY_GP_RAX)) {
      return 0;
    }
    for (int k = 0; k < K; k++) {
      if (!wcs_avx_vbroadcastsd_ymm_xmm(b, kAcc[k], 4)) {
        return 0;
      }
    }
  } else {
    /* divergent: lane g (global) seeds from seed(p+g). Evaluate scalar per lane
     * into the stack seed array, then load each accumulator's 4 lanes. */
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
      !wcs_jcc(b, inner_cmp ? 0x8F /* jg */ : 0x8D /* jge */, &j_inner_end)) {
    return 0;
  }
  /* Replay the recurrence chain on the lane vector. */
  for (int s = 0; s < n_chain; s++) {
    int c_op = (int)args[chain_off + 4 * s].int_value;
    int side = (int)args[chain_off + 4 * s + 1].int_value;
    int kind = (int)args[chain_off + 4 * s + 2].int_value;
    int idx = (int)args[chain_off + 4 * s + 3].int_value;
    if (kind == 0) { /* const term: broadcast already on the stack -> ymm3 */
      if (idx < 0 || idx >= n_fconst ||
          !wcs_avx_vmovups_ymm_mem(b, 3, BINARY_GP_RSP, 32 * idx)) {
        code_generator_set_error(generator, "vloop outer: const idx");
        return 0;
      }
    } else { /* uniform-of-i term: evaluate scalar (xmm4) then broadcast -> ymm3 */
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
    /* Apply term (ymm3) to every lane accumulator: acc = acc OP term (side 0)
     * or acc = term OP acc (side 1). The K vops are independent -> they fill the
     * divide/FP pipeline. */
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
  /* i += istep; loop. */
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
  /* xmm5 = running total (reloaded from the stack slot). */
  if (!wcs_avx_vmovsd_xmm_mem(b, 5, BINARY_GP_RSP, total_off)) {
    return 0;
  }
  if (init_mode == 0) {
    /* identical lanes: lane0 of ymm0 == S. Add S to total for each of the
     * min(GROUP, P-p) outer iterations this super-group covers (exact). */
    if (!binary_emit_mov_reg_reg(b, BINARY_GP_R11, BINARY_GP_R9) ||
        !binary_emit_alu_reg_reg(b, 0x29 /* sub */, BINARY_GP_R11, BINARY_GP_RDX) ||
        !binary_emit_mov_reg_imm64(b, BINARY_GP_RAX, (uint64_t)GROUP) ||
        !wcs_cmp_reg_imm32(b, BINARY_GP_R11, (uint32_t)GROUP) ||
        !binary_emit_cmovcc_reg_reg(b, 0x4F /* cmovg */, BINARY_GP_R11,
                                    BINARY_GP_RAX)) {
      return 0;
    }
    size_t add_top = b->size;
    if (!wcs_avx_vaddsd(b, 5, 5, 0) || /* total += S (lane0) */
        !wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 1) ||
        !wcs_addsub_reg_imm8(b, BINARY_GP_R11, 1, 1)) {
      return 0;
    }
    size_t j_add_back = 0;
    if (!wcs_jcc(b, 0x85 /* jnz */, &j_add_back) ||
        !wcs_patch_to(b, j_add_back, add_top)) {
      return 0;
    }
    /* rdx was advanced by the group size inside the add-loop. */
  } else {
    /* divergent lanes: lane g (global) holds S for outer iteration p+g. Add each
     * valid lane (p+g < P) to total in p order -> bit-exact. */
    for (int g = 0; g < GROUP; g++) {
      int k = g / 4, j = g % 4;
      int acc = kAcc[k];
      size_t j_skip = 0;
      /* if (p + g >= P) skip this lane. */
      if (!binary_emit_mov_reg_reg(b, BINARY_GP_R11, BINARY_GP_RDX) ||
          !wcs_addsub_reg_imm8(b, BINARY_GP_R11, 0, (unsigned char)g) ||
          !binary_emit_cmp_reg_reg(b, BINARY_GP_R11, BINARY_GP_R9) ||
          !wcs_jcc(b, 0x8D /* jge */, &j_skip)) {
        return 0;
      }
      /* bring lane j of `acc` into xmm4 low, then total += it. */
      int ok = 1;
      if (j == 0) {
        ok = wcs_avx_vaddsd(b, 5, 5, acc); /* lane0 = acc low */
      } else if (j == 1) {
        ok = wcs_avx_vunpckhpd_xmm(b, 4, acc, acc) && wcs_avx_vaddsd(b, 5, 5, 4);
      } else if (j == 2) {
        ok = wcs_avx_vextractf128(b, 4, acc, 1) && wcs_avx_vaddsd(b, 5, 5, 4);
      } else { /* j == 3 */
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
  /* Load the final total from the stack, free the frame, store to dest. */
  if (!wcs_movsd_xmm_mem(b, 0, BINARY_GP_RSP, total_off) ||
      !binary_emit_movq_reg_xmm(b, BINARY_GP_RAX, BINARY_XMM0)) {
    return 0;
  }
  if (!binary_emit_add_rsp_imm32(b, cbytes)) {
    return 0;
  }
  return code_generator_binary_emit_destination_store(generator, context,
                                                      &instruction->dest,
                                                      BINARY_GP_RAX);
}

/* Counted-loop counter reduction:  dest += sum_{i=0}^{n-1} (int64)trunc(CHAIN(i))
 * where CHAIN is the float64 expression applied to (float64)i described by the
 * (op,const) pairs in arguments[1..]. arguments[0] = trip count n (constant).
 *
 * The recognizer (ir_simd_i2f_reduce_pass) has already proven every per-element
 * value fits int32 and the integer sum stays < 2^52, so this kernel:
 *   - processes 4 lanes/iter: i_vec=[k..k+3] -> vcvtdq2pd -> replay CHAIN with
 *     packed ops (bit-identical to scalar) -> vcvttpd2dq (exact truncate) ->
 *     vcvtdq2pd back -> accumulate in a packed-f64 accumulator (ymm2);
 *   - mops up the < 4 tail with the identical scalar chain into xmm3;
 *   - folds ymm2+xmm3 to one double (an exact integer), truncates once to int64,
 *     and adds it to dest's prior value.
 * Registers: rcx=i, r8=vec_end, r9=n, rax/r10 scratch; ymm5=iota[0,1,2,3],
 * ymm0=chain value, ymm1=scratch/broadcast, ymm2=packed acc, xmm3=scalar tail. */
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

  /* acc(ymm2)=0, tail(xmm3)=0, iota(ymm5)=[0,1,2,3], i(rcx)=0,
   * vec_end(r8), n(r9). */
  if (!wcs_avx_vpxor_ymm(b, 2, 2, 2) || !wcs_avx_vpxor_ymm(b, 3, 3, 3) ||
      !binary_emit_mov_reg_imm64(b, BINARY_GP_RAX, 0x0000000100000000ULL) ||
      !binary_emit_movq_xmm_reg(b, BINARY_XMM0, BINARY_GP_RAX) ||
      !binary_emit_mov_reg_imm64(b, BINARY_GP_RAX, 0x0000000300000002ULL) ||
      !binary_emit_movq_xmm_reg(b, BINARY_XMM1, BINARY_GP_RAX) ||
      !wcs_avx_vpunpcklqdq_xmm(b, 5, 0, 1) ||
      !binary_emit_mov_reg_imm64(b, BINARY_GP_RCX, 0) ||
      !binary_emit_mov_reg_imm64(b, BINARY_GP_R8, (uint64_t)vec_end) ||
      !binary_emit_mov_reg_imm64(b, BINARY_GP_R9, (uint64_t)bound)) {
    return 0;
  }

  /* Hoist the chain constants: broadcast each once into a 32-byte stack slot so
   * the loop body re-reads them from L1 instead of rematerializing a movabs +
   * vbroadcastsd every iteration. r11 -> the constant array base. */
  if (!binary_emit_sub_rsp_imm32(b, const_bytes) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_R11, BINARY_GP_RSP)) {
    return 0;
  }
  for (size_t s = 0; s < nsteps; s++) {
    double kd = instruction->arguments[2 + 2 * s].float_value;
    uint64_t kbits = 0;
    memcpy(&kbits, &kd, sizeof(kbits));
    if (!binary_emit_mov_reg_imm64(b, BINARY_GP_RAX, kbits) ||
        !binary_emit_movq_xmm_reg(b, BINARY_XMM1, BINARY_GP_RAX) ||
        !wcs_avx_vbroadcastsd_ymm_xmm(b, 1, 1) ||
        !wcs_avx_vmovups_mem_ymm(b, BINARY_GP_R11, (int)(32 * s), 1)) {
      return 0;
    }
  }

  /* ---- vector loop: while (i < vec_end) ---- */
  vec_top = b->size;
  if (!binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R8) ||
      !wcs_jcc(b, 0x8D /* jge */, &j_after_vec) ||
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
    case 0: ok = wcs_avx_vmulpd_ymm(b, 0, 0, 1); break; /* x*k */
    case 1: ok = wcs_avx_vaddpd_ymm(b, 0, 0, 1); break; /* x+k */
    case 2: ok = wcs_avx_vsubpd_ymm(b, 0, 0, 1); break; /* x-k */
    case 3: ok = wcs_avx_vsubpd_ymm(b, 0, 1, 0); break; /* k-x */
    case 4: ok = wcs_avx_vdivpd_ymm(b, 0, 0, 1); break; /* x/k */
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

  /* ---- scalar tail: while (i < n) ---- */
  if (!wcs_patch_here(b, j_after_vec)) {
    return 0;
  }
  rem_top = b->size;
  if (!binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R9) ||
      !wcs_jcc(b, 0x8D /* jge */, &j_done) ||
      !binary_emit_cvtsi2sd_xmm_reg(b, BINARY_XMM0, BINARY_GP_RCX)) {
    return 0;
  }
  for (size_t s = 0; s < nsteps; s++) {
    int op = (int)instruction->arguments[1 + 2 * s].int_value;
    if (!wcs_movsd_xmm_mem(b, 1, BINARY_GP_R11, (int)(32 * s))) {
      return 0;
    }
    int ok = 0;
    switch (op) {
    case 0: ok = binary_emit_mulsd_xmm_xmm(b, BINARY_XMM0, BINARY_XMM1); break;
    case 1: ok = binary_emit_addsd_xmm_xmm(b, BINARY_XMM0, BINARY_XMM1); break;
    case 2: ok = binary_emit_subsd_xmm_xmm(b, BINARY_XMM0, BINARY_XMM1); break;
    case 3: /* k - x: compute in xmm1, move back to xmm0 */
      ok = binary_emit_subsd_xmm_xmm(b, BINARY_XMM1, BINARY_XMM0) &&
           wcs_sse_f2(b, 0x10, 0, 1) /* movsd xmm0, xmm1 */;
      break;
    case 4: ok = binary_emit_divsd_xmm_xmm(b, BINARY_XMM0, BINARY_XMM1); break;
    default: code_generator_set_error(generator, "bad i2f step op"); return 0;
    }
    if (!ok) {
      return 0;
    }
  }
  /* trunc to int64 then back to an integer-valued double, add into the tail. */
  if (!binary_emit_cvttsd2si_reg_xmm(b, BINARY_GP_RAX, BINARY_XMM0) ||
      !binary_emit_cvtsi2sd_xmm_reg(b, BINARY_XMM0, BINARY_GP_RAX) ||
      !binary_emit_addsd_xmm_xmm(b, BINARY_XMM3, BINARY_XMM0) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 1) ||
      !wcs_jcc(b, 0, &back) || !wcs_patch_to(b, back, rem_top)) {
    return 0;
  }

  /* ---- fold + finalize: total = (int64)(reduce(ymm2)+xmm3); dest += total ---- */
  if (!wcs_patch_here(b, j_done) ||
      !binary_emit_add_rsp_imm32(b, const_bytes) /* free the constant array */ ||
      !wcs_reduce_pd_acc_to_rax(b) ||
      !binary_emit_movq_xmm_reg(b, BINARY_XMM0, BINARY_GP_RAX) ||
      !binary_emit_cvttsd2si_reg_xmm(b, BINARY_GP_R10, BINARY_XMM0) ||
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

/* Float32 dot product of a[0..n-1]*b[0..n-1], ADDED to dest's prior value.
 * dest = float32 sum, lhs = a, rhs = b, arguments[0] = element count. */
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
      !binary_emit_movd_xmm_reg(b, BINARY_XMM3, BINARY_GP_RAX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->lhs,
                                               BINARY_GP_RCX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->rhs,
                                               BINARY_GP_RDX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->arguments[0],
                                               BINARY_GP_R8) ||
      !wcs_mov_reg_reg32(b, BINARY_GP_R9, BINARY_GP_R8) ||
      !wcs_shift_reg_imm(b, BINARY_GP_R9, 0, 2) ||
      !wcs_add_reg_reg64(b, BINARY_GP_R9, BINARY_GP_RCX) ||
      !wcs_avx_vpxor_ymm(b, 2, 2, 2) ||
      !wcs_avx_vpxor_ymm(b, 4, 4, 4)) {
    return 0;
  }

  loop_top = b->size;
  if (!binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R9) ||
      !wcs_jcc(b, 0x83 /* jae */, &j_done) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_R10, BINARY_GP_R9) ||
      !binary_emit_alu_reg_reg(b, 0x29, BINARY_GP_R10, BINARY_GP_RCX) ||
      !wcs_cmp_reg_imm32(b, BINARY_GP_R10, 64) ||
      !wcs_jcc(b, 0x83 /* jae */, &j_vec2) ||
      !wcs_cmp_reg_imm32(b, BINARY_GP_R10, 32) ||
      !wcs_jcc(b, 0x83 /* jae */, &j_vec) ||
      !wcs_jcc(b, 0, &j_scalar)) {
    return 0;
  }

  /* Two-accumulator FMA unroll: 16 floats/iter into the independent ymm2/ymm4
   * chains via vfmadd231ps. */
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
      !wcs_movss_xmm_mem(b, 0, BINARY_GP_RCX, 0) ||
      !wcs_movss_xmm_mem(b, 1, BINARY_GP_RDX, 0) ||
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

/* ===================== vectorized exp() (float32) ========================= */
/* AVX2 Cephes single-precision exp, 8 lanes at a time. exp(x) = 2^k * exp(r)
 * with k = floor(x/ln2 + 0.5) and r = x - k*ln2 reduced into a small interval,
 * exp(r) a degree-6 polynomial. Constants are broadcast on demand from a GP
 * register (VEX vmovd + vpbroadcastd) so the kernel touches only the Win64
 * volatile xmm0..5, with no callee-saved vector registers to preserve. The
 * scalar exp_f32 the recognizer matches uses the identical polynomial, so the
 * vectorized result tracks it (validated within tolerance and against libm). */

static uint32_t exp_f32_bits(float f) {
  uint32_t u;
  memcpy(&u, &f, 4);
  return u;
}

/* Constant pool laid out at [rsp + EXP_POOL_OFF]; 14 float32 slots. The kernel
 * fills it once, and exp_compute broadcasts each constant from there per use
 * (vbroadcastss, a single L1 op) -- far cheaper than re-materializing constants
 * through a GP register every iteration, and it leaves the callee-saved vector
 * registers untouched. */
#define EXP_POOL_OFF 32
#define EXP_C(i) (EXP_POOL_OFF + (i) * 4)
/* slot 0=clamp_hi 1=clamp_lo 2=LOG2EF 3=0.5 4=ln2_hi 5=ln2_lo 6..11=p0..p5
 * 12=1.0 13=127(int). */
static const float exp_pool_consts[13] = {
    88.3762626647949f,    -88.3762626647949f,  1.44269504088896341f,
    0.5f,                 0.693359375f,        -2.12194440e-4f,
    1.9875691500E-4f,     1.3981999507E-3f,    8.3334519073E-3f,
    4.1665795894E-2f,     1.6666665459E-1f,    5.0000001201E-1f,
    1.0f};

/* x in ymm0 -> exp(x) in ymm2. Constants from the [rsp] pool. Clobbers
 * ymm0..5 (RAX is untouched). */
static int exp_compute(BinaryCodeBuffer *b) {
  if (!wcs_avx_vbroadcastss_ymm_mem(b, 4, BINARY_GP_RSP, EXP_C(0)) ||
      !wcs_avx_vminps_ymm(b, 0, 0, 4) ||
      !wcs_avx_vbroadcastss_ymm_mem(b, 4, BINARY_GP_RSP, EXP_C(1)) ||
      !wcs_avx_vmaxps_ymm(b, 0, 0, 4)) {
    return 0;
  }
  /* fx = floor(x * LOG2EF + 0.5) */
  if (!wcs_avx_vbroadcastss_ymm_mem(b, 1, BINARY_GP_RSP, EXP_C(2)) ||
      !wcs_avx_vmulps_ymm(b, 1, 0, 1) ||
      !wcs_avx_vbroadcastss_ymm_mem(b, 4, BINARY_GP_RSP, EXP_C(3)) ||
      !wcs_avx_vaddps_ymm(b, 1, 1, 4) ||
      !wcs_avx_vroundps_ymm(b, 1, 1, 1 /* floor */)) {
    return 0;
  }
  /* r = x - fx*ln2_hi - fx*ln2_lo  (Cody-Waite split) */
  if (!wcs_avx_vbroadcastss_ymm_mem(b, 4, BINARY_GP_RSP, EXP_C(4)) ||
      !wcs_avx_vmulps_ymm(b, 5, 1, 4) || !wcs_avx_vsubps_ymm(b, 0, 0, 5) ||
      !wcs_avx_vbroadcastss_ymm_mem(b, 4, BINARY_GP_RSP, EXP_C(5)) ||
      !wcs_avx_vmulps_ymm(b, 5, 1, 4) || !wcs_avx_vsubps_ymm(b, 0, 0, 5)) {
    return 0;
  }
  /* Horner: y = ((((p0*r+p1)*r+p2)*r+p3)*r+p4)*r+p5 */
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
  /* y = y*r*r + r + 1 */
  if (!wcs_avx_vmulps_ymm(b, 3, 0, 0) || !wcs_avx_vmulps_ymm(b, 2, 2, 3) ||
      !wcs_avx_vaddps_ymm(b, 2, 2, 0) ||
      !wcs_avx_vbroadcastss_ymm_mem(b, 4, BINARY_GP_RSP, EXP_C(12)) ||
      !wcs_avx_vaddps_ymm(b, 2, 2, 4)) {
    return 0;
  }
  /* 2^fx via ((int)fx + 127) << 23 reinterpreted as float; y *= 2^fx */
  if (!wcs_avx_vcvttps2dq_ymm(b, 1, 1) ||
      !wcs_avx_vbroadcastss_ymm_mem(b, 4, BINARY_GP_RSP, EXP_C(13)) ||
      !wcs_avx_vpaddd_ymm(b, 1, 1, 4) || !wcs_avx_vpslld_ymm_imm(b, 1, 1, 23) ||
      !wcs_avx_vmulps_ymm(b, 2, 2, 1)) {
    return 0;
  }
  return 1;
}

/* Fill the [rsp + EXP_POOL_OFF] constant pool (called once after the kernel's
 * stack reservation). */
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

/* IR_OP_SIMD_EXP_F32: in-place a[i] = exp(a[i]) over a float32 array.
 * dest = array base, arguments[0] = element count. */
int code_generator_binary_emit_simd_exp_f32(CodeGenerator *generator,
                                            BinaryFunctionContext *context,
                                            const IRInstruction *instruction) {
  BinaryCodeBuffer *b = NULL;
  size_t loop_top = 0, done_main = 0, small = 0, fin = 0, noclamp = 0;
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
      !binary_emit_mov_reg_reg(b, BINARY_GP_R9, BINARY_GP_R8) ||
      !binary_emit_shift_reg_imm8(b, 4, BINARY_GP_R9, 2) ||
      !wcs_add_reg_reg64(b, BINARY_GP_R9, BINARY_GP_RCX)) {
    return 0;
  }
  /* Reserve 96 bytes once: [rsp+0..31] = the n<8 scratch buffer, [rsp+32..87] =
   * the constant pool. rsp stays put for the whole kernel (no calls), so both
   * the pool and the buffer are at fixed offsets. R9 (end) is a heap pointer,
   * unaffected by the stack reservation. */
  if (!binary_emit_sub_rsp_imm32(b, 96) || !exp_fill_pool(b)) {
    return 0;
  }
  if (!wcs_cmp_reg_imm32(b, BINARY_GP_R8, 8) ||
      !wcs_jcc(b, 0x82 /* jb */, &small)) {
    return 0;
  }
  /* R9 = last8 = end - 32; 8-wide loop overlapping the final vector. */
  if (!wcs_addsub_reg_imm8(b, BINARY_GP_R9, 1 /* sub */, 32)) {
    return 0;
  }
  loop_top = b->size;
  if (!wcs_avx_vmovups_ymm_mem(b, 0, BINARY_GP_RCX, 0) || !exp_compute(b) ||
      !wcs_avx_vmovups_mem_ymm(b, BINARY_GP_RCX, 0, 2) ||
      !binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R9) ||
      !wcs_jcc(b, 0x83 /* jae */, &done_main) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 32) ||
      !binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R9) ||
      !wcs_jcc(b, 0x86 /* jbe */, &noclamp) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R9)) {
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

  /* n < 8: copy n floats into the [rsp+0] scratch buffer (already reserved),
   * exp it 8-wide, copy the n results back. R9 saves the base; RCX/R10/R11/RAX
   * are scratch. */
  if (!wcs_patch_here(b, small) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_R9, BINARY_GP_RCX) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_R11, BINARY_GP_RSP) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_R10, BINARY_GP_R8)) {
    return 0;
  }
  {
    size_t ci = b->size, di = 0;
    if (!binary_emit_test_reg_reg(b, BINARY_GP_R10) ||
        !wcs_jcc(b, 0x84 /* jz */, &di) ||
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
        !wcs_jcc(b, 0x84 /* jz */, &dout) ||
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
