#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { ROW_MAJOR = 0, COLUMN_MAJOR = 1 };

static size_t at(uint32_t row, uint32_t column, uint32_t leading_dimension,
                 int layout) {
  return layout == ROW_MAJOR
             ? (size_t)row * leading_dimension + column
             : (size_t)column * leading_dimension + row;
}

static size_t operand_at(uint32_t row, uint32_t column,
                         uint32_t leading_dimension, int layout,
                         int transpose) {
  return transpose ? at(column, row, leading_dimension, layout)
                   : at(row, column, leading_dimension, layout);
}

static void reference_f32(const float *a, const float *b, const float *c,
                          float *d, uint32_t m, uint32_t n, uint32_t k,
                          uint32_t lda, uint32_t ldb, uint32_t ldc,
                          uint32_t ldd, int transpose_a, int transpose_b) {
  for (uint32_t row = 0; row < m; row++) {
    for (uint32_t column = 0; column < n; column++) {
      float value = c[at(row, column, ldc, COLUMN_MAJOR)];
      for (uint32_t q = 0; q < k; q++)
        value += a[operand_at(row, q, lda, COLUMN_MAJOR, transpose_a)] *
                 b[operand_at(q, column, ldb, ROW_MAJOR, transpose_b)];
      d[at(row, column, ldd, ROW_MAJOR)] = value;
    }
  }
}

static void region_f32(const float *a, const float *b, const float *c,
                       float *d, uint32_t row_origin,
                       uint32_t column_origin, uint32_t problem_m,
                       uint32_t problem_n, uint32_t problem_k,
                       uint32_t tile_m, uint32_t tile_n, uint32_t lda,
                       uint32_t ldb, uint32_t ldc, uint32_t ldd,
                       int transpose_a, int transpose_b) {
  for (uint32_t local_row = 0; local_row < tile_m; local_row++) {
    uint32_t row = row_origin + local_row;
    if (row < row_origin || row >= problem_m) continue;
    for (uint32_t local_column = 0; local_column < tile_n;
         local_column++) {
      uint32_t column = column_origin + local_column;
      if (column < column_origin || column >= problem_n) continue;
      float value = c[at(row, column, ldc, COLUMN_MAJOR)];
      for (uint32_t q = 0; q < problem_k; q++)
        value += a[operand_at(row, q, lda, COLUMN_MAJOR, transpose_a)] *
                 b[operand_at(q, column, ldb, ROW_MAJOR, transpose_b)];
      d[at(row, column, ldd, ROW_MAJOR)] = value;
    }
  }
}

static float fp32_from_bits(uint32_t bits) {
  float value;
  memcpy(&value, &bits, sizeof(value));
  return value;
}

static float decode_fp8(uint8_t storage, int exponent_bits,
                        int mantissa_bits, int bias, int finite_e4m3) {
  uint32_t sign = storage >> 7;
  uint32_t exponent_mask = (UINT32_C(1) << exponent_bits) - 1u;
  uint32_t mantissa_mask = (UINT32_C(1) << mantissa_bits) - 1u;
  uint32_t exponent = (storage >> mantissa_bits) & exponent_mask;
  uint32_t mantissa = storage & mantissa_mask;
  if (finite_e4m3 && exponent == exponent_mask &&
      mantissa == mantissa_mask)
    return fp32_from_bits(UINT32_C(0x7fc00000) | (sign << 31));
  if (!finite_e4m3 && exponent == exponent_mask) {
    if (mantissa == 0)
      return fp32_from_bits(UINT32_C(0x7f800000) | (sign << 31));
    return fp32_from_bits(UINT32_C(0x7fc00000) | (sign << 31));
  }
  uint32_t significand =
      exponent == 0 ? mantissa : (UINT32_C(1) << mantissa_bits) + mantissa;
  int power = exponent == 0 ? 1 - bias - mantissa_bits
                            : (int)exponent - bias - mantissa_bits;
  float value = (float)significand;
  while (power > 0) {
    value *= 2.0f;
    power--;
  }
  while (power < 0) {
    value *= 0.5f;
    power++;
  }
  return sign ? -value : value;
}

static float decode_e4m3(uint8_t storage) {
  return decode_fp8(storage, 4, 3, 7, 1);
}

static float decode_e5m2(uint8_t storage) {
  return decode_fp8(storage, 5, 2, 15, 0);
}

static float decode_finite_narrow(uint8_t storage, int total_bits,
                                  int exponent_bits, int mantissa_bits,
                                  int bias) {
  uint32_t sign = (uint32_t)storage >> (total_bits - 1);
  uint32_t exponent_mask = (UINT32_C(1) << exponent_bits) - 1u;
  uint32_t mantissa_mask = (UINT32_C(1) << mantissa_bits) - 1u;
  uint32_t exponent = ((uint32_t)storage >> mantissa_bits) & exponent_mask;
  uint32_t mantissa = (uint32_t)storage & mantissa_mask;
  uint32_t significand =
      exponent == 0 ? mantissa : (UINT32_C(1) << mantissa_bits) + mantissa;
  int power = exponent == 0 ? 1 - bias - mantissa_bits
                            : (int)exponent - bias - mantissa_bits;
  float value = (float)significand;
  while (power > 0) {
    value *= 2.0f;
    power--;
  }
  while (power < 0) {
    value *= 0.5f;
    power++;
  }
  return sign ? -value : value;
}

static float decode_e2m1(uint8_t storage) {
  return decode_finite_narrow(storage, 4, 2, 1, 1);
}

static float decode_e2m3(uint8_t storage) {
  return decode_finite_narrow(storage, 6, 2, 3, 1);
}

static float decode_e3m2(uint8_t storage) {
  return decode_finite_narrow(storage, 6, 3, 2, 3);
}

static float decode_ue8m0(uint8_t storage) {
  if (storage == UINT8_C(0xff)) return fp32_from_bits(UINT32_C(0x7fc00000));
  if (storage == 0) return fp32_from_bits(UINT32_C(0x00400000));
  return fp32_from_bits((uint32_t)storage << 23);
}

static float decode_ue4m3(uint8_t storage) {
  return decode_e4m3((uint8_t)(storage & UINT8_C(0x7f)));
}

static uint8_t packed_get(const uint8_t *storage, size_t logical_index,
                          unsigned bits) {
  size_t bit_index = logical_index * bits;
  size_t byte_index = bit_index >> 3;
  unsigned shift = (unsigned)(bit_index & 7u);
  uint16_t word = storage[byte_index];
  if (shift + bits > 8) word |= (uint16_t)storage[byte_index + 1] << 8;
  return (uint8_t)((word >> shift) & ((UINT16_C(1) << bits) - 1u));
}

static void packed_set(uint8_t *storage, size_t logical_index, unsigned bits,
                       uint8_t value) {
  size_t bit_index = logical_index * bits;
  size_t byte_index = bit_index >> 3;
  unsigned shift = (unsigned)(bit_index & 7u);
  uint16_t word = storage[byte_index];
  if (shift + bits > 8) word |= (uint16_t)storage[byte_index + 1] << 8;
  uint16_t mask = (uint16_t)(((UINT16_C(1) << bits) - 1u) << shift);
  word = (uint16_t)((word & (uint16_t)~mask) |
                    (((uint16_t)value << shift) & mask));
  storage[byte_index] = (uint8_t)word;
  if (shift + bits > 8) storage[byte_index + 1] = (uint8_t)(word >> 8);
}

typedef float (*DecodeNarrow)(uint8_t);

static float load_narrow(const uint8_t *storage, size_t logical_index,
                         unsigned bits, DecodeNarrow decode) {
  uint8_t encoded = bits == 8 ? storage[logical_index]
                              : packed_get(storage, logical_index, bits);
  return decode(encoded);
}

static void reference_fp8(const uint8_t *a, const uint8_t *b,
                          const float *c, float *d, uint32_t m,
                          uint32_t n, uint32_t k, uint32_t lda,
                          uint32_t ldb, uint32_t ldc, uint32_t ldd,
                          int a_layout, int b_layout, int transpose_a,
                          int transpose_b) {
  for (uint32_t row = 0; row < m; row++) {
    for (uint32_t column = 0; column < n; column++) {
      float value = c[at(row, column, ldc, COLUMN_MAJOR)];
      for (uint32_t q = 0; q < k; q++)
        value += decode_e4m3(a[operand_at(row, q, lda, a_layout,
                                         transpose_a)]) *
                 decode_e5m2(b[operand_at(q, column, ldb, b_layout,
                                         transpose_b)]);
      d[at(row, column, ldd, ROW_MAJOR)] = value;
    }
  }
}

static void region_fp8(const uint8_t *a, const uint8_t *b, const float *c,
                       float *d, uint32_t row_origin,
                       uint32_t column_origin, uint32_t problem_m,
                       uint32_t problem_n, uint32_t problem_k,
                       uint32_t tile_m, uint32_t tile_n, uint32_t lda,
                       uint32_t ldb, uint32_t ldc, uint32_t ldd,
                       int a_layout, int b_layout, int transpose_a,
                       int transpose_b) {
  for (uint32_t local_row = 0; local_row < tile_m; local_row++) {
    uint32_t row = row_origin + local_row;
    if (row < row_origin || row >= problem_m) continue;
    for (uint32_t local_column = 0; local_column < tile_n;
         local_column++) {
      uint32_t column = column_origin + local_column;
      if (column < column_origin || column >= problem_n) continue;
      float value = c[at(row, column, ldc, COLUMN_MAJOR)];
      for (uint32_t q = 0; q < problem_k; q++)
        value += decode_e4m3(a[operand_at(row, q, lda, a_layout,
                                         transpose_a)]) *
                 decode_e5m2(b[operand_at(q, column, ldb, b_layout,
                                         transpose_b)]);
      d[at(row, column, ldd, ROW_MAJOR)] = value;
    }
  }
}

static void reference_scaled(
    const uint8_t *a, const uint8_t *b, const float *c, float *d,
    const uint8_t *a_scale, const uint8_t *b_scale, uint32_t m, uint32_t n,
    uint32_t k, uint32_t lda, uint32_t ldb, uint32_t ldc, uint32_t ldd,
    uint32_t ldsa, uint32_t ldsb, uint32_t scale_block, unsigned a_bits,
    unsigned b_bits, DecodeNarrow decode_a, DecodeNarrow decode_b,
    DecodeNarrow decode_scale, int a_layout, int b_layout, int transpose_a,
    int transpose_b) {
  for (uint32_t row = 0; row < m; row++) {
    for (uint32_t column = 0; column < n; column++) {
      float value = c[at(row, column, ldc, COLUMN_MAJOR)];
      for (uint32_t q = 0; q < k; q++) {
        uint32_t chunk = q / scale_block;
        float av = load_narrow(
            a, operand_at(row, q, lda, a_layout, transpose_a), a_bits,
            decode_a);
        float bv = load_narrow(
            b, operand_at(q, column, ldb, b_layout, transpose_b), b_bits,
            decode_b);
        float as = decode_scale(a_scale[at(row, chunk, ldsa, ROW_MAJOR)]);
        float bs =
            decode_scale(b_scale[at(chunk, column, ldsb, COLUMN_MAJOR)]);
        value += (av * as) * (bv * bs);
      }
      d[at(row, column, ldd, ROW_MAJOR)] = value;
    }
  }
}

static void region_scaled(
    const uint8_t *a, const uint8_t *b, const float *c, float *d,
    const uint8_t *a_scale, const uint8_t *b_scale, uint32_t row_origin,
    uint32_t column_origin, uint32_t problem_m, uint32_t problem_n,
    uint32_t problem_k, uint32_t tile_m, uint32_t tile_n, uint32_t lda,
    uint32_t ldb, uint32_t ldc, uint32_t ldd, uint32_t ldsa, uint32_t ldsb,
    uint32_t scale_block, unsigned a_bits, unsigned b_bits,
    DecodeNarrow decode_a, DecodeNarrow decode_b, DecodeNarrow decode_scale,
    int a_layout, int b_layout, int transpose_a, int transpose_b) {
  for (uint32_t local_row = 0; local_row < tile_m; local_row++) {
    uint32_t row = row_origin + local_row;
    if (row < row_origin || row >= problem_m) continue;
    for (uint32_t local_column = 0; local_column < tile_n;
         local_column++) {
      uint32_t column = column_origin + local_column;
      if (column < column_origin || column >= problem_n) continue;
      float value = c[at(row, column, ldc, COLUMN_MAJOR)];
      for (uint32_t q = 0; q < problem_k; q++) {
        uint32_t chunk = q / scale_block;
        float av = load_narrow(
            a, operand_at(row, q, lda, a_layout, transpose_a), a_bits,
            decode_a);
        float bv = load_narrow(
            b, operand_at(q, column, ldb, b_layout, transpose_b), b_bits,
            decode_b);
        float as = decode_scale(a_scale[at(row, chunk, ldsa, ROW_MAJOR)]);
        float bs =
            decode_scale(b_scale[at(chunk, column, ldsb, COLUMN_MAJOR)]);
        value += (av * as) * (bv * bs);
      }
      d[at(row, column, ldd, ROW_MAJOR)] = value;
    }
  }
}

static int test_narrow_formats(void) {
  uint8_t packed4[2] = {0};
  uint8_t packed6[3] = {0};
  for (size_t i = 0; i < 4; i++) {
    packed_set(packed4, i, 4, (uint8_t)(i + 1));
    packed_set(packed6, i, 6, (uint8_t)(i + 1));
  }
  if (packed4[0] != UINT8_C(0x21) || packed4[1] != UINT8_C(0x43) ||
      packed6[0] != UINT8_C(0x81) || packed6[1] != UINT8_C(0x30) ||
      packed6[2] != UINT8_C(0x10))
    return 1;
  for (size_t i = 0; i < 4; i++)
    if (packed_get(packed4, i, 4) != i + 1 ||
        packed_get(packed6, i, 6) != i + 1)
      return 2;
  if (decode_e2m1(UINT8_C(0x01)) != 0.5f ||
      decode_e2m1(UINT8_C(0x09)) != -0.5f ||
      decode_e2m3(UINT8_C(0x01)) != 0.125f ||
      decode_e2m3(UINT8_C(0x08)) != 1.0f ||
      decode_e3m2(UINT8_C(0x01)) != 0.0625f ||
      decode_e3m2(UINT8_C(0x0c)) != 1.0f)
    return 3;
  if (decode_ue8m0(0) != fp32_from_bits(UINT32_C(0x00400000)) ||
      decode_ue8m0(UINT8_C(0x7f)) != 1.0f ||
      decode_ue8m0(UINT8_C(0x80)) != 2.0f ||
      decode_ue4m3(UINT8_C(0x38)) != 1.0f ||
      decode_ue4m3(UINT8_C(0xb8)) != 1.0f)
    return 4;
  float nan_scale = decode_ue8m0(UINT8_C(0xff));
  return nan_scale == nan_scale ? 5 : 0;
}

static int test_scaled_case(
    unsigned a_bits, unsigned b_bits, DecodeNarrow decode_a,
    DecodeNarrow decode_b, uint32_t scale_block, DecodeNarrow decode_scale,
    const uint8_t *a_codes, size_t a_code_count, const uint8_t *b_codes,
    size_t b_code_count, const uint8_t *scale_codes,
    size_t scale_code_count) {
  enum {
    M = 19,
    N = 23,
    K = 67,
    TILE_M = 16,
    TILE_N = 16,
    LDA = 72,
    LDB = 76,
    LDC = 29,
    LDD = 31,
    MAX_LDSA = 7,
    MAX_LDSB = 9
  };
  uint32_t ldsa = scale_block == 16 ? MAX_LDSA : 5;
  uint32_t ldsb = scale_block == 16 ? MAX_LDSB : 7;
  uint8_t a[LDA * M], b[LDB * N];
  uint8_t a_scale[MAX_LDSA * M], b_scale[MAX_LDSB * N];
  float c[LDC * N], expected[LDD * M], tiled[LDD * M];
  float k_zero[LDD * M];
  memset(a, 0, sizeof(a));
  memset(b, 0, sizeof(b));
  for (uint32_t row = 0; row < M; row++)
    for (uint32_t q = 0; q < K; q++)
      packed_set(a, at(row, q, LDA, ROW_MAJOR), a_bits,
                 a_codes[(row * 17u + q * 3u) % a_code_count]);
  for (uint32_t column = 0; column < N; column++)
    for (uint32_t q = 0; q < K; q++)
      packed_set(b, at(q, column, LDB, COLUMN_MAJOR), b_bits,
                 b_codes[(column * 11u + q * 5u) % b_code_count]);
  for (size_t i = 0; i < sizeof(a_scale); i++)
    a_scale[i] = scale_codes[(i * 3u + 1u) % scale_code_count];
  for (size_t i = 0; i < sizeof(b_scale); i++)
    b_scale[i] = scale_codes[(i * 5u + 2u) % scale_code_count];
  for (size_t i = 0; i < sizeof(c) / sizeof(c[0]); i++)
    c[i] = (float)((int)(i % 19) - 9) * 0.125f;
  memset(expected, 0x6d, sizeof(expected));
  memset(tiled, 0x6d, sizeof(tiled));
  reference_scaled(a, b, c, expected, a_scale, b_scale, M, N, K, LDA,
                   LDB, LDC, LDD, ldsa, ldsb, scale_block, a_bits, b_bits,
                   decode_a, decode_b, decode_scale, ROW_MAJOR, COLUMN_MAJOR,
                   0, 0);
  for (uint32_t row = 0; row < M; row += TILE_M)
    for (uint32_t column = 0; column < N; column += TILE_N)
      region_scaled(a, b, c, tiled, a_scale, b_scale, row, column, M, N, K,
                    TILE_M, TILE_N, LDA, LDB, LDC, LDD, ldsa, ldsb,
                    scale_block, a_bits, b_bits, decode_a, decode_b,
                    decode_scale, ROW_MAJOR, COLUMN_MAJOR, 0, 0);
  if (memcmp(expected, tiled, sizeof(expected)) != 0) return 1;

  memset(k_zero, 0x4e, sizeof(k_zero));
  for (uint32_t row = 0; row < M; row += TILE_M)
    for (uint32_t column = 0; column < N; column += TILE_N)
      region_scaled(a, b, c, k_zero, a_scale, b_scale, row, column, M, N, 0,
                    TILE_M, TILE_N, LDA, LDB, LDC, LDD, ldsa, ldsb,
                    scale_block, a_bits, b_bits, decode_a, decode_b,
                    decode_scale, ROW_MAJOR, COLUMN_MAJOR, 0, 0);
  for (uint32_t row = 0; row < M; row++)
    for (uint32_t column = 0; column < N; column++)
      if (k_zero[at(row, column, LDD, ROW_MAJOR)] !=
          c[at(row, column, LDC, COLUMN_MAJOR)])
        return 2;
  float before = k_zero[0];
  region_scaled(a, b, c, k_zero, a_scale, b_scale, M, N, M, N, K, TILE_M,
                TILE_N, LDA, LDB, LDC, LDD, ldsa, ldsb, scale_block, a_bits,
                b_bits, decode_a, decode_b, decode_scale, ROW_MAJOR,
                COLUMN_MAJOR, 0, 0);
  return k_zero[0] == before ? 0 : 3;
}

static unsigned popcount4(uint8_t value) {
  value = (uint8_t)(value & UINT8_C(0x0f));
  return (unsigned)(value & 1u) + (unsigned)((value >> 1) & 1u) +
         (unsigned)((value >> 2) & 1u) +
         (unsigned)((value >> 3) & 1u);
}

static uint8_t canonical_sparse_mask(uint8_t mask) {
  mask = (uint8_t)(mask & UINT8_C(0x0f));
  return popcount4(mask) == 2u ? mask : UINT8_C(0x03);
}

static float sparse_a_at(const float *compressed_a, const uint8_t *metadata,
                         uint32_t row, uint32_t q, uint32_t lda,
                         uint32_t metadata_stride, int a_layout,
                         int transpose_a) {
  uint32_t group = q / 4u;
  uint32_t position = q % 4u;
  uint8_t mask = canonical_sparse_mask(
      metadata[at(row, group, metadata_stride, ROW_MAJOR)]);
  uint8_t bit = (uint8_t)(UINT8_C(1) << position);
  if ((mask & bit) == 0) return 0.0f;
  unsigned rank = popcount4((uint8_t)(mask & (uint8_t)(bit - 1u)));
  uint32_t compressed_column = group * 2u + rank;
  return compressed_a[operand_at(row, compressed_column, lda, a_layout,
                                 transpose_a)];
}

/* Build a conventional dense logical A first.  The region implementation
 * below independently decodes compressed A at each logical q, so agreement is
 * evidence for grouping, selected-value order, transpose, and odd-K tails. */
static void expand_sparse_a(const float *compressed_a,
                            const uint8_t *metadata, float *dense_a,
                            uint32_t m, uint32_t k, uint32_t lda,
                            uint32_t metadata_stride, int a_layout,
                            int transpose_a) {
  for (uint32_t row = 0; row < m; row++)
    for (uint32_t q = 0; q < k; q++) {
      uint32_t group = q >> 2;
      uint8_t mask = canonical_sparse_mask(
          metadata[(size_t)row * metadata_stride + group]);
      uint8_t selected = (uint8_t)(UINT8_C(1) << (q & 3u));
      float value = 0.0f;
      if ((mask & selected) != 0) {
        unsigned rank = popcount4(
            (uint8_t)(mask & (uint8_t)(selected - UINT8_C(1))));
        value = compressed_a[operand_at(row, group * 2u + rank, lda,
                                        a_layout, transpose_a)];
      }
      dense_a[(size_t)row * k + q] = value;
    }
}

static void reference_sparse_dense(const float *dense_a, const float *b,
                                   const float *c, float *d, uint32_t m,
                                   uint32_t n, uint32_t k, uint32_t ldb,
                                   uint32_t ldc, uint32_t ldd) {
  for (uint32_t row = 0; row < m; row++) {
    for (uint32_t column = 0; column < n; column++) {
      float value = c[at(row, column, ldc, COLUMN_MAJOR)];
      for (uint32_t q = 0; q < k; q++)
        value += dense_a[(size_t)row * k + q] *
                 b[at(q, column, ldb, COLUMN_MAJOR)];
      d[at(row, column, ldd, ROW_MAJOR)] = value;
    }
  }
}

static void region_sparse(const float *compressed_a, const float *b,
                          const float *c, float *d,
                          const uint8_t *metadata, uint32_t row_origin,
                          uint32_t column_origin, uint32_t problem_m,
                          uint32_t problem_n, uint32_t problem_k,
                          uint32_t tile_m, uint32_t tile_n, uint32_t lda,
                          uint32_t ldb, uint32_t ldc, uint32_t ldd,
                          int a_layout, int transpose_a) {
  uint32_t metadata_stride = (problem_k + 3u) / 4u;
  for (uint32_t local_row = 0; local_row < tile_m; local_row++) {
    uint32_t row = row_origin + local_row;
    if (row < row_origin || row >= problem_m) continue;
    for (uint32_t local_column = 0; local_column < tile_n;
         local_column++) {
      uint32_t column = column_origin + local_column;
      if (column < column_origin || column >= problem_n) continue;
      float value = c[at(row, column, ldc, COLUMN_MAJOR)];
      for (uint32_t q = 0; q < problem_k; q++)
        value += sparse_a_at(compressed_a, metadata, row, q, lda,
                             metadata_stride, a_layout, transpose_a) *
                 b[at(q, column, ldb, COLUMN_MAJOR)];
      d[at(row, column, ldd, ROW_MAJOR)] = value;
    }
  }
}

static int test_sparse_case(int a_layout, int transpose_a) {
  enum {
    M = 19,
    N = 23,
    K = 19,
    TILE_M = 16,
    TILE_N = 16,
    LDA = 29,
    LDB = 23,
    LDC = 29,
    LDD = 31,
    METADATA_STRIDE = (K + 3) / 4
  };
  float compressed_a[LDA * M], b[LDB * N], c[LDC * N];
  float dense_a[K * M], expected[LDD * M], tiled[LDD * M];
  float k_zero[LDD * M];
  uint8_t metadata[METADATA_STRIDE * M];
  static const uint8_t masks[] = {0x03, 0x05, 0x09, 0x06, 0x0c};
  memset(compressed_a, 0, sizeof(compressed_a));
  for (uint32_t row = 0; row < M; row++) {
    for (uint32_t group = 0; group < METADATA_STRIDE; group++) {
      metadata[at(row, group, METADATA_STRIDE, ROW_MAJOR)] =
          (uint8_t)(masks[(row + group * 3u) %
                          (sizeof(masks) / sizeof(masks[0]))] |
                    ((row + group) & 1u ? UINT8_C(0xa0) : 0));
      for (uint32_t slot = 0; slot < 2; slot++) {
        uint32_t compressed_column = group * 2u + slot;
        compressed_a[operand_at(row, compressed_column, LDA, a_layout,
                                transpose_a)] =
            (float)((int)(row * 7u + compressed_column * 3u + slot) % 17 -
                    8) *
            0.125f;
      }
    }
  }
  for (size_t i = 0; i < sizeof(b) / sizeof(b[0]); i++)
    b[i] = (float)((int)(i % 13) - 6) * 0.0625f;
  for (size_t i = 0; i < sizeof(c) / sizeof(c[0]); i++)
    c[i] = (float)((int)(i % 11) - 5) * 0.25f;

  expand_sparse_a(compressed_a, metadata, dense_a, M, K, LDA,
                  METADATA_STRIDE, a_layout, transpose_a);
  memset(expected, 0x57, sizeof(expected));
  memset(tiled, 0x57, sizeof(tiled));
  reference_sparse_dense(dense_a, b, c, expected, M, N, K, LDB, LDC, LDD);
  for (uint32_t row = 0; row < M; row += TILE_M)
    for (uint32_t column = 0; column < N; column += TILE_N)
      region_sparse(compressed_a, b, c, tiled, metadata, row, column, M, N,
                    K, TILE_M, TILE_N, LDA, LDB, LDC, LDD, a_layout,
                    transpose_a);
  if (memcmp(expected, tiled, sizeof(expected)) != 0) return 1;

  memset(k_zero, 0x68, sizeof(k_zero));
  for (uint32_t row = 0; row < M; row += TILE_M)
    for (uint32_t column = 0; column < N; column += TILE_N)
      region_sparse(compressed_a, b, c, k_zero, metadata, row, column, M, N,
                    0, TILE_M, TILE_N, LDA, LDB, LDC, LDD, a_layout,
                    transpose_a);
  for (uint32_t row = 0; row < M; row++)
    for (uint32_t column = 0; column < N; column++)
      if (k_zero[at(row, column, LDD, ROW_MAJOR)] !=
          c[at(row, column, LDC, COLUMN_MAJOR)])
        return 2;
  float before = k_zero[0];
  region_sparse(compressed_a, b, c, k_zero, metadata, M, N, M, N, K,
                TILE_M, TILE_N, LDA, LDB, LDC, LDD, a_layout, transpose_a);
  return k_zero[0] == before ? 0 : 3;
}

static void reference_i8(const int8_t *a, const int8_t *b,
                         const uint32_t *c, uint32_t *d, uint32_t m,
                         uint32_t n, uint32_t k, uint32_t lda,
                         uint32_t ldb, uint32_t ldc, uint32_t ldd) {
  for (uint32_t row = 0; row < m; row++) {
    for (uint32_t column = 0; column < n; column++) {
      uint32_t value = c[at(row, column, ldc, ROW_MAJOR)];
      for (uint32_t q = 0; q < k; q++) {
        int32_t product = (int32_t)a[at(row, q, lda, ROW_MAJOR)] *
                          (int32_t)b[at(q, column, ldb, COLUMN_MAJOR)];
        value += (uint32_t)product;
      }
      d[at(row, column, ldd, COLUMN_MAJOR)] = value;
    }
  }
}

static void region_i8(const int8_t *a, const int8_t *b, const uint32_t *c,
                      uint32_t *d, uint32_t row_origin,
                      uint32_t column_origin, uint32_t problem_m,
                      uint32_t problem_n, uint32_t problem_k,
                      uint32_t tile_m, uint32_t tile_n, uint32_t lda,
                      uint32_t ldb, uint32_t ldc, uint32_t ldd) {
  for (uint32_t local_row = 0; local_row < tile_m; local_row++) {
    uint32_t row = row_origin + local_row;
    if (row < row_origin || row >= problem_m) continue;
    for (uint32_t local_column = 0; local_column < tile_n;
         local_column++) {
      uint32_t column = column_origin + local_column;
      if (column < column_origin || column >= problem_n) continue;
      uint32_t value = c[at(row, column, ldc, ROW_MAJOR)];
      for (uint32_t q = 0; q < problem_k; q++) {
        int32_t product = (int32_t)a[at(row, q, lda, ROW_MAJOR)] *
                          (int32_t)b[at(q, column, ldb, COLUMN_MAJOR)];
        value += (uint32_t)product;
      }
      d[at(row, column, ldd, COLUMN_MAJOR)] = value;
    }
  }
}

int main(void) {
  enum { M = 19, N = 23, K = 21, TILE_M = 16, TILE_N = 16 };
  enum { LDA = 29, LDB = 31, LDC = 37, LDD = 41 };
  float af[LDA * K], bf[LDB * N], cf[LDC * N];
  float expected_f[LDD * M], tiled_f[LDD * M];
  float expected_transpose[LDD * M], tiled_transpose[LDD * M];
  uint8_t a8[LDA * K], b8[LDB * N];
  float expected_fp8[LDD * M], tiled_fp8[LDD * M];
  float expected_fp8_transpose[LDD * M], tiled_fp8_transpose[LDD * M];
  int8_t ai[LDA * M], bi[LDB * N];
  uint32_t ci[LDC * M], expected_i[LDD * N], tiled_i[LDD * N];

  for (size_t i = 0; i < sizeof(af) / sizeof(af[0]); i++)
    af[i] = (float)((int)(i % 13) - 6) * 0.125f;
  for (size_t i = 0; i < sizeof(bf) / sizeof(bf[0]); i++)
    bf[i] = (float)((int)(i % 11) - 5) * 0.0625f;
  for (size_t i = 0; i < sizeof(cf) / sizeof(cf[0]); i++)
    cf[i] = (float)((int)(i % 17) - 8) * 0.25f;
  for (size_t i = 0; i < sizeof(ai) / sizeof(ai[0]); i++)
    ai[i] = (int8_t)((int)(i % 23) - 11);
  for (size_t i = 0; i < sizeof(bi) / sizeof(bi[0]); i++)
    bi[i] = (int8_t)((int)(i % 19) - 9);
  for (size_t i = 0; i < sizeof(ci) / sizeof(ci[0]); i++)
    ci[i] = UINT32_C(0xfffffff0) + (uint32_t)(i % 31);
  static const uint8_t e4_values[] = {
      0x00, 0x01, 0x08, 0x10, 0x30, 0x38, 0x3c, 0x40, 0xb8};
  static const uint8_t e5_values[] = {
      0x00, 0x01, 0x04, 0x10, 0x34, 0x3c, 0x40, 0xbc};
  for (size_t i = 0; i < sizeof(a8); i++)
    a8[i] = e4_values[i % (sizeof(e4_values) / sizeof(e4_values[0]))];
  for (size_t i = 0; i < sizeof(b8); i++)
    b8[i] = e5_values[i % (sizeof(e5_values) / sizeof(e5_values[0]))];

  memset(expected_f, 0x5a, sizeof(expected_f));
  memset(tiled_f, 0x5a, sizeof(tiled_f));
  memset(expected_transpose, 0x6b, sizeof(expected_transpose));
  memset(tiled_transpose, 0x6b, sizeof(tiled_transpose));
  memset(expected_fp8, 0x7c, sizeof(expected_fp8));
  memset(tiled_fp8, 0x7c, sizeof(tiled_fp8));
  memset(expected_fp8_transpose, 0x8d, sizeof(expected_fp8_transpose));
  memset(tiled_fp8_transpose, 0x8d, sizeof(tiled_fp8_transpose));
  memset(expected_i, 0xa5, sizeof(expected_i));
  memset(tiled_i, 0xa5, sizeof(tiled_i));
  reference_f32(af, bf, cf, expected_f, M, N, K, LDA, LDB, LDC, LDD,
                0, 0);
  reference_f32(af, bf, cf, expected_transpose, M, N, K, LDA, LDB,
                LDC, LDD, 1, 1);
  reference_i8(ai, bi, ci, expected_i, M, N, K, LDA, LDB, LDC, LDD);
  reference_fp8(a8, b8, cf, expected_fp8, M, N, K, LDA, LDB, LDC,
                LDD, ROW_MAJOR, COLUMN_MAJOR, 0, 0);
  reference_fp8(a8, b8, cf, expected_fp8_transpose, M, N, K, LDA,
                LDB, LDC, LDD, COLUMN_MAJOR, ROW_MAJOR, 1, 1);
  for (uint32_t row = 0; row < M; row += TILE_M) {
    for (uint32_t column = 0; column < N; column += TILE_N) {
      region_f32(af, bf, cf, tiled_f, row, column, M, N, K, TILE_M,
                 TILE_N, LDA, LDB, LDC, LDD, 0, 0);
      region_f32(af, bf, cf, tiled_transpose, row, column, M, N, K,
                 TILE_M, TILE_N, LDA, LDB, LDC, LDD, 1, 1);
      region_i8(ai, bi, ci, tiled_i, row, column, M, N, K, TILE_M,
                TILE_N, LDA, LDB, LDC, LDD);
      region_fp8(a8, b8, cf, tiled_fp8, row, column, M, N, K,
                 TILE_M, TILE_N, LDA, LDB, LDC, LDD, ROW_MAJOR,
                 COLUMN_MAJOR, 0, 0);
      region_fp8(a8, b8, cf, tiled_fp8_transpose, row, column, M, N,
                 K, TILE_M, TILE_N, LDA, LDB, LDC, LDD,
                 COLUMN_MAJOR, ROW_MAJOR, 1, 1);
    }
  }
  if (memcmp(expected_f, tiled_f, sizeof(expected_f)) != 0) return 1;
  if (memcmp(expected_i, tiled_i, sizeof(expected_i)) != 0) return 2;
  if (memcmp(expected_transpose, tiled_transpose,
             sizeof(expected_transpose)) != 0)
    return 3;
  if (memcmp(expected_fp8, tiled_fp8, sizeof(expected_fp8)) != 0) return 4;
  if (memcmp(expected_fp8_transpose, tiled_fp8_transpose,
             sizeof(expected_fp8_transpose)) != 0)
    return 5;

  /* K=0 is C->D, and a completely out-of-range region is a no-op. */
  memset(tiled_f, 0x3c, sizeof(tiled_f));
  for (uint32_t row = 0; row < M; row += TILE_M)
    for (uint32_t column = 0; column < N; column += TILE_N)
      region_f32(af, bf, cf, tiled_f, row, column, M, N, 0, TILE_M,
                 TILE_N, LDA, LDB, LDC, LDD, 0, 0);
  for (uint32_t row = 0; row < M; row++)
    for (uint32_t column = 0; column < N; column++)
      if (tiled_f[at(row, column, LDD, ROW_MAJOR)] !=
          cf[at(row, column, LDC, COLUMN_MAJOR)])
        return 6;
  float before = tiled_f[0];
  region_f32(af, bf, cf, tiled_f, M, N, M, N, K, TILE_M, TILE_N,
             LDA, LDB, LDC, LDD, 1, 1);
  if (tiled_f[0] != before) return 7;

  static const uint8_t e2m1_values[] = {
      0x00, 0x01, 0x02, 0x03, 0x06, 0x08, 0x09, 0x0d};
  static const uint8_t e2m3_values[] = {
      0x00, 0x01, 0x07, 0x08, 0x0f, 0x18, 0x21, 0x2d};
  static const uint8_t e3m2_values[] = {
      0x00, 0x01, 0x03, 0x04, 0x0c, 0x1b, 0x21, 0x35};
  static const uint8_t ue8m0_values[] = {0x7e, 0x7f, 0x80};
  static const uint8_t ue4m3_values[] = {0x30, 0x38, 0x40};
  int narrow_result = test_narrow_formats();
  if (narrow_result) return 8 + narrow_result;
  int scaled_result = test_scaled_case(
      6, 6, decode_e3m2, decode_e2m3, 32, decode_ue8m0, e3m2_values,
      sizeof(e3m2_values), e2m3_values, sizeof(e2m3_values), ue8m0_values,
      sizeof(ue8m0_values));
  if (scaled_result) return 20 + scaled_result;
  scaled_result = test_scaled_case(
      4, 4, decode_e2m1, decode_e2m1, 32, decode_ue8m0, e2m1_values,
      sizeof(e2m1_values), e2m1_values, sizeof(e2m1_values), ue8m0_values,
      sizeof(ue8m0_values));
  if (scaled_result) return 30 + scaled_result;
  scaled_result = test_scaled_case(
      4, 4, decode_e2m1, decode_e2m1, 16, decode_ue4m3, e2m1_values,
      sizeof(e2m1_values), e2m1_values, sizeof(e2m1_values), ue4m3_values,
      sizeof(ue4m3_values));
  if (scaled_result) return 40 + scaled_result;
  scaled_result = test_scaled_case(
      8, 8, decode_e4m3, decode_e5m2, 32, decode_ue8m0, e4_values,
      sizeof(e4_values), e5_values, sizeof(e5_values), ue8m0_values,
      sizeof(ue8m0_values));
  if (scaled_result) return 50 + scaled_result;

  int sparse_result = test_sparse_case(ROW_MAJOR, 0);
  if (sparse_result) return 60 + sparse_result;
  sparse_result = test_sparse_case(ROW_MAJOR, 1);
  if (sparse_result) return 70 + sparse_result;

  puts("tensor_matmul_oracle: exact tails, transpose, layouts, wrap, structured 2:4, FP8/FP6/FP4 block scales, packed streams, K=0, and no-op bounds OK");
  return 0;
}
