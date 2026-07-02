#ifndef TYPE_CHECKER_H
#define TYPE_CHECKER_H

#include "../error/error_reporter.h"
#include "../parser/ast.h"

// Forward declarations
struct SymbolTable;
struct Type;
struct Symbol;
struct TrackedBufferExtent;

#include "symbol_table.h"

typedef struct {
  SymbolTable *symbol_table;
  int has_error;
  char *error_message;
  ErrorReporter *error_reporter;
  // Built-in type instances
  Type *builtin_int8;
  Type *builtin_int16;
  Type *builtin_int32;
  Type *builtin_int64;
  Type *builtin_uint8;
  Type *builtin_uint16;
  Type *builtin_uint32;
  Type *builtin_uint64;
  Type *builtin_bool;
  Type *builtin_float32;
  Type *builtin_float64;
  Type *builtin_string;
  Type *builtin_cstring;
  Type *builtin_void;

  // Generic enum template cache: uninstantiated enum AST nodes
  ASTNode **generic_enum_templates;
  size_t generic_enum_template_count;
  Symbol *current_function;
  ASTNode *current_function_decl;
  int loop_depth;
  int switch_depth;
  char **tracked_var_names;
  unsigned char *tracked_var_initialized;
  int *tracked_var_scope_depth;
  size_t tracked_var_count;
  size_t tracked_var_capacity;
  size_t *tracked_scope_markers;
  size_t tracked_scope_count;
  size_t tracked_scope_capacity;
  int tracked_scope_depth;
  struct TrackedBufferExtent *tracked_buffer_extents;
} TypeChecker;

// Function declarations
TypeChecker *type_checker_create(SymbolTable *symbol_table);
TypeChecker *
type_checker_create_with_error_reporter(SymbolTable *symbol_table,
                                        ErrorReporter *error_reporter);
void type_checker_destroy(TypeChecker *checker);
int type_checker_check_program(TypeChecker *checker, ASTNode *program);
Type *type_checker_infer_type(TypeChecker *checker, ASTNode *expression);
int type_checker_are_compatible(Type *type1, Type *type2);

// Built-in type system functions
void type_checker_init_builtin_types(TypeChecker *checker);
Type *type_checker_get_type_by_name(TypeChecker *checker, const char *name);
int type_checker_is_integer_type(Type *type);
int type_checker_is_floating_type(Type *type);
int type_checker_is_numeric_type(Type *type);

// Type inference and promotion functions
Type *type_checker_promote_types(TypeChecker *checker, Type *left, Type *right,
                                 const char *operator);
Type *type_checker_get_larger_type(TypeChecker *checker, Type *type1,
                                   Type *type2);
int type_checker_get_type_rank(Type *type);

// Type compatibility and conversion functions
int type_checker_is_assignable(TypeChecker *checker, Type *dest_type,
                               Type *src_type);
int type_checker_is_implicitly_convertible(Type *from_type, Type *to_type);
int type_checker_is_cast_valid(Type *from, Type *to);
void type_checker_set_error(TypeChecker *checker, const char *format, ...);
void type_checker_set_error_at_location(TypeChecker *checker,
                                        SourceLocation location,
                                        const char *format, ...);
void type_checker_report_type_mismatch(TypeChecker *checker,
                                       SourceLocation location,
                                       const char *expected,
                                       const char *actual);
void type_checker_report_undefined_symbol(TypeChecker *checker,
                                          SourceLocation location,
                                          const char *symbol_name,
                                          const char *symbol_type);
void type_checker_report_duplicate_declaration(TypeChecker *checker,
                                               SourceLocation location,
                                               const char *symbol_name);
void type_checker_report_duplicate_declaration_prev(TypeChecker *checker,
                                                    SourceLocation location,
                                                    const char *symbol_name,
                                                    const Symbol *previous);
void type_checker_report_type_mismatch_node(TypeChecker *checker,
                                            const ASTNode *node,
                                            const char *expected,
                                            const char *actual);
size_t type_checker_node_span_length(const ASTNode *node);
void type_checker_note_declared_here(TypeChecker *checker,
                                     const Symbol *symbol, const char *what);
void type_checker_warn_unused_locals(TypeChecker *checker);
void type_checker_register_test_builtin(TypeChecker *checker, const char *name,
                                        size_t parameter_count);

// Struct type processing functions
int type_checker_process_struct_declaration(TypeChecker *checker,
                                            ASTNode *struct_decl);
int type_checker_process_enum_declaration(TypeChecker *checker,
                                          ASTNode *enum_decl);
int type_checker_process_declaration(TypeChecker *checker,
                                     ASTNode *declaration);

// Statement and expression validation functions
int type_checker_check_statement(TypeChecker *checker, ASTNode *statement);
int type_checker_check_expression(TypeChecker *checker, ASTNode *expression);
Type *type_checker_check_binary_expression(TypeChecker *checker,
                                           BinaryExpression *binop,
                                           SourceLocation location);

#endif // TYPE_CHECKER_H
