#if !defined(_WIN32) && !defined(__APPLE__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "metal_provider.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#define rt_write_fd _write
#else
#include <time.h>
#include <unistd.h>
#define rt_write_fd write
#endif

#define METAL_RT_MAX_KERNELS 256

static MettleMetal *g_metal;
static char g_error[2048];
static char *g_kernels[METAL_RT_MAX_KERNELS];
static size_t g_kernel_count;
static int g_version_major = 3;
static int g_version_minor = 2;

#define RT_OP_LAUNCH 0
#define RT_OP_FLUSH 1
#define RT_OP_BARRIERS_OFF 2
#define RT_OP_BARRIERS_ON 3

typedef struct {
  int op;
  int64_t kernel;
  uint32_t grid[3];
  uint64_t counts;
  uint32_t block[3];
  unsigned char *args;
  size_t args_size;
  size_t shared_bytes;
} RtLaunch;

typedef struct {
  RtLaunch *launches;
  size_t count;
  size_t capacity;
  int live;
} RtGraph;

static RtGraph *g_graphs;
static size_t g_graph_count;
static int g_capturing;
static size_t g_capture;

static void rt_error(const char *text) {
  snprintf(g_error, sizeof(g_error), "%s", text);
}

static void rt_print(const char *text) {
  size_t length = text ? strlen(text) : 0;
  while (length > 0) {
    int written = (int)rt_write_fd(2, text, (unsigned)length);
    if (written <= 0) {
      return;
    }
    text += written;
    length -= (size_t)written;
  }
}

static void rt_forget_kernels(void) {
  for (size_t i = 0; i < g_kernel_count; i++) {
    free(g_kernels[i]);
    g_kernels[i] = NULL;
  }
  g_kernel_count = 0;
}

static void rt_read_version(void) {
  const char *text = getenv("METTLE_METAL_VERSION");
  int major = 0;
  int minor = 0;
  if (text && sscanf(text, "%d.%d", &major, &minor) == 2 && major >= 3) {
    g_version_major = major;
    g_version_minor = minor;
  }
}

const char *mettle_metal_rt_error(void) { return g_error; }

void mettle_metal_rt_set_version(int32_t major, int32_t minor) {
  g_version_major = major;
  g_version_minor = minor;
}

int32_t mettle_metal_rt_sync(void) {
  if (!g_metal) {
    return 0;
  }
  if (!mettle_metal_sync(g_metal, g_error, sizeof(g_error))) {
    char line[2200];
    snprintf(line, sizeof(line), "mettle: Metal: %s\n", g_error);
    rt_print(line);
    return 1;
  }
  return 0;
}

static void rt_finish(void) {
  if (g_metal) {
    mettle_metal_rt_sync();
  }
}

int32_t mettle_metal_rt_init(void) {
  if (g_metal) {
    return 1;
  }
  rt_read_version();
  g_metal = mettle_metal_open(g_error, sizeof(g_error));
  if (g_metal) {
    atexit(rt_finish);
  }
  return g_metal ? 1 : 0;
}

void mettle_metal_rt_shutdown(void) {
  rt_forget_kernels();
  mettle_metal_close(g_metal);
  g_metal = NULL;
}

const char *mettle_metal_rt_device_name(void) {
  return g_metal ? mettle_metal_device_name(g_metal) : "";
}

int64_t mettle_metal_rt_property(int32_t which) {
  return g_metal ? mettle_metal_property(g_metal, which) : 0;
}

int32_t mettle_metal_rt_load_source(const char *text) {
  if (!text) {
    rt_error("no Metal source was given");
    return 0;
  }
  if (!mettle_metal_rt_init()) {
    return 0;
  }
  rt_forget_kernels();
  return mettle_metal_load(g_metal, text, strlen(text), g_version_major,
                           g_version_minor, g_error, sizeof(g_error))
             ? 1
             : 0;
}

int32_t mettle_metal_rt_load_file(const char *path) {
  if (!path) {
    rt_error("no Metal module path was given");
    return 0;
  }
  if (!mettle_metal_rt_init()) {
    return 0;
  }
  rt_forget_kernels();
  return mettle_metal_load_file(g_metal, path, g_version_major,
                                g_version_minor, g_error, sizeof(g_error))
             ? 1
             : 0;
}

int64_t mettle_metal_rt_kernel(const char *name) {
  char *copy;
  size_t length;
  if (!name || !g_metal) {
    rt_error("no Metal module is loaded");
    return 0;
  }
  for (size_t i = 0; i < g_kernel_count; i++) {
    if (strcmp(g_kernels[i], name) == 0) {
      return (int64_t)i + 1;
    }
  }
  if (g_kernel_count == METAL_RT_MAX_KERNELS) {
    rt_error("more than 256 distinct Metal kernels were looked up");
    return 0;
  }
  if (!mettle_metal_prepare(g_metal, name, g_error, sizeof(g_error))) {
    return 0;
  }
  length = strlen(name) + 1;
  copy = (char *)malloc(length);
  if (!copy) {
    rt_error("out of memory recording a Metal kernel");
    return 0;
  }
  memcpy(copy, name, length);
  g_kernels[g_kernel_count++] = copy;
  return (int64_t)g_kernel_count;
}

const char *mettle_metal_rt_kernel_name(int64_t handle) {
  if (handle < 1 || (uint64_t)handle > g_kernel_count) {
    return "an unknown kernel handle";
  }
  return g_kernels[handle - 1];
}

int64_t mettle_metal_rt_alloc(int64_t bytes) {
  uint64_t address;
  if (bytes < 0 || !mettle_metal_rt_init()) {
    return 0;
  }
  address = mettle_metal_alloc(g_metal, (size_t)bytes);
  if (!address) {
    snprintf(g_error, sizeof(g_error),
             "Metal could not allocate %lld bytes", (long long)bytes);
  }
  return (int64_t)address;
}

uint8_t *mettle_metal_rt_host(int64_t address) {
  return g_metal ? (uint8_t *)mettle_metal_host(g_metal, (uint64_t)address)
                 : NULL;
}

int32_t mettle_metal_rt_free(int64_t address) {
  if (!g_metal || !mettle_metal_host(g_metal, (uint64_t)address)) {
    return 1;
  }
  mettle_metal_free(g_metal, (uint64_t)address);
  return 0;
}

uint8_t *mettle_metal_rt_alloc_mapped(int64_t bytes, int64_t *device_out) {
  int64_t address = mettle_metal_rt_alloc(bytes);
  if (device_out) {
    *device_out = address;
  }
  return address ? mettle_metal_rt_host(address) : NULL;
}

int32_t mettle_metal_rt_free_mapped(uint8_t *host) {
  uint64_t address = g_metal ? mettle_metal_address_of(g_metal, host) : 0;
  if (!address) {
    return 1;
  }
  return mettle_metal_rt_free((int64_t)address);
}

static int rt_span_ok(int64_t address, int64_t bytes, const char *what) {
  size_t span;
  if (bytes < 0) {
    snprintf(g_error, sizeof(g_error), "%s of a negative byte count", what);
    return 0;
  }
  if (bytes == 0) {
    return 1;
  }
  span = g_metal ? mettle_metal_span(g_metal, (uint64_t)address) : 0;
  if ((uint64_t)bytes > span) {
    snprintf(g_error, sizeof(g_error),
             "%s of %lld bytes at 0x%llx runs past its Metal buffer", what,
             (long long)bytes, (unsigned long long)address);
    return 0;
  }
  return 1;
}

int32_t mettle_metal_rt_to_device(int64_t dst, const uint8_t *src,
                                  int64_t bytes) {
  if (mettle_metal_rt_sync() != 0) {
    return 1;
  }
  if (!rt_span_ok(dst, bytes, "a copy to the GPU")) {
    return 1;
  }
  if (bytes) {
    memcpy(mettle_metal_rt_host(dst), src, (size_t)bytes);
  }
  return 0;
}

int32_t mettle_metal_rt_to_host(uint8_t *dst, int64_t src, int64_t bytes) {
  if (mettle_metal_rt_sync() != 0) {
    return 1;
  }
  if (!rt_span_ok(src, bytes, "a copy from the GPU")) {
    return 1;
  }
  if (bytes) {
    memcpy(dst, mettle_metal_rt_host(src), (size_t)bytes);
  }
  return 0;
}

int32_t mettle_metal_rt_fill(int64_t dst, uint32_t value, int32_t element_bytes,
                             int64_t count) {
  int64_t bytes = count * (int64_t)element_bytes;
  unsigned char *at;
  if (element_bytes != 1 && element_bytes != 2 && element_bytes != 4) {
    return 1;
  }
  if (mettle_metal_rt_sync() != 0 || !rt_span_ok(dst, bytes, "a fill on the GPU")) {
    return 1;
  }
  at = (unsigned char *)mettle_metal_rt_host(dst);
  if (element_bytes == 1) {
    memset(at, (int)(value & 0xFFu), (size_t)bytes);
    return 0;
  }
  for (int64_t i = 0; i < count; i++) {
    memcpy(at + i * element_bytes, &value, (size_t)element_bytes);
  }
  return 0;
}

static size_t rt_align_up(size_t value, size_t alignment) {
  return (value + alignment - 1) / alignment * alignment;
}

static int rt_pack(const int64_t *params, int32_t nargs, unsigned char **block,
                   size_t *block_size) {
  size_t offset = 0;
  size_t max_align = 1;
  unsigned char *bytes;
  *block = NULL;
  *block_size = 0;
  if (nargs <= 0) {
    return 1;
  }
  if (!params) {
    rt_error("a launch with arguments passed no parameter table");
    return 0;
  }
  for (int32_t pass = 0; pass < 2; pass++) {
    offset = 0;
    for (int32_t i = 0; i < nargs; i++) {
      uint64_t word = (uint64_t)params[nargs + i];
      size_t size = (size_t)(word & 0xFFFFFFu);
      size_t alignment = (size_t)((word >> 24) & 0x7Fu);
      if (size == 0 || alignment == 0 || (alignment & (alignment - 1)) ||
          (word >> 31) != 0 || params[i] == 0) {
        snprintf(g_error, sizeof(g_error),
                 "argument %d of the launch has no valid layout word; a "
                 "Metal launch needs nargs pointers followed by nargs "
                 "(alignment << 24 | size) words",
                 (int)i);
        free(*block);
        *block = NULL;
        return 0;
      }
      offset = rt_align_up(offset, alignment);
      if (pass == 1) {
        memcpy(*block + offset, (const void *)(uintptr_t)params[i], size);
      } else if (alignment > max_align) {
        max_align = alignment;
      }
      offset += size;
    }
    if (pass == 0) {
      *block_size = rt_align_up(offset, max_align);
      bytes = (unsigned char *)calloc(*block_size ? *block_size : 1, 1);
      if (!bytes) {
        rt_error("out of memory packing Metal launch arguments");
        return 0;
      }
      *block = bytes;
    }
  }
  return 1;
}

static int rt_capture(const RtLaunch *launch) {
  RtGraph *graph = &g_graphs[g_capture];
  if (graph->count == graph->capacity) {
    size_t next = graph->capacity ? graph->capacity * 2 : 16;
    RtLaunch *grown =
        (RtLaunch *)realloc(graph->launches, next * sizeof(RtLaunch));
    if (!grown) {
      rt_error("out of memory capturing a Metal launch graph");
      return 0;
    }
    graph->launches = grown;
    graph->capacity = next;
  }
  graph->launches[graph->count++] = *launch;
  return 1;
}

static int rt_run(const RtLaunch *launch) {
  const char *name;
  if (launch->op == RT_OP_FLUSH) {
    mettle_metal_flush(g_metal);
    return 1;
  }
  if (launch->op != RT_OP_LAUNCH) {
    mettle_metal_set_barriers(g_metal, launch->op == RT_OP_BARRIERS_ON);
    return 1;
  }
  name = g_kernels[launch->kernel - 1];
  if (launch->counts) {
    return mettle_metal_launch_indirect(
        g_metal, name, launch->counts, launch->block, launch->args,
        launch->args_size, launch->shared_bytes, g_error, sizeof(g_error));
  }
  return mettle_metal_launch(g_metal, name, launch->grid, launch->block,
                             launch->args, launch->args_size,
                             launch->shared_bytes, g_error, sizeof(g_error));
}

static int32_t rt_submit(RtLaunch *launch, const int64_t *params,
                         int32_t nargs) {
  int ok;
  if (!g_metal || launch->kernel < 1 ||
      (uint64_t)launch->kernel > g_kernel_count) {
    rt_error("the launch names no prepared Metal kernel");
    return 1;
  }
  if (!rt_pack(params, nargs, &launch->args, &launch->args_size)) {
    return 1;
  }
  if (g_capturing) {
    if (!rt_capture(launch)) {
      free(launch->args);
      return 1;
    }
    return 0;
  }
  ok = rt_run(launch);
  free(launch->args);
  return ok ? 0 : 1;
}

int32_t mettle_metal_rt_launch(int64_t kernel, int32_t gx, int32_t gy,
                               int32_t gz, int32_t bx, int32_t by, int32_t bz,
                               int32_t shared_bytes, int64_t stream,
                               const int64_t *params, int32_t nargs) {
  RtLaunch launch;
  (void)stream;
  if (gx <= 0 || gy <= 0 || gz <= 0 || bx <= 0 || by <= 0 || bz <= 0 ||
      shared_bytes < 0) {
    rt_error("grid, block and shared bytes must be positive");
    return 1;
  }
  memset(&launch, 0, sizeof(launch));
  launch.kernel = kernel;
  launch.grid[0] = (uint32_t)gx;
  launch.grid[1] = (uint32_t)gy;
  launch.grid[2] = (uint32_t)gz;
  launch.block[0] = (uint32_t)bx;
  launch.block[1] = (uint32_t)by;
  launch.block[2] = (uint32_t)bz;
  launch.shared_bytes = (size_t)shared_bytes;
  return rt_submit(&launch, params, nargs);
}

int32_t mettle_metal_rt_launch_indirect(int64_t kernel, int64_t counts,
                                        int32_t bx, int32_t by, int32_t bz,
                                        int32_t shared_bytes, int64_t stream,
                                        const int64_t *params, int32_t nargs) {
  RtLaunch launch;
  (void)stream;
  if (counts == 0 || bx <= 0 || by <= 0 || bz <= 0 || shared_bytes < 0) {
    rt_error("an indirect launch needs a grid address, and block and shared "
             "bytes must be positive");
    return 1;
  }
  memset(&launch, 0, sizeof(launch));
  launch.kernel = kernel;
  launch.counts = (uint64_t)counts;
  launch.block[0] = (uint32_t)bx;
  launch.block[1] = (uint32_t)by;
  launch.block[2] = (uint32_t)bz;
  launch.shared_bytes = (size_t)shared_bytes;
  return rt_submit(&launch, params, nargs);
}

static int32_t rt_control(int op) {
  RtLaunch control;
  if (g_capturing) {
    memset(&control, 0, sizeof(control));
    control.op = op;
    return rt_capture(&control) ? 0 : 1;
  }
  if (!g_metal) {
    return 0;
  }
  memset(&control, 0, sizeof(control));
  control.op = op;
  rt_run(&control);
  return 0;
}

int32_t mettle_metal_rt_flush(void) { return rt_control(RT_OP_FLUSH); }

int32_t mettle_metal_rt_barriers(int32_t on) {
  return rt_control(on ? RT_OP_BARRIERS_ON : RT_OP_BARRIERS_OFF);
}

double mettle_metal_rt_gpu_ms(void) {
  return g_metal ? mettle_metal_gpu_ms(g_metal) : 0.0;
}

int32_t mettle_metal_rt_capture_begin(void) {
  RtGraph *grown;
  if (g_capturing) {
    rt_error("a Metal launch graph is already being captured");
    return 1;
  }
  grown = (RtGraph *)realloc(g_graphs, (g_graph_count + 1) * sizeof(RtGraph));
  if (!grown) {
    rt_error("out of memory starting a Metal launch graph");
    return 1;
  }
  g_graphs = grown;
  memset(&g_graphs[g_graph_count], 0, sizeof(RtGraph));
  g_graphs[g_graph_count].live = 1;
  g_capture = g_graph_count++;
  g_capturing = 1;
  return 0;
}

int64_t mettle_metal_rt_capture_end(void) {
  if (!g_capturing) {
    rt_error("no Metal launch graph is being captured");
    return 0;
  }
  g_capturing = 0;
  return (int64_t)g_capture + 1;
}

int32_t mettle_metal_rt_graph_launch(int64_t handle) {
  RtGraph *graph;
  if (handle < 1 || (uint64_t)handle > g_graph_count ||
      !g_graphs[handle - 1].live || g_capturing || !g_metal) {
    rt_error("the launch names no captured Metal graph");
    return 1;
  }
  graph = &g_graphs[handle - 1];
  for (size_t i = 0; i < graph->count; i++) {
    RtLaunch *launch = &graph->launches[i];
    if (launch->op == RT_OP_LAUNCH &&
        (uint64_t)launch->kernel > g_kernel_count) {
      rt_error("a captured Metal graph outlived the library it launches");
      return 1;
    }
    if (!rt_run(launch)) {
      return 1;
    }
  }
  return 0;
}

int32_t mettle_metal_rt_graph_destroy(int64_t handle) {
  RtGraph *graph;
  if (handle < 1 || (uint64_t)handle > g_graph_count ||
      !g_graphs[handle - 1].live) {
    return 1;
  }
  graph = &g_graphs[handle - 1];
  for (size_t i = 0; i < graph->count; i++) {
    free(graph->launches[i].args);
  }
  free(graph->launches);
  memset(graph, 0, sizeof(*graph));
  return 0;
}

void mettle_metal_rt_note(const char *text) {
  rt_print(text);
}

void mettle_metal_rt_fail(const char *first, const char *second,
                          const char *third) {
  rt_print(first);
  rt_print(second);
  rt_print(third);
  rt_print("\n");
  exit(1);
}

void mettle_metal_rt_launch_checked(int64_t kernel, int32_t gx, int32_t gy,
                                    int32_t gz, int32_t bx, int32_t by,
                                    int32_t bz, int32_t shared_bytes,
                                    int64_t stream, const int64_t *params,
                                    int32_t nargs) {
  if (mettle_metal_rt_launch(kernel, gx, gy, gz, bx, by, bz, shared_bytes,
                             stream, params, nargs) == 0) {
    return;
  }
  {
    char line[2600];
    snprintf(line, sizeof(line),
             "mettle: GPU launch of %s failed with grid %dx%dx%d, block "
             "%dx%dx%d, %d shared bytes\n  Metal: %s\n",
             mettle_metal_rt_kernel_name(kernel), (int)gx, (int)gy, (int)gz,
             (int)bx, (int)by, (int)bz, (int)shared_bytes, g_error);
    rt_print(line);
  }
  exit(1);
}

void mettle_metal_rt_launch_indirect_checked(int64_t kernel, int64_t counts,
                                             int32_t bx, int32_t by,
                                             int32_t bz, int32_t shared_bytes,
                                             int64_t stream,
                                             const int64_t *params,
                                             int32_t nargs) {
  if (mettle_metal_rt_launch_indirect(kernel, counts, bx, by, bz, shared_bytes,
                                      stream, params, nargs) == 0) {
    return;
  }
  {
    char line[2600];
    snprintf(line, sizeof(line),
             "mettle: GPU launch of %s failed with its grid at 0x%llx, block "
             "%dx%dx%d, %d shared bytes\n  Metal: %s\n",
             mettle_metal_rt_kernel_name(kernel), (unsigned long long)counts,
             (int)bx, (int)by, (int)bz, (int)shared_bytes, g_error);
    rt_print(line);
  }
  exit(1);
}

double mettle_metal_rt_now_ms(void) {
#ifdef _WIN32
  LARGE_INTEGER frequency;
  LARGE_INTEGER counter;
  QueryPerformanceFrequency(&frequency);
  QueryPerformanceCounter(&counter);
  return (double)counter.QuadPart * 1000.0 / (double)frequency.QuadPart;
#else
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (double)now.tv_sec * 1000.0 + (double)now.tv_nsec / 1000000.0;
#endif
}
