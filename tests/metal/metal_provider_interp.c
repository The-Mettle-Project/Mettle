#include "../../src/runtime/metal_provider.h"
#include "msl_interp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  unsigned char *host;
  size_t size;
  uint64_t address;
  int live;
} InterpBuffer;

struct MettleMetal {
  MslProgram *program;
  char *source;
  size_t source_length;
  InterpBuffer *buffers;
  size_t buffer_count;
  size_t buffer_capacity;
};

static void interp_error(char *error, size_t error_size, const char *text) {
  if (error && error_size) {
    snprintf(error, error_size, "%s", text);
  }
}

MettleMetal *mettle_metal_open(char *error, size_t error_size) {
  MettleMetal *metal = (MettleMetal *)calloc(1, sizeof(MettleMetal));
  if (!metal) {
    interp_error(error, error_size, "out of memory opening the interpreter");
  }
  return metal;
}

void mettle_metal_close(MettleMetal *metal) {
  if (!metal) {
    return;
  }
  for (size_t i = 0; i < metal->buffer_count; i++) {
    free(metal->buffers[i].host);
  }
  free(metal->buffers);
  free(metal->source);
  msl_program_free(metal->program);
  free(metal);
}

const char *mettle_metal_device_name(const MettleMetal *metal) {
  (void)metal;
  return "Mettle MSL interpreter";
}

int mettle_metal_load(MettleMetal *metal, const char *source, size_t length,
                      int version_major, int version_minor, char *error,
                      size_t error_size) {
  MslProgram *program;
  char *copy;
  (void)version_major;
  (void)version_minor;
  if (!metal || !source) {
    interp_error(error, error_size, "no interpreter or source");
    return 0;
  }
  program = msl_program_load(source, length, error, error_size);
  if (!program) {
    return 0;
  }
  for (size_t i = 0; i < metal->buffer_count; i++) {
    InterpBuffer *buffer = &metal->buffers[i];
    uint64_t address = msl_device_register(program, buffer->host, buffer->size);
    if (address != buffer->address) {
      msl_program_free(program);
      interp_error(error, error_size,
                   "the interpreter cannot reload a library after buffers "
                   "were allocated at other addresses");
      return 0;
    }
  }
  copy = (char *)malloc(length + 1);
  if (!copy) {
    msl_program_free(program);
    interp_error(error, error_size, "out of memory keeping the source");
    return 0;
  }
  memcpy(copy, source, length);
  copy[length] = '\0';
  msl_program_free(metal->program);
  free(metal->source);
  metal->program = program;
  metal->source = copy;
  metal->source_length = length;
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
    interp_error(error, error_size, "cannot read the Metal source");
    return 0;
  }
  fclose(file);
  ok = mettle_metal_load(metal, data, (size_t)size, version_major,
                         version_minor, error, error_size);
  free(data);
  return ok;
}

int mettle_metal_prepare(MettleMetal *metal, const char *kernel, char *error,
                         size_t error_size) {
  char pattern[512];
  if (!metal || !metal->source || !kernel) {
    interp_error(error, error_size, "no Metal library is loaded");
    return 0;
  }
  snprintf(pattern, sizeof(pattern), "kernel void %s(", kernel);
  if (!strstr(metal->source, pattern)) {
    if (error && error_size) {
      snprintf(error, error_size, "the Metal library has no kernel '%s'",
               kernel);
    }
    return 0;
  }
  return 1;
}

int64_t mettle_metal_property(MettleMetal *metal, int which) {
  (void)metal;
  switch (which) {
  case METTLE_METAL_PROPERTY_MAX_THREADS:
    return 1024;
  case METTLE_METAL_PROPERTY_THREADGROUP_MEMORY:
    return 32768;
  case METTLE_METAL_PROPERTY_WORKING_SET:
    return (int64_t)1 << 30;
  case METTLE_METAL_PROPERTY_UNIFIED_MEMORY:
    return 1;
  case METTLE_METAL_PROPERTY_SIMD_WIDTH:
    return 32;
  default:
    return 0;
  }
}

static InterpBuffer *interp_find(MettleMetal *metal, uint64_t address) {
  for (size_t i = 0; i < metal->buffer_count; i++) {
    InterpBuffer *buffer = &metal->buffers[i];
    if (buffer->live && address >= buffer->address &&
        address - buffer->address < (uint64_t)buffer->size) {
      return buffer;
    }
  }
  return NULL;
}

uint64_t mettle_metal_alloc(MettleMetal *metal, size_t size) {
  InterpBuffer *buffer;
  size_t bytes = size ? size : 16;
  if (!metal || !metal->program) {
    return 0;
  }
  if (metal->buffer_count == metal->buffer_capacity) {
    size_t next = metal->buffer_capacity ? metal->buffer_capacity * 2 : 32;
    InterpBuffer *grown =
        (InterpBuffer *)realloc(metal->buffers, next * sizeof(InterpBuffer));
    if (!grown) {
      return 0;
    }
    metal->buffers = grown;
    metal->buffer_capacity = next;
  }
  buffer = &metal->buffers[metal->buffer_count];
  buffer->host = (unsigned char *)calloc(bytes, 1);
  if (!buffer->host) {
    return 0;
  }
  buffer->size = bytes;
  buffer->live = 1;
  buffer->address = msl_device_register(metal->program, buffer->host, bytes);
  if (!buffer->address) {
    free(buffer->host);
    return 0;
  }
  metal->buffer_count++;
  return buffer->address;
}

void *mettle_metal_host(MettleMetal *metal, uint64_t address) {
  InterpBuffer *buffer = metal ? interp_find(metal, address) : NULL;
  return buffer ? buffer->host + (address - buffer->address) : NULL;
}

void mettle_metal_free(MettleMetal *metal, uint64_t address) {
  InterpBuffer *buffer = metal ? interp_find(metal, address) : NULL;
  if (buffer && buffer->address == address) {
    buffer->live = 0;
  }
}

size_t mettle_metal_span(MettleMetal *metal, uint64_t address) {
  InterpBuffer *buffer = metal ? interp_find(metal, address) : NULL;
  return buffer ? buffer->size - (size_t)(address - buffer->address) : 0;
}

uint64_t mettle_metal_address_of(MettleMetal *metal, const void *host) {
  const unsigned char *p = (const unsigned char *)host;
  if (!metal || !p) {
    return 0;
  }
  for (size_t i = 0; i < metal->buffer_count; i++) {
    InterpBuffer *buffer = &metal->buffers[i];
    if (buffer->live && p >= buffer->host && p < buffer->host + buffer->size) {
      return buffer->address + (uint64_t)(p - buffer->host);
    }
  }
  return 0;
}

int mettle_metal_sync(MettleMetal *metal, char *error, size_t error_size) {
  (void)metal;
  (void)error;
  (void)error_size;
  return 1;
}

int mettle_metal_launch(MettleMetal *metal, const char *kernel,
                        const uint32_t grid[3], const uint32_t block[3],
                        const void *args, size_t args_size,
                        size_t threadgroup_bytes, char *error,
                        size_t error_size) {
  MslDispatch dispatch;
  const char *spurious = getenv("METTLE_METAL_SPURIOUS_CAS");
  if (!metal || !metal->program) {
    interp_error(error, error_size, "no Metal library is loaded");
    return 0;
  }
  memset(&dispatch, 0, sizeof(dispatch));
  dispatch.kernel = kernel;
  memcpy(dispatch.grid, grid, sizeof(dispatch.grid));
  memcpy(dispatch.block, block, sizeof(dispatch.block));
  dispatch.args = args;
  dispatch.args_size = args_size;
  dispatch.threadgroup_bytes = threadgroup_bytes;
  dispatch.simd_width = 32;
  dispatch.spurious_cas = spurious && spurious[0] == '1';
  return msl_dispatch(metal->program, &dispatch, error, error_size);
}

int mettle_metal_launch_indirect(MettleMetal *metal, const char *kernel,
                                 uint64_t counts, const uint32_t block[3],
                                 const void *args, size_t args_size,
                                 size_t threadgroup_bytes, char *error,
                                 size_t error_size) {
  uint32_t grid[3];
  const unsigned char *host =
      metal && mettle_metal_span(metal, counts) >= sizeof(grid)
          ? (const unsigned char *)mettle_metal_host(metal, counts)
          : NULL;
  if (!host || (counts & 3u)) {
    interp_error(error, error_size,
                 "an indirect grid is not three aligned uint32 in a buffer");
    return 0;
  }
  memcpy(grid, host, sizeof(grid));
  if (!grid[0] || !grid[1] || !grid[2]) {
    return 1;
  }
  return mettle_metal_launch(metal, kernel, grid, block, args, args_size,
                             threadgroup_bytes, error, error_size);
}

void mettle_metal_flush(MettleMetal *metal) { (void)metal; }

void mettle_metal_set_barriers(MettleMetal *metal, int on) {
  (void)metal;
  (void)on;
}

double mettle_metal_gpu_ms(const MettleMetal *metal) {
  (void)metal;
  return 0.0;
}
