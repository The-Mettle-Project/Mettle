#ifndef METTLE_LIVE_PATCH_H
#define METTLE_LIVE_PATCH_H

#include "linker/symbol_resolve.h"

#include <stddef.h>
#include <stdint.h>

#define LIVE_PATCH_MAGIC 0x48435441504C544Dull
#define LIVE_PATCH_VERSION 1u
#define LIVE_PATCH_HEADER_SIZE 64u
#define LIVE_FIXUP_ABS64 1u
#define LIVE_FIXUP_PC32 2u
#define LIVE_FIXUP_SITE_HOST 0x100u
#define LIVE_FIXUP_TARGET_HOST 0x200u
#define LIVE_SPARE_SLOT_COUNT 4096u
#define LIVE_ARENA_SIZE 1048576u

typedef struct {
  unsigned patch_index;
  size_t changed_functions;
  size_t new_functions;
  size_t new_globals;
  size_t blob_size;
  char patch_path[1024];
  char names[1024];
  char running[512];
} LivePatchSummary;

int live_write_state(const LinkResolution *resolution, uint64_t image_base,
                     const char *executable_path, char **error_message_out);

int live_build_patch(const char *object_path, const char *executable_path,
                     LivePatchSummary *summary, char **error_message_out);

#endif
