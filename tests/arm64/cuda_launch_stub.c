/* Hardware-free provider for tests/test_gpu_dispatch.mettle. On AArch64 it
 * additionally verifies the AAPCS64 placement of all eleven cuLaunchKernel
 * arguments, including the three overflow arguments carried on the stack. */
#include <stdint.h>
#include <stdio.h>

static int launch_calls;

int cuLaunchKernel(int64_t function, uint32_t gx, uint32_t gy, uint32_t gz,
                   uint32_t bx, uint32_t by, uint32_t bz,
                   uint32_t shared_bytes, int64_t stream,
                   int64_t *kernel_params, int64_t *extra) {
  int valid = function == 0 && kernel_params != 0 && extra == 0;
  if (launch_calls == 0) {
    valid = valid && gx == 4 && gy == 1 && gz == 1 && bx == 256 &&
            by == 1 && bz == 1 && shared_bytes == 0 && stream == 0;
  } else if (launch_calls == 1) {
    valid = valid && gx == 7 && gy == 3 && gz == 2 && bx == 32 &&
            by == 4 && bz == 1 && shared_bytes == 4096 && stream == 99;
  } else if (launch_calls == 2) {
    valid = valid && gx == 4 && gy == 1 && gz == 1 && bx == 256 &&
            by == 1 && bz == 1 && shared_bytes == 0 && stream == 99;
  } else {
    valid = 0;
  }
  if (!valid) {
    return 97;
  }
  launch_calls++;
  return 0;
}

int gpu_stub_finish(void) { return launch_calls == 3 ? 0 : 96; }

/* Link-only no-op stubs for the rest of the CUDA driver surface std/gpu
 * binds. The dispatch test never calls these; they exist because linking the
 * whole relocatable object resolves every std/gpu function, not just the
 * ones main() reaches. Out-parameters are zeroed defensively. */
static int stub_out64(int64_t *out) {
  if (out) *out = 0;
  return 0;
}
static int stub_out32(int32_t *out) {
  if (out) *out = 0;
  return 0;
}
int cuInit(unsigned flags) { (void)flags; return 0; }
int cuDeviceGet(int32_t *device, int32_t ordinal) { (void)ordinal; return stub_out32(device); }
int cuDeviceGetAttribute(int32_t *value, int32_t attribute, int32_t device) {
  (void)attribute; (void)device; return stub_out32(value);
}
int cuDeviceGetCount(int32_t *count) { return stub_out32(count); }
int cuDeviceGetName(char *name, int32_t length, int32_t device) {
  (void)device;
  if (name && length > 0) name[0] = '\0';
  return 0;
}
int cuDeviceTotalMem_v2(int64_t *bytes, int32_t device) {
  (void)device; return stub_out64(bytes);
}
int cuDriverGetVersion(int32_t *version) { return stub_out32(version); }
int cuGetErrorName(int32_t code, const char **out) {
  (void)code;
  if (out) *out = "CUDA_STUB";
  return 0;
}
int cuGetErrorString(int32_t code, const char **out) {
  (void)code;
  if (out) *out = "stubbed CUDA driver";
  return 0;
}
int cuCtxCreate_v2(int64_t *ctx, unsigned flags, int32_t device) {
  (void)flags; (void)device; return stub_out64(ctx);
}
int cuCtxSynchronize(void) { return 0; }
int cuModuleLoadData(int64_t *module, const void *image) { (void)image; return stub_out64(module); }
int cuModuleLoadDataEx(int64_t *module, const void *image, uint32_t count,
                       int32_t *options, int64_t *values) {
  (void)image; (void)count; (void)options; (void)values;
  return stub_out64(module);
}
int cuModuleUnload(int64_t module) { (void)module; return 0; }
int cuModuleGetFunction(int64_t *function, int64_t module, const char *name) {
  (void)module; (void)name; return stub_out64(function);
}
int cuMemAlloc_v2(int64_t *dptr, int64_t bytes) { (void)bytes; return stub_out64(dptr); }
int cuMemFree_v2(int64_t dptr) { (void)dptr; return 0; }
int cuMemAllocAsync(int64_t *dptr, int64_t bytes, int64_t stream) {
  (void)bytes; (void)stream; return stub_out64(dptr);
}
int cuMemFreeAsync(int64_t dptr, int64_t stream) { (void)dptr; (void)stream; return 0; }
int cuMemAllocManaged(int64_t *dptr, int64_t bytes, unsigned flags) {
  (void)bytes; (void)flags; return stub_out64(dptr);
}
int cuMemHostAlloc(int64_t *pp, int64_t bytes, unsigned flags) {
  (void)bytes; (void)flags; return stub_out64(pp);
}
int cuMemFreeHost(int64_t p) { (void)p; return 0; }
int cuMemHostGetDevicePointer_v2(int64_t *dptr, int64_t p, unsigned flags) {
  (void)p; (void)flags; return stub_out64(dptr);
}
int cuMemcpyHtoD_v2(int64_t dst, const void *src, int64_t bytes) {
  (void)dst; (void)src; (void)bytes; return 0;
}
int cuMemcpyDtoH_v2(void *dst, int64_t src, int64_t bytes) {
  (void)dst; (void)src; (void)bytes; return 0;
}
int cuMemcpyHtoDAsync_v2(int64_t dst, const void *src, int64_t bytes, int64_t stream) {
  (void)dst; (void)src; (void)bytes; (void)stream; return 0;
}
int cuMemcpyDtoHAsync_v2(void *dst, int64_t src, int64_t bytes, int64_t stream) {
  (void)dst; (void)src; (void)bytes; (void)stream; return 0;
}
int cuStreamCreate(int64_t *stream, unsigned flags) { (void)flags; return stub_out64(stream); }
int cuStreamDestroy_v2(int64_t stream) { (void)stream; return 0; }
int cuStreamSynchronize(int64_t stream) { (void)stream; return 0; }
int cuStreamWaitEvent(int64_t stream, int64_t event, unsigned flags) {
  (void)stream; (void)event; (void)flags; return 0;
}
int cuEventCreate(int64_t *event, unsigned flags) { (void)flags; return stub_out64(event); }
int cuEventDestroy_v2(int64_t event) { (void)event; return 0; }
int cuEventRecord(int64_t event, int64_t stream) { (void)event; (void)stream; return 0; }
int cuEventSynchronize(int64_t event) { (void)event; return 0; }
int cuEventElapsedTime(float *ms, int64_t start, int64_t end) {
  if (ms) *ms = 0.0f;
  (void)start; (void)end;
  return 0;
}
int cuStreamBeginCapture_v2(int64_t stream, int32_t mode) {
  (void)stream; (void)mode; return 0;
}
int cuStreamEndCapture(int64_t stream, int64_t *graph) {
  (void)stream; return stub_out64(graph);
}
int cuGraphInstantiateWithFlags(int64_t *exec, int64_t graph, int64_t flags) {
  (void)graph; (void)flags; return stub_out64(exec);
}
int cuGraphLaunch(int64_t exec, int64_t stream) { (void)exec; (void)stream; return 0; }
int cuGraphExecDestroy(int64_t exec) { (void)exec; return 0; }
int cuGraphDestroy(int64_t graph) { (void)graph; return 0; }

/* std/gpu reaches stderr through the UCRT spelling. The bundled runtime
 * defines it, but this link is plain glibc. */
void *__acrt_iob_func(int index) {
  return index == 0 ? (void *)stdin : index == 1 ? (void *)stdout : (void *)stderr;
}

/* `"{code}"` in std/gpu's diagnostics lowers to this runtime helper. The
 * bundled string runtime supplies it; this link does not have it, and the
 * dispatch test only reaches it on a failure path, so a small ring of
 * buffers is enough to keep one message's digits apart. `string` is a
 * 16-byte view: `chars` at offset 0, `length` at offset 8. */
typedef struct {
  const char *chars;
  uint64_t length;
} MettleStringView;

MettleStringView mettle_string_from_int(int64_t value) {
  static char rings[4][24];
  static unsigned next_ring;
  char *out = rings[next_ring++ % 4];
  int written = snprintf(out, sizeof(rings[0]), "%lld", (long long)value);
  MettleStringView view;
  view.chars = out;
  view.length = written > 0 ? (uint64_t)written : 0u;
  return view;
}
