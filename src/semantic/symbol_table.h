#ifndef SYMBOL_TABLE_H
#define SYMBOL_TABLE_H

#include "mtlc/memory.h"
#include "comptime_value.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef enum {
  TYPE_INT8,
  TYPE_INT16,
  TYPE_INT32,
  TYPE_INT64,
  TYPE_UINT8,
  TYPE_UINT16,
  TYPE_UINT32,
  TYPE_UINT64,
  TYPE_BOOL,
  TYPE_CHAR,
  TYPE_FLOAT32,
  TYPE_FLOAT64,
  TYPE_FLOAT16,
  TYPE_BFLOAT16,
  TYPE_STRING,
  TYPE_FUNCTION_POINTER,
  TYPE_POINTER,
  TYPE_ARRAY,
  TYPE_SLICE,
  TYPE_STRUCT,
  TYPE_ENUM,
  TYPE_TAGGED_ENUM,
  TYPE_VOID,
  TYPE_TYPE,
  TYPE_FIELD,
  TYPE_SEQUENCE
} TypeKind;

typedef struct {
  size_t stride;
  size_t alignment;
} TypeMetaRegion;

typedef struct {
  const char *field;
  uint32_t region;
  size_t offset;
  uint32_t word;
  uint32_t bit;
  uint32_t width;
  uint32_t scale;
} TypeMetaPlace;

typedef struct TypeMetaLayout {
  TypeMetaRegion *regions;
  size_t region_count;
  TypeMetaPlace *places;
  size_t place_count;
  const char *allocator;
  const char *written_by;
} TypeMetaLayout;

typedef enum {
  DEVICE_SPACE_NONE = 0,
  DEVICE_SPACE_GENERIC,
  DEVICE_SPACE_GLOBAL,
  DEVICE_SPACE_SHARED,
  DEVICE_SPACE_CONSTANT,
  DEVICE_SPACE_LOCAL
} DeviceSpace;

typedef enum {
  VIEW_LAYOUT_NONE = 0,
  VIEW_LAYOUT_ROW,
  VIEW_LAYOUT_COL,
  VIEW_LAYOUT_SWIZZLE32,
  VIEW_LAYOUT_SWIZZLE64,
  VIEW_LAYOUT_SWIZZLE128,
  VIEW_LAYOUT_INTERLEAVE,
  VIEW_LAYOUT_FRAGMENT_A,
  VIEW_LAYOUT_FRAGMENT_B,
  VIEW_LAYOUT_FRAGMENT_C
} ViewLayout;

typedef struct Type {
  TypeKind kind;
  char *name;
  int is_volatile;
  size_t size;
  size_t alignment;
  struct Type *base_type;
  size_t array_size;
  size_t view_rank;
  unsigned char device_space;
  size_t declared_align;
  unsigned char view_layout;
  unsigned short view_layout_param;
  size_t view_extents[4];
  struct Type **fn_param_types;
  size_t fn_param_count;
  struct Type *fn_return_type;
  struct Type *closure_env;
  const char **fn_effects;
  size_t fn_effect_count;
  int fn_effects_closed;
  const char **fn_requires;
  size_t fn_require_count;
  const char *fn_effect_signature;

  char **field_names;
  struct Type **field_types;
  size_t *field_offsets;
  uint32_t *field_bit_offsets;
  uint32_t *field_bit_widths;
  size_t field_count;
  const struct TypeMetaLayout *meta_layout;

  char **tagged_variant_names;
  int *tagged_variant_tags;
  struct Type **tagged_variant_payloads;
  size_t tagged_variant_count;

  char **enum_member_names;
  long long *enum_member_values;
  size_t enum_member_count;
  size_t tagged_data_offset;
  size_t tagged_data_size;

  char *generic_template_name;

  uint32_t type_table_index;

  char *qualified_name;
  struct Type *refined_base;
  struct ASTNode *refinement;
  const char *refine_binding;
  int refine_has_range;
  long long refine_min;
  long long refine_max;
  int refine_relational;
  const char *refine_relation_name;
  int refine_uniform;
  int refine_has_frange;
  double refine_fmin;
  double refine_fmax;
  double refine_ferr;
} Type;

typedef enum { SCOPE_GLOBAL, SCOPE_FUNCTION, SCOPE_BLOCK } ScopeType;

typedef struct Scope {
  ScopeType type;
  size_t scope_id;
  struct Scope *parent;
  struct Symbol **symbols;
  size_t symbol_count;
  size_t symbol_capacity;
  size_t *name_index;
  size_t name_index_bucket_count;
} Scope;

typedef enum {
  SYMBOL_VARIABLE,
  SYMBOL_FUNCTION,
  SYMBOL_STRUCT,
  SYMBOL_ENUM,
  SYMBOL_CONSTANT,
  SYMBOL_PARAMETER,
  SYMBOL_TAGGED_ENUM_CONSTRUCTOR
} SymbolKind;

struct ASTNode;

typedef struct Symbol {
  char *name;
  SymbolKind kind;
  Type *type;
  Scope *scope;
  int is_initialized;
  int is_forward_declaration;
  int is_extern;
  int is_immutable;
  int is_address_space_binding;
  MtlcAddressSpace address_space;
  int is_builtin;
  int is_rule;
  int is_meta;
  int is_kernel;
  int kernel_block[3];
  int kernel_threads_per_item;
  char *link_name;
  size_t decl_line;
  size_t decl_column;
  const char *decl_file;
  int is_used;
  int has_constant_value;
  int constant_is_float;
  long long constant_integer_value;
  double constant_float_value;
  int post_state;
  int post_has_min;
  int post_has_max;
  long long post_min;
  long long post_max;
  int move_computed;
  int move_direction;
  long long move_step;
  struct ASTNode *move_declaration;
  struct ASTNode *move_addend;
  ComptimeValue comptime_value;
  struct ASTNode *constant_initializer;
  int is_comptime_binding;
  union {
    struct {
      int register_id;
      int memory_offset;
      int is_in_register;
      int is_indirect_param;
    } variable;
    struct {
      char **parameter_names;
      Type **parameter_types;
      size_t parameter_count;
      Type *return_type;
      int is_variadic;
    } function;
    struct {
      long long value;
    } constant;
    struct {
      Type *enum_type;
      int tag_value;
      Type *payload_type;
    } constructor;
  } data;
} Symbol;

typedef struct SymbolTable {
  Scope *current_scope;
  Scope *global_scope;
  size_t next_scope_id;
} SymbolTable;

SymbolTable *symbol_table_create(void);
void symbol_table_destroy(SymbolTable *table);
int symbol_table_enter_scope(SymbolTable *table, ScopeType type);
void symbol_table_exit_scope(SymbolTable *table);
int symbol_table_declare(SymbolTable *table, Symbol *symbol);
Symbol *symbol_table_lookup(SymbolTable *table, const char *name);
Symbol *symbol_table_lookup_current_scope(SymbolTable *table, const char *name);
void symbol_table_insert(SymbolTable *table, Symbol *symbol);
int symbol_table_declare_forward(SymbolTable *table, Symbol *symbol);
int symbol_table_resolve_forward_declaration(SymbolTable *table,
                                             Symbol *symbol);
int symbol_table_validate_declaration(SymbolTable *table, Symbol *symbol);
Scope *symbol_table_get_current_scope(SymbolTable *table);

char *symbol_table_suggest_similar(SymbolTable *table, const char *name,
                                   const SymbolKind *kinds, size_t kind_count);

Symbol *symbol_create(const char *name, SymbolKind kind, Type *type);
void symbol_destroy(Symbol *symbol);
Type *type_create(TypeKind kind, const char *name);
Type *type_create_function_pointer(Type **param_types, size_t param_count,
                                   Type *return_type);
void type_destroy(Type *type);

Type *type_create_struct(const char *name, char **field_names,
                         Type **field_types, size_t field_count);
Type *type_get_field_type(Type *struct_type, const char *field_name);
size_t type_get_field_offset(Type *struct_type, const char *field_name);
size_t type_view_rank(const Type *type);
int type_get_field_index(const Type *struct_type, const char *field_name);

typedef struct TypeField {
  const char *name;
  struct Type *type;
  size_t byte_offset;
  uint32_t bit_offset;
  uint32_t bit_width;
} TypeField;

typedef struct TypeEnumVariant {
  const char *name;
  long long value;
  struct Type *payload;
} TypeEnumVariant;

int type_alloc_fields(Type *type, size_t field_count);
int type_set_field(Type *type, size_t index, const char *name,
                   Type *field_type, uint32_t bit_width);
int type_compute_layout(Type *type);
int type_is_columnar(const Type *type);
const TypeMetaPlace *type_field_place(const Type *type, size_t index);
int type_field_is_packed(const Type *type, size_t index);
size_t type_columns_region_base(const Type *type, size_t region, size_t count);
size_t type_columns_bytes(const Type *type, size_t count);
int type_apply_meta_layout(Type *type, TypeMetaLayout *layout, char *why,
                           size_t why_size);

size_t type_field_count(const Type *type);
int type_field_at(const Type *type, size_t index, TypeField *out);
int type_field_by_name(const Type *type, const char *name, TypeField *out);

int type_alloc_enum_members(Type *type, size_t count);
int type_set_enum_member(Type *type, size_t index, const char *name,
                         long long value);
size_t type_enum_variant_count(const Type *type);
int type_enum_variant_at(const Type *type, size_t index,
                         TypeEnumVariant *out);

Type *type_pointee(const Type *type);
Type *type_element(const Type *type);
size_t type_len(const Type *type);
int type_has_static_len(const Type *type);

int type_is_comptime_only(const Type *type);

#endif
