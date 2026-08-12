#ifndef METTLE_LINKER_COMMON_H
#define METTLE_LINKER_COMMON_H

#include <stdint.h>
#include <stddef.h>

/* COFF AMD64 relocation types */
#define COFF_RELOC_AMD64_ADDR64   1
#define COFF_RELOC_AMD64_ADDR32NB 3
#define COFF_RELOC_AMD64_REL32    4
#define COFF_RELOC_AMD64_SECREL   11

static inline uint16_t linker_read_u16(const unsigned char *data) {
  return (uint16_t)(data[0] | ((uint16_t)data[1] << 8));
}

static inline uint32_t linker_read_u32(const unsigned char *data) {
  return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
         ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static inline uint64_t linker_read_u64(const unsigned char *data) {
  return (uint64_t)linker_read_u32(data) |
         ((uint64_t)linker_read_u32(data + 4) << 32);
}

static inline void linker_write_u32(unsigned char *data, uint32_t value) {
  data[0] = (unsigned char)(value & 0xFF);
  data[1] = (unsigned char)((value >> 8) & 0xFF);
  data[2] = (unsigned char)((value >> 16) & 0xFF);
  data[3] = (unsigned char)((value >> 24) & 0xFF);
}

static inline void linker_write_u64(unsigned char *data, uint64_t value) {
  data[0] = (unsigned char)(value & 0xFF);
  data[1] = (unsigned char)((value >> 8) & 0xFF);
  data[2] = (unsigned char)((value >> 16) & 0xFF);
  data[3] = (unsigned char)((value >> 24) & 0xFF);
  data[4] = (unsigned char)((value >> 32) & 0xFF);
  data[5] = (unsigned char)((value >> 40) & 0xFF);
  data[6] = (unsigned char)((value >> 48) & 0xFF);
  data[7] = (unsigned char)((value >> 56) & 0xFF);
}

static inline size_t linker_align_up(size_t value, size_t alignment) {
  if (alignment == 0) return value;
  return (value + alignment - 1) & ~(alignment - 1);
}

#endif
