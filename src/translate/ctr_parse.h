#ifndef METTLE_TRANSLATE_CTR_PARSE_H
#define METTLE_TRANSLATE_CTR_PARSE_H

#include "translate/ctr.h"

typedef struct CtrVarScope {
  CtrObj *var;
  CtrType *type_def;
  CtrObj *typedef_obj;
  CtrObj *enum_obj;
} CtrVarScope;

typedef struct CtrScope {
  struct CtrScope *next;
  CtrMap vars;
  CtrMap tags;
  int depth;
} CtrScope;

typedef struct CtrVarAttr {
  int is_typedef;
  int is_static;
  int is_extern;
  int is_inline;
  int is_tls;
  int is_noreturn;
  int align;
  int is_packed;
  const char *mettle_name;
  const char *mettle_module;
  const char *mettle_alias;
  int is_builtin;
} CtrVarAttr;

typedef struct CtrParser {
  CtrScope *scope;
  CtrObj *locals;
  CtrObj *globals;
  CtrObj *current_fn;
  CtrNode *brk_target;
  CtrNode *cont_target;
  CtrNode *current_switch;
  int switch_depth;
  int block_depth;
  CtrNode *gotos;
  CtrLabel *labels;
  CtrFile *unit_file;
  int pack;
  int obj_ids;
  int anon_count;
  CtrVec all_structs;
  CtrVarAttr trailing;
} CtrParser;

extern CtrParser ctr_ps;

void ctr_enter_scope(void);
void ctr_leave_scope(void);
CtrVarScope *ctr_push_scope(const char *name);
CtrVarScope *ctr_find_var(CtrToken *tok);
CtrType *ctr_find_typedef(CtrToken *tok);
CtrObj *ctr_new_lvar(const char *name, CtrType *ty, CtrToken *tok);
CtrObj *ctr_new_gvar(const char *name, CtrType *ty, CtrToken *tok);
CtrObj *ctr_new_anon_gvar(CtrType *ty, CtrToken *tok);
CtrObj *ctr_new_string_literal(CtrToken *tok);
char *ctr_get_ident(CtrToken *tok);
int ctr_is_typename(CtrToken *tok);
CtrType *ctr_declspec(CtrToken **rest, CtrToken *tok, CtrVarAttr *attr);
CtrType *ctr_declarator(CtrToken **rest, CtrToken *tok, CtrType *ty);
CtrType *ctr_abstract_declarator(CtrToken **rest, CtrToken *tok, CtrType *ty);
CtrType *ctr_typename(CtrToken **rest, CtrToken *tok);
CtrToken *ctr_skip_attributes(CtrToken *tok, CtrVarAttr *attr);

CtrNode *ctr_expr(CtrToken **rest, CtrToken *tok);
CtrNode *ctr_assign(CtrToken **rest, CtrToken *tok);
CtrNode *ctr_conditional(CtrToken **rest, CtrToken *tok);
CtrNode *ctr_new_num(int64_t val, CtrType *ty, CtrToken *tok);
CtrNode *ctr_new_var_node(CtrObj *var, CtrToken *tok);
CtrNode *ctr_new_binary(CtrNodeKind kind, CtrNode *lhs, CtrNode *rhs, CtrToken *tok);
CtrNode *ctr_new_unary(CtrNodeKind kind, CtrNode *expr, CtrToken *tok);

CtrNode *ctr_compound_stmt(CtrToken **rest, CtrToken *tok);
CtrNode *ctr_declaration(CtrToken **rest, CtrToken *tok, CtrType *basety, CtrVarAttr *attr);
CtrInit *ctr_parse_initializer(CtrToken **rest, CtrToken *tok, CtrType *ty, CtrType **new_ty);
CtrType *ctr_struct_union_decl(CtrToken **rest, CtrToken *tok, int is_union);
CtrType *ctr_enum_specifier(CtrToken **rest, CtrToken *tok);
int ctr_is_end(CtrToken *tok);
CtrToken *ctr_function(CtrToken *tok, CtrType *basety, CtrVarAttr *attr);
CtrToken *ctr_global_variable(CtrToken *tok, CtrType *basety, CtrVarAttr *attr);
void ctr_finish_struct(CtrStructDecl *decl);
CtrNode *ctr_compound_literal(CtrToken **rest, CtrToken *tok, CtrType *ty, CtrToken *start);

#endif
