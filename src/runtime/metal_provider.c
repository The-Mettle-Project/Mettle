#include "metal_provider.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void *id;
typedef void *SEL;
typedef unsigned long NSUInteger;

typedef struct {
  NSUInteger width;
  NSUInteger height;
  NSUInteger depth;
} MTLSize;

extern id objc_getClass(const char *name);
extern SEL sel_registerName(const char *name);
extern void objc_msgSend(void);
extern void *objc_autoreleasePoolPush(void);
extern void objc_autoreleasePoolPop(void *pool);
extern id MTLCreateSystemDefaultDevice(void);

static void *msg_send(void) { return (void *)objc_msgSend; }

#define SEL_OF(name) sel_registerName(name)
#define SEND0(type, object, name) \
  ((type(*)(id, SEL))msg_send())((object), SEL_OF(name))

typedef struct {
  uint64_t address;
  size_t size;
  id buffer;
  unsigned char *host;
} MetalBuffer;

typedef struct {
  char *name;
  id pipeline;
  NSUInteger max_threads;
} MetalPipeline;

struct MettleMetal {
  id device;
  id queue;
  id library;
  char device_name[256];
  MetalBuffer *buffers;
  size_t buffer_count;
  size_t buffer_capacity;
  MetalPipeline *pipelines;
  size_t pipeline_count;
  size_t pipeline_capacity;
  id command;
  id encoder;
  size_t batched;
  char first_kernel[128];
  char last_kernel[128];
  id *spills;
  size_t spill_count;
  size_t spill_capacity;
  id *resident;
  size_t resident_capacity;
  int synchronous;
  char deferred_error[512];
};

#define METAL_BATCH_LIMIT 512

static void put_error(char *error, size_t error_size, const char *text) {
  if (error && error_size) {
    snprintf(error, error_size, "%s", text);
  }
}

static id ns_string(const char *text) {
  return ((id(*)(id, SEL, const char *))msg_send())(
      objc_getClass("NSString"), SEL_OF("stringWithUTF8String:"), text);
}

static const char *ns_text(id string) {
  return string ? SEND0(const char *, string, "UTF8String") : "";
}

static const char *ns_error_text(id error) {
  return error ? ns_text(SEND0(id, error, "localizedDescription"))
               : "unknown Metal error";
}

static int responds(id object, const char *selector) {
  return ((signed char (*)(id, SEL, SEL))msg_send())(
             object, SEL_OF("respondsToSelector:"), SEL_OF(selector)) != 0;
}

static void release(id object) {
  if (object) {
    SEND0(void, object, "release");
  }
}

MettleMetal *mettle_metal_open(char *error, size_t error_size) {
  MettleMetal *metal = (MettleMetal *)calloc(1, sizeof(MettleMetal));
  void *pool;
  if (!metal) {
    put_error(error, error_size, "out of memory opening Metal");
    return NULL;
  }
  pool = objc_autoreleasePoolPush();
  metal->device = MTLCreateSystemDefaultDevice();
  if (!metal->device) {
    objc_autoreleasePoolPop(pool);
    free(metal);
    put_error(error, error_size, "this machine has no Metal device");
    return NULL;
  }
  if (!responds(metal->device, "supportsFamily:") ||
      !((signed char (*)(id, SEL, long))msg_send())(
          metal->device, SEL_OF("supportsFamily:"), 5001L)) {
    objc_autoreleasePoolPop(pool);
    release(metal->device);
    free(metal);
    put_error(error, error_size,
              "Mettle kernels need a Metal 3 GPU (Apple silicon or a recent "
              "AMD/Intel Mac GPU) for 64-bit GPU addresses");
    return NULL;
  }
  metal->queue = SEND0(id, metal->device, "newCommandQueue");
  snprintf(metal->device_name, sizeof(metal->device_name), "%s",
           ns_text(SEND0(id, metal->device, "name")));
  metal->synchronous = getenv("METTLE_METAL_SYNC") &&
                       getenv("METTLE_METAL_SYNC")[0] == '1';
  objc_autoreleasePoolPop(pool);
  return metal;
}

static void release_spills(MettleMetal *metal) {
  for (size_t i = 0; i < metal->spill_count; i++) {
    release(metal->spills[i]);
  }
  metal->spill_count = 0;
}

int mettle_metal_sync(MettleMetal *metal, char *error, size_t error_size) {
  long status;
  int ok = 1;
  void *pool;
  if (!metal) {
    return 1;
  }
  if (metal->deferred_error[0]) {
    put_error(error, error_size, metal->deferred_error);
    metal->deferred_error[0] = '\0';
    ok = 0;
  }
  if (!metal->command) {
    return ok;
  }
  pool = objc_autoreleasePoolPush();
  SEND0(void, metal->encoder, "endEncoding");
  SEND0(void, metal->command, "commit");
  SEND0(void, metal->command, "waitUntilCompleted");
  status = SEND0(long, metal->command, "status");
  if (status != 4 && ok) {
    if (error && error_size) {
      snprintf(error, error_size,
               "a Metal command buffer of %lu launches (%s through %s) ended "
               "with status %ld: %s",
               (unsigned long)metal->batched, metal->first_kernel,
               metal->last_kernel, status,
               ns_error_text(SEND0(id, metal->command, "error")));
    }
    ok = 0;
  }
  release(metal->encoder);
  release(metal->command);
  metal->encoder = NULL;
  metal->command = NULL;
  metal->batched = 0;
  release_spills(metal);
  objc_autoreleasePoolPop(pool);
  return ok;
}

static void sync_deferred(MettleMetal *metal) {
  char text[512];
  if (metal->command && !mettle_metal_sync(metal, text, sizeof(text)) &&
      !metal->deferred_error[0]) {
    snprintf(metal->deferred_error, sizeof(metal->deferred_error), "%s", text);
  }
}

const char *mettle_metal_device_name(const MettleMetal *metal) {
  return metal ? metal->device_name : "";
}

int mettle_metal_load(MettleMetal *metal, const char *source, size_t length,
                      int version_major, int version_minor, char *error,
                      size_t error_size) {
  char *text;
  id options;
  id ns_error = NULL;
  void *pool;
  if (!metal || !source) {
    put_error(error, error_size, "no Metal device or source");
    return 0;
  }
  sync_deferred(metal);
  text = (char *)malloc(length + 1);
  if (!text) {
    put_error(error, error_size, "out of memory loading Metal source");
    return 0;
  }
  memcpy(text, source, length);
  text[length] = '\0';
  pool = objc_autoreleasePoolPush();
  options = SEND0(id, SEND0(id, objc_getClass("MTLCompileOptions"), "alloc"),
                  "init");
  ((void (*)(id, SEL, NSUInteger))msg_send())(
      options, SEL_OF("setLanguageVersion:"),
      (NSUInteger)(((unsigned)version_major << 16) | (unsigned)version_minor));
  if (responds(options, "setMathMode:")) {
    ((void (*)(id, SEL, long))msg_send())(options, SEL_OF("setMathMode:"), 0L);
  } else {
    ((void (*)(id, SEL, signed char))msg_send())(
        options, SEL_OF("setFastMathEnabled:"), 0);
  }
  if (responds(options, "setEnableLogging:")) {
    ((void (*)(id, SEL, signed char))msg_send())(
        options, SEL_OF("setEnableLogging:"), 1);
  }
  release(metal->library);
  metal->library = ((id(*)(id, SEL, id, id, id *))msg_send())(
      metal->device, SEL_OF("newLibraryWithSource:options:error:"),
      ns_string(text), options, &ns_error);
  free(text);
  release(options);
  if (!metal->library) {
    if (error && error_size) {
      snprintf(error, error_size, "Metal rejected the library: %s",
               ns_error_text(ns_error));
    }
    objc_autoreleasePoolPop(pool);
    return 0;
  }
  for (size_t i = 0; i < metal->pipeline_count; i++) {
    free(metal->pipelines[i].name);
    release(metal->pipelines[i].pipeline);
  }
  metal->pipeline_count = 0;
  objc_autoreleasePoolPop(pool);
  return 1;
}

int mettle_metal_load_file(MettleMetal *metal, const char *path,
                           int version_major, int version_minor, char *error,
                           size_t error_size) {
  FILE *file = fopen(path, "rb");
  char *data;
  long size;
  int ok;
  if (!file) {
    if (error && error_size) {
      snprintf(error, error_size, "cannot open %s", path);
    }
    return 0;
  }
  fseek(file, 0, SEEK_END);
  size = ftell(file);
  fseek(file, 0, SEEK_SET);
  data = (char *)malloc((size_t)size + 1);
  if (!data || fread(data, 1, (size_t)size, file) != (size_t)size) {
    fclose(file);
    free(data);
    put_error(error, error_size, "cannot read the Metal source");
    return 0;
  }
  fclose(file);
  ok = mettle_metal_load(metal, data, (size_t)size, version_major,
                         version_minor, error, error_size);
  free(data);
  return ok;
}

static MetalBuffer *find_buffer(MettleMetal *metal, uint64_t address) {
  for (size_t i = 0; i < metal->buffer_count; i++) {
    MetalBuffer *buffer = &metal->buffers[i];
    if (address >= buffer->address &&
        address - buffer->address < (uint64_t)(buffer->size ? buffer->size : 1)) {
      return buffer;
    }
  }
  return NULL;
}

uint64_t mettle_metal_alloc(MettleMetal *metal, size_t size) {
  id buffer;
  MetalBuffer *entry;
  if (!metal) {
    return 0;
  }
  if (metal->buffer_count == metal->buffer_capacity) {
    size_t next = metal->buffer_capacity ? metal->buffer_capacity * 2 : 32;
    MetalBuffer *grown =
        (MetalBuffer *)realloc(metal->buffers, next * sizeof(MetalBuffer));
    if (!grown) {
      return 0;
    }
    metal->buffers = grown;
    metal->buffer_capacity = next;
  }
  buffer = ((id(*)(id, SEL, NSUInteger, NSUInteger))msg_send())(
      metal->device, SEL_OF("newBufferWithLength:options:"),
      (NSUInteger)(size ? size : 16), (NSUInteger)0);
  if (!buffer) {
    return 0;
  }
  entry = &metal->buffers[metal->buffer_count++];
  entry->buffer = buffer;
  entry->size = size ? size : 16;
  entry->host = SEND0(unsigned char *, buffer, "contents");
  entry->address = SEND0(uint64_t, buffer, "gpuAddress");
  memset(entry->host, 0, entry->size);
  return entry->address;
}

void *mettle_metal_host(MettleMetal *metal, uint64_t address) {
  MetalBuffer *buffer = metal ? find_buffer(metal, address) : NULL;
  return buffer ? buffer->host + (address - buffer->address) : NULL;
}

void mettle_metal_free(MettleMetal *metal, uint64_t address) {
  if (!metal) {
    return;
  }
  sync_deferred(metal);
  for (size_t i = 0; i < metal->buffer_count; i++) {
    if (metal->buffers[i].address == address) {
      release(metal->buffers[i].buffer);
      metal->buffers[i] = metal->buffers[metal->buffer_count - 1];
      metal->buffer_count--;
      return;
    }
  }
}

static MetalPipeline *pipeline_for(MettleMetal *metal, const char *kernel,
                                   char *error, size_t error_size) {
  id function;
  id pipeline;
  id ns_error = NULL;
  MetalPipeline *entry;
  for (size_t i = 0; i < metal->pipeline_count; i++) {
    if (!strcmp(metal->pipelines[i].name, kernel)) {
      return &metal->pipelines[i];
    }
  }
  if (!metal->library) {
    put_error(error, error_size, "no Metal library is loaded");
    return NULL;
  }
  function = ((id(*)(id, SEL, id))msg_send())(
      metal->library, SEL_OF("newFunctionWithName:"), ns_string(kernel));
  if (!function) {
    if (error && error_size) {
      snprintf(error, error_size, "the Metal library has no kernel '%s'",
               kernel);
    }
    return NULL;
  }
  pipeline = ((id(*)(id, SEL, id, id *))msg_send())(
      metal->device, SEL_OF("newComputePipelineStateWithFunction:error:"),
      function, &ns_error);
  release(function);
  if (!pipeline) {
    if (error && error_size) {
      snprintf(error, error_size, "Metal could not build '%s': %s", kernel,
               ns_error_text(ns_error));
    }
    return NULL;
  }
  if (metal->pipeline_count == metal->pipeline_capacity) {
    size_t next = metal->pipeline_capacity ? metal->pipeline_capacity * 2 : 16;
    MetalPipeline *grown = (MetalPipeline *)realloc(
        metal->pipelines, next * sizeof(MetalPipeline));
    if (!grown) {
      release(pipeline);
      return NULL;
    }
    metal->pipelines = grown;
    metal->pipeline_capacity = next;
  }
  entry = &metal->pipelines[metal->pipeline_count++];
  entry->name = (char *)malloc(strlen(kernel) + 1);
  if (entry->name) {
    memcpy(entry->name, kernel, strlen(kernel) + 1);
  }
  entry->pipeline = pipeline;
  entry->max_threads =
      SEND0(NSUInteger, pipeline, "maxTotalThreadsPerThreadgroup");
  return entry;
}

int mettle_metal_prepare(MettleMetal *metal, const char *kernel, char *error,
                         size_t error_size) {
  void *pool;
  int ok;
  if (!metal || !kernel) {
    put_error(error, error_size, "no Metal device or kernel");
    return 0;
  }
  pool = objc_autoreleasePoolPush();
  ok = pipeline_for(metal, kernel, error, error_size) != NULL;
  objc_autoreleasePoolPop(pool);
  return ok;
}

int64_t mettle_metal_property(MettleMetal *metal, int which) {
  if (!metal) {
    return 0;
  }
  switch (which) {
  case METTLE_METAL_PROPERTY_MAX_THREADS:
    return 1024;
  case METTLE_METAL_PROPERTY_THREADGROUP_MEMORY:
    return (int64_t)SEND0(NSUInteger, metal->device,
                          "maxThreadgroupMemoryLength");
  case METTLE_METAL_PROPERTY_WORKING_SET:
    return (int64_t)SEND0(uint64_t, metal->device,
                          "recommendedMaxWorkingSetSize");
  case METTLE_METAL_PROPERTY_UNIFIED_MEMORY:
    return SEND0(signed char, metal->device, "hasUnifiedMemory") ? 1 : 0;
  case METTLE_METAL_PROPERTY_SIMD_WIDTH:
    return 32;
  default:
    return 0;
  }
}

size_t mettle_metal_span(MettleMetal *metal, uint64_t address) {
  MetalBuffer *buffer = metal ? find_buffer(metal, address) : NULL;
  return buffer ? buffer->size - (size_t)(address - buffer->address) : 0;
}

uint64_t mettle_metal_address_of(MettleMetal *metal, const void *host) {
  const unsigned char *p = (const unsigned char *)host;
  if (!metal || !p) {
    return 0;
  }
  for (size_t i = 0; i < metal->buffer_count; i++) {
    MetalBuffer *buffer = &metal->buffers[i];
    if (p >= buffer->host && p < buffer->host + buffer->size) {
      return buffer->address + (uint64_t)(p - buffer->host);
    }
  }
  return 0;
}

static int grow_launch_tables(MettleMetal *metal, char *error,
                              size_t error_size) {
  if (metal->spill_count == metal->spill_capacity) {
    size_t next = metal->spill_capacity ? metal->spill_capacity * 2 : 16;
    id *grown = (id *)realloc(metal->spills, next * sizeof(id));
    if (!grown) {
      put_error(error, error_size, "out of memory batching Metal launches");
      return 0;
    }
    metal->spills = grown;
    metal->spill_capacity = next;
  }
  if (metal->buffer_count > metal->resident_capacity) {
    size_t next = metal->buffer_count * 2;
    id *grown = (id *)realloc(metal->resident, next * sizeof(id));
    if (!grown) {
      put_error(error, error_size, "out of memory batching Metal launches");
      return 0;
    }
    metal->resident = grown;
    metal->resident_capacity = next;
  }
  return 1;
}

static void bind_arguments(MettleMetal *metal, const void *args,
                           size_t args_size) {
  id spill;
  if (!args || !args_size) {
    return;
  }
  if (args_size <= 4096) {
    ((void (*)(id, SEL, const void *, NSUInteger, NSUInteger))msg_send())(
        metal->encoder, SEL_OF("setBytes:length:atIndex:"), args,
        (NSUInteger)args_size, (NSUInteger)0);
    return;
  }
  spill = ((id(*)(id, SEL, const void *, NSUInteger, NSUInteger))msg_send())(
      metal->device, SEL_OF("newBufferWithBytes:length:options:"), args,
      (NSUInteger)args_size, (NSUInteger)0);
  metal->spills[metal->spill_count++] = spill;
  ((void (*)(id, SEL, id, NSUInteger, NSUInteger))msg_send())(
      metal->encoder, SEL_OF("setBuffer:offset:atIndex:"), spill,
      (NSUInteger)0, (NSUInteger)0);
}

static void use_buffers(MettleMetal *metal) {
  if (!metal->buffer_count) {
    return;
  }
  for (size_t i = 0; i < metal->buffer_count; i++) {
    metal->resident[i] = metal->buffers[i].buffer;
  }
  ((void (*)(id, SEL, const id *, NSUInteger, NSUInteger))msg_send())(
      metal->encoder, SEL_OF("useResources:count:usage:"), metal->resident,
      (NSUInteger)metal->buffer_count, (NSUInteger)3);
}

static void open_encoder(MettleMetal *metal, const char *kernel) {
  if (!metal->command) {
    metal->command = SEND0(id, metal->queue, "commandBuffer");
    SEND0(id, metal->command, "retain");
    metal->encoder = SEND0(id, metal->command, "computeCommandEncoder");
    SEND0(id, metal->encoder, "retain");
    snprintf(metal->first_kernel, sizeof(metal->first_kernel), "%s", kernel);
  } else {
    ((void (*)(id, SEL, NSUInteger))msg_send())(
        metal->encoder, SEL_OF("memoryBarrierWithScope:"), (NSUInteger)1);
  }
  snprintf(metal->last_kernel, sizeof(metal->last_kernel), "%s", kernel);
}

static int encode_launch(MettleMetal *metal, const char *kernel,
                         const uint32_t block[3], const void *args,
                         size_t args_size, size_t threadgroup_bytes,
                         char *error, size_t error_size) {
  MetalPipeline *pipeline = pipeline_for(metal, kernel, error, error_size);
  unsigned long long block_threads;
  if (!pipeline) {
    return 0;
  }
  block_threads = (unsigned long long)block[0] * block[1] * block[2];
  if (block_threads == 0 || block_threads > pipeline->max_threads) {
    if (error && error_size) {
      snprintf(error, error_size,
               "'%s' launched with %llu threads per threadgroup; this GPU "
               "runs at most %lu for it",
               kernel, block_threads, (unsigned long)pipeline->max_threads);
    }
    return 0;
  }
  if (!grow_launch_tables(metal, error, error_size)) {
    return 0;
  }
  open_encoder(metal, kernel);
  ((void (*)(id, SEL, id))msg_send())(
      metal->encoder, SEL_OF("setComputePipelineState:"), pipeline->pipeline);
  bind_arguments(metal, args, args_size);
  use_buffers(metal);
  if (threadgroup_bytes) {
    ((void (*)(id, SEL, NSUInteger, NSUInteger))msg_send())(
        metal->encoder, SEL_OF("setThreadgroupMemoryLength:atIndex:"),
        (NSUInteger)((threadgroup_bytes + 15) / 16 * 16), (NSUInteger)0);
  }
  return 1;
}

static MTLSize size_of(const uint32_t v[3]) {
  MTLSize size;
  size.width = v[0];
  size.height = v[1];
  size.depth = v[2];
  return size;
}

static int finish_launch(MettleMetal *metal, void *pool, char *error,
                         size_t error_size) {
  metal->batched++;
  objc_autoreleasePoolPop(pool);
  if (metal->synchronous || metal->batched >= METAL_BATCH_LIMIT) {
    return mettle_metal_sync(metal, error, error_size);
  }
  return 1;
}

int mettle_metal_launch(MettleMetal *metal, const char *kernel,
                        const uint32_t grid[3], const uint32_t block[3],
                        const void *args, size_t args_size,
                        size_t threadgroup_bytes, char *error,
                        size_t error_size) {
  void *pool;
  if (!metal || !kernel) {
    put_error(error, error_size, "no Metal device or kernel");
    return 0;
  }
  pool = objc_autoreleasePoolPush();
  if (!encode_launch(metal, kernel, block, args, args_size, threadgroup_bytes,
                     error, error_size)) {
    objc_autoreleasePoolPop(pool);
    return 0;
  }
  ((void (*)(id, SEL, MTLSize, MTLSize))msg_send())(
      metal->encoder, SEL_OF("dispatchThreadgroups:threadsPerThreadgroup:"),
      size_of(grid), size_of(block));
  return finish_launch(metal, pool, error, error_size);
}

int mettle_metal_launch_indirect(MettleMetal *metal, const char *kernel,
                                 uint64_t counts, const uint32_t block[3],
                                 const void *args, size_t args_size,
                                 size_t threadgroup_bytes, char *error,
                                 size_t error_size) {
  MetalBuffer *buffer;
  uint64_t offset;
  void *pool;
  if (!metal || !kernel) {
    put_error(error, error_size, "no Metal device or kernel");
    return 0;
  }
  buffer = find_buffer(metal, counts);
  offset = buffer ? counts - buffer->address : 0;
  if (!buffer || (counts & 3u) || buffer->size - offset < 12) {
    if (error && error_size) {
      snprintf(error, error_size,
               "the grid of '%s' is read from 0x%llx, which is not a "
               "4-byte aligned run of three uint32 inside a Metal buffer",
               kernel, (unsigned long long)counts);
    }
    return 0;
  }
  pool = objc_autoreleasePoolPush();
  if (!encode_launch(metal, kernel, block, args, args_size, threadgroup_bytes,
                     error, error_size)) {
    objc_autoreleasePoolPop(pool);
    return 0;
  }
  ((void (*)(id, SEL, id, NSUInteger, MTLSize))msg_send())(
      metal->encoder,
      SEL_OF("dispatchThreadgroupsWithIndirectBuffer:indirectBufferOffset:"
             "threadsPerThreadgroup:"),
      buffer->buffer, (NSUInteger)offset, size_of(block));
  return finish_launch(metal, pool, error, error_size);
}

void mettle_metal_close(MettleMetal *metal) {
  if (!metal) {
    return;
  }
  sync_deferred(metal);
  free(metal->spills);
  free(metal->resident);
  for (size_t i = 0; i < metal->buffer_count; i++) {
    release(metal->buffers[i].buffer);
  }
  for (size_t i = 0; i < metal->pipeline_count; i++) {
    free(metal->pipelines[i].name);
    release(metal->pipelines[i].pipeline);
  }
  free(metal->buffers);
  free(metal->pipelines);
  release(metal->library);
  release(metal->queue);
  release(metal->device);
  free(metal);
}
