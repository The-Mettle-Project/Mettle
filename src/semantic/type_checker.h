#ifndef TYPE_CHECKER_H
#define TYPE_CHECKER_H

#include <stdio.h>
#include "error/error_reporter.h"
#include "parser/ast.h"
#include "semantic/schedule_expand.h"

struct SymbolTable;
struct Type;
struct Symbol;
struct TrackedBufferExtent;
typedef struct ComptimeExpansionTable ComptimeExpansionTable;
typedef struct ComptimeSequenceArena ComptimeSequenceArena;
struct ComptimeBindingSlot;
struct TypeCheckerGuard;

#include "symbol_table.h"

typedef struct TypeCheckerEffect {
  const char *name;
  SourceLocation site;
  int is_exported;
  int is_builtin;
} TypeCheckerEffect;

typedef struct TypeCheckerProof {
  char *type_name;
  char *expression;
  char *proof;
  char *range;
  size_t line;
  size_t column;
  int proven;
  long long steps;
} TypeCheckerProof;

typedef struct TypeCheckerEffectObligation {
  const char *function;
  const char *signature;
  SourceLocation location;
} TypeCheckerEffectObligation;

typedef struct {
  SymbolTable *symbol_table;
  int has_error;
  int defer_unresolved_comptime;
  char *error_message;
  ErrorReporter *error_reporter;
  const char *failed_bindings[64];
  size_t failed_binding_count;
  const void *failed_bindings_owner;
  struct TypeCheckerGuard *guards;
  size_t guard_count;
  size_t guard_capacity;
  size_t guard_pushes;
  char *refine_failure;
  char *effect_failure;
  TypeCheckerEffect *effects;
  size_t effect_count;
  size_t effect_capacity;
  TypeCheckerEffectObligation *effect_obligations;
  size_t effect_obligation_count;
  size_t effect_obligation_capacity;
  Type *builtin_int8;
  Type *builtin_int16;
  Type *builtin_int32;
  Type *builtin_int64;
  Type *builtin_uint8;
  Type *builtin_uint16;
  Type *builtin_uint32;
  Type *builtin_uint64;
  Type *builtin_bool;
  Type *builtin_char;
  Type *builtin_float32;
  Type *builtin_float64;
  Type *builtin_float16;
  Type *builtin_bfloat16;
  Type *builtin_string;
  Type *builtin_cstring;
  Type *builtin_rawptr;
  Type *builtin_void;
  Type *builtin_type;
  Type *builtin_field;
  Type *builtin_row;
  Type *builtin_sequence;

  Type **type_table;
  size_t type_table_count;
  size_t type_table_capacity;

  ComptimeExpansionTable *expansions;

  struct ComptimeBindingSlot *comptime_bindings;
  size_t comptime_binding_count;
  size_t comptime_binding_capacity;

  Type *builtin_kind;
  int user_declares_kind;
  ComptimeSequenceArena *sequences;

  ASTNode **generic_enum_templates;
  size_t generic_enum_template_count;
  Symbol *current_function;
  ASTNode *current_function_decl;
  ASTNode *errdefer_scan_decl;
  int errdefer_scan_found;
  int loop_depth;
  int switch_depth;
  const char **loop_labels;
  size_t loop_label_count;
  size_t loop_label_capacity;
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
  Type *aggregate_target_type;
  int aggregate_requires_constant;
  struct ASTNode *module_program;
  int device_module;
  Type **struct_placeholders;
  size_t struct_placeholder_count;
  size_t struct_placeholder_capacity;
  int monotone_busy;
  void *movement_index;
  struct {
    struct ASTNode *body;
    long long trips;
  } *loop_trips;
  size_t loop_trip_count;
  size_t loop_trip_capacity;
  size_t expansion_rounds;
  ScheduleStats schedule_stats;
  size_t comptime_text_bytes;
  long long proof_steps;
  const char *narrowing[16];
  size_t narrowing_count;
  int proof_ceiling_hit;
  size_t proofs_attempted;
  size_t proofs_proven;
  size_t proofs_refused;
  struct TypeCheckerProof *proof_log;
  size_t proof_log_count;
  size_t proof_log_capacity;
  struct TypeCheckerBorrowFact *borrow_facts;
  size_t borrow_fact_count;
} TypeChecker;

typedef struct TypeCheckerBorrowFact {
  const char *name;
  unsigned stores;
  unsigned frees;
} TypeCheckerBorrowFact;

int type_checker_callee_borrows(const TypeChecker *checker, const char *callee,
                                size_t index);

int type_checker_predicate_is_relational(TypeChecker *checker,
                                         struct ASTNode *predicate,
                                         const char *binding,
                                         struct Type *refined);
int type_checker_float_bound(const struct Type *type, double *min, double *max,
                             double *err);
int type_checker_type_excludes_zero(const struct Type *type);
int type_checker_is_tile(const struct Type *type);
int type_checker_is_row_vector(const struct Type *type);
int type_checker_check_field_write(TypeChecker *checker, struct ASTNode *object,
                                   const char *field, struct ASTNode *value,
                                   SourceLocation location);
void type_checker_bind_predicate_check(TypeChecker *checker,
                                       struct Type *refined,
                                       struct ASTNode *expr);
int type_checker_push_loop_trip(TypeChecker *checker,
                                struct ASTNode *condition,
                                struct ASTNode *body);
void type_checker_pop_loop_trip(TypeChecker *checker, size_t depth);
size_t type_checker_loop_trip_depth(const TypeChecker *checker);
void type_checker_note_return_range(TypeChecker *checker, struct ASTNode *value);
void type_checker_report_proofs(const TypeChecker *checker, FILE *out);

void type_checker_set_gpu_type_report(int enabled);
void type_checker_print_gpu_type_report(FILE *out);
long long type_checker_proof_steps(const TypeChecker *checker);
int type_checker_proof_ceiling_hit(const TypeChecker *checker);
int type_checker_why_proof(const TypeChecker *checker, const char *site,
                           const char *type_name, FILE *out);

TypeChecker *type_checker_create(SymbolTable *symbol_table);
TypeChecker *
type_checker_create_with_error_reporter(SymbolTable *symbol_table,
                                        ErrorReporter *error_reporter);
void type_checker_destroy(TypeChecker *checker);
int type_checker_check_program(TypeChecker *checker, ASTNode *program);
int type_checker_register_generated_types(TypeChecker *checker,
                                          struct ASTNode *program);
Type *type_checker_infer_type(TypeChecker *checker, ASTNode *expression);
int type_checker_are_compatible(Type *type1, Type *type2);

void type_checker_init_builtin_types(TypeChecker *checker);
Type *type_checker_get_type_by_name(TypeChecker *checker, const char *name);
int type_checker_is_integer_type(Type *type);
int type_checker_gpu_abi_type(const Type *type);
int type_checker_is_discrete_type(Type *type);
int type_checker_register_variant_constructor(TypeChecker *checker, Type *te,
                                              const char *enum_name,
                                              const char *variant_name,
                                              size_t index, int from_generic);
int type_checker_is_floating_type(Type *type);
int type_checker_is_numeric_type(Type *type);

int type_checker_body_assigns(const ASTNode *node, const char *name);
int type_checker_push_guard(TypeChecker *checker, ASTNode *condition,
                            int negated);
int type_checker_push_range_guard(TypeChecker *checker, const char *name,
                                  int has_min, long long min, int has_max,
                                  long long max);
size_t type_checker_guard_depth(const TypeChecker *checker);
void type_checker_pop_guards(TypeChecker *checker, size_t depth);
void type_checker_movement_index_drop(TypeChecker *checker);
int type_checker_expression_range(TypeChecker *checker, ASTNode *expr,
                                  int *has_min, long long *min, int *has_max,
                                  long long *max);
int type_checker_prove_refinement(TypeChecker *checker, Type *refined,
                                  ASTNode *expr);
void type_checker_compute_refinement_range(TypeChecker *checker,
                                           Type *refined);
int type_checker_refined_index_fits(const Type *index_type, size_t length);
void type_checker_report_refinement_failure(TypeChecker *checker,
                                            const ASTNode *expr,
                                            SourceLocation location);
Type *type_checker_refinement_base(Type *type);

Type *type_checker_promote_types(TypeChecker *checker, Type *left, Type *right,
                                 const char *operator);
Type *type_checker_get_larger_type(TypeChecker *checker, Type *type1,
                                   Type *type2);
int type_checker_get_type_rank(Type *type);

int type_checker_is_assignable(TypeChecker *checker, Type *dest_type,
                               Type *src_type);
int type_checker_is_assignable_from(TypeChecker *checker, Type *dest_type,
                                    Type *src_type, ASTNode *src_expr);
int type_checker_is_implicitly_convertible(Type *from_type, Type *to_type);
int type_checker_is_cast_valid(Type *from, Type *to);

int type_checker_integer_bounds(const Type *type, long long *out_min,
                                unsigned long long *out_max);
int type_checker_int_conversion_is_value_preserving(const Type *from,
                                                    const Type *to);
int type_checker_constant_fits_type(const Type *dest_type, const Type *src_type,
                                    long long value);
int type_checker_reject_rawptr_element_use(TypeChecker *checker,
                                           SourceLocation location,
                                           const char *what);
void type_checker_report_assign_mismatch(TypeChecker *checker,
                                         const ASTNode *src_expr,
                                         SourceLocation location,
                                         Type *dest_type, Type *src_type);
const char *type_checker_spell_type(const Type *type, char *room,
                                    size_t room_size);
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
void type_checker_report_parameter_shadow(TypeChecker *checker,
                                          SourceLocation location,
                                          const char *symbol_name,
                                          const Symbol *parameter);
void type_checker_set_launch_report(int enabled);

void type_checker_set_launch_report(int enabled);

void type_checker_report_type_mismatch_node(TypeChecker *checker,
                                            const ASTNode *node,
                                            const char *expected,
                                            const char *actual);
size_t type_checker_node_span_length(const ASTNode *node);
void type_checker_note_declared_here(TypeChecker *checker,
                                     const Symbol *symbol, const char *what);
void type_checker_mark_captures_used(TypeChecker *checker,
                                    const FunctionDeclaration *lam);
void type_checker_warn_unused_locals(TypeChecker *checker);
void type_checker_register_test_builtin(TypeChecker *checker, const char *name,
                                        size_t parameter_count);

uint32_t type_checker_intern_type(TypeChecker *checker, Type *type);
Type *type_checker_type_from_index(const TypeChecker *checker,
                                   uint32_t index);
Type *type_checker_canon_type(TypeChecker *checker, Type *type);

int type_checker_expand_comptime_block(TypeChecker *checker, ASTNode *block,
                                       int module_scope);
int type_checker_expand_directive_now(TypeChecker *checker, ASTNode *directive,
                                      ASTNode ***out, size_t *out_count);
int type_checker_prepare_directives(TypeChecker *checker, ASTNode *program);

typedef struct {
  size_t bindings_pushed;
  size_t notes_pushed;
} ComptimeDeclScope;
void type_checker_enter_expansion_decl(TypeChecker *checker,
                                       const ASTNode *declaration,
                                       ComptimeDeclScope *scope);
void type_checker_leave_expansion_decl(TypeChecker *checker,
                                       ComptimeDeclScope *scope);

Symbol *type_checker_lookup_expansion_binding(const TypeChecker *checker,
                                              const char *name);

int type_checker_check_composed_names(TypeChecker *checker, ASTNode *node);

const char *type_checker_expansion_note(TypeChecker *checker,
                                        const ASTNode *block,
                                        SourceSpan *out_origin);

int type_checker_declare_expansion_binding(TypeChecker *checker,
                                           const ASTNode *block);

void type_checker_expansions_destroy(ComptimeExpansionTable *table);

size_t type_checker_expansion_site_count(const TypeChecker *checker);
size_t type_checker_expansion_total_nodes(const TypeChecker *checker);
void type_checker_report_expansion(const TypeChecker *checker, FILE *out);
int type_checker_check_expansion_budget(TypeChecker *checker, size_t budget);

int type_checker_eval_offsetof(TypeChecker *checker, CallExpression *call,
                               SourceLocation location, long long *out_offset);

int type_checker_find_effect(const TypeChecker *checker, const char *name);
int type_checker_declare_effect(TypeChecker *checker, const char *name,
                                SourceLocation site, int is_exported,
                                int is_builtin);
void type_checker_declare_builtin_effects(TypeChecker *checker);
int type_checker_register_effects(TypeChecker *checker, ASTNode *program);
int type_checker_validate_effect_clauses(TypeChecker *checker,
                                         ASTNode *declaration,
                                         FunctionDeclaration *func_decl);
const char *type_checker_effect_signature(const char *const *with,
                                          size_t with_count, int closed,
                                          const char *const *requires,
                                          size_t require_count);
int type_checker_add_effect_obligation(TypeChecker *checker,
                                       const char *function,
                                       const char *signature,
                                       SourceLocation location);
const char *type_checker_function_value_name(TypeChecker *checker,
                                             const ASTNode *expression);
int type_checker_fn_effects_flow(TypeChecker *checker, const Type *dest,
                                 const Type *src);
int type_checker_fn_effect_sets_equal(const Type *lhs, const Type *rhs);
int type_checker_fn_signatures_equal(const Type *lhs, const Type *rhs);
int type_checker_fn_type_apply_effect_clauses(TypeChecker *checker, Type *fp,
                                              const char *clauses);
const char *type_checker_fn_type_effect_clause_start(const char *text);
void type_checker_report_effect_failure(TypeChecker *checker,
                                        const ASTNode *src_expr,
                                        SourceLocation location);

long long type_checker_layout_digest(const Type *type);

int type_checker_eval_layoutof(TypeChecker *checker, CallExpression *call,
                               SourceLocation location, long long *out_digest);

int type_checker_eval_comptime(TypeChecker *checker, ASTNode *expression,
                               ComptimeValue *out_value);

int type_checker_type_member_exists(const char *member);
int type_checker_eval_type_member(TypeChecker *checker,
                                  ComptimeValue type_value, const char *member,
                                  ComptimeValue *out_value);
int type_checker_field_member_exists(const char *member);

int type_checker_eval_row_member(TypeChecker *checker, ComptimeValue row,
                                 const char *member, ComptimeValue *out_value);
int type_checker_row_member_exists(TypeChecker *checker, ComptimeValue row,
                                   const char *member);
int type_checker_eval_field_member(TypeChecker *checker, ComptimeValue field,
                                   const char *member,
                                   ComptimeValue *out_value);
int type_checker_eval_sequence_member(TypeChecker *checker,
                                      ComptimeValue sequence,
                                      const char *member,
                                      ComptimeValue *out_value);
int type_checker_eval_sequence_index(ComptimeValue sequence, long long index,
                                     ComptimeValue *out_value);

void type_checker_register_kind_enum(TypeChecker *checker);
void type_checker_set_qualified_name(TypeChecker *checker, Type *type,
                                     const char *filename);
Type *type_checker_comptime_result(TypeChecker *checker, ComptimeValue value,
                                   ASTNode *expression);
Type *type_checker_kind_result(TypeChecker *checker, ComptimeValue value,
                               ASTNode *expression);
void type_checker_sequences_destroy(ComptimeSequenceArena *arena);

int type_contains_comptime_only(const Type *type);

int type_checker_reject_no_runtime_repr(TypeChecker *checker,
                                        SourceLocation location,
                                        const Type *type);

int type_checker_reject_comptime_escape(TypeChecker *checker,
                                        SourceLocation location,
                                        const Type *type);

int type_checker_declare_struct_placeholder(TypeChecker *checker,
                                            ASTNode *struct_decl);

int type_checker_process_struct_declaration(TypeChecker *checker,
                                            ASTNode *struct_decl);
int type_checker_process_enum_declaration(TypeChecker *checker,
                                          ASTNode *enum_decl);
int type_checker_process_type_declaration(TypeChecker *checker,
                                          ASTNode *type_decl);
int type_checker_check_type_predicate(TypeChecker *checker,
                                      ASTNode *type_decl);
int type_checker_process_declaration(TypeChecker *checker,
                                     ASTNode *declaration);

int type_checker_check_statement(TypeChecker *checker, ASTNode *statement);
int type_checker_check_expression(TypeChecker *checker, ASTNode *expression);
int type_checker_comptime_integer(TypeChecker *checker, ASTNode *node,
                                  long long *out_value);
int type_checker_fold_kernel_literals(ASTNode *node);
Type *type_checker_check_binary_expression(TypeChecker *checker,
                                           BinaryExpression *binop,
                                           SourceLocation location);

Symbol *type_checker_resolve_identifier(TypeChecker *checker,
                                        Identifier *identifier);

#endif
