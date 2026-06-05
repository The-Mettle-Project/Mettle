#include "codegen/binary/internal.h"
#include "codegen/binary/simd_internal.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Integer-array SIMD kernels (AVX2-widened reductions, element-wise maps, sort/search). Low-level encoders live in simd_encoders.c; see simd_internal.h. */

/* Lower IR_OP_SIMD_SUM_I32: add sum of base[0..len-1] int32s into dest. */
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

  /* rax=sum, rcx=base, edx=i, r8d=len */
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
  /* ymm2 = four int64 partial sums; rax = prior value + scalar-tail sums. */
  if (!wcs_xor_self32(b, BINARY_GP_RDX) ||
      !wcs_avx_vpxor_ymm(b, 2, 2, 2)) {
    return 0;
  }

  loop_top = b->size;
  if (!wcs_cmp_reg_reg32(b, BINARY_GP_RDX, BINARY_GP_R8)) return 0;
  if (!wcs_jcc(b, 0x83 /* jae */, &j_done)) return 0;

  /* len - i >= 8 ? */
  if (!wcs_mov_reg_reg32(b, BINARY_GP_R9, BINARY_GP_R8) ||
      !wcs_sub_reg_reg32(b, BINARY_GP_R9, BINARY_GP_RDX) ||
      !wcs_cmp_reg_imm32(b, BINARY_GP_R9, 8) ||
      !wcs_jcc(b, 0x83 /* jae */, &j_vec)) {
    return 0;
  }
  if (!wcs_jcc(b, 0, &j_scalar)) return 0;

  /* AVX2: two vpmovsxdq loads sign-extend 8 int32 to int64, summed into ymm2.
   * Accumulating in 64-bit lanes preserves the int64 result semantics without
   * the per-lane extract the SSE path used. */
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

  /* Reduce the four int64 lanes of ymm2 into rax (drop AVX state first). */
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

/* Lower IR_OP_SIMD_SUM_U8: add the unsigned sum of base[0..len-1] uint8s into
 * dest (an int64). Uses vpsadbw against zero to fold 32 bytes per iteration
 * into four int64 lane accumulators, then a scalar movzx tail. */
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

  /* rax=sum, rcx=base, edx=i, r8d=len */
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
  /* ymm2 = four int64 partial sums; ymm3 = zero operand for vpsadbw. */
  if (!wcs_xor_self32(b, BINARY_GP_RDX) ||
      !wcs_avx_vpxor_ymm(b, 2, 2, 2) ||
      !wcs_avx_vpxor_ymm(b, 3, 3, 3)) {
    return 0;
  }

  loop_top = b->size;
  if (!wcs_cmp_reg_reg32(b, BINARY_GP_RDX, BINARY_GP_R8)) return 0;
  if (!wcs_jcc(b, 0x83 /* jae */, &j_done)) return 0;

  /* len - i >= 32 ? */
  if (!wcs_mov_reg_reg32(b, BINARY_GP_R9, BINARY_GP_R8) ||
      !wcs_sub_reg_reg32(b, BINARY_GP_R9, BINARY_GP_RDX) ||
      !wcs_cmp_reg_imm32(b, BINARY_GP_R9, 32) ||
      !wcs_jcc(b, 0x83 /* jae */, &j_vec)) {
    return 0;
  }
  if (!wcs_jcc(b, 0, &j_scalar)) return 0;

  /* AVX2: vpsadbw of 32 bytes against zero yields four 64-bit lane sums (each
   * <= 8*255), accumulated into ymm2 with vpaddq. */
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

  /* Scalar tail: zero-extend one byte and add (unsigned). */
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

  /* Reduce the four int64 lanes of ymm2 into rax (drop AVX state first). */
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

/* Emit one byte-map chain step applied to the 16 packed bytes in xmm0. The
 * step's broadcast constant lives at [rsp+bcast_off]; the 0x00FF word mask (for
 * the multiply path's pack) at [rsp+mask_off]. Scratch: xmm1-xmm5. */
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
    /* Widen bytes to words, multiply by the broadcast word, keep the low byte
     * of each product (mask so packuswb does not saturate), repack. */
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

/* Scalar-tail equivalent: apply one step to the byte value in r10 (32-bit ALU;
 * the byte store truncates, and these ops agree mod 256 with the vector path). */
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

/* Lower IR_OP_SIMD_BYTE_MAP: in-place apply the constant byte-op chain to
 * base[0..len-1], 16 bytes per VEX.128 iteration plus a scalar tail. */
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
  cbytes = (uint32_t)(16 * ((int)nsteps + 1)); /* per-step broadcasts + mask */

  if (!binary_emit_sub_rsp_imm32(b, cbytes)) {
    return 0;
  }

  /* Build each step's 16-byte broadcast (and the word mask) into the scratch
   * frame. This setup runs once; the hot loop below is pure VEX. */
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

  /* rcx=base, r8=len, rdx=i. Loaded after the broadcasts (which use rax/xmm0). */
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
  if (!wcs_cmp_reg_reg32(b, BINARY_GP_RDX, BINARY_GP_R8) ||
      !wcs_jcc(b, 0x83 /* jae */, &j_done)) {
    return 0;
  }
  /* len - i >= 16 ? */
  if (!wcs_mov_reg_reg32(b, BINARY_GP_R9, BINARY_GP_R8) ||
      !wcs_sub_reg_reg32(b, BINARY_GP_R9, BINARY_GP_RDX) ||
      !wcs_cmp_reg_imm32(b, BINARY_GP_R9, 16) ||
      !wcs_jcc(b, 0x83 /* jae */, &j_vec) ||
      !wcs_jcc(b, 0, &j_scalar)) {
    return 0;
  }

  /* Vector path: 16 bytes through the chain, stored back in place. */
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

  /* Scalar tail: one byte through the same chain. */
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

/* In-place int32 insertion sort with SSE2 chunk scan and 16-byte shifts. */
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

  /* rcx=base, r8d=len, edx=i, r9d=key, r10=scan_ptr, r11d=current */
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
  if (!wcs_cmp_reg_reg32(b, BINARY_GP_RDX, BINARY_GP_R8) ||
      !wcs_jcc(b, 0x8D /* jge */, &j_done)) {
    return 0;
  }

  if (!binary_emit_lea_reg_base_index_scale_disp(
          b, BINARY_GP_RAX, BINARY_GP_RCX, BINARY_GP_RDX, 4, 0) ||
      !binary_emit_mov_reg_mem32(b, BINARY_GP_R9, BINARY_GP_RAX, 0) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_R10, BINARY_GP_RAX) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_R10, 1, 4)) {
    return 0;
  }

  inner_loop = b->size;
  if (!binary_emit_cmp_reg_reg(b, BINARY_GP_R10, BINARY_GP_RCX) ||
      !wcs_jcc(b, 0x82 /* jb */, &j_insert_from_bound) ||
      !binary_emit_mov_reg_mem32(b, BINARY_GP_R11, BINARY_GP_R10, 0) ||
      !wcs_cmp_reg_reg32(b, BINARY_GP_R11, BINARY_GP_R9) ||
      !wcs_jcc(b, 0x8E /* jle */, &j_insert_from_le) ||
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

  /* r10=lo, r11=hi, rsi=arr, r9d=key, rax=mid, rcx=arr[mid]. All Win64 volatile
   * GPs (rax/rcx/rdx/r8..r11) are already in use, so the array base must live in
   * a callee-saved register; RSI is nonvolatile on Win64, so save/restore it
   * here (the function prologue does not know this special codegen uses it).
   * On SysV RSI is caller-saved, so the push/pop is merely harmless there. */
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
      !wcs_jcc(b, 0x8D /* jge */, &j_done) ||
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
      !binary_emit_cmovcc_reg_reg(b, 0x4C /* cmovl */, BINARY_GP_R10,
                                  BINARY_GP_R8) ||
      !binary_emit_cmovcc_reg_reg(b, 0x4D /* cmovge */, BINARY_GP_R11,
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
  /* Restore the caller's RSI (saved above). The destination store used R10, not
   * RSI, so it is safe to pop here on the single success path. */
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

  /* rax=sum, rcx=src, rdx=dst, r8=src_end, xmm4/xmm5=mul/add */
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
      !wcs_jcc(b, 0x83 /* jae */, &j_done)) {
    return 0;
  }

  if (!binary_emit_mov_reg_reg(b, BINARY_GP_R9, BINARY_GP_R8) ||
      !binary_emit_alu_reg_reg(b, 0x29, BINARY_GP_R9, BINARY_GP_RCX) ||
      !wcs_cmp_reg_imm32(b, BINARY_GP_R9, 64) ||
      !wcs_jcc(b, 0x83 /* jae */, &j_vec) ||
      !wcs_jcc(b, 0, &j_scalar)) {
    return 0;
  }

  /* AVX2: two 32-byte lanes per 64-byte block. Each is multiplied by the
   * broadcast mul (vpmulld), offset by add (vpaddd), stored, and folded into
   * the ymm2 int32 sum accumulator. */
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

int code_generator_binary_emit_simd_reverse_copy_i32(
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

  /* rax=sum, rcx=src (last elem), rdx=dst walk, r8=dst_end, r10=src_base */
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
      !wcs_jcc(b, 0x83 /* jae */, &j_done)) {
    return 0;
  }

  if (!binary_emit_mov_reg_reg(b, BINARY_GP_R9, BINARY_GP_R8) ||
      !binary_emit_alu_reg_reg(b, 0x29, BINARY_GP_R9, BINARY_GP_RDX) ||
      !wcs_cmp_reg_imm32(b, BINARY_GP_R9, 64) ||
      !wcs_jcc(b, 0x82 /* jb */, &j_scalar) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_R9, BINARY_GP_RCX) ||
      !binary_emit_alu_reg_reg(b, 0x29, BINARY_GP_R9, BINARY_GP_R10) ||
      !wcs_cmp_reg_imm32(b, BINARY_GP_R9, 60) ||
      !wcs_jcc(b, 0x82 /* jb */, &j_scalar) ||
      !wcs_jcc(b, 0, &j_vec)) {
    return 0;
  }

  /* AVX2: reverse eight int32 per 32-byte lane. The 32 bytes ending at rcx
   * (disp -28) load as ascending memory [src[k-7]..src[k]]; vpshufd 0x1B
   * reverses dwords within each 128-bit half, then vperm2i128 swaps the halves,
   * giving the full 8-lane reverse [src[k]..src[k-7]] to store ascending. Two
   * such lanes cover the 64-byte block (second ends at rcx-32 -> disp -60). */
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

  if (!wcs_patch_here(b, j_scalar) ||
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
      !wcs_jcc(b, 0x83 /* jae */, &j_done)) {
    return 0;
  }

  if (!binary_emit_mov_reg_reg(b, BINARY_GP_R9, BINARY_GP_R8) ||
      !binary_emit_alu_reg_reg(b, 0x29, BINARY_GP_R9, BINARY_GP_RCX) ||
      !wcs_cmp_reg_imm32(b, BINARY_GP_R9, 32) ||
      !wcs_jcc(b, 0x83 /* jae */, &j_vec) ||
      !wcs_jcc(b, 0, &j_scalar)) {
    return 0;
  }

  /* AVX2: clamp eight int32 per iteration — vpmaxsd against the lo broadcast,
   * vpminsd against the hi broadcast — store, and fold into the ymm2 sum. */
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
        !wcs_jcc(b, 0x8D /* jge */, &j_not_lo) ||
        !wcs_mov_reg_imm32(b, BINARY_GP_R10, (uint32_t)lo) ||
        !wcs_jcc(b, 0, &j_clamp_done)) {
      return 0;
    }
    if (!wcs_patch_here(b, j_not_lo) ||
        !wcs_cmp_reg_imm32(b, BINARY_GP_R10, hi) ||
        !wcs_jcc(b, 0x8E /* jle */, &j_not_hi) ||
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

  if (!code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->lhs,
                                               BINARY_GP_RCX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->rhs,
                                               BINARY_GP_RDX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->arguments[0],
                                               BINARY_GP_R8) ||
      !binary_emit_mov_reg_imm64(b, BINARY_GP_RAX, 0) ||
      !wcs_avx_vpxor_ymm(b, 2, 2, 2) ||
      !wcs_avx_vpxor_ymm(b, 5, 5, 5) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_R11, BINARY_GP_R8) ||
      !binary_emit_shift_reg_imm8(b, 4, BINARY_GP_R11, 2) ||
      !wcs_add_reg_reg64(b, BINARY_GP_R11, BINARY_GP_RCX)) {
    return 0;
  }

  loop_top = b->size;
  if (!binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R11) ||
      !wcs_jcc(b, 0x83 /* jae */, &j_done)) {
    return 0;
  }

  if (!binary_emit_mov_reg_reg(b, BINARY_GP_R9, BINARY_GP_R11) ||
      !binary_emit_alu_reg_reg(b, 0x29, BINARY_GP_R9, BINARY_GP_RCX) ||
      !wcs_cmp_reg_imm32(b, BINARY_GP_R9, 128) ||
      !wcs_jcc(b, 0x83 /* jae */, &j_vec) ||
      !wcs_jcc(b, 0, &j_scalar)) {
    return 0;
  }

  if (!wcs_patch_here(b, j_vec) ||
      !wcs_avx_vmovdqu_ymm_mem(b, 0, BINARY_GP_RCX, 0) ||
      !wcs_avx_vmovdqu_ymm_mem(b, 1, BINARY_GP_RDX, 0) ||
      !wcs_avx_vpmuldq_ymm(b, 3, 0, 1) ||
      !wcs_avx_vpsrlq_ymm_imm(b, 0, 0, 32) ||
      !wcs_avx_vpsrlq_ymm_imm(b, 1, 1, 32) ||
      !wcs_avx_vpmuldq_ymm(b, 4, 0, 1) ||
      !wcs_avx_vpaddq_ymm(b, 2, 2, 3) ||
      !wcs_avx_vpaddq_ymm(b, 5, 5, 4) ||
      !wcs_avx_vmovdqu_ymm_mem(b, 0, BINARY_GP_RCX, 32) ||
      !wcs_avx_vmovdqu_ymm_mem(b, 1, BINARY_GP_RDX, 32) ||
      !wcs_avx_vpmuldq_ymm(b, 3, 0, 1) ||
      !wcs_avx_vpsrlq_ymm_imm(b, 0, 0, 32) ||
      !wcs_avx_vpsrlq_ymm_imm(b, 1, 1, 32) ||
      !wcs_avx_vpmuldq_ymm(b, 4, 0, 1) ||
      !wcs_avx_vpaddq_ymm(b, 2, 2, 3) ||
      !wcs_avx_vpaddq_ymm(b, 5, 5, 4) ||
      !wcs_avx_vmovdqu_ymm_mem(b, 0, BINARY_GP_RCX, 64) ||
      !wcs_avx_vmovdqu_ymm_mem(b, 1, BINARY_GP_RDX, 64) ||
      !wcs_avx_vpmuldq_ymm(b, 3, 0, 1) ||
      !wcs_avx_vpsrlq_ymm_imm(b, 0, 0, 32) ||
      !wcs_avx_vpsrlq_ymm_imm(b, 1, 1, 32) ||
      !wcs_avx_vpmuldq_ymm(b, 4, 0, 1) ||
      !wcs_avx_vpaddq_ymm(b, 2, 2, 3) ||
      !wcs_avx_vpaddq_ymm(b, 5, 5, 4) ||
      !wcs_avx_vmovdqu_ymm_mem(b, 0, BINARY_GP_RCX, 96) ||
      !wcs_avx_vmovdqu_ymm_mem(b, 1, BINARY_GP_RDX, 96) ||
      !wcs_avx_vpmuldq_ymm(b, 3, 0, 1) ||
      !wcs_avx_vpsrlq_ymm_imm(b, 0, 0, 32) ||
      !wcs_avx_vpsrlq_ymm_imm(b, 1, 1, 32) ||
      !wcs_avx_vpmuldq_ymm(b, 4, 0, 1) ||
      !wcs_avx_vpaddq_ymm(b, 2, 2, 3) ||
      !wcs_avx_vpaddq_ymm(b, 5, 5, 4) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 64) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 64) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 64) ||
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
      !binary_emit_movsxd_reg_reg32(b, BINARY_GP_R10, BINARY_GP_R10) ||
      !binary_emit_mov_reg_mem32(b, BINARY_GP_R9, BINARY_GP_RDX, 0) ||
      !binary_emit_movsxd_reg_reg32(b, BINARY_GP_R9, BINARY_GP_R9) ||
      !binary_emit_imul_reg_reg(b, BINARY_GP_R10, BINARY_GP_R9) ||
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
      !wcs_avx_vpaddq_ymm(b, 2, 2, 5) ||
      !wcs_avx_vextracti128(b, 3, 2, 1) ||
      !wcs_avx_vzeroupper(b) ||
      !wcs_paddq(b, 2, 3) ||
      !binary_emit_movq_reg_xmm(b, BINARY_GP_R10, BINARY_XMM2) ||
      !wcs_add_reg_reg64(b, BINARY_GP_RAX, BINARY_GP_R10) ||
      !wcs_pshufd(b, 3, 2, 0xEE) ||
      !binary_emit_movq_reg_xmm(b, BINARY_GP_R10, BINARY_XMM3) ||
      !wcs_add_reg_reg64(b, BINARY_GP_RAX, BINARY_GP_R10)) {
    return 0;
  }

  return code_generator_binary_emit_destination_store(generator, context,
                                                      &instruction->dest,
                                                      BINARY_GP_RAX);
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

  /* r8=sum, rcx=src, rdx=dst, r9=end */
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

  loop_top = b->size;
  if (!binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R9) ||
      !wcs_jcc(b, 0x83 /* jae */, &j_done)) {
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

  /* r10=min, r11=max, rcx=walk, r8=end, xmm4/xmm5 extrema */
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
      /* Keep the GP min/max accumulators sign-extended so the 64-bit signed
       * compares in the scalar tail and final reduce stay correct for
       * negative extrema (broadcasts above already used the low 32 bits). */
      !binary_emit_movsxd_reg_reg32(b, BINARY_GP_R10, BINARY_GP_R10) ||
      !binary_emit_movsxd_reg_reg32(b, BINARY_GP_R11, BINARY_GP_R11)) {
    return 0;
  }

  loop_top = b->size;
  if (!binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R8) ||
      !wcs_jcc(b, 0x83 /* jae */, &j_done)) {
    return 0;
  }

  if (!binary_emit_mov_reg_reg(b, BINARY_GP_R9, BINARY_GP_R8) ||
      !binary_emit_alu_reg_reg(b, 0x29, BINARY_GP_R9, BINARY_GP_RCX) ||
      !wcs_cmp_reg_imm32(b, BINARY_GP_R9, 64) ||
      !wcs_jcc(b, 0x83 /* jae */, &j_vec) ||
      !wcs_jcc(b, 0, &j_scalar)) {
    return 0;
  }

  /* AVX2: two 32-byte loads cover the same 64-byte block the SSE path used,
   * folding 8 int32 per vpminsd/vpmaxsd into the ymm4/ymm5 running extrema. */
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
      !binary_emit_cmovcc_reg_reg(b, 0x4C /* cmovl */, BINARY_GP_R10,
                                  BINARY_GP_R14) ||
      !binary_emit_cmp_reg_reg(b, BINARY_GP_R14, BINARY_GP_R11) ||
      !binary_emit_cmovcc_reg_reg(b, 0x4F /* cmovg */, BINARY_GP_R11,
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

  /* Fold the upper 128-bit lanes of ymm4/ymm5 down onto the lower lanes, drop
   * the AVX upper state (vzeroupper) before the SSE horizontal reduce, then
   * collapse the 4 surviving lanes to the scalar min/max. */
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
