#ifndef METTLE_TRANSLATE_CTR_EMIT_H
#define METTLE_TRANSLATE_CTR_EMIT_H

#include "translate/ctr.h"

typedef struct CtrEmitter {
  CtrBuf *out;
  int indent;
  CtrModule *mod;
  CtrObj *fn;
  CtrMap local_names;
  int temps;
  int labels;
  int cfg_mode;
  CtrNode *bare_node;
} CtrEmitter;

typedef enum {
  PREC_LOWEST = 0,
  PREC_IFEXPR = 1,
  PREC_LOGOR = 2,
  PREC_LOGAND = 3,
  PREC_BITOR = 4,
  PREC_BITXOR = 5,
  PREC_BITAND = 6,
  PREC_EQ = 7,
  PREC_REL = 8,
  PREC_SHIFT = 9,
  PREC_ADD = 10,
  PREC_MUL = 11,
  PREC_UNARY = 12,
  PREC_POSTFIX = 13
} CtrPrec;

extern CtrEmitter *ctr_em;

const char *ctr_mettle_name_of(CtrObj *obj);
const char *ctr_claim_name(const char *base);
void ctr_use_module(CtrModule *home);
const char *ctr_wide_string_name(CtrNode *str);
const char *ctr_struct_name(CtrStructDecl *decl);
const char *ctr_member_name(CtrMember *mem);
const char *ctr_type_text(CtrType *ty);
const char *ctr_fn_type_text(CtrType *fn);
const char *ctr_return_type_text(CtrType *ty);
const char *ctr_scalar_type(CtrType *ty);
int ctr_is_mettle_keyword(const char *name);
const char *ctr_safe_ident(const char *name);
int ctr_is_opaque_struct(CtrType *ty);
CtrMember *ctr_union_primary(CtrStructDecl *decl);
const char *ctr_unit_type(int size);
void ctr_prepare_names(CtrProgram *prog);
void ctr_assign_local_names(CtrObj *fn);
int ctr_same_mettle_type(CtrType *a, CtrType *b);
int ctr_is_va_list(CtrType *ty);
int ctr_is_bool_type(CtrType *ty);
const char *ctr_unique_label(const char *base);

char *ctr_expr_text(CtrNode *node, int prec);
char *ctr_cond_text(CtrNode *node);
char *ctr_value_text(CtrNode *node, CtrType *ty, int prec);
char *ctr_assign_value_text(CtrNode *node, CtrType *ty);
const char *ctr_zero_suffix(CtrObj *var);
void ctr_find_uninitialized_reads(CtrObj *fn, int whole_function);
char *ctr_lvalue_text(CtrNode *node, int prec);
char *ctr_literal_text(CtrNode *node, CtrType *ty);
char *ctr_init_literal(CtrInit *init, CtrType *ty, int *ok);
int ctr_init_is_constant(CtrInit *init);
char *ctr_float_text(double v, int is_float);
char *ctr_string_literal_text(const char *bytes, int len, int elem);
void ctr_emit_expr_stmt(CtrNode *node);
void ctr_emit_assign_stmt(CtrNode *lhs, CtrNode *rhs, CtrType *ty);
void ctr_emit_bitfield_store(CtrNode *member_node, const char *value_text);
char *ctr_bitfield_read(CtrNode *member_node);
char *ctr_bitfield_unit(CtrNode *member_node);
char *ctr_new_temp(const char *base);
int ctr_has_vla(CtrType *ty);

void ctr_line(const char *fmt, ...);
void ctr_open(const char *fmt, ...);
void ctr_close(void);
void ctr_close_with(const char *fmt, ...);
void ctr_pre_line(const char *fmt, ...);

void ctr_emit_block_body(CtrNode *block);
void ctr_emit_stmt(CtrNode *node);
void ctr_emit_local_decl(CtrNode *node, int as_assignment);
void ctr_emit_function_body(CtrObj *fn);
void ctr_emit_function(CtrBuf *out, CtrObj *fn);
void ctr_structure_gotos(CtrObj *fn);
int ctr_body_falls_through(CtrNode *body);
void ctr_emit_struct(CtrBuf *out, CtrStructDecl *decl);
void ctr_emit_global_init_stmts(CtrObj *var);
void ctr_plan_all_structs(CtrProgram *prog);
void ctr_recover_macros(CtrProgram *prog);
int ctr_needs_global_init(CtrObj *globals);
void ctr_emit_cfg_body(CtrObj *fn);
int ctr_needs_cfg(CtrObj *fn);
void ctr_hoist_decl_names(CtrNode *node, CtrVec *out);

char *ctr_break_text(CtrNode *target);
char *ctr_continue_text(CtrNode *target);

const char *ctr_callee_text(CtrNode *fn_node, CtrType *fnty, int *is_mettle_variadic, CtrObj **callee);
const char *ctr_module_prefix(const char *module);

#endif
