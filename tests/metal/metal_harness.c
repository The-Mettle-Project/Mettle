#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef METAL_HARNESS_NATIVE
#include "msl_interp.h"
#endif

typedef struct {
  void *host;
  size_t size;
  uint64_t address;
  void *handle;
} Buf;

typedef struct Backend Backend;

struct Backend {
  const char *name;
  void *state;
  int (*load)(Backend *backend, const char *source, size_t length, char *error,
              size_t error_size);
  int (*buffer)(Backend *backend, size_t size, Buf *out);
  int (*dispatch)(Backend *backend, const char *kernel, const uint32_t grid[3],
                  const uint32_t block[3], const void *args, size_t args_size,
                  size_t threadgroup_bytes, char *error, size_t error_size);
  void (*destroy)(Backend *backend);
};

#ifndef METAL_HARNESS_NATIVE

typedef struct {
  MslProgram *program;
  void **blocks;
  size_t count;
  size_t capacity;
  int spurious;
} InterpState;

static int interp_load(Backend *backend, const char *source, size_t length,
                       char *error, size_t error_size) {
  InterpState *state = (InterpState *)backend->state;
  state->program = msl_program_load(source, length, error, error_size);
  return state->program != NULL;
}

static int interp_buffer(Backend *backend, size_t size, Buf *out) {
  InterpState *state = (InterpState *)backend->state;
  void *host = calloc(size ? size : 1, 1);
  if (!host) {
    return 0;
  }
  if (state->count == state->capacity) {
    size_t next = state->capacity ? state->capacity * 2 : 32;
    void **grown = (void **)realloc(state->blocks, next * sizeof(void *));
    if (!grown) {
      free(host);
      return 0;
    }
    state->blocks = grown;
    state->capacity = next;
  }
  state->blocks[state->count++] = host;
  out->host = host;
  out->size = size;
  out->address = msl_device_register(state->program, host, size);
  out->handle = NULL;
  return out->address != 0;
}

static int interp_dispatch(Backend *backend, const char *kernel,
                           const uint32_t grid[3], const uint32_t block[3],
                           const void *args, size_t args_size,
                           size_t threadgroup_bytes, char *error,
                           size_t error_size) {
  InterpState *state = (InterpState *)backend->state;
  MslDispatch dispatch;
  memset(&dispatch, 0, sizeof(dispatch));
  dispatch.kernel = kernel;
  memcpy(dispatch.grid, grid, sizeof(dispatch.grid));
  memcpy(dispatch.block, block, sizeof(dispatch.block));
  dispatch.args = args;
  dispatch.args_size = args_size;
  dispatch.threadgroup_bytes = threadgroup_bytes;
  dispatch.simd_width = 32;
  dispatch.spurious_cas = state->spurious;
  return msl_dispatch(state->program, &dispatch, error, error_size);
}

static void interp_destroy(Backend *backend) {
  InterpState *state = (InterpState *)backend->state;
  for (size_t i = 0; i < state->count; i++) {
    free(state->blocks[i]);
  }
  free(state->blocks);
  if (state->program) {
    msl_program_free(state->program);
  }
  free(state);
}

static int interp_backend(Backend *backend, int spurious) {
  InterpState *state = (InterpState *)calloc(1, sizeof(InterpState));
  if (!state) {
    return 0;
  }
  state->spurious = spurious;
  backend->name = spurious ? "interpreter (spurious CAS failures)"
                           : "interpreter";
  backend->state = state;
  backend->load = interp_load;
  backend->buffer = interp_buffer;
  backend->dispatch = interp_dispatch;
  backend->destroy = interp_destroy;
  return 1;
}

#else

#include "../../src/runtime/metal_provider.h"

static int native_load(Backend *backend, const char *source, size_t length,
                       char *error, size_t error_size) {
  return mettle_metal_load((MettleMetal *)backend->state, source, length, 3, 2,
                           error, error_size);
}

static int native_buffer(Backend *backend, size_t size, Buf *out) {
  MettleMetal *metal = (MettleMetal *)backend->state;
  out->address = mettle_metal_alloc(metal, size);
  out->host = out->address ? mettle_metal_host(metal, out->address) : NULL;
  out->size = size;
  out->handle = NULL;
  return out->host != NULL;
}

static int native_dispatch(Backend *backend, const char *kernel,
                           const uint32_t grid[3], const uint32_t block[3],
                           const void *args, size_t args_size,
                           size_t threadgroup_bytes, char *error,
                           size_t error_size) {
  return mettle_metal_launch((MettleMetal *)backend->state, kernel, grid, block,
                             args, args_size, threadgroup_bytes, error,
                             error_size) &&
         mettle_metal_sync((MettleMetal *)backend->state, error, error_size);
}

static void native_destroy(Backend *backend) {
  mettle_metal_close((MettleMetal *)backend->state);
}

static int metal_backend(Backend *backend) {
  char error[512];
  MettleMetal *metal = mettle_metal_open(error, sizeof(error));
  if (!metal) {
    fprintf(stderr, "metal_harness: %s\n", error);
    return 0;
  }
  backend->name = mettle_metal_device_name(metal);
  backend->state = metal;
  backend->load = native_load;
  backend->buffer = native_buffer;
  backend->dispatch = native_dispatch;
  backend->destroy = native_destroy;
  return 1;
}

#endif

typedef struct {
  unsigned char bytes[512];
  size_t size;
  size_t align;
} Args;

static void args_put(Args *args, const void *value, size_t size,
                     size_t align) {
  size_t at = (args->size + align - 1) / align * align;
  memcpy(args->bytes + at, value, size);
  args->size = at + size;
  if (align > args->align) {
    args->align = align;
  }
}

static void args_ptr(Args *args, const Buf *buf) {
  args_put(args, &buf->address, 8, 8);
}

static void args_i32(Args *args, int32_t value) { args_put(args, &value, 4, 4); }

static void args_f32(Args *args, float value) { args_put(args, &value, 4, 4); }

static size_t args_total(const Args *args) {
  size_t align = args->align ? args->align : 1;
  return (args->size + align - 1) / align * align;
}

static Backend *g_backend;
static int g_failures;
static int g_passes;
static const char *g_only;

static Buf make(size_t size) {
  Buf buf;
  memset(&buf, 0, sizeof(buf));
  if (!g_backend->buffer(g_backend, size, &buf)) {
    fprintf(stderr, "metal_harness: could not allocate %zu bytes\n", size);
    exit(2);
  }
  return buf;
}

static int run(const char *kernel, uint32_t gx, uint32_t gy, uint32_t gz,
               uint32_t bx, uint32_t by, uint32_t bz, const Args *args,
               size_t threadgroup_bytes) {
  uint32_t grid[3] = {gx, gy, gz};
  uint32_t block[3] = {bx, by, bz};
  char error[2048];
  error[0] = '\0';
  if (!g_backend->dispatch(g_backend, kernel, grid, block,
                           args ? args->bytes : NULL,
                           args ? args_total(args) : 0, threadgroup_bytes,
                           error, sizeof(error))) {
    printf("  dispatch of %s failed: %s\n", kernel, error);
    return 0;
  }
  return 1;
}

static int report(const char *name, int ok) {
  printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
  if (ok) {
    g_passes++;
  } else {
    g_failures++;
  }
  return ok;
}

static int close_enough(float got, double want, double relative,
                        double absolute) {
  double diff = fabs((double)got - want);
  if (got != got || want != want) {
    return got != got && want != want;
  }
  return diff <= absolute || diff <= relative * fabs(want);
}

static uint32_t f32_bits(float f) {
  uint32_t bits;
  memcpy(&bits, &f, 4);
  return bits;
}

static float f32_from(uint32_t bits) {
  float f;
  memcpy(&f, &bits, 4);
  return f;
}

static float metal_f32(float f) {
  uint32_t bits = f32_bits(f);
  return (bits & 0x7f800000u) == 0 ? f32_from(bits & 0x80000000u) : f;
}

static float half_to_float(uint16_t h) {
  uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
  int exponent = (h >> 10) & 0x1f;
  uint32_t mantissa = h & 0x3ffu;
  double value;
  if (exponent == 31) {
    return f32_from(sign | 0x7f800000u | (mantissa << 13));
  }
  if (exponent == 0) {
    value = ldexp((double)mantissa, -24);
  } else {
    value = ldexp((double)(mantissa | 0x400u), exponent - 25);
  }
  return sign ? -(float)value : (float)value;
}

static uint16_t half_from_double(double value, double residual) {
  uint16_t sign = 0;
  double magnitude;
  int exponent;
  double scaled;
  double floor_scaled;
  double fraction;
  uint32_t mantissa;
  if (value != value) {
    return 0x7e00u;
  }
  if (value < 0 || (value == 0 && signbit(value))) {
    sign = 0x8000u;
    value = -value;
    residual = -residual;
  }
  magnitude = value;
  if (magnitude >= 65520.0) {
    return (uint16_t)(sign | 0x7c00u);
  }
  if (magnitude < ldexp(1.0, -14)) {
    scaled = magnitude * ldexp(1.0, 24);
    exponent = 0;
  } else {
    frexp(magnitude, &exponent);
    exponent -= 1;
    scaled = ldexp(magnitude, 10 - exponent);
  }
  floor_scaled = floor(scaled);
  fraction = scaled - floor_scaled;
  mantissa = (uint32_t)floor_scaled;
  if (fraction > 0.5 || (fraction == 0.5 && residual > 0) ||
      (fraction == 0.5 && residual == 0 && (mantissa & 1u))) {
    mantissa++;
  }
  if (exponent == 0 && magnitude < ldexp(1.0, -14)) {
    if (mantissa >= 1024u) {
      return (uint16_t)(sign | 0x0400u);
    }
    return (uint16_t)(sign | mantissa);
  }
  if (mantissa >= 2048u) {
    mantissa >>= 1;
    exponent++;
  }
  if (exponent > 15) {
    return (uint16_t)(sign | 0x7c00u);
  }
  return (uint16_t)(sign | (uint16_t)((exponent + 15) << 10) |
                    (uint16_t)(mantissa & 0x3ffu));
}

static uint16_t half_from_float(float f) {
  if (f != f) {
    return (uint16_t)(((f32_bits(f) >> 16) & 0x8000u) | 0x7e00u |
                      ((f32_bits(f) >> 13) & 0x3ffu));
  }
  if (isinf(f)) {
    return (uint16_t)(((f32_bits(f) >> 16) & 0x8000u) | 0x7c00u);
  }
  return half_from_double((double)f, 0.0);
}

static float fma_exact(float a, float b, float c) {
  double product = (double)a * (double)b;
  double addend = (double)c;
  double sum = product + addend;
  double back = sum - product;
  double error = (product - (sum - back)) + (addend - back);
  if (error != 0.0 && sum == sum && sum - sum == 0.0) {
    uint64_t bits;
    memcpy(&bits, &sum, 8);
    if ((bits & 1u) == 0) {
      bits = (error > 0.0) == (sum > 0.0) ? bits + 1u : bits - 1u;
      memcpy(&sum, &bits, 8);
    }
  }
  return (float)sum;
}

static uint16_t half_fma(uint16_t a, uint16_t b, uint16_t c) {
  double product = (double)half_to_float(a) * (double)half_to_float(b);
  double addend = (double)half_to_float(c);
  double sum = product + addend;
  double back = sum - product;
  double error = (product - (sum - back)) + (addend - back);
  float fa = half_to_float(a), fb = half_to_float(b), fc = half_to_float(c);
  if (fa != fa || fb != fb || fc != fc) {
    return 0x7e00u;
  }
  if (isinf(fa) || isinf(fb) || isinf(fc)) {
    return half_from_float(fa * fb + fc);
  }
  return half_from_double(sum, error);
}

static uint16_t half_add(uint16_t a, uint16_t b) {
  return half_from_float(half_to_float(a) + half_to_float(b));
}

static uint16_t half_mul(uint16_t a, uint16_t b) {
  float product = half_to_float(a) * half_to_float(b);
  return half_from_float(product);
}

static uint32_t rng_state = 0x2545F491u;

static uint32_t rng(void) {
  rng_state ^= rng_state << 13;
  rng_state ^= rng_state >> 17;
  rng_state ^= rng_state << 5;
  return rng_state;
}

static float rng_float(float lo, float hi) {
  return lo + (hi - lo) * (float)(rng() & 0xffffffu) / 16777216.0f;
}

static uint16_t rng_half(void) {
  float v = rng_float(-2.0f, 2.0f);
  return half_from_float(v);
}

static int wants(const char *name) {
  return !g_only || strcmp(g_only, name) == 0;
}

static void contract_index_3d(void) {
  Buf out = make(8 * 24 * 12 * 4);
  Args args = {{0}, 0, 0};
  int ok;
  args_ptr(&args, &out);
  ok = run("index_3d", 2, 2, 2, 2, 3, 4, &args, 0);
  for (int gz = 0; gz < 2 && ok; gz++)
    for (int gy = 0; gy < 2 && ok; gy++)
      for (int gx = 0; gx < 2 && ok; gx++)
        for (int tz = 0; tz < 4 && ok; tz++)
          for (int ty = 0; ty < 3 && ok; ty++)
            for (int tx = 0; tx < 2 && ok; tx++) {
              int block_linear = (gz * 2 + gy) * 2 + gx;
              int local = (tz * 3 + ty) * 2 + tx;
              int base = (block_linear * 24 + local) * 12;
              int want[12] = {tx, ty, tz, gx, gy, gz, 2, 3, 4, 2, 2, 2};
              for (int k = 0; k < 12; k++) {
                if (((int32_t *)out.host)[base + k] != want[k]) {
                  printf("  index_3d slot %d of thread (%d,%d,%d) block "
                         "(%d,%d,%d) = %d, want %d\n",
                         k, tx, ty, tz, gx, gy, gz,
                         ((int32_t *)out.host)[base + k], want[k]);
                  ok = 0;
                  break;
                }
              }
            }
  report("index_3d", ok);
}

static void contract_saxpy(void) {
  int n = 1000;
  Buf x = make((size_t)n * 4), y = make((size_t)n * 4);
  float *want = (float *)malloc((size_t)n * 4);
  Args args = {{0}, 0, 0};
  int ok;
  float alpha = 1.75f;
  for (int i = 0; i < n; i++) {
    float xv = rng_float(-10, 10);
    float yv = rng_float(-10, 10);
    float product;
    ((float *)x.host)[i] = xv;
    ((float *)y.host)[i] = yv;
    product = alpha * xv;
    want[i] = product + yv;
  }
  args_f32(&args, alpha);
  args_ptr(&args, &x);
  args_ptr(&args, &y);
  args_i32(&args, n);
  ok = run("saxpy_odd", (uint32_t)((n + 127) / 128), 1, 1, 128, 1, 1, &args, 0);
  for (int i = 0; i < n && ok; i++) {
    if (f32_bits(((float *)y.host)[i]) != f32_bits(want[i])) {
      printf("  saxpy y[%d] = %.9g, want %.9g\n", i, ((float *)y.host)[i],
             want[i]);
      ok = 0;
    }
  }
  free(want);
  report("saxpy_odd", ok);
}

static void contract_branch_return_join(void) {
  int n = 257;
  Buf x = make((size_t)n * 4), y = make((size_t)n * 4), kinds = make((size_t)n * 4),
      out = make((size_t)n * 4);
  Args args = {{0}, 0, 0};
  int ok;
  for (int i = 0; i < n; i++) {
    ((float *)x.host)[i] = rng_float(-4, 4);
    ((float *)y.host)[i] = rng_float(-2, 2);
    ((int32_t *)kinds.host)[i] = i % 3;
  }
  args_ptr(&args, &x);
  args_ptr(&args, &y);
  args_ptr(&args, &kinds);
  args_ptr(&args, &out);
  args_i32(&args, n);
  ok = run("branch_return_join", 3, 1, 1, 128, 1, 1, &args, 0);
  for (int i = 0; i < n && ok; i++) {
    double v = ((float *)x.host)[i];
    double activation = i % 3 == 0 ? v / (1.0 + exp(-v))
                        : i % 3 == 1 ? v * 0.5
                        : v > 0 ? v
                                : 0.0;
    double want = activation * ((float *)y.host)[i];
    if (!close_enough(((float *)out.host)[i], want, 1e-5, 1e-6)) {
      printf("  branch_return_join out[%d] = %.9g, want %.9g\n", i,
             ((float *)out.host)[i], want);
      ok = 0;
    }
  }
  report("branch_return_join", ok);
}

static void contract_row_norm(void) {
  int rows = 37, cols = 19;
  Buf in = make((size_t)rows * cols * 4), out = make((size_t)rows * 4);
  Args args = {{0}, 0, 0};
  int ok;
  for (int i = 0; i < rows * cols; i++) {
    ((float *)in.host)[i] = rng_float(-3, 3);
  }
  args_ptr(&args, &in);
  args_ptr(&args, &out);
  args_i32(&args, rows);
  args_i32(&args, cols);
  ok = run("row_norm", 2, 1, 1, 32, 1, 1, &args, 0);
  for (int r = 0; r < rows && ok; r++) {
    float sum = 0.0f;
    for (int c = 0; c < cols; c++) {
      float v = ((float *)in.host)[r * cols + c];
      float square = v * v;
      sum = sum + square;
    }
    if (f32_bits(((float *)out.host)[r]) != f32_bits((float)sqrt((double)sum))) {
      printf("  row_norm[%d] = %.9g, want %.9g\n", r, ((float *)out.host)[r],
             (double)(float)sqrt((double)sum));
      ok = 0;
    }
  }
  report("row_norm", ok);
}

static void contract_staged_copy(void) {
  int n = 100;
  Buf x = make((size_t)n * 4), out = make((size_t)n * 4);
  Args args = {{0}, 0, 0};
  int ok;
  for (int i = 0; i < n; i++) {
    ((float *)x.host)[i] = (float)i * 1.25f - 7.0f;
  }
  args_ptr(&args, &x);
  args_ptr(&args, &out);
  args_i32(&args, n);
  ok = run("staged_copy", 2, 1, 1, 64, 1, 1, &args, 0);
  for (int i = 0; i < n && ok; i++) {
    if (((float *)out.host)[i] != ((float *)x.host)[i]) {
      printf("  staged_copy out[%d] = %g\n", i, ((float *)out.host)[i]);
      ok = 0;
    }
  }
  report("staged_copy", ok);
}

static void contract_async_stage(void) {
  Buf src = make(128 * 4), out = make(128 * 4);
  Args args = {{0}, 0, 0};
  int ok;
  for (int i = 0; i < 128; i++) {
    ((uint32_t *)src.host)[i] = 0x1000u + (uint32_t)i * 7u;
  }
  args_ptr(&args, &src);
  args_ptr(&args, &out);
  ok = run("async_stage_u32x4", 1, 1, 1, 32, 1, 1, &args, 0);
  for (int lane = 0; lane < 32 && ok; lane++) {
    for (int j = 0; j < 4; j++) {
      int base = lane * 4;
      uint32_t want = ((uint32_t *)src.host)[(base + 124 + j) % 128];
      if (((uint32_t *)out.host)[base + j] != want) {
        printf("  async_stage out[%d] = %u, want %u\n", base + j,
               ((uint32_t *)out.host)[base + j], want);
        ok = 0;
        break;
      }
    }
  }
  report("async_stage_u32x4", ok);
}

static void contract_auto_stage(void) {
  Buf src = make(32 * 4), out = make(32 * 4);
  Args args = {{0}, 0, 0};
  int ok;
  for (int i = 0; i < 32; i++) {
    ((uint32_t *)src.host)[i] = rng();
  }
  args_ptr(&args, &src);
  args_ptr(&args, &out);
  ok = run("auto_stage_u32", 1, 1, 1, 32, 1, 1, &args, 0);
  for (int lane = 0; lane < 32 && ok; lane++) {
    uint32_t mix = (uint32_t)lane * 1664525u + 1013904223u;
    uint32_t want = ((uint32_t *)src.host)[(lane + 31) % 32] ^ mix;
    if (((uint32_t *)out.host)[lane] != want) {
      printf("  auto_stage out[%d] = %u, want %u\n", lane,
             ((uint32_t *)out.host)[lane], want);
      ok = 0;
    }
  }
  report("auto_stage_u32", ok);
}

static void contract_dynamic_staged(void) {
  int n = 100;
  Buf x = make((size_t)n * 4), out = make((size_t)n * 4);
  Args args = {{0}, 0, 0};
  int ok;
  for (int i = 0; i < n; i++) {
    ((float *)x.host)[i] = (float)(i * i) * 0.5f;
  }
  args_ptr(&args, &x);
  args_ptr(&args, &out);
  args_i32(&args, n);
  ok = run("dynamic_staged_copy", 2, 1, 1, 64, 1, 1, &args, 512);
  for (int i = 0; i < n && ok; i++) {
    if (((float *)out.host)[i] != ((float *)x.host)[i]) {
      printf("  dynamic_staged_copy out[%d] = %g, want %g\n", i,
             ((float *)out.host)[i], ((float *)x.host)[i]);
      ok = 0;
    }
  }
  report("dynamic_staged_copy", ok);
}

static void contract_subgroup_contract(void) {
  int n = 70, threads = 96;
  Buf fsrc = make((size_t)threads * 4), fdst = make((size_t)threads * 4),
      usrc = make((size_t)threads * 4), udst = make((size_t)threads * 4);
  Args args = {{0}, 0, 0};
  int ok;
  for (int i = 0; i < threads; i++) {
    ((float *)fsrc.host)[i] = (float)((i * 13) % 17) - 8.0f;
    ((uint32_t *)usrc.host)[i] = (uint32_t)i * 2654435761u;
  }
  args_ptr(&args, &fsrc);
  args_ptr(&args, &fdst);
  args_ptr(&args, &usrc);
  args_ptr(&args, &udst);
  args_i32(&args, n);
  ok = run("subgroup_contract", 1, 1, 1, (uint32_t)threads, 1, 1, &args, 0);
  for (int i = 0; i < n && ok; i++) {
    int group = i / 32;
    double fsum = 0.0;
    uint32_t usum = 0;
    for (int lane = 0; lane < 32; lane++) {
      int j = group * 32 + lane;
      if (j < n) {
        fsum += ((float *)fsrc.host)[j];
        usum += ((uint32_t *)usrc.host)[j];
      }
    }
    if (((float *)fdst.host)[i] != (float)(fsum + ((float *)fsrc.host)[group * 32]) ||
        ((uint32_t *)udst.host)[i] != usum + ((uint32_t *)usrc.host)[group * 32]) {
      printf("  subgroup_contract[%d] = %g/%u\n", i, ((float *)fdst.host)[i],
             ((uint32_t *)udst.host)[i]);
      ok = 0;
    }
  }
  report("subgroup_contract", ok);
}

static void contract_subgroup_extended(void) {
  int n = 75, threads = 96;
  Buf fsrc = make((size_t)threads * 4), fdst = make((size_t)n * 16),
      usrc = make((size_t)threads * 4), udst = make((size_t)n * 16);
  Args args = {{0}, 0, 0};
  int ok;
  for (int i = 0; i < threads; i++) {
    ((float *)fsrc.host)[i] = (float)((i * 29) % 31) - 15.0f;
    ((uint32_t *)usrc.host)[i] = (rng() % 100000u) + 1u;
  }
  args_ptr(&args, &fsrc);
  args_ptr(&args, &fdst);
  args_ptr(&args, &usrc);
  args_ptr(&args, &udst);
  args_i32(&args, n);
  ok = run("subgroup_extended", 1, 1, 1, (uint32_t)threads, 1, 1, &args, 0);
  for (int i = 0; i < n && ok; i++) {
    int group = i / 32, lane = i % 32;
    float fmin_value = 3.4e38f, fmax_value = -3.4e38f;
    double finc = 0, fexc = 0;
    uint32_t umin_value = 0xffffffffu, umax_value = 0, uinc = 0, uexc = 0;
    for (int l = 0; l < 32; l++) {
      int j = group * 32 + l;
      float fv = j < n ? ((float *)fsrc.host)[j] : 0.0f;
      uint32_t uv = j < n ? ((uint32_t *)usrc.host)[j] : 0u;
      float fminin = j < n ? fv : 340282300000000000000000000000000000000.0f;
      float fmaxin = j < n ? fv : -340282300000000000000000000000000000000.0f;
      uint32_t uminin = j < n ? uv : 0xffffffffu;
      if (fminin < fmin_value) fmin_value = fminin;
      if (fmaxin > fmax_value) fmax_value = fmaxin;
      if (uminin < umin_value) umin_value = uminin;
      if (uv > umax_value) umax_value = uv;
      if (l <= lane) { finc += fv; uinc += uv; }
      if (l < lane) { fexc += fv; uexc += uv; }
    }
    if (((float *)fdst.host)[i] != fmin_value ||
        ((float *)fdst.host)[n + i] != fmax_value ||
        ((float *)fdst.host)[2 * n + i] != (float)finc ||
        ((float *)fdst.host)[3 * n + i] != (float)fexc ||
        ((uint32_t *)udst.host)[i] != umin_value ||
        ((uint32_t *)udst.host)[n + i] != umax_value ||
        ((uint32_t *)udst.host)[2 * n + i] != uinc ||
        ((uint32_t *)udst.host)[3 * n + i] != uexc) {
      printf("  subgroup_extended[%d] differs\n", i);
      ok = 0;
    }
  }
  report("subgroup_extended", ok);
}

static void contract_exchange_vote(void) {
  int threads = 48;
  Buf usrc = make((size_t)threads * 4), uout = make((size_t)threads * 28),
      fsrc = make((size_t)threads * 4), fout = make((size_t)threads * 4);
  Args args = {{0}, 0, 0};
  int ok;
  for (int i = 0; i < threads; i++) {
    ((uint32_t *)usrc.host)[i] = 0x9000u + (uint32_t)i;
    ((float *)fsrc.host)[i] = (float)i + 0.25f;
  }
  args_ptr(&args, &usrc);
  args_ptr(&args, &uout);
  args_ptr(&args, &fsrc);
  args_ptr(&args, &fout);
  ok = run("subgroup_exchange_vote", 1, 1, 1, (uint32_t)threads, 1, 1, &args, 0);
  for (int i = 0; i < threads && ok; i++) {
    int group = i / 32, lane = i % 32;
    int active = group == 0 ? 32 : threads - 32;
    int source = (lane * 5 + 7) % 32;
    int source_active = source < active;
    uint32_t want_shuffle = source_active
                                ? ((uint32_t *)usrc.host)[group * 32 + source]
                                : ((uint32_t *)usrc.host)[i];
    float want_fshuffle = source_active
                              ? ((float *)fsrc.host)[group * 32 + source]
                              : ((float *)fsrc.host)[i];
    uint32_t ballot = 0;
    int any = 0, all = 1;
    for (int l = 0; l < active; l++) {
      int predicate = l % 3 == 1;
      if (predicate) {
        ballot |= 1u << l;
        any = 1;
      } else {
        all = 0;
      }
    }
    uint32_t *row = (uint32_t *)uout.host + i * 7;
    if (row[0] != want_shuffle || row[1] != ballot || row[2] != 0 ||
        row[3] != (uint32_t)any || row[4] != 1 || row[5] != (uint32_t)all ||
        row[6] != 0 || ((float *)fout.host)[i] != want_fshuffle) {
      printf("  exchange_vote[%d]: %u %u %u %u %u %u %u %g\n", i, row[0],
             row[1], row[2], row[3], row[4], row[5], row[6],
             ((float *)fout.host)[i]);
      ok = 0;
    }
  }
  report("subgroup_exchange_vote", ok);
}

static void contract_softmax(void) {
  int rows = 5, cols = 70, ldin = 72, ldout = 75;
  Buf in = make((size_t)rows * ldin * 4), out = make((size_t)rows * ldout * 4);
  Args args = {{0}, 0, 0};
  int ok;
  for (int i = 0; i < rows * ldin; i++) {
    ((float *)in.host)[i] = rng_float(-6, 6);
  }
  args_ptr(&args, &in);
  args_ptr(&args, &out);
  args_i32(&args, rows);
  args_i32(&args, cols);
  args_i32(&args, ldin);
  args_i32(&args, ldout);
  ok = run("softmax_rows_f32", (uint32_t)rows, 1, 1, 32, 1, 1, &args, 0);
  for (int r = 0; r < rows && ok; r++) {
    double max_value = -1e300, sum = 0;
    for (int c = 0; c < cols; c++) {
      double v = ((float *)in.host)[r * ldin + c];
      if (v > max_value) max_value = v;
    }
    for (int c = 0; c < cols; c++) {
      sum += exp(((float *)in.host)[r * ldin + c] - max_value);
    }
    for (int c = 0; c < cols; c++) {
      double want = exp(((float *)in.host)[r * ldin + c] - max_value) / sum;
      if (!close_enough(((float *)out.host)[r * ldout + c], want, 1e-5, 1e-9)) {
        printf("  softmax[%d][%d] = %.9g, want %.9g\n", r, c,
               ((float *)out.host)[r * ldout + c], want);
        ok = 0;
        break;
      }
    }
  }
  report("softmax_rows_f32", ok);
}

static void contract_layer_norm(void) {
  int rows = 4, cols = 45, ldin = 48, ldout = 50;
  float epsilon = 1e-5f;
  Buf in = make((size_t)rows * ldin * 4), gamma = make((size_t)cols * 4),
      beta = make((size_t)cols * 4), out = make((size_t)rows * ldout * 4);
  Args args = {{0}, 0, 0};
  int ok;
  for (int i = 0; i < rows * ldin; i++) ((float *)in.host)[i] = rng_float(-3, 5);
  for (int c = 0; c < cols; c++) {
    ((float *)gamma.host)[c] = rng_float(0.5f, 1.5f);
    ((float *)beta.host)[c] = rng_float(-0.5f, 0.5f);
  }
  args_ptr(&args, &in);
  args_ptr(&args, &gamma);
  args_ptr(&args, &beta);
  args_ptr(&args, &out);
  args_i32(&args, rows);
  args_i32(&args, cols);
  args_i32(&args, ldin);
  args_i32(&args, ldout);
  args_f32(&args, epsilon);
  ok = run("layer_norm_rows_f32", (uint32_t)rows, 1, 1, 32, 1, 1, &args, 0);
  for (int r = 0; r < rows && ok; r++) {
    double mean = 0, variance = 0;
    for (int c = 0; c < cols; c++) mean += ((float *)in.host)[r * ldin + c];
    mean /= cols;
    for (int c = 0; c < cols; c++) {
      double d = ((float *)in.host)[r * ldin + c] - mean;
      variance += d * d;
    }
    variance /= cols;
    for (int c = 0; c < cols; c++) {
      double want = (((float *)in.host)[r * ldin + c] - mean) /
                        sqrt(variance + epsilon) * ((float *)gamma.host)[c] +
                    ((float *)beta.host)[c];
      if (!close_enough(((float *)out.host)[r * ldout + c], want, 1e-4, 1e-5)) {
        printf("  layer_norm[%d][%d] = %.9g, want %.9g\n", r, c,
               ((float *)out.host)[r * ldout + c], want);
        ok = 0;
        break;
      }
    }
  }
  report("layer_norm_rows_f32", ok);
}

static double mma_dot(const uint16_t *a, int lda, const uint16_t *b, int ldb,
                      int row, int col, int k0, int k) {
  double sum = 0;
  for (int q = k0; q < k0 + k; q++) {
    sum += (double)half_to_float(a[row * lda + q]) *
           (double)half_to_float(b[col * ldb + q]);
  }
  return sum;
}

static void contract_tensor_f16(void) {
  Buf a = make(16 * 16 * 2), b = make(16 * 16 * 2), c = make(16 * 16 * 4),
      d = make(16 * 16 * 4);
  Args args = {{0}, 0, 0};
  int ok;
  for (int i = 0; i < 256; i++) {
    ((uint16_t *)a.host)[i] = rng_half();
    ((uint16_t *)b.host)[i] = rng_half();
    ((float *)c.host)[i] = rng_float(-1, 1);
  }
  args_ptr(&args, &a);
  args_ptr(&args, &b);
  args_ptr(&args, &c);
  args_ptr(&args, &d);
  args_i32(&args, 16);
  args_i32(&args, 16);
  args_i32(&args, 16);
  args_i32(&args, 16);
  ok = run("tensor_f16_f32", 1, 1, 1, 32, 1, 1, &args, 0);
  for (int r = 0; r < 16 && ok; r++) {
    for (int col = 0; col < 16; col++) {
      double want = ((float *)c.host)[r * 16 + col] +
                    mma_dot((uint16_t *)a.host, 16, (uint16_t *)b.host, 16, r,
                            col, 0, 16);
      if (!close_enough(((float *)d.host)[r * 16 + col], want, 1e-5, 1e-4)) {
        printf("  tensor_f16 d[%d][%d] = %.9g, want %.9g\n", r, col,
               ((float *)d.host)[r * 16 + col], want);
        ok = 0;
        break;
      }
    }
  }
  report("tensor_f16_f32", ok);
}

static void contract_tensor_chain(void) {
  Buf a = make(16 * 64 * 2), b = make(16 * 64 * 2), c = make(16 * 16 * 4),
      d = make(16 * 16 * 4);
  Args args = {{0}, 0, 0};
  int ok;
  for (int i = 0; i < 16 * 64; i++) {
    ((uint16_t *)a.host)[i] = rng_half();
    ((uint16_t *)b.host)[i] = rng_half();
  }
  for (int i = 0; i < 256; i++) ((float *)c.host)[i] = rng_float(-1, 1);
  args_ptr(&args, &a);
  args_ptr(&args, &b);
  args_ptr(&args, &c);
  args_ptr(&args, &d);
  args_i32(&args, 64);
  args_i32(&args, 64);
  args_i32(&args, 16);
  args_i32(&args, 16);
  ok = run("tensor_chain4", 1, 1, 1, 32, 1, 1, &args, 0);
  for (int r = 0; r < 16 && ok; r++) {
    for (int col = 0; col < 16; col++) {
      double want = ((float *)c.host)[r * 16 + col] +
                    mma_dot((uint16_t *)a.host, 64, (uint16_t *)b.host, 64, r,
                            col, 0, 64);
      if (!close_enough(((float *)d.host)[r * 16 + col], want, 1e-5, 5e-4)) {
        printf("  tensor_chain4 d[%d][%d] = %.9g, want %.9g\n", r, col,
               ((float *)d.host)[r * 16 + col], want);
        ok = 0;
        break;
      }
    }
  }
  report("tensor_chain4", ok);
}

static void contract_gemm_full_tiles(void) {
  int m = 32, n = 48, k = 64;
  Buf a = make((size_t)m * k * 2), b = make((size_t)n * k * 2),
      c = make((size_t)m * n * 4), d = make((size_t)m * n * 4);
  Args args = {{0}, 0, 0};
  int ok;
  for (int i = 0; i < m * k; i++) ((uint16_t *)a.host)[i] = rng_half();
  for (int i = 0; i < n * k; i++) ((uint16_t *)b.host)[i] = rng_half();
  for (int i = 0; i < m * n; i++) ((float *)c.host)[i] = rng_float(-1, 1);
  args_ptr(&args, &a);
  args_ptr(&args, &b);
  args_ptr(&args, &c);
  args_ptr(&args, &d);
  args_i32(&args, m);
  args_i32(&args, n);
  args_i32(&args, k);
  args_i32(&args, k);
  args_i32(&args, k);
  args_i32(&args, n);
  args_i32(&args, n);
  ok = run("gemm_full_tiles_f16_f32", (uint32_t)(n / 16), (uint32_t)(m / 16), 1,
           32, 1, 1, &args, 0);
  for (int r = 0; r < m && ok; r++) {
    for (int col = 0; col < n; col++) {
      double want = ((float *)c.host)[r * n + col] +
                    mma_dot((uint16_t *)a.host, k, (uint16_t *)b.host, k, r,
                            col, 0, k);
      if (!close_enough(((float *)d.host)[r * n + col], want, 1e-5, 5e-4)) {
        printf("  gemm d[%d][%d] = %.9g, want %.9g\n", r, col,
               ((float *)d.host)[r * n + col], want);
        ok = 0;
        break;
      }
    }
  }
  report("gemm_full_tiles_f16_f32", ok);
}

static void contract_tensor_pipeline(void) {
  Buf a = make(512 * 2), b = make(512 * 2), c = make(256 * 4), d = make(256 * 4);
  Args args = {{0}, 0, 0};
  int ok;
  for (int i = 0; i < 512; i++) {
    ((uint16_t *)a.host)[i] = rng_half();
    ((uint16_t *)b.host)[i] = rng_half();
  }
  for (int i = 0; i < 256; i++) ((float *)c.host)[i] = rng_float(-1, 1);
  args_ptr(&args, &a);
  args_ptr(&args, &b);
  args_ptr(&args, &c);
  args_ptr(&args, &d);
  ok = run("tensor_pipeline_f16_f32", 1, 1, 1, 32, 1, 1, &args, 0);
  for (int r = 0; r < 16 && ok; r++) {
    for (int col = 0; col < 16; col++) {
      double want = ((float *)c.host)[r * 16 + col] +
                    mma_dot((uint16_t *)a.host, 16, (uint16_t *)b.host, 16, r,
                            col, 0, 16) +
                    mma_dot((uint16_t *)a.host + 256, 16,
                            (uint16_t *)b.host + 256, 16, r, col, 0, 16);
      if (!close_enough(((float *)d.host)[r * 16 + col], want, 1e-5, 5e-4)) {
        printf("  tensor_pipeline d[%d][%d] = %.9g, want %.9g\n", r, col,
               ((float *)d.host)[r * 16 + col], want);
        ok = 0;
        break;
      }
    }
  }
  report("tensor_pipeline_f16_f32", ok);
}

static int q4_value(const uint8_t *b, int position) {
  int byte = 16 * (position / 32) + position % 16;
  int high = position % 32 >= 16;
  int nibble = high ? b[byte] >> 4 : b[byte] & 15;
  return nibble - 8;
}

static void contract_q4_scaled(void) {
  Buf a = make(32 * 64), b = make(32 * 64 / 2), d = make(32 * 32 * 4),
      sa = make(32 * 2 * 4), sb = make(32 * 2 * 2);
  Args args = {{0}, 0, 0};
  int ok;
  for (int i = 0; i < 32 * 64; i++) ((int8_t *)a.host)[i] = (int8_t)(rng() & 0xff);
  for (int i = 0; i < 32 * 32; i++) ((uint8_t *)b.host)[i] = (uint8_t)(rng() & 0xff);
  for (int i = 0; i < 64; i++) {
    ((float *)sa.host)[i] = rng_float(0.001f, 0.05f);
    ((uint16_t *)sb.host)[i] = half_from_float(rng_float(0.001f, 0.05f));
  }
  args_ptr(&args, &a);
  args_ptr(&args, &b);
  args_ptr(&args, &d);
  args_ptr(&args, &sa);
  args_ptr(&args, &sb);
  ok = run("q4_scaled", 1, 1, 1, 32, 1, 1, &args, 0);
  for (int r = 0; r < 32 && ok; r++) {
    for (int col = 0; col < 32; col++) {
      float acc = 0.0f;
      for (int kb = 0; kb < 2; kb++) {
        int32_t dot = 0;
        float s_a = ((float *)sa.host)[r * 2 + kb];
        float s_b = half_to_float(((uint16_t *)sb.host)[col * 2 + kb]);
        float f;
        float t;
        float bias;
        for (int j = 0; j < 32; j++) {
          int q = kb * 32 + j;
          dot += (int32_t)((int8_t *)a.host)[r * 64 + q] *
                 q4_value((uint8_t *)b.host, col * 64 + q);
        }
        f = 12582912.0f + (float)dot;
        bias = 12582912.0f * s_b;
        t = fma_exact(f, s_b, -bias);
        acc = fma_exact(t, s_a, acc);
      }
      if (f32_bits(((float *)d.host)[r * 32 + col]) != f32_bits(acc)) {
        printf("  q4_scaled d[%d][%d] = %.9g (0x%08x), want %.9g (0x%08x)\n", r,
               col, ((float *)d.host)[r * 32 + col],
               f32_bits(((float *)d.host)[r * 32 + col]), acc, f32_bits(acc));
        ok = 0;
        break;
      }
    }
  }
  report("q4_scaled", ok);
}

static void contract_narrow_abi(void) {
  Buf out = make(4);
  Args args = {{0}, 0, 0};
  int8_t i8 = -100;
  uint8_t u8 = 250;
  int16_t i16 = -30000;
  uint16_t u16 = 60000;
  uint8_t flag = 1;
  int ok;
  args_put(&args, &i8, 1, 1);
  args_put(&args, &u8, 1, 1);
  args_put(&args, &i16, 2, 2);
  args_put(&args, &u16, 2, 2);
  args_put(&args, &flag, 1, 1);
  args_ptr(&args, &out);
  ok = run("narrow_scalar_abi", 1, 1, 1, 1, 1, 1, &args, 0) &&
       ((int32_t *)out.host)[0] == -100 + 250 - 30000 + 60000;
  if (!ok) {
    printf("  narrow_scalar_abi = %d\n", ((int32_t *)out.host)[0]);
  }
  report("narrow_scalar_abi", ok);
}

static void contract_records(void) {
  int count = 50;
  Buf out = make((size_t)count * 4), src = make((size_t)count * 4);
  Args args = {{0}, 0, 0};
  struct {
    int32_t count;
    float alpha;
    float beta;
  } params = {50, 1.5f, -2.25f};
  int ok;
  for (int i = 0; i < count; i++) ((float *)src.host)[i] = (float)i * 0.75f;
  args_ptr(&args, &out);
  args_ptr(&args, &src);
  args_put(&args, &params, sizeof(params), 4);
  ok = run("record_pipeline", 2, 1, 1, 32, 1, 1, &args, 0);
  for (int i = 0; i < count && ok; i++) {
    float lo = ((float *)src.host)[i] * params.alpha;
    float hi = (float)i + params.alpha;
    int pick = i % 4;
    float table_lo = lo + (float)pick;
    float span = hi - table_lo;
    float total = 100.0f + span;
    total = total + params.beta;
    if (((float *)out.host)[i] != total) {
      printf("  record_pipeline out[%d] = %.9g, want %.9g\n", i,
             ((float *)out.host)[i], total);
      ok = 0;
    }
  }
  report("record_pipeline", ok);
}

static void contract_narrow_wrap(void) {
  Buf out = make(4 * 16 * 4);
  Args args = {{0}, 0, 0};
  int ok;
  args_ptr(&args, &out);
  ok = run("narrow_wrap", 1, 1, 1, 4, 1, 1, &args, 0);
  for (int t = 0; t < 4 && ok; t++) {
    int32_t want[16];
    int8_t a8 = (int8_t)(uint8_t)(t * 37);
    int8_t b8 = (int8_t)(uint8_t)(100 + t);
    uint8_t u8 = (uint8_t)(t * 91);
    int16_t a16 = (int16_t)(uint16_t)(t * 2311);
    int32_t big = 2147483600 + t * 13;
    int32_t neg = -7 - t;
    int32_t lowest = INT32_MIN;
    int64_t w64 = (int64_t)((uint64_t)9223372036854775000ULL + (uint64_t)(t * 1000));
    want[0] = (int8_t)(uint8_t)((int)a8 + (int)b8);
    want[1] = (uint8_t)(u8 * 3);
    want[2] = (int16_t)(uint16_t)((int)a16 * 19);
    want[3] = (int32_t)((uint32_t)big + 100u);
    want[4] = neg / 3;
    want[5] = neg % 3;
    want[6] = lowest;
    want[7] = 0;
    want[8] = (int32_t)(1u << ((t + 30) & 31));
    want[9] = (int32_t)(0x80000000u >> ((t + 31) & 31));
    want[10] = lowest >> ((t + 1) & 31);
    want[11] = (int32_t)(w64 >> 32);
    want[12] = (int32_t)(uint32_t)(uint64_t)w64;
    want[13] = (int32_t)((uint32_t)t * 4000000000u);
    want[14] = (int32_t)((int64_t)t * -3);
    want[15] = (int32_t)(uint8_t)(t * 300);
    for (int k = 0; k < 16; k++) {
      if (((int32_t *)out.host)[t * 16 + k] != want[k]) {
        printf("  narrow_wrap thread %d slot %d = %d, want %d\n", t, k,
               ((int32_t *)out.host)[t * 16 + k], want[k]);
        ok = 0;
        break;
      }
    }
  }
  report("narrow_wrap", ok);
}

static void contract_float_rules(void) {
  float inputs[8] = {0.099999994f, 0.1f, 0.100000009f, 1.0f,
                     2.0f,         -1.0f, 1e-30f,      3e38f};
  Buf x = make(8 * 4), out = make(8 * 16), flags = make(8 * 4);
  Args args = {{0}, 0, 0};
  int ok;
  memcpy(x.host, inputs, sizeof(inputs));
  args_ptr(&args, &x);
  args_ptr(&args, &out);
  args_ptr(&args, &flags);
  ok = run("float_rules", 1, 1, 1, 8, 1, 1, &args, 0);
  for (int t = 0; t < 8 && ok; t++) {
    double v = inputs[t];
    int flag = 0;
    float want[4];
    want[0] = metal_f32((float)(v * 2.0));
    want[1] = metal_f32((float)(1.0 / v));
    want[2] = metal_f32((float)(-3.000001e38));
    want[3] = metal_f32(inputs[t] + 0.1f);
    if (inputs[t] > 0.1f) flag += 1;
    if (inputs[t] >= 0.1f) flag += 2;
    if (inputs[t] < 0.1f) flag += 4;
    if (inputs[t] <= 0.1f) flag += 8;
    if (inputs[t] == 0.1f) flag += 16;
    if (inputs[t] != 0.1f) flag += 32;
    if (v > 1.0) flag += 64;
    for (int k = 0; k < 4; k++) {
      if (f32_bits(((float *)out.host)[t * 4 + k]) != f32_bits(want[k])) {
        printf("  float_rules thread %d slot %d = %.9g, want %.9g\n", t, k,
               ((float *)out.host)[t * 4 + k], want[k]);
        ok = 0;
      }
    }
    if (((int32_t *)flags.host)[t] != flag) {
      printf("  float_rules thread %d flags = %d, want %d\n", t,
             ((int32_t *)flags.host)[t], flag);
      ok = 0;
    }
  }
  report("float_rules", ok);
}

static int half_same(uint16_t a, uint16_t b) {
  int a_nan = (a & 0x7c00u) == 0x7c00u && (a & 0x3ffu) != 0;
  int b_nan = (b & 0x7c00u) == 0x7c00u && (b & 0x3ffu) != 0;
  return (a_nan && b_nan) || a == b;
}

static int float_same(float a, float b) {
  return (a != a && b != b) || f32_bits(a) == f32_bits(b);
}

static uint32_t prmt(uint32_t a, uint32_t b, uint32_t s) {
  uint32_t r = 0;
  for (int i = 0; i < 4; i++) {
    uint32_t sel = (s >> (4 * i)) & 0xfu;
    uint32_t index = sel & 7u;
    uint32_t source = index < 4 ? a : b;
    uint32_t v = (source >> (8 * (index & 3u))) & 0xffu;
    if (sel & 8u) v = (v & 0x80u) ? 0xffu : 0u;
    r |= v << (8 * i);
  }
  return r;
}

static void contract_bit_ops(void) {
  int threads = 16;
  Buf a = make((size_t)threads * 4), b = make((size_t)threads * 4),
      out = make((size_t)threads * 48), fout = make((size_t)threads * 12);
  Args args = {{0}, 0, 0};
  int ok;
  for (int i = 0; i < threads; i++) {
    ((uint32_t *)a.host)[i] = (uint32_t)half_from_float(rng_float(-3, 3)) |
                              ((uint32_t)half_from_float(rng_float(-3, 3)) << 16);
    ((uint32_t *)b.host)[i] = (uint32_t)half_from_float(rng_float(-3, 3)) |
                              ((uint32_t)half_from_float(rng_float(-3, 3)) << 16);
  }
  ((uint32_t *)a.host)[0] = 0x7fc00001u;
  ((uint32_t *)a.host)[1] = 0x3f800000u;
  ((uint32_t *)b.host)[2] = 0x477ff000u;
  args_ptr(&args, &a);
  args_ptr(&args, &b);
  args_ptr(&args, &out);
  args_ptr(&args, &fout);
  ok = run("bit_ops", 1, 1, 1, (uint32_t)threads, 1, 1, &args, 0);
  for (int t = 0; t < threads && ok; t++) {
    uint32_t x = ((uint32_t *)a.host)[t];
    uint32_t y = ((uint32_t *)b.host)[t];
    uint32_t want[12];
    uint32_t acc = (uint32_t)t;
    int32_t sacc = t - 5;
    for (int k = 0; k < 4; k++) {
      acc += ((x >> (8 * k)) & 0xffu) * ((y >> (8 * k)) & 0xffu);
      sacc = (int32_t)((uint32_t)sacc +
                       (uint32_t)((int8_t)(uint8_t)(x >> (8 * k)) *
                                  (int8_t)(uint8_t)(y >> (8 * k))));
    }
    want[0] = acc;
    want[1] = (uint32_t)sacc;
    want[2] = 7u + (x & 0xffffu) * (y & 0xffu) + (x >> 16) * ((y >> 8) & 0xffu);
    want[3] = 7u + (x & 0xffffu) * ((y >> 16) & 0xffu) + (x >> 16) * (y >> 24);
    want[4] = (uint32_t)(-3 + (int16_t)(uint16_t)(x & 0xffffu) * (int8_t)(uint8_t)y +
                         (int16_t)(uint16_t)(x >> 16) * (int8_t)(uint8_t)(y >> 8));
    want[5] = (uint32_t)(-3 + (int16_t)(uint16_t)(x & 0xffffu) * (int8_t)(uint8_t)(y >> 16) +
                         (int16_t)(uint16_t)(x >> 16) * (int8_t)(uint8_t)(y >> 24));
    want[6] = prmt(x, y, (uint32_t)(0x7531 + t * 0x1111) & 0xffffu);
    want[7] = (x + 32767u + ((x >> 16) & 1u)) >> 16;
    want[8] = half_from_float(f32_from(y));
    want[9] = (uint32_t)half_add((uint16_t)x, (uint16_t)y) |
              ((uint32_t)half_add((uint16_t)(x >> 16), (uint16_t)(y >> 16)) << 16);
    want[10] = (uint32_t)half_mul((uint16_t)x, (uint16_t)y) |
               ((uint32_t)half_mul((uint16_t)(x >> 16), (uint16_t)(y >> 16)) << 16);
    want[11] = (uint32_t)half_fma((uint16_t)x, (uint16_t)y, (uint16_t)(x ^ y)) |
               ((uint32_t)half_fma((uint16_t)(x >> 16), (uint16_t)(y >> 16),
                                   (uint16_t)((x ^ y) >> 16)) << 16);
    for (int k = 0; k < 12; k++) {
      uint32_t got_word = ((uint32_t *)out.host)[t * 12 + k];
      int same = k >= 9 ? half_same((uint16_t)got_word, (uint16_t)want[k]) &&
                              half_same((uint16_t)(got_word >> 16),
                                        (uint16_t)(want[k] >> 16))
                        : got_word == want[k];
      if (!same) {
        printf("  bit_ops thread %d slot %d = 0x%08x, want 0x%08x\n", t, k,
               ((uint32_t *)out.host)[t * 12 + k], want[k]);
        ok = 0;
      }
    }
    {
      float f0 = f32_from((x & 0xffffu) << 16);
      float f1 = half_to_float((uint16_t)(y & 0xffffu));
      float f2 = half_to_float((uint16_t)(x >> 16)) + half_to_float((uint16_t)y);
      float *got = (float *)fout.host + t * 3;
      if (!float_same(got[0], f0) || !float_same(got[1], f1) ||
          !float_same(got[2], f2)) {
        printf("  bit_ops thread %d floats = %g %g %g, want %g %g %g\n", t,
               got[0], got[1], got[2], f0, f1, f2);
        ok = 0;
      }
    }
  }
  report("bit_ops", ok);
}

static void contract_control_flow(void) {
  int threads = 40;
  Buf out = make((size_t)threads * 4), limits = make((size_t)threads * 4);
  Args args = {{0}, 0, 0};
  int ok;
  for (int i = 0; i < threads; i++) ((int32_t *)limits.host)[i] = (i * 7) % 50;
  args_ptr(&args, &out);
  args_ptr(&args, &limits);
  ok = run("control_flow", 1, 1, 1, (uint32_t)threads, 1, 1, &args, 0);
  for (int t = 0; t < threads && ok; t++) {
    int limit = ((int32_t *)limits.host)[t];
    int acc = 0, i = 0, done = 0, want = 0;
    while (i < 10 && !done) {
      i = i + 1;
      if (i % 3 == t % 3) continue;
      for (int j = 0; j < i; j++) {
        if (j * i > limit) break;
        if (j == 7 && t % 5 == 0) {
          want = acc * 1000 + i * 10 + j;
          done = 1;
          break;
        }
        acc = acc + j;
      }
      if (done) break;
      if (acc > limit * 4) break;
    }
    if (!done) want = acc * 1000 + i;
    if (((int32_t *)out.host)[t] != want) {
      printf("  control_flow[%d] = %d, want %d\n", t, ((int32_t *)out.host)[t],
             want);
      ok = 0;
    }
  }
  report("control_flow", ok);
}

static void contract_atomics(void) {
  int groups = 4, threads = 64, total = groups * threads;
  Buf state = make(64 * 4), tickets = make((size_t)total * 4),
      cas = make((size_t)total * 4), shared_out = make((size_t)groups * 4);
  Args args = {{0}, 0, 0};
  uint32_t *s = (uint32_t *)state.host;
  int ok;
  unsigned char *seen = (unsigned char *)calloc((size_t)total, 1);
  s[0] = 0;
  s[1] = 1000;
  s[2] = 0xffffffffu;
  s[3] = 0;
  s[4] = 0xffffffffu;
  s[5] = 0;
  s[6] = 0;
  s[7] = 0xdeadbeefu;
  s[8] = 0;
  args_ptr(&args, &state);
  args_ptr(&args, &tickets);
  args_ptr(&args, &cas);
  args_ptr(&args, &shared_out);
  ok = run("atomic_litmus32", (uint32_t)groups, 1, 1, (uint32_t)threads, 1, 1,
           &args, 0);
  if (ok) {
    uint32_t winner = s[8] - 1u;
    int zeros = 0;
    if (s[0] != (uint32_t)total || s[1] != 1000u - (uint32_t)total ||
        s[2] != 0 || s[3] != (uint32_t)total - 1 || s[4] != 0 ||
        s[5] != 0xffffffffu || s[6] != 0 || s[7] >= (uint32_t)total ||
        winner >= (uint32_t)total) {
      printf("  atomics final state %u %u %u %u %u %u %u %u %u\n", s[0], s[1],
             s[2], s[3], s[4], s[5], s[6], s[7], s[8]);
      ok = 0;
    }
    for (int i = 0; i < total && ok; i++) {
      uint32_t ticket = ((uint32_t *)tickets.host)[i];
      uint32_t old = ((uint32_t *)cas.host)[i];
      if (ticket >= (uint32_t)total || seen[ticket]) {
        printf("  atomics ticket %u repeats or escapes\n", ticket);
        ok = 0;
        break;
      }
      seen[ticket] = 1;
      if (old == 0) {
        zeros++;
        if ((uint32_t)i != winner) {
          printf("  atomics CAS winner %d does not match state %u\n", i, winner);
          ok = 0;
        }
      } else if (old != winner + 1u) {
        printf("  atomics CAS loser %d saw %u\n", i, old);
        ok = 0;
      }
    }
    if (zeros != 1) {
      printf("  atomics CAS had %d winners\n", zeros);
      ok = 0;
    }
    for (int g = 0; g < groups && ok; g++) {
      if (((uint32_t *)shared_out.host)[g] != (uint32_t)threads ||
          s[16 + g * 3] != 1u || s[16 + g * 3 + 2] != 0x5a00u + (uint32_t)g) {
        printf("  atomics group %d: shared %u flag %u payload 0x%x\n", g,
               ((uint32_t *)shared_out.host)[g], s[16 + g * 3],
               s[16 + g * 3 + 2]);
        ok = 0;
      }
    }
  }
  free(seen);
  report("atomic_litmus32", ok);
}

typedef struct {
  const char *name;
  void (*run)(void);
} Contract;

static const Contract CONTRACTS[] = {
    {"index_3d", contract_index_3d},
    {"saxpy_odd", contract_saxpy},
    {"branch_return_join", contract_branch_return_join},
    {"row_norm", contract_row_norm},
    {"staged_copy", contract_staged_copy},
    {"async_stage_u32x4", contract_async_stage},
    {"auto_stage_u32", contract_auto_stage},
    {"dynamic_staged_copy", contract_dynamic_staged},
    {"subgroup_contract", contract_subgroup_contract},
    {"subgroup_extended", contract_subgroup_extended},
    {"subgroup_exchange_vote", contract_exchange_vote},
    {"softmax_rows_f32", contract_softmax},
    {"layer_norm_rows_f32", contract_layer_norm},
    {"tensor_f16_f32", contract_tensor_f16},
    {"tensor_chain4", contract_tensor_chain},
    {"gemm_full_tiles_f16_f32", contract_gemm_full_tiles},
    {"tensor_pipeline_f16_f32", contract_tensor_pipeline},
    {"q4_scaled", contract_q4_scaled},
    {"narrow_scalar_abi", contract_narrow_abi},
    {"record_pipeline", contract_records},
    {"narrow_wrap", contract_narrow_wrap},
    {"float_rules", contract_float_rules},
    {"bit_ops", contract_bit_ops},
    {"control_flow", contract_control_flow},
    {"atomic_litmus32", contract_atomics},
};

static char *read_file(const char *path, size_t *length) {
  FILE *file = fopen(path, "rb");
  char *data;
  long size;
  if (!file) {
    return NULL;
  }
  fseek(file, 0, SEEK_END);
  size = ftell(file);
  fseek(file, 0, SEEK_SET);
  data = (char *)malloc((size_t)size + 1);
  if (data && fread(data, 1, (size_t)size, file) != (size_t)size) {
    free(data);
    data = NULL;
  }
  fclose(file);
  if (data) {
    data[size] = '\0';
    *length = (size_t)size;
  }
  return data;
}

int main(int argc, char **argv) {
  Backend backend;
  const char *path = NULL;
  int spurious = 0;
  size_t length = 0;
  char *source;
  char error[4096];
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--spurious-cas")) {
      spurious = 1;
    } else if (!strcmp(argv[i], "--only") && i + 1 < argc) {
      g_only = argv[++i];
    } else {
      path = argv[i];
    }
  }
  if (!path) {
    fprintf(stderr,
            "usage: metal_harness [--spurious-cas] [--only NAME] FILE.metal\n");
    return 2;
  }
  source = read_file(path, &length);
  if (!source) {
    fprintf(stderr, "metal_harness: cannot read %s\n", path);
    return 2;
  }
  memset(&backend, 0, sizeof(backend));
#ifdef METAL_HARNESS_NATIVE
  (void)spurious;
  if (!metal_backend(&backend)) {
    return 2;
  }
#else
  if (!interp_backend(&backend, spurious)) {
    return 2;
  }
#endif
  g_backend = &backend;
  error[0] = '\0';
  if (!backend.load(&backend, source, length, error, sizeof(error))) {
    fprintf(stderr, "metal_harness: %s\n", error);
    return 1;
  }
  for (size_t i = 0; i < sizeof(CONTRACTS) / sizeof(CONTRACTS[0]); i++) {
    if (wants(CONTRACTS[i].name)) {
      CONTRACTS[i].run();
    }
  }
  printf("metal_harness: %d/%d contracts passed on %s\n", g_passes,
         g_passes + g_failures, backend.name);
  backend.destroy(&backend);
  free(source);
  return g_failures == 0 ? 0 : 1;
}
