#ifndef AST_H
#define AST_H

#include "simd_attr.h"
#include "source_location.h"
#include "mtlc/memory.h"
#include "mtlc/tensor.h"
#include <stddef.h>

typedef size_t ASTScopeId;
#define AST_SCOPE_ID_UNRESOLVED ((ASTScopeId)-1)

typedef enum {
  AST_PROGRAM,
  AST_IMPORT,
  AST_IMPORT_STR,
  AST_VAR_DECLARATION,
  AST_FUNCTION_DECLARATION,
  AST_STRUCT_DECLARATION,
  AST_ENUM_DECLARATION,
  AST_TYPE_DECLARATION,
  AST_EFFECT_DECLARATION,
  AST_TRAIT_DECLARATION,
  AST_IMPL_DECLARATION,
  AST_METHOD_DECLARATION,
  AST_ASSIGNMENT,
  AST_FUNCTION_CALL,
  AST_FUNC_PTR_CALL,
  AST_GPU_LAUNCH,
  AST_RETURN_STATEMENT,
  AST_IF_STATEMENT,
  AST_WHILE_STATEMENT,
  AST_FOR_STATEMENT,
  AST_SWITCH_STATEMENT,
  AST_CASE_CLAUSE,
  AST_MATCH_STATEMENT,
  AST_BREAK_STATEMENT,
  AST_CONTINUE_STATEMENT,
  AST_QUIESCE_STATEMENT,
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
  AST_CLOSURE_ADAPT_EXPRESSION,
  AST_BARRIER_STATEMENT,
  AST_AGGREGATE_LITERAL,
  AST_COMPTIME_FOR,
  AST_FALLTHROUGH_STATEMENT,
  AST_VALUE_SCOPE,
  AST_YIELD_STATEMENT,
  AST_NODE_TYPE_COUNT
} ASTNodeType;

typedef struct ASTNode {
  ASTNodeType type;
  SourceLocation location;
  struct ASTNode **children;
  size_t child_count;
  size_t child_capacity;
  struct ASTNode **child_capacity_owner;
  void *data;
  struct Type *resolved_type;
  struct Type *proven_refinement;
  struct ASTNode *proven_predicate;
  int type_registered;
  const char *proven_binding;
  int conflict_free_mode;
  int check_only;
  const struct ComptimeExpansionOrigin *expansion_origin;
} ASTNode;

typedef struct ComptimeExpansionOrigin {
  const char *binding_name;
  struct ASTNode *sequence;
  struct ASTNode *range_end;
  int range_inclusive;
  size_t index;
  SourceLocation keyword_location;
  const struct ComptimeExpansionOrigin *parent;
} ComptimeExpansionOrigin;

typedef struct {
  char *name;
  char **args;
  size_t arg_count;
  SourceLocation location;
} MetaDecorator;

typedef enum {
  META_ROLE_NONE = 0,
  META_ROLE_LAYOUT,
  META_ROLE_GENERATE,
  META_ROLE_PASS,
  META_ROLE_REGALLOC
} MetaRole;

struct TypeMetaLayout;

typedef struct {
  char *module_name;
  char *namespace_alias;
  char **selected_names;
  size_t selected_count;
  char *platform_guard;
} ImportDeclaration;

typedef struct {
  char *file_path;
} ImportStrExpression;

typedef enum {
  AST_ADDRESS_SPACE_DEFAULT = 0,
  AST_ADDRESS_SPACE_WORKGROUP,
  AST_ADDRESS_SPACE_PRIVATE
} AstAddressSpace;

typedef struct {
  char *name;
  char *type_name;
  ASTNode *initializer;
  int is_extern;
  int is_exported;
  int is_const;
  char *link_name;
  int structural_type;
  int type_is_inferred;
  char *capture_type;
  AstAddressSpace address_space;
  ASTNode *composed_name;
} VarDeclaration;

typedef struct {
  char *name;
  char **parameter_names;
  char **parameter_types;
  size_t parameter_count;
  char *return_type;
  char **return_types;
  size_t return_type_count;
  ASTNode *body;
  int is_exported;
  int is_extern;
  int is_kernel;
  int kernel_block[3];
  ASTNode *kernel_block_expression;
  int kernel_threads_per_item;
  char *link_name;
  char **type_params;
  char **type_param_traits;
  size_t type_param_count;
  int is_inline;
  int is_inline_contract;
  int is_noinline;
  int is_pure;
  char *reference_twin;
  long long deadline_cycles;
  int has_deadline;
  int deadline_inclusive;
  char *explain_code;
  char *explain_text;
  int is_noalloc;
  int is_noreturn;
  int tail_must_yield;
  int is_test;
  int is_swappable;
  int is_naked;
  int is_interrupt;
  int is_rule;
  char *numerics_contract;
  int rewrite_role;
  char **effects_with;
  size_t effects_with_count;
  char **effects_forbids;
  size_t effects_forbids_count;
  char **effects_requires;
  size_t effects_requires_count;
  char **effects_provides;
  size_t effects_provides_count;
  int is_variadic;
  int simd_mode;
  char **captured_names;
  char **captured_types;
  size_t captured_count;
  char *env_struct_name;
  ASTNode *composed_name;
  MetaDecorator *meta_decorators;
  size_t meta_decorator_count;
  int meta_role;
  int meta_optimized;
  char *meta_point;
  const char *generated_by;
} FunctionDeclaration;

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
  ASTNode *composed_name;
  MetaDecorator *meta_decorators;
  size_t meta_decorator_count;
  struct TypeMetaLayout *meta_layout;
  int meta_ref_hint;
} StructDeclaration;

typedef struct {
  char *name;
  ASTNode *value;
  char *payload_type;
} EnumVariant;

typedef struct {
  char *name;
  char *base_type;
  char *binding;
  ASTNode *predicate;
  int is_exported;
  ASTNode *composed_name;
  char **type_params;
  size_t type_param_count;
} TypeDeclaration;

typedef struct {
  char *name;
  int is_exported;
} EffectDeclaration;

typedef struct {
  char *name;
  EnumVariant *variants;
  size_t variant_count;
  int is_exported;
  char **type_params;
  size_t type_param_count;
} EnumDeclaration;

typedef struct {
  char *variant_name;
  char *binding_name;
  ASTNode *body;
  int is_default;
} MatchArm;

typedef struct {
  ASTNode *expression;
  MatchArm *arms;
  size_t arm_count;
} MatchStatement;

typedef enum {
  VALUE_SCOPE_BLOCK = 0,
  VALUE_SCOPE_IF,
  VALUE_SCOPE_MATCH,
  VALUE_SCOPE_SWITCH,
  VALUE_SCOPE_LOOP
} ValueScopeKind;

typedef struct {
  ASTNode *body;
  int scope_id;
  ValueScopeKind kind;
  char *label;
  int tail_pending;
} ValueScope;

typedef struct {
  ASTNode *value;
  int scope_id;
  char *label;
  int from_break;
} YieldStatement;

typedef struct {
  char *name;
  int is_exported;
  ASTNode **methods;
  size_t method_count;
  MetaDecorator *meta_decorators;
  size_t meta_decorator_count;
  char **implementors;
  size_t implementor_count;
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
  int tail_open;
} Program;

typedef enum {
  TILE_BUILTIN_NONE = 0,
  TILE_BUILTIN_LOAD,
  TILE_BUILTIN_STORE,
  TILE_BUILTIN_ROW,
  TILE_BUILTIN_COL,
  TILE_BUILTIN_ROW_MAX,
  TILE_BUILTIN_ROW_SUM,
  TILE_BUILTIN_SELECT,
  TILE_BUILTIN_MAX,
  TILE_BUILTIN_MIN,
  TILE_BUILTIN_MATH
} TileBuiltin;

typedef struct {
  char *function_name;
  ASTNode **arguments;
  char **argument_names;
  size_t argument_count;
  ASTNode *object;
  char **type_args;
  size_t type_arg_count;
  int is_function_reference;
  char *written_name;
  int is_indirect_call;
  struct Type *callee_closure_env;
  const char *effect_signature;
  int is_gpu_index;
  int is_gpu_atomic;
  MtlcAddressSpace atomic_address_space;
  MtlcMemoryOrder atomic_memory_order;
  MtlcMemoryOrder atomic_failure_order;
  MtlcMemoryScope atomic_memory_scope;
  int is_gpu_async_copy;
  uint32_t async_copy_element_count;
  uint32_t async_copy_transaction_bytes;
  uint32_t async_copy_pending_groups;
  MtlcAsyncCache async_copy_cache;
  int is_tensor_transfer;
  MtlcTensorTransferDesc tensor_transfer_desc;
  size_t tensor_transfer_view_argument;
  size_t tensor_transfer_coordinate_arguments[MTLC_TENSOR_MAX_RANK];
  int is_tensor_mma;
  int is_tensor_matmul;
  int is_tensor_scale_rows;
  int tile_builtin;
  unsigned tensor_tile_mask;
  int tensor_c_zero;
  MtlcTensorMmaDesc tensor_mma_desc;
  size_t tensor_metadata_argument;
  size_t tensor_a_scale_argument;
  size_t tensor_b_scale_argument;
  size_t tensor_a_stride_argument;
  size_t tensor_b_stride_argument;
  size_t tensor_c_stride_argument;
  size_t tensor_d_stride_argument;
  size_t tensor_c_scale_argument;
  int is_tensor_epilogue;
  MtlcTensorEpilogueDesc tensor_epilogue_desc;
  size_t tensor_epilogue_bias_argument;
  size_t tensor_epilogue_alpha_argument;
  size_t tensor_epilogue_beta_argument;
  size_t tensor_epilogue_clamp_min_argument;
  size_t tensor_epilogue_clamp_max_argument;
  size_t tensor_epilogue_stride_argument;
  size_t tensor_epilogue_bias_stride_argument;
  size_t task_capture_argument;
  const char *task_entry_name;
} CallExpression;

typedef struct {
  ASTNode *function;
  ASTNode **arguments;
  size_t argument_count;
  const char *effect_signature;
} FuncPtrCall;

typedef struct {
  ASTNode *kernel;
  ASTNode *grid[3];
  ASTNode *block[3];
  ASTNode *dynamic_shared_bytes;
  ASTNode *stream;
  ASTNode **arguments;
  size_t argument_count;
  int typed_kernel;
  ASTNode *work;
  ASTNode *grid_indirect;
  int kernel_block[3];
  int kernel_threads_per_item;
} GpuLaunchStatement;

typedef enum {
  AST_MEMORY_REGION_WORKGROUP = 1u << 0,
  AST_MEMORY_REGION_GLOBAL = 1u << 1
} AstMemoryRegion;

typedef enum {
  AST_MEMORY_ORDER_ACQUIRE = 1,
  AST_MEMORY_ORDER_RELEASE,
  AST_MEMORY_ORDER_ACQ_REL,
  AST_MEMORY_ORDER_SEQ_CST
} AstMemoryOrder;

typedef struct {
  unsigned memory_regions;
  AstMemoryOrder memory_order;
  /* Execution scope: 0 for the whole workgroup, 1 for the subgroup. */
  int subgroup;
} BarrierStatement;

typedef struct {
  char *variable_name;
  ASTNode *value;
  ASTNode *target;
  ASTNode **targets;
  size_t target_count;
} Assignment;

typedef struct {
  char *name;
  ASTScopeId scope_id;
} Identifier;

typedef struct {
  union {
    long long int_value;
    double float_value;
  };
  int is_float;
  int is_char;
  unsigned char int_radix;
} NumberLiteral;

typedef struct {
  char *value;
  size_t length;
} StringLiteral;

typedef struct {
  char *type_name;
  ASTNode *count;
  ASTNode **extents;
  size_t extent_count;
} NewExpression;

typedef struct {
  char *type_name;
  ASTNode *operand;
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
  size_t offset;
  char *symbol;
  char *string;
  size_t string_length;
  int string_wants_record;
} AggregateReloc;

typedef struct {
  size_t offset;
  ASTNode *element;
  struct Type *element_type;
  const void *packed_place;
} AggregateRuntimeStore;

typedef struct {
  int is_struct;
  ASTNode **elements;
  char **field_names;
  size_t element_count;
  ASTNode *repeat_count;
  unsigned char *image;
  size_t image_size;
  AggregateReloc *relocs;
  size_t reloc_count;
  AggregateRuntimeStore *runtime_stores;
  size_t runtime_store_count;
} AggregateLiteral;

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
  int uniform_mode;
  int condition_uniform;
  int warp_uniform;
} IfStatement;

typedef struct {
  ASTNode *condition;
  ASTNode *body;
  char *label;
  int simd_mode;
  int unroll_factor;
  int uniform_mode;
  int warp_uniform;
  int parallel_mode;
} WhileStatement;

typedef struct {
  ASTNode *initializer;
  ASTNode *condition;
  ASTNode *increment;
  ASTNode *body;
  char *label;
  int simd_mode;
  int unroll_factor;
  int uniform_mode;
  int warp_uniform;
  int parallel_mode;
} ForStatement;

typedef struct {
  char *binding_name;
  ASTNode *sequence;
  ASTNode *range_end;
  int range_inclusive;
  ASTNode *body;
  SourceLocation keyword_location;
} ComptimeForStatement;

typedef struct {
  ASTNode *value;
  ASTNode *value_high;
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
  ASTNode **values;
  size_t value_count;
  int implicit_tail;
} ReturnStatement;

enum {
  RETURN_WRITTEN = 0,
  RETURN_TAIL_VALUE,
  RETURN_TAIL_CALL,
  RETURN_TAIL_MAIN_CALL
};

typedef struct {
  char *target_label;
  ASTNode *value;
} LoopControlStatement;

typedef struct {
  ASTNode *statement;
} DeferStatement;

ASTNode *ast_create_node(ASTNodeType type, SourceLocation location);
ASTNode *ast_clone_node(ASTNode *node);
MetaDecorator *ast_clone_meta_decorators(const MetaDecorator *decorators,
                                         size_t count);
void ast_destroy_node(ASTNode *node);
void ast_add_child(ASTNode *parent, ASTNode *child);

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
ASTNode *ast_create_type_declaration(const char *name, const char *base_type,
                                     const char *binding, ASTNode *predicate,
                                     SourceLocation location);
ASTNode *ast_create_enum_declaration(const char *name, EnumVariant *variants,
                                     size_t variant_count,
                                     SourceLocation location);
ASTNode *ast_create_trait_declaration(const char *name,
                                      SourceLocation location);
ASTNode *ast_create_effect_declaration(const char *name,
                                       SourceLocation location);
int ast_function_set_effects(FunctionDeclaration *decl, int clause,
                             char **names, size_t count);
enum {
  AST_EFFECT_CLAUSE_WITH = 0,
  AST_EFFECT_CLAUSE_FORBIDS = 1,
  AST_EFFECT_CLAUSE_REQUIRES = 2,
  AST_EFFECT_CLAUSE_PROVIDES = 3
};
ASTNode *ast_create_impl_declaration(const char *trait_name,
                                     const char *for_type_name,
                                     SourceLocation location);
ASTNode *ast_create_call_expression(const char *function_name,
                                    ASTNode **arguments, size_t argument_count,
                                    SourceLocation location);
ASTNode *ast_create_func_ptr_call(ASTNode *function, ASTNode **arguments,
                                  size_t argument_count,
                                  SourceLocation location);
ASTNode *ast_create_gpu_launch(ASTNode *kernel, ASTNode **grid,
                               ASTNode **block,
                               ASTNode *dynamic_shared_bytes, ASTNode *stream,
                               ASTNode **arguments, size_t argument_count,
                               SourceLocation location);
ASTNode *ast_create_barrier_statement(unsigned memory_regions,
                                      AstMemoryOrder memory_order,
                                      SourceLocation location);
ASTNode *ast_create_assignment(const char *variable_name, ASTNode *value,
                               SourceLocation location);
ASTNode *ast_create_multi_assignment(ASTNode **targets, size_t target_count,
                                     ASTNode *value, SourceLocation location);
ASTNode *ast_create_inline_asm(const char *assembly_code,
                               SourceLocation location);
ASTNode *ast_create_identifier(const char *name, SourceLocation location);
ASTNode *ast_create_identifier_with_scope(const char *name,
                                          ASTScopeId scope_id,
                                          SourceLocation location);
const char *ast_integer_literal_type_name(const NumberLiteral *literal);
ASTNode *ast_create_number_literal(long long int_value,
                                   SourceLocation location,
                                   unsigned char int_radix);
ASTNode *ast_create_float_literal(double float_value, SourceLocation location);
ASTNode *ast_create_string_literal(const char *value, size_t length,
                                  SourceLocation location);
ASTNode *ast_create_binary_expression(ASTNode *left, const char *op,
                                      ASTNode *right, SourceLocation location);
ASTNode *ast_create_unary_expression(const char *op, ASTNode *operand,
                                     SourceLocation location);
ASTNode *ast_create_member_access(ASTNode *object, const char *member,
                                  SourceLocation location);
ASTNode *ast_create_array_index_expression(ASTNode *array, ASTNode *index,
                                           SourceLocation location);
ASTNode *ast_create_aggregate_literal(int is_struct, ASTNode **elements,
                                      char **field_names, size_t element_count,
                                      ASTNode *repeat_count,
                                      SourceLocation location);
ASTNode *ast_create_method_call(ASTNode *object, const char *method_name,
                                ASTNode **arguments, size_t argument_count,
                                SourceLocation location);
ASTNode *ast_create_new_expression(const char *type_name,
                                   SourceLocation location);
void ast_release_children(ASTNode *node);

int ast_new_expression_add_extent(ASTNode *node, ASTNode *extent);
ASTNode *ast_create_new_array_expression(const char *type_name, ASTNode *count,
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
ASTNode *ast_create_comptime_for(const char *binding_name, ASTNode *sequence,
                                 ASTNode *body, SourceLocation location);
int ast_fold_member_access_to_int(ASTNode *node, long long value);
int ast_fold_member_access_to_string(ASTNode *node, const char *value);
int ast_fold_member_access_to_float(ASTNode *node, double value);
int ast_fold_call_to_identifier(ASTNode *node, const char *name);
int ast_fold_expression_to_int(ASTNode *node, long long value);
ASTNode *ast_create_case_clause(ASTNode *value, ASTNode *body, int is_default,
                                SourceLocation location);
ASTNode *ast_create_switch_statement(ASTNode *expression, ASTNode **cases,
                                     size_t case_count,
                                     SourceLocation location);
ASTNode *ast_create_quiesce_statement(SourceLocation location);
ASTNode *ast_create_fallthrough_statement(SourceLocation location);
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
ASTNode *ast_create_value_scope(ASTNode *body, int scope_id,
                                ValueScopeKind kind, const char *label,
                                SourceLocation location);
ASTNode *ast_create_yield_statement(ASTNode *value, int scope_id,
                                    SourceLocation location);
int ast_convert_break_to_yield(ASTNode *node, int scope_id);
ASTNode *ast_create_implicit_return(ASTNode *value, int implicit_tail,
                                    SourceLocation location);
ASTNode *ast_create_break_with_value(const char *label, ASTNode *value,
                                     SourceLocation location);
void ast_replace_child(ASTNode *parent, ASTNode *old_child,
                       ASTNode *new_child);
void ast_unwrap_implicit_return(ASTNode *node);

#endif
