#ifndef METTLE_METAL_PROVIDER_H
#define METTLE_METAL_PROVIDER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MettleMetal MettleMetal;

#define METTLE_METAL_PROPERTY_MAX_THREADS 0
#define METTLE_METAL_PROPERTY_THREADGROUP_MEMORY 1
#define METTLE_METAL_PROPERTY_WORKING_SET 2
#define METTLE_METAL_PROPERTY_UNIFIED_MEMORY 3
#define METTLE_METAL_PROPERTY_SIMD_WIDTH 4

MettleMetal *mettle_metal_open(char *error, size_t error_size);
void mettle_metal_close(MettleMetal *metal);
const char *mettle_metal_device_name(const MettleMetal *metal);

int mettle_metal_load(MettleMetal *metal, const char *source, size_t length,
                      int version_major, int version_minor, char *error,
                      size_t error_size);
int mettle_metal_load_file(MettleMetal *metal, const char *path,
                           int version_major, int version_minor, char *error,
                           size_t error_size);

uint64_t mettle_metal_alloc(MettleMetal *metal, size_t size);
void *mettle_metal_host(MettleMetal *metal, uint64_t address);
void mettle_metal_free(MettleMetal *metal, uint64_t address);

int mettle_metal_prepare(MettleMetal *metal, const char *kernel, char *error,
                         size_t error_size);
int64_t mettle_metal_property(MettleMetal *metal, int which);
size_t mettle_metal_span(MettleMetal *metal, uint64_t address);
uint64_t mettle_metal_address_of(MettleMetal *metal, const void *host);

int mettle_metal_sync(MettleMetal *metal, char *error, size_t error_size);
void mettle_metal_flush(MettleMetal *metal);
void mettle_metal_set_barriers(MettleMetal *metal, int on);
double mettle_metal_gpu_ms(const MettleMetal *metal);

int mettle_metal_launch(MettleMetal *metal, const char *kernel,
                        const uint32_t grid[3], const uint32_t block[3],
                        const void *args, size_t args_size,
                        size_t threadgroup_bytes, char *error,
                        size_t error_size);
int mettle_metal_launch_indirect(MettleMetal *metal, const char *kernel,
                                 uint64_t counts, const uint32_t block[3],
                                 const void *args, size_t args_size,
                                 size_t threadgroup_bytes, char *error,
                                 size_t error_size);

#ifdef __cplusplus
}
#endif

#endif
