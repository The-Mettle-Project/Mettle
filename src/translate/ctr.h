#ifndef METTLE_TRANSLATE_CTR_H
#define METTLE_TRANSLATE_CTR_H

#include <stddef.h>
#include <stdint.h>

typedef struct CtrBuf {
  char *data;
  size_t len;
  size_t cap;
} CtrBuf;

typedef struct CtrMapEntry {
  const char *key;
  int key_len;
  void *value;
} CtrMapEntry;

typedef struct CtrMap {
  CtrMapEntry *entries;
  int capacity;
  int used;
} CtrMap;

typedef struct CtrVec {
  void **items;
  int len;
  int cap;
} CtrVec;

typedef struct CtrFile {
  char *name;
  char *display;
  char *key;
  char *contents;
  int index;
  int is_system;
  int is_header;
  struct CtrModule *module;
  int line_marker_at;
  int line_marker_value;
  char *line_marker_name;
  CtrVec includes;
} CtrFile;

typedef enum {
  TK_IDENT,
  TK_PUNCT,
  TK_KEYWORD,
  TK_STR,
  TK_NUM,
  TK_PPNUM,
  TK_MARK_BEGIN,
  TK_MARK_END,
  TK_EOF
} CtrTokenKind;

typedef struct CtrHideset {
  struct CtrHideset *next;
  const char *name;
} CtrHideset;

typedef struct CtrType CtrType;
typedef struct CtrNode CtrNode;
typedef struct CtrObj CtrObj;
typedef struct CtrMember CtrMember;
typedef struct CtrMacro CtrMacro;
typedef struct CtrMacroUse CtrMacroUse;
typedef struct CtrStructDecl CtrStructDecl;

typedef struct CtrToken {
  CtrTokenKind kind;
  struct CtrToken *next;
  const char *loc;
  int len;
  CtrFile *file;
  int line;
  int col;
  int at_bol;
  int has_space;
  CtrHideset *hideset;
  struct CtrToken *origin;
  int64_t ival;
  double fval;
  CtrType *ty;
  char *str;
  int str_len;
  CtrMacroUse *mark;
  const char *comment;
  int line_delta;
} CtrToken;

typedef enum {
  TY_VOID,
  TY_BOOL,
  TY_CHAR,
  TY_SHORT,
  TY_INT,
  TY_LONG,
  TY_LLONG,
  TY_FLOAT,
  TY_DOUBLE,
  TY_LDOUBLE,
  TY_ENUM,
  TY_PTR,
  TY_FUNC,
  TY_ARRAY,
  TY_VLA,
  TY_STRUCT,
  TY_UNION
} CtrTypeKind;

struct CtrStructDecl {
  const char *tag;
  const char *mettle_name;
  int id;
  int is_union;
  int complete;
  int emitted;
  int size;
  int align;
  int is_packed;
  int is_flexible;
  int field_align;
  CtrMember *members;
  CtrToken *tok;
  CtrFile *file;
  CtrType *type;
  int unit_count;
  int used;
  CtrStructDecl *canonical;
  struct CtrModule *home;
};

struct CtrType {
  CtrTypeKind kind;
  int size;
  int align;
  int is_unsigned;
  int is_plain_char;
  int is_const;
  int is_volatile;
  CtrType *origin;
  CtrType *base;
  const char *typedef_name;
  CtrObj *typedef_obj;
  CtrToken *name;
  CtrToken *name_pos;
  int64_t array_len;
  CtrNode *vla_len;
  CtrObj *vla_size;
  CtrStructDecl *sdecl;
  CtrType *return_ty;
  CtrType *params;
  int is_variadic;
  int has_proto;
  CtrType *next;
  const char *enum_name;
  CtrToken *enum_tok;
  int enum_unsigned;
  CtrObj *param_obj;
};

struct CtrMember {
  CtrMember *next;
  CtrType *ty;
  CtrToken *tok;
  CtrToken *name;
  int idx;
  int align;
  int offset;
  int is_bitfield;
  int bit_offset;
  int bit_width;
  int unit_index;
  int unit_offset;
  int unit_size;
  const char *mettle_name;
  int anon;
  int emit_kind;
  const char *unit_name;
  int unit_is_field;
};

enum {
  CTR_MEMBER_DIRECT,
  CTR_MEMBER_CAST,
  CTR_MEMBER_BITFIELD
};

typedef struct CtrUnit {
  CtrFile *file;
  CtrObj *globals;
  const char *path;
  CtrVec files;
  struct CtrModule *module;
} CtrUnit;

extern CtrVec *ctr_unit_files;

typedef struct CtrRelocation CtrRelocation;
struct CtrRelocation {
  CtrRelocation *next;
  int offset;
  CtrObj *target;
  int64_t addend;
  char *label_str;
};

typedef struct CtrInit CtrInit;
struct CtrInit {
  CtrInit *next;
  CtrType *ty;
  CtrToken *tok;
  int is_flexible;
  CtrNode *expr;
  CtrInit **children;
  CtrMember *mem;
  int zeroed;
};

typedef struct CtrLabel {
  struct CtrLabel *next;
  const char *name;
  CtrNode *node;
  int used;
} CtrLabel;

struct CtrObj {
  CtrObj *next;
  const char *name;
  CtrType *ty;
  CtrToken *tok;
  struct CtrModule *home;
  CtrObj *same_as;
  int skip_emit;
  int is_local;
  int is_static;
  int is_extern;
  int is_definition;
  int is_function;
  int is_inline;
  int is_tentative;
  int is_param;
  int is_typedef;
  int is_enum_const;
  int64_t enum_value;
  CtrType *enum_type;
  CtrInit *init;
  CtrNode *init_expr;
  CtrObj *params;
  CtrNode *body;
  CtrObj *locals;
  CtrLabel *labels;
  int has_goto;
  int has_irregular_case;
  int uses_va;
  int has_setjmp;
  const char *mettle_name;
  const char *link_name;
  const char *mettle_module;
  const char *mettle_alias;
  int addr_taken;
  int assigned;
  int referenced;
  int emitted;
  CtrFile *file;
  CtrObj *owner;
  CtrObj *defined_by;
  int scope_depth;
  int id;
  int is_system;
  int is_builtin;
  int is_noreturn;
  CtrObj *va_param;
  int needs_zero;
  int local_index;
};

typedef enum {
  ND_NULL_EXPR,
  ND_ADD,
  ND_SUB,
  ND_MUL,
  ND_DIV,
  ND_MOD,
  ND_BITAND,
  ND_BITOR,
  ND_BITXOR,
  ND_SHL,
  ND_SHR,
  ND_EQ,
  ND_NE,
  ND_LT,
  ND_LE,
  ND_GT,
  ND_GE,
  ND_LOGAND,
  ND_LOGOR,
  ND_ASSIGN,
  ND_OPASSIGN,
  ND_PREINC,
  ND_PREDEC,
  ND_POSTINC,
  ND_POSTDEC,
  ND_COND,
  ND_COMMA,
  ND_MEMBER,
  ND_ADDR,
  ND_DEREF,
  ND_INDEX,
  ND_NOT,
  ND_BITNOT,
  ND_NEG,
  ND_POS,
  ND_NUM,
  ND_STR,
  ND_VAR,
  ND_FUNCALL,
  ND_CAST,
  ND_SIZEOF,
  ND_ALIGNOF,
  ND_COMPOUND_LIT,
  ND_STMT_EXPR,
  ND_VA_START,
  ND_VA_ARG,
  ND_VA_END,
  ND_VA_COPY,
  ND_BUILTIN,
  ND_MACRO,
  ND_LABEL_VAL,
  ND_RETURN,
  ND_IF,
  ND_FOR,
  ND_DO,
  ND_WHILE,
  ND_SWITCH,
  ND_CASE,
  ND_BLOCK,
  ND_GOTO,
  ND_GOTO_EXPR,
  ND_LABEL,
  ND_BREAK,
  ND_CONTINUE,
  ND_EXPR_STMT,
  ND_DECL,
  ND_ASM,
  ND_EMPTY
} CtrNodeKind;

struct CtrNode {
  CtrNodeKind kind;
  CtrNode *next;
  CtrType *ty;
  CtrToken *tok;
  CtrNode *lhs;
  CtrNode *rhs;
  CtrNode *cond;
  CtrNode *then;
  CtrNode *els;
  CtrNode *init;
  CtrNode *inc;
  CtrNode *body;
  CtrNode *args;
  CtrMember *member;
  CtrObj *var;
  CtrType *func_ty;
  CtrType *cast_ty;
  int implicit;
  int op;
  int64_t ival;
  double fval;
  int is_float_lit;
  const char *spelling;
  char *str;
  int str_len;
  CtrType *str_ty;
  CtrNode *case_next;
  CtrNode *default_case;
  int64_t case_begin;
  int64_t case_end;
  int is_default;
  CtrNode *target;
  const char *label;
  CtrLabel *label_ref;
  CtrInit *decl_init;
  CtrMacroUse *macro;
  int builtin;
  int has_break;
  int has_continue;
  int irregular;
  int id;
  int flow_index;
  const char *mettle_label;
  int print_plain;
  int as_assign;
  int inc_at_continue;
};

typedef struct CtrMacroArg {
  const char *name;
  struct CtrMacroArg *next;
} CtrMacroArg;

struct CtrMacro {
  const char *name;
  struct CtrModule *home;
  CtrMacro *same_as;
  int is_objlike;
  CtrMacroArg *params;
  const char *va_args_name;
  CtrToken *body;
  CtrToken *def_tok;
  CtrFile *file;
  int builtin;
  int recoverable;
  int use_count;
  int recovered;
  int rejected;
  const char *mettle_name;
  CtrNode *value_expr;
  CtrType *value_type;
  CtrType **param_types;
  CtrType *result_type;
  int param_count;
  CtrNode *fn_body;
  CtrObj **fn_params;
  int fn_ok;
  int emitted;
  int removed;
};

struct CtrMacroUse {
  CtrMacro *macro;
  CtrToken **args;
  int arg_count;
  CtrToken *use_tok;
  CtrToken *end;
  CtrMacroUse *inner;
  int id;
};

typedef enum {
  CTR_TARGET_WINDOWS_X64,
  CTR_TARGET_LINUX_X64,
  CTR_TARGET_LINUX_ARM64,
  CTR_TARGET_MACOS_ARM64
} CtrTarget;

typedef struct CtrModule {
  CtrFile *file;
  const char *name;
  const char *out_path;
  int is_unit;
  CtrVec imports;
  CtrVec items;
  int needs_c;
  int needs_math;
  CtrMap externs;
  CtrBuf extern_text;
  const char *init_name;
  int index;
} CtrModule;

typedef struct CtrOptions {
  CtrVec inputs;
  CtrVec include_dirs;
  CtrVec defines;
  const char *output;
  const char *system_include;
  CtrTarget target;
  int expand_macros;
  int keep_comments;
  int emit_preprocessed;
  int verbose;
} CtrOptions;

typedef struct CtrProgram {
  CtrOptions *options;
  CtrObj *globals;
  CtrVec structs;
  CtrVec typedefs;
  CtrVec enum_consts;
  CtrVec macros;
  CtrVec modules;
  CtrVec files;
  CtrVec units;
  CtrVec fnptr_aliases;
  int errors;
  int warnings;
} CtrProgram;

extern CtrProgram *ctr_prog;
extern CtrMap ctr_libc_mappings;
extern CtrType *ctr_ty_void;
extern CtrType *ctr_ty_bool;
extern CtrType *ctr_ty_char;
extern CtrType *ctr_ty_schar;
extern CtrType *ctr_ty_uchar;
extern CtrType *ctr_ty_short;
extern CtrType *ctr_ty_ushort;
extern CtrType *ctr_ty_int;
extern CtrType *ctr_ty_uint;
extern CtrType *ctr_ty_long;
extern CtrType *ctr_ty_ulong;
extern CtrType *ctr_ty_llong;
extern CtrType *ctr_ty_ullong;
extern CtrType *ctr_ty_float;
extern CtrType *ctr_ty_double;
extern CtrType *ctr_ty_ldouble;

void *ctr_alloc(size_t size);
char *ctr_strdup(const char *s);
char *ctr_strndup(const char *s, size_t n);
char *ctr_format(const char *fmt, ...);
void ctr_buf_put(CtrBuf *buf, const char *s);
void ctr_buf_putn(CtrBuf *buf, const char *s, size_t n);
void ctr_buf_putc(CtrBuf *buf, char c);
void ctr_buf_printf(CtrBuf *buf, const char *fmt, ...);
char *ctr_buf_take(CtrBuf *buf);
void ctr_vec_push(CtrVec *vec, void *item);
void *ctr_map_get(CtrMap *map, const char *key);
void *ctr_map_get2(CtrMap *map, const char *key, int key_len);
void ctr_map_put(CtrMap *map, const char *key, void *value);
void ctr_map_put2(CtrMap *map, const char *key, int key_len, void *value);
void ctr_map_delete(CtrMap *map, const char *key);
void ctr_map_delete2(CtrMap *map, const char *key, int key_len);
char *ctr_read_file(const char *path);
int ctr_file_exists(const char *path);
int ctr_write_file(const char *path, const char *data, size_t len);
char *ctr_path_dir(const char *path);
char *ctr_path_join(const char *dir, const char *name);
char *ctr_path_stem(const char *path);
char *ctr_path_key(const char *path);
int ctr_make_dirs(const char *path);

void ctr_error_tok(CtrToken *tok, const char *fmt, ...);
void ctr_warn_tok(CtrToken *tok, const char *fmt, ...);
void ctr_fatal_tok(CtrToken *tok, const char *fmt, ...);
void ctr_fatal(const char *fmt, ...);
void ctr_note(const char *fmt, ...);
int ctr_equal(CtrToken *tok, const char *op);
CtrToken *ctr_skip(CtrToken *tok, const char *op);
int ctr_consume(CtrToken **rest, CtrToken *tok, const char *str);
char *ctr_token_text(CtrToken *tok);

CtrFile *ctr_new_file(const char *name, int index, char *contents);
CtrToken *ctr_tokenize(CtrFile *file);
CtrToken *ctr_tokenize_file(const char *path, int is_system);
void ctr_convert_pp_tokens(CtrToken *tok);
void ctr_convert_pp_number(CtrToken *tok);
int ctr_is_keyword(CtrToken *tok);
CtrToken *ctr_new_eof(CtrToken *tok);
CtrToken *ctr_copy_token(CtrToken *tok);
int ctr_read_escaped_char(const char **new_pos, const char *p);
int ctr_is_ident1(uint32_t c);
int ctr_is_ident2(uint32_t c);
uint32_t ctr_decode_utf8(const char **new_pos, const char *p);
int ctr_encode_utf8(char *buf, uint32_t c);

void ctr_pp_init(void);
void ctr_pp_define(const char *name, const char *body);
void ctr_pp_undef(const char *name);
CtrToken *ctr_preprocess(CtrToken *tok);
const char *ctr_search_include(const char *filename, int quoted, const char *from_dir);

CtrType *ctr_new_type(CtrTypeKind kind, int size, int align);
CtrType *ctr_copy_type(CtrType *ty);
CtrType *ctr_pointer_to(CtrType *base);
CtrType *ctr_func_type(CtrType *return_ty);
CtrType *ctr_array_of(CtrType *base, int64_t len);
CtrType *ctr_vla_of(CtrType *base, CtrNode *expr);
CtrType *ctr_enum_type(void);
CtrType *ctr_struct_type(void);
CtrType *ctr_unqual(CtrType *ty);
int ctr_is_integer(CtrType *ty);
int ctr_is_flonum(CtrType *ty);
int ctr_is_numeric(CtrType *ty);
int ctr_is_scalar(CtrType *ty);
int ctr_is_compatible(CtrType *t1, CtrType *t2);
int ctr_same_type(CtrType *a, CtrType *b);
CtrType *ctr_common_type(CtrType *a, CtrType *b);
CtrType *ctr_promote(CtrType *ty);
void ctr_add_type(CtrNode *node);
void ctr_types_init(CtrTarget target);
int ctr_int_rank(CtrType *ty);
CtrType *ctr_size_t_type(void);
CtrType *ctr_ptrdiff_type(void);
CtrType *ctr_wchar_type(void);
CtrType *ctr_strip_typedef(CtrType *ty);
CtrNode *ctr_new_cast(CtrNode *expr, CtrType *ty);
CtrNode *ctr_new_node(CtrNodeKind kind, CtrToken *tok);
void ctr_layout_struct(CtrStructDecl *decl, CtrTarget target);
void ctr_sync_type(CtrType *ty);

CtrObj *ctr_parse_unit(CtrToken *tok, CtrFile *file);
int64_t ctr_const_expr(CtrToken **rest, CtrToken *tok);
int64_t ctr_eval(CtrNode *node);
int64_t ctr_eval2(CtrNode *node, CtrObj **label, int64_t *ok);
int ctr_is_const_expr(CtrNode *node);
double ctr_eval_double(CtrNode *node);
int ctr_try_eval(CtrNode *node, int64_t *out);
int ctr_eval_address(CtrNode *node, CtrObj **base, int64_t *offset);
int ctr_has_side_effects(CtrNode *node);

int ctr_flow_needs_cfg(CtrObj *fn);

void ctr_emit_program(CtrProgram *prog);
const char *ctr_mettle_type(CtrType *ty);
const char *ctr_mettle_scalar_name(CtrType *ty);

#include "translate/translate.h"

#endif
