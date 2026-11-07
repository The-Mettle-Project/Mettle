#include "codegen/binary/mir.h"

#define GPCLOB(reg) (1u << (unsigned)(reg))

static const MirIrKernel kMirIrKernels[] = {
    {IR_OP_SIMD_SUM_I32, "simd_sum_i32",
     code_generator_binary_emit_simd_sum_i32, 0},
    {IR_OP_SIMD_SUM_U8, "simd_sum_u8", code_generator_binary_emit_simd_sum_u8,
     0},
    {IR_OP_SIMD_DOT_I32, "simd_dot_i32",
     code_generator_binary_emit_simd_dot_i32, 0},
    {IR_OP_SIMD_DOT_I8, "simd_dot_i8", code_generator_binary_emit_simd_dot_i8,
     0},
    {IR_OP_SIMD_MINMAX_I32, "simd_minmax_i32",
     code_generator_binary_emit_simd_minmax_i32, GPCLOB(BINARY_GP_R14)},
    {IR_OP_PREFIX_SUM_I32, "prefix_sum_i32",
     code_generator_binary_emit_prefix_sum_i32, 0},
    {IR_OP_LOWER_BOUND_I32, "lower_bound_i32",
     code_generator_binary_emit_lower_bound_i32, 0},

    {IR_OP_SIMD_SCALE_I32, "simd_scale_i32",
     code_generator_binary_emit_simd_scale_i32, 0},
    {IR_OP_SIMD_CLAMP_I32, "simd_clamp_i32",
     code_generator_binary_emit_simd_clamp_i32, 0},
    {IR_OP_SIMD_REVERSE_COPY_I32, "simd_reverse_copy_i32",
     code_generator_binary_emit_simd_reverse_copy_i32, 0},
    {IR_OP_SIMD_BYTE_MAP, "simd_byte_map",
     code_generator_binary_emit_simd_byte_map, 0},

    {IR_OP_SIMD_FIND, "simd_find", code_generator_binary_emit_simd_find, 0},
    {IR_OP_COUNT_WORD_STARTS, "count_word_starts",
     code_generator_binary_emit_count_word_starts, 0},

    {IR_OP_SIMD_SUM_F64, "simd_sum_f64",
     code_generator_binary_emit_simd_sum_f64, 0},
    {IR_OP_SIMD_SUM_F32, "simd_sum_f32",
     code_generator_binary_emit_simd_sum_f32, 0},
    {IR_OP_SIMD_DOT_F64, "simd_dot_f64",
     code_generator_binary_emit_simd_dot_f64, 0},
    {IR_OP_SIMD_DOT_F32, "simd_dot_f32",
     code_generator_binary_emit_simd_dot_f32, 0},
    {IR_OP_SIMD_MULTIDOT_F32, "simd_multidot_f32",
     code_generator_binary_emit_simd_multidot_f32, GPCLOB(BINARY_GP_R12),
     0xFFFFu},

    {IR_OP_SIMD_COPY, "simd_copy", code_generator_binary_emit_simd_copy,
     0},
    {IR_OP_MEMCPY_INLINE, "memcpy_inline",
     code_generator_binary_emit_memcpy_inline, 0},

    {IR_OP_SIMD_VLOOP_F64, "simd_vloop_f64",
     code_generator_binary_emit_simd_vloop_unmarshaled, 0},
    {IR_OP_SIMD_VLOOP_I32, "simd_vloop_i32",
     code_generator_binary_emit_simd_vloop_unmarshaled, 0},

    {IR_OP_SIMD_INSERTION_SORT_I32, "simd_insertion_sort_i32",
     code_generator_binary_emit_simd_insertion_sort_i32, 0},
    {IR_OP_SIMD_I2F_REDUCE_F64, "simd_i2f_reduce_f64",
     code_generator_binary_emit_simd_i2f_reduce_f64, 0},
    {IR_OP_SIMD_EXP_F32, "simd_exp_f32",
     code_generator_binary_emit_simd_exp_f32, 0},
    {IR_OP_SIMD_AFFINE_MAP_F32, "simd_affine_map_f32",
     code_generator_binary_emit_simd_affine_map_f32, 0},
    {IR_OP_SIMD_AFFINE_MAP_F64, "simd_affine_map_f64",
     code_generator_binary_emit_simd_affine_map_f64, 0},
    {IR_OP_SIMD_OUTER_LANE_F64, "simd_outer_lane_f64",
     code_generator_binary_emit_simd_outer_lane_f64, 0},
    {IR_OP_SIMD_LCG_U32, "simd_lcg_u32",
     code_generator_binary_emit_simd_lcg_u32, 0, 0x03FFu},
    {IR_OP_SIMD_LANES, "simd_lanes", code_generator_binary_emit_simd_lanes,
     GPCLOB(BINARY_GP_RBX) | GPCLOB(BINARY_GP_RSI) | GPCLOB(BINARY_GP_RDI) |
         GPCLOB(BINARY_GP_R12) | GPCLOB(BINARY_GP_R13),
     0xFFFFu},
};

#define MIR_IR_KERNEL_COUNT \
  ((int)(sizeof(kMirIrKernels) / sizeof(kMirIrKernels[0])))

int mir_ir_kernel_index_for_op(IROpcode op) {
  for (int i = 0; i < MIR_IR_KERNEL_COUNT; i++) {
    if (kMirIrKernels[i].ir_op == op) {
      return i;
    }
  }
  return -1;
}

const MirIrKernel *mir_ir_kernel_for_op(IROpcode op) {
  int i = mir_ir_kernel_index_for_op(op);
  return i < 0 ? NULL : &kMirIrKernels[i];
}

const MirIrKernel *mir_ir_kernel_at(int index) {
  if (index < 0 || index >= MIR_IR_KERNEL_COUNT) {
    return NULL;
  }
  return &kMirIrKernels[index];
}
