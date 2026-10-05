#include <stdint.h>

static int launch_calls;
static int indirect_calls;
static int launch_failed;

static int params_valid(int64_t function, int64_t *kernel_params,
                        int32_t nargs) {
  int valid = function == 0 && kernel_params != 0 && nargs == 5;
  if (valid) {
    valid = *(int64_t *)(uintptr_t)kernel_params[0] == 11 &&
            *(int64_t *)(uintptr_t)kernel_params[1] == 22 &&
            *(int64_t *)(uintptr_t)kernel_params[2] == 33 &&
            *(int32_t *)(uintptr_t)kernel_params[3] == 1024 &&
            *(float *)(uintptr_t)kernel_params[4] == 2.5f &&
            kernel_params[5] == ((8 << 24) | 8) &&
            kernel_params[6] == ((8 << 24) | 8) &&
            kernel_params[7] == ((8 << 24) | 8) &&
            kernel_params[8] == ((4 << 24) | 4) &&
            kernel_params[9] == ((4 << 24) | 4);
  }
  return valid;
}

void mtlc_gpu_launch_indirect_checked(int64_t function, int64_t counts,
                                      int32_t bx, int32_t by, int32_t bz,
                                      int32_t shared_bytes, int64_t stream,
                                      int64_t *kernel_params, int32_t nargs) {
  int valid = params_valid(function, kernel_params, nargs) &&
              counts == 4112 && bx == 64 && by == 2 && bz == 1 &&
              shared_bytes == 128 && stream == 99 && launch_calls == 2;
  if (!valid) launch_failed = 1;
  indirect_calls++;
}

void mtlc_gpu_launch_checked(int64_t function,
                             int32_t gx, int32_t gy, int32_t gz,
                             int32_t bx, int32_t by, int32_t bz,
                             int32_t shared_bytes, int64_t stream,
                             int64_t *kernel_params, int32_t nargs) {
  int valid = params_valid(function, kernel_params, nargs);
  if (launch_calls == 0) {
    valid = valid && gx == 4 && gy == 1 && gz == 1 && bx == 256 &&
            by == 1 && bz == 1 && shared_bytes == 0 && stream == 0;
  } else if (launch_calls == 1) {
    valid = valid && gx == 7 && gy == 3 && gz == 2 && bx == 32 &&
            by == 4 && bz == 1 && shared_bytes == 4096 && stream == 99;
  } else {
    valid = 0;
  }
  if (!valid) launch_failed = 1;
  launch_calls++;
}

int gpu_stub_finish(void) {
  return !launch_failed && launch_calls == 2 && indirect_calls == 1 ? 0 : 98;
}
