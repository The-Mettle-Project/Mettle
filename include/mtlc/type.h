
#ifndef MTLC_TYPE_H
#define MTLC_TYPE_H

#include <stddef.h>
#include "memory.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  MTLC_TYPE_INT8,
  MTLC_TYPE_INT16,
  MTLC_TYPE_INT32,
  MTLC_TYPE_INT64,
  MTLC_TYPE_UINT8,
  MTLC_TYPE_UINT16,
  MTLC_TYPE_UINT32,
  MTLC_TYPE_UINT64,
  MTLC_TYPE_BOOL,
  MTLC_TYPE_FLOAT32,
  MTLC_TYPE_FLOAT64,
  MTLC_TYPE_FLOAT16,
  MTLC_TYPE_BFLOAT16,
  MTLC_TYPE_STRING,
  MTLC_TYPE_FUNCTION_POINTER,
  MTLC_TYPE_POINTER,
  MTLC_TYPE_ARRAY,
  MTLC_TYPE_STRUCT,
  MTLC_TYPE_ENUM,
  MTLC_TYPE_TAGGED_ENUM,
  MTLC_TYPE_VOID
} MtlcTypeKind;

typedef struct MtlcType {
  MtlcTypeKind kind;
  const char *name;
  size_t size;
  size_t alignment;

  MtlcAddressSpace address_space;

  size_t pointee_align;

  MtlcViewLayout view_layout;
  unsigned view_layout_param;

  size_t view_extents[4];
  unsigned view_extent_count;

  struct MtlcType *base_type;
  size_t array_size;

  struct MtlcType **fn_param_types;
  size_t fn_param_count;
  struct MtlcType *fn_return_type;

  struct MtlcType *closure_env;

  const char **field_names;
  struct MtlcType **field_types;
  size_t *field_offsets;
  size_t field_count;

  const char **tagged_variant_names;
  int *tagged_variant_tags;
  struct MtlcType **tagged_variant_payloads;
  size_t tagged_variant_count;
  size_t tagged_data_offset;
  size_t tagged_data_size;

  const void *frontend_layout_type;
  const uint32_t *field_bit_widths;
} MtlcType;

const MtlcType *mtlc_type_scalar(MtlcTypeKind kind);

const MtlcType *mtlc_type_pointer(const MtlcType *base);

const MtlcType *mtlc_type_pointer_in(const MtlcType *base,
                                     MtlcAddressSpace address_space);

const MtlcType *mtlc_type_array(const MtlcType *element, size_t count);

const MtlcType *mtlc_type_struct(const char *name,
                                 const char *const *field_names,
                                 const MtlcType *const *field_types,
                                 size_t field_count);

size_t mtlc_type_field_count(const MtlcType *t);
size_t mtlc_type_field_offset(const MtlcType *t, size_t index);

size_t mtlc_type_field_index(const MtlcType *t, const char *name);

const MtlcType *mtlc_type_function_pointer(const MtlcType *return_type,
                                           const MtlcType *const *param_types,
                                           size_t param_count);

int mtlc_type_is_integer(const MtlcType *t);
int mtlc_type_is_unsigned(const MtlcType *t);
int mtlc_type_is_float(const MtlcType *t);
int mtlc_type_is_aggregate(const MtlcType *t);
size_t mtlc_type_size(const MtlcType *t);
size_t mtlc_type_alignment(const MtlcType *t);
const char *mtlc_type_kind_name(MtlcTypeKind kind);

#ifdef __cplusplus
}
#endif

#endif
