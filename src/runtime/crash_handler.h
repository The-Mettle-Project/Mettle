#ifndef METTLE_CRASH_HANDLER_H
#define METTLE_CRASH_HANDLER_H

#include <stddef.h>
#include <stdint.h>

#define METTLE_CRASH_DEBUG_MAGIC 0x4742544Du
#define METTLE_CRASH_DEBUG_VERSION 1u

typedef enum {
  METTLE_CRASH_TRAP_UNKNOWN = 0,
  METTLE_CRASH_TRAP_NULL_DEREF = 1,
  METTLE_CRASH_TRAP_ARRAY_BOUNDS = 2,
} MettleCrashTrapKind;

typedef struct {
  uint32_t magic;
  uint32_t version;
  uint32_t function_count;
  uint32_t location_count;
  uint32_t trap_site_count;
  uint32_t flags;
  uint32_t reserved0;
  uint32_t reserved1;
} MettleCrashDebugHeader;

typedef struct {
  const void *start_address;
  const void *end_address;
  const char *function_name;
  const char *filename;
  uintptr_t line;
  uintptr_t column;
} MettleCrashFunctionInfo;

typedef struct {
  const void *address;
  const char *function_name;
  const char *filename;
  uintptr_t line;
  uintptr_t column;
} MettleCrashLocationInfo;

typedef struct {
  const void *address;
  uint32_t kind;
  uint32_t reserved;
  const char *function_name;
  const char *filename;
  uintptr_t line;
  uintptr_t column;
  const char *source_line;
  const char *message_template;
  const char *static_context;
} MettleCrashTrapSiteInfo;

typedef struct {
  const MettleCrashDebugHeader *header;
  const MettleCrashFunctionInfo *functions;
  const MettleCrashLocationInfo *locations;
  const MettleCrashTrapSiteInfo *trap_sites;
} MettleCrashDebugImage;

#if defined(_WIN32) || defined(_WIN64)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
const char *mettle_crash_exception_name(DWORD code);
#endif

void mettle_crash_install(void);
void mettle_crash_register_image(const MettleCrashFunctionInfo *functions,
                                 size_t function_count,
                                 const MettleCrashLocationInfo *locations,
                                 size_t location_count);
void mettle_crash_register_debug_image(const MettleCrashDebugImage *image);
void mettle_crash_set_live_recovery(int64_t point, int64_t wait, int64_t jump);
void mettle_crash_register_extra_code(const MettleCrashFunctionInfo *functions,
                                      size_t function_count,
                                      const MettleCrashLocationInfo *locations,
                                      size_t location_count);
void mettle_crash_trap(const char *message, const void *program_counter,
                       const void *frame_pointer);
void mettle_crash_trap_ex(uint32_t kind, const char *message,
                          const void *program_counter,
                          const void *frame_pointer, uint64_t arg0,
                          uint64_t arg1);
void mettle_crash_write_stderr_bytes(const char *text, size_t length);
void mettle_crash_write_stderr(const char *text);

#endif
