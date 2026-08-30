#include "target.h"

#include <stdio.h>
#include <string.h>

typedef struct {
  const char *name;
  MtlcTargetArch arch;
  MtlcTargetOs os;
  int code_bits;
  BinaryTargetFormat format;
  int freestanding;
} MtlcTargetEntry;

static const MtlcTargetEntry TARGET_TABLE[] = {
    {"x86_64-windows", MTLC_TARGET_ARCH_X86_64, MTLC_TARGET_OS_WINDOWS, 64,
     BINARY_TARGET_FORMAT_COFF_WIN64, 0},
    {"x86_64-pc-windows", MTLC_TARGET_ARCH_X86_64, MTLC_TARGET_OS_WINDOWS, 64,
     BINARY_TARGET_FORMAT_COFF_WIN64, 0},
    {"x86_64-linux", MTLC_TARGET_ARCH_X86_64, MTLC_TARGET_OS_LINUX, 64,
     BINARY_TARGET_FORMAT_ELF_X64, 0},
    {"x86_64-unknown-linux", MTLC_TARGET_ARCH_X86_64, MTLC_TARGET_OS_LINUX, 64,
     BINARY_TARGET_FORMAT_ELF_X64, 0},
    {"x86_64-none", MTLC_TARGET_ARCH_X86_64, MTLC_TARGET_OS_NONE, 64,
     BINARY_TARGET_FORMAT_ELF_X64, 1},
    {"x86_64-freestanding", MTLC_TARGET_ARCH_X86_64, MTLC_TARGET_OS_NONE, 64,
     BINARY_TARGET_FORMAT_ELF_X64, 1},
    {"aarch64-linux", MTLC_TARGET_ARCH_AARCH64, MTLC_TARGET_OS_LINUX, 64,
     BINARY_TARGET_FORMAT_ELF_ARM64, 0},
    {"aarch64-unknown-linux", MTLC_TARGET_ARCH_AARCH64, MTLC_TARGET_OS_LINUX,
     64, BINARY_TARGET_FORMAT_ELF_ARM64, 0},
    {"aarch64-none", MTLC_TARGET_ARCH_AARCH64, MTLC_TARGET_OS_NONE, 64,
     BINARY_TARGET_FORMAT_ELF_ARM64, 1},
    {"i386-none", MTLC_TARGET_ARCH_X86_32, MTLC_TARGET_OS_NONE, 32,
     BINARY_TARGET_FORMAT_ELF_X64, 1},
    {"i686-none", MTLC_TARGET_ARCH_X86_32, MTLC_TARGET_OS_NONE, 32,
     BINARY_TARGET_FORMAT_ELF_X64, 1},
    {"i8086-none", MTLC_TARGET_ARCH_X86_16, MTLC_TARGET_OS_NONE, 16,
     BINARY_TARGET_FORMAT_ELF_X64, 1},
    {"i8086", MTLC_TARGET_ARCH_X86_16, MTLC_TARGET_OS_NONE, 16,
     BINARY_TARGET_FORMAT_ELF_X64, 1},
};

static MtlcTarget g_target;
static int g_target_initialized;

static void mtlc_target_init_host(void) {
  BinaryTargetFormat format = binary_target_format_host_default();
  memset(&g_target, 0, sizeof(g_target));
  g_target.format = format;
  g_target.code_bits = 64;
  g_target.section_alignment = 0;
  switch (format) {
  case BINARY_TARGET_FORMAT_ELF_ARM64:
    g_target.arch = MTLC_TARGET_ARCH_AARCH64;
    g_target.os = MTLC_TARGET_OS_LINUX;
    snprintf(g_target.triple, sizeof(g_target.triple), "aarch64-linux");
    break;
  case BINARY_TARGET_FORMAT_ELF_X64:
    g_target.arch = MTLC_TARGET_ARCH_X86_64;
    g_target.os = MTLC_TARGET_OS_LINUX;
    snprintf(g_target.triple, sizeof(g_target.triple), "x86_64-linux");
    break;
  case BINARY_TARGET_FORMAT_COFF_WIN64:
  default:
    g_target.arch = MTLC_TARGET_ARCH_X86_64;
    g_target.os = MTLC_TARGET_OS_WINDOWS;
    snprintf(g_target.triple, sizeof(g_target.triple), "x86_64-windows");
    break;
  }
  g_target_initialized = 1;
}

const MtlcTarget *mtlc_target(void) {
  if (!g_target_initialized) {
    mtlc_target_init_host();
  }
  return &g_target;
}

int mtlc_target_select(const char *triple, char *error, size_t error_size) {
  size_t i;
  if (!g_target_initialized) {
    mtlc_target_init_host();
  }
  if (!triple || triple[0] == '\0') {
    snprintf(error, error_size, "--target needs a triple");
    return 0;
  }
  for (i = 0; i < sizeof(TARGET_TABLE) / sizeof(TARGET_TABLE[0]); i++) {
    const MtlcTargetEntry *entry = &TARGET_TABLE[i];
    size_t name_length = strlen(entry->name);
    if (strncmp(triple, entry->name, name_length) != 0) {
      continue;
    }
    if (triple[name_length] != '\0' && triple[name_length] != '-') {
      continue;
    }
    g_target.arch = entry->arch;
    g_target.os = entry->os;
    g_target.code_bits = entry->code_bits;
    g_target.format = entry->format;
    g_target.freestanding = entry->freestanding;
    g_target.explicit_triple = 1;
    snprintf(g_target.triple, sizeof(g_target.triple), "%s", triple);
    return 1;
  }
  snprintf(error, error_size, "unknown target `%s`; known targets are %s",
           triple, mtlc_target_triple_list());
  return 0;
}

void mtlc_target_set_image_base(uint64_t base) {
  if (!g_target_initialized) {
    mtlc_target_init_host();
  }
  g_target.image_base = base;
  g_target.image_base_set = 1;
}

const char *mtlc_target_triple_list(void) {
  return "x86_64-windows, x86_64-linux, x86_64-none, aarch64-linux, "
         "aarch64-none, i386-none, i686-none, i8086-none";
}

int mtlc_target_is_object_capable(const MtlcTarget *target) {
  if (!target) {
    return 0;
  }
  return target->code_bits == 64;
}
