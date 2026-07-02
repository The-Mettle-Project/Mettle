#ifndef AST_H
#define AST_H

#include <stddef.h>

typedef enum {
  AST_PROGRAM,
  AST_IMPORT,
  AST_IMPORT_STR,
  AST_VAR_DECLARATION,
  AST_FUNCTION_DECLARATION,
  AST_STRUCT_DECLARATION,
  AST_ENUM_DECLARATION,
  AST_TRAIT_DECLARATION,
  AST_IMPL_DECLARATION,
  AST_METHOD_DECLARATION,
  AST_ASSIGNMENT,
  AST_FUNCTION_CALL,
  AST_FUNC_PTR_CALL,
  AST_RETURN_STATEMENT,
  AST_IF_STATEMENT,
  AST_WHILE_STATEMENT,
  AST_FOR_STATEMENT,
  AST_SWITCH_STATEMENT,
  AST_CASE_CLAUSE,
  AST_MATCH_STATEMENT,
  AST_BREAK_STATEMENT,
  AST_CONTINUE_STATEMENT,
  AST_DEFER_STATEMENT,
  AST_ERRDEFER_STATEMENT,
  AST_INLINE_ASM,
  AST_IDENTIFIER,
  AST_NUMBER_LITERAL,
  AST_STRING_LITERAL,
  AST_BINARY_EXPRESSION,
  AST_UNARY_EXPRESSION,
  AST_MEMBER_ACCESS,
  AST_INDEX_EXPRESSION,
  AST_NEW_EXPRESSION,
  AST_CAST_EXPRESSION,
  AST_LAMBDA_EXPRESSION,
  AST_CLOSURE_ADAPT_EXPRESSION
} ASTNodeType;

typedef struct {
  size_t line;
  size_t column;
  const char *filename;
} SourceLocation;

typedef struct ASTNode {
  ASTNodeType type;
  SourceLocation location;
  struct ASTNode **children;
  size_t child_count;
  void *data;                 // Node-specific data
  struct Type *resolved_type; // Cached type from semantic analysis
} ASTNode;

typedef struct {
  char *module_name;
  char *namespace_alias;
  char **selected_names;   // non-NULL when import { a, b } from "mod"
  size_t selected_count;
  char *platform_guard;    // "windows"/"linux" for `import ... if <platform>`;
                           // NULL means the import is unconditional
} ImportDeclaration;

typedef struct {
  char *file_path;
} ImportStrExpression;

typedef struct {
  char *name;
  char *type_name;
  ASTNode *initializer;
  int is_extern;
  int is_exported;
  int is_const; // declared with `const`: immutable, compile-time integer value
  char *link_name;
  // Set on compiler-synthesized bindings whose type is determined structurally
  // (e.g. a range-`for` loop counter takes the type of its bound), which are
  // exempt from the "explicit type required on var/const" rule. User-written
  // `var`/`const` declarations always leave this 0.
  int structural_type;
} VarDeclaration;

typedef struct {
  char *name;
  char **parameter_names;
  char **parameter_types;
  size_t parameter_count;
  char *return_type;
  ASTNode *body;
  int is_exported;
  int is_extern;
  char *link_name;
  char **type_params;
  char **type_param_traits;
  size_t type_param_count;
  // Function decorators (`@inline[!]` / `@noinline` / `@pure` / `@noalloc` /
  // `@simd[!]`):
  int is_inline;          // `@inline`  : force past the inliner's heuristics
  int is_inline_contract; // `@inline!` : every call inlines or compile error
  int is_noinline;        // `@noinline`: never inline this function
  int is_pure;            // `@pure`    : side-effect-free; enables call LICM
  int is_noalloc;         // `@noalloc` : proven allocation-free or compile error
  int is_test;            // `@test`    : compile-time unit test; compiled out
                          //              of normal builds, run by `mettle test`
  int simd_mode;          // SimdAttr applied as the default to every body loop
  // Closure conversion metadata (set on AST_LAMBDA_EXPRESSION nodes only). A
  // capturing lambda records the variables it captures by value, their types,
  // and the synthesized environment struct; `name` then holds the constructor
  // function the lambda value is produced by.
  char **captured_names;
  char **captured_types;
  size_t captured_count;
  char *env_struct_name;
} FunctionDeclaration;

// A thin function value (`&func`, or a non-capturing lambda) implicitly wrapped
// to satisfy an `Fn(...)->R` closure-typed boundary (parameter, return, or var
// declaration). Synthesized by the closure-adapt pass; `ctor_name` is the
// generated adapter constructor to call, `inner` is the original thin
// expression, and `param_types`/`return_type` are the wrapped signature (used
// by the type checker to build the resulting closure type).
typedef struct {
  ASTNode *inner;
  char *ctor_name;
  char **param_types;
  size_t param_count;
  char *return_type;
} ClosureAdapt;

typedef struct {
  char *name;
  char **field_names;
  char **field_types;
  size_t field_count;
  ASTNode **methods;
  size_t method_count;
  int is_exported;
  char **type_params;
  char **type_param_traits;
  size_t type_param_count;
} StructDeclaration;

typedef struct {
  char *name;
  ASTNode *value;       // Initializer expression (for plain integer enums)
  char *payload_type;   // Associated data type name, e.g. "T" or "int32"
                        // NULL means this variant carries no payload
} EnumVariant;

typedef struct {
  char *name;
  EnumVariant *variants;
  size_t variant_count;
  int is_exported;
  // Generic type parameters e.g. enum Option<T> { Some(T), None }
  char **type_params;
  size_t type_param_count;
} EnumDeclaration;

// Match arm: case Some(v): body  or  case None: body
typedef struct {
  char *variant_name;   // "Some", "None", "Ok", "Err"
  char *binding_name;   // variable bound to payload, NULL if no binding/payload
  ASTNode *body;
  int is_default;       // 1 for a wildcard default arm
} MatchArm;

typedef struct {
  ASTNode *expression;  // Value being matched
  MatchArm *arms;
  size_t arm_count;
  int is_expression;    // 1 if used in expression position (arm bodies are
                        // value-yielding expressions, exhaustiveness required)
} MatchStatement;

typedef struct {
  char *name;
  int is_exported;
  ASTNode **methods;
  size_t method_count;
} TraitDeclaration;

typedef struct {
  char *trait_name;
  char *for_type_name;
  ASTNode **methods;
  size_t method_count;
} ImplDeclaration;

typedef struct {
  char *assembly_code;
} InlineAsm;

typedef struct {
  ASTNode **declarations;
  size_t declaration_count;
} Program;

typedef struct {
  char *function_name;
  ASTNode **arguments;
  size_t argument_count;
  ASTNode *object; // Non-null for method calls (obj.method(args))
  char **type_args;
  size_t type_arg_count;
  int is_indirect_call; // 1 if callee is a variable with function pointer type
  struct Type *callee_closure_env; // non-NULL if the callee is a capturing
                                   // closure; set by the type checker
} CallExpression;

typedef struct {
  ASTNode *function;
  ASTNode **arguments;
  size_t argument_count;
} FuncPtrCall;

typedef struct {
  char *variable_name;
  ASTNode *value;
  ASTNode *target; // Non-null for struct field assignment (obj.field = expr)
} Assignment;

typedef struct {
  char *name;
} Identifier;

typedef struct {
  union {
    long long int_value;
    double float_value;
  };
  int is_float;
  /* TOKEN_NUMBER source radix for default integer type (2, 10, 16); 10 for
   * synthesized literals. */
  unsigned char int_radix;
} NumberLiteral;

typedef struct {
  char *value;
} StringLiteral;

typedef struct {
  char *type_name; // The target struct or type name
} NewExpression;

typedef struct {
  char *type_name;  // Target type string
  ASTNode *operand; // Expression being cast
} CastExpression;

typedef struct {
  ASTNode *left;
  ASTNode *right;
  char *operator;
} BinaryExpression;

typedef struct {
  ASTNode *operand;
  char *operator;
} UnaryExpression;

typedef struct {
  ASTNode *object;
  char *member;
} MemberAccess;

typedef struct {
  ASTNode *array;
  ASTNode *index;
} ArrayIndexExpression;

typedef struct {
  ASTNode *condition;
  ASTNode *body;
} ElseIfClause;

typedef struct {
  ASTNode *condition;
  ASTNode *then_branch;
  ElseIfClause *else_ifs;
  size_t else_if_count;
  ASTNode *else_branch;
} IfStatement;

// SIMD vectorization attribute on a loop (`@simd` / `@simd!`).
typedef enum {
  SIMD_ATTR_NONE = 0,     // no attribute
  SIMD_ATTR_HINT = 1,     // `@simd`  : best-effort; warn if it can't vectorize
  SIMD_ATTR_CONTRACT = 2, // `@simd!` : hard contract; compile error if it can't
  SIMD_ATTR_REPORT = 3    // internal: `--explain` marks every unannotated loop
                          // so the verifier can report what became of it;
                          // never warns or errors, only emits notes
} SimdAttr;

typedef struct {
  ASTNode *condition;
  ASTNode *body;
  char *label; // Optional label for labeled break/continue; NULL if unlabeled
  int simd_mode; // SimdAttr: vectorization attribute requested on this loop
} WhileStatement;

typedef struct {
  ASTNode *initializer;
  ASTNode *condition;
  ASTNode *increment;
  ASTNode *body;
  char *label; // Optional label
  int simd_mode; // SimdAttr: vectorization attribute requested on this loop
} ForStatement;

typedef struct {
  ASTNode *value;
  ASTNode *value_high; // non-NULL for a range case `lo..hi`; `value` holds lo
  ASTNode *body;
  int is_default;
} CaseClause;

typedef struct {
  ASTNode *expression;
  ASTNode **cases;
  size_t case_count;
} SwitchStatement;

typedef struct {
  ASTNode *value;
} ReturnStatement;

typedef struct {
  char *target_label; // Optional label name; NULL for unlabeled break/continue
} LoopControlStatement;

typedef struct {
  ASTNode *statement;
} DeferStatement;

// Function declarations
ASTNode *ast_create_node(ASTNodeType type, SourceLocation location);
ASTNode *ast_clone_node(ASTNode *node);
void ast_destroy_node(ASTNode *node);
void ast_add_child(ASTNode *parent, ASTNode *child);

// Specific node creation functions
ASTNode *ast_create_program();
ASTNode *ast_create_import_declaration(const char *module_name,
                                       const char *namespace_alias,
                                       const char **selected_names,
                                       size_t selected_count,
                                       SourceLocation location);
ASTNode *ast_create_import_str(const char *file_path, SourceLocation location);
ASTNode *ast_create_var_declaration(const char *name, const char *type_name,
                                    ASTNode *initializer,
                                    SourceLocation location);
ASTNode *ast_create_function_declaration(const char *name, char **param_names,
                                         char **param_types, size_t param_count,
                                         const char *return_type, ASTNode *body,
                                         SourceLocation location);
ASTNode *ast_create_struct_declaration(const char *name, char **field_names,
                                       char **field_types, size_t field_count,
                                       ASTNode **methods, size_t method_count,
                                       SourceLocation location);
ASTNode *ast_create_enum_declaration(const char *name, EnumVariant *variants,
                                     size_t variant_count,
                                     SourceLocation location);
ASTNode *ast_create_trait_declaration(const char *name,
                                      SourceLocation location);
ASTNode *ast_create_impl_declaration(const char *trait_name,
                                     const char *for_type_name,
                                     SourceLocation location);
ASTNode *ast_create_call_expression(const char *function_name,
                                    ASTNode **arguments, size_t argument_count,
                                    SourceLocation location);
ASTNode *ast_create_func_ptr_call(ASTNode *function, ASTNode **arguments,
                                  size_t argument_count,
                                  SourceLocation location);
ASTNode *ast_create_assignment(const char *variable_name, ASTNode *value,
                               SourceLocation location);
ASTNode *ast_create_inline_asm(const char *assembly_code,
                               SourceLocation location);
ASTNode *ast_create_identifier(const char *name, SourceLocation location);
ASTNode *ast_create_number_literal(long long int_value,
                                   SourceLocation location,
                                   unsigned char int_radix);
ASTNode *ast_create_float_literal(double float_value, SourceLocation location);
ASTNode *ast_create_string_literal(const char *value, SourceLocation location);
ASTNode *ast_create_binary_expression(ASTNode *left, const char *op,
                                      ASTNode *right, SourceLocation location);
ASTNode *ast_create_unary_expression(const char *op, ASTNode *operand,
                                     SourceLocation location);
ASTNode *ast_create_member_access(ASTNode *object, const char *member,
                                  SourceLocation location);
ASTNode *ast_create_array_index_expression(ASTNode *array, ASTNode *index,
                                           SourceLocation location);
ASTNode *ast_create_method_call(ASTNode *object, const char *method_name,
                                ASTNode **arguments, size_t argument_count,
                                SourceLocation location);
ASTNode *ast_create_new_expression(const char *type_name,
                                   SourceLocation location);
ASTNode *ast_create_field_assignment(ASTNode *target, ASTNode *value,
                                     SourceLocation location);
ASTNode *ast_create_cast_expression(const char *type_name, ASTNode *operand,
                                    SourceLocation location);
ASTNode *ast_create_closure_adapt(ASTNode *inner, const char *ctor_name,
                                  char **param_types, size_t param_count,
                                  const char *return_type,
                                  SourceLocation location);
ASTNode *ast_create_for_statement(ASTNode *initializer, ASTNode *condition,
                                  ASTNode *increment, ASTNode *body,
                                  SourceLocation location);
ASTNode *ast_create_case_clause(ASTNode *value, ASTNode *body, int is_default,
                                SourceLocation location);
ASTNode *ast_create_switch_statement(ASTNode *expression, ASTNode **cases,
                                     size_t case_count,
                                     SourceLocation location);
ASTNode *ast_create_break_statement(SourceLocation location);
ASTNode *ast_create_continue_statement(SourceLocation location);
ASTNode *ast_create_labeled_break_statement(const char *label,
                                            SourceLocation location);
ASTNode *ast_create_labeled_continue_statement(const char *label,
                                               SourceLocation location);
ASTNode *ast_create_defer_statement(ASTNode *statement,
                                    SourceLocation location);
ASTNode *ast_create_errdefer_statement(ASTNode *statement,
                                       SourceLocation location);
ASTNode *ast_create_match_statement(ASTNode *expression, MatchArm *arms,
                                    size_t arm_count, SourceLocation location);
ASTNode *ast_create_match_expression(ASTNode *expression, MatchArm *arms,
                                     size_t arm_count, SourceLocation location);

#endif // AST_H
