#include "verify_owned.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint16_t read_u16(const unsigned char *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_u32(const unsigned char *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t read_u64(const unsigned char *p) {
  return (uint64_t)read_u32(p) | ((uint64_t)read_u32(p + 4) << 32);
}

static int ascii_lower(int c) {
  return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

static int contains_ci(const char *text, const char *needle) {
  size_t needle_len;
  if (!text || !needle) return 0;
  needle_len = strlen(needle);
  if (needle_len == 0) return 1;
  for (; *text; text++) {
    size_t i = 0;
    while (i < needle_len && text[i] &&
           ascii_lower((unsigned char)text[i]) ==
               ascii_lower((unsigned char)needle[i])) {
      i++;
    }
    if (i == needle_len) return 1;
  }
  return 0;
}

static int equals_ci(const char *a, const char *b) {
  if (!a || !b) return 0;
  while (*a && *b) {
    if (ascii_lower((unsigned char)*a) !=
        ascii_lower((unsigned char)*b)) {
      return 0;
    }
    a++;
    b++;
  }
  return *a == '\0' && *b == '\0';
}

static void set_reason(char *reason, size_t reason_size, const char *text) {
  if (!reason || reason_size == 0) return;
  snprintf(reason, reason_size, "%s", text ? text : "invalid executable");
}

static int forbidden_dll_name(const char *name) {
  static const char *const forbidden[] = {
      "msvcrt",       "ucrt",         "vcruntime",  "msvcp",
      "concrt",       "appcrt",       "api-ms-win-crt",
      "libgcc",       "libstdc++",    "libwinpthread"};
  size_t i;
  for (i = 0; i < sizeof(forbidden) / sizeof(forbidden[0]); i++) {
    if (contains_ci(name, forbidden[i])) return 1;
  }
  return 0;
}

static size_t pe_rva_to_offset(const unsigned char *data, size_t size,
                               size_t section_table, uint16_t section_count,
                               uint32_t rva) {
  uint16_t i;
  for (i = 0; i < section_count; i++) {
    size_t section = section_table + (size_t)i * 40u;
    uint32_t virtual_size;
    uint32_t virtual_address;
    uint32_t raw_size;
    uint32_t raw_offset;
    uint32_t span;
    uint64_t file_offset;
    if (section > size || size - section < 40u) return (size_t)-1;
    virtual_size = read_u32(data + section + 8);
    virtual_address = read_u32(data + section + 12);
    raw_size = read_u32(data + section + 16);
    raw_offset = read_u32(data + section + 20);
    span = virtual_size > raw_size ? virtual_size : raw_size;
    if (rva < virtual_address || (uint64_t)rva >=
                                      (uint64_t)virtual_address + span) {
      continue;
    }
    file_offset = (uint64_t)raw_offset + (rva - virtual_address);
    return file_offset < size ? (size_t)file_offset : (size_t)-1;
  }
  return (size_t)-1;
}

static int pe_check_name(const unsigned char *data, size_t size,
                         size_t section_table, uint16_t section_count,
                         uint32_t name_rva, char *reason, size_t reason_size) {
  size_t name_offset = pe_rva_to_offset(data, size, section_table,
                                        section_count, name_rva);
  size_t end;
  if (name_offset == (size_t)-1) {
    set_reason(reason, reason_size, "PE import name points outside the file");
    return 0;
  }
  end = name_offset;
  while (end < size && data[end] != 0 && end - name_offset < 260u) end++;
  if (end == size || end - name_offset == 260u) {
    set_reason(reason, reason_size, "PE import name is not terminated");
    return 0;
  }
  if (forbidden_dll_name((const char *)(data + name_offset))) {
    if (reason && reason_size) {
      snprintf(reason, reason_size, "forbidden runtime import '%s'",
               (const char *)(data + name_offset));
    }
    return 0;
  }
  return 1;
}

static int verify_pe(const unsigned char *data, size_t size, char *reason,
                     size_t reason_size) {
  size_t pe_offset;
  size_t optional;
  size_t section_table;
  uint16_t section_count;
  uint16_t optional_size;
  uint32_t directory_count;
  uint64_t image_base;
  size_t directory;
  uint32_t import_rva;
  uint32_t import_size;
  uint32_t delay_rva;
  uint32_t delay_size;
  size_t cursor;
  size_t limit;

  if (size < 0x40u) {
    set_reason(reason, reason_size, "truncated PE DOS header");
    return 0;
  }
  pe_offset = read_u32(data + 0x3c);
  if (pe_offset > size || size - pe_offset < 24u ||
      read_u32(data + pe_offset) != 0x00004550u) {
    set_reason(reason, reason_size, "invalid PE header");
    return 0;
  }
  section_count = read_u16(data + pe_offset + 6);
  optional_size = read_u16(data + pe_offset + 20);
  optional = pe_offset + 24u;
  if (optional > size || size - optional < optional_size ||
      optional_size < 120u || read_u16(data + optional) != 0x20bu) {
    set_reason(reason, reason_size, "expected a PE32+ executable");
    return 0;
  }
  section_table = optional + optional_size;
  if (section_table > size ||
      (size_t)section_count > (size - section_table) / 40u) {
    set_reason(reason, reason_size, "truncated PE section table");
    return 0;
  }

  directory_count = read_u32(data + optional + 108u);
  if (directory_count > (uint32_t)((optional_size - 112u) / 8u)) {
    directory_count = (uint32_t)((optional_size - 112u) / 8u);
  }
  image_base = read_u64(data + optional + 24u);
  directory = optional + 112u;
  import_rva = directory_count > 1u ? read_u32(data + directory + 8u) : 0u;
  import_size = directory_count > 1u ? read_u32(data + directory + 12u) : 0u;
  delay_rva = directory_count > 13u ? read_u32(data + directory + 104u) : 0u;
  delay_size = directory_count > 13u ? read_u32(data + directory + 108u) : 0u;

  if (import_rva) {
    cursor = pe_rva_to_offset(data, size, section_table, section_count,
                              import_rva);
    if (cursor == (size_t)-1) {
      set_reason(reason, reason_size, "PE import table points outside the file");
      return 0;
    }
    limit = import_size / 20u + 1u;
    if (limit > 4096u) limit = 4096u;
    while (limit-- && cursor <= size && size - cursor >= 20u) {
      uint32_t name_rva = read_u32(data + cursor + 12u);
      if (read_u32(data + cursor) == 0u && name_rva == 0u &&
          read_u32(data + cursor + 16u) == 0u) {
        break;
      }
      if (!pe_check_name(data, size, section_table, section_count, name_rva,
                         reason, reason_size)) {
        return 0;
      }
      cursor += 20u;
    }
  }

  if (delay_rva) {
    cursor = pe_rva_to_offset(data, size, section_table, section_count,
                              delay_rva);
    if (cursor == (size_t)-1) {
      set_reason(reason, reason_size,
                 "PE delay import table points outside the file");
      return 0;
    }
    limit = delay_size / 32u + 1u;
    if (limit > 4096u) limit = 4096u;
    while (limit-- && cursor <= size && size - cursor >= 32u) {
      uint32_t attributes = read_u32(data + cursor);
      uint32_t name_value = read_u32(data + cursor + 4u);
      uint32_t name_rva;
      if (attributes == 0u && name_value == 0u) break;
      if (attributes & 1u) {
        name_rva = name_value;
      } else if ((uint64_t)name_value >= image_base &&
                 (uint64_t)name_value - image_base <= 0xffffffffu) {
        name_rva = (uint32_t)((uint64_t)name_value - image_base);
      } else {
        set_reason(reason, reason_size, "invalid PE delay import name");
        return 0;
      }
      if (!pe_check_name(data, size, section_table, section_count, name_rva,
                         reason, reason_size)) {
        return 0;
      }
      cursor += 32u;
    }
  }

  return 1;
}

static int verify_elf(const unsigned char *data, size_t size, char *reason,
                      size_t reason_size, int allow_dynamic) {
  uint64_t program_offset;
  uint16_t program_size;
  uint16_t program_count;
  uint16_t i;
  if (size < 64u || data[4] != 2u || data[5] != 1u) {
    set_reason(reason, reason_size, "expected a little endian ELF64 executable");
    return 0;
  }
  if (read_u16(data + 16u) != 2u && !(allow_dynamic && read_u16(data + 16u) == 3u)) {
    set_reason(reason, reason_size, "owned ELF output must use ET_EXEC");
    return 0;
  }
  program_offset = read_u64(data + 32u);
  program_size = read_u16(data + 54u);
  program_count = read_u16(data + 56u);
  if (program_size < 56u || program_offset > size ||
      (uint64_t)program_count > (size - (size_t)program_offset) / program_size) {
    set_reason(reason, reason_size, "truncated ELF program header table");
    return 0;
  }
  for (i = 0; allow_dynamic == 0 && i < program_count; i++) {
    size_t offset = (size_t)program_offset + (size_t)i * program_size;
    uint32_t type = read_u32(data + offset);
    if (type == 3u) {
      set_reason(reason, reason_size,
                 "ELF output requests a dynamic program loader");
      return 0;
    }
    if (type == 2u) {
      set_reason(reason, reason_size,
                 "ELF output contains a dynamic dependency table");
      return 0;
    }
  }
  return 1;
}

static int mettle_verify_owned_file(const char *path, int allow_dynamic,
                                    char *reason, size_t reason_size);

int mettle_verify_owned_executable(const char *path, char *reason,
                                   size_t reason_size) {
  return mettle_verify_owned_file(path, 0, reason, reason_size);
}

int mettle_verify_owned_dynamic_executable(const char *path, char *reason,
                                           size_t reason_size) {
  return mettle_verify_owned_file(path, 1, reason, reason_size);
}

static int mettle_verify_owned_image_ex(const unsigned char *data, size_t size,
                                        int allow_dynamic, char *reason,
                                        size_t reason_size);

static int mettle_verify_owned_file(const char *path, int allow_dynamic,
                                    char *reason, size_t reason_size) {
  FILE *file = NULL;
  unsigned char *data = NULL;
  long length;
  int result = 0;
  if (reason && reason_size) reason[0] = '\0';
  if (!path) {
    set_reason(reason, reason_size, "missing executable path");
    return 0;
  }
  file = fopen(path, "rb");
  if (!file || fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 ||
      fseek(file, 0, SEEK_SET) != 0) {
    set_reason(reason, reason_size, "could not read linked executable");
    if (file) fclose(file);
    return 0;
  }
  data = (unsigned char *)malloc((size_t)length ? (size_t)length : 1u);
  if (!data || fread(data, 1, (size_t)length, file) != (size_t)length) {
    set_reason(reason, reason_size, "could not load linked executable");
    free(data);
    fclose(file);
    return 0;
  }
  fclose(file);
  result = mettle_verify_owned_image_ex(data, (size_t)length, allow_dynamic,
                                        reason, reason_size);
  free(data);
  return result;
}

int mettle_verify_owned_image(const unsigned char *data, size_t size,
                              char *reason, size_t reason_size) {
  return mettle_verify_owned_image_ex(data, size, 0, reason, reason_size);
}

static int mettle_verify_owned_image_ex(const unsigned char *data, size_t size,
                                        int allow_dynamic, char *reason,
                                        size_t reason_size) {
  if (reason && reason_size) reason[0] = '\0';
  if (!data) {
    set_reason(reason, reason_size, "missing executable image");
    return 0;
  }
  if (size >= 4u && data[0] == 0x7fu && data[1] == 'E' && data[2] == 'L' &&
      data[3] == 'F') {
    return verify_elf(data, size, reason, reason_size, allow_dynamic);
  }
  if (size >= 2u && data[0] == 'M' && data[1] == 'Z') {
    return verify_pe(data, size, reason, reason_size);
  }
  set_reason(reason, reason_size, "linked output is not PE32+ or ELF64");
  return 0;
}

int mettle_link_argument_uses_forbidden_runtime(const char *argument) {
  static const char *const exact[] = {
      "-lc",          "-lpthread",   "-lmsvcrt",   "-lucrt",
      "-lvcruntime", "-lmingw",     "-lmingw32",  "-lmingwex",
      "-lstdc++",    "-lsupc++",    "-lgcc",      "-lgcc_s",
      "-lwinpthread", "msvcrt.lib",  "ucrt.lib",   "libcmt.lib",
      "libcmtd.lib",  "vcruntime.lib", "vcruntimed.lib"};
  static const char *const path_tokens[] = {
      "/libc.a",       "\\libc.a",       "/libpthread.a",
      "\\libpthread.a", "/libgcc",        "\\libgcc",
      "/libstdc++",    "\\libstdc++",    "/libwinpthread",
      "\\libwinpthread", "api-ms-win-crt", "ucrtbase",
      "msvcrt",        "vcruntime"};
  size_t i;
  if (!argument || !argument[0]) return 0;
  for (i = 0; i < sizeof(exact) / sizeof(exact[0]); i++) {
    if (equals_ci(argument, exact[i])) return 1;
  }
  for (i = 0; i < sizeof(path_tokens) / sizeof(path_tokens[0]); i++) {
    if (contains_ci(argument, path_tokens[i])) return 1;
  }
  return 0;
}
