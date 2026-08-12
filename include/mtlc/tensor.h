/* mtlc/tensor.h - target-neutral cooperative tensor operation vocabulary. */
#ifndef MTLC_TENSOR_H
#define MTLC_TENSOR_H

#include "memory.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Logical element formats. A frontend without a native float16 scalar may
 * carry FLOAT16 matrix elements through uint16 pointers; the descriptor, not
 * the pointer's host-language spelling, defines their arithmetic meaning. */
typedef enum {
  MTLC_TENSOR_ELEMENT_INVALID = 0,
  MTLC_TENSOR_ELEMENT_FLOAT16,
  MTLC_TENSOR_ELEMENT_BFLOAT16,
  MTLC_TENSOR_ELEMENT_TFLOAT32,
  MTLC_TENSOR_ELEMENT_FLOAT32,
  MTLC_TENSOR_ELEMENT_FLOAT64,
  MTLC_TENSOR_ELEMENT_FLOAT8_E4M3,
  MTLC_TENSOR_ELEMENT_FLOAT8_E5M2,
  MTLC_TENSOR_ELEMENT_FLOAT6_E2M3,
  MTLC_TENSOR_ELEMENT_FLOAT6_E3M2,
  MTLC_TENSOR_ELEMENT_FLOAT4_E2M1,
  MTLC_TENSOR_ELEMENT_SCALE_UE8M0,
  MTLC_TENSOR_ELEMENT_SCALE_UE4M3,
  MTLC_TENSOR_ELEMENT_INT8,
  MTLC_TENSOR_ELEMENT_UINT8,
  MTLC_TENSOR_ELEMENT_INT4,
  MTLC_TENSOR_ELEMENT_UINT4,
  MTLC_TENSOR_ELEMENT_BIT1,
  MTLC_TENSOR_ELEMENT_INT32
} MtlcTensorElement;

typedef enum {
  MTLC_TENSOR_LAYOUT_INVALID = 0,
  MTLC_TENSOR_LAYOUT_ROW_MAJOR,
  MTLC_TENSOR_LAYOUT_COLUMN_MAJOR
} MtlcTensorLayout;

typedef enum {
  MTLC_TENSOR_MATH_MULTIPLY_ADD = 0,
  MTLC_TENSOR_MATH_XOR_POPCOUNT,
  MTLC_TENSOR_MATH_AND_POPCOUNT
} MtlcTensorMathMode;

typedef enum {
  MTLC_TENSOR_SPARSITY_DENSE = 0,
  MTLC_TENSOR_SPARSITY_STRUCTURED_1_TO_2,
  MTLC_TENSOR_SPARSITY_STRUCTURED_2_TO_4,
  MTLC_TENSOR_SPARSITY_STRUCTURED_4_TO_8
} MtlcTensorSparsity;

typedef enum {
  MTLC_TENSOR_ROUND_DEFAULT = 0,
  MTLC_TENSOR_ROUND_NEAREST_EVEN,
  MTLC_TENSOR_ROUND_TOWARD_ZERO,
  MTLC_TENSOR_ROUND_DOWN,
  MTLC_TENSOR_ROUND_UP
} MtlcTensorRounding;

typedef enum {
  MTLC_TENSOR_OVERFLOW_WRAP = 0,
  MTLC_TENSOR_OVERFLOW_SATURATE_FINITE
} MtlcTensorOverflow;

typedef enum {
  MTLC_TENSOR_SCALE_NONE = 0,
  MTLC_TENSOR_SCALE_PER_TENSOR,
  MTLC_TENSOR_SCALE_BLOCK_16,
  MTLC_TENSOR_SCALE_BLOCK_32
} MtlcTensorScaleMode;

/* Sub-byte logical elements may either occupy one addressable storage scalar
 * each or be densely bit-packed. Dense packing places consecutive logical
 * elements from least- to most-significant bits of each byte. Leading
 * dimensions always remain logical element counts, so this choice never leaks
 * a backend fragment layout. */
typedef enum {
  MTLC_TENSOR_PACKING_LOGICAL = 0,
  MTLC_TENSOR_PACKING_DENSE_SUBBYTE
} MtlcTensorPacking;

/* A cooperative tensor epilogue is separate from MMA so nonlinear activation
 * never changes the exact sequential-composition semantics of an MMA chain.
 * Bias coordinates are semantic matrix coordinates, not fragment lanes. */
typedef enum {
  MTLC_TENSOR_BIAS_NONE = 0,
  MTLC_TENSOR_BIAS_PER_ROW,
  MTLC_TENSOR_BIAS_PER_COLUMN,
  MTLC_TENSOR_BIAS_MATRIX
} MtlcTensorBiasMode;

typedef enum {
  MTLC_TENSOR_ACTIVATION_IDENTITY = 0,
  MTLC_TENSOR_ACTIVATION_RELU,
  MTLC_TENSOR_ACTIVATION_CLAMP
} MtlcTensorActivation;

/* Rank-aware cooperative tensor movement. This vocabulary describes memory,
 * not an accelerator descriptor: global extents/strides, tile extents, and
 * coordinates are sufficient to replay the operation on a backend without a
 * tensor-transfer engine. Dimension 0 is the contiguous/fastest-changing
 * dimension. */
#define MTLC_TENSOR_MAX_RANK 5u

typedef enum {
  MTLC_TENSOR_TRANSFER_GLOBAL_TO_WORKGROUP = 0,
  MTLC_TENSOR_TRANSFER_WORKGROUP_TO_GLOBAL
} MtlcTensorTransferDirection;

typedef enum {
  /* An out-of-range source coordinate contributes an all-zero element. An
   * out-of-range destination coordinate discards the element. */
  MTLC_TENSOR_BOUNDS_ZERO = 0
} MtlcTensorBoundsMode;

/* A collective rectangular tensor tile transfer. `global_stride_bytes[d]`
 * is the byte distance between adjacent logical elements in dimension d;
 * `element_stride[d]` selects every Nth logical element within the tile.
 * The workgroup tile is always canonical dimension-0-major storage. These
 * semantics deliberately do not expose a vendor tensor-map encoding, async
 * proxy, transaction barrier, or shared-memory bank layout.
 *
 * A backend may consume an optional prepared-view operand as an acceleration
 * token, but that token must describe this exact descriptor and the exact raw
 * global base pointer. Supplying a mismatched token is a dynamic precondition
 * violation, just like supplying a misaligned pointer to a vector operation.
 * The raw pointer and this descriptor remain the source of truth and make a
 * portable replay possible when no acceleration token is supplied. */
typedef struct {
  uint8_t rank;
  MtlcTensorTransferDirection direction;
  MtlcTensorElement element;
  MtlcTensorPacking packing;
  MtlcTensorBoundsMode bounds;
  MtlcMemoryScope scope;
  uint64_t global_extent[MTLC_TENSOR_MAX_RANK];
  uint64_t global_stride_bytes[MTLC_TENSOR_MAX_RANK];
  uint32_t tile_extent[MTLC_TENSOR_MAX_RANK];
  uint32_t element_stride[MTLC_TENSOR_MAX_RANK];
} MtlcTensorTransferDesc;

/* Runtime leading-dimension operands are selected independently. The mask is
 * carried by MtlcTensorMmaOperands; a selected descriptor field must be zero.
 * Unselected fields remain compile-time dimensions in the descriptor. */
typedef enum {
  MTLC_TENSOR_RUNTIME_STRIDE_NONE = 0,
  MTLC_TENSOR_RUNTIME_STRIDE_A = 1u << 0,
  MTLC_TENSOR_RUNTIME_STRIDE_B = 1u << 1,
  MTLC_TENSOR_RUNTIME_STRIDE_C = 1u << 2,
  MTLC_TENSOR_RUNTIME_STRIDE_D = 1u << 3,
  MTLC_TENSOR_RUNTIME_STRIDE_ALL = (1u << 4) - 1u
} MtlcTensorRuntimeStride;

/* One collective D = A*B + C tile. Dimensions and leading dimensions are in
 * stored elements, never bytes. The operation is executed collectively at `scope`;
 * every live invocation in that scope must execute it uniformly with identical
 * descriptors and pointer arguments. A nonzero leading dimension is a static
 * descriptor constant. Zero selects a uniform runtime scalar operand supplied
 * through the builder/source operation; it is never interpreted as stride 0.
 *
 * This is intentionally a whole-tile memory operation. Backend register
 * fragments, lane-to-fragment mappings, and vendor instruction spellings are
 * not part of the frontend or shared-IR contract.
 *
 * `tensor_matmul` reuses this semantic descriptor for one bounded region of a
 * whole matrix: m/n are region extents and k is the preferred exact native K
 * chunk. Separate unsigned runtime problem extents still require every K and
 * M/N edge to be computed; this descriptor never authorizes truncation.
 *
 * Sparse descriptors always make logical A the sparse operand. Logical K is
 * split into consecutive groups of 2, 4, or 8 for the 1:2, 2:4, or 4:8 mode.
 * Each group stores exactly half of its values in increasing logical-index
 * order; A's leading dimension therefore measures the packed K/2 storage, not
 * the logical K. `metadata` is a uint8 mask per group, in row-major
 * [M][K/group] order, whose low `group` bits identify the stored values and
 * have exactly group/2 bits set. This canonical representation is a language
 * contract, not an accelerator metadata word; a backend must translate it.
 * For tensor_matmul runtime K that ends inside a group, the final group still
 * stores group/2 values and one metadata byte; logical positions >= problem_k
 * simply do not contribute. Thus structured-2:4 whole-matrix A requires
 * 2*ceil(problem_k/4) stored columns and metadata uses the compact
 * ceil(problem_k/4) row stride. */
typedef struct {
  uint16_t m;
  uint16_t n;
  uint16_t k;
  MtlcTensorMathMode math_mode;
  MtlcTensorSparsity sparsity;
  MtlcTensorElement a_element;
  MtlcTensorElement b_element;
  MtlcTensorElement accumulator_element;
  MtlcTensorElement result_element;
  MtlcTensorLayout a_layout;
  MtlcTensorLayout b_layout;
  MtlcTensorLayout c_layout;
  MtlcTensorLayout d_layout;
  uint32_t a_leading_dimension;
  uint32_t b_leading_dimension;
  uint32_t c_leading_dimension;
  uint32_t d_leading_dimension;
  MtlcTensorRounding rounding;
  MtlcTensorOverflow overflow;
  MtlcTensorScaleMode a_scale_mode;
  MtlcTensorScaleMode b_scale_mode;
  MtlcTensorElement a_scale_element;
  MtlcTensorElement b_scale_element;
  MtlcTensorPacking a_packing;
  MtlcTensorPacking b_packing;
  /* Scale matrices use a canonical, target-neutral layout: A scales are
   * row-major M x ceil(K/block), B scales are column-major
   * ceil(K/block) x N. Zero selects the dense minimum leading dimension. */
  uint32_t a_scale_leading_dimension;
  uint32_t b_scale_leading_dimension;
  uint8_t transpose_a;
  uint8_t transpose_b;
  MtlcMemoryScope scope;
} MtlcTensorMmaDesc;

/* Cooperative in-place post-processing of a logical MxN tile:
 *
 *   D[row,col] = activation(alpha * D[row,col] + beta * bias[row,col])
 *
 * `alpha` and `beta` default to one when their corresponding `scale_*` flag is
 * zero. Bias may be absent, a contiguous per-row/per-column vector, or a
 * matrix with its own layout/leading dimension. Clamp bounds and enabled
 * scales are uniform runtime scalar operands; data leading dimensions use the
 * same zero-means-runtime convention as MMA. f16/bf16 storage computes in f32;
 * f32 and f64 compute in their own format. ReLU replaces an ordered negative
 * value with positive zero; clamp first replaces an ordered value below its
 * lower bound, then one above its upper bound. Unordered values are preserved.
 * When bias is present, the bias region read by the tile must not overlap the
 * destination region written by the tile.
 *
 * The operation is collective at `scope` and includes entry/exit ordering for
 * participating invocations. This permits exact portable replay after a
 * cooperative MMA store without exposing a backend fragment mapping. */
typedef struct {
  uint16_t m;
  uint16_t n;
  MtlcTensorElement element;
  MtlcTensorLayout layout;
  uint32_t leading_dimension;
  MtlcTensorBiasMode bias_mode;
  MtlcTensorLayout bias_layout;
  uint32_t bias_leading_dimension;
  MtlcTensorActivation activation;
  uint8_t scale_output;
  uint8_t scale_bias;
  MtlcMemoryScope scope;
} MtlcTensorEpilogueDesc;

/* Convenience constructor for a common profile, not a restriction on the
 * descriptor or builder API. */
static inline MtlcTensorMmaDesc
mtlc_tensor_mma_f16_f32_m16n16k16_desc(void) {
  MtlcTensorMmaDesc desc = {0};
  desc.m = 16;
  desc.n = 16;
  desc.k = 16;
  desc.a_element = MTLC_TENSOR_ELEMENT_FLOAT16;
  desc.b_element = MTLC_TENSOR_ELEMENT_FLOAT16;
  desc.accumulator_element = MTLC_TENSOR_ELEMENT_FLOAT32;
  desc.result_element = MTLC_TENSOR_ELEMENT_FLOAT32;
  desc.a_layout = MTLC_TENSOR_LAYOUT_ROW_MAJOR;
  desc.b_layout = MTLC_TENSOR_LAYOUT_COLUMN_MAJOR;
  desc.c_layout = MTLC_TENSOR_LAYOUT_ROW_MAJOR;
  desc.d_layout = MTLC_TENSOR_LAYOUT_ROW_MAJOR;
  desc.a_leading_dimension = 16;
  desc.b_leading_dimension = 16;
  desc.c_leading_dimension = 16;
  desc.d_leading_dimension = 16;
  desc.scope = MTLC_MEMORY_SCOPE_SUBGROUP;
  return desc;
}

/* Structural validation only. This checks that the descriptor is internally
 * meaningful (including static/runtime leading dimensions and enum ranges), not
 * that a particular target supports it. Backend capability checks are a
 * separate, target-specific step. */
int mtlc_tensor_mma_desc_is_valid(const MtlcTensorMmaDesc *desc);

/* Structural validation only. A backend may support a subset of the neutral
 * storage/activation combinations and must reject the rest explicitly. */
int mtlc_tensor_epilogue_desc_is_valid(
    const MtlcTensorEpilogueDesc *desc);

/* Structural validation only; target-specific tensor-map restrictions are
 * checked by the selected backend/runtime encoder. */
int mtlc_tensor_transfer_desc_is_valid(const MtlcTensorTransferDesc *desc);

#ifdef __cplusplus
}
#endif

#endif /* MTLC_TENSOR_H */
