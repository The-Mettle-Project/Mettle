/* Hardware-free provider for tests/test_gpu_dispatch.mettle. On AArch64 it
 * additionally verifies the AAPCS64 placement of all eleven cuLaunchKernel
 * arguments, including the three overflow arguments carried on the stack. */
#include <stdint.h>

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
  } else {
    valid = 0;
  }
  if (!valid) {
    return 97;
  }
  launch_calls++;
  return 0;
}

int gpu_stub_finish(void) { return launch_calls == 2 ? 0 : 96; }

/* Link-only no-op stubs for the rest of the CUDA driver surface std/gpu
 * binds. The dispatch test never calls these; they exist because linking the
 * whole relocatable object resolves every std/gpu function, not just the
 * ones main() reaches. Out-parameters are zeroed defensively. */
static int stub_out64(int64_t *out) {
  if (out) *out = 0;
  return 0;
}
int cuInit(unsigned flags) { (void)flags; return 0; }
int cuDeviceGet(int64_t *device, int ordinal) { (void)ordinal; return stub_out64(device); }
int cuDeviceGetAttribute(int64_t *value, int attribute, int64_t device) {
  (void)attribute; (void)device; return stub_out64(value);
}
int cuCtxCreate_v2(int64_t *ctx, unsigned flags, int64_t device) {
  (void)flags; (void)device; return stub_out64(ctx);
}
int cuCtxSynchronize(void) { return 0; }
int cuModuleLoadData(int64_t *module, const void *image) { (void)image; return stub_out64(module); }
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
