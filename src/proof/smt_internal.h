#ifndef METTLE_SMT_INTERNAL_H
#define METTLE_SMT_INTERNAL_H

#include "smt.h"

#define SMT_WORDS 4u

typedef struct {
  uint64_t w[SMT_WORDS];
} SmtBits;

typedef struct {
  uint16_t kind;
  uint16_t nargs;
  uint32_t sort;
  uint32_t args;
  uint32_t p0;
  uint32_t p1;
  uint32_t next;
  uint32_t hash;
} SmtNode;

enum { SMT_SORT_BOOL = 1, SMT_SORT_BV = 2, SMT_SORT_ARRAY = 3 };

typedef struct {
  uint32_t kind;
  uint32_t width;
  uint32_t a;
  uint32_t b;
} SmtSortInfo;

typedef struct {
  uint32_t name;
  uint32_t range;
  uint32_t domain;
  uint32_t arity;
} SmtFuncInfo;

struct SmtCtx {
  SmtNode *nodes;
  uint32_t node_count;
  uint32_t node_cap;
  uint32_t *argpool;
  size_t arg_count;
  size_t arg_cap;
  uint32_t *buckets;
  uint32_t bucket_cap;
  SmtSortInfo *sorts;
  uint32_t sort_count;
  uint32_t sort_cap;
  SmtBits *consts;
  uint32_t const_count;
  uint32_t const_cap;
  char **names;
  uint32_t *name_hash;
  uint32_t name_count;
  uint32_t name_cap;
  uint32_t *name_buckets;
  uint32_t name_bucket_cap;
  SmtFuncInfo *funcs;
  uint32_t func_count;
  uint32_t func_cap;
  uint32_t *func_domains;
  uint32_t func_domain_count;
  uint32_t func_domain_cap;
  uint32_t fresh_counter;
  int simplify;
  int error;
  char error_message[256];
  uint32_t *mark;
  uint32_t mark_cap;
  uint32_t mark_stamp;
  SmtTerm bool_true;
  SmtTerm bool_false;
  SmtSort sort_bool;
};

typedef struct {
  SmtBits index;
  SmtBits value;
} SmtArrayEntry;

typedef struct {
  uint32_t term;
  SmtArrayEntry *entries;
  uint32_t count;
  uint32_t cap;
} SmtArrayModel;

typedef struct {
  SmtBits *args;
  SmtBits value;
} SmtFuncEntry;

typedef struct {
  uint32_t func;
  SmtFuncEntry *entries;
  uint32_t count;
  uint32_t cap;
} SmtFuncModel;

typedef struct {
  SmtBits *values;
  uint8_t *has;
  uint32_t cap;
  SmtArrayModel *arrays;
  uint32_t array_count;
  uint32_t array_cap;
  SmtFuncModel *funcs;
  uint32_t func_count;
  uint32_t func_cap;
} SmtModel;

typedef struct {
  SmtCtx *ctx;
  const SmtModel *model;
  SmtBits *memo;
  uint32_t *stamp;
  uint32_t cap;
  uint32_t current;
  int failed;
} SmtEval;

typedef struct {
  SmtTerm *keys;
  SmtTerm *vals;
  uint32_t cap;
  uint32_t count;
} SmtMap;

void smt_map_init(SmtMap *m);
void smt_map_free(SmtMap *m);
SmtTerm smt_map_get(const SmtMap *m, SmtTerm k);
void smt_map_put(SmtMap *m, SmtTerm k, SmtTerm v);

static inline const SmtNode *smt_node(const SmtCtx *ctx, SmtTerm t) {
  return &ctx->nodes[t];
}

static inline SmtTerm smt_arg(const SmtCtx *ctx, SmtTerm t, unsigned i) {
  return ctx->argpool[ctx->nodes[t].args + i];
}

static inline unsigned smt_width(const SmtCtx *ctx, SmtTerm t) {
  return ctx->sorts[ctx->nodes[t].sort].width;
}

void smt_bits_zero(SmtBits *b);
void smt_bits_mask(SmtBits *b, unsigned width);
void smt_bits_from_u64(SmtBits *b, uint64_t v, unsigned width);
int smt_bits_eq(const SmtBits *a, const SmtBits *b);
int smt_bits_is_zero(const SmtBits *a);
int smt_bits_get(const SmtBits *a, unsigned i);
void smt_bits_set(SmtBits *a, unsigned i, int v);
void smt_bits_ones(SmtBits *b, unsigned width);
int smt_bits_ult(const SmtBits *a, const SmtBits *b);
int smt_bits_slt(const SmtBits *a, const SmtBits *b, unsigned width);
void smt_bits_add(SmtBits *r, const SmtBits *a, const SmtBits *b,
                  unsigned width);
void smt_bits_sub(SmtBits *r, const SmtBits *a, const SmtBits *b,
                  unsigned width);
void smt_bits_neg(SmtBits *r, const SmtBits *a, unsigned width);
void smt_bits_not(SmtBits *r, const SmtBits *a, unsigned width);
void smt_bits_mul(SmtBits *r, const SmtBits *a, const SmtBits *b,
                  unsigned width);
void smt_bits_udivrem(SmtBits *q, SmtBits *r, const SmtBits *a,
                      const SmtBits *b, unsigned width);
void smt_bits_shl(SmtBits *r, const SmtBits *a, const SmtBits *s,
                  unsigned width);
void smt_bits_lshr(SmtBits *r, const SmtBits *a, const SmtBits *s,
                   unsigned width);
void smt_bits_ashr(SmtBits *r, const SmtBits *a, const SmtBits *s,
                   unsigned width);
int smt_bits_apply(SmtKind kind, SmtBits *r, const SmtBits *a,
                   const SmtBits *b, unsigned width, uint32_t p0,
                   uint32_t p1, unsigned bwidth);
void smt_bits_print_hex(const SmtBits *b, unsigned width, FILE *out);

void smt_model_init(SmtModel *m);
void smt_model_free(SmtModel *m);
void smt_model_set(SmtModel *m, SmtTerm var, const SmtBits *value);
SmtArrayModel *smt_model_array(SmtModel *m, SmtTerm var);
SmtFuncModel *smt_model_func(SmtModel *m, SmtFunc f);
void smt_model_array_put(SmtArrayModel *a, const SmtBits *index,
                         const SmtBits *value);
void smt_model_func_put(SmtFuncModel *f, unsigned arity, const SmtBits *args,
                        const SmtBits *value);

void smt_eval_init(SmtEval *e, SmtCtx *ctx, const SmtModel *model);
void smt_eval_reset(SmtEval *e);
void smt_eval_free(SmtEval *e);
int smt_eval(SmtEval *e, SmtTerm t, SmtBits *out);

uint32_t smt_mark_begin(SmtCtx *ctx);
int smt_mark_test_set(SmtCtx *ctx, SmtTerm t, uint32_t stamp);

double smt_now_seconds(void);

#endif
