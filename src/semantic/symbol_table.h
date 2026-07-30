#ifndef SYMBOL_TABLE_H
#define SYMBOL_TABLE_H

#include "mtlc/memory.h"
#include <stddef.h>
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
  TYPE_FLOAT32,
  TYPE_FLOAT64,
  TYPE_STRING,
  TYPE_FUNCTION_POINTER,
  TYPE_POINTER,
  TYPE_ARRAY,
  TYPE_STRUCT,
  TYPE_ENUM,
  TYPE_TAGGED_ENUM,
  TYPE_VOID
} TypeKind;

typedef struct Type {
  TypeKind kind;
  char *name;
  size_t size;
  size_t alignment;
  struct Type *base_type; // For pointers and arrays
  size_t array_size;      // For arrays
  struct Type **fn_param_types; // For function pointers
  size_t fn_param_count;        // For function pointers
  struct Type *fn_return_type;  // For function pointers
  // For a capturing closure: the synthesized environment struct type. NULL for
  // a thin function pointer. The closure VALUE is an 8-byte pointer to a heap
  // record whose field 0 is the code pointer and remaining fields are captures.
  struct Type *closure_env;

  // Struct-specific fields
  char **field_names;        // For structs - field names
  struct Type **field_types; // For structs - field types
  size_t *field_offsets;     // For structs - field memory offsets
  size_t field_count;        // For structs - number of fields

  // Tagged enum variant info (TYPE_TAGGED_ENUM only)
  char **tagged_variant_names;
  int *tagged_variant_tags;              // discriminant value per variant
  struct Type **tagged_variant_payloads; // payload type per variant (NULL = none)
  size_t tagged_variant_count;
  size_t tagged_data_offset;   // byte offset of the data union inside the struct
  size_t tagged_data_size;     // size of the data union

  // Template info: for un-instantiated generic enum templates
  char *generic_template_name; // base name e.g. "Option" (NULL if not generic)
} Type;

typedef enum { SCOPE_GLOBAL, SCOPE_FUNCTION, SCOPE_BLOCK } ScopeType;

typedef struct Scope {
  ScopeType type;
  struct Scope *parent;
  struct Symbol **symbols;
  size_t symbol_count;
  size_t symbol_capacity;
  /* Open-addressing hash index over `symbols`, keyed by name. Stores
   * (symbol_index + 1); 0 marks an empty bucket. Lets symbol_table_lookup and
   * the declare-time duplicate check avoid a linear strcmp scan per query.
   * Built lazily once a scope grows past a small threshold. */
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

typedef struct Symbol {
  char *name;
  SymbolKind kind;
  Type *type;
  Scope *scope;
  int is_initialized;
  int is_forward_declaration; // For functions that are declared but not defined
  int is_extern;              // For extern declarations (C interop)
  int is_immutable;           // For local `const`: reassignment is rejected
  int is_address_space_binding; // Fixed GPU storage binding; elements stay mutable
  MtlcAddressSpace address_space; // Neutral GPU storage provenance when known
  int is_builtin;             // Compiler-provided (assert/assert_eq test builtins)
  char *link_name;            // Link-time symbol name for extern declarations
  /* Declaration site, for "previous declaration here" / "defined here"
     diagnostic notes. Zero line when unknown. */
  size_t decl_line;
  size_t decl_column;
  const char *decl_file;
  /* Set on every scope-chain lookup; drives unused-variable warnings. */
  int is_used;
  union {
    struct {
      int register_id;
      int memory_offset;
      int is_in_register;
      /* Set on SYMBOL_PARAMETER when the parameter is passed indirectly
       * per the Microsoft x64 ABI (struct >8 bytes or non-power-of-2 ≤8).
       * memory_offset then names a home slot holding a POINTER to the
       * struct, not the struct itself. See docs/struct-abi-design.md. */
      int is_indirect_param;
    } variable;
    struct {
      char **parameter_names;
      Type **parameter_types;
      size_t parameter_count;
      Type *return_type;
    } function;
    struct {
      long long value;
    } constant;
    struct {
      Type *enum_type;    // The concrete tagged enum type this constructs
      int tag_value;      // Discriminant value for this variant
      Type *payload_type; // NULL if variant carries no payload
    } constructor;
  } data;
} Symbol;

typedef struct SymbolTable {
  Scope *current_scope;
  Scope *global_scope;
} SymbolTable;

// Function declarations
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

// Returns a heap-allocated name of the in-scope symbol most similar to
// `name` (typo suggestion for "did you mean?"), or NULL if nothing is close
// enough. Walks the full scope chain (current -> ... -> global). When
// `kinds`/`kind_count` are provided, only symbols of those kinds are
// considered; pass NULL/0 to consider every kind. Caller frees the result.
char *symbol_table_suggest_similar(SymbolTable *table, const char *name,
                                   const SymbolKind *kinds, size_t kind_count);

Symbol *symbol_create(const char *name, SymbolKind kind, Type *type);
void symbol_destroy(Symbol *symbol);
Type *type_create(TypeKind kind, const char *name);
Type *type_create_function_pointer(Type **param_types, size_t param_count,
                                   Type *return_type);
void type_destroy(Type *type);

// Struct type creation and manipulation functions
Type *type_create_struct(const char *name, char **field_names,
                         Type **field_types, size_t field_count);
Type *type_get_field_type(Type *struct_type, const char *field_name);
size_t type_get_field_offset(Type *struct_type, const char *field_name);

#endif // SYMBOL_TABLE_H
