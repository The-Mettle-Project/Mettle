#include "smt_internal.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *smt_grow(void *ptr, size_t elem, uint32_t *cap, size_t need) {
  if (need <= *cap) {
    return ptr;
  }
  size_t next = *cap ? (size_t)*cap : 16u;
  while (next < need) {
    next *= 2u;
  }
  void *out = realloc(ptr, next * elem);
  if (!out) {
    fprintf(stderr, "smt: out of memory\n");
    abort();
  }
  *cap = (uint32_t)next;
  return out;
}

static void smt_fail(SmtCtx *ctx, const char *message) {
  if (!ctx->error) {
    ctx->error = 1;
    snprintf(ctx->error_message, sizeof(ctx->error_message), "%s", message);
  }
}

int smt_ctx_error(const SmtCtx *ctx, const char **message) {
  if (message) {
    *message = ctx->error ? ctx->error_message : NULL;
  }
  return ctx->error;
}

void smt_ctx_clear_error(SmtCtx *ctx) {
  ctx->error = 0;
  ctx->error_message[0] = 0;
}

uint32_t smt_ctx_term_count(const SmtCtx *ctx) { return ctx->node_count; }

static uint32_t smt_hash_string(const char *s) {
  uint32_t h = 2166136261u;
  while (*s) {
    h = (h ^ (uint8_t)*s++) * 16777619u;
  }
  return h;
}

static uint32_t smt_intern(SmtCtx *ctx, const char *name) {
  uint32_t h = smt_hash_string(name);
  if (ctx->name_bucket_cap) {
    uint32_t slot = h & (ctx->name_bucket_cap - 1u);
    while (ctx->name_buckets[slot]) {
      uint32_t id = ctx->name_buckets[slot] - 1u;
      if (ctx->name_hash[id] == h && strcmp(ctx->names[id], name) == 0) {
        return id;
      }
      slot = (slot + 1u) & (ctx->name_bucket_cap - 1u);
    }
  }
  if ((ctx->name_count + 1u) * 2u > ctx->name_bucket_cap) {
    uint32_t cap = ctx->name_bucket_cap ? ctx->name_bucket_cap * 2u : 64u;
    uint32_t *buckets = calloc(cap, sizeof(uint32_t));
    for (uint32_t i = 0; i < ctx->name_count; i++) {
      uint32_t slot = ctx->name_hash[i] & (cap - 1u);
      while (buckets[slot]) {
        slot = (slot + 1u) & (cap - 1u);
      }
      buckets[slot] = i + 1u;
    }
    free(ctx->name_buckets);
    ctx->name_buckets = buckets;
    ctx->name_bucket_cap = cap;
  }
  uint32_t cap_before = ctx->name_cap;
  ctx->names = smt_grow(ctx->names, sizeof(char *), &ctx->name_cap,
                        ctx->name_count + 1u);
  uint32_t cap_copy = cap_before;
  ctx->name_hash = smt_grow(ctx->name_hash, sizeof(uint32_t), &cap_copy,
                            ctx->name_cap);
  uint32_t id = ctx->name_count++;
  size_t len = strlen(name);
  ctx->names[id] = malloc(len + 1u);
  memcpy(ctx->names[id], name, len + 1u);
  ctx->name_hash[id] = h;
  uint32_t slot = h & (ctx->name_bucket_cap - 1u);
  while (ctx->name_buckets[slot]) {
    slot = (slot + 1u) & (ctx->name_bucket_cap - 1u);
  }
  ctx->name_buckets[slot] = id + 1u;
  return id;
}

static SmtSort smt_sort_find(SmtCtx *ctx, uint32_t kind, uint32_t width,
                             uint32_t a, uint32_t b) {
  for (uint32_t i = 1; i < ctx->sort_count; i++) {
    const SmtSortInfo *s = &ctx->sorts[i];
    if (s->kind == kind && s->width == width && s->a == a && s->b == b) {
      return i;
    }
  }
  ctx->sorts = smt_grow(ctx->sorts, sizeof(SmtSortInfo), &ctx->sort_cap,
                        ctx->sort_count + 1u);
  SmtSortInfo *s = &ctx->sorts[ctx->sort_count];
  s->kind = kind;
  s->width = width;
  s->a = a;
  s->b = b;
  return ctx->sort_count++;
}

SmtSort smt_bool_sort(SmtCtx *ctx) { return ctx->sort_bool; }

SmtSort smt_bv_sort(SmtCtx *ctx, unsigned width) {
  if (width == 0 || width > SMT_MAX_WIDTH) {
    smt_fail(ctx, "bit-vector width out of range");
    width = width == 0 ? 1u : SMT_MAX_WIDTH;
  }
  return smt_sort_find(ctx, SMT_SORT_BV, width, 0, 0);
}

SmtSort smt_array_sort(SmtCtx *ctx, SmtSort index, SmtSort element) {
  return smt_sort_find(ctx, SMT_SORT_ARRAY, 0, index, element);
}

int smt_sort_is_bool(const SmtCtx *ctx, SmtSort s) {
  return ctx->sorts[s].kind == SMT_SORT_BOOL;
}
int smt_sort_is_bv(const SmtCtx *ctx, SmtSort s) {
  return ctx->sorts[s].kind == SMT_SORT_BV;
}
int smt_sort_is_array(const SmtCtx *ctx, SmtSort s) {
  return ctx->sorts[s].kind == SMT_SORT_ARRAY;
}
unsigned smt_sort_width(const SmtCtx *ctx, SmtSort s) {
  return ctx->sorts[s].width;
}
SmtSort smt_sort_index(const SmtCtx *ctx, SmtSort s) { return ctx->sorts[s].a; }
SmtSort smt_sort_element(const SmtCtx *ctx, SmtSort s) {
  return ctx->sorts[s].b;
}

static uint32_t smt_node_hash(uint32_t kind, uint32_t sort, uint32_t p0,
                              uint32_t p1, unsigned nargs,
                              const SmtTerm *args) {
  uint64_t h = 1469598103934665603ULL;
  h = (h ^ kind) * 1099511628211ULL;
  h = (h ^ sort) * 1099511628211ULL;
  h = (h ^ p0) * 1099511628211ULL;
  h = (h ^ p1) * 1099511628211ULL;
  for (unsigned i = 0; i < nargs; i++) {
    h = (h ^ args[i]) * 1099511628211ULL;
  }
  return (uint32_t)(h ^ (h >> 32));
}

static void smt_rehash(SmtCtx *ctx) {
  uint32_t cap = ctx->bucket_cap ? ctx->bucket_cap * 2u : 1024u;
  uint32_t *buckets = calloc(cap, sizeof(uint32_t));
  for (uint32_t i = 1; i < ctx->node_count; i++) {
    uint32_t slot = ctx->nodes[i].hash & (cap - 1u);
    ctx->nodes[i].next = buckets[slot];
    buckets[slot] = i;
  }
  free(ctx->buckets);
  ctx->buckets = buckets;
  ctx->bucket_cap = cap;
}

static SmtTerm smt_node_make(SmtCtx *ctx, uint32_t kind, uint32_t sort,
                             uint32_t p0, uint32_t p1, unsigned nargs,
                             const SmtTerm *args, const SmtBits *bits) {
  uint32_t h = smt_node_hash(kind, sort, p0, p1, nargs, args);
  if (bits) {
    for (unsigned i = 0; i < SMT_WORDS; i++) {
      h = (h ^ (uint32_t)bits->w[i] ^ (uint32_t)(bits->w[i] >> 32)) *
          16777619u;
    }
  }
  uint32_t slot = h & (ctx->bucket_cap - 1u);
  for (uint32_t id = ctx->buckets[slot]; id; id = ctx->nodes[id].next) {
    const SmtNode *n = &ctx->nodes[id];
    if (n->hash != h || n->kind != kind || n->sort != sort ||
        n->nargs != nargs) {
      continue;
    }
    if (bits) {
      if (smt_bits_eq(&ctx->consts[n->p0], bits)) {
        return id;
      }
      continue;
    }
    if (n->p0 != p0 || n->p1 != p1) {
      continue;
    }
    if (nargs && memcmp(&ctx->argpool[n->args], args,
                        nargs * sizeof(SmtTerm)) != 0) {
      continue;
    }
    return id;
  }
  if (bits) {
    ctx->consts = smt_grow(ctx->consts, sizeof(SmtBits), &ctx->const_cap,
                           ctx->const_count + 1u);
    ctx->consts[ctx->const_count] = *bits;
    p0 = ctx->const_count++;
  }
  if (ctx->arg_count + nargs > ctx->arg_cap) {
    size_t cap = ctx->arg_cap ? ctx->arg_cap : 1024u;
    while (cap < ctx->arg_count + nargs) {
      cap *= 2u;
    }
    ctx->argpool = realloc(ctx->argpool, cap * sizeof(uint32_t));
    ctx->arg_cap = cap;
  }
  ctx->nodes = smt_grow(ctx->nodes, sizeof(SmtNode), &ctx->node_cap,
                        ctx->node_count + 1u);
  SmtTerm id = ctx->node_count++;
  SmtNode *n = &ctx->nodes[id];
  n->kind = (uint16_t)kind;
  n->nargs = (uint16_t)nargs;
  n->sort = sort;
  n->args = (uint32_t)ctx->arg_count;
  n->p0 = p0;
  n->p1 = p1;
  n->hash = h;
  if (nargs) {
    memcpy(&ctx->argpool[ctx->arg_count], args, nargs * sizeof(SmtTerm));
    ctx->arg_count += nargs;
  }
  n->next = ctx->buckets[slot];
  ctx->buckets[slot] = id;
  if (ctx->node_count > ctx->bucket_cap) {
    smt_rehash(ctx);
  }
  return id;
}

SmtCtx *smt_ctx_new(void) {
  SmtCtx *ctx = calloc(1, sizeof(SmtCtx));
  ctx->simplify = 1;
  ctx->node_count = 1;
  ctx->nodes = smt_grow(NULL, sizeof(SmtNode), &ctx->node_cap, 1024u);
  memset(&ctx->nodes[0], 0, sizeof(SmtNode));
  smt_rehash(ctx);
  ctx->sort_count = 1;
  ctx->sorts = smt_grow(NULL, sizeof(SmtSortInfo), &ctx->sort_cap, 16u);
  memset(&ctx->sorts[0], 0, sizeof(SmtSortInfo));
  ctx->sort_bool = smt_sort_find(ctx, SMT_SORT_BOOL, 1, 0, 0);
  ctx->func_count = 1;
  ctx->funcs = smt_grow(NULL, sizeof(SmtFuncInfo), &ctx->func_cap, 16u);
  memset(&ctx->funcs[0], 0, sizeof(SmtFuncInfo));
  ctx->bool_true = smt_node_make(ctx, SMT_K_TRUE, ctx->sort_bool, 0, 0, 0,
                                 NULL, NULL);
  ctx->bool_false = smt_node_make(ctx, SMT_K_FALSE, ctx->sort_bool, 0, 0, 0,
                                  NULL, NULL);
  return ctx;
}

void smt_ctx_free(SmtCtx *ctx) {
  if (!ctx) {
    return;
  }
  for (uint32_t i = 0; i < ctx->name_count; i++) {
    free(ctx->names[i]);
  }
  free(ctx->names);
  free(ctx->name_hash);
  free(ctx->name_buckets);
  free(ctx->nodes);
  free(ctx->argpool);
  free(ctx->buckets);
  free(ctx->sorts);
  free(ctx->consts);
  free(ctx->funcs);
  free(ctx->func_domains);
  free(ctx->mark);
  free(ctx);
}

void smt_ctx_set_simplify(SmtCtx *ctx, int enabled) {
  ctx->simplify = enabled;
}

uint32_t smt_mark_begin(SmtCtx *ctx) {
  if (ctx->mark_cap < ctx->node_count) {
    uint32_t old = ctx->mark_cap;
    ctx->mark = smt_grow(ctx->mark, sizeof(uint32_t), &ctx->mark_cap,
                         ctx->node_count + 1024u);
    memset(ctx->mark + old, 0, (ctx->mark_cap - old) * sizeof(uint32_t));
  }
  if (++ctx->mark_stamp == 0) {
    memset(ctx->mark, 0, ctx->mark_cap * sizeof(uint32_t));
    ctx->mark_stamp = 1;
  }
  return ctx->mark_stamp;
}

int smt_mark_test_set(SmtCtx *ctx, SmtTerm t, uint32_t stamp) {
  if (t >= ctx->mark_cap) {
    uint32_t old = ctx->mark_cap;
    ctx->mark = smt_grow(ctx->mark, sizeof(uint32_t), &ctx->mark_cap,
                         t + 1024u);
    memset(ctx->mark + old, 0, (ctx->mark_cap - old) * sizeof(uint32_t));
  }
  if (ctx->mark[t] == stamp) {
    return 1;
  }
  ctx->mark[t] = stamp;
  return 0;
}

SmtTerm smt_true(SmtCtx *ctx) { return ctx->bool_true; }
SmtTerm smt_false(SmtCtx *ctx) { return ctx->bool_false; }
SmtTerm smt_bool(SmtCtx *ctx, int value) {
  return value ? ctx->bool_true : ctx->bool_false;
}

static SmtTerm smt_const_bits(SmtCtx *ctx, unsigned width, const SmtBits *b) {
  SmtBits v = *b;
  smt_bits_mask(&v, width);
  return smt_node_make(ctx, SMT_K_CONST, smt_bv_sort(ctx, width), 0, 0, 0,
                       NULL, &v);
}

SmtTerm smt_bv_u64(SmtCtx *ctx, unsigned width, uint64_t value) {
  SmtBits b;
  smt_bits_from_u64(&b, value, SMT_MAX_WIDTH);
  return smt_const_bits(ctx, width, &b);
}

SmtTerm smt_bv_i64(SmtCtx *ctx, unsigned width, int64_t value) {
  SmtBits b;
  uint64_t fill = value < 0 ? ~(uint64_t)0 : 0;
  b.w[0] = (uint64_t)value;
  for (unsigned i = 1; i < SMT_WORDS; i++) {
    b.w[i] = fill;
  }
  return smt_const_bits(ctx, width, &b);
}

SmtTerm smt_bv_words(SmtCtx *ctx, unsigned width, const uint64_t *words) {
  SmtBits b;
  smt_bits_zero(&b);
  for (unsigned i = 0; i < SMT_WORDS && i * 64u < width; i++) {
    b.w[i] = words[i];
  }
  return smt_const_bits(ctx, width, &b);
}

SmtTerm smt_var(SmtCtx *ctx, const char *name, SmtSort sort) {
  uint32_t id = smt_intern(ctx, name);
  return smt_node_make(ctx, SMT_K_VAR, sort, id, 0, 0, NULL, NULL);
}

SmtTerm smt_fresh(SmtCtx *ctx, const char *prefix, SmtSort sort) {
  char buf[160];
  for (;;) {
    snprintf(buf, sizeof(buf), "%s!%u", prefix ? prefix : "k",
             ++ctx->fresh_counter);
    uint32_t h = smt_hash_string(buf);
    int taken = 0;
    if (ctx->name_bucket_cap) {
      uint32_t slot = h & (ctx->name_bucket_cap - 1u);
      while (ctx->name_buckets[slot]) {
        uint32_t id = ctx->name_buckets[slot] - 1u;
        if (ctx->name_hash[id] == h && strcmp(ctx->names[id], buf) == 0) {
          taken = 1;
          break;
        }
        slot = (slot + 1u) & (ctx->name_bucket_cap - 1u);
      }
    }
    if (!taken) {
      return smt_var(ctx, buf, sort);
    }
  }
}

SmtKind smt_term_kind(const SmtCtx *ctx, SmtTerm t) {
  return (SmtKind)ctx->nodes[t].kind;
}
SmtSort smt_term_sort(const SmtCtx *ctx, SmtTerm t) {
  return ctx->nodes[t].sort;
}
unsigned smt_term_nargs(const SmtCtx *ctx, SmtTerm t) {
  return ctx->nodes[t].nargs;
}
SmtTerm smt_term_arg(const SmtCtx *ctx, SmtTerm t, unsigned i) {
  return smt_arg(ctx, t, i);
}
uint32_t smt_term_param(const SmtCtx *ctx, SmtTerm t, unsigned i) {
  return i == 0 ? ctx->nodes[t].p0 : ctx->nodes[t].p1;
}
const char *smt_term_name(const SmtCtx *ctx, SmtTerm t) {
  if (ctx->nodes[t].kind == SMT_K_VAR) {
    return ctx->names[ctx->nodes[t].p0];
  }
  if (ctx->nodes[t].kind == SMT_K_APPLY) {
    return ctx->names[ctx->funcs[ctx->nodes[t].p0].name];
  }
  return NULL;
}
int smt_term_is_const(const SmtCtx *ctx, SmtTerm t) {
  SmtKind k = (SmtKind)ctx->nodes[t].kind;
  return k == SMT_K_CONST || k == SMT_K_TRUE || k == SMT_K_FALSE;
}
int smt_term_const_words(const SmtCtx *ctx, SmtTerm t, uint64_t *words) {
  const SmtNode *n = &ctx->nodes[t];
  if (n->kind == SMT_K_CONST) {
    memcpy(words, ctx->consts[n->p0].w, sizeof(uint64_t) * SMT_WORDS);
    return 1;
  }
  if (n->kind == SMT_K_TRUE || n->kind == SMT_K_FALSE) {
    memset(words, 0, sizeof(uint64_t) * SMT_WORDS);
    words[0] = n->kind == SMT_K_TRUE;
    return 1;
  }
  return 0;
}

SmtFunc smt_func(SmtCtx *ctx, const char *name, unsigned nargs,
                 const SmtSort *domain, SmtSort range) {
  uint32_t name_id = smt_intern(ctx, name);
  for (uint32_t i = 1; i < ctx->func_count; i++) {
    SmtFuncInfo *f = &ctx->funcs[i];
    if (f->name == name_id && f->arity == nargs && f->range == range &&
        memcmp(&ctx->func_domains[f->domain], domain,
               nargs * sizeof(uint32_t)) == 0) {
      return i;
    }
  }
  ctx->func_domains = smt_grow(ctx->func_domains, sizeof(uint32_t),
                               &ctx->func_domain_cap,
                               ctx->func_domain_count + nargs + 1u);
  ctx->funcs = smt_grow(ctx->funcs, sizeof(SmtFuncInfo), &ctx->func_cap,
                        ctx->func_count + 1u);
  SmtFuncInfo *f = &ctx->funcs[ctx->func_count];
  f->name = name_id;
  f->arity = nargs;
  f->range = range;
  f->domain = ctx->func_domain_count;
  if (nargs) {
    memcpy(&ctx->func_domains[ctx->func_domain_count], domain,
           nargs * sizeof(uint32_t));
  }
  ctx->func_domain_count += nargs;
  return ctx->func_count++;
}

SmtFunc smt_func_lookup(const SmtCtx *ctx, const char *name) {
  for (uint32_t i = ctx->func_count; i-- > 1;) {
    if (strcmp(ctx->names[ctx->funcs[i].name], name) == 0) {
      return i;
    }
  }
  return 0;
}

const char *smt_func_name(const SmtCtx *ctx, SmtFunc f) {
  return ctx->names[ctx->funcs[f].name];
}
unsigned smt_func_arity(const SmtCtx *ctx, SmtFunc f) {
  return ctx->funcs[f].arity;
}
SmtSort smt_func_range(const SmtCtx *ctx, SmtFunc f) {
  return ctx->funcs[f].range;
}
SmtSort smt_func_domain(const SmtCtx *ctx, SmtFunc f, unsigned i) {
  return ctx->func_domains[ctx->funcs[f].domain + i];
}

static int smt_is_true(const SmtCtx *ctx, SmtTerm t) {
  return t == ctx->bool_true;
}
static int smt_is_false(const SmtCtx *ctx, SmtTerm t) {
  return t == ctx->bool_false;
}
static int smt_is_bvconst(const SmtCtx *ctx, SmtTerm t) {
  return ctx->nodes[t].kind == SMT_K_CONST;
}
static const SmtBits *smt_bits_of(const SmtCtx *ctx, SmtTerm t) {
  return &ctx->consts[ctx->nodes[t].p0];
}
static int smt_const_is_zero(const SmtCtx *ctx, SmtTerm t) {
  return smt_is_bvconst(ctx, t) && smt_bits_is_zero(smt_bits_of(ctx, t));
}
static int smt_const_is_one(const SmtCtx *ctx, SmtTerm t) {
  if (!smt_is_bvconst(ctx, t)) {
    return 0;
  }
  SmtBits one;
  smt_bits_from_u64(&one, 1, smt_width(ctx, t));
  return smt_bits_eq(smt_bits_of(ctx, t), &one);
}
static int smt_const_is_ones(const SmtCtx *ctx, SmtTerm t) {
  if (!smt_is_bvconst(ctx, t)) {
    return 0;
  }
  SmtBits ones;
  smt_bits_ones(&ones, smt_width(ctx, t));
  return smt_bits_eq(smt_bits_of(ctx, t), &ones);
}

static int smt_is_not_of(const SmtCtx *ctx, SmtTerm a, SmtTerm b) {
  return (ctx->nodes[a].kind == SMT_K_NOT && smt_arg(ctx, a, 0) == b) ||
         (ctx->nodes[b].kind == SMT_K_NOT && smt_arg(ctx, b, 0) == a);
}

static int smt_kind_commutative(SmtKind k) {
  return k == SMT_K_AND || k == SMT_K_OR || k == SMT_K_XOR || k == SMT_K_EQ ||
         k == SMT_K_BVAND || k == SMT_K_BVOR || k == SMT_K_BVXOR ||
         k == SMT_K_BVADD || k == SMT_K_BVMUL;
}

static SmtSort smt_result_sort(SmtCtx *ctx, SmtKind kind, unsigned nargs,
                               const SmtTerm *args, uint32_t p0,
                               uint32_t p1) {
  for (unsigned i = 0; i < nargs; i++) {
    if (args[i] == 0 || args[i] >= ctx->node_count) {
      smt_fail(ctx, "invalid term argument");
      return 0;
    }
  }
  SmtSort s0 = nargs > 0 ? ctx->nodes[args[0]].sort : 0;
  SmtSort s1 = nargs > 1 ? ctx->nodes[args[1]].sort : 0;
  switch (kind) {
  case SMT_K_NOT:
    if (nargs != 1 || !smt_sort_is_bool(ctx, s0)) {
      break;
    }
    return ctx->sort_bool;
  case SMT_K_AND:
  case SMT_K_OR:
  case SMT_K_XOR:
    if (nargs != 2 || !smt_sort_is_bool(ctx, s0) ||
        !smt_sort_is_bool(ctx, s1)) {
      break;
    }
    return ctx->sort_bool;
  case SMT_K_EQ:
    if (nargs != 2 || s0 != s1) {
      break;
    }
    return ctx->sort_bool;
  case SMT_K_ITE:
    if (nargs != 3 || !smt_sort_is_bool(ctx, s0) ||
        s1 != ctx->nodes[args[2]].sort) {
      break;
    }
    return s1;
  case SMT_K_BVNOT:
  case SMT_K_BVNEG:
    if (nargs != 1 || !smt_sort_is_bv(ctx, s0)) {
      break;
    }
    return s0;
  case SMT_K_BVAND:
  case SMT_K_BVOR:
  case SMT_K_BVXOR:
  case SMT_K_BVADD:
  case SMT_K_BVSUB:
  case SMT_K_BVMUL:
  case SMT_K_BVUDIV:
  case SMT_K_BVUREM:
  case SMT_K_BVSDIV:
  case SMT_K_BVSREM:
  case SMT_K_BVSMOD:
  case SMT_K_BVSHL:
  case SMT_K_BVLSHR:
  case SMT_K_BVASHR:
    if (nargs != 2 || !smt_sort_is_bv(ctx, s0) || s0 != s1) {
      break;
    }
    return s0;
  case SMT_K_BVULT:
  case SMT_K_BVULE:
  case SMT_K_BVSLT:
  case SMT_K_BVSLE:
    if (nargs != 2 || !smt_sort_is_bv(ctx, s0) || s0 != s1) {
      break;
    }
    return ctx->sort_bool;
  case SMT_K_CONCAT:
    if (nargs != 2 || !smt_sort_is_bv(ctx, s0) || !smt_sort_is_bv(ctx, s1) ||
        ctx->sorts[s0].width + ctx->sorts[s1].width > SMT_MAX_WIDTH) {
      break;
    }
    return smt_bv_sort(ctx, ctx->sorts[s0].width + ctx->sorts[s1].width);
  case SMT_K_EXTRACT:
    if (nargs != 1 || !smt_sort_is_bv(ctx, s0) || p0 < p1 ||
        p0 >= ctx->sorts[s0].width) {
      break;
    }
    return smt_bv_sort(ctx, p0 - p1 + 1u);
  case SMT_K_ZEXT:
  case SMT_K_SEXT:
    if (nargs != 1 || !smt_sort_is_bv(ctx, s0) ||
        ctx->sorts[s0].width + p0 > SMT_MAX_WIDTH) {
      break;
    }
    return smt_bv_sort(ctx, ctx->sorts[s0].width + p0);
  case SMT_K_SELECT:
    if (nargs != 2 || !smt_sort_is_array(ctx, s0) ||
        ctx->sorts[s0].a != s1) {
      break;
    }
    return ctx->sorts[s0].b;
  case SMT_K_STORE:
    if (nargs != 3 || !smt_sort_is_array(ctx, s0) ||
        ctx->sorts[s0].a != s1 ||
        ctx->sorts[s0].b != ctx->nodes[args[2]].sort) {
      break;
    }
    return s0;
  case SMT_K_FORALL:
  case SMT_K_EXISTS:
    if (nargs != 2 || ctx->nodes[args[0]].kind != SMT_K_VAR ||
        !smt_sort_is_bool(ctx, s1)) {
      break;
    }
    return ctx->sort_bool;
  default:
    break;
  }
  smt_fail(ctx, "ill-sorted term");
  return 0;
}

static SmtTerm smt_fold(SmtCtx *ctx, SmtKind kind, unsigned nargs,
                        const SmtTerm *args, SmtSort sort, uint32_t p0,
                        uint32_t p1) {
  SmtBits a, b, r;
  smt_bits_zero(&b);
  if (!smt_term_const_words(ctx, args[0], a.w)) {
    return 0;
  }
  unsigned bwidth = smt_width(ctx, args[0]);
  if (nargs > 1) {
    if (!smt_term_const_words(ctx, args[1], b.w)) {
      return 0;
    }
    if (kind == SMT_K_CONCAT) {
      bwidth = smt_width(ctx, args[1]);
    }
  }
  if (!smt_bits_apply(kind, &r, &a, &b, ctx->sorts[sort].width, p0, p1,
                      bwidth)) {
    return 0;
  }
  if (smt_sort_is_bool(ctx, sort)) {
    return smt_bool(ctx, (int)(r.w[0] & 1u));
  }
  return smt_const_bits(ctx, ctx->sorts[sort].width, &r);
}

static SmtTerm smt_simplify_node(SmtCtx *ctx, SmtKind kind, unsigned nargs,
                                 SmtTerm *args, SmtSort sort, uint32_t p0,
                                 uint32_t p1) {
  SmtTerm a = nargs > 0 ? args[0] : 0;
  SmtTerm b = nargs > 1 ? args[1] : 0;
  switch (kind) {
  case SMT_K_NOT:
    if (smt_is_true(ctx, a)) {
      return ctx->bool_false;
    }
    if (smt_is_false(ctx, a)) {
      return ctx->bool_true;
    }
    if (ctx->nodes[a].kind == SMT_K_NOT) {
      return smt_arg(ctx, a, 0);
    }
    return 0;
  case SMT_K_AND:
    if (smt_is_false(ctx, a) || smt_is_false(ctx, b)) {
      return ctx->bool_false;
    }
    if (smt_is_true(ctx, a)) {
      return b;
    }
    if (smt_is_true(ctx, b) || a == b) {
      return a;
    }
    if (smt_is_not_of(ctx, a, b)) {
      return ctx->bool_false;
    }
    return 0;
  case SMT_K_OR:
    if (smt_is_true(ctx, a) || smt_is_true(ctx, b)) {
      return ctx->bool_true;
    }
    if (smt_is_false(ctx, a)) {
      return b;
    }
    if (smt_is_false(ctx, b) || a == b) {
      return a;
    }
    if (smt_is_not_of(ctx, a, b)) {
      return ctx->bool_true;
    }
    if (ctx->nodes[a].kind == SMT_K_AND && ctx->nodes[b].kind == SMT_K_AND) {
      SmtTerm a0 = smt_arg(ctx, a, 0), a1 = smt_arg(ctx, a, 1);
      SmtTerm b0 = smt_arg(ctx, b, 0), b1 = smt_arg(ctx, b, 1);
      if (a0 == b0 && smt_is_not_of(ctx, a1, b1)) {
        return a0;
      }
      if (a0 == b1 && smt_is_not_of(ctx, a1, b0)) {
        return a0;
      }
      if (a1 == b0 && smt_is_not_of(ctx, a0, b1)) {
        return a1;
      }
      if (a1 == b1 && smt_is_not_of(ctx, a0, b0)) {
        return a1;
      }
    }
    return 0;
  case SMT_K_XOR:
    if (a == b) {
      return ctx->bool_false;
    }
    if (smt_is_false(ctx, a)) {
      return b;
    }
    if (smt_is_false(ctx, b)) {
      return a;
    }
    if (smt_is_true(ctx, a)) {
      return smt_not(ctx, b);
    }
    if (smt_is_true(ctx, b)) {
      return smt_not(ctx, a);
    }
    if (smt_is_not_of(ctx, a, b)) {
      return ctx->bool_true;
    }
    return 0;
  case SMT_K_EQ:
    if (a == b) {
      return ctx->bool_true;
    }
    if (smt_term_is_const(ctx, a) && smt_term_is_const(ctx, b)) {
      return ctx->bool_false;
    }
    for (int side = 0; side < 2; side++) {
      SmtTerm it = side ? b : a, k = side ? a : b;
      if (ctx->nodes[it].kind != SMT_K_ITE || !smt_is_bvconst(ctx, k)) {
        continue;
      }
      SmtTerm t1 = smt_arg(ctx, it, 1), t2 = smt_arg(ctx, it, 2);
      if (!smt_is_bvconst(ctx, t1) || !smt_is_bvconst(ctx, t2)) {
        continue;
      }
      int hit1 = t1 == k, hit2 = t2 == k;
      SmtTerm c = smt_arg(ctx, it, 0);
      if (hit1 && hit2) {
        return ctx->bool_true;
      }
      if (hit1) {
        return c;
      }
      if (hit2) {
        return smt_not(ctx, c);
      }
      return ctx->bool_false;
    }
    if (smt_sort_is_bool(ctx, ctx->nodes[a].sort)) {
      if (smt_is_true(ctx, a)) {
        return b;
      }
      if (smt_is_true(ctx, b)) {
        return a;
      }
      if (smt_is_false(ctx, a)) {
        return smt_not(ctx, b);
      }
      if (smt_is_false(ctx, b)) {
        return smt_not(ctx, a);
      }
      if (smt_is_not_of(ctx, a, b)) {
        return ctx->bool_false;
      }
    }
    return 0;
  case SMT_K_ITE: {
    SmtTerm c = args[0], x = args[1], y = args[2];
    if (smt_is_true(ctx, c) || x == y) {
      return x;
    }
    if (smt_is_false(ctx, c)) {
      return y;
    }
    if (ctx->nodes[c].kind == SMT_K_NOT) {
      return smt_ite(ctx, smt_arg(ctx, c, 0), y, x);
    }
    if (smt_sort_is_bool(ctx, sort)) {
      if (smt_is_true(ctx, x) && smt_is_false(ctx, y)) {
        return c;
      }
      if (smt_is_false(ctx, x) && smt_is_true(ctx, y)) {
        return smt_not(ctx, c);
      }
      if (smt_is_true(ctx, x)) {
        return smt_or(ctx, c, y);
      }
      if (smt_is_false(ctx, y)) {
        return smt_and(ctx, c, x);
      }
      if (smt_is_false(ctx, x)) {
        return smt_and(ctx, smt_not(ctx, c), y);
      }
      if (smt_is_true(ctx, y)) {
        return smt_or(ctx, smt_not(ctx, c), x);
      }
    }
    if (ctx->nodes[x].kind == SMT_K_ITE && smt_arg(ctx, x, 0) == c) {
      return smt_ite(ctx, c, smt_arg(ctx, x, 1), y);
    }
    if (ctx->nodes[y].kind == SMT_K_ITE && smt_arg(ctx, y, 0) == c) {
      return smt_ite(ctx, c, x, smt_arg(ctx, y, 2));
    }
    return 0;
  }
  case SMT_K_BVNOT:
    if (ctx->nodes[a].kind == SMT_K_BVNOT) {
      return smt_arg(ctx, a, 0);
    }
    return 0;
  case SMT_K_BVNEG:
    if (ctx->nodes[a].kind == SMT_K_BVNEG) {
      return smt_arg(ctx, a, 0);
    }
    return 0;
  case SMT_K_BVAND:
    if (smt_const_is_zero(ctx, a) || smt_const_is_ones(ctx, b) || a == b) {
      return a;
    }
    if (smt_const_is_zero(ctx, b) || smt_const_is_ones(ctx, a)) {
      return b;
    }
    return 0;
  case SMT_K_BVOR:
    if (smt_const_is_ones(ctx, a) || smt_const_is_zero(ctx, b) || a == b) {
      return a;
    }
    if (smt_const_is_ones(ctx, b) || smt_const_is_zero(ctx, a)) {
      return b;
    }
    return 0;
  case SMT_K_BVXOR:
    if (a == b) {
      return smt_bv_u64(ctx, ctx->sorts[sort].width, 0);
    }
    if (smt_const_is_zero(ctx, a)) {
      return b;
    }
    if (smt_const_is_zero(ctx, b)) {
      return a;
    }
    return 0;
  case SMT_K_BVADD:
    if (smt_const_is_zero(ctx, a)) {
      return b;
    }
    if (smt_const_is_zero(ctx, b)) {
      return a;
    }
    return 0;
  case SMT_K_BVSUB:
    if (a == b) {
      return smt_bv_u64(ctx, ctx->sorts[sort].width, 0);
    }
    if (smt_const_is_zero(ctx, b)) {
      return a;
    }
    return 0;
  case SMT_K_BVMUL:
    if (smt_const_is_zero(ctx, a) || smt_const_is_one(ctx, b)) {
      return a;
    }
    if (smt_const_is_zero(ctx, b) || smt_const_is_one(ctx, a)) {
      return b;
    }
    return 0;
  case SMT_K_BVUDIV:
  case SMT_K_BVUREM: {
    unsigned w = ctx->sorts[sort].width;
    if (!smt_is_bvconst(ctx, b)) {
      return 0;
    }
    const SmtBits *c = smt_bits_of(ctx, b);
    unsigned ones = 0, at = 0;
    for (unsigned i = 0; i < w; i++) {
      if (smt_bits_get(c, i)) {
        ones++;
        at = i;
      }
    }
    if (ones != 1u) {
      return 0;
    }
    if (kind == SMT_K_BVUDIV) {
      return at == 0 ? a : smt_mk2(ctx, SMT_K_BVLSHR, a, smt_bv_u64(ctx, w, at));
    }
    if (at == 0) {
      return smt_bv_u64(ctx, w, 0);
    }
    SmtBits mask;
    smt_bits_sub(&mask, c, &(SmtBits){{1, 0, 0, 0}}, w);
    return smt_mk2(ctx, SMT_K_BVAND, a, smt_bv_words(ctx, w, mask.w));
  }
  case SMT_K_BVSDIV:
  case SMT_K_BVSREM: {
    unsigned w = ctx->sorts[sort].width;
    if (!smt_is_bvconst(ctx, b)) {
      return 0;
    }
    const SmtBits *c = smt_bits_of(ctx, b);
    int negative = smt_bits_get(c, w - 1u);
    SmtBits mag = *c;
    if (negative) {
      smt_bits_neg(&mag, c, w);
    }
    SmtTerm m = smt_bv_words(ctx, w, mag.w);
    SmtTerm zero = smt_bv_u64(ctx, w, 0);
    SmtTerm a_neg = smt_mk2(ctx, SMT_K_BVSLT, a, zero);
    SmtTerm abs_a = smt_mk1(ctx, SMT_K_BVNEG, a);
    if (kind == SMT_K_BVSREM) {
      SmtTerm on_neg = smt_mk1(ctx, SMT_K_BVNEG,
                               smt_mk2(ctx, SMT_K_BVUREM, abs_a, m));
      return smt_ite(ctx, a_neg, on_neg, smt_mk2(ctx, SMT_K_BVUREM, a, m));
    }
    SmtTerm q_neg = smt_mk2(ctx, SMT_K_BVUDIV, abs_a, m);
    SmtTerm q_pos = smt_mk2(ctx, SMT_K_BVUDIV, a, m);
    if (negative) {
      return smt_ite(ctx, a_neg, q_neg, smt_mk1(ctx, SMT_K_BVNEG, q_pos));
    }
    return smt_ite(ctx, a_neg, smt_mk1(ctx, SMT_K_BVNEG, q_neg), q_pos);
  }
  case SMT_K_BVSHL:
  case SMT_K_BVLSHR:
  case SMT_K_BVASHR:
    if (smt_const_is_zero(ctx, b)) {
      return a;
    }
    return 0;
  case SMT_K_BVULT:
    if (a == b || smt_const_is_zero(ctx, b) || smt_const_is_ones(ctx, a)) {
      return ctx->bool_false;
    }
    return 0;
  case SMT_K_BVULE:
    if (a == b || smt_const_is_zero(ctx, a) || smt_const_is_ones(ctx, b)) {
      return ctx->bool_true;
    }
    return 0;
  case SMT_K_BVSLT:
    if (a == b) {
      return ctx->bool_false;
    }
    return 0;
  case SMT_K_BVSLE:
    if (a == b) {
      return ctx->bool_true;
    }
    return 0;
  case SMT_K_EXTRACT:
    if (p1 == 0 && p0 + 1u == smt_width(ctx, a)) {
      return a;
    }
    if (ctx->nodes[a].kind == SMT_K_EXTRACT) {
      uint32_t base = ctx->nodes[a].p1;
      return smt_extract(ctx, smt_arg(ctx, a, 0), p0 + base, p1 + base);
    }
    if (ctx->nodes[a].kind == SMT_K_ZEXT || ctx->nodes[a].kind == SMT_K_SEXT) {
      SmtTerm inner = smt_arg(ctx, a, 0);
      if (p0 < smt_width(ctx, inner)) {
        return smt_extract(ctx, inner, p0, p1);
      }
    }
    if (ctx->nodes[a].kind == SMT_K_CONCAT) {
      SmtTerm hi = smt_arg(ctx, a, 0), lo = smt_arg(ctx, a, 1);
      unsigned lw = smt_width(ctx, lo);
      if (p0 < lw) {
        return smt_extract(ctx, lo, p0, p1);
      }
      if (p1 >= lw) {
        return smt_extract(ctx, hi, p0 - lw, p1 - lw);
      }
    }
    return 0;
  case SMT_K_ZEXT:
  case SMT_K_SEXT:
    if (p0 == 0) {
      return a;
    }
    return 0;
  case SMT_K_SELECT:
    if (ctx->nodes[a].kind == SMT_K_STORE) {
      SmtTerm idx = smt_arg(ctx, a, 1);
      if (idx == b) {
        return smt_arg(ctx, a, 2);
      }
      if (smt_is_bvconst(ctx, idx) && smt_is_bvconst(ctx, b)) {
        return smt_select(ctx, smt_arg(ctx, a, 0), b);
      }
    }
    return 0;
  default:
    return 0;
  }
}

static SmtTerm smt_mk_param(SmtCtx *ctx, SmtKind kind, unsigned nargs,
                            const SmtTerm *in, uint32_t p0, uint32_t p1) {
  SmtTerm args[4];
  if (nargs > 4) {
    smt_fail(ctx, "too many arguments");
    return 0;
  }
  for (unsigned i = 0; i < nargs; i++) {
    args[i] = in[i];
  }
  SmtSort sort = smt_result_sort(ctx, kind, nargs, args, p0, p1);
  if (!sort) {
    return ctx->bool_false;
  }
  if (ctx->simplify) {
    int all_const = kind != SMT_K_SELECT && kind != SMT_K_STORE &&
                    kind != SMT_K_ITE && kind != SMT_K_EQ &&
                    kind != SMT_K_FORALL && kind != SMT_K_EXISTS;
    for (unsigned i = 0; i < nargs && all_const; i++) {
      all_const = smt_term_is_const(ctx, args[i]);
    }
    if (all_const && nargs) {
      SmtTerm folded = smt_fold(ctx, kind, nargs, args, sort, p0, p1);
      if (folded) {
        return folded;
      }
    }
    if (kind == SMT_K_EQ && smt_is_bvconst(ctx, args[0]) &&
        smt_is_bvconst(ctx, args[1])) {
      return smt_bool(ctx, args[0] == args[1]);
    }
    if (smt_kind_commutative(kind) && nargs == 2 && args[0] > args[1]) {
      SmtTerm t = args[0];
      args[0] = args[1];
      args[1] = t;
    }
    SmtTerm s = smt_simplify_node(ctx, kind, nargs, args, sort, p0, p1);
    if (s) {
      return s;
    }
  }
  return smt_node_make(ctx, kind, sort, p0, p1, nargs, args, NULL);
}

SmtTerm smt_mk(SmtCtx *ctx, SmtKind kind, unsigned nargs,
               const SmtTerm *args) {
  return smt_mk_param(ctx, kind, nargs, args, 0, 0);
}
SmtTerm smt_mk1(SmtCtx *ctx, SmtKind kind, SmtTerm a) {
  return smt_mk_param(ctx, kind, 1, &a, 0, 0);
}
SmtTerm smt_mk2(SmtCtx *ctx, SmtKind kind, SmtTerm a, SmtTerm b) {
  SmtTerm args[2] = {a, b};
  return smt_mk_param(ctx, kind, 2, args, 0, 0);
}
SmtTerm smt_not(SmtCtx *ctx, SmtTerm a) { return smt_mk1(ctx, SMT_K_NOT, a); }
SmtTerm smt_and(SmtCtx *ctx, SmtTerm a, SmtTerm b) {
  return smt_mk2(ctx, SMT_K_AND, a, b);
}
SmtTerm smt_or(SmtCtx *ctx, SmtTerm a, SmtTerm b) {
  return smt_mk2(ctx, SMT_K_OR, a, b);
}
SmtTerm smt_xor(SmtCtx *ctx, SmtTerm a, SmtTerm b) {
  return smt_mk2(ctx, SMT_K_XOR, a, b);
}
SmtTerm smt_implies(SmtCtx *ctx, SmtTerm a, SmtTerm b) {
  return smt_or(ctx, smt_not(ctx, a), b);
}
SmtTerm smt_iff(SmtCtx *ctx, SmtTerm a, SmtTerm b) {
  return smt_mk2(ctx, SMT_K_EQ, a, b);
}
SmtTerm smt_eq(SmtCtx *ctx, SmtTerm a, SmtTerm b) {
  return smt_mk2(ctx, SMT_K_EQ, a, b);
}
SmtTerm smt_ite(SmtCtx *ctx, SmtTerm c, SmtTerm a, SmtTerm b) {
  SmtTerm args[3] = {c, a, b};
  return smt_mk_param(ctx, SMT_K_ITE, 3, args, 0, 0);
}
SmtTerm smt_extract(SmtCtx *ctx, SmtTerm a, unsigned hi, unsigned lo) {
  return smt_mk_param(ctx, SMT_K_EXTRACT, 1, &a, hi, lo);
}
SmtTerm smt_zext(SmtCtx *ctx, SmtTerm a, unsigned extra) {
  return smt_mk_param(ctx, SMT_K_ZEXT, 1, &a, extra, 0);
}
SmtTerm smt_sext(SmtCtx *ctx, SmtTerm a, unsigned extra) {
  return smt_mk_param(ctx, SMT_K_SEXT, 1, &a, extra, 0);
}
SmtTerm smt_resize(SmtCtx *ctx, SmtTerm a, unsigned width, int is_signed) {
  unsigned w = smt_width(ctx, a);
  if (w == width) {
    return a;
  }
  if (w > width) {
    return smt_extract(ctx, a, width - 1u, 0);
  }
  return is_signed ? smt_sext(ctx, a, width - w) : smt_zext(ctx, a, width - w);
}
SmtTerm smt_select(SmtCtx *ctx, SmtTerm array, SmtTerm index) {
  return smt_mk2(ctx, SMT_K_SELECT, array, index);
}
SmtTerm smt_store(SmtCtx *ctx, SmtTerm array, SmtTerm index, SmtTerm value) {
  SmtTerm args[3] = {array, index, value};
  return smt_mk_param(ctx, SMT_K_STORE, 3, args, 0, 0);
}
SmtTerm smt_forall(SmtCtx *ctx, SmtTerm var, SmtTerm body) {
  if (!smt_term_contains(ctx, body, var)) {
    return body;
  }
  return smt_mk2(ctx, SMT_K_FORALL, var, body);
}
SmtTerm smt_exists(SmtCtx *ctx, SmtTerm var, SmtTerm body) {
  if (!smt_term_contains(ctx, body, var)) {
    return body;
  }
  return smt_mk2(ctx, SMT_K_EXISTS, var, body);
}

SmtTerm smt_apply(SmtCtx *ctx, SmtFunc func, unsigned nargs,
                  const SmtTerm *args) {
  if (func == 0 || func >= ctx->func_count ||
      ctx->funcs[func].arity != nargs) {
    smt_fail(ctx, "bad function application");
    return ctx->bool_false;
  }
  for (unsigned i = 0; i < nargs; i++) {
    if (ctx->nodes[args[i]].sort != smt_func_domain(ctx, func, i)) {
      smt_fail(ctx, "ill-sorted function argument");
      return ctx->bool_false;
    }
  }
  return smt_node_make(ctx, SMT_K_APPLY, ctx->funcs[func].range, func, 0,
                       nargs, args, NULL);
}

void smt_map_init(SmtMap *m) {
  m->cap = 256;
  m->count = 0;
  m->keys = calloc(m->cap, sizeof(SmtTerm));
  m->vals = calloc(m->cap, sizeof(SmtTerm));
}

void smt_map_free(SmtMap *m) {
  free(m->keys);
  free(m->vals);
}

SmtTerm smt_map_get(const SmtMap *m, SmtTerm k) {
  uint32_t slot = (k * 2654435761u) & (m->cap - 1u);
  while (m->keys[slot]) {
    if (m->keys[slot] == k) {
      return m->vals[slot];
    }
    slot = (slot + 1u) & (m->cap - 1u);
  }
  return 0;
}

void smt_map_put(SmtMap *m, SmtTerm k, SmtTerm v) {
  if ((m->count + 1u) * 2u > m->cap) {
    SmtMap n;
    n.cap = m->cap * 2u;
    n.count = 0;
    n.keys = calloc(n.cap, sizeof(SmtTerm));
    n.vals = calloc(n.cap, sizeof(SmtTerm));
    for (uint32_t i = 0; i < m->cap; i++) {
      if (m->keys[i]) {
        smt_map_put(&n, m->keys[i], m->vals[i]);
      }
    }
    smt_map_free(m);
    *m = n;
  }
  uint32_t slot = (k * 2654435761u) & (m->cap - 1u);
  while (m->keys[slot] && m->keys[slot] != k) {
    slot = (slot + 1u) & (m->cap - 1u);
  }
  if (!m->keys[slot]) {
    m->count++;
  }
  m->keys[slot] = k;
  m->vals[slot] = v;
}

static int smt_contains_rec(SmtCtx *ctx, SmtTerm t, SmtTerm sub,
                            uint32_t stamp) {
  if (t == sub) {
    return 1;
  }
  if (smt_mark_test_set(ctx, t, stamp)) {
    return 0;
  }
  unsigned n = ctx->nodes[t].nargs;
  for (unsigned i = 0; i < n; i++) {
    if (smt_contains_rec(ctx, smt_arg(ctx, t, i), sub, stamp)) {
      return 1;
    }
  }
  return 0;
}

int smt_term_contains(SmtCtx *ctx, SmtTerm t, SmtTerm sub) {
  uint32_t stamp = smt_mark_begin(ctx);
  return smt_contains_rec(ctx, t, sub, stamp);
}

static int smt_quant_rec(SmtCtx *ctx, SmtTerm t, uint32_t stamp) {
  if (smt_mark_test_set(ctx, t, stamp)) {
    return 0;
  }
  SmtKind k = (SmtKind)ctx->nodes[t].kind;
  if (k == SMT_K_FORALL || k == SMT_K_EXISTS) {
    return 1;
  }
  unsigned n = ctx->nodes[t].nargs;
  for (unsigned i = 0; i < n; i++) {
    if (smt_quant_rec(ctx, smt_arg(ctx, t, i), stamp)) {
      return 1;
    }
  }
  return 0;
}

int smt_term_has_quantifier(SmtCtx *ctx, SmtTerm t) {
  uint32_t stamp = smt_mark_begin(ctx);
  return smt_quant_rec(ctx, t, stamp);
}

typedef struct {
  SmtTerm *out;
  unsigned count;
  unsigned capacity;
  SmtTerm bound[64];
  unsigned bound_count;
  uint32_t stamp;
} SmtFreeVars;

static void smt_free_vars_rec(SmtCtx *ctx, SmtTerm t, SmtFreeVars *fv) {
  const SmtNode *n = &ctx->nodes[t];
  if (fv->bound_count == 0 && smt_mark_test_set(ctx, t, fv->stamp)) {
    return;
  }
  if (n->kind == SMT_K_VAR) {
    for (unsigned i = 0; i < fv->bound_count; i++) {
      if (fv->bound[i] == t) {
        return;
      }
    }
    for (unsigned i = 0; i < fv->count && i < fv->capacity; i++) {
      if (fv->out[i] == t) {
        return;
      }
    }
    if (fv->count < fv->capacity) {
      fv->out[fv->count] = t;
    }
    fv->count++;
    return;
  }
  if (n->kind == SMT_K_FORALL || n->kind == SMT_K_EXISTS) {
    if (fv->bound_count < 64u) {
      fv->bound[fv->bound_count++] = smt_arg(ctx, t, 0);
      smt_free_vars_rec(ctx, smt_arg(ctx, t, 1), fv);
      fv->bound_count--;
    }
    return;
  }
  for (unsigned i = 0; i < n->nargs; i++) {
    smt_free_vars_rec(ctx, smt_arg(ctx, t, i), fv);
  }
}

unsigned smt_term_free_vars(SmtCtx *ctx, SmtTerm t, SmtTerm *out,
                            unsigned capacity) {
  SmtFreeVars fv;
  fv.out = out;
  fv.count = 0;
  fv.capacity = capacity;
  fv.bound_count = 0;
  fv.stamp = smt_mark_begin(ctx);
  smt_free_vars_rec(ctx, t, &fv);
  return fv.count;
}

typedef struct {
  SmtCtx *ctx;
  unsigned n;
  const SmtTerm *vars;
  const SmtTerm *values;
  SmtMap memo;
} SmtSubst;

static SmtTerm smt_rebuild(SmtCtx *ctx, SmtTerm t, const SmtTerm *args) {
  const SmtNode *n = &ctx->nodes[t];
  SmtKind kind = (SmtKind)n->kind;
  if (kind == SMT_K_APPLY) {
    return smt_apply(ctx, n->p0, n->nargs, args);
  }
  if (kind == SMT_K_FORALL) {
    return smt_forall(ctx, args[0], args[1]);
  }
  if (kind == SMT_K_EXISTS) {
    return smt_exists(ctx, args[0], args[1]);
  }
  return smt_mk_param(ctx, kind, n->nargs, args, n->p0, n->p1);
}

static SmtTerm smt_subst_rec(SmtSubst *s, SmtTerm t) {
  SmtCtx *ctx = s->ctx;
  const SmtNode *n = &ctx->nodes[t];
  if (n->kind == SMT_K_VAR) {
    for (unsigned i = 0; i < s->n; i++) {
      if (s->vars[i] == t) {
        return s->values[i];
      }
    }
    return t;
  }
  if (n->nargs == 0) {
    return t;
  }
  SmtTerm cached = smt_map_get(&s->memo, t);
  if (cached) {
    return cached;
  }
  SmtTerm out;
  if (n->kind == SMT_K_FORALL || n->kind == SMT_K_EXISTS) {
    SmtTerm var = smt_arg(ctx, t, 0);
    SmtTerm body = smt_arg(ctx, t, 1);
    SmtTerm vars[64], values[64];
    unsigned m = 0;
    int capture = 0;
    for (unsigned i = 0; i < s->n && m < 64u; i++) {
      if (s->vars[i] == var) {
        continue;
      }
      if (!smt_term_contains(ctx, body, s->vars[i])) {
        continue;
      }
      vars[m] = s->vars[i];
      values[m] = s->values[i];
      if (smt_term_contains(ctx, s->values[i], var)) {
        capture = 1;
      }
      m++;
    }
    if (m == 0) {
      out = t;
    } else {
      SmtTerm bound = var;
      if (capture) {
        bound = smt_fresh(ctx, smt_term_name(ctx, var),
                          ctx->nodes[var].sort);
        vars[m] = var;
        values[m] = bound;
        m++;
      }
      int is_forall = ctx->nodes[t].kind == SMT_K_FORALL;
      SmtTerm nb = smt_subst(ctx, body, m, vars, values);
      out = is_forall ? smt_forall(ctx, bound, nb)
                      : smt_exists(ctx, bound, nb);
    }
  } else {
    SmtTerm args[64];
    unsigned na = n->nargs;
    int changed = 0;
    SmtTerm *buf = na <= 64u ? args : malloc(na * sizeof(SmtTerm));
    for (unsigned i = 0; i < na; i++) {
      SmtTerm a = smt_arg(ctx, t, i);
      buf[i] = smt_subst_rec(s, a);
      changed |= buf[i] != a;
    }
    out = changed ? smt_rebuild(ctx, t, buf) : t;
    if (buf != args) {
      free(buf);
    }
  }
  smt_map_put(&s->memo, t, out);
  return out;
}

typedef struct {
  SmtCtx *ctx;
  SmtMap pairs;
  SmtMap memo;
} SmtReplace;

static SmtTerm smt_replace_rec(SmtReplace *r, SmtTerm t) {
  SmtCtx *ctx = r->ctx;
  SmtTerm hit = smt_map_get(&r->pairs, t);
  if (hit) {
    return hit;
  }
  unsigned na = ctx->nodes[t].nargs;
  if (na == 0) {
    return t;
  }
  SmtTerm cached = smt_map_get(&r->memo, t);
  if (cached) {
    return cached;
  }
  SmtTerm args[64];
  SmtTerm *buf = na <= 64u ? args : malloc(na * sizeof(SmtTerm));
  int changed = 0;
  for (unsigned i = 0; i < na; i++) {
    SmtTerm a = smt_arg(ctx, t, i);
    buf[i] = smt_replace_rec(r, a);
    changed |= buf[i] != a;
  }
  SmtTerm out = changed ? smt_rebuild(ctx, t, buf) : t;
  if (buf != args) {
    free(buf);
  }
  smt_map_put(&r->memo, t, out);
  return out;
}

SmtTerm smt_replace(SmtCtx *ctx, SmtTerm t, unsigned n, const SmtTerm *from,
                    const SmtTerm *to) {
  SmtReplace r;
  r.ctx = ctx;
  smt_map_init(&r.pairs);
  smt_map_init(&r.memo);
  for (unsigned i = 0; i < n; i++) {
    smt_map_put(&r.pairs, from[i], to[i]);
  }
  SmtTerm out = smt_replace_rec(&r, t);
  smt_map_free(&r.pairs);
  smt_map_free(&r.memo);
  return out;
}

SmtTerm smt_term_rebuild(SmtCtx *ctx, SmtTerm t, const SmtTerm *args) {
  return smt_rebuild(ctx, t, args);
}

SmtTerm smt_subst(SmtCtx *ctx, SmtTerm t, unsigned n, const SmtTerm *vars,
                  const SmtTerm *values) {
  SmtSubst s;
  s.ctx = ctx;
  s.n = n;
  s.vars = vars;
  s.values = values;
  smt_map_init(&s.memo);
  SmtTerm out = smt_subst_rec(&s, t);
  smt_map_free(&s.memo);
  return out;
}

static const char *smt_kind_name(SmtKind k) {
  switch (k) {
  case SMT_K_NOT:
    return "not";
  case SMT_K_AND:
    return "and";
  case SMT_K_OR:
    return "or";
  case SMT_K_XOR:
    return "xor";
  case SMT_K_ITE:
    return "ite";
  case SMT_K_EQ:
    return "=";
  case SMT_K_BVNOT:
    return "bvnot";
  case SMT_K_BVNEG:
    return "bvneg";
  case SMT_K_BVAND:
    return "bvand";
  case SMT_K_BVOR:
    return "bvor";
  case SMT_K_BVXOR:
    return "bvxor";
  case SMT_K_BVADD:
    return "bvadd";
  case SMT_K_BVSUB:
    return "bvsub";
  case SMT_K_BVMUL:
    return "bvmul";
  case SMT_K_BVUDIV:
    return "bvudiv";
  case SMT_K_BVUREM:
    return "bvurem";
  case SMT_K_BVSDIV:
    return "bvsdiv";
  case SMT_K_BVSREM:
    return "bvsrem";
  case SMT_K_BVSMOD:
    return "bvsmod";
  case SMT_K_BVSHL:
    return "bvshl";
  case SMT_K_BVLSHR:
    return "bvlshr";
  case SMT_K_BVASHR:
    return "bvashr";
  case SMT_K_CONCAT:
    return "concat";
  case SMT_K_BVULT:
    return "bvult";
  case SMT_K_BVULE:
    return "bvule";
  case SMT_K_BVSLT:
    return "bvslt";
  case SMT_K_BVSLE:
    return "bvsle";
  case SMT_K_SELECT:
    return "select";
  case SMT_K_STORE:
    return "store";
  case SMT_K_FORALL:
    return "forall";
  case SMT_K_EXISTS:
    return "exists";
  default:
    return "?";
  }
}

typedef struct {
  FILE *file;
  char *buf;
  size_t cap;
  size_t len;
  size_t budget;
} SmtOut;

static void smt_out_puts(SmtOut *o, const char *s) {
  size_t n = strlen(s);
  if (o->file) {
    fputs(s, o->file);
  } else if (o->cap) {
    for (size_t i = 0; i < n && o->len + 1u < o->cap; i++) {
      o->buf[o->len++] = s[i];
    }
    o->buf[o->len] = 0;
  }
  if (o->budget > n) {
    o->budget -= n;
  } else {
    o->budget = 0;
  }
}

static void smt_out_printf(SmtOut *o, const char *fmt, ...) {
  char tmp[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);
  smt_out_puts(o, tmp);
}

static void smt_out_bits(SmtOut *o, const SmtBits *b, unsigned width) {
  char tmp[SMT_MAX_WIDTH + 4u];
  size_t k = 0;
  if (width % 4u == 0) {
    tmp[k++] = '#';
    tmp[k++] = 'x';
    for (int i = (int)width / 4 - 1; i >= 0; i--) {
      unsigned nib = (unsigned)((b->w[(unsigned)i * 4u / 64u] >>
                                 (((unsigned)i * 4u) & 63u)) &
                                15u);
      tmp[k++] = "0123456789abcdef"[nib];
    }
  } else {
    tmp[k++] = '#';
    tmp[k++] = 'b';
    for (int i = (int)width - 1; i >= 0; i--) {
      tmp[k++] = smt_bits_get(b, (unsigned)i) ? '1' : '0';
    }
  }
  tmp[k] = 0;
  smt_out_puts(o, tmp);
}

static void smt_sort_out(const SmtCtx *ctx, SmtSort s, SmtOut *o) {
  const SmtSortInfo *info = &ctx->sorts[s];
  if (info->kind == SMT_SORT_BOOL) {
    smt_out_puts(o, "Bool");
  } else if (info->kind == SMT_SORT_BV) {
    smt_out_printf(o, "(_ BitVec %u)", info->width);
  } else if (info->kind == SMT_SORT_ARRAY) {
    smt_out_puts(o, "(Array ");
    smt_sort_out(ctx, info->a, o);
    smt_out_puts(o, " ");
    smt_sort_out(ctx, info->b, o);
    smt_out_puts(o, ")");
  }
}

void smt_sort_print(const SmtCtx *ctx, SmtSort s, FILE *out) {
  SmtOut o;
  memset(&o, 0, sizeof(o));
  o.file = out;
  o.budget = (size_t)-1;
  smt_sort_out(ctx, s, &o);
}

static void smt_print_rec(const SmtCtx *ctx, SmtTerm t, SmtOut *out,
                          unsigned depth) {
  const SmtNode *n = &ctx->nodes[t];
  SmtKind k = (SmtKind)n->kind;
  if (depth > 400u || out->budget == 0) {
    smt_out_puts(out, "...");
    return;
  }
  switch (k) {
  case SMT_K_TRUE:
    smt_out_puts(out, "true");
    return;
  case SMT_K_FALSE:
    smt_out_puts(out, "false");
    return;
  case SMT_K_CONST:
    smt_out_bits(out, &ctx->consts[n->p0], ctx->sorts[n->sort].width);
    return;
  case SMT_K_VAR:
    smt_out_puts(out, ctx->names[n->p0]);
    return;
  case SMT_K_EXTRACT:
    smt_out_printf(out, "((_ extract %u %u) ", n->p0, n->p1);
    smt_print_rec(ctx, smt_arg(ctx, t, 0), out, depth + 1u);
    smt_out_puts(out, ")");
    return;
  case SMT_K_ZEXT:
  case SMT_K_SEXT:
    smt_out_printf(out, "((_ %s %u) ",
                   k == SMT_K_ZEXT ? "zero_extend" : "sign_extend", n->p0);
    smt_print_rec(ctx, smt_arg(ctx, t, 0), out, depth + 1u);
    smt_out_puts(out, ")");
    return;
  case SMT_K_FORALL:
  case SMT_K_EXISTS: {
    SmtTerm v = smt_arg(ctx, t, 0);
    smt_out_printf(out, "(%s ((%s ", smt_kind_name(k),
                   ctx->names[ctx->nodes[v].p0]);
    smt_sort_out(ctx, ctx->nodes[v].sort, out);
    smt_out_puts(out, ")) ");
    smt_print_rec(ctx, smt_arg(ctx, t, 1), out, depth + 1u);
    smt_out_puts(out, ")");
    return;
  }
  case SMT_K_APPLY:
    if (n->nargs == 0) {
      smt_out_puts(out, ctx->names[ctx->funcs[n->p0].name]);
      return;
    }
    smt_out_printf(out, "(%s", ctx->names[ctx->funcs[n->p0].name]);
    break;
  default:
    smt_out_printf(out, "(%s", smt_kind_name(k));
    break;
  }
  for (unsigned i = 0; i < n->nargs; i++) {
    smt_out_puts(out, " ");
    smt_print_rec(ctx, smt_arg(ctx, t, i), out, depth + 1u);
  }
  smt_out_puts(out, ")");
}

void smt_term_print(const SmtCtx *ctx, SmtTerm t, FILE *out) {
  SmtOut o;
  memset(&o, 0, sizeof(o));
  o.file = out;
  o.budget = (size_t)1 << 24;
  smt_print_rec(ctx, t, &o, 0);
}

size_t smt_term_to_string(const SmtCtx *ctx, SmtTerm t, char *buf,
                          size_t capacity) {
  SmtOut o;
  memset(&o, 0, sizeof(o));
  o.buf = buf;
  o.cap = capacity;
  o.budget = capacity ? capacity : 1u;
  if (capacity) {
    buf[0] = 0;
  }
  smt_print_rec(ctx, t, &o, 0);
  return o.len;
}

void smt_model_init(SmtModel *m) { memset(m, 0, sizeof(*m)); }

void smt_model_free(SmtModel *m) {
  free(m->values);
  free(m->has);
  for (uint32_t i = 0; i < m->array_count; i++) {
    free(m->arrays[i].entries);
  }
  free(m->arrays);
  for (uint32_t i = 0; i < m->func_count; i++) {
    for (uint32_t j = 0; j < m->funcs[i].count; j++) {
      free(m->funcs[i].entries[j].args);
    }
    free(m->funcs[i].entries);
  }
  free(m->funcs);
  memset(m, 0, sizeof(*m));
}

void smt_model_set(SmtModel *m, SmtTerm var, const SmtBits *value) {
  if (var >= m->cap) {
    uint32_t old = m->cap;
    uint32_t cap = old ? old : 256u;
    while (cap <= var) {
      cap *= 2u;
    }
    m->values = realloc(m->values, cap * sizeof(SmtBits));
    m->has = realloc(m->has, cap);
    memset(m->has + old, 0, cap - old);
    m->cap = cap;
  }
  m->values[var] = *value;
  m->has[var] = 1;
}

SmtArrayModel *smt_model_array(SmtModel *m, SmtTerm var) {
  for (uint32_t i = 0; i < m->array_count; i++) {
    if (m->arrays[i].term == var) {
      return &m->arrays[i];
    }
  }
  m->arrays = smt_grow(m->arrays, sizeof(SmtArrayModel), &m->array_cap,
                       m->array_count + 1u);
  SmtArrayModel *a = &m->arrays[m->array_count++];
  memset(a, 0, sizeof(*a));
  a->term = var;
  return a;
}

SmtFuncModel *smt_model_func(SmtModel *m, SmtFunc f) {
  for (uint32_t i = 0; i < m->func_count; i++) {
    if (m->funcs[i].func == f) {
      return &m->funcs[i];
    }
  }
  m->funcs = smt_grow(m->funcs, sizeof(SmtFuncModel), &m->func_cap,
                      m->func_count + 1u);
  SmtFuncModel *fm = &m->funcs[m->func_count++];
  memset(fm, 0, sizeof(*fm));
  fm->func = f;
  return fm;
}

void smt_model_array_put(SmtArrayModel *a, const SmtBits *index,
                         const SmtBits *value) {
  for (uint32_t i = 0; i < a->count; i++) {
    if (smt_bits_eq(&a->entries[i].index, index)) {
      a->entries[i].value = *value;
      return;
    }
  }
  a->entries = smt_grow(a->entries, sizeof(SmtArrayEntry), &a->cap,
                        a->count + 1u);
  a->entries[a->count].index = *index;
  a->entries[a->count].value = *value;
  a->count++;
}

void smt_model_func_put(SmtFuncModel *f, unsigned arity, const SmtBits *args,
                        const SmtBits *value) {
  for (uint32_t i = 0; i < f->count; i++) {
    int same = 1;
    for (unsigned j = 0; j < arity && same; j++) {
      same = smt_bits_eq(&f->entries[i].args[j], &args[j]);
    }
    if (same) {
      f->entries[i].value = *value;
      return;
    }
  }
  f->entries = smt_grow(f->entries, sizeof(SmtFuncEntry), &f->cap,
                        f->count + 1u);
  SmtFuncEntry *e = &f->entries[f->count++];
  e->args = malloc((arity ? arity : 1u) * sizeof(SmtBits));
  memcpy(e->args, args, arity * sizeof(SmtBits));
  e->value = *value;
}

void smt_eval_init(SmtEval *e, SmtCtx *ctx, const SmtModel *model) {
  memset(e, 0, sizeof(*e));
  e->ctx = ctx;
  e->model = model;
  e->current = 1;
}

void smt_eval_reset(SmtEval *e) {
  if (++e->current == 0) {
    memset(e->stamp, 0, e->cap * sizeof(uint32_t));
    e->current = 1;
  }
}

void smt_eval_free(SmtEval *e) {
  free(e->memo);
  free(e->stamp);
  memset(e, 0, sizeof(*e));
}

static int smt_eval_select(SmtEval *e, SmtTerm array, const SmtBits *index,
                           SmtBits *out) {
  SmtCtx *ctx = e->ctx;
  for (;;) {
    const SmtNode *n = &ctx->nodes[array];
    if (n->kind == SMT_K_STORE) {
      SmtBits j;
      if (!smt_eval(e, smt_arg(ctx, array, 1), &j)) {
        return 0;
      }
      if (smt_bits_eq(&j, index)) {
        return smt_eval(e, smt_arg(ctx, array, 2), out);
      }
      array = smt_arg(ctx, array, 0);
      continue;
    }
    if (n->kind == SMT_K_ITE) {
      SmtBits c;
      if (!smt_eval(e, smt_arg(ctx, array, 0), &c)) {
        return 0;
      }
      array = smt_arg(ctx, array, c.w[0] ? 1 : 2);
      continue;
    }
    if (n->kind == SMT_K_VAR) {
      smt_bits_zero(out);
      if (e->model) {
        for (uint32_t i = 0; i < e->model->array_count; i++) {
          const SmtArrayModel *a = &e->model->arrays[i];
          if (a->term != array) {
            continue;
          }
          for (uint32_t k = 0; k < a->count; k++) {
            if (smt_bits_eq(&a->entries[k].index, index)) {
              *out = a->entries[k].value;
              return 1;
            }
          }
        }
      }
      return 1;
    }
    e->failed = 1;
    return 0;
  }
}

int smt_eval(SmtEval *e, SmtTerm t, SmtBits *out) {
  SmtCtx *ctx = e->ctx;
  if (t >= e->cap) {
    uint32_t old = e->cap;
    uint32_t cap = old ? old : 1024u;
    while (cap <= t || cap < ctx->node_count) {
      cap *= 2u;
    }
    e->memo = realloc(e->memo, cap * sizeof(SmtBits));
    e->stamp = realloc(e->stamp, cap * sizeof(uint32_t));
    memset(e->stamp + old, 0, (cap - old) * sizeof(uint32_t));
    e->cap = cap;
  }
  if (e->stamp[t] == e->current) {
    *out = e->memo[t];
    return 1;
  }
  const SmtNode *n = &ctx->nodes[t];
  SmtKind k = (SmtKind)n->kind;
  SmtBits a, b, r;
  smt_bits_zero(&r);
  switch (k) {
  case SMT_K_TRUE:
    r.w[0] = 1;
    break;
  case SMT_K_FALSE:
    break;
  case SMT_K_CONST:
    r = ctx->consts[n->p0];
    break;
  case SMT_K_VAR:
    if (smt_sort_is_array(ctx, n->sort)) {
      e->failed = 1;
      return 0;
    }
    if (e->model && t < e->model->cap && e->model->has[t]) {
      r = e->model->values[t];
    }
    break;
  case SMT_K_NOT:
    if (!smt_eval(e, smt_arg(ctx, t, 0), &a)) {
      return 0;
    }
    r.w[0] = !a.w[0];
    break;
  case SMT_K_AND:
  case SMT_K_OR:
  case SMT_K_XOR:
    if (!smt_eval(e, smt_arg(ctx, t, 0), &a) ||
        !smt_eval(e, smt_arg(ctx, t, 1), &b)) {
      return 0;
    }
    r.w[0] = k == SMT_K_AND  ? (a.w[0] & b.w[0])
             : k == SMT_K_OR ? (a.w[0] | b.w[0])
                             : (a.w[0] ^ b.w[0]);
    break;
  case SMT_K_EQ:
    if (smt_sort_is_array(ctx, ctx->nodes[smt_arg(ctx, t, 0)].sort)) {
      e->failed = 1;
      return 0;
    }
    if (!smt_eval(e, smt_arg(ctx, t, 0), &a) ||
        !smt_eval(e, smt_arg(ctx, t, 1), &b)) {
      return 0;
    }
    r.w[0] = (uint64_t)smt_bits_eq(&a, &b);
    break;
  case SMT_K_ITE:
    if (smt_sort_is_array(ctx, n->sort)) {
      e->failed = 1;
      return 0;
    }
    if (!smt_eval(e, smt_arg(ctx, t, 0), &a)) {
      return 0;
    }
    if (!smt_eval(e, smt_arg(ctx, t, a.w[0] ? 1 : 2), &r)) {
      return 0;
    }
    break;
  case SMT_K_SELECT:
    if (!smt_eval(e, smt_arg(ctx, t, 1), &a)) {
      return 0;
    }
    if (!smt_eval_select(e, smt_arg(ctx, t, 0), &a, &r)) {
      return 0;
    }
    break;
  case SMT_K_APPLY: {
    SmtBits args[16];
    if (n->nargs > 16u) {
      e->failed = 1;
      return 0;
    }
    for (unsigned i = 0; i < n->nargs; i++) {
      if (!smt_eval(e, smt_arg(ctx, t, i), &args[i])) {
        return 0;
      }
    }
    if (e->model) {
      for (uint32_t i = 0; i < e->model->func_count; i++) {
        const SmtFuncModel *f = &e->model->funcs[i];
        if (f->func != n->p0) {
          continue;
        }
        for (uint32_t j = 0; j < f->count; j++) {
          int same = 1;
          for (unsigned q = 0; q < n->nargs && same; q++) {
            same = smt_bits_eq(&f->entries[j].args[q], &args[q]);
          }
          if (same) {
            r = f->entries[j].value;
            break;
          }
        }
      }
    }
    break;
  }
  case SMT_K_STORE:
  case SMT_K_FORALL:
  case SMT_K_EXISTS:
    e->failed = 1;
    return 0;
  default: {
    unsigned bwidth = 0;
    if (!smt_eval(e, smt_arg(ctx, t, 0), &a)) {
      return 0;
    }
    bwidth = smt_width(ctx, smt_arg(ctx, t, 0));
    smt_bits_zero(&b);
    if (n->nargs > 1) {
      if (!smt_eval(e, smt_arg(ctx, t, 1), &b)) {
        return 0;
      }
      if (k == SMT_K_CONCAT) {
        bwidth = smt_width(ctx, smt_arg(ctx, t, 1));
      }
    }
    if (!smt_bits_apply(k, &r, &a, &b, ctx->sorts[n->sort].width, n->p0,
                        n->p1, bwidth)) {
      e->failed = 1;
      return 0;
    }
    break;
  }
  }
  e->memo[t] = r;
  e->stamp[t] = e->current;
  *out = r;
  return 1;
}
