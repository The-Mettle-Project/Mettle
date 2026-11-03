#ifndef CODEGEN_BINARY_MIR_EXTERNAL_H
#define CODEGEN_BINARY_MIR_EXTERNAL_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
  int id;
  int rclass;
  int width;
  uint32_t allowed;
  uint32_t saved;
  int cost;
  int hint;
  const int *neighbors;
  size_t neighbor_count;
} MirAllocVreg;

typedef struct {
  const char *function;
  const MirAllocVreg *vregs;
  size_t count;
} MirAllocRequest;

typedef int (*MirExternalAllocator)(const MirAllocRequest *request,
                                    int *out_registers, char *why,
                                    size_t why_size);

void mir_set_external_allocator(MirExternalAllocator allocator,
                                const char *name);

#endif
