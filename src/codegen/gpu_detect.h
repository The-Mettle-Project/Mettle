#ifndef GPU_DETECT_H
#define GPU_DETECT_H

#include <stddef.h>

/* What the local machine can actually run, asked of the CUDA driver rather
 * than of a command-line tool. `--emit-ptx` uses it to pick a `.target` the
 * installed GPU accepts, `--report-occupancy` uses the multiprocessor count,
 * and `--gpu-info` prints the whole thing. */

#define GPU_DETECT_MAX_DEVICES 8

typedef struct GpuDetectDevice {
  char name[128];
  int compute_major;
  int compute_minor;
  int multiprocessor_count;
  int warp_size;
  int max_threads_per_block;
  int max_shared_memory_per_block;
  int clock_khz;
  int integrated; /* 1 on unified-memory parts such as GB10 */
  long long total_memory;
} GpuDetectDevice;

typedef struct GpuDetectResult {
  int available;      /* 1 when a driver answered with at least one device */
  int device_count;   /* devices described in `devices` */
  int driver_version; /* 12090 means CUDA 12.9; 0 when unknown */
  const char *source; /* "cuda-driver", "nvidia-smi", or the reason it failed */
  GpuDetectDevice devices[GPU_DETECT_MAX_DEVICES];
} GpuDetectResult;

/* Query the local driver once per process and cache the answer. Never NULL:
 * a machine with no GPU gets `available == 0` and a `source` explaining why. */
const GpuDetectResult *gpu_detect_local(void);

/* Write the PTX `.target` for `device` ("sm_120a") into `out`. Returns 0 when
 * the device does not exist or detection found nothing, leaving `out` alone,
 * so callers keep their cross-compile default. */
int gpu_detect_ptx_target(int device, char *out, size_t out_size);

/* The PTX targets the installed `ptxas` accepts, as a NUL-separated block of
 * names. Returns NULL when ptxas is absent. Cached like the driver query. */
const char *gpu_detect_ptxas_targets(void);

/* 1 when the installed ptxas lists `target`. Also 1 when ptxas is absent: an
 * assembler nobody has cannot object to a target, and the driver JIT -- the
 * real consumer of emitted PTX -- is the authority in that case. */
int gpu_detect_ptxas_supports(const char *target);

/* `ptxas --version`'s release, "12.9", or NULL when ptxas is absent. */
const char *gpu_detect_ptxas_version(void);

/* The newest PTX ISA the local driver can load, written to `major`/`minor`.
 * Emitting a `.version` above this makes cuModuleLoadData fail at run time
 * with nothing but a status code to explain it. Returns 0 when no driver
 * answered, leaving both outputs alone. */
int gpu_detect_ptx_isa(int *major, int *minor);

#endif
