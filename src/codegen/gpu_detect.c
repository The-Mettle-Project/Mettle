#include "gpu_detect.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The CUDA driver library is loaded by hand rather than linked. A compiler
 * that linked `cuda.lib` would refuse to start on a machine with no NVIDIA
 * driver, and cross-compiling for a GPU the build host does not have is a
 * normal thing to do. Loading it late keeps GPU support a property of the
 * machine running the compile, not of the binary. */

typedef int (*GpuCuInt)(unsigned int);
typedef int (*GpuCuIntOut)(int *);
typedef int (*GpuCuDeviceGet)(int *, int);
typedef int (*GpuCuDeviceGetName)(char *, int, int);
typedef int (*GpuCuDeviceGetAttribute)(int *, int, int);
typedef int (*GpuCuDeviceTotalMem)(size_t *, int);

/* CUdevice_attribute values that matter here. These are ABI, fixed since
 * CUDA 2.x, so naming them beats depending on cuda.h being installed. */
#define GPU_ATTR_MAX_THREADS_PER_BLOCK 1
#define GPU_ATTR_MAX_SHARED_MEMORY_PER_BLOCK 8
#define GPU_ATTR_WARP_SIZE 10
#define GPU_ATTR_CLOCK_RATE 13
#define GPU_ATTR_MULTIPROCESSOR_COUNT 16
#define GPU_ATTR_INTEGRATED 18
#define GPU_ATTR_COMPUTE_CAPABILITY_MAJOR 75
#define GPU_ATTR_COMPUTE_CAPABILITY_MINOR 76

#if defined(_WIN32)

__declspec(dllimport) void *LoadLibraryA(const char *name);
__declspec(dllimport) void *GetProcAddress(void *module, const char *name);

static void *gpu_load_driver(void) {
  return LoadLibraryA("nvcuda.dll");
}

static void *gpu_driver_symbol(void *library, const char *name) {
  return GetProcAddress(library, name);
}

#else

#include <dlfcn.h>

static void *gpu_load_driver(void) {
  void *library = dlopen("libcuda.so.1", RTLD_LAZY);
  if (!library) library = dlopen("libcuda.so", RTLD_LAZY);
  return library;
}

static void *gpu_driver_symbol(void *library, const char *name) {
  return dlsym(library, name);
}

#endif

static int gpu_driver_attribute(GpuCuDeviceGetAttribute get, int device,
                                int attribute) {
  int value = 0;
  if (!get || get(&value, attribute, device) != 0) return 0;
  return value;
}

/* Ask the driver. Returns 1 when at least one device answered. */
static int gpu_detect_via_driver(GpuDetectResult *out) {
  void *library = gpu_load_driver();
  if (!library) return 0;

  GpuCuInt cu_init = (GpuCuInt)gpu_driver_symbol(library, "cuInit");
  GpuCuIntOut cu_driver_version =
      (GpuCuIntOut)gpu_driver_symbol(library, "cuDriverGetVersion");
  GpuCuIntOut cu_device_count =
      (GpuCuIntOut)gpu_driver_symbol(library, "cuDeviceGetCount");
  GpuCuDeviceGet cu_device_get =
      (GpuCuDeviceGet)gpu_driver_symbol(library, "cuDeviceGet");
  GpuCuDeviceGetName cu_device_name =
      (GpuCuDeviceGetName)gpu_driver_symbol(library, "cuDeviceGetName");
  GpuCuDeviceGetAttribute cu_attribute =
      (GpuCuDeviceGetAttribute)gpu_driver_symbol(library,
                                                 "cuDeviceGetAttribute");
  GpuCuDeviceTotalMem cu_total_memory =
      (GpuCuDeviceTotalMem)gpu_driver_symbol(library, "cuDeviceTotalMem_v2");

  if (!cu_init || !cu_device_count || !cu_device_get || !cu_attribute) return 0;
  if (cu_init(0) != 0) return 0;

  int count = 0;
  if (cu_device_count(&count) != 0 || count <= 0) return 0;
  if (count > GPU_DETECT_MAX_DEVICES) count = GPU_DETECT_MAX_DEVICES;

  if (cu_driver_version) {
    int version = 0;
    if (cu_driver_version(&version) == 0) out->driver_version = version;
  }

  int described = 0;
  for (int i = 0; i < count; i++) {
    int device = 0;
    if (cu_device_get(&device, i) != 0) continue;
    GpuDetectDevice *slot = &out->devices[described];
    memset(slot, 0, sizeof(*slot));
    if (cu_device_name) {
      if (cu_device_name(slot->name, (int)sizeof(slot->name), device) != 0) {
        slot->name[0] = '\0';
      }
      slot->name[sizeof(slot->name) - 1] = '\0';
    }
    slot->compute_major =
        gpu_driver_attribute(cu_attribute, device,
                             GPU_ATTR_COMPUTE_CAPABILITY_MAJOR);
    slot->compute_minor =
        gpu_driver_attribute(cu_attribute, device,
                             GPU_ATTR_COMPUTE_CAPABILITY_MINOR);
    slot->multiprocessor_count =
        gpu_driver_attribute(cu_attribute, device,
                             GPU_ATTR_MULTIPROCESSOR_COUNT);
    slot->warp_size =
        gpu_driver_attribute(cu_attribute, device, GPU_ATTR_WARP_SIZE);
    slot->max_threads_per_block =
        gpu_driver_attribute(cu_attribute, device,
                             GPU_ATTR_MAX_THREADS_PER_BLOCK);
    slot->max_shared_memory_per_block =
        gpu_driver_attribute(cu_attribute, device,
                             GPU_ATTR_MAX_SHARED_MEMORY_PER_BLOCK);
    slot->clock_khz =
        gpu_driver_attribute(cu_attribute, device, GPU_ATTR_CLOCK_RATE);
    slot->integrated =
        gpu_driver_attribute(cu_attribute, device, GPU_ATTR_INTEGRATED);
    if (cu_total_memory) {
      size_t bytes = 0;
      if (cu_total_memory(&bytes, device) == 0) {
        slot->total_memory = (long long)bytes;
      }
    }
    if (slot->compute_major <= 0) continue;
    described++;
  }
  if (described == 0) return 0;
  out->device_count = described;
  out->available = 1;
  out->source = "cuda-driver";
  return 1;
}

/* Run `command` and copy its first line into `line`. Returns 1 when the
 * command exited successfully and wrote something. */
static int gpu_read_command_line(const char *command, char *line,
                                 size_t line_size) {
#if defined(_WIN32)
  FILE *pipe = _popen(command, "r");
#else
  FILE *pipe = popen(command, "r");
#endif
  if (!pipe) return 0;
  line[0] = '\0';
  const char *read = fgets(line, (int)line_size, pipe);
#if defined(_WIN32)
  int status = _pclose(pipe);
#else
  int status = pclose(pipe);
#endif
  if (!read || status != 0) return 0;
  size_t length = strlen(line);
  while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r')) {
    line[--length] = '\0';
  }
  return length > 0;
}

/* No driver library, but possibly still a GPU: nvidia-smi reports the name and
 * compute capability. It cannot report a multiprocessor count, so occupancy's
 * whole-card threshold stays unavailable on this path. */
static int gpu_detect_via_nvidia_smi(GpuDetectResult *out) {
  char line[256];
  if (!gpu_read_command_line(
          "nvidia-smi --query-gpu=compute_cap,name --format=csv,noheader",
          line, sizeof(line))) {
    return 0;
  }
  int major = 0, minor = 0;
  if (sscanf(line, "%d.%d", &major, &minor) != 2 || major < 1 || major > 99 ||
      minor < 0 || minor > 9) {
    return 0;
  }
  GpuDetectDevice *slot = &out->devices[0];
  memset(slot, 0, sizeof(*slot));
  slot->compute_major = major;
  slot->compute_minor = minor;
  slot->warp_size = 32;
  const char *comma = strchr(line, ',');
  if (comma) {
    const char *name = comma + 1;
    while (*name == ' ') name++;
    snprintf(slot->name, sizeof(slot->name), "%s", name);
  }
  out->device_count = 1;
  out->available = 1;
  out->source = "nvidia-smi";
  return 1;
}

const GpuDetectResult *gpu_detect_local(void) {
  static GpuDetectResult cached;
  static int done = 0;
  if (done) return &cached;
  done = 1;
  memset(&cached, 0, sizeof(cached));
  cached.source = "no NVIDIA driver found";
  if (gpu_detect_via_driver(&cached)) return &cached;
  memset(&cached, 0, sizeof(cached));
  cached.source = "no NVIDIA driver found";
  if (gpu_detect_via_nvidia_smi(&cached)) return &cached;
  return &cached;
}

const char *gpu_detect_ptxas_version(void) {
  static char version[32];
  static int done = 0;
  if (done) return version[0] ? version : NULL;
  done = 1;
#if defined(_WIN32)
  FILE *pipe = _popen("ptxas --version 2>nul", "r");
#else
  FILE *pipe = popen("ptxas --version 2>/dev/null", "r");
#endif
  if (!pipe) return NULL;
  char line[512];
  while (fgets(line, sizeof(line), pipe)) {
    const char *release = strstr(line, "release ");
    if (!release) continue;
    release += 8;
    size_t i = 0;
    while (i + 1 < sizeof(version) &&
           ((release[i] >= '0' && release[i] <= '9') || release[i] == '.')) {
      version[i] = release[i];
      i++;
    }
    version[i] = '\0';
    break;
  }
#if defined(_WIN32)
  _pclose(pipe);
#else
  pclose(pipe);
#endif
  return version[0] ? version : NULL;
}

/* `ptxas --help` enumerates every `--gpu-name` it accepts, so the installed
 * assembler's capability list is a question it can answer about itself. */
const char *gpu_detect_ptxas_targets(void) {
  static char targets[4096];
  static int done = 0;
  if (done) return targets[0] ? targets : NULL;
  done = 1;
#if defined(_WIN32)
  FILE *pipe = _popen("ptxas --help 2>nul", "r");
#else
  FILE *pipe = popen("ptxas --help 2>/dev/null", "r");
#endif
  if (!pipe) return NULL;
  size_t used = 0;
  char line[1024];
  while (fgets(line, sizeof(line), pipe)) {
    const char *cursor = line;
    while ((cursor = strstr(cursor, "sm_")) != NULL) {
      const char *end = cursor + 3;
      while ((*end >= '0' && *end <= '9')) end++;
      if (*end == 'a' || *end == 'f') end++;
      size_t length = (size_t)(end - cursor);
      if (length > 4 && used + length + 2 < sizeof(targets)) {
        /* One NUL-separated copy of each name; the list is short enough that
         * a linear duplicate scan costs less than a set would. */
        int seen = 0;
        for (size_t at = 0; at < used; at += strlen(targets + at) + 1) {
          if (strlen(targets + at) == length &&
              strncmp(targets + at, cursor, length) == 0) {
            seen = 1;
            break;
          }
        }
        if (!seen) {
          memcpy(targets + used, cursor, length);
          used += length;
          targets[used++] = '\0';
        }
      }
      cursor = end;
    }
  }
#if defined(_WIN32)
  _pclose(pipe);
#else
  pclose(pipe);
#endif
  if (used + 1 < sizeof(targets)) targets[used] = '\0';
  return used > 0 ? targets : NULL;
}

int gpu_detect_ptxas_supports(const char *target) {
  if (!target) return 0;
  const char *targets = gpu_detect_ptxas_targets();
  if (!targets) return 1;
  for (size_t at = 0; targets[at]; at += strlen(targets + at) + 1) {
    if (strcmp(targets + at, target) == 0) return 1;
  }
  return 0;
}

/* CUDA release -> newest PTX ISA that release's driver accepts. Every entry is
 * a published pair from the PTX ISA release notes; a driver newer than the
 * last row keeps the last row's answer, which only ever understates what it
 * can load. */
int gpu_detect_ptx_isa(int *major, int *minor) {
  static const struct {
    int cuda;    /* 12090 == CUDA 12.9 */
    int isa[2];
  } releases[] = {
      {11000, {7, 0}}, {11010, {7, 1}}, {11020, {7, 2}}, {11030, {7, 3}},
      {11040, {7, 4}}, {11050, {7, 5}}, {11060, {7, 6}}, {11070, {7, 7}},
      {11080, {7, 8}}, {12000, {8, 0}}, {12010, {8, 1}}, {12020, {8, 2}},
      {12030, {8, 3}}, {12040, {8, 4}}, {12050, {8, 5}}, {12060, {8, 5}},
      {12080, {8, 7}}, {12090, {8, 8}}, {13000, {9, 0}},
  };
  const GpuDetectResult *local = gpu_detect_local();
  if (!local->available || local->driver_version <= 0) return 0;

  const int count = (int)(sizeof(releases) / sizeof(releases[0]));
  int best = -1;
  for (int i = 0; i < count; i++) {
    if (local->driver_version >= releases[i].cuda) best = i;
  }
  if (best < 0) return 0;
  if (major) *major = releases[best].isa[0];
  if (minor) *minor = releases[best].isa[1];
  return 1;
}

int gpu_detect_ptx_target(int device, char *out, size_t out_size) {
  const GpuDetectResult *local = gpu_detect_local();
  if (!local->available || device < 0 || device >= local->device_count) {
    return 0;
  }
  const GpuDetectDevice *found = &local->devices[device];
  if (found->compute_major <= 0) return 0;

  /* Compute capability 9.0 onward publishes architecture-specific targets --
   * sm_90a, sm_120a -- whose extra instructions (block-scaled MMA and the
   * rest) are exactly what an inference kernel wants. Take the `a` form when
   * the installed assembler confirms it exists, so a toolkit that predates
   * the card still gets PTX it can read. */
  char candidate[32];
  if (found->compute_major >= 9) {
    snprintf(candidate, sizeof(candidate), "sm_%d%da", found->compute_major,
             found->compute_minor);
    if (gpu_detect_ptxas_supports(candidate)) {
      snprintf(out, out_size, "%s", candidate);
      return 1;
    }
  }
  snprintf(candidate, sizeof(candidate), "sm_%d%d", found->compute_major,
           found->compute_minor);
  snprintf(out, out_size, "%s", candidate);
  return 1;
}
