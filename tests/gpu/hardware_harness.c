/*
 * CUDA Driver differential harness for Mettle-emitted PTX.
 *
 * This intentionally does not include CUDA headers or link to a CUDA import
 * library. It loads the stable Driver API dynamically so the identical source
 * compiles on Windows/x86-64 development machines and Linux/AArch64 DGX Spark.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef HMODULE DriverLibrary;
#else
#include <dlfcn.h>
typedef void *DriverLibrary;
#endif

typedef int CUresult;
typedef int CUdevice;
typedef void *CUcontext;
typedef void *CUmodule;
typedef void *CUfunction;
typedef uint64_t CUdeviceptr;

/* CUDA tensor maps are opaque 128-byte values with a 64-byte host alignment
 * requirement.  Keep the harness independent of CUDA headers while matching
 * the stable Driver API ABI exactly on x86-64 and AArch64. */
typedef struct {
  _Alignas(64) uint64_t opaque[16];
} CUtensorMap;

_Static_assert(sizeof(CUtensorMap) == 128, "CUtensorMap ABI size");
_Static_assert(_Alignof(CUtensorMap) == 64, "CUtensorMap ABI alignment");

enum {
  CUDA_SUCCESS = 0,
  CU_DEVICE_ATTRIBUTE_INTEGRATED = 18,
  CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR = 75,
  CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR = 76,
  CU_TENSOR_MAP_DATA_TYPE_FLOAT32 = 7,
  CU_TENSOR_MAP_INTERLEAVE_NONE = 0,
  CU_TENSOR_MAP_SWIZZLE_NONE = 0,
  CU_TENSOR_MAP_L2_PROMOTION_NONE = 0,
  CU_TENSOR_MAP_FLOAT_OOB_FILL_NONE = 0
};

typedef struct {
  DriverLibrary library;
  CUresult (*cuInit)(unsigned int);
  CUresult (*cuDeviceGet)(CUdevice *, int);
  CUresult (*cuDeviceGetName)(char *, int, CUdevice);
  CUresult (*cuDeviceGetAttribute)(int *, int, CUdevice);
  CUresult (*cuCtxCreate_v2)(CUcontext *, unsigned int, CUdevice);
  CUresult (*cuCtxDestroy_v2)(CUcontext);
  CUresult (*cuModuleLoadData)(CUmodule *, const void *);
  CUresult (*cuModuleUnload)(CUmodule);
  CUresult (*cuModuleGetFunction)(CUfunction *, CUmodule, const char *);
  CUresult (*cuMemAlloc_v2)(CUdeviceptr *, size_t);
  CUresult (*cuMemFree_v2)(CUdeviceptr);
  CUresult (*cuMemcpyHtoD_v2)(CUdeviceptr, const void *, size_t);
  CUresult (*cuMemcpyDtoH_v2)(void *, CUdeviceptr, size_t);
  CUresult (*cuTensorMapEncodeTiled)(
      CUtensorMap *, int, uint32_t, void *, const uint64_t *,
      const uint64_t *, const uint32_t *, const uint32_t *, int, int, int,
      int);
  CUresult (*cuLaunchKernel)(CUfunction, unsigned int, unsigned int,
                             unsigned int, unsigned int, unsigned int,
                             unsigned int, unsigned int, void *, void **,
                             void **);
  CUresult (*cuCtxSynchronize)(void);
  CUresult (*cuGetErrorName)(CUresult, const char **);
  CUresult (*cuGetErrorString)(CUresult, const char **);
} DriverApi;

typedef struct {
  DriverApi api;
  CUcontext context;
  CUmodule module;
} Harness;

static void close_driver_library(DriverLibrary library) {
  if (!library) return;
#ifdef _WIN32
  FreeLibrary(library);
#else
  dlclose(library);
#endif
}

static int load_symbol(DriverLibrary library, const char *name, void *out,
                       size_t out_size, int required) {
#ifdef _WIN32
  FARPROC symbol = GetProcAddress(library, name);
  if (!symbol) {
    if (required) fprintf(stderr, "missing CUDA Driver symbol %s\n", name);
    return required ? 0 : 1;
  }
  if (sizeof(symbol) != out_size) {
    fprintf(stderr, "unexpected function-pointer size for %s\n", name);
    return 0;
  }
  memcpy(out, &symbol, out_size);
#else
  void *symbol = dlsym(library, name);
  if (!symbol) {
    if (required) fprintf(stderr, "missing CUDA Driver symbol %s: %s\n", name,
                          dlerror());
    return required ? 0 : 1;
  }
  if (sizeof(symbol) != out_size) {
    fprintf(stderr, "unexpected function-pointer size for %s\n", name);
    return 0;
  }
  memcpy(out, &symbol, out_size);
#endif
  return 1;
}

#define LOAD_REQUIRED(api, name)                                                \
  load_symbol((api)->library, #name, &(api)->name, sizeof((api)->name), 1)
#define LOAD_OPTIONAL(api, name)                                                \
  load_symbol((api)->library, #name, &(api)->name, sizeof((api)->name), 0)

static int load_driver(DriverApi *api) {
  memset(api, 0, sizeof(*api));
#ifdef _WIN32
  api->library = LoadLibraryA("nvcuda.dll");
#else
  api->library = dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL);
#endif
  if (!api->library) {
    fprintf(stderr, "CUDA Driver library is not available\n");
    return 0;
  }
  if (!LOAD_REQUIRED(api, cuInit) || !LOAD_REQUIRED(api, cuDeviceGet) ||
      !LOAD_REQUIRED(api, cuDeviceGetName) ||
      !LOAD_REQUIRED(api, cuDeviceGetAttribute) ||
      !LOAD_REQUIRED(api, cuCtxCreate_v2) ||
      !LOAD_REQUIRED(api, cuCtxDestroy_v2) ||
      !LOAD_REQUIRED(api, cuModuleLoadData) ||
      !LOAD_REQUIRED(api, cuModuleUnload) ||
      !LOAD_REQUIRED(api, cuModuleGetFunction) ||
      !LOAD_REQUIRED(api, cuMemAlloc_v2) ||
      !LOAD_REQUIRED(api, cuMemFree_v2) ||
      !LOAD_REQUIRED(api, cuMemcpyHtoD_v2) ||
      !LOAD_REQUIRED(api, cuMemcpyDtoH_v2) ||
      !LOAD_REQUIRED(api, cuLaunchKernel) ||
      !LOAD_REQUIRED(api, cuCtxSynchronize)) {
    close_driver_library(api->library);
    memset(api, 0, sizeof(*api));
    return 0;
  }
  (void)LOAD_OPTIONAL(api, cuGetErrorName);
  (void)LOAD_OPTIONAL(api, cuGetErrorString);
  (void)LOAD_OPTIONAL(api, cuTensorMapEncodeTiled);
  return 1;
}

static void report_cuda_error(Harness *h, const char *operation, CUresult rc) {
  const char *name = NULL;
  const char *description = NULL;
  if (h->api.cuGetErrorName)
    (void)h->api.cuGetErrorName(rc, &name);
  if (h->api.cuGetErrorString)
    (void)h->api.cuGetErrorString(rc, &description);
  fprintf(stderr, "%s failed: CUDA %d%s%s%s%s\n", operation, rc,
          name ? " (" : "", name ? name : "", name ? ")" : "",
          description ? description : "");
}

static char *read_file(const char *path) {
  FILE *file = fopen(path, "rb");
  long length;
  char *data;
  if (!file) {
    fprintf(stderr, "cannot open PTX module %s\n", path);
    return NULL;
  }
  if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 ||
      fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    fprintf(stderr, "cannot size PTX module %s\n", path);
    return NULL;
  }
  data = (char *)malloc((size_t)length + 1u);
  if (!data || fread(data, 1, (size_t)length, file) != (size_t)length) {
    free(data);
    fclose(file);
    fprintf(stderr, "cannot read PTX module %s\n", path);
    return NULL;
  }
  data[length] = '\0';
  fclose(file);
  return data;
}

static int alloc_device(Harness *h, CUdeviceptr *ptr, const void *source,
                        size_t bytes) {
  CUresult rc = h->api.cuMemAlloc_v2(ptr, bytes);
  if (rc != CUDA_SUCCESS) {
    report_cuda_error(h, "cuMemAlloc_v2", rc);
    return 0;
  }
  if (source) {
    rc = h->api.cuMemcpyHtoD_v2(*ptr, source, bytes);
    if (rc != CUDA_SUCCESS) {
      report_cuda_error(h, "cuMemcpyHtoD_v2", rc);
      h->api.cuMemFree_v2(*ptr);
      *ptr = 0;
      return 0;
    }
  }
  return 1;
}

static void free_device(Harness *h, CUdeviceptr *ptr) {
  if (*ptr) {
    (void)h->api.cuMemFree_v2(*ptr);
    *ptr = 0;
  }
}

static int copy_from_device(Harness *h, void *destination, CUdeviceptr source,
                            size_t bytes) {
  CUresult rc = h->api.cuMemcpyDtoH_v2(destination, source, bytes);
  if (rc != CUDA_SUCCESS) {
    report_cuda_error(h, "cuMemcpyDtoH_v2", rc);
    return 0;
  }
  return 1;
}

static int copy_to_device(Harness *h, CUdeviceptr destination,
                          const void *source, size_t bytes) {
  CUresult rc = h->api.cuMemcpyHtoD_v2(destination, source, bytes);
  if (rc != CUDA_SUCCESS) {
    report_cuda_error(h, "cuMemcpyHtoD_v2", rc);
    return 0;
  }
  return 1;
}

static int launch_with_shared(Harness *h, const char *name, unsigned int gx,
                              unsigned int gy, unsigned int gz,
                              unsigned int bx, unsigned int by,
                              unsigned int bz, unsigned int shared_bytes,
                              void **parameters) {
  CUfunction function = NULL;
  CUresult rc = h->api.cuModuleGetFunction(&function, h->module, name);
  if (rc != CUDA_SUCCESS) {
    report_cuda_error(h, name, rc);
    return 0;
  }
  rc = h->api.cuLaunchKernel(function, gx, gy, gz, bx, by, bz, shared_bytes,
                             NULL, parameters, NULL);
  if (rc != CUDA_SUCCESS) {
    report_cuda_error(h, name, rc);
    return 0;
  }
  rc = h->api.cuCtxSynchronize();
  if (rc != CUDA_SUCCESS) {
    report_cuda_error(h, "cuCtxSynchronize", rc);
    return 0;
  }
  return 1;
}

static int launch(Harness *h, const char *name, unsigned int gx,
                  unsigned int gy, unsigned int gz, unsigned int bx,
                  unsigned int by, unsigned int bz, void **parameters) {
  return launch_with_shared(h, name, gx, gy, gz, bx, by, bz, 0, parameters);
}

static int test_index_3d(Harness *h) {
  enum { COUNT = 2 * 2 * 2 * 4 * 2 * 2, FIELDS = 12 };
  int32_t host[COUNT * FIELDS];
  CUdeviceptr device = 0;
  void *parameters[] = {&device};
  int ok = 0;
  memset(host, 0xcc, sizeof(host));
  if (!alloc_device(h, &device, NULL, sizeof(host)) ||
      !launch(h, "index_3d", 2, 2, 2, 4, 2, 2, parameters) ||
      !copy_from_device(h, host, device, sizeof(host)))
    goto cleanup;
  for (int index = 0; index < COUNT; index++) {
    int block_linear = index / 16;
    int local_linear = index % 16;
    int expected[FIELDS] = {
        local_linear % 4,       (local_linear / 4) % 2,
        local_linear / 8,       block_linear % 2,
        (block_linear / 2) % 2, block_linear / 4,
        4,                      2,
        2,                      2,
        2,                      2};
    for (int field = 0; field < FIELDS; field++) {
      if (host[index * FIELDS + field] != expected[field]) {
        fprintf(stderr,
                "index_3d mismatch item=%d field=%d: got %d expected %d\n",
                index, field, host[index * FIELDS + field], expected[field]);
        goto cleanup;
      }
    }
  }
  ok = 1;
cleanup:
  free_device(h, &device);
  printf("[%s] 3-D index contract\n", ok ? "PASS" : "FAIL");
  return ok;
}

static int test_saxpy_odd(Harness *h) {
  enum { N = 1003 };
  float *x = (float *)malloc(sizeof(float) * N);
  float *y = (float *)malloc(sizeof(float) * N);
  float *expected = (float *)malloc(sizeof(float) * N);
  CUdeviceptr dx = 0, dy = 0;
  float alpha = 1.5f;
  int32_t n = N;
  void *parameters[] = {&alpha, &dx, &dy, &n};
  int ok = 0;
  if (!x || !y || !expected) goto cleanup;
  for (int i = 0; i < N; i++) {
    x[i] = (float)((i % 31) - 15);
    y[i] = (float)((i % 13) - 6);
    expected[i] = alpha * x[i] + y[i];
  }
  if (!alloc_device(h, &dx, x, sizeof(float) * N) ||
      !alloc_device(h, &dy, y, sizeof(float) * N) ||
      !launch(h, "saxpy_odd", (N + 127) / 128, 1, 1, 128, 1, 1,
              parameters) ||
      !copy_from_device(h, y, dy, sizeof(float) * N))
    goto cleanup;
  for (int i = 0; i < N; i++) {
    if (y[i] != expected[i]) {
      fprintf(stderr, "saxpy_odd mismatch i=%d: got %.9g expected %.9g\n", i,
              y[i], expected[i]);
      goto cleanup;
    }
  }
  ok = 1;
cleanup:
  free_device(h, &dx);
  free_device(h, &dy);
  free(x);
  free(y);
  free(expected);
  printf("[%s] odd-size SAXPY and device helper\n", ok ? "PASS" : "FAIL");
  return ok;
}

static int test_row_norm(Harness *h) {
  enum { ROWS = 37, COLS = 19 };
  float *input = (float *)malloc(sizeof(float) * ROWS * COLS);
  float *output = (float *)malloc(sizeof(float) * ROWS);
  float expected[ROWS];
  CUdeviceptr dinput = 0, doutput = 0;
  int32_t rows = ROWS, cols = COLS;
  void *parameters[] = {&dinput, &doutput, &rows, &cols};
  int ok = 0;
  if (!input || !output) goto cleanup;
  for (int row = 0; row < ROWS; row++) {
    float sum = 0.0f;
    for (int col = 0; col < COLS; col++) {
      float value = (float)(((row * 3 + col * 5) % 7) - 3);
      input[row * COLS + col] = value;
      sum += value * value;
    }
    expected[row] = sqrtf(sum);
  }
  if (!alloc_device(h, &dinput, input, sizeof(float) * ROWS * COLS) ||
      !alloc_device(h, &doutput, NULL, sizeof(float) * ROWS) ||
      !launch(h, "row_norm", 2, 1, 1, 32, 1, 1, parameters) ||
      !copy_from_device(h, output, doutput, sizeof(float) * ROWS))
    goto cleanup;
  for (int row = 0; row < ROWS; row++) {
    float delta = fabsf(output[row] - expected[row]);
    if (delta > 1.0e-5f * fmaxf(1.0f, expected[row])) {
      fprintf(stderr, "row_norm mismatch row=%d: got %.9g expected %.9g\n",
              row, output[row], expected[row]);
      goto cleanup;
    }
  }
  ok = 1;
cleanup:
  free_device(h, &dinput);
  free_device(h, &doutput);
  free(input);
  free(output);
  printf("[%s] counted-loop row norm and math intrinsic\n",
         ok ? "PASS" : "FAIL");
  return ok;
}

static int test_staged_copy(Harness *h) {
  enum { N = 173 };
  float input[N], output[N];
  CUdeviceptr dinput = 0, doutput = 0;
  int32_t n = N;
  void *parameters[] = {&dinput, &doutput, &n};
  int ok = 0;
  for (int i = 0; i < N; i++) {
    input[i] = (float)(i - 91) * 0.25f;
    output[i] = 0.0f;
  }
  if (!alloc_device(h, &dinput, input, sizeof(input)) ||
      !alloc_device(h, &doutput, NULL, sizeof(output)) ||
      !launch(h, "staged_copy", (N + 63) / 64, 1, 1, 64, 1, 1,
              parameters) ||
      !copy_from_device(h, output, doutput, sizeof(output)))
    goto cleanup;
  for (int i = 0; i < N; i++) {
    if (output[i] != input[i]) {
      fprintf(stderr, "staged_copy mismatch i=%d: got %.9g expected %.9g\n",
              i, output[i], input[i]);
      goto cleanup;
    }
  }
  ok = 1;
cleanup:
  free_device(h, &dinput);
  free_device(h, &doutput);
  printf("[%s] workgroup/private storage and barrier\n",
         ok ? "PASS" : "FAIL");
  return ok;
}

static int test_async_stage_u32x4(Harness *h) {
  enum { N = 128 };
  uint32_t input[N], output[N];
  CUdeviceptr dinput = 0, doutput = 0;
  void *parameters[] = {&dinput, &doutput};
  int ok = 0;
  for (int i = 0; i < N; i++) {
    input[i] = UINT32_C(0x9e370000) ^ (uint32_t)(i * 0x101u + 17u);
    output[i] = UINT32_C(0xdeadbeef);
  }
  if (!alloc_device(h, &dinput, input, sizeof(input)) ||
      !alloc_device(h, &doutput, output, sizeof(output)) ||
      !launch(h, "async_stage_u32x4", 1, 1, 1, 32, 1, 1, parameters) ||
      !copy_from_device(h, output, doutput, sizeof(output)))
    goto cleanup;
  for (int i = 0; i < N; i++) {
    uint32_t expected = input[(i + 124) % N];
    if (output[i] != expected) {
      fprintf(stderr,
              "async_stage_u32x4 mismatch i=%d: got 0x%08x expected 0x%08x\n",
              i, output[i], expected);
      goto cleanup;
    }
  }
  ok = 1;
cleanup:
  free_device(h, &dinput);
  free_device(h, &doutput);
  printf("[%s] native 16-byte async workgroup staging\n",
         ok ? "PASS" : "FAIL");
  return ok;
}

static int test_auto_stage_u32(Harness *h) {
  enum { N = 32 };
  uint32_t input[N], output[N];
  CUdeviceptr dinput = 0, doutput = 0;
  void *parameters[] = {&dinput, &doutput};
  int ok = 0;
  for (int i = 0; i < N; i++) {
    input[i] = UINT32_C(0xa5000000) ^ (uint32_t)(i * 313u + 29u);
    output[i] = UINT32_C(0xdeadbeef);
  }
  if (!alloc_device(h, &dinput, input, sizeof(input)) ||
      !alloc_device(h, &doutput, output, sizeof(output)) ||
      !launch(h, "auto_stage_u32", 1, 1, 1, N, 1, 1, parameters) ||
      !copy_from_device(h, output, doutput, sizeof(output)))
    goto cleanup;
  for (int i = 0; i < N; i++) {
    uint32_t mix = (uint32_t)i * UINT32_C(1664525) +
                   UINT32_C(1013904223);
    uint32_t expected = input[(i + N - 1) % N] ^ mix;
    if (output[i] != expected) {
      fprintf(stderr,
              "auto_stage_u32 mismatch i=%d: got 0x%08x expected 0x%08x\n",
              i, output[i], expected);
      goto cleanup;
    }
  }
  ok = 1;
cleanup:
  free_device(h, &dinput);
  free_device(h, &doutput);
  printf("[%s] optimizer-generated asynchronous staging and overlap\n",
         ok ? "PASS" : "FAIL");
  return ok;
}

static int test_dynamic_staged_copy(Harness *h) {
  enum { N = 173, BLOCK = 64, ARENA_BYTES = 2 * BLOCK * (int)sizeof(float) };
  float input[N], output[N];
  CUdeviceptr dinput = 0, doutput = 0;
  int32_t n = N;
  void *parameters[] = {&dinput, &doutput, &n};
  int ok = 0;
  for (int i = 0; i < N; i++) {
    input[i] = (float)(i * 7 - 313) * 0.125f;
    output[i] = 0.0f;
  }
  if (!alloc_device(h, &dinput, input, sizeof(input)) ||
      !alloc_device(h, &doutput, NULL, sizeof(output)) ||
      !launch_with_shared(h, "dynamic_staged_copy", (N + BLOCK - 1) / BLOCK,
                          1, 1, BLOCK, 1, 1, ARENA_BYTES, parameters) ||
      !copy_from_device(h, output, doutput, sizeof(output)))
    goto cleanup;
  for (int i = 0; i < N; i++) {
    if (output[i] != input[i]) {
      fprintf(stderr,
              "dynamic_staged_copy mismatch i=%d: got %.9g expected %.9g\n",
              i, output[i], input[i]);
      goto cleanup;
    }
  }
  ok = 1;
cleanup:
  free_device(h, &dinput);
  free_device(h, &doutput);
  printf("[%s] launch-sized aliased workgroup arena\n",
         ok ? "PASS" : "FAIL");
  return ok;
}

static int test_subgroup_contract(Harness *h) {
  enum { N = 119 };
  float fsrc[N], fdst[N];
  uint32_t usrc[N], udst[N];
  CUdeviceptr dfsrc = 0, dfdst = 0, dusrc = 0, dudst = 0;
  int32_t n = N;
  void *parameters[] = {&dfsrc, &dfdst, &dusrc, &dudst, &n};
  int ok = 0;
  for (int i = 0; i < N; i++) {
    fsrc[i] = (float)((i % 9) + 1);
    usrc[i] = (uint32_t)((i % 11) + 1);
    fdst[i] = 0.0f;
    udst[i] = 0;
  }
  if (!alloc_device(h, &dfsrc, fsrc, sizeof(fsrc)) ||
      !alloc_device(h, &dfdst, NULL, sizeof(fdst)) ||
      !alloc_device(h, &dusrc, usrc, sizeof(usrc)) ||
      !alloc_device(h, &dudst, NULL, sizeof(udst)) ||
      !launch(h, "subgroup_contract", 2, 1, 1, 64, 1, 1, parameters) ||
      !copy_from_device(h, fdst, dfdst, sizeof(fdst)) ||
      !copy_from_device(h, udst, dudst, sizeof(udst)))
    goto cleanup;
  for (int i = 0; i < N; i++) {
    int base = (i / 32) * 32;
    int end = base + 32 < N ? base + 32 : N;
    float fsum = 0.0f;
    uint32_t usum = 0;
    for (int lane = base; lane < end; lane++) {
      fsum += fsrc[lane];
      usum += usrc[lane];
    }
    if (fdst[i] != fsum + fsrc[base] || udst[i] != usum + usrc[base]) {
      fprintf(stderr,
              "subgroup mismatch i=%d: f=%.9g/%.9g u=%u/%u\n", i,
              fdst[i], fsum + fsrc[base], udst[i], usum + usrc[base]);
      goto cleanup;
    }
  }
  ok = 1;
cleanup:
  free_device(h, &dfsrc);
  free_device(h, &dfdst);
  free_device(h, &dusrc);
  free_device(h, &dudst);
  printf("[%s] subgroup reduce and broadcast\n", ok ? "PASS" : "FAIL");
  return ok;
}

static int test_subgroup_extended(Harness *h) {
  enum { N = 119, OUTPUTS = 4 * N };
  float fsrc[N], fdst[OUTPUTS];
  uint32_t usrc[N], udst[OUTPUTS];
  CUdeviceptr dfsrc = 0, dfdst = 0, dusrc = 0, dudst = 0;
  int32_t n = N;
  void *parameters[] = {&dfsrc, &dfdst, &dusrc, &dudst, &n};
  int ok = 0;
  for (int i = 0; i < N; i++) {
    fsrc[i] = (float)((i % 13) - 6);
    usrc[i] = (uint32_t)((i * 17) % 101 + 1);
  }
  for (int i = 0; i < OUTPUTS; i++) {
    fdst[i] = NAN;
    udst[i] = UINT32_MAX;
  }
  if (!alloc_device(h, &dfsrc, fsrc, sizeof(fsrc)) ||
      !alloc_device(h, &dfdst, NULL, sizeof(fdst)) ||
      !alloc_device(h, &dusrc, usrc, sizeof(usrc)) ||
      !alloc_device(h, &dudst, NULL, sizeof(udst)) ||
      !launch(h, "subgroup_extended", 2, 1, 1, 64, 1, 1, parameters) ||
      !copy_from_device(h, fdst, dfdst, sizeof(fdst)) ||
      !copy_from_device(h, udst, dudst, sizeof(udst)))
    goto cleanup;
  for (int i = 0; i < N; i++) {
    int base = (i / 32) * 32;
    int end = base + 32 < N ? base + 32 : N;
    float fmin = fsrc[base], fmax = fsrc[base];
    float finc = 0.0f, fexc = 0.0f;
    uint32_t umin = usrc[base], umax = usrc[base];
    uint32_t uinc = 0, uexc = 0;
    for (int lane = base; lane < end; lane++) {
      if (fsrc[lane] < fmin) fmin = fsrc[lane];
      if (fsrc[lane] > fmax) fmax = fsrc[lane];
      if (usrc[lane] < umin) umin = usrc[lane];
      if (usrc[lane] > umax) umax = usrc[lane];
    }
    for (int lane = base; lane <= i; lane++) {
      finc += fsrc[lane];
      uinc += usrc[lane];
    }
    for (int lane = base; lane < i; lane++) {
      fexc += fsrc[lane];
      uexc += usrc[lane];
    }
    if (fdst[i] != fmin || fdst[N + i] != fmax ||
        fdst[2 * N + i] != finc || fdst[3 * N + i] != fexc ||
        udst[i] != umin || udst[N + i] != umax ||
        udst[2 * N + i] != uinc || udst[3 * N + i] != uexc) {
      fprintf(stderr,
              "extended subgroup mismatch i=%d: f=(%.9g %.9g %.9g %.9g)/(%.9g %.9g %.9g %.9g) u=(%u %u %u %u)/(%u %u %u %u)\n",
              i, fdst[i], fdst[N + i], fdst[2 * N + i],
              fdst[3 * N + i], fmin, fmax, finc, fexc, udst[i],
              udst[N + i], udst[2 * N + i], udst[3 * N + i], umin,
              umax, uinc, uexc);
      goto cleanup;
    }
  }
  ok = 1;
cleanup:
  free_device(h, &dfsrc);
  free_device(h, &dfdst);
  free_device(h, &dusrc);
  free_device(h, &dudst);
  printf("[%s] subgroup min/max and inclusive/exclusive scans\n",
         ok ? "PASS" : "FAIL");
  return ok;
}

static int test_subgroup_exchange_vote(Harness *h) {
  enum { N = 45, OUTPUTS = 7 * N };
  uint32_t usrc[N], uout[OUTPUTS];
  float fsrc[N], fout[N];
  CUdeviceptr dusrc = 0, duout = 0, dfsrc = 0, dfout = 0;
  void *parameters[] = {&dusrc, &duout, &dfsrc, &dfout};
  int ok = 0;

  for (int i = 0; i < N; i++) {
    usrc[i] = UINT32_C(1000) + (uint32_t)i;
    fsrc[i] = (float)i + 0.25f;
    fout[i] = NAN;
  }
  for (int i = 0; i < OUTPUTS; i++) uout[i] = UINT32_MAX;
  if (!alloc_device(h, &dusrc, usrc, sizeof(usrc)) ||
      !alloc_device(h, &duout, NULL, sizeof(uout)) ||
      !alloc_device(h, &dfsrc, fsrc, sizeof(fsrc)) ||
      !alloc_device(h, &dfout, NULL, sizeof(fout)) ||
      !launch(h, "subgroup_exchange_vote", 1, 1, 1, N, 1, 1,
              parameters) ||
      !copy_from_device(h, uout, duout, sizeof(uout)) ||
      !copy_from_device(h, fout, dfout, sizeof(fout)))
    goto cleanup;
  for (int i = 0; i < N; i++) {
    const int subgroup_base = (i / 32) * 32;
    const int lane = i - subgroup_base;
    const int active = subgroup_base == 0 ? 32 : N - subgroup_base;
    const int source = (lane * 5 + 7) % 32;
    const int selected = subgroup_base + (source < active ? source : lane);
    uint32_t ballot = 0;
    for (int source_lane = 0; source_lane < active; source_lane++) {
      if (source_lane % 3 == 1) ballot |= UINT32_C(1) << source_lane;
    }
    const uint32_t expected[] = {
        usrc[selected], ballot, 0, 1, 1, 0, 0};
    for (int field = 0; field < 7; field++) {
      if (uout[i * 7 + field] != expected[field]) {
        fprintf(stderr,
                "subgroup exchange/vote mismatch i=%d field=%d: got %u "
                "expected %u\n",
                i, field, uout[i * 7 + field], expected[field]);
        goto cleanup;
      }
    }
    if (fout[i] != fsrc[selected]) {
      fprintf(stderr,
              "subgroup float shuffle mismatch i=%d: got %.9g expected %.9g\n",
              i, fout[i], fsrc[selected]);
      goto cleanup;
    }
  }
  ok = 1;
cleanup:
  free_device(h, &dusrc);
  free_device(h, &duout);
  free_device(h, &dfsrc);
  free_device(h, &dfout);
  printf("[%s] subgroup variable shuffle, ballot words, and votes\n",
         ok ? "PASS" : "FAIL");
  return ok;
}

static int test_softmax_rows_f32(Harness *h) {
  enum { ROWS = 5, COLS = 47, LDIN = 53, LDOUT = 59 };
  float input[ROWS * LDIN], output[ROWS * LDOUT];
  CUdeviceptr dinput = 0, doutput = 0;
  int32_t rows = ROWS, cols = COLS, ldin = LDIN, ldout = LDOUT;
  void *parameters[] = {&dinput, &doutput, &rows, &cols, &ldin, &ldout};
  int ok = 0;
  for (int i = 0; i < ROWS * LDIN; i++) input[i] = NAN;
  for (int i = 0; i < ROWS * LDOUT; i++) output[i] = -12345.0f;
  for (int row = 0; row < ROWS; row++) {
    for (int col = 0; col < COLS; col++) {
      input[row * LDIN + col] =
          (float)(((row * 7 + col * 11) % 31) - 15) * 0.125f;
    }
  }
  if (!alloc_device(h, &dinput, input, sizeof(input)) ||
      !alloc_device(h, &doutput, output, sizeof(output)) ||
      !launch(h, "softmax_rows_f32", ROWS, 1, 1, 32, 1, 1, parameters) ||
      !copy_from_device(h, output, doutput, sizeof(output)))
    goto cleanup;
  for (int row = 0; row < ROWS; row++) {
    float maximum = input[row * LDIN];
    float denominator = 0.0f;
    float actual_sum = 0.0f;
    for (int col = 1; col < COLS; col++) {
      float value = input[row * LDIN + col];
      if (value > maximum) maximum = value;
    }
    for (int col = 0; col < COLS; col++) {
      denominator += expf(input[row * LDIN + col] - maximum);
    }
    for (int col = 0; col < COLS; col++) {
      float expected = expf(input[row * LDIN + col] - maximum) / denominator;
      float actual = output[row * LDOUT + col];
      actual_sum += actual;
      if (!isfinite(actual) || fabsf(actual - expected) > 2.0e-4f) {
        fprintf(stderr,
                "softmax mismatch row=%d col=%d: got %.9g expected %.9g\n",
                row, col, actual, expected);
        goto cleanup;
      }
    }
    if (fabsf(actual_sum - 1.0f) > 3.0e-4f) {
      fprintf(stderr, "softmax normalization mismatch row=%d: sum %.9g\n",
              row, actual_sum);
      goto cleanup;
    }
    for (int col = COLS; col < LDOUT; col++) {
      if (output[row * LDOUT + col] != -12345.0f) {
        fprintf(stderr, "softmax overwrote padding row=%d col=%d: %.9g\n",
                row, col, output[row * LDOUT + col]);
        goto cleanup;
      }
    }
  }
  ok = 1;
cleanup:
  free_device(h, &dinput);
  free_device(h, &doutput);
  printf("[%s] numerically stable arbitrary-width row softmax\n",
         ok ? "PASS" : "FAIL");
  return ok;
}

static int test_layer_norm_rows_f32(Harness *h) {
  enum { ROWS = 4, COLS = 45, LDIN = 52, LDOUT = 56 };
  float input[ROWS * LDIN], gamma[COLS], beta[COLS];
  float output[ROWS * LDOUT];
  CUdeviceptr dinput = 0, dgamma = 0, dbeta = 0, doutput = 0;
  int32_t rows = ROWS, cols = COLS, ldin = LDIN, ldout = LDOUT;
  float epsilon = 1.0e-5f;
  void *parameters[] = {&dinput, &dgamma, &dbeta, &doutput, &rows,
                        &cols,   &ldin,   &ldout, &epsilon};
  int ok = 0;
  for (int i = 0; i < ROWS * LDIN; i++) input[i] = NAN;
  for (int i = 0; i < ROWS * LDOUT; i++) output[i] = -12345.0f;
  for (int col = 0; col < COLS; col++) {
    gamma[col] = 0.75f + (float)(col % 7) * 0.0625f;
    beta[col] = (float)((col % 9) - 4) * 0.03125f;
  }
  for (int row = 0; row < ROWS; row++) {
    for (int col = 0; col < COLS; col++) {
      input[row * LDIN + col] =
          (float)(((row * 13 + col * 5) % 37) - 18) * 0.09375f;
    }
  }
  if (!alloc_device(h, &dinput, input, sizeof(input)) ||
      !alloc_device(h, &dgamma, gamma, sizeof(gamma)) ||
      !alloc_device(h, &dbeta, beta, sizeof(beta)) ||
      !alloc_device(h, &doutput, output, sizeof(output)) ||
      !launch(h, "layer_norm_rows_f32", ROWS, 1, 1, 32, 1, 1,
              parameters) ||
      !copy_from_device(h, output, doutput, sizeof(output)))
    goto cleanup;
  for (int row = 0; row < ROWS; row++) {
    float mean = 0.0f, variance = 0.0f;
    for (int col = 0; col < COLS; col++) mean += input[row * LDIN + col];
    mean /= (float)COLS;
    for (int col = 0; col < COLS; col++) {
      float centered = input[row * LDIN + col] - mean;
      variance += centered * centered;
    }
    variance /= (float)COLS;
    for (int col = 0; col < COLS; col++) {
      float expected = (input[row * LDIN + col] - mean) /
                           sqrtf(variance + epsilon) * gamma[col] +
                       beta[col];
      float actual = output[row * LDOUT + col];
      if (!isfinite(actual) || fabsf(actual - expected) > 8.0e-4f) {
        fprintf(stderr,
                "layer norm mismatch row=%d col=%d: got %.9g expected %.9g\n",
                row, col, actual, expected);
        goto cleanup;
      }
    }
    for (int col = COLS; col < LDOUT; col++) {
      if (output[row * LDOUT + col] != -12345.0f) {
        fprintf(stderr,
                "layer norm overwrote padding row=%d col=%d: %.9g\n",
                row, col, output[row * LDOUT + col]);
        goto cleanup;
      }
    }
  }
  ok = 1;
cleanup:
  free_device(h, &dinput);
  free_device(h, &dgamma);
  free_device(h, &dbeta);
  free_device(h, &doutput);
  printf("[%s] arbitrary-width affine layer normalization\n",
         ok ? "PASS" : "FAIL");
  return ok;
}

static int test_tensor_f16_f32(Harness *h) {
  /* PTX WMMA requires half strides to be multiples of eight elements and
   * float strides to be multiples of four. Keep every stride padded but legal
   * so this test distinguishes runtime-stride handling from invalid input. */
  enum { M = 16, N = 16, K = 16, LDA = 24, LDB = 24, LDC = 20, LDD = 24 };
  uint16_t a[M * LDA], b[N * LDB];
  float c[M * LDC], d[M * LDD];
  CUdeviceptr da = 0, db = 0, dc = 0, dd = 0;
  int32_t lda = LDA, ldb = LDB, ldc = LDC, ldd = LDD;
  void *parameters[] = {&da, &db, &dc, &dd, &lda, &ldb, &ldc, &ldd};
  int ok = 0;
  for (int i = 0; i < M * LDA; i++) a[i] = UINT16_C(0x7e00);
  for (int i = 0; i < N * LDB; i++) b[i] = UINT16_C(0x7e00);
  for (int i = 0; i < M * LDC; i++) c[i] = NAN;
  for (int i = 0; i < M * LDD; i++) d[i] = -12345.0f;
  for (int row = 0; row < M; row++) {
    for (int inner = 0; inner < K; inner++) {
      a[row * LDA + inner] = UINT16_C(0x3c00); /* binary16 1.0 */
    }
  }
  for (int col = 0; col < N; col++) {
    for (int inner = 0; inner < K; inner++) {
      b[col * LDB + inner] = UINT16_C(0x3c00); /* column-major */
    }
  }
  for (int row = 0; row < M; row++) {
    for (int col = 0; col < N; col++) {
      c[row * LDC + col] = (float)((row * N + col) % 7) * 0.25f;
    }
  }
  if (!alloc_device(h, &da, a, sizeof(a)) ||
      !alloc_device(h, &db, b, sizeof(b)) ||
      !alloc_device(h, &dc, c, sizeof(c)) ||
      !alloc_device(h, &dd, d, sizeof(d)) ||
      !launch(h, "tensor_f16_f32", 1, 1, 1, 32, 1, 1, parameters) ||
      !copy_from_device(h, d, dd, sizeof(d)))
    goto cleanup;
  for (int row = 0; row < M; row++) {
    for (int col = 0; col < N; col++) {
      float expected = 16.0f + c[row * LDC + col];
      float actual = d[row * LDD + col];
      if (actual != expected) {
        fprintf(stderr,
                "tensor_f16_f32 mismatch row=%d col=%d: got %.9g expected %.9g\n",
                row, col, actual, expected);
        goto cleanup;
      }
    }
    for (int col = N; col < LDD; col++) {
      if (d[row * LDD + col] != -12345.0f) {
        fprintf(stderr,
                "tensor_f16_f32 overwrote D padding row=%d col=%d: %.9g\n",
                row, col, d[row * LDD + col]);
        goto cleanup;
      }
    }
  }
  ok = 1;
cleanup:
  free_device(h, &da);
  free_device(h, &db);
  free_device(h, &dc);
  free_device(h, &dd);
  printf("[%s] f16/f32 tensor numerical and runtime-stride contract\n",
         ok ? "PASS" : "FAIL");
  return ok;
}

static void set_packed_nibble(uint8_t *storage, size_t logical_index,
                              uint8_t value) {
  size_t byte = logical_index >> 1;
  unsigned shift = (unsigned)(logical_index & 1u) * 4u;
  storage[byte] = (uint8_t)((storage[byte] & ~(UINT8_C(0xf) << shift)) |
                            ((value & UINT8_C(0xf)) << shift));
}

/* Target-independent least-significant-bit-first subbyte packing. Byte-wise
 * updates avoid unaligned or host-endian integer accesses on DGX Spark's
 * AArch64 host as well as x86-64 development systems. */
static void set_packed_bits(uint8_t *storage, size_t logical_index,
                            unsigned bits, uint8_t value) {
  size_t first_bit = logical_index * (size_t)bits;
  for (unsigned bit = 0; bit < bits; bit++) {
    size_t storage_bit = first_bit + bit;
    uint8_t mask = (uint8_t)(UINT8_C(1) << (storage_bit & 7u));
    if ((value >> bit) & 1u)
      storage[storage_bit >> 3] |= mask;
    else
      storage[storage_bit >> 3] &= (uint8_t)~mask;
  }
}

static int test_tensor_mxfp4_native(Harness *h) {
  enum {
    M = 16,
    N = 16,
    K = 64,
    LDA = 72,
    LDB = 72,
    LDC = 20,
    LDD = 24,
    LDSA = 3,
    LDSB = 3
  };
  const uint8_t e2m1_bits[5] = {
      UINT8_C(0x1), UINT8_C(0x2), UINT8_C(0xa), UINT8_C(0x4),
      UINT8_C(0x9)};
  const float values[5] = {0.5f, 1.0f, -1.0f, 2.0f, -0.5f};
  const uint8_t ue8m0_bits[3] = {
      UINT8_C(0x7e), UINT8_C(0x7f), UINT8_C(0x80)};
  const float scales[3] = {0.5f, 1.0f, 2.0f};
  uint8_t a[M * LDA / 2], b[N * LDB / 2];
  uint8_t a_scale[M * LDSA], b_scale[N * LDSB];
  float c[M * LDC], d[M * LDD];
  CUdeviceptr da = 0, db = 0, dc = 0, dd = 0, dsa = 0, dsb = 0;
  void *parameters[] = {&da, &db, &dc, &dd, &dsa, &dsb};
  int ok = 0;

  memset(a, 0x77, sizeof(a));
  memset(b, 0x77, sizeof(b));
  memset(a_scale, 0xff, sizeof(a_scale));
  memset(b_scale, 0xff, sizeof(b_scale));
  for (int i = 0; i < M * LDC; i++) c[i] = NAN;
  for (int i = 0; i < M * LDD; i++) d[i] = -56789.0f;
  for (int row = 0; row < M; row++) {
    for (int inner = 0; inner < K; inner++) {
      set_packed_nibble(a, (size_t)row * LDA + (size_t)inner,
                        e2m1_bits[(row * 3 + inner * 2) % 5]);
    }
    for (int chunk = 0; chunk < 2; chunk++)
      a_scale[row * LDSA + chunk] = ue8m0_bits[(row * 2 + chunk) % 3];
  }
  for (int col = 0; col < N; col++) {
    for (int inner = 0; inner < K; inner++) {
      set_packed_nibble(b, (size_t)col * LDB + (size_t)inner,
                        e2m1_bits[(col * 2 + inner * 3) % 5]);
    }
    for (int chunk = 0; chunk < 2; chunk++)
      b_scale[col * LDSB + chunk] = ue8m0_bits[(col + chunk * 2) % 3];
  }
  for (int row = 0; row < M; row++)
    for (int col = 0; col < N; col++)
      c[row * LDC + col] = (float)((row * N + col) % 13) * 0.125f;

  if (!alloc_device(h, &da, a, sizeof(a)) ||
      !alloc_device(h, &db, b, sizeof(b)) ||
      !alloc_device(h, &dc, c, sizeof(c)) ||
      !alloc_device(h, &dd, d, sizeof(d)) ||
      !alloc_device(h, &dsa, a_scale, sizeof(a_scale)) ||
      !alloc_device(h, &dsb, b_scale, sizeof(b_scale)) ||
      !launch(h, "tensor_mxfp4_m16n16k64", 1, 1, 1, 32, 1, 1,
              parameters) ||
      !copy_from_device(h, d, dd, sizeof(d)))
    goto cleanup;
  for (int row = 0; row < M; row++) {
    for (int col = 0; col < LDD; col++) {
      float actual = d[row * LDD + col];
      if (col >= N) {
        if (actual != -56789.0f) {
          fprintf(stderr,
                  "MXFP4 tile overwrote D padding row=%d col=%d: %.9g\n",
                  row, col, actual);
          goto cleanup;
        }
        continue;
      }
      float expected = c[row * LDC + col];
      for (int inner = 0; inner < K; inner++) {
        int chunk = inner / 32;
        expected += values[(row * 3 + inner * 2) % 5] *
                    scales[(row * 2 + chunk) % 3] *
                    values[(col * 2 + inner * 3) % 5] *
                    scales[(col + chunk * 2) % 3];
      }
      if (actual != expected) {
        fprintf(stderr,
                "MXFP4 tile mismatch row=%d col=%d: got %.9g expected %.9g\n",
                row, col, actual, expected);
        goto cleanup;
      }
    }
  }
  ok = 1;
cleanup:
  free_device(h, &da);
  free_device(h, &db);
  free_device(h, &dc);
  free_device(h, &dd);
  free_device(h, &dsa);
  free_device(h, &dsb);
  printf("[%s] packed MXFP4/UE8M0 block-scale tensor contract\n",
         ok ? "PASS" : "FAIL");
  return ok;
}

static int test_tensor_mxfp4_chain_resident(Harness *h) {
  enum {
    M = 16,
    N = 16,
    K = 64,
    LDA = 72,
    LDB = 72,
    LDOUT = 24,
    LDS = 3
  };
  const uint8_t e2m1_bits[5] = {UINT8_C(0x1), UINT8_C(0x2),
                                  UINT8_C(0xa), UINT8_C(0x4),
                                  UINT8_C(0x9)};
  const float values[5] = {0.5f, 1.0f, -1.0f, 2.0f, -0.5f};
  const uint8_t ue8m0_bits[3] = {UINT8_C(0x7e), UINT8_C(0x7f),
                                   UINT8_C(0x80)};
  const float scales[3] = {0.5f, 1.0f, 2.0f};
  uint8_t a[M * LDA / 2], b[N * LDB / 2];
  uint8_t a_scale[M * LDS], b_scale[N * LDS];
  float c[M * LDOUT], d[M * LDOUT];
  CUdeviceptr da = 0, db = 0, dc = 0, dd = 0, dsa = 0, dsb = 0;
  void *parameters[] = {&da, &db, &dc, &dd, &dsa, &dsb};
  int ok = 0;

  memset(a, 0x77, sizeof(a));
  memset(b, 0x77, sizeof(b));
  memset(a_scale, 0xff, sizeof(a_scale));
  memset(b_scale, 0xff, sizeof(b_scale));
  for (int i = 0; i < M * LDOUT; i++) {
    c[i] = NAN;
    d[i] = -60123.0f;
  }
  for (int row = 0; row < M; row++) {
    for (int inner = 0; inner < K; inner++)
      set_packed_nibble(a, (size_t)row * LDA + (size_t)inner,
                        e2m1_bits[(row * 3 + inner * 2) % 5]);
    for (int chunk = 0; chunk < 2; chunk++)
      a_scale[row * LDS + chunk] = ue8m0_bits[(row * 2 + chunk) % 3];
  }
  for (int col = 0; col < N; col++) {
    for (int inner = 0; inner < K; inner++)
      set_packed_nibble(b, (size_t)col * LDB + (size_t)inner,
                        e2m1_bits[(col * 2 + inner * 3) % 5]);
    for (int chunk = 0; chunk < 2; chunk++)
      b_scale[col * LDS + chunk] = ue8m0_bits[(col + chunk * 2) % 3];
  }
  for (int row = 0; row < M; row++)
    for (int col = 0; col < N; col++)
      c[row * LDOUT + col] = (float)((row * N + col) % 11) * 0.125f;

  if (!alloc_device(h, &da, a, sizeof(a)) ||
      !alloc_device(h, &db, b, sizeof(b)) ||
      !alloc_device(h, &dc, c, sizeof(c)) ||
      !alloc_device(h, &dd, d, sizeof(d)) ||
      !alloc_device(h, &dsa, a_scale, sizeof(a_scale)) ||
      !alloc_device(h, &dsb, b_scale, sizeof(b_scale)) ||
      !launch(h, "tensor_mxfp4_chain3_m16n16k64", 1, 1, 1, 32, 1, 1,
              parameters) ||
      !copy_from_device(h, d, dd, sizeof(d)))
    goto cleanup;
  for (int row = 0; row < M; row++) {
    for (int col = 0; col < LDOUT; col++) {
      float actual = d[row * LDOUT + col];
      if (col >= N) {
        if (actual != -60123.0f) {
          fprintf(stderr,
                  "MXFP4 resident chain overwrote padding row=%d col=%d\n",
                  row, col);
          goto cleanup;
        }
        continue;
      }
      float product = 0.0f;
      for (int inner = 0; inner < K; inner++) {
        int chunk = inner / 32;
        product += values[(row * 3 + inner * 2) % 5] *
                   scales[(row * 2 + chunk) % 3] *
                   values[(col * 2 + inner * 3) % 5] *
                   scales[(col + chunk * 2) % 3];
      }
      float expected = c[row * LDOUT + col] + 3.0f * product;
      if (actual != expected) {
        fprintf(stderr,
                "MXFP4 resident chain mismatch row=%d col=%d: got %.9g expected %.9g\n",
                row, col, actual, expected);
        goto cleanup;
      }
    }
  }
  ok = 1;
cleanup:
  free_device(h, &da);
  free_device(h, &db);
  free_device(h, &dc);
  free_device(h, &dd);
  free_device(h, &dsa);
  free_device(h, &dsb);
  printf("[%s] three-tile MXFP4 accumulator-residency contract\n",
         ok ? "PASS" : "FAIL");
  return ok;
}

static int test_tensor_mxfp4_loop_resident(Harness *h) {
  enum {
    M = 16,
    N = 16,
    K = 192,
    LDA = 200,
    LDB = 200,
    LDC = 20,
    LDD = 24,
    LDS = 7
  };
  const uint8_t e2m1_bits[5] = {UINT8_C(0x1), UINT8_C(0x2),
                                  UINT8_C(0xa), UINT8_C(0x4),
                                  UINT8_C(0x9)};
  const float values[5] = {0.5f, 1.0f, -1.0f, 2.0f, -0.5f};
  const uint8_t ue8m0_bits[3] = {UINT8_C(0x7e), UINT8_C(0x7f),
                                   UINT8_C(0x80)};
  const float scales[3] = {0.5f, 1.0f, 2.0f};
  uint8_t a[M * LDA / 2], b[N * LDB / 2];
  uint8_t a_scale[M * LDS], b_scale[N * LDS];
  float c[M * LDC], d[M * LDD];
  CUdeviceptr da = 0, db = 0, dc = 0, dd = 0, dsa = 0, dsb = 0;
  int32_t k = K, lda = LDA, ldb = LDB, ldc = LDC, ldd = LDD;
  void *parameters[] = {&da,  &db,  &dc,  &dd,  &dsa, &dsb,
                        &k,   &lda, &ldb, &ldc, &ldd};
  int ok = 0;

  memset(a, 0x77, sizeof(a));
  memset(b, 0x77, sizeof(b));
  memset(a_scale, 0xff, sizeof(a_scale));
  memset(b_scale, 0xff, sizeof(b_scale));
  for (int i = 0; i < M * LDC; i++) c[i] = NAN;
  for (int i = 0; i < M * LDD; i++) d[i] = -61234.0f;
  for (int row = 0; row < M; row++) {
    for (int inner = 0; inner < K; inner++)
      set_packed_nibble(a, (size_t)row * LDA + (size_t)inner,
                        e2m1_bits[(row * 3 + inner * 2) % 5]);
    for (int chunk = 0; chunk < K / 32; chunk++)
      a_scale[row * LDS + chunk] = ue8m0_bits[(row * 2 + chunk) % 3];
  }
  for (int col = 0; col < N; col++) {
    for (int inner = 0; inner < K; inner++)
      set_packed_nibble(b, (size_t)col * LDB + (size_t)inner,
                        e2m1_bits[(col * 2 + inner * 3) % 5]);
    for (int chunk = 0; chunk < K / 32; chunk++)
      b_scale[col * LDS + chunk] = ue8m0_bits[(col + chunk * 2) % 3];
  }
  for (int row = 0; row < M; row++)
    for (int col = 0; col < N; col++)
      c[row * LDC + col] = (float)((row * N + col) % 13) * 0.125f;

  if (!alloc_device(h, &da, a, sizeof(a)) ||
      !alloc_device(h, &db, b, sizeof(b)) ||
      !alloc_device(h, &dc, c, sizeof(c)) ||
      !alloc_device(h, &dd, d, sizeof(d)) ||
      !alloc_device(h, &dsa, a_scale, sizeof(a_scale)) ||
      !alloc_device(h, &dsb, b_scale, sizeof(b_scale)) ||
      !launch(h, "tensor_mxfp4_runtime_k_m16n16k64", 1, 1, 1, 32, 1, 1,
              parameters) ||
      !copy_from_device(h, d, dd, sizeof(d)))
    goto cleanup;
  for (int row = 0; row < M; row++) {
    for (int col = 0; col < LDD; col++) {
      float actual = d[row * LDD + col];
      if (col >= N) {
        if (actual != -61234.0f) {
          fprintf(stderr,
                  "MXFP4 resident loop overwrote padding row=%d col=%d\n",
                  row, col);
          goto cleanup;
        }
        continue;
      }
      float expected = c[row * LDC + col];
      for (int inner = 0; inner < K; inner++) {
        int chunk = inner / 32;
        expected += values[(row * 3 + inner * 2) % 5] *
                    scales[(row * 2 + chunk) % 3] *
                    values[(col * 2 + inner * 3) % 5] *
                    scales[(col + chunk * 2) % 3];
      }
      if (actual != expected) {
        fprintf(stderr,
                "MXFP4 resident loop mismatch row=%d col=%d: got %.9g expected %.9g\n",
                row, col, actual, expected);
        goto cleanup;
      }
    }
  }
  ok = 1;
cleanup:
  free_device(h, &da);
  free_device(h, &db);
  free_device(h, &dc);
  free_device(h, &dd);
  free_device(h, &dsa);
  free_device(h, &dsb);
  printf("[%s] runtime-K MXFP4 accumulator-residency contract\n",
         ok ? "PASS" : "FAIL");
  return ok;
}

typedef enum {
  NVFP4_DIRECT,
  NVFP4_CHAIN,
  NVFP4_LOOP
} Nvfp4TestMode;

static int test_tensor_nvfp4(Harness *h, Nvfp4TestMode mode) {
  enum { M = 16, N = 16 };
  const uint8_t e2m1_bits[5] = {UINT8_C(0x1), UINT8_C(0x2),
                                  UINT8_C(0xa), UINT8_C(0x4),
                                  UINT8_C(0x9)};
  const float values[5] = {0.5f, 1.0f, -1.0f, 2.0f, -0.5f};
  /* UE4M3 is positive E4M3 with the high bit clear. These exact binary
   * fractions exercise mantissa-bearing scales, not only powers of two. */
  const uint8_t ue4m3_bits[5] = {UINT8_C(0x30), UINT8_C(0x38),
                                   UINT8_C(0x3c), UINT8_C(0x40),
                                   UINT8_C(0x34)};
  const float scales[5] = {0.5f, 1.0f, 1.5f, 2.0f, 0.75f};
  int k_extent = mode == NVFP4_LOOP ? 192 : 64;
  int lda = mode == NVFP4_LOOP ? 200 : 72;
  int ldb = lda;
  int ldc = mode == NVFP4_CHAIN ? 24 : 20;
  int ldd = 24;
  int scale_ld = mode == NVFP4_LOOP ? 13 : 5;
  size_t a_bytes = (size_t)M * (size_t)lda / 2u;
  size_t b_bytes = (size_t)N * (size_t)ldb / 2u;
  size_t a_scale_bytes = (size_t)M * (size_t)scale_ld;
  size_t b_scale_bytes = (size_t)N * (size_t)scale_ld;
  size_t c_bytes = (size_t)M * (size_t)ldc * sizeof(float);
  size_t d_bytes = (size_t)M * (size_t)ldd * sizeof(float);
  uint8_t *a = (uint8_t *)malloc(a_bytes);
  uint8_t *b = (uint8_t *)malloc(b_bytes);
  uint8_t *a_scale = (uint8_t *)malloc(a_scale_bytes);
  uint8_t *b_scale = (uint8_t *)malloc(b_scale_bytes);
  float *c = (float *)malloc(c_bytes);
  float *d = (float *)malloc(d_bytes);
  CUdeviceptr da = 0, db = 0, dc = 0, dd = 0, dsa = 0, dsb = 0;
  int32_t runtime_k = k_extent;
  int32_t runtime_lda = lda, runtime_ldb = ldb;
  int32_t runtime_ldc = ldc, runtime_ldd = ldd;
  void *static_parameters[] = {&da, &db, &dc, &dd, &dsa, &dsb};
  void *loop_parameters[] = {&da,          &db,          &dc,          &dd,
                             &dsa,         &dsb,         &runtime_k,   &runtime_lda,
                             &runtime_ldb, &runtime_ldc, &runtime_ldd};
  const char *kernel = mode == NVFP4_DIRECT
                           ? "tensor_nvfp4_m16n16k64"
                           : (mode == NVFP4_CHAIN
                                  ? "tensor_nvfp4_chain3_m16n16k64"
                                  : "tensor_nvfp4_runtime_k_m16n16k64");
  const char *label = mode == NVFP4_DIRECT
                          ? "packed NVFP4/UE4M3 block-scale tensor contract"
                          : (mode == NVFP4_CHAIN
                                 ? "three-tile NVFP4 accumulator-residency contract"
                                 : "runtime-K NVFP4 accumulator-residency contract");
  const float sentinel = -62345.0f;
  int ok = 0;

  if (!a || !b || !a_scale || !b_scale || !c || !d) {
    fprintf(stderr, "host allocation failed in NVFP4 test\n");
    goto cleanup;
  }
  memset(a, 0x77, a_bytes);
  memset(b, 0x77, b_bytes);
  memset(a_scale, 0xff, a_scale_bytes);
  memset(b_scale, 0xff, b_scale_bytes);
  for (int i = 0; i < M * ldc; i++) c[i] = NAN;
  for (int i = 0; i < M * ldd; i++) d[i] = sentinel;
  for (int row = 0; row < M; row++) {
    for (int inner = 0; inner < k_extent; inner++)
      set_packed_nibble(a, (size_t)row * (size_t)lda + (size_t)inner,
                        e2m1_bits[(row * 3 + inner * 2) % 5]);
    for (int chunk = 0; chunk < k_extent / 16; chunk++)
      a_scale[row * scale_ld + chunk] =
          ue4m3_bits[(row * 2 + chunk) % 5];
  }
  for (int col = 0; col < N; col++) {
    for (int inner = 0; inner < k_extent; inner++)
      set_packed_nibble(b, (size_t)col * (size_t)ldb + (size_t)inner,
                        e2m1_bits[(col * 2 + inner * 3) % 5]);
    for (int chunk = 0; chunk < k_extent / 16; chunk++)
      b_scale[col * scale_ld + chunk] =
          ue4m3_bits[(col + chunk * 3) % 5];
  }
  for (int row = 0; row < M; row++)
    for (int col = 0; col < N; col++)
      c[row * ldc + col] = (float)((row * N + col) % 13) * 0.125f;

  if (!alloc_device(h, &da, a, a_bytes) ||
      !alloc_device(h, &db, b, b_bytes) ||
      !alloc_device(h, &dc, c, c_bytes) ||
      !alloc_device(h, &dd, d, d_bytes) ||
      !alloc_device(h, &dsa, a_scale, a_scale_bytes) ||
      !alloc_device(h, &dsb, b_scale, b_scale_bytes) ||
      !launch(h, kernel, 1, 1, 1, 32, 1, 1,
              mode == NVFP4_LOOP ? loop_parameters : static_parameters) ||
      !copy_from_device(h, d, dd, d_bytes))
    goto cleanup;

  for (int row = 0; row < M; row++) {
    for (int col = 0; col < ldd; col++) {
      float actual = d[row * ldd + col];
      if (col >= N) {
        if (actual != sentinel) {
          fprintf(stderr,
                  "NVFP4 overwrote D padding mode=%d row=%d col=%d: %.9g\n",
                  (int)mode, row, col, actual);
          goto cleanup;
        }
        continue;
      }
      float product = 0.0f;
      for (int inner = 0; inner < k_extent; inner++) {
        int chunk = inner / 16;
        product += values[(row * 3 + inner * 2) % 5] *
                   scales[(row * 2 + chunk) % 5] *
                   values[(col * 2 + inner * 3) % 5] *
                   scales[(col + chunk * 3) % 5];
      }
      float expected = c[row * ldc + col] +
                       (mode == NVFP4_CHAIN ? 3.0f : 1.0f) * product;
      if (actual != expected) {
        fprintf(stderr,
                "NVFP4 mismatch mode=%d row=%d col=%d: got %.9g expected %.9g\n",
                (int)mode, row, col, actual, expected);
        goto cleanup;
      }
    }
  }
  ok = 1;
cleanup:
  free_device(h, &da);
  free_device(h, &db);
  free_device(h, &dc);
  free_device(h, &dd);
  free_device(h, &dsa);
  free_device(h, &dsb);
  free(a);
  free(b);
  free(a_scale);
  free(b_scale);
  free(c);
  free(d);
  printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
  return ok;
}

typedef enum {
  MXFP6_DIRECT,
  MXFP6_CHAIN,
  MXFP6_LOOP
} Mxfp6TestMode;

static int test_tensor_mxfp6(Harness *h, Mxfp6TestMode mode) {
  enum { M = 16, N = 16 };
  /* NVIDIA's E3M2 encoding has sign bit 5, exponent bias 3, and two
   * explicit mantissa bits. All oracle values and products are exact binary
   * fractions, so equality tests are deterministic across host ISAs. */
  const uint8_t e3m2_bits[5] = {UINT8_C(0x08), UINT8_C(0x0c),
                                  UINT8_C(0x2c), UINT8_C(0x10),
                                  UINT8_C(0x28)};
  const uint8_t e2m3_bits[5] = {UINT8_C(0x04), UINT8_C(0x08),
                                  UINT8_C(0x28), UINT8_C(0x10),
                                  UINT8_C(0x24)};
  const float values[5] = {0.5f, 1.0f, -1.0f, 2.0f, -0.5f};
  const uint8_t ue8m0_bits[3] = {UINT8_C(0x7e), UINT8_C(0x7f),
                                   UINT8_C(0x80)};
  const float scales[3] = {0.5f, 1.0f, 2.0f};
  int k_extent = mode == MXFP6_LOOP ? 96 : 32;
  int lda = mode == MXFP6_LOOP ? 104 : 36;
  int ldb = lda;
  int ldc = mode == MXFP6_CHAIN ? 24 : 20;
  int ldd = 24;
  int scale_ld = mode == MXFP6_LOOP ? 4 : 2;
  size_t a_bytes = ((size_t)M * (size_t)lda * 6u + 7u) / 8u;
  size_t b_bytes = ((size_t)N * (size_t)ldb * 6u + 7u) / 8u;
  size_t a_scale_bytes = (size_t)M * (size_t)scale_ld;
  size_t b_scale_bytes = (size_t)N * (size_t)scale_ld;
  size_t c_bytes = (size_t)M * (size_t)ldc * sizeof(float);
  size_t d_bytes = (size_t)M * (size_t)ldd * sizeof(float);
  uint8_t *a = (uint8_t *)malloc(a_bytes);
  uint8_t *b = (uint8_t *)malloc(b_bytes);
  uint8_t *a_scale = (uint8_t *)malloc(a_scale_bytes);
  uint8_t *b_scale = (uint8_t *)malloc(b_scale_bytes);
  float *c = (float *)malloc(c_bytes);
  float *d = (float *)malloc(d_bytes);
  CUdeviceptr da = 0, db = 0, dc = 0, dd = 0, dsa = 0, dsb = 0;
  int32_t runtime_k = k_extent;
  int32_t runtime_lda = lda, runtime_ldb = ldb;
  int32_t runtime_ldc = ldc, runtime_ldd = ldd;
  void *static_parameters[] = {&da, &db, &dc, &dd, &dsa, &dsb};
  void *loop_parameters[] = {&da,          &db,          &dc,          &dd,
                             &dsa,         &dsb,         &runtime_k,   &runtime_lda,
                             &runtime_ldb, &runtime_ldc, &runtime_ldd};
  const char *kernel = mode == MXFP6_DIRECT
                           ? "tensor_mxfp6_m16n16k32"
                           : (mode == MXFP6_CHAIN
                                  ? "tensor_mxfp6_chain3_m16n16k32"
                                  : "tensor_mxfp6_runtime_k_m16n16k32");
  const char *label =
      mode == MXFP6_DIRECT
          ? "dense FP6/UE8M0 block-scale tensor contract"
          : (mode == MXFP6_CHAIN
                 ? "three-tile FP6 accumulator-residency contract"
                 : "runtime-K FP6 accumulator-residency contract");
  const float sentinel = -63456.0f;
  int ok = 0;

  if (!a || !b || !a_scale || !b_scale || !c || !d) {
    fprintf(stderr, "host allocation failed in FP6 test\n");
    goto cleanup;
  }
  memset(a, 0xdb, a_bytes);
  memset(b, 0xdb, b_bytes);
  memset(a_scale, 0xff, a_scale_bytes);
  memset(b_scale, 0xff, b_scale_bytes);
  for (int i = 0; i < M * ldc; i++) c[i] = NAN;
  for (int i = 0; i < M * ldd; i++) d[i] = sentinel;
  for (int row = 0; row < M; row++) {
    for (int inner = 0; inner < k_extent; inner++)
      set_packed_bits(a, (size_t)row * (size_t)lda + (size_t)inner, 6,
                      e3m2_bits[(row * 3 + inner * 2) % 5]);
    for (int chunk = 0; chunk < k_extent / 32; chunk++)
      a_scale[row * scale_ld + chunk] =
          ue8m0_bits[(row * 2 + chunk) % 3];
  }
  for (int col = 0; col < N; col++) {
    for (int inner = 0; inner < k_extent; inner++)
      set_packed_bits(b, (size_t)col * (size_t)ldb + (size_t)inner, 6,
                      e2m3_bits[(col * 2 + inner * 3) % 5]);
    for (int chunk = 0; chunk < k_extent / 32; chunk++)
      b_scale[col * scale_ld + chunk] =
          ue8m0_bits[(col + chunk * 2) % 3];
  }
  for (int row = 0; row < M; row++)
    for (int col = 0; col < N; col++)
      c[row * ldc + col] = (float)((row * N + col) % 13) * 0.125f;

  if (!alloc_device(h, &da, a, a_bytes) ||
      !alloc_device(h, &db, b, b_bytes) ||
      !alloc_device(h, &dc, c, c_bytes) ||
      !alloc_device(h, &dd, d, d_bytes) ||
      !alloc_device(h, &dsa, a_scale, a_scale_bytes) ||
      !alloc_device(h, &dsb, b_scale, b_scale_bytes) ||
      !launch(h, kernel, 1, 1, 1, 32, 1, 1,
              mode == MXFP6_LOOP ? loop_parameters : static_parameters) ||
      !copy_from_device(h, d, dd, d_bytes))
    goto cleanup;

  for (int row = 0; row < M; row++) {
    for (int col = 0; col < ldd; col++) {
      float actual = d[row * ldd + col];
      if (col >= N) {
        if (actual != sentinel) {
          fprintf(stderr,
                  "FP6 overwrote D padding mode=%d row=%d col=%d: %.9g\n",
                  (int)mode, row, col, actual);
          goto cleanup;
        }
        continue;
      }
      float product = 0.0f;
      for (int inner = 0; inner < k_extent; inner++) {
        int chunk = inner / 32;
        product += values[(row * 3 + inner * 2) % 5] *
                   scales[(row * 2 + chunk) % 3] *
                   values[(col * 2 + inner * 3) % 5] *
                   scales[(col + chunk * 2) % 3];
      }
      float expected = c[row * ldc + col] +
                       (mode == MXFP6_CHAIN ? 3.0f : 1.0f) * product;
      if (actual != expected) {
        fprintf(stderr,
                "FP6 mismatch mode=%d row=%d col=%d: got %.9g expected %.9g\n",
                (int)mode, row, col, actual, expected);
        goto cleanup;
      }
    }
  }
  ok = 1;
cleanup:
  free_device(h, &da);
  free_device(h, &db);
  free_device(h, &dc);
  free_device(h, &dd);
  free_device(h, &dsa);
  free_device(h, &dsb);
  free(a);
  free(b);
  free(a_scale);
  free(b_scale);
  free(c);
  free(d);
  printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
  return ok;
}

static int test_tensor_fp8_native(Harness *h) {
  const uint8_t e4m3_bits[5] = {
      UINT8_C(0x30), UINT8_C(0x38), UINT8_C(0xb8),
      UINT8_C(0x40), UINT8_C(0xb0)};
  const uint8_t e5m2_bits[5] = {
      UINT8_C(0x38), UINT8_C(0x3c), UINT8_C(0xbc),
      UINT8_C(0x40), UINT8_C(0xb8)};
  const float values[5] = {0.5f, 1.0f, -1.0f, 2.0f, -0.5f};
  CUdeviceptr da = 0, db = 0, dc = 0, dd = 0;
  int ok = 0;

  {
    enum { M = 16, N = 16, K = 32, LDA = 36, LDB = 40, LDC = 20, LDD = 24 };
    uint8_t a[M * LDA], b[N * LDB];
    float c[M * LDC], d[M * LDD];
    int32_t lda = LDA, ldb = LDB, ldc = LDC, ldd = LDD;
    void *parameters[] = {&da, &db, &dc, &dd, &lda, &ldb, &ldc, &ldd};
    for (int i = 0; i < M * LDA; i++) a[i] = UINT8_C(0x7f);
    for (int i = 0; i < N * LDB; i++) b[i] = UINT8_C(0x7f);
    for (int i = 0; i < M * LDC; i++) c[i] = NAN;
    for (int i = 0; i < M * LDD; i++) d[i] = -12345.0f;
    for (int row = 0; row < M; row++)
      for (int inner = 0; inner < K; inner++)
        a[row * LDA + inner] = e4m3_bits[(row * 3 + inner * 2) % 5];
    for (int col = 0; col < N; col++)
      for (int inner = 0; inner < K; inner++)
        b[col * LDB + inner] = e5m2_bits[(col * 2 + inner * 3) % 5];
    for (int row = 0; row < M; row++)
      for (int col = 0; col < N; col++)
        c[row * LDC + col] = (float)((row * N + col) % 13) * 0.125f;
    if (!alloc_device(h, &da, a, sizeof(a)) ||
        !alloc_device(h, &db, b, sizeof(b)) ||
        !alloc_device(h, &dc, c, sizeof(c)) ||
        !alloc_device(h, &dd, d, sizeof(d)) ||
        !launch(h, "tensor_fp8_m16n16k32", 1, 1, 1, 32, 1, 1, parameters) ||
        !copy_from_device(h, d, dd, sizeof(d)))
      goto cleanup;
    for (int row = 0; row < M; row++) {
      for (int col = 0; col < LDD; col++) {
        float actual = d[row * LDD + col];
        if (col >= N) {
          if (actual != -12345.0f) {
            fprintf(stderr, "FP8 standard tile overwrote padding row=%d col=%d\n",
                    row, col);
            goto cleanup;
          }
          continue;
        }
        float expected = c[row * LDC + col];
        for (int inner = 0; inner < K; inner++)
          expected += values[(row * 3 + inner * 2) % 5] *
                      values[(col * 2 + inner * 3) % 5];
        if (actual != expected) {
          fprintf(stderr,
                  "FP8 standard tile mismatch row=%d col=%d: got %.9g expected %.9g\n",
                  row, col, actual, expected);
          goto cleanup;
        }
      }
    }
    free_device(h, &da);
    free_device(h, &db);
    free_device(h, &dc);
    free_device(h, &dd);
  }

  {
    /* A and B are both stored transposed. A is column-major (KxM), B is
     * row-major (NxK), and C/D are column-major. The 32x24 result forces six
     * backend-owned m16n8 native subtiles. */
    enum { M = 32, N = 24, K = 16, LDA = 20, LDB = 20, LDC = 36, LDD = 40 };
    uint8_t a[M * LDA], b[N * LDB];
    float c[N * LDC], d[N * LDD];
    int32_t lda = LDA, ldb = LDB, ldc = LDC, ldd = LDD;
    void *parameters[] = {&da, &db, &dc, &dd, &lda, &ldb, &ldc, &ldd};
    for (int i = 0; i < M * LDA; i++) a[i] = UINT8_C(0x7f);
    for (int i = 0; i < N * LDB; i++) b[i] = UINT8_C(0x7f);
    for (int i = 0; i < N * LDC; i++) c[i] = NAN;
    for (int i = 0; i < N * LDD; i++) d[i] = -23456.0f;
    for (int row = 0; row < M; row++)
      for (int inner = 0; inner < K; inner++)
        a[row * LDA + inner] = e5m2_bits[(row * 4 + inner) % 5];
    for (int col = 0; col < N; col++)
      for (int inner = 0; inner < K; inner++)
        b[col * LDB + inner] = e4m3_bits[(col * 3 + inner * 2) % 5];
    for (int col = 0; col < N; col++)
      for (int row = 0; row < M; row++)
        c[col * LDC + row] = (float)((row * N + col) % 17) * 0.0625f;
    if (!alloc_device(h, &da, a, sizeof(a)) ||
        !alloc_device(h, &db, b, sizeof(b)) ||
        !alloc_device(h, &dc, c, sizeof(c)) ||
        !alloc_device(h, &dd, d, sizeof(d)) ||
        !launch(h, "tensor_fp8_m32n24k16_transposed", 1, 1, 1, 32, 1, 1,
                parameters) ||
        !copy_from_device(h, d, dd, sizeof(d)))
      goto cleanup;
    for (int col = 0; col < N; col++) {
      for (int row = 0; row < LDD; row++) {
        float actual = d[col * LDD + row];
        if (row >= M) {
          if (actual != -23456.0f) {
            fprintf(stderr,
                    "FP8 transposed tile overwrote padding row=%d col=%d\n",
                    row, col);
            goto cleanup;
          }
          continue;
        }
        float expected = c[col * LDC + row];
        for (int inner = 0; inner < K; inner++)
          expected += values[(row * 4 + inner) % 5] *
                      values[(col * 3 + inner * 2) % 5];
        if (actual != expected) {
          fprintf(stderr,
                  "FP8 transposed tile mismatch row=%d col=%d: got %.9g expected %.9g\n",
                  row, col, actual, expected);
          goto cleanup;
        }
      }
    }
  }

  ok = 1;
cleanup:
  free_device(h, &da);
  free_device(h, &db);
  free_device(h, &dc);
  free_device(h, &dd);
  printf("[%s] native mixed-FP8 whole-tile/layout/transpose contract\n",
         ok ? "PASS" : "FAIL");
  return ok;
}

static int test_tensor_fp8_chain_resident(Harness *h) {
  enum { M = 16, N = 16, K = 32, LDA = 36, LDB = 40, LDC = 20, LDD = 24 };
  const uint8_t e4m3_bits[5] = {
      UINT8_C(0x30), UINT8_C(0x38), UINT8_C(0xb8),
      UINT8_C(0x40), UINT8_C(0xb0)};
  const uint8_t e5m2_bits[5] = {
      UINT8_C(0x38), UINT8_C(0x3c), UINT8_C(0xbc),
      UINT8_C(0x40), UINT8_C(0xb8)};
  const float values[5] = {0.5f, 1.0f, -1.0f, 2.0f, -0.5f};
  uint8_t a[M * LDA], b[N * LDB];
  float c[M * LDC], d[M * LDD];
  CUdeviceptr da = 0, db = 0, dc = 0, dd = 0;
  int32_t lda = LDA, ldb = LDB, ldc = LDC, ldd = LDD;
  void *parameters[] = {&da, &db, &dc, &dd, &lda, &ldb, &ldc, &ldd};
  int ok = 0;

  for (int i = 0; i < M * LDA; i++) a[i] = UINT8_C(0x7f);
  for (int i = 0; i < N * LDB; i++) b[i] = UINT8_C(0x7f);
  for (int i = 0; i < M * LDC; i++) c[i] = NAN;
  for (int i = 0; i < M * LDD; i++) d[i] = -34567.0f;
  for (int row = 0; row < M; row++)
    for (int inner = 0; inner < K; inner++)
      a[row * LDA + inner] = e4m3_bits[(row * 3 + inner * 2) % 5];
  for (int col = 0; col < N; col++)
    for (int inner = 0; inner < K; inner++)
      b[col * LDB + inner] = e5m2_bits[(col * 2 + inner * 3) % 5];
  for (int row = 0; row < M; row++)
    for (int col = 0; col < N; col++)
      c[row * LDC + col] = (float)((row * N + col) % 13) * 0.125f;

  if (!alloc_device(h, &da, a, sizeof(a)) ||
      !alloc_device(h, &db, b, sizeof(b)) ||
      !alloc_device(h, &dc, c, sizeof(c)) ||
      !alloc_device(h, &dd, d, sizeof(d)) ||
      !launch(h, "tensor_fp8_chain4_m16n16k32", 1, 1, 1, 32, 1, 1,
              parameters) ||
      !copy_from_device(h, d, dd, sizeof(d)))
    goto cleanup;
  for (int row = 0; row < M; row++) {
    for (int col = 0; col < LDD; col++) {
      float actual = d[row * LDD + col];
      if (col >= N) {
        if (actual != -34567.0f) {
          fprintf(stderr,
                  "FP8 resident chain overwrote padding row=%d col=%d\n",
                  row, col);
          goto cleanup;
        }
        continue;
      }
      float product = 0.0f;
      for (int inner = 0; inner < K; inner++)
        product += values[(row * 3 + inner * 2) % 5] *
                   values[(col * 2 + inner * 3) % 5];
      float expected = c[row * LDC + col] + 4.0f * product;
      if (actual != expected) {
        fprintf(stderr,
                "FP8 resident chain mismatch row=%d col=%d: got %.9g expected %.9g\n",
                row, col, actual, expected);
        goto cleanup;
      }
    }
  }
  ok = 1;
cleanup:
  free_device(h, &da);
  free_device(h, &db);
  free_device(h, &dc);
  free_device(h, &dd);
  printf("[%s] four-tile native FP8 accumulator-residency contract\n",
         ok ? "PASS" : "FAIL");
  return ok;
}

static int test_tensor_fp8_loop_resident(Harness *h) {
  enum { M = 16, N = 16, K = 96, LDA = 100, LDB = 104, LDC = 20, LDD = 24 };
  const uint8_t e4m3_bits[5] = {
      UINT8_C(0x30), UINT8_C(0x38), UINT8_C(0xb8),
      UINT8_C(0x40), UINT8_C(0xb0)};
  const uint8_t e5m2_bits[5] = {
      UINT8_C(0x38), UINT8_C(0x3c), UINT8_C(0xbc),
      UINT8_C(0x40), UINT8_C(0xb8)};
  const float values[5] = {0.5f, 1.0f, -1.0f, 2.0f, -0.5f};
  uint8_t a[M * LDA], b[N * LDB];
  float c[M * LDC], d[M * LDD];
  CUdeviceptr da = 0, db = 0, dc = 0, dd = 0;
  int32_t k = K, lda = LDA, ldb = LDB, ldc = LDC, ldd = LDD;
  void *parameters[] = {
      &da, &db, &dc, &dd, &k, &lda, &ldb, &ldc, &ldd};
  int ok = 0;

  for (int i = 0; i < M * LDA; i++) a[i] = UINT8_C(0x7f);
  for (int i = 0; i < N * LDB; i++) b[i] = UINT8_C(0x7f);
  for (int i = 0; i < M * LDC; i++) c[i] = NAN;
  for (int i = 0; i < M * LDD; i++) d[i] = -45678.0f;
  for (int row = 0; row < M; row++)
    for (int inner = 0; inner < K; inner++)
      a[row * LDA + inner] = e4m3_bits[(row * 3 + inner * 2) % 5];
  for (int col = 0; col < N; col++)
    for (int inner = 0; inner < K; inner++)
      b[col * LDB + inner] = e5m2_bits[(col * 2 + inner * 3) % 5];
  for (int row = 0; row < M; row++)
    for (int col = 0; col < N; col++)
      c[row * LDC + col] = (float)((row * N + col) % 11) * 0.125f;

  if (!alloc_device(h, &da, a, sizeof(a)) ||
      !alloc_device(h, &db, b, sizeof(b)) ||
      !alloc_device(h, &dc, c, sizeof(c)) ||
      !alloc_device(h, &dd, d, sizeof(d)) ||
      !launch(h, "tensor_fp8_runtime_k_m16n16k32", 1, 1, 1, 32, 1, 1,
              parameters) ||
      !copy_from_device(h, d, dd, sizeof(d)))
    goto cleanup;
  for (int row = 0; row < M; row++) {
    for (int col = 0; col < LDD; col++) {
      float actual = d[row * LDD + col];
      if (col >= N) {
        if (actual != -45678.0f) {
          fprintf(stderr,
                  "FP8 resident loop overwrote padding row=%d col=%d\n",
                  row, col);
          goto cleanup;
        }
        continue;
      }
      float expected = c[row * LDC + col];
      for (int inner = 0; inner < K; inner++)
        expected += values[(row * 3 + inner * 2) % 5] *
                    values[(col * 2 + inner * 3) % 5];
      if (actual != expected) {
        fprintf(stderr,
                "FP8 resident loop mismatch row=%d col=%d: got %.9g expected %.9g\n",
                row, col, actual, expected);
        goto cleanup;
      }
    }
  }
  ok = 1;
cleanup:
  free_device(h, &da);
  free_device(h, &db);
  free_device(h, &dc);
  free_device(h, &dd);
  printf("[%s] runtime-K native FP8 accumulator-residency contract\n",
         ok ? "PASS" : "FAIL");
  return ok;
}

static int test_gemm_full_tiles_f16_f32(Harness *h) {
  enum {
    M = 32,
    N = 32,
    K = 64,
    LDA = 72,
    LDB = 80,
    LDC = 36,
    LDD = 40
  };
  uint16_t a[M * LDA], b[N * LDB];
  float c[M * LDC], d[M * LDD];
  CUdeviceptr da = 0, db = 0, dc = 0, dd = 0;
  int32_t m = M, n = N, k = K;
  int32_t lda = LDA, ldb = LDB, ldc = LDC, ldd = LDD;
  void *parameters[] = {&da, &db, &dc, &dd, &m,   &n,   &k, &lda,
                        &ldb, &ldc, &ldd};
  int ok = 0;
  for (int i = 0; i < M * LDA; i++) a[i] = UINT16_C(0x7e00);
  for (int i = 0; i < N * LDB; i++) b[i] = UINT16_C(0x7e00);
  for (int i = 0; i < M * LDC; i++) c[i] = NAN;
  for (int i = 0; i < M * LDD; i++) d[i] = -12345.0f;
  for (int row = 0; row < M; row++) {
    uint16_t value = row < 16 ? UINT16_C(0x3c00) : UINT16_C(0x4000);
    for (int inner = 0; inner < K; inner++) a[row * LDA + inner] = value;
  }
  for (int col = 0; col < N; col++) {
    uint16_t value = col < 16 ? UINT16_C(0x3c00) : UINT16_C(0x4000);
    for (int inner = 0; inner < K; inner++) b[col * LDB + inner] = value;
  }
  for (int row = 0; row < M; row++) {
    for (int col = 0; col < N; col++) {
      c[row * LDC + col] = (float)((row * N + col) % 13) * 0.125f;
    }
  }
  if (!alloc_device(h, &da, a, sizeof(a)) ||
      !alloc_device(h, &db, b, sizeof(b)) ||
      !alloc_device(h, &dc, c, sizeof(c)) ||
      !alloc_device(h, &dd, d, sizeof(d)) ||
      !launch(h, "gemm_full_tiles_f16_f32", N / 16, M / 16, 1, 32, 1, 1,
              parameters) ||
      !copy_from_device(h, d, dd, sizeof(d)))
    goto cleanup;
  for (int row = 0; row < M; row++) {
    float a_value = row < 16 ? 1.0f : 2.0f;
    for (int col = 0; col < N; col++) {
      float b_value = col < 16 ? 1.0f : 2.0f;
      float expected = a_value * b_value * (float)K + c[row * LDC + col];
      float actual = d[row * LDD + col];
      if (actual != expected) {
        fprintf(stderr,
                "gemm_full_tiles mismatch row=%d col=%d: got %.9g expected %.9g\n",
                row, col, actual, expected);
        goto cleanup;
      }
    }
    for (int col = N; col < LDD; col++) {
      if (d[row * LDD + col] != -12345.0f) {
        fprintf(stderr,
                "gemm_full_tiles overwrote D padding row=%d col=%d: %.9g\n",
                row, col, d[row * LDD + col]);
        goto cleanup;
      }
    }
  }
  /* The outer shape/K guards share the source-level join with the loop exit.
   * A residency transform must split only the loop edge: when the guard fails,
   * no accumulator exists and D must remain entirely untouched. */
  for (int i = 0; i < M * LDD; i++) d[i] = -23456.0f;
  k = 15;
  if (!copy_to_device(h, dd, d, sizeof(d)) ||
      !launch(h, "gemm_full_tiles_f16_f32", N / 16, M / 16, 1, 32, 1, 1,
              parameters) ||
      !copy_from_device(h, d, dd, sizeof(d)))
    goto cleanup;
  for (int i = 0; i < M * LDD; i++) {
    if (d[i] != -23456.0f) {
      fprintf(stderr,
              "gemm_full_tiles guard committed an undefined accumulator at %d: %.9g\n",
              i, d[i]);
      goto cleanup;
    }
  }
  ok = 1;
cleanup:
  free_device(h, &da);
  free_device(h, &db);
  free_device(h, &dc);
  free_device(h, &dd);
  printf("[%s] 2-D runtime-K resident f16/f32 GEMM and guard contract\n",
         ok ? "PASS" : "FAIL");
  return ok;
}

static int test_gemm_tail_complete_f16_f32(Harness *h) {
  enum {
    MAX_M = 19,
    MAX_N = 23,
    MAX_K = 21,
    LDA = 24,
    LDB = 24,
    LDC = 28,
    LDD = 32
  };
  const uint16_t a_bits[5] = {
      UINT16_C(0xc000), UINT16_C(0xbc00), UINT16_C(0x0000),
      UINT16_C(0x3c00), UINT16_C(0x4000)};
  const float a_values[5] = {-2.0f, -1.0f, 0.0f, 1.0f, 2.0f};
  const uint16_t b_bits[4] = {
      UINT16_C(0xbc00), UINT16_C(0x0000),
      UINT16_C(0x3c00), UINT16_C(0x4000)};
  const float b_values[4] = {-1.0f, 0.0f, 1.0f, 2.0f};
  const int phase_shape[2][3] = {{MAX_M, MAX_N, MAX_K}, {16, 16, 7}};
  uint16_t a[MAX_M * LDA], b[MAX_N * LDB];
  float c[MAX_M * LDC], d[MAX_M * LDD];
  CUdeviceptr da = 0, db = 0, dc = 0, dd = 0;
  int32_t m = 0, n = 0, k = 0;
  int32_t lda = LDA, ldb = LDB, ldc = LDC, ldd = LDD;
  void *parameters[] = {&da, &db, &dc, &dd, &m,   &n,   &k, &lda,
                        &ldb, &ldc, &ldd};
  int ok = 0;

  for (int i = 0; i < MAX_M * LDA; i++) a[i] = UINT16_C(0x7e00);
  for (int i = 0; i < MAX_N * LDB; i++) b[i] = UINT16_C(0x7e00);
  for (int i = 0; i < MAX_M * LDC; i++) c[i] = NAN;
  for (int row = 0; row < MAX_M; row++) {
    for (int inner = 0; inner < MAX_K; inner++)
      a[row * LDA + inner] = a_bits[(row * 3 + inner * 2) % 5];
  }
  for (int col = 0; col < MAX_N; col++) {
    for (int inner = 0; inner < MAX_K; inner++)
      b[col * LDB + inner] = b_bits[(col * 5 + inner * 3) % 4];
  }
  for (int row = 0; row < MAX_M; row++) {
    for (int col = 0; col < MAX_N; col++)
      c[row * LDC + col] = (float)((row * MAX_N + col) % 11) * 0.125f;
  }
  if (!alloc_device(h, &da, a, sizeof(a)) ||
      !alloc_device(h, &db, b, sizeof(b)) ||
      !alloc_device(h, &dc, c, sizeof(c)) ||
      !alloc_device(h, &dd, NULL, sizeof(d)))
    goto cleanup;

  for (int phase = 0; phase < 2; phase++) {
    m = phase_shape[phase][0];
    n = phase_shape[phase][1];
    k = phase_shape[phase][2];
    for (int i = 0; i < MAX_M * LDD; i++) d[i] = -12345.0f;
    if (!copy_to_device(h, dd, d, sizeof(d)) ||
        !launch(h, "gemm_tail_complete_f16_f32",
                (unsigned)(n + 15) / 16, (unsigned)(m + 15) / 16, 1,
                32, 1, 1, parameters) ||
        !copy_from_device(h, d, dd, sizeof(d)))
      goto cleanup;
    for (int row = 0; row < MAX_M; row++) {
      for (int col = 0; col < LDD; col++) {
        const float actual = d[row * LDD + col];
        if (row >= m || col >= n) {
          if (actual != -12345.0f) {
            fprintf(stderr,
                    "tail GEMM phase=%d overwrote padding row=%d col=%d: %.9g\n",
                    phase, row, col, actual);
            goto cleanup;
          }
          continue;
        }
        float expected = c[row * LDC + col];
        for (int inner = 0; inner < k; inner++) {
          expected += a_values[(row * 3 + inner * 2) % 5] *
                      b_values[(col * 5 + inner * 3) % 4];
        }
        if (actual != expected) {
          fprintf(stderr,
                  "tail GEMM phase=%d row=%d col=%d: got %.9g expected %.9g\n",
                  phase, row, col, actual, expected);
          goto cleanup;
        }
      }
    }
  }
  ok = 1;
cleanup:
  free_device(h, &da);
  free_device(h, &db);
  free_device(h, &dc);
  free_device(h, &dd);
  printf("[%s] tail-complete tensor/scalar f16/f32 GEMM contract\n",
         ok ? "PASS" : "FAIL");
  return ok;
}

static int test_tensor_chain4(Harness *h) {
  /* WMMA f16 strides are multiples of 8 elements and f32 strides multiples
   * of 4. Keep every matrix unequal and padded without violating that device
   * contract. */
  enum { M = 16, N = 16, K = 64, LDA = 72, LDB = 80, LDC = 20, LDD = 24 };
  uint16_t a[M * LDA], b[N * LDB];
  float c[M * LDC], d[M * LDD];
  CUdeviceptr da = 0, db = 0, dc = 0, dd = 0;
  int32_t lda = LDA, ldb = LDB, ldc = LDC, ldd = LDD;
  void *parameters[] = {&da, &db, &dc, &dd, &lda, &ldb, &ldc, &ldd};
  const uint16_t tile_values[4] = {
      UINT16_C(0x3c00), UINT16_C(0x4000),
      UINT16_C(0x4200), UINT16_C(0x4400)}; /* 1, 2, 3, 4 */
  int ok = 0;

  for (int i = 0; i < M * LDA; i++) a[i] = UINT16_C(0x7e00);
  for (int i = 0; i < N * LDB; i++) b[i] = UINT16_C(0x7e00);
  for (int i = 0; i < M * LDC; i++) c[i] = NAN;
  for (int i = 0; i < M * LDD; i++) d[i] = -12345.0f;
  for (int row = 0; row < M; row++) {
    uint16_t row_scale = row < 8 ? UINT16_C(0x3c00) : UINT16_C(0x4000);
    for (int inner = 0; inner < K; inner++) {
      /* Keep each operand individually representable as binary16 while making
       * every K tile contribute a distinct exact amount. */
      a[row * LDA + inner] =
          row < 8 ? tile_values[inner / 16]
                  : (inner / 16 == 0 ? row_scale
                                     : (uint16_t)(tile_values[inner / 16] +
                                                  UINT16_C(0x0400)));
    }
  }
  for (int col = 0; col < N; col++) {
    uint16_t value = col < 8 ? UINT16_C(0x3c00) : UINT16_C(0x4000);
    for (int inner = 0; inner < K; inner++) b[col * LDB + inner] = value;
  }
  for (int row = 0; row < M; row++) {
    for (int col = 0; col < N; col++) {
      c[row * LDC + col] = (float)((row * N + col) % 11) * 0.125f;
    }
  }
  if (!alloc_device(h, &da, a, sizeof(a)) ||
      !alloc_device(h, &db, b, sizeof(b)) ||
      !alloc_device(h, &dc, c, sizeof(c)) ||
      !alloc_device(h, &dd, d, sizeof(d)) ||
      !launch(h, "tensor_chain4", 1, 1, 1, 32, 1, 1, parameters) ||
      !copy_from_device(h, d, dd, sizeof(d)))
    goto cleanup;
  for (int row = 0; row < M; row++) {
    float a_tile_sum = row < 8 ? 10.0f : 20.0f;
    for (int col = 0; col < N; col++) {
      float b_value = col < 8 ? 1.0f : 2.0f;
      float expected = 16.0f * a_tile_sum * b_value +
                       c[row * LDC + col];
      float actual = d[row * LDD + col];
      if (actual != expected) {
        fprintf(stderr,
                "tensor_chain4 mismatch row=%d col=%d: got %.9g expected %.9g\n",
                row, col, actual, expected);
        goto cleanup;
      }
    }
    for (int col = N; col < LDD; col++) {
      if (d[row * LDD + col] != -12345.0f) {
        fprintf(stderr,
                "tensor_chain4 overwrote D padding row=%d col=%d: %.9g\n",
                row, col, d[row * LDD + col]);
        goto cleanup;
      }
    }
  }
  ok = 1;
cleanup:
  free_device(h, &da);
  free_device(h, &db);
  free_device(h, &dc);
  free_device(h, &dd);
  printf("[%s] four-tile accumulator-resident tensor chain contract\n",
         ok ? "PASS" : "FAIL");
  return ok;
}

static int test_tensor_pipeline_f16_f32(Harness *h) {
  enum { M = 16, N = 16, K = 16, TILE = 256, TILES = 2 };
  uint16_t a[TILES * TILE], b[TILES * TILE];
  float c[M * N], d[M * N];
  CUdeviceptr da = 0, db = 0, dc = 0, dd = 0;
  void *parameters[] = {&da, &db, &dc, &dd};
  const uint16_t a_values[2][2] = {
      {UINT16_C(0x3c00), UINT16_C(0x4000)},
      {UINT16_C(0x4200), UINT16_C(0x4400)}}; /* (1,2), (3,4) */
  const uint16_t b_values[2][2] = {
      {UINT16_C(0x3c00), UINT16_C(0x4000)},
      {UINT16_C(0x4000), UINT16_C(0x4200)}}; /* (1,2), (2,3) */
  int ok = 0;

  for (int tile = 0; tile < TILES; tile++) {
    for (int row = 0; row < M; row++) {
      for (int inner = 0; inner < K; inner++)
        a[tile * TILE + row * K + inner] = a_values[tile][row >= 8];
    }
    for (int col = 0; col < N; col++) {
      for (int inner = 0; inner < K; inner++)
        b[tile * TILE + col * K + inner] = b_values[tile][col >= 8];
    }
  }
  for (int row = 0; row < M; row++) {
    for (int col = 0; col < N; col++) {
      c[row * N + col] = (float)((row * N + col) % 17) * 0.0625f;
      d[row * N + col] = -12345.0f;
    }
  }
  if (!alloc_device(h, &da, a, sizeof(a)) ||
      !alloc_device(h, &db, b, sizeof(b)) ||
      !alloc_device(h, &dc, c, sizeof(c)) ||
      !alloc_device(h, &dd, d, sizeof(d)) ||
      !launch(h, "tensor_pipeline_f16_f32", 1, 1, 1, 32, 1, 1,
              parameters) ||
      !copy_from_device(h, d, dd, sizeof(d)))
    goto cleanup;
  for (int row = 0; row < M; row++) {
    const float a0 = row < 8 ? 1.0f : 2.0f;
    const float a1 = row < 8 ? 3.0f : 4.0f;
    for (int col = 0; col < N; col++) {
      const float b0 = col < 8 ? 1.0f : 2.0f;
      const float b1 = col < 8 ? 2.0f : 3.0f;
      const float expected = 16.0f * (a0 * b0 + a1 * b1) +
                             c[row * N + col];
      const float actual = d[row * N + col];
      if (actual != expected) {
        fprintf(stderr,
                "tensor_pipeline mismatch row=%d col=%d: got %.9g expected %.9g\n",
                row, col, actual, expected);
        goto cleanup;
      }
    }
  }
  ok = 1;
cleanup:
  free_device(h, &da);
  free_device(h, &db);
  free_device(h, &dc);
  free_device(h, &dd);
  printf("[%s] double-buffered async-copy tensor residency contract\n",
         ok ? "PASS" : "FAIL");
  return ok;
}

static int test_tensor_pipeline4_f16_f32(Harness *h) {
  enum { M = 16, N = 16, K = 16, TILE = 256, TILES = 4 };
  uint16_t a[TILES * TILE], b[TILES * TILE];
  float c[M * N], d[M * N];
  CUdeviceptr da = 0, db = 0, dc = 0, dd = 0;
  void *parameters[] = {&da, &db, &dc, &dd};
  const uint16_t a_values[TILES][2] = {
      {UINT16_C(0x3c00), UINT16_C(0x4000)}, /* (1,2) */
      {UINT16_C(0x4200), UINT16_C(0x4400)}, /* (3,4) */
      {UINT16_C(0x3c00), UINT16_C(0x4200)}, /* (1,3) */
      {UINT16_C(0x4000), UINT16_C(0x4400)}}; /* (2,4) */
  const uint16_t b_values[TILES][2] = {
      {UINT16_C(0x3c00), UINT16_C(0x4000)}, /* (1,2) */
      {UINT16_C(0x4000), UINT16_C(0x4200)}, /* (2,3) */
      {UINT16_C(0x4200), UINT16_C(0x3c00)}, /* (3,1) */
      {UINT16_C(0x4400), UINT16_C(0x4000)}}; /* (4,2) */
  const float a_numeric[TILES][2] = {
      {1.0f, 2.0f}, {3.0f, 4.0f}, {1.0f, 3.0f}, {2.0f, 4.0f}};
  const float b_numeric[TILES][2] = {
      {1.0f, 2.0f}, {2.0f, 3.0f}, {3.0f, 1.0f}, {4.0f, 2.0f}};
  int ok = 0;

  for (int tile = 0; tile < TILES; tile++) {
    for (int row = 0; row < M; row++) {
      for (int inner = 0; inner < K; inner++)
        a[tile * TILE + row * K + inner] = a_values[tile][row >= 8];
    }
    for (int col = 0; col < N; col++) {
      for (int inner = 0; inner < K; inner++)
        b[tile * TILE + col * K + inner] = b_values[tile][col >= 8];
    }
  }
  for (int row = 0; row < M; row++) {
    for (int col = 0; col < N; col++) {
      c[row * N + col] = (float)((row * N + col) % 19) * 0.03125f;
      d[row * N + col] = -12345.0f;
    }
  }
  if (!alloc_device(h, &da, a, sizeof(a)) ||
      !alloc_device(h, &db, b, sizeof(b)) ||
      !alloc_device(h, &dc, c, sizeof(c)) ||
      !alloc_device(h, &dd, d, sizeof(d)) ||
      !launch(h, "tensor_pipeline4_f16_f32", 1, 1, 1, 32, 1, 1,
              parameters) ||
      !copy_from_device(h, d, dd, sizeof(d)))
    goto cleanup;
  for (int row = 0; row < M; row++) {
    for (int col = 0; col < N; col++) {
      float products = 0.0f;
      for (int tile = 0; tile < TILES; tile++)
        products += a_numeric[tile][row >= 8] *
                    b_numeric[tile][col >= 8];
      const float expected = 16.0f * products + c[row * N + col];
      const float actual = d[row * N + col];
      if (actual != expected) {
        fprintf(stderr,
                "tensor_pipeline4 mismatch row=%d col=%d: got %.9g "
                "expected %.9g\n",
                row, col, actual, expected);
        goto cleanup;
      }
    }
  }
  ok = 1;
cleanup:
  free_device(h, &da);
  free_device(h, &db);
  free_device(h, &dc);
  free_device(h, &dd);
  printf("[%s] four-stage async-copy tensor residency contract\n",
         ok ? "PASS" : "FAIL");
  return ok;
}

static int test_atomic_litmus(Harness *h) {
  enum {
    BLOCKS = 4,
    THREADS = 64,
    N = BLOCKS * THREADS,
    STATE32_COUNT = 16 + BLOCKS * 3,
    STATE64_COUNT = 16
  };
  uint32_t state32[STATE32_COUNT] = {0};
  uint64_t state64[STATE64_COUNT] = {0};
  uint32_t tickets32[N], cas32[N], shared32[BLOCKS];
  uint64_t tickets64[N], cas64[N], shared64[BLOCKS];
  unsigned char seen32[N], seen64[N];
  CUdeviceptr dstate32 = 0, dstate64 = 0, dtickets32 = 0, dtickets64 = 0;
  CUdeviceptr dcas32 = 0, dcas64 = 0, dshared32 = 0, dshared64 = 0;
  void *parameters[] = {&dstate32,  &dstate64, &dtickets32, &dtickets64,
                        &dcas32,    &dcas64,   &dshared32,  &dshared64};
  int zero_cas32 = 0, zero_cas64 = 0;
  int ok = 0;

  state32[1] = N;
  state32[2] = UINT32_MAX;
  state32[4] = UINT32_MAX;
  state32[7] = UINT32_MAX;
  state64[1] = N;
  state64[2] = UINT64_MAX;
  state64[4] = UINT64_MAX;
  state64[7] = UINT64_MAX;
  for (int i = 0; i < N; i++) {
    tickets32[i] = cas32[i] = UINT32_MAX;
    tickets64[i] = cas64[i] = UINT64_MAX;
  }
  memset(shared32, 0, sizeof(shared32));
  memset(shared64, 0, sizeof(shared64));
  memset(seen32, 0, sizeof(seen32));
  memset(seen64, 0, sizeof(seen64));

  if (!alloc_device(h, &dstate32, state32, sizeof(state32)) ||
      !alloc_device(h, &dstate64, state64, sizeof(state64)) ||
      !alloc_device(h, &dtickets32, tickets32, sizeof(tickets32)) ||
      !alloc_device(h, &dtickets64, tickets64, sizeof(tickets64)) ||
      !alloc_device(h, &dcas32, cas32, sizeof(cas32)) ||
      !alloc_device(h, &dcas64, cas64, sizeof(cas64)) ||
      !alloc_device(h, &dshared32, shared32, sizeof(shared32)) ||
      !alloc_device(h, &dshared64, shared64, sizeof(shared64)) ||
      !launch(h, "atomic_litmus", BLOCKS, 1, 1, THREADS, 1, 1,
              parameters) ||
      !copy_from_device(h, state32, dstate32, sizeof(state32)) ||
      !copy_from_device(h, state64, dstate64, sizeof(state64)) ||
      !copy_from_device(h, tickets32, dtickets32, sizeof(tickets32)) ||
      !copy_from_device(h, tickets64, dtickets64, sizeof(tickets64)) ||
      !copy_from_device(h, cas32, dcas32, sizeof(cas32)) ||
      !copy_from_device(h, cas64, dcas64, sizeof(cas64)) ||
      !copy_from_device(h, shared32, dshared32, sizeof(shared32)) ||
      !copy_from_device(h, shared64, dshared64, sizeof(shared64)))
    goto cleanup;

  if (state32[0] != N || state32[1] != 0 || state32[2] != 0 ||
      state32[3] != N - 1 || state32[4] != 0 ||
      state32[5] != UINT32_MAX || state32[6] != 0 || state32[7] >= N ||
      state32[8] == 0 || state32[8] > N || state64[0] != N ||
      state64[1] != 0 || state64[2] != 0 || state64[3] != N - 1 ||
      state64[4] != 0 || state64[5] != UINT64_MAX || state64[6] != 0 ||
      state64[7] >= N || state64[8] == 0 || state64[8] > N) {
    fprintf(stderr,
            "atomic final-state mismatch: u32=(%u %u %u %u %08x %08x %08x %u %u) u64=(%llu %llu %llu %llu %llx %llx %llx %llu %llu)\n",
            state32[0], state32[1], state32[2], state32[3], state32[4],
            state32[5], state32[6], state32[7], state32[8],
            (unsigned long long)state64[0],
            (unsigned long long)state64[1],
            (unsigned long long)state64[2],
            (unsigned long long)state64[3],
            (unsigned long long)state64[4],
            (unsigned long long)state64[5],
            (unsigned long long)state64[6],
            (unsigned long long)state64[7],
            (unsigned long long)state64[8]);
    goto cleanup;
  }
  for (int i = 0; i < N; i++) {
    if (tickets32[i] >= N || tickets64[i] >= N ||
        seen32[tickets32[i]] || seen64[tickets64[i]]) {
      fprintf(stderr,
              "atomic returned-old permutation mismatch i=%d u32=%u u64=%llu\n",
              i, tickets32[i], (unsigned long long)tickets64[i]);
      goto cleanup;
    }
    seen32[tickets32[i]] = 1;
    seen64[tickets64[i]] = 1;
    if (cas32[i] == 0)
      zero_cas32++;
    else if (cas32[i] != state32[8]) {
      fprintf(stderr, "u32 CAS observed inconsistent winner at i=%d\n", i);
      goto cleanup;
    }
    if (cas64[i] == 0)
      zero_cas64++;
    else if (cas64[i] != state64[8]) {
      fprintf(stderr, "u64 CAS observed inconsistent winner at i=%d\n", i);
      goto cleanup;
    }
  }
  if (zero_cas32 != 1 || zero_cas64 != 1) {
    fprintf(stderr, "CAS winner count mismatch: u32=%d u64=%d\n",
            zero_cas32, zero_cas64);
    goto cleanup;
  }
  for (int block = 0; block < BLOCKS; block++) {
    int message = 16 + block * 3;
    uint32_t expected_payload = (uint32_t)(0x5a00 + block);
    if (shared32[block] != THREADS || shared64[block] != THREADS ||
        state32[message] != 1 || state32[message + 1] != expected_payload ||
        state32[message + 2] != expected_payload) {
      fprintf(stderr,
              "workgroup/message atomic mismatch block=%d shared=%u/%llu message=%u/%u/%u expected=%u\n",
              block, shared32[block], (unsigned long long)shared64[block],
              state32[message], state32[message + 1],
              state32[message + 2], expected_payload);
      goto cleanup;
    }
  }
  ok = 1;
cleanup:
  free_device(h, &dstate32);
  free_device(h, &dstate64);
  free_device(h, &dtickets32);
  free_device(h, &dtickets64);
  free_device(h, &dcas32);
  free_device(h, &dcas64);
  free_device(h, &dshared32);
  free_device(h, &dshared64);
  printf("[%s] contended u32/u64 atomics and release/acquire litmus\n",
         ok ? "PASS" : "FAIL");
  return ok;
}

static int test_narrow_scalar_abi(Harness *h) {
  int8_t i8 = -7;
  uint8_t u8 = 250;
  int16_t i16 = -1234;
  uint16_t u16 = 60000;
  uint8_t flag = 1;
  int32_t output = 0;
  const int32_t expected = (int32_t)i8 + (int32_t)u8 + (int32_t)i16 +
                           (int32_t)u16;
  CUdeviceptr doutput = 0;
  void *parameters[] = {&i8, &u8, &i16, &u16, &flag, &doutput};
  int ok = 0;
  if (!alloc_device(h, &doutput, &output, sizeof(output)) ||
      !launch(h, "narrow_scalar_abi", 1, 1, 1, 1, 1, 1, parameters) ||
      !copy_from_device(h, &output, doutput, sizeof(output)))
    goto cleanup;
  if (output != expected) {
    fprintf(stderr, "scalar ABI mismatch: got %d expected %d\n", output,
            expected);
    goto cleanup;
  }
  ok = 1;
cleanup:
  free_device(h, &doutput);
  printf("[%s] natural-width scalar launch ABI\n", ok ? "PASS" : "FAIL");
  return ok;
}

static int encode_tiled_tensor_map(Harness *h, CUtensorMap *map,
                                   uint32_t rank, CUdeviceptr global_address,
                                   const uint64_t *global_dimensions,
                                   const uint64_t *global_strides,
                                   const uint32_t *box_dimensions,
                                   const uint32_t *element_strides) {
  CUresult rc;
  if (!h->api.cuTensorMapEncodeTiled) {
    fprintf(stderr, "CUDA Driver does not export cuTensorMapEncodeTiled\n");
    return 0;
  }
  memset(map, 0, sizeof(*map));
  rc = h->api.cuTensorMapEncodeTiled(
      map, CU_TENSOR_MAP_DATA_TYPE_FLOAT32, rank,
      (void *)(uintptr_t)global_address, global_dimensions, global_strides,
      box_dimensions, element_strides, CU_TENSOR_MAP_INTERLEAVE_NONE,
      CU_TENSOR_MAP_SWIZZLE_NONE, CU_TENSOR_MAP_L2_PROMOTION_NONE,
      CU_TENSOR_MAP_FLOAT_OOB_FILL_NONE);
  if (rc != CUDA_SUCCESS) {
    report_cuda_error(h, "cuTensorMapEncodeTiled", rc);
    return 0;
  }
  return 1;
}

static int test_tensor_transfer_load_2d(Harness *h) {
  enum { WIDTH = 128, HEIGHT = 96, TILE_X = 16, TILE_Y = 16 };
  const size_t input_count = (size_t)WIDTH * HEIGHT;
  float *input = (float *)malloc(sizeof(float) * input_count);
  float output[TILE_X * TILE_Y];
  CUtensorMap map;
  CUdeviceptr dinput = 0, dmap = 0, doutput = 0;
  const uint64_t dimensions[2] = {WIDTH, HEIGHT};
  const uint64_t strides[1] = {WIDTH * sizeof(float)};
  const uint32_t box[2] = {TILE_X, TILE_Y};
  const uint32_t element_strides[2] = {1, 1};
  int32_t x = WIDTH - TILE_X / 2;
  int32_t y = HEIGHT - TILE_Y / 2;
  void *parameters[] = {&dinput, &dmap, &doutput, &x, &y};
  int ok = 0;

  if (!input) goto cleanup;
  for (size_t index = 0; index < input_count; index++)
    input[index] = (float)(index / WIDTH) * 1000.0f +
                   (float)(index % WIDTH) + 0.25f;
  for (size_t index = 0; index < TILE_X * TILE_Y; index++)
    output[index] = -9999.0f;
  if (!alloc_device(h, &dinput, input, sizeof(float) * input_count) ||
      !alloc_device(h, &doutput, output, sizeof(output)) ||
      !encode_tiled_tensor_map(h, &map, 2, dinput, dimensions, strides, box,
                               element_strides) ||
      !alloc_device(h, &dmap, &map, sizeof(map)) ||
      !launch(h, "tensor_transfer_load_2d", 1, 1, 1, 256, 1, 1,
              parameters) ||
      !copy_from_device(h, output, doutput, sizeof(output)))
    goto cleanup;

  for (int local_y = 0; local_y < TILE_Y; local_y++) {
    for (int local_x = 0; local_x < TILE_X; local_x++) {
      int global_x = x + local_x;
      int global_y = y + local_y;
      float expected = global_x >= 0 && global_x < WIDTH && global_y >= 0 &&
                               global_y < HEIGHT
                           ? input[(size_t)global_y * WIDTH + global_x]
                           : 0.0f;
      float actual = output[local_y * TILE_X + local_x];
      if (actual != expected) {
        fprintf(stderr,
                "TMA load mismatch local=(%d,%d) global=(%d,%d): got %.9g expected %.9g\n",
                local_x, local_y, global_x, global_y, actual, expected);
        goto cleanup;
      }
    }
  }
  ok = 1;
cleanup:
  free_device(h, &dinput);
  free_device(h, &dmap);
  free_device(h, &doutput);
  free(input);
  printf("[%s] rank-2 TMA load with zero-filled boundaries\n",
         ok ? "PASS" : "FAIL");
  return ok;
}

static int test_tensor_transfer_store_5d(Harness *h) {
  enum {
    D0 = 32,
    D1 = 24,
    D2 = 8,
    D3 = 4,
    D4 = 3,
    T0 = 4,
    T1 = 4,
    T2 = 2,
    T3 = 2,
    T4 = 2
  };
  const size_t element_count = (size_t)D0 * D1 * D2 * D3 * D4;
  const float untouched = -777.0f;
  float *output = (float *)malloc(sizeof(float) * element_count);
  CUtensorMap map;
  CUdeviceptr doutput = 0, dmap = 0;
  const uint64_t dimensions[5] = {D0, D1, D2, D3, D4};
  const uint64_t strides[4] = {
      D0 * sizeof(float),
      (uint64_t)D0 * D1 * sizeof(float),
      (uint64_t)D0 * D1 * D2 * sizeof(float),
      (uint64_t)D0 * D1 * D2 * D3 * sizeof(float)};
  const uint32_t box[5] = {T0, T1, T2, T3, T4};
  const uint32_t element_strides[5] = {1, 1, 1, 1, 1};
  int32_t c0 = D0 - 2, c1 = D1 - 2, c2 = D2 - 1, c3 = D3 - 1,
          c4 = D4 - 1;
  void *parameters[] = {&doutput, &dmap, &c0, &c1, &c2, &c3, &c4};
  int ok = 0;

  if (!output) goto cleanup;
  for (size_t index = 0; index < element_count; index++)
    output[index] = untouched;
  if (!alloc_device(h, &doutput, output, sizeof(float) * element_count) ||
      !encode_tiled_tensor_map(h, &map, 5, doutput, dimensions, strides, box,
                               element_strides) ||
      !alloc_device(h, &dmap, &map, sizeof(map)) ||
      !launch(h, "tensor_transfer_store_5d", 1, 1, 1, 128, 1, 1,
              parameters) ||
      !copy_from_device(h, output, doutput,
                        sizeof(float) * element_count))
    goto cleanup;

  for (int g4 = 0; g4 < D4; g4++) {
    for (int g3 = 0; g3 < D3; g3++) {
      for (int g2 = 0; g2 < D2; g2++) {
        for (int g1 = 0; g1 < D1; g1++) {
          for (int g0 = 0; g0 < D0; g0++) {
            int l0 = g0 - c0, l1 = g1 - c1, l2 = g2 - c2,
                l3 = g3 - c3, l4 = g4 - c4;
            int in_box = l0 >= 0 && l0 < T0 && l1 >= 0 && l1 < T1 &&
                         l2 >= 0 && l2 < T2 && l3 >= 0 && l3 < T3 &&
                         l4 >= 0 && l4 < T4;
            float expected =
                in_box
                    ? (float)(l0 + T0 * (l1 + T1 * (l2 + T2 * (l3 + T3 * l4))))
                    : untouched;
            size_t index =
                (size_t)g0 + D0 * ((size_t)g1 + D1 * ((size_t)g2 +
                    D2 * ((size_t)g3 + D3 * (size_t)g4)));
            if (output[index] != expected) {
              fprintf(stderr,
                      "TMA store mismatch global=(%d,%d,%d,%d,%d): got %.9g expected %.9g\n",
                      g0, g1, g2, g3, g4, output[index], expected);
              goto cleanup;
            }
          }
        }
      }
    }
  }
  ok = 1;
cleanup:
  free_device(h, &doutput);
  free_device(h, &dmap);
  free(output);
  printf("[%s] rank-5 TMA store with out-of-bounds discard\n",
         ok ? "PASS" : "FAIL");
  return ok;
}

static int host_is_aarch64(void) {
#if defined(__aarch64__) || defined(_M_ARM64)
  return 1;
#else
  return 0;
#endif
}

int main(int argc, char **argv) {
  Harness h;
  char *ptx = NULL;
  char *mxfp4_ptx = NULL;
  char *mxfp6_ptx = NULL;
  char *tensor_transfer_ptx = NULL;
  const char *mxfp4_path = NULL;
  const char *mxfp6_path = NULL;
  const char *tensor_transfer_path = NULL;
  CUmodule mxfp4_module = NULL;
  CUmodule mxfp6_module = NULL;
  CUmodule tensor_transfer_module = NULL;
  CUdevice device = 0;
  char device_name[256] = {0};
  int major = 0, minor = 0, integrated = 0;
  int require_gb10 = 0;
  int tensor_transfer_only = 0;
  int passed = 0;
  int total = 0;
  CUresult rc;

  memset(&h, 0, sizeof(h));
  if (argc < 2) {
    fprintf(stderr,
            "usage: %s <module.ptx> [--require-gb10] [--mxfp4 <module.ptx>] [--mxfp6 <module.ptx>] [--tensor-transfer-only --tensor-transfer <module.ptx>]\n",
            argv[0]);
    return 2;
  }
  for (int argument = 2; argument < argc; argument++) {
    if (strcmp(argv[argument], "--require-gb10") == 0) {
      require_gb10 = 1;
    } else if (strcmp(argv[argument], "--tensor-transfer-only") == 0) {
      tensor_transfer_only = 1;
    } else if (strcmp(argv[argument], "--mxfp4") == 0 &&
               argument + 1 < argc && !mxfp4_path) {
      mxfp4_path = argv[++argument];
    } else if (strcmp(argv[argument], "--mxfp6") == 0 &&
               argument + 1 < argc && !mxfp6_path) {
      mxfp6_path = argv[++argument];
    } else if (strcmp(argv[argument], "--tensor-transfer") == 0 &&
               argument + 1 < argc && !tensor_transfer_path) {
      tensor_transfer_path = argv[++argument];
    } else {
      fprintf(stderr,
              "usage: %s <module.ptx> [--require-gb10] [--mxfp4 <module.ptx>] [--mxfp6 <module.ptx>] [--tensor-transfer-only --tensor-transfer <module.ptx>]\n",
              argv[0]);
      return 2;
    }
  }
  if (tensor_transfer_path && !tensor_transfer_only) {
    fprintf(stderr,
            "experimental TMA must run alone; add --tensor-transfer-only and do not mix it with the ordinary suite\n");
    return 2;
  }
  if (tensor_transfer_only &&
      (!tensor_transfer_path || mxfp4_path || mxfp6_path)) {
    fprintf(stderr,
            "--tensor-transfer-only requires exactly one --tensor-transfer module and rejects ordinary extension modules\n");
    return 2;
  }
  total = tensor_transfer_only ? 2 : 23;
  if (!tensor_transfer_only && mxfp4_path) total += 6;
  if (!tensor_transfer_only && mxfp6_path) total += 3;
  if (tensor_transfer_path &&
      (!getenv("MTLC_ALLOW_EXPERIMENTAL_TMA") ||
       strcmp(getenv("MTLC_ALLOW_EXPERIMENTAL_TMA"),
              "I_ACCEPT_GPU_RESET_RISK") != 0)) {
    fprintf(stderr,
            "experimental TMA execution is quarantined; use only a disposable/recoverable host and set MTLC_ALLOW_EXPERIMENTAL_TMA=I_ACCEPT_GPU_RESET_RISK\n");
    return 2;
  }
  if (tensor_transfer_path &&
      (!getenv("MTLC_TMA_RECOVERY_READY") ||
       strcmp(getenv("MTLC_TMA_RECOVERY_READY"),
              "I_HAVE_OUT_OF_BAND_RECOVERY") != 0)) {
    fprintf(stderr,
            "experimental TMA execution requires MTLC_TMA_RECOVERY_READY=I_HAVE_OUT_OF_BAND_RECOVERY and actual out-of-band reset/reboot access\n");
    return 2;
  }
  if (sizeof(void *) != 8 || sizeof(CUdeviceptr) != 8) {
    fprintf(stderr, "64-bit host and device pointers are required\n");
    return 2;
  }
  if (!load_driver(&h.api)) return 2;
  rc = h.api.cuInit(0);
  if (rc != CUDA_SUCCESS) {
    report_cuda_error(&h, "cuInit", rc);
    goto cleanup;
  }
  rc = h.api.cuDeviceGet(&device, 0);
  if (rc != CUDA_SUCCESS) {
    report_cuda_error(&h, "cuDeviceGet", rc);
    goto cleanup;
  }
  if (h.api.cuDeviceGetName(device_name, (int)sizeof(device_name), device) !=
          CUDA_SUCCESS ||
      h.api.cuDeviceGetAttribute(
          &major, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, device) !=
          CUDA_SUCCESS ||
      h.api.cuDeviceGetAttribute(
          &minor, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, device) !=
          CUDA_SUCCESS ||
      h.api.cuDeviceGetAttribute(&integrated, CU_DEVICE_ATTRIBUTE_INTEGRATED,
                                 device) != CUDA_SUCCESS) {
    fprintf(stderr, "cannot query CUDA device identity\n");
    goto cleanup;
  }
  printf("GPU: %s, compute %d.%d, integrated=%d, host=%s\n", device_name,
         major, minor, integrated, host_is_aarch64() ? "AArch64" : "non-Arm");
  if (require_gb10 &&
      (!host_is_aarch64() || major != 12 || minor != 1 || !integrated)) {
    fprintf(stderr,
            "GB10 gate refused: requires AArch64 + integrated compute 12.1\n");
    goto cleanup;
  }
  rc = h.api.cuCtxCreate_v2(&h.context, 0, device);
  if (rc != CUDA_SUCCESS) {
    report_cuda_error(&h, "cuCtxCreate_v2", rc);
    goto cleanup;
  }
  if (!tensor_transfer_only) {
    ptx = read_file(argv[1]);
    if (!ptx) goto cleanup;
    rc = h.api.cuModuleLoadData(&h.module, ptx);
    if (rc != CUDA_SUCCESS) {
      report_cuda_error(&h, "cuModuleLoadData", rc);
      goto cleanup;
    }

    passed += test_index_3d(&h);
    passed += test_saxpy_odd(&h);
    passed += test_row_norm(&h);
    passed += test_staged_copy(&h);
    passed += test_async_stage_u32x4(&h);
    passed += test_auto_stage_u32(&h);
    passed += test_dynamic_staged_copy(&h);
    passed += test_atomic_litmus(&h);
    passed += test_subgroup_contract(&h);
    passed += test_subgroup_extended(&h);
    passed += test_subgroup_exchange_vote(&h);
    passed += test_softmax_rows_f32(&h);
    passed += test_layer_norm_rows_f32(&h);
    passed += test_tensor_f16_f32(&h);
    passed += test_tensor_fp8_native(&h);
    passed += test_tensor_fp8_chain_resident(&h);
    passed += test_tensor_fp8_loop_resident(&h);
    passed += test_tensor_chain4(&h);
    passed += test_tensor_pipeline_f16_f32(&h);
    passed += test_tensor_pipeline4_f16_f32(&h);
    passed += test_gemm_full_tiles_f16_f32(&h);
    passed += test_gemm_tail_complete_f16_f32(&h);
    passed += test_narrow_scalar_abi(&h);

    if (mxfp4_path) {
      mxfp4_ptx = read_file(mxfp4_path);
      if (!mxfp4_ptx) goto cleanup;
      rc = h.api.cuModuleLoadData(&mxfp4_module, mxfp4_ptx);
      if (rc != CUDA_SUCCESS) {
        report_cuda_error(&h, "cuModuleLoadData(MXFP4)", rc);
        goto cleanup;
      }
      CUmodule primary_module = h.module;
      h.module = mxfp4_module;
      passed += test_tensor_mxfp4_native(&h);
      passed += test_tensor_mxfp4_chain_resident(&h);
      passed += test_tensor_mxfp4_loop_resident(&h);
      passed += test_tensor_nvfp4(&h, NVFP4_DIRECT);
      passed += test_tensor_nvfp4(&h, NVFP4_CHAIN);
      passed += test_tensor_nvfp4(&h, NVFP4_LOOP);
      h.module = primary_module;
    }
    if (mxfp6_path) {
      mxfp6_ptx = read_file(mxfp6_path);
      if (!mxfp6_ptx) goto cleanup;
      rc = h.api.cuModuleLoadData(&mxfp6_module, mxfp6_ptx);
      if (rc != CUDA_SUCCESS) {
        report_cuda_error(&h, "cuModuleLoadData(MXFP6)", rc);
        goto cleanup;
      }
      CUmodule primary_module = h.module;
      h.module = mxfp6_module;
      passed += test_tensor_mxfp6(&h, MXFP6_DIRECT);
      passed += test_tensor_mxfp6(&h, MXFP6_CHAIN);
      passed += test_tensor_mxfp6(&h, MXFP6_LOOP);
      h.module = primary_module;
    }
  }
  if (tensor_transfer_path) {
    tensor_transfer_ptx = read_file(tensor_transfer_path);
    if (!tensor_transfer_ptx) goto cleanup;
    rc = h.api.cuModuleLoadData(&tensor_transfer_module, tensor_transfer_ptx);
    if (rc != CUDA_SUCCESS) {
      report_cuda_error(&h, "cuModuleLoadData(tensor transfer)", rc);
      goto cleanup;
    }
    CUmodule primary_module = h.module;
    h.module = tensor_transfer_module;
    passed += test_tensor_transfer_load_2d(&h);
    passed += test_tensor_transfer_store_5d(&h);
    h.module = primary_module;
  }
  printf("%s: %d/%d passed\n",
         tensor_transfer_only ? "Experimental TMA summary"
                              : "Hardware GPU summary",
         passed, total);

cleanup:
  if (tensor_transfer_module)
    (void)h.api.cuModuleUnload(tensor_transfer_module);
  if (mxfp6_module) (void)h.api.cuModuleUnload(mxfp6_module);
  if (mxfp4_module) (void)h.api.cuModuleUnload(mxfp4_module);
  if (h.module) (void)h.api.cuModuleUnload(h.module);
  if (h.context) (void)h.api.cuCtxDestroy_v2(h.context);
  free(ptx);
  free(mxfp4_ptx);
  free(mxfp6_ptx);
  free(tensor_transfer_ptx);
  close_driver_library(h.api.library);
  return passed == total ? 0 : 1;
}
