#include "ir_smt.h"
#include "../common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IRS_MAX_INLINE_DEPTH 4u

typedef struct {
  const char *name;
  uint32_t hash;
  int size;
  int is_unsigned;
  int declared;
  int param;
  int addressed;
  int slot;
  SmtTerm addr;
  uint64_t region_size;
  size_t origin;
  int escaped;
  int aggregate;
} IrsVar;

typedef struct {
  size_t target;
  SmtTerm reach;
  SmtTerm *env;
} IrsEdge;

typedef struct {
  IRSmtFrame *frame;
  SmtCtx *ctx;
  const IRFunction *fn;
  const IRInstruction *insns;
  size_t count;
  IrsVar *vars;
  size_t nvars;
  size_t var_cap;
  uint32_t *buckets;
  size_t bucket_cap;
  SmtTerm *env;
  SmtTerm *params;
  SmtTerm reach;
  IrsEdge *edges;
  size_t nedges;
  size_t edge_cap;
  SmtTerm result;
  SmtTerm trap;
  SmtTerm returned;
  const char *prefix;
  size_t *loop_end;
  size_t *inv_marker;
  size_t *inv_start;
  unsigned global_epoch;
  size_t mem_id;
  SmtTerm mem_in;
  SmtTerm mem_out;
  int failed;
  char why[192];
} IrsEnc;

static SmtTerm irs_eval_invariant(IrsEnc *e, size_t header);
static void irs_trap(IrsEnc *e, SmtTerm cond);
static void irs_note_access(IrsEnc *e, SmtTerm addr, int size);
static int irs_is_foreign(const IRSmtFrame *f, SmtTerm t);
static void irs_record_site(IrsEnc *e, SmtTerm reach, SmtTerm cond,
                            const SourceLocation *at, int kind);

static uint32_t irs_hash(const char *s) {
  uint32_t h = 2166136261u;
  while (*s) {
    h = (h ^ (uint8_t)*s++) * 16777619u;
  }
  return h;
}

static void irs_fail(IrsEnc *e, const char *what, const char *detail) {
  if (e->failed) {
    return;
  }
  e->failed = 1;
  snprintf(e->why, sizeof(e->why), "%s%s%s", what, detail ? " " : "",
           detail ? detail : "");
}

static SmtSort irs_word(SmtCtx *ctx) { return smt_bv_sort(ctx, 64); }

static int irs_parse_scalar(const char *type, int *size, int *is_unsigned) {
  static const struct {
    const char *name;
    int size;
    int is_unsigned;
  } scalars[] = {{"int8", 1, 0},   {"int16", 2, 0},  {"int32", 4, 0},
                 {"int64", 8, 0},  {"uint8", 1, 1},  {"uint16", 2, 1},
                 {"uint32", 4, 1}, {"uint64", 8, 1}, {"bool", 1, 1}};
  if (!type) {
    return 0;
  }
  size_t len = strlen(type);
  if ((len > 0 && type[len - 1] == '*') || strcmp(type, "cstring") == 0 ||
      strcmp(type, "rawptr") == 0 || strncmp(type, "fn(", 3) == 0) {
    *size = 8;
    *is_unsigned = 1;
    return 1;
  }
  for (size_t i = 0; i < sizeof(scalars) / sizeof(scalars[0]); i++) {
    if (strcmp(type, scalars[i].name) == 0) {
      *size = scalars[i].size;
      *is_unsigned = scalars[i].is_unsigned;
      return 1;
    }
  }
  return 0;
}

int ir_smt_parse_scalar(const char *type, int *size, int *is_unsigned) {
  return irs_parse_scalar(type, size, is_unsigned);
}

SmtTerm ir_smt_narrow(SmtCtx *ctx, SmtTerm v, int size, int is_unsigned) {
  if (size <= 0 || size >= 8) {
    return v;
  }
  SmtTerm low = smt_extract(ctx, v, (unsigned)size * 8u - 1u, 0);
  unsigned extra = 64u - (unsigned)size * 8u;
  return is_unsigned ? smt_zext(ctx, low, extra) : smt_sext(ctx, low, extra);
}

static SmtTerm irs_bool_word(SmtCtx *ctx, SmtTerm b) {
  return smt_ite(ctx, b, smt_bv_u64(ctx, 64, 1), smt_bv_u64(ctx, 64, 0));
}

static SmtTerm irs_nonzero(SmtCtx *ctx, SmtTerm v) {
  return smt_not(ctx, smt_eq(ctx, v, smt_bv_u64(ctx, 64, 0)));
}

static SmtTerm irs_compare(SmtCtx *ctx, const char *op, int is_unsigned,
                           SmtTerm a, SmtTerm b, int *ok) {
  *ok = 1;
  if (strcmp(op, "==") == 0) {
    return smt_eq(ctx, a, b);
  }
  if (strcmp(op, "!=") == 0) {
    return smt_not(ctx, smt_eq(ctx, a, b));
  }
  SmtKind lt = is_unsigned ? SMT_K_BVULT : SMT_K_BVSLT;
  SmtKind le = is_unsigned ? SMT_K_BVULE : SMT_K_BVSLE;
  if (strcmp(op, "<") == 0) {
    return smt_mk2(ctx, lt, a, b);
  }
  if (strcmp(op, "<=") == 0) {
    return smt_mk2(ctx, le, a, b);
  }
  if (strcmp(op, ">") == 0) {
    return smt_mk2(ctx, lt, b, a);
  }
  if (strcmp(op, ">=") == 0) {
    return smt_mk2(ctx, le, b, a);
  }
  *ok = 0;
  return 0;
}

SmtTerm ir_smt_binary(SmtCtx *ctx, const char *op, int is_unsigned, SmtTerm a,
                      SmtTerm b, SmtTerm *trap) {
  SmtTerm six3 = smt_bv_u64(ctx, 64, 63);
  if (trap) {
    *trap = smt_false(ctx);
  }
  if (strcmp(op, "+") == 0) {
    return smt_mk2(ctx, SMT_K_BVADD, a, b);
  }
  if (strcmp(op, "-") == 0) {
    return smt_mk2(ctx, SMT_K_BVSUB, a, b);
  }
  if (strcmp(op, "*") == 0) {
    return smt_mk2(ctx, SMT_K_BVMUL, a, b);
  }
  if (strcmp(op, "&") == 0) {
    return smt_mk2(ctx, SMT_K_BVAND, a, b);
  }
  if (strcmp(op, "|") == 0) {
    return smt_mk2(ctx, SMT_K_BVOR, a, b);
  }
  if (strcmp(op, "^") == 0) {
    return smt_mk2(ctx, SMT_K_BVXOR, a, b);
  }
  if (strcmp(op, "<<") == 0) {
    return smt_mk2(ctx, SMT_K_BVSHL, a, smt_mk2(ctx, SMT_K_BVAND, b, six3));
  }
  if (strcmp(op, ">>") == 0) {
    return smt_mk2(ctx, is_unsigned ? SMT_K_BVLSHR : SMT_K_BVASHR, a,
                   smt_mk2(ctx, SMT_K_BVAND, b, six3));
  }
  if (strcmp(op, "/") == 0 || strcmp(op, "%") == 0) {
    int div = op[0] == '/';
    SmtTerm zero = smt_bv_u64(ctx, 64, 0);
    if (trap) {
      *trap = smt_eq(ctx, b, zero);
    }
    if (is_unsigned) {
      return smt_mk2(ctx, div ? SMT_K_BVUDIV : SMT_K_BVUREM, a, b);
    }
    SmtTerm minus_one = smt_bv_i64(ctx, 64, -1);
    SmtTerm special = div ? smt_mk1(ctx, SMT_K_BVNEG, a) : zero;
    SmtTerm normal = smt_mk2(ctx, div ? SMT_K_BVSDIV : SMT_K_BVSREM, a, b);
    return smt_ite(ctx, smt_eq(ctx, b, minus_one), special, normal);
  }
  if (strcmp(op, "&&") == 0) {
    return irs_bool_word(ctx, smt_and(ctx, irs_nonzero(ctx, a),
                                      irs_nonzero(ctx, b)));
  }
  if (strcmp(op, "||") == 0) {
    return irs_bool_word(ctx, smt_or(ctx, irs_nonzero(ctx, a),
                                     irs_nonzero(ctx, b)));
  }
  int ok;
  SmtTerm c = irs_compare(ctx, op, is_unsigned, a, b, &ok);
  if (ok) {
    return irs_bool_word(ctx, c);
  }
  return 0;
}

static size_t irs_var_lookup(IrsEnc *e, const char *name, int create) {
  uint32_t h = irs_hash(name);
  if (e->bucket_cap) {
    size_t slot = h & (e->bucket_cap - 1u);
    while (e->buckets[slot]) {
      IrsVar *v = &e->vars[e->buckets[slot] - 1u];
      if (v->hash == h && strcmp(v->name, name) == 0) {
        return e->buckets[slot] - 1u;
      }
      slot = (slot + 1u) & (e->bucket_cap - 1u);
    }
  }
  if (!create) {
    return (size_t)-1;
  }
  if ((e->nvars + 1u) * 2u > e->bucket_cap) {
    size_t cap = e->bucket_cap ? e->bucket_cap * 2u : 64u;
    uint32_t *buckets = calloc(cap, sizeof(uint32_t));
    for (size_t i = 0; i < e->nvars; i++) {
      size_t slot = e->vars[i].hash & (cap - 1u);
      while (buckets[slot]) {
        slot = (slot + 1u) & (cap - 1u);
      }
      buckets[slot] = (uint32_t)(i + 1u);
    }
    free(e->buckets);
    e->buckets = buckets;
    e->bucket_cap = cap;
  }
  if (e->nvars == e->var_cap) {
    e->var_cap = e->var_cap ? e->var_cap * 2u : 32u;
    e->vars = realloc(e->vars, e->var_cap * sizeof(IrsVar));
  }
  IrsVar *v = &e->vars[e->nvars];
  memset(v, 0, sizeof(*v));
  v->name = name;
  v->hash = h;
  size_t slot = h & (e->bucket_cap - 1u);
  while (e->buckets[slot]) {
    slot = (slot + 1u) & (e->bucket_cap - 1u);
  }
  e->buckets[slot] = (uint32_t)(e->nvars + 1u);
  return e->nvars++;
}

static void irs_note_operand(IrsEnc *e, const IROperand *op) {
  if ((op->kind == IR_OPERAND_TEMP || op->kind == IR_OPERAND_SYMBOL) &&
      op->name) {
    irs_var_lookup(e, op->name, 1);
  }
}

static SmtSort irs_memory_sort(SmtCtx *ctx) {
  return smt_array_sort(ctx, smt_bv_sort(ctx, 64), smt_bv_sort(ctx, 8));
}

static SmtTerm irs_shared_sorted(IrsEnc *e, const char *kind, const char *name,
                                 SmtSort sort) {
  IRSmtFrame *f = e->frame;
  char key[256];
  snprintf(key, sizeof(key), "%s%s:%s", e->prefix ? e->prefix : "", kind,
           name);
  for (size_t i = 0; i < f->shared_count; i++) {
    if (strcmp(f->shared[i].name, key) == 0) {
      return f->shared[i].term;
    }
  }
  if (f->shared_count == f->shared_cap) {
    f->shared_cap = f->shared_cap ? f->shared_cap * 2u : 32u;
    f->shared = realloc(f->shared, f->shared_cap * sizeof(IRSmtShared));
  }
  SmtTerm t = smt_var(e->ctx, key, sort);
  f->shared[f->shared_count].name = mettle_strdup(key);
  f->shared[f->shared_count].term = t;
  f->shared_count++;
  return t;
}

static SmtTerm irs_shared(IrsEnc *e, const char *kind, const char *name) {
  return irs_shared_sorted(e, kind, name, irs_word(e->ctx));
}

static SmtTerm irs_address(SmtCtx *ctx, SmtTerm base, unsigned offset) {
  if (offset == 0) {
    return base;
  }
  return smt_mk2(ctx, SMT_K_BVADD, base, smt_bv_u64(ctx, 64, offset));
}

static size_t irs_region_of(const IRSmtFrame *f, SmtCtx *ctx, SmtTerm addr,
                            int size);

static SmtTerm irs_skip_other_regions(const IRSmtFrame *f, SmtCtx *ctx,
                                      SmtTerm mem, SmtTerm addr) {
  size_t mine = irs_region_of(f, ctx, addr, 1);
  int mine_private = mine != (size_t)-1 && f->region_private[mine];
  int mine_foreign = mine == (size_t)-1 && irs_is_foreign(f, addr);
  if (mine == (size_t)-1 && !mine_foreign) {
    return mem;
  }
  while (smt_term_kind(ctx, mem) == SMT_K_STORE) {
    SmtTerm at = smt_term_arg(ctx, mem, 1);
    size_t theirs = irs_region_of(f, ctx, at, 1);
    int apart = 0;
    if (mine != (size_t)-1) {
      apart = theirs != (size_t)-1 ? theirs != mine
                                   : mine_private && irs_is_foreign(f, at);
    } else {
      apart = theirs != (size_t)-1 && f->region_private[theirs];
    }
    if (!apart) {
      break;
    }
    mem = smt_term_arg(ctx, mem, 0);
  }
  return mem;
}

static int irs_frame_inside_region(const IRSmtFrame *f, SmtCtx *ctx,
                                   SmtTerm addr, int size) {
  return irs_region_of(f, ctx, addr, size) != (size_t)-1;
}

static size_t irs_region_of(const IRSmtFrame *f, SmtCtx *ctx, SmtTerm addr,
                            int size) {
  SmtTerm base = addr;
  uint64_t offset = 0;
  if (smt_term_kind(ctx, addr) == SMT_K_BVADD && smt_term_nargs(ctx, addr) == 2) {
    SmtTerm x = smt_term_arg(ctx, addr, 0), y = smt_term_arg(ctx, addr, 1);
    uint64_t w[4];
    if (smt_term_is_const(ctx, y) && smt_term_const_words(ctx, y, w)) {
      base = x;
      offset = w[0];
    } else if (smt_term_is_const(ctx, x) && smt_term_const_words(ctx, x, w)) {
      base = y;
      offset = w[0];
    }
  }
  for (size_t i = 0; i < f->region_count; i++) {
    if (f->region_base[i] == base) {
      return offset < f->region_size[i] &&
                     (uint64_t)size <= f->region_size[i] - offset
                 ? i
                 : (size_t)-1;
    }
  }
  return (size_t)-1;
}

typedef struct {
  SmtTerm *from;
  SmtTerm *to;
  size_t count;
  size_t cap;
} IrsMemo;

static SmtTerm irs_strip_dead_stores(const IRSmtFrame *f, SmtCtx *ctx,
                                     SmtTerm mem, IrsMemo *memo) {
  for (size_t i = 0; i < memo->count; i++) {
    if (memo->from[i] == mem) {
      return memo->to[i];
    }
  }
  SmtTerm out = mem;
  SmtKind kind = smt_term_kind(ctx, mem);
  if (kind == SMT_K_STORE) {
    SmtTerm inner =
        irs_strip_dead_stores(f, ctx, smt_term_arg(ctx, mem, 0), memo);
    SmtTerm addr = smt_term_arg(ctx, mem, 1);
    out = irs_frame_inside_region(f, ctx, addr, 1)
              ? inner
              : smt_store(ctx, inner, addr, smt_term_arg(ctx, mem, 2));
  } else if (kind == SMT_K_ITE) {
    out = smt_ite(
        ctx, smt_term_arg(ctx, mem, 0),
        irs_strip_dead_stores(f, ctx, smt_term_arg(ctx, mem, 1), memo),
        irs_strip_dead_stores(f, ctx, smt_term_arg(ctx, mem, 2), memo));
  }
  if (memo->count == memo->cap) {
    memo->cap = memo->cap ? memo->cap * 2u : 64u;
    memo->from = realloc(memo->from, memo->cap * sizeof(SmtTerm));
    memo->to = realloc(memo->to, memo->cap * sizeof(SmtTerm));
  }
  memo->from[memo->count] = mem;
  memo->to[memo->count] = out;
  memo->count++;
  return out;
}

static SmtTerm irs_access_trap(IrsEnc *e, SmtTerm addr, int size) {
  SmtCtx *ctx = e->ctx;
  if (irs_frame_inside_region(e->frame, ctx, addr, size)) {
    return smt_false(ctx);
  }
  irs_note_access(e, addr, size);
  char name[48];
  snprintf(name, sizeof(name), "invalid_address_%d", size);
  SmtSort domain = irs_word(ctx);
  SmtFunc invalid = smt_func(ctx, name, 1, &domain, smt_bool_sort(ctx));
  return smt_or(ctx, smt_eq(ctx, addr, smt_bv_u64(ctx, 64, 0)),
                smt_apply(ctx, invalid, 1, &addr));
}

static int irs_access_size(IrsEnc *e, const IRInstruction *insn) {
  if (insn->is_float) {
    irs_fail(e, "it loads or stores a float", NULL);
    return 0;
  }
  if (insn->rhs.kind != IR_OPERAND_INT ||
      (insn->rhs.int_value != 1 && insn->rhs.int_value != 2 &&
       insn->rhs.int_value != 4 && insn->rhs.int_value != 8)) {
    irs_fail(e, "it copies a block of memory", NULL);
    return 0;
  }
  return (int)insn->rhs.int_value;
}

static SmtTerm irs_load_bytes(IrsEnc *e, SmtTerm addr, int size,
                               int is_unsigned) {
  SmtCtx *ctx = e->ctx;
  SmtTerm mem = e->env[e->mem_id];
  SmtTerm v = 0;
  for (int i = 0; i < size; i++) {
    SmtTerm at = irs_address(ctx, addr, (unsigned)i);
    SmtTerm byte =
        smt_select(ctx, irs_skip_other_regions(e->frame, ctx, mem, at), at);
    v = v ? smt_mk2(ctx, SMT_K_CONCAT, byte, v) : byte;
  }
  unsigned extra = 64u - (unsigned)size * 8u;
  if (extra) {
    v = is_unsigned ? smt_zext(ctx, v, extra) : smt_sext(ctx, v, extra);
  }
  return v;
}

static void irs_store_bytes(IrsEnc *e, SmtTerm addr, int size, SmtTerm value) {
  SmtCtx *ctx = e->ctx;
  SmtTerm mem = e->env[e->mem_id];
  for (int i = 0; i < size; i++) {
    SmtTerm byte =
        smt_extract(ctx, value, (unsigned)i * 8u + 7u, (unsigned)i * 8u);
    mem = smt_store(ctx, mem, irs_address(ctx, addr, (unsigned)i), byte);
  }
  e->env[e->mem_id] = mem;
}

static uint64_t irs_aggregate_size(const IRInstruction *insn) {
  const MtlcType *vt = insn->value_type;
  if (vt && (vt->kind == MTLC_TYPE_STRUCT || vt->kind == MTLC_TYPE_ARRAY) &&
      vt->size > 0) {
    return (uint64_t)vt->size;
  }
  const char *type = insn->text;
  if (!type) {
    return 0;
  }
  const char *open = strchr(type, '[');
  if (!open || open == type) {
    return 0;
  }
  char elem[64];
  size_t len = (size_t)(open - type);
  if (len >= sizeof(elem)) {
    return 0;
  }
  memcpy(elem, type, len);
  elem[len] = '\0';
  int size = 0, is_unsigned = 0;
  if (strcmp(elem, "float64") == 0) {
    size = 8;
  } else if (strcmp(elem, "float32") == 0) {
    size = 4;
  } else if (!irs_parse_scalar(elem, &size, &is_unsigned)) {
    return 0;
  }
  uint64_t total = (uint64_t)size;
  const char *at = open;
  while (*at == '[') {
    char *end = NULL;
    unsigned long long n = strtoull(at + 1, &end, 10);
    if (!end || *end != ']' || n == 0 || n > (1ull << 32)) {
      return 0;
    }
    total *= n;
    at = end + 1;
  }
  return *at == '\0' ? total : 0;
}

static SmtTerm irs_region(IrsEnc *e, const char *name, uint64_t size,
                          int private_region) {
  SmtCtx *ctx = e->ctx;
  IRSmtFrame *f = e->frame;
  SmtTerm base = irs_shared(e, "frame", name);
  for (size_t i = 0; i < f->region_count; i++) {
    if (f->region_base[i] == base) {
      f->region_private[i] &= (unsigned char)(private_region != 0);
      return base;
    }
  }
  if (size == 0) {
    size = 1;
  }
  SmtTerm last = irs_address(ctx, base, 0);
  if (size > 1) {
    last = smt_mk2(ctx, SMT_K_BVADD, base, smt_bv_u64(ctx, 64, size - 1u));
  }
  SmtTerm fact = smt_and(ctx, smt_not(ctx, smt_eq(ctx, base, smt_bv_u64(ctx, 64, 0))),
                         smt_mk2(ctx, SMT_K_BVULE, base, last));
  for (size_t i = 0; i < f->region_count; i++) {
    SmtTerm other = f->region_base[i];
    SmtTerm other_last = smt_mk2(ctx, SMT_K_BVADD, other,
                                 smt_bv_u64(ctx, 64, f->region_size[i] - 1u));
    fact = smt_and(ctx, fact,
                   smt_or(ctx, smt_mk2(ctx, SMT_K_BVULT, last, other),
                          smt_mk2(ctx, SMT_K_BVULT, other_last, base)));
  }
  f->assume = smt_and(ctx, f->assume ? f->assume : smt_true(ctx), fact);
  if (f->region_count == f->region_cap) {
    f->region_cap = f->region_cap ? f->region_cap * 2u : 16u;
    f->region_base = realloc(f->region_base, f->region_cap * sizeof(SmtTerm));
    f->region_size = realloc(f->region_size, f->region_cap * sizeof(uint64_t));
    f->region_private = realloc(f->region_private, f->region_cap);
  }
  f->region_base[f->region_count] = base;
  f->region_size[f->region_count] = size;
  f->region_private[f->region_count] = (unsigned char)(private_region != 0);
  f->region_count++;
  return base;
}

static int irs_mentions_region(const IRSmtFrame *f, SmtCtx *ctx, SmtTerm t,
                               SmtTerm *seen, size_t *nseen, size_t cap) {
  for (size_t i = 0; i < *nseen; i++) {
    if (seen[i] == t) {
      return 0;
    }
  }
  if (*nseen == cap) {
    return 1;
  }
  seen[(*nseen)++] = t;
  unsigned n = smt_term_nargs(ctx, t);
  if (n == 0) {
    for (size_t i = 0; i < f->region_count; i++) {
      if (f->region_base[i] == t) {
        return 1;
      }
    }
    return 0;
  }
  for (unsigned i = 0; i < n; i++) {
    if (irs_mentions_region(f, ctx, smt_term_arg(ctx, t, i), seen, nseen,
                            cap)) {
      return 1;
    }
  }
  return 0;
}

static int irs_is_foreign(const IRSmtFrame *f, SmtTerm t) {
  if (!f->foreign_cap) {
    return 0;
  }
  size_t slot = (t * 2654435761u) & (f->foreign_cap - 1u);
  while (f->foreign[slot]) {
    if (f->foreign[slot] == t) {
      return 1;
    }
    slot = (slot + 1u) & (f->foreign_cap - 1u);
  }
  return 0;
}

static void irs_add_foreign(IRSmtFrame *f, SmtTerm t) {
  if (irs_is_foreign(f, t)) {
    return;
  }
  if ((f->foreign_count + 1u) * 2u > f->foreign_cap) {
    size_t cap = f->foreign_cap ? f->foreign_cap * 2u : 64u;
    SmtTerm *table = calloc(cap, sizeof(SmtTerm));
    for (size_t i = 0; i < f->foreign_cap; i++) {
      if (f->foreign[i]) {
        size_t slot = (f->foreign[i] * 2654435761u) & (cap - 1u);
        while (table[slot]) {
          slot = (slot + 1u) & (cap - 1u);
        }
        table[slot] = f->foreign[i];
      }
    }
    free(f->foreign);
    f->foreign = table;
    f->foreign_cap = cap;
  }
  size_t slot = (t * 2654435761u) & (f->foreign_cap - 1u);
  while (f->foreign[slot]) {
    slot = (slot + 1u) & (f->foreign_cap - 1u);
  }
  f->foreign[slot] = t;
  f->foreign_count++;
}

static void irs_note_access(IrsEnc *e, SmtTerm addr, int size) {
  IRSmtFrame *f = e->frame;
  SmtTerm seen[512];
  size_t nseen = 0;
  if (f->access_count >= 512u ||
      irs_mentions_region(f, e->ctx, addr, seen, &nseen, 512u)) {
    return;
  }
  for (size_t i = 0; i < f->access_count; i++) {
    if (f->access_addr[i] == addr && f->access_size[i] == (uint64_t)size) {
      return;
    }
  }
  if (f->access_count == f->access_cap) {
    f->access_cap = f->access_cap ? f->access_cap * 2u : 32u;
    f->access_addr = realloc(f->access_addr, f->access_cap * sizeof(SmtTerm));
    f->access_size = realloc(f->access_size, f->access_cap * sizeof(uint64_t));
  }
  f->access_addr[f->access_count] = addr;
  f->access_size[f->access_count] = (uint64_t)size;
  f->access_count++;
  for (int i = 0; i < size; i++) {
    irs_add_foreign(f, irs_address(e->ctx, addr, (unsigned)i));
  }
}

SmtTerm ir_smt_frame_assumption(IRSmtFrame *f) {
  SmtCtx *ctx = f->ctx;
  SmtTerm out = f->assume ? f->assume : smt_true(ctx);
  for (size_t a = 0; a < f->access_count; a++) {
    SmtTerm lo = f->access_addr[a];
    SmtTerm hi = smt_mk2(ctx, SMT_K_BVADD, lo,
                         smt_bv_u64(ctx, 64, f->access_size[a] - 1u));
    int any = 0;
    for (size_t r = 0; r < f->region_count; r++) {
      if (!f->region_private[r]) {
        continue;
      }
      SmtTerm base = f->region_base[r];
      SmtTerm last = smt_mk2(ctx, SMT_K_BVADD, base,
                             smt_bv_u64(ctx, 64, f->region_size[r] - 1u));
      out = smt_and(ctx, out,
                    smt_or(ctx, smt_mk2(ctx, SMT_K_BVULT, hi, base),
                           smt_mk2(ctx, SMT_K_BVULT, last, lo)));
      any = 1;
    }
    if (any) {
      out = smt_and(ctx, out, smt_mk2(ctx, SMT_K_BVULE, lo, hi));
    }
  }
  return out;
}

static int irs_holds_aggregate(IrsEnc *e, const IROperand *op) {
  if ((op->kind != IR_OPERAND_TEMP && op->kind != IR_OPERAND_SYMBOL) ||
      !op->name) {
    return 0;
  }
  IrsVar *v = &e->vars[irs_var_lookup(e, op->name, 1)];
  return v->aggregate || (v->addr && !v->slot);
}

static void irs_copy_bytes(IrsEnc *e, SmtTerm dst, SmtTerm src,
                           long long size) {
  SmtCtx *ctx = e->ctx;
  if (size <= 0 || size > 256) {
    irs_fail(e, "it copies a block of memory larger than 256 bytes", NULL);
    return;
  }
  int n = (int)size;
  irs_trap(e, irs_access_trap(e, src, n));
  irs_trap(e, irs_access_trap(e, dst, n));
  SmtTerm before = e->env[e->mem_id];
  SmtTerm mem = before;
  for (int i = 0; i < n; i++) {
    SmtTerm from = irs_address(ctx, src, (unsigned)i);
    SmtTerm byte = smt_select(
        ctx, irs_skip_other_regions(e->frame, ctx, before, from), from);
    mem = smt_store(ctx, mem, irs_address(ctx, dst, (unsigned)i), byte);
  }
  e->env[e->mem_id] = mem;
}

static int irs_returns_aggregate(const IRFunction *fn) {
  const char *type = fn ? fn->return_type_name : NULL;
  int size, is_unsigned;
  return type && strcmp(type, "void") != 0 &&
         !irs_parse_scalar(type, &size, &is_unsigned);
}

static void irs_mark_aggregate(IrsEnc *e, const IROperand *dest) {
  if ((dest->kind == IR_OPERAND_TEMP || dest->kind == IR_OPERAND_SYMBOL) &&
      dest->name) {
    e->vars[irs_var_lookup(e, dest->name, 1)].aggregate = 1;
  }
}

static SmtTerm irs_read(IrsEnc *e, const IROperand *op) {
  SmtCtx *ctx = e->ctx;
  switch (op->kind) {
  case IR_OPERAND_INT:
    return smt_bv_i64(ctx, 64, op->int_value);
  case IR_OPERAND_TEMP:
  case IR_OPERAND_SYMBOL: {
    if (!op->name) {
      irs_fail(e, "an operand has no name", NULL);
      return smt_bv_u64(ctx, 64, 0);
    }
    size_t id = irs_var_lookup(e, op->name, 1);
    if (e->vars[id].slot) {
      return irs_load_bytes(e, e->vars[id].addr, e->vars[id].size,
                            e->vars[id].is_unsigned);
    }
    if (e->env[id]) {
      return e->env[id];
    }
    IrsVar *v = &e->vars[id];
    if (op->kind == IR_OPERAND_SYMBOL && !v->declared && !v->param) {
      if (e->frame->uf_calls && e->global_epoch) {
        char kind[32];
        snprintf(kind, sizeof(kind), "global@%u", e->global_epoch);
        return irs_shared(e, kind, op->name);
      }
      return irs_shared(e, "global", op->name);
    }
    return irs_shared(e, "undefined", op->name);
  }
  case IR_OPERAND_FLOAT:
    irs_fail(e, "it uses a float", NULL);
    break;
  case IR_OPERAND_STRING:
    irs_fail(e, "it uses a string", NULL);
    break;
  default:
    irs_fail(e, "it uses an operand the encoder does not model", NULL);
    break;
  }
  return smt_bv_u64(ctx, 64, 0);
}

static void irs_write(IrsEnc *e, const IROperand *dest, SmtTerm value) {
  if (dest->kind == IR_OPERAND_NONE) {
    return;
  }
  if ((dest->kind != IR_OPERAND_TEMP && dest->kind != IR_OPERAND_SYMBOL) ||
      !dest->name) {
    irs_fail(e, "it writes an operand the encoder does not model", NULL);
    return;
  }
  size_t id = irs_var_lookup(e, dest->name, 1);
  IrsVar *v = &e->vars[id];
  if (dest->kind == IR_OPERAND_SYMBOL && !v->declared && !v->param) {
    irs_fail(e, "it writes the global", dest->name);
    return;
  }
  if (v->slot) {
    irs_store_bytes(e, v->addr, v->size, value);
    return;
  }
  if (v->size > 0 && v->size < 8) {
    value = ir_smt_narrow(e->ctx, value, v->size, v->is_unsigned);
  }
  e->env[id] = value;
}

static size_t irs_label(IrsEnc *e, const char *name) {
  for (size_t i = 0; i < e->count; i++) {
    if (e->insns[i].op == IR_OP_LABEL && e->insns[i].text &&
        strcmp(e->insns[i].text, name) == 0) {
      return i;
    }
  }
  return (size_t)-1;
}

static void irs_edge(IrsEnc *e, size_t at, const char *label, SmtTerm cond) {
  if (!label) {
    irs_fail(e, "a jump has no target", NULL);
    return;
  }
  size_t target = irs_label(e, label);
  if (target == (size_t)-1) {
    irs_fail(e, "a jump targets an unknown label", label);
    return;
  }
  if (target <= at) {
    if (!e->frame->havoc_loops) {
      irs_fail(e, "it has a loop", NULL);
      return;
    }
    if (e->inv_marker && e->inv_marker[target]) {
      SmtTerm reach = smt_and(e->ctx, e->reach, cond);
      if (reach != smt_false(e->ctx)) {
        SmtTerm saved = e->reach;
        e->reach = reach;
        SmtTerm step = irs_eval_invariant(e, target);
        e->reach = saved;
        irs_record_site(e, reach, step,
                        &e->insns[e->inv_marker[target]].location,
                        IR_SMT_SITE_INVARIANT_STEP);
      }
    }
    return;
  }
  SmtTerm reach = smt_and(e->ctx, e->reach, cond);
  if (reach == smt_false(e->ctx)) {
    return;
  }
  if (e->nedges == e->edge_cap) {
    e->edge_cap = e->edge_cap ? e->edge_cap * 2u : 16u;
    e->edges = realloc(e->edges, e->edge_cap * sizeof(IrsEdge));
  }
  IrsEdge *edge = &e->edges[e->nedges++];
  edge->target = target;
  edge->reach = reach;
  edge->env = malloc((e->nvars ? e->nvars : 1u) * sizeof(SmtTerm));
  memcpy(edge->env, e->env, e->nvars * sizeof(SmtTerm));
}

static SmtTerm irs_materialize(IrsEnc *e, size_t id, SmtTerm v) {
  if (v) {
    return v;
  }
  IROperand op;
  memset(&op, 0, sizeof(op));
  op.kind = e->vars[id].declared || e->vars[id].param ? IR_OPERAND_TEMP
                                                       : IR_OPERAND_SYMBOL;
  op.name = (char *)e->vars[id].name;
  SmtTerm saved = e->env[id];
  e->env[id] = 0;
  SmtTerm out = irs_read(e, &op);
  e->env[id] = saved;
  return out;
}

static void irs_merge(IrsEnc *e, size_t at) {
  SmtCtx *ctx = e->ctx;
  int any = e->reach != smt_false(ctx);
  for (size_t k = 0; k < e->nedges; k++) {
    IrsEdge *edge = &e->edges[k];
    if (edge->target != at) {
      continue;
    }
    if (!any) {
      memcpy(e->env, edge->env, e->nvars * sizeof(SmtTerm));
      e->reach = edge->reach;
      any = 1;
    } else {
      for (size_t i = 0; i < e->nvars; i++) {
        SmtTerm here = e->env[i], there = edge->env[i];
        if (here == there) {
          continue;
        }
        here = irs_materialize(e, i, here);
        there = irs_materialize(e, i, there);
        e->env[i] = smt_ite(ctx, edge->reach, there, here);
      }
      e->reach = smt_or(ctx, e->reach, edge->reach);
    }
    free(edge->env);
    *edge = e->edges[--e->nedges];
    k--;
  }
}

static void irs_trap(IrsEnc *e, SmtTerm cond) {
  e->trap = smt_or(e->ctx, e->trap, smt_and(e->ctx, e->reach, cond));
}

static const IRFunction *irs_find_function(const IRProgram *program,
                                           const char *name) {
  if (!program || !name) {
    return NULL;
  }
  for (size_t i = 0; i < program->function_count; i++) {
    const IRFunction *f = program->functions[i];
    if (f && f->name && strcmp(f->name, name) == 0) {
      return f;
    }
  }
  return NULL;
}

static int irs_run(IrsEnc *e);

SmtFunc ir_smt_value_func(SmtCtx *ctx, const char *name, size_t arity) {
  SmtSort domain[16];
  for (size_t i = 0; i < arity && i < 16u; i++) {
    domain[i] = smt_bv_sort(ctx, 64);
  }
  return smt_func(ctx, name, (unsigned)arity, domain, smt_bv_sort(ctx, 64));
}

SmtFunc ir_smt_trap_func(SmtCtx *ctx, const char *name, size_t arity) {
  SmtSort domain[16];
  char trap_name[256];
  for (size_t i = 0; i < arity && i < 16u; i++) {
    domain[i] = smt_bv_sort(ctx, 64);
  }
  snprintf(trap_name, sizeof(trap_name), "%s!trap", name);
  return smt_func(ctx, trap_name, (unsigned)arity, domain,
                  smt_bool_sort(ctx));
}

static int irs_is_trap_call(const char *name) {
  return name && (strcmp(name, "mettle_crash_trap") == 0 ||
                  strcmp(name, "mettle_crash_trap_ex") == 0 ||
                  strcmp(name, "mettle_crash_panic") == 0);
}

static int irs_callee_keeps_memory(const IRFunction *callee) {
  for (size_t i = 0; i < callee->instruction_count; i++) {
    const IRInstruction *insn = &callee->instructions[i];
    switch (insn->op) {
    case IR_OP_NOP:
    case IR_OP_LABEL:
    case IR_OP_JUMP:
    case IR_OP_BRANCH_ZERO:
    case IR_OP_BRANCH_EQ:
    case IR_OP_DECLARE_LOCAL:
    case IR_OP_ASSIGN:
    case IR_OP_BINARY:
    case IR_OP_UNARY:
    case IR_OP_CAST:
    case IR_OP_SELECT:
    case IR_OP_RETURN:
    case IR_OP_LOAD:
      break;
    case IR_OP_CALL:
      if (insn->text && (strcmp(insn->text, IR_SMT_PROVE_CALL) == 0 ||
                         strcmp(insn->text, IR_SMT_INVARIANT_CALL) == 0 ||
                         irs_is_trap_call(insn->text))) {
        break;
      }
      return 0;
    default:
      return 0;
    }
  }
  return 1;
}

static void irs_after_call_memory(IrsEnc *e, const IRFunction *callee) {
  if (irs_callee_keeps_memory(callee)) {
    return;
  }
  SmtCtx *ctx = e->ctx;
  SmtTerm old = e->env[e->mem_id];
  SmtTerm mem = smt_fresh(ctx, "memory@call", irs_memory_sort(ctx));
  for (size_t i = 0; i < e->nvars; i++) {
    IrsVar *v = &e->vars[i];
    if (!v->addr || v->escaped || v->region_size > 256u) {
      continue;
    }
    for (uint64_t k = 0; k < v->region_size; k++) {
      SmtTerm at = irs_address(ctx, v->addr, (unsigned)k);
      mem = smt_store(ctx, mem, at, smt_select(ctx, old, at));
    }
  }
  e->env[e->mem_id] = mem;
}

static void irs_call_uf(IrsEnc *e, const IRInstruction *insn) {
  SmtCtx *ctx = e->ctx;
  if (!insn->text) {
    irs_fail(e, "a call has no target", NULL);
    return;
  }
  if (insn->argument_count > 16u) {
    irs_fail(e, "a call passes more than 16 arguments to", insn->text);
    return;
  }
  const IRFunction *callee = irs_find_function(e->frame->program, insn->text);
  if (!callee) {
    irs_fail(e, "it calls", insn->text);
    return;
  }
  SmtTerm args[16];
  for (size_t i = 0; i < insn->argument_count; i++) {
    args[i] = irs_read(e, &insn->arguments[i]);
    int size = 8, is_unsigned = 0;
    if (i < callee->parameter_count && callee->parameter_types &&
        irs_parse_scalar(callee->parameter_types[i], &size, &is_unsigned)) {
      args[i] = ir_smt_narrow(ctx, args[i], size, is_unsigned);
    }
  }
  IRSmtFrame *f = e->frame;
  SmtTerm value, trap;
  if (f->deterministic &&
      !f->deterministic(f->deterministic_context, insn->text)) {
    char prefix[200];
    snprintf(prefix, sizeof(prefix), "%s@call", insn->text);
    value = smt_fresh(ctx, prefix, irs_word(ctx));
    trap = smt_fresh(ctx, prefix, smt_bool_sort(ctx));
    irs_trap(e, trap);
    irs_write(e, &insn->dest, value);
    if (irs_returns_aggregate(callee)) {
      irs_mark_aggregate(e, &insn->dest);
    }
    e->global_epoch++;
    irs_after_call_memory(e, callee);
    return;
  }
  SmtFunc vf = ir_smt_value_func(ctx, insn->text, insn->argument_count);
  SmtFunc tf = ir_smt_trap_func(ctx, insn->text, insn->argument_count);
  value = smt_apply(ctx, vf, (unsigned)insn->argument_count, args);
  trap = smt_apply(ctx, tf, (unsigned)insn->argument_count, args);
  if (f->call_count == f->call_cap) {
    f->call_cap = f->call_cap ? f->call_cap * 2u : 16u;
    f->calls = realloc(f->calls, f->call_cap * sizeof(IRSmtCallSite));
  }
  IRSmtCallSite *site = &f->calls[f->call_count++];
  site->callee = insn->text;
  site->arg_count = insn->argument_count;
  memcpy(site->args, args, insn->argument_count * sizeof(SmtTerm));
  site->reach = e->reach;
  irs_trap(e, trap);
  irs_write(e, &insn->dest, value);
  if (irs_returns_aggregate(callee)) {
    irs_mark_aggregate(e, &insn->dest);
  }
  e->global_epoch++;
  irs_after_call_memory(e, callee);
}

static void irs_record_site(IrsEnc *e, SmtTerm reach, SmtTerm cond,
                            const SourceLocation *at, int kind) {
  IRSmtFrame *f = e->frame;
  if (f->prove_count == f->prove_cap) {
    f->prove_cap = f->prove_cap ? f->prove_cap * 2u : 8u;
    f->proves = realloc(f->proves, f->prove_cap * sizeof(IRSmtProveSite));
  }
  IRSmtProveSite *site = &f->proves[f->prove_count++];
  site->reach = reach;
  site->trap = e->trap;
  site->condition = cond;
  site->location = *at;
  site->kind = kind;
}

static void irs_step(IrsEnc *e, size_t pc);

static SmtTerm irs_eval_invariant(IrsEnc *e, size_t header) {
  SmtCtx *ctx = e->ctx;
  size_t marker = e->inv_marker[header];
  SmtTerm *saved_env = malloc((e->nvars ? e->nvars : 1u) * sizeof(SmtTerm));
  memcpy(saved_env, e->env, e->nvars * sizeof(SmtTerm));
  SmtTerm saved_reach = e->reach, saved_trap = e->trap;
  unsigned saved_epoch = e->global_epoch;
  IrsEdge *saved_edges = e->edges;
  size_t saved_count = e->nedges, saved_cap = e->edge_cap;
  e->edges = NULL;
  e->nedges = 0;
  e->edge_cap = 0;
  e->reach = smt_true(ctx);
  for (size_t pc = e->inv_start[header]; pc < marker && !e->failed; pc++) {
    irs_step(e, pc);
  }
  SmtTerm value = irs_nonzero(ctx, irs_read(e, &e->insns[marker].arguments[0]));
  for (size_t k = 0; k < e->nedges; k++) {
    free(e->edges[k].env);
  }
  free(e->edges);
  e->edges = saved_edges;
  e->nedges = saved_count;
  e->edge_cap = saved_cap;
  memcpy(e->env, saved_env, e->nvars * sizeof(SmtTerm));
  free(saved_env);
  e->reach = saved_reach;
  e->trap = saved_trap;
  e->global_epoch = saved_epoch;
  return value;
}

static void irs_find_invariants(IrsEnc *e) {
  e->inv_marker = calloc(e->count ? e->count : 1u, sizeof(size_t));
  e->inv_start = calloc(e->count ? e->count : 1u, sizeof(size_t));
  for (size_t h = 0; h < e->count; h++) {
    size_t end = e->loop_end[h];
    if (end <= h) {
      continue;
    }
    size_t marker = 0;
    for (size_t pc = h + 1; pc <= end; pc++) {
      const IRInstruction *insn = &e->insns[pc];
      if (insn->op == IR_OP_CALL && insn->text &&
          strcmp(insn->text, IR_SMT_INVARIANT_CALL) == 0 &&
          insn->argument_count == 1) {
        marker = pc;
        break;
      }
    }
    if (!marker) {
      continue;
    }
    size_t start = h + 1;
    for (size_t pc = h + 1; pc < marker; pc++) {
      const IRInstruction *insn = &e->insns[pc];
      if ((insn->op == IR_OP_BRANCH_ZERO || insn->op == IR_OP_BRANCH_EQ ||
           insn->op == IR_OP_JUMP) &&
          insn->text) {
        size_t target = irs_label(e, insn->text);
        if (target != (size_t)-1 && target > end) {
          start = pc + 1;
        }
      }
    }
    for (size_t pc = start; pc < marker; pc++) {
      const IRInstruction *insn = &e->insns[pc];
      if (insn->dest.kind == IR_OPERAND_SYMBOL || insn->op == IR_OP_STORE) {
        e->failed = 1;
        snprintf(e->why, sizeof(e->why),
                 "an invariant has to be the first statement of its loop's "
                 "body");
        return;
      }
    }
    e->inv_marker[h] = marker;
    e->inv_start[h] = start;
  }
}

static void irs_call(IrsEnc *e, const IRInstruction *insn) {
  SmtCtx *ctx = e->ctx;
  if (insn->text && strcmp(insn->text, IR_SMT_INVARIANT_CALL) == 0) {
    return;
  }
  if (insn->text && strcmp(insn->text, IR_SMT_PROVE_CALL) == 0) {
    if (e->frame->record_proves && !e->prefix && insn->argument_count == 1) {
      SmtTerm cond = irs_nonzero(ctx, irs_read(e, &insn->arguments[0]));
      irs_record_site(e, e->reach, cond, &insn->location, IR_SMT_SITE_PROVE);
    }
    return;
  }
  if (irs_is_trap_call(insn->text)) {
    irs_trap(e, smt_true(ctx));
    e->reach = smt_false(ctx);
    return;
  }
  if (e->frame->uf_calls) {
    irs_call_uf(e, insn);
    return;
  }
  const IRFunction *callee = irs_find_function(e->frame->program, insn->text);
  if (!callee || callee->instruction_count == 0) {
    irs_fail(e, "it calls", insn->text ? insn->text : "an unknown function");
    return;
  }
  if (e->frame->inline_depth >= IRS_MAX_INLINE_DEPTH) {
    irs_fail(e, "calls nest deeper than the encoder inlines at", insn->text);
    return;
  }
  if (callee->parameter_count != insn->argument_count) {
    irs_fail(e, "a call passes the wrong number of arguments to", insn->text);
    return;
  }
  SmtTerm args[16];
  if (insn->argument_count > 16u) {
    irs_fail(e, "a call passes more than 16 arguments to", insn->text);
    return;
  }
  for (size_t i = 0; i < insn->argument_count; i++) {
    args[i] = irs_read(e, &insn->arguments[i]);
  }
  IrsEnc sub;
  memset(&sub, 0, sizeof(sub));
  sub.frame = e->frame;
  sub.ctx = ctx;
  sub.fn = callee;
  sub.insns = callee->instructions;
  sub.count = callee->instruction_count;
  sub.params = args;
  sub.mem_in = e->env[e->mem_id];
  char prefix[160];
  snprintf(prefix, sizeof(prefix), "%s%s@%zu/", e->prefix ? e->prefix : "",
           callee->name, (size_t)(insn - e->insns));
  sub.prefix = prefix;
  e->frame->inline_depth++;
  int ok = irs_run(&sub);
  e->frame->inline_depth--;
  if (!ok) {
    irs_fail(e, "a callee cannot be encoded:", sub.why);
  } else {
    irs_trap(e, sub.trap);
    irs_write(e, &insn->dest, sub.result);
    if (irs_returns_aggregate(callee)) {
      irs_mark_aggregate(e, &insn->dest);
    }
    e->env[e->mem_id] = sub.mem_out;
  }
  free(sub.vars);
  free(sub.buckets);
  free(sub.env);
  free(sub.loop_end);
  for (size_t k = 0; k < sub.nedges; k++) {
    free(sub.edges[k].env);
  }
  free(sub.edges);
}

static void irs_havoc_loop(IrsEnc *e, size_t header, size_t end) {
  SmtCtx *ctx = e->ctx;
  int calls = 0, stores = 0;
  for (size_t i = header; i <= end && i < e->count; i++) {
    if (e->insns[i].op == IR_OP_CALL && e->insns[i].text &&
        strcmp(e->insns[i].text, IR_SMT_PROVE_CALL) != 0 &&
        strcmp(e->insns[i].text, IR_SMT_INVARIANT_CALL) != 0 &&
        !irs_is_trap_call(e->insns[i].text)) {
      calls = 1;
    }
    stores |= e->insns[i].op == IR_OP_STORE;
    const IROperand *d = &e->insns[i].dest;
    if ((d->kind == IR_OPERAND_TEMP || d->kind == IR_OPERAND_SYMBOL) &&
        d->name) {
      stores |= e->vars[irs_var_lookup(e, d->name, 1)].addressed;
    }
  }
  if (calls) {
    e->global_epoch++;
  }
  if (calls || stores) {
    e->env[e->mem_id] = smt_fresh(ctx, "loop:memory", irs_memory_sort(ctx));
  }
  for (size_t i = header; i <= end && i < e->count; i++) {
    if (e->insns[i].op == IR_OP_STORE) {
      continue;
    }
    const IROperand *dest = &e->insns[i].dest;
    if ((dest->kind != IR_OPERAND_TEMP && dest->kind != IR_OPERAND_SYMBOL) ||
        !dest->name) {
      continue;
    }
    size_t id = irs_var_lookup(e, dest->name, 1);
    IrsVar *v = &e->vars[id];
    if (dest->kind == IR_OPERAND_SYMBOL && !v->declared && !v->param) {
      irs_fail(e, "a loop writes the global", dest->name);
      return;
    }
    char prefix[160];
    snprintf(prefix, sizeof(prefix), "loop:%s", dest->name);
    SmtTerm fresh = smt_fresh(ctx, prefix, irs_word(ctx));
    e->env[id] = v->size > 0 && v->size < 8
                     ? ir_smt_narrow(ctx, fresh, v->size, v->is_unsigned)
                     : fresh;
  }
}

static void irs_step(IrsEnc *e, size_t pc) {
  SmtCtx *ctx = e->ctx;
  const IRInstruction *insn = &e->insns[pc];
  if (insn->op == IR_OP_LABEL) {
    irs_merge(e, pc);
    if (e->frame->havoc_loops && e->loop_end && e->loop_end[pc] > pc &&
        e->reach != smt_false(ctx)) {
      size_t marker = e->inv_marker ? e->inv_marker[pc] : 0;
      if (marker) {
        SmtTerm entry = irs_eval_invariant(e, pc);
        irs_record_site(e, e->reach, entry, &e->insns[marker].location,
                        IR_SMT_SITE_INVARIANT_ENTRY);
      }
      irs_havoc_loop(e, pc, e->loop_end[pc]);
      if (marker) {
        e->reach = smt_and(ctx, e->reach, irs_eval_invariant(e, pc));
      }
    }
    return;
  }
  if (e->reach == smt_false(ctx) && insn->op != IR_OP_NOP) {
    return;
  }
  switch (insn->op) {
  case IR_OP_NOP:
    return;
  case IR_OP_JUMP:
    irs_edge(e, pc, insn->text, smt_true(ctx));
    e->reach = smt_false(ctx);
    return;
  case IR_OP_BRANCH_ZERO: {
    SmtTerm v = irs_read(e, &insn->lhs);
    SmtTerm zero = smt_eq(ctx, v, smt_bv_u64(ctx, 64, 0));
    irs_edge(e, pc, insn->text, zero);
    e->reach = smt_and(ctx, e->reach, smt_not(ctx, zero));
    return;
  }
  case IR_OP_BRANCH_EQ: {
    SmtTerm eq = smt_eq(ctx, irs_read(e, &insn->lhs), irs_read(e, &insn->rhs));
    irs_edge(e, pc, insn->text, eq);
    e->reach = smt_and(ctx, e->reach, smt_not(ctx, eq));
    return;
  }
  case IR_OP_DECLARE_LOCAL: {
    if (!insn->dest.name) {
      irs_fail(e, "a local has no name", NULL);
      return;
    }
    size_t id = irs_var_lookup(e, insn->dest.name, 1);
    int size, is_unsigned;
    if (!irs_parse_scalar(insn->text, &size, &is_unsigned)) {
      uint64_t bytes = irs_aggregate_size(insn);
      if (!bytes) {
        irs_fail(e, "it declares a local of type", insn->text);
        return;
      }
      SmtTerm base =
          irs_region(e, insn->dest.name, bytes, !e->vars[id].escaped);
      e->vars[id].addr = base;
      e->vars[id].region_size = bytes;
      e->vars[id].size = 8;
      e->vars[id].is_unsigned = 1;
      e->env[id] = base;
      return;
    }
    e->vars[id].size = size;
    e->vars[id].is_unsigned = is_unsigned;
    if (e->vars[id].addressed) {
      e->vars[id].addr = irs_region(e, insn->dest.name, (uint64_t)size,
                                    !e->vars[id].escaped);
      e->vars[id].region_size = (uint64_t)size;
      e->vars[id].slot = 1;
      return;
    }
    e->env[id] = irs_shared(e, "undefined", insn->dest.name);
    return;
  }
  case IR_OP_ASSIGN: {
    IrsVar *d = (insn->dest.kind == IR_OPERAND_TEMP ||
                 insn->dest.kind == IR_OPERAND_SYMBOL) &&
                        insn->dest.name
                    ? &e->vars[irs_var_lookup(e, insn->dest.name, 1)]
                    : NULL;
    if (d && ((d->addr && !d->slot) || (d->param && d->aggregate))) {
      SmtTerm base =
          d->addr ? d->addr : e->env[irs_var_lookup(e, insn->dest.name, 1)];
      SmtTerm source = irs_read(e, &insn->lhs);
      if (irs_holds_aggregate(e, &insn->lhs) || d->region_size > 8u) {
        irs_copy_bytes(e, base, source, (long long)d->region_size);
      } else {
        irs_store_bytes(e, base, (int)d->region_size, source);
      }
      return;
    }
    if (d && irs_holds_aggregate(e, &insn->lhs)) {
      d->aggregate = 1;
    }
    irs_write(e, &insn->dest, irs_read(e, &insn->lhs));
    return;
  }
  case IR_OP_BINARY: {
    if (insn->is_float || !insn->text) {
      irs_fail(e, "it computes with floats", NULL);
      return;
    }
    SmtTerm a = irs_read(e, &insn->lhs), b = irs_read(e, &insn->rhs);
    SmtTerm trap;
    SmtTerm v = ir_smt_binary(ctx, insn->text, insn->is_unsigned, a, b, &trap);
    if (!v) {
      irs_fail(e, "it uses the operator", insn->text);
      return;
    }
    irs_trap(e, trap);
    irs_write(e, &insn->dest, v);
    return;
  }
  case IR_OP_UNARY: {
    if (insn->is_float || !insn->text) {
      irs_fail(e, "it computes with floats", NULL);
      return;
    }
    SmtTerm a = irs_read(e, &insn->lhs), v;
    if (strcmp(insn->text, "-") == 0) {
      v = smt_mk1(ctx, SMT_K_BVNEG, a);
    } else if (strcmp(insn->text, "~") == 0) {
      v = smt_mk1(ctx, SMT_K_BVNOT, a);
    } else if (strcmp(insn->text, "!") == 0) {
      v = irs_bool_word(ctx, smt_eq(ctx, a, smt_bv_u64(ctx, 64, 0)));
    } else if (strcmp(insn->text, "+") == 0) {
      v = a;
    } else {
      irs_fail(e, "it uses the unary operator", insn->text);
      return;
    }
    irs_write(e, &insn->dest, v);
    return;
  }
  case IR_OP_CAST: {
    const char *type = insn->text ? insn->text : "";
    SmtTerm a = irs_read(e, &insn->lhs);
    size_t len = strlen(type);
    int size, is_unsigned;
    if (insn->is_float) {
      irs_fail(e, "it casts a float", NULL);
      return;
    }
    if ((len > 0 && type[len - 1] == '*') || strcmp(type, "cstring") == 0 ||
        strcmp(type, "rawptr") == 0 || strncmp(type, "fn(", 3) == 0) {
      irs_write(e, &insn->dest, a);
      return;
    }
    if (strcmp(type, "bool") == 0) {
      irs_write(e, &insn->dest, irs_bool_word(ctx, irs_nonzero(ctx, a)));
      return;
    }
    if (irs_parse_scalar(type, &size, &is_unsigned)) {
      irs_write(e, &insn->dest, ir_smt_narrow(ctx, a, size, is_unsigned));
      return;
    }
    if (insn->value_type && insn->value_type->kind == MTLC_TYPE_ENUM) {
      size_t s = insn->value_type->size;
      int bytes = (s == 1 || s == 2 || s == 8) ? (int)s : 4;
      irs_write(e, &insn->dest, ir_smt_narrow(ctx, a, bytes, 0));
      return;
    }
    irs_fail(e, "it casts to", type);
    return;
  }
  case IR_OP_SELECT: {
    SmtTerm truth;
    if (insn->text && insn->argument_count > 1) {
      int ok;
      truth = irs_compare(ctx, insn->text, insn->is_unsigned,
                          irs_read(e, &insn->lhs),
                          irs_read(e, &insn->arguments[1]), &ok);
      if (!ok) {
        irs_fail(e, "a select compares with", insn->text);
        return;
      }
    } else {
      truth = irs_nonzero(ctx, irs_read(e, &insn->lhs));
    }
    if (insn->argument_count == 0) {
      irs_fail(e, "a select has no false value", NULL);
      return;
    }
    SmtTerm v = smt_ite(ctx, truth, irs_read(e, &insn->rhs),
                        irs_read(e, &insn->arguments[0]));
    irs_write(e, &insn->dest, v);
    return;
  }
  case IR_OP_RETURN: {
    SmtTerm v = insn->lhs.kind == IR_OPERAND_NONE ? smt_bv_u64(ctx, 64, 0)
                                                  : irs_read(e, &insn->lhs);
    e->result = smt_ite(ctx, e->reach, v, e->result);
    e->mem_out = smt_ite(ctx, e->reach, e->env[e->mem_id], e->mem_out);
    e->returned = smt_or(ctx, e->returned, e->reach);
    e->reach = smt_false(ctx);
    return;
  }
  case IR_OP_CALL:
    irs_call(e, insn);
    return;
  case IR_OP_LOAD: {
    int size = irs_access_size(e, insn);
    if (!size) {
      return;
    }
    SmtTerm addr = irs_read(e, &insn->lhs);
    irs_trap(e, irs_access_trap(e, addr, size));
    irs_write(e, &insn->dest,
              irs_load_bytes(e, addr, size, insn->is_unsigned));
    return;
  }
  case IR_OP_STORE: {
    if (insn->rhs.kind == IR_OPERAND_INT && !insn->is_float &&
        (irs_holds_aggregate(e, &insn->lhs) ||
         (insn->rhs.int_value != 1 && insn->rhs.int_value != 2 &&
          insn->rhs.int_value != 4 && insn->rhs.int_value != 8))) {
      SmtTerm dst = irs_read(e, &insn->dest);
      SmtTerm src = irs_read(e, &insn->lhs);
      irs_copy_bytes(e, dst, src, insn->rhs.int_value);
      return;
    }
    int size = irs_access_size(e, insn);
    if (!size) {
      return;
    }
    SmtTerm addr = irs_read(e, &insn->dest);
    SmtTerm value = irs_read(e, &insn->lhs);
    irs_trap(e, irs_access_trap(e, addr, size));
    irs_store_bytes(e, addr, size, value);
    return;
  }
  case IR_OP_ADDRESS_OF: {
    if (insn->lhs.kind != IR_OPERAND_SYMBOL || !insn->lhs.name) {
      irs_fail(e, "it takes an address the encoder does not model", NULL);
      return;
    }
    size_t id = irs_var_lookup(e, insn->lhs.name, 1);
    IrsVar *v = &e->vars[id];
    if (v->slot) {
      irs_write(e, &insn->dest, v->addr);
      return;
    }
    if ((v->addr || (v->aggregate && v->param)) && e->env[id]) {
      irs_write(e, &insn->dest, e->env[id]);
      return;
    }
    irs_fail(e, "it takes the address of", insn->lhs.name);
    return;
  }
  default:
    irs_fail(e, "it uses the instruction", ir_opcode_name(insn->op));
    return;
  }
}

static size_t irs_origin(IrsEnc *e, const IROperand *op) {
  if ((op->kind != IR_OPERAND_TEMP && op->kind != IR_OPERAND_SYMBOL) ||
      !op->name) {
    return 0;
  }
  return e->vars[irs_var_lookup(e, op->name, 1)].origin;
}

static int irs_is_region_decl(IrsEnc *e, const IROperand *op) {
  if ((op->kind != IR_OPERAND_TEMP && op->kind != IR_OPERAND_SYMBOL) ||
      !op->name) {
    return 0;
  }
  size_t id = irs_var_lookup(e, op->name, 1);
  return e->vars[id].origin == id + 1u;
}

static void irs_escape(IrsEnc *e, size_t origin) {
  if (origin) {
    e->vars[origin - 1u].escaped = 1;
  }
}

static int irs_flow(IrsEnc *e, const IROperand *dest, size_t origin) {
  if (!origin || (dest->kind != IR_OPERAND_TEMP &&
                  dest->kind != IR_OPERAND_SYMBOL) ||
      !dest->name) {
    return 0;
  }
  IrsVar *d = &e->vars[irs_var_lookup(e, dest->name, 1)];
  if (d->origin == origin) {
    return 0;
  }
  if (d->origin) {
    irs_escape(e, d->origin);
    irs_escape(e, origin);
    return 0;
  }
  d->origin = origin;
  return 1;
}

static int irs_is_compare(const char *op) {
  static const char *const ops[] = {"==", "!=", "<", "<=", ">", ">="};
  for (size_t i = 0; op && i < sizeof(ops) / sizeof(ops[0]); i++) {
    if (strcmp(op, ops[i]) == 0) {
      return 1;
    }
  }
  return 0;
}

static void irs_find_escapes(IrsEnc *e) {
  for (size_t i = 0; i < e->count; i++) {
    const IRInstruction *insn = &e->insns[i];
    int size, is_unsigned;
    if (insn->op == IR_OP_ADDRESS_OF && insn->lhs.kind == IR_OPERAND_SYMBOL &&
        insn->lhs.name) {
      size_t id = irs_var_lookup(e, insn->lhs.name, 1);
      irs_flow(e, &insn->dest, id + 1u);
    } else if (insn->op == IR_OP_DECLARE_LOCAL && insn->dest.name &&
               !irs_parse_scalar(insn->text, &size, &is_unsigned)) {
      size_t id = irs_var_lookup(e, insn->dest.name, 1);
      e->vars[id].origin = id + 1u;
    }
  }
  int changed = 1;
  for (int round = 0; changed && round < 8; round++) {
    changed = 0;
    for (size_t i = 0; i < e->count; i++) {
      const IRInstruction *insn = &e->insns[i];
      switch (insn->op) {
      case IR_OP_ASSIGN:
      case IR_OP_CAST:
        if (irs_is_region_decl(e, &insn->dest)) {
          break;
        }
        changed |= irs_flow(e, &insn->dest, irs_origin(e, &insn->lhs));
        break;
      case IR_OP_BINARY:
        if (insn->text && (strcmp(insn->text, "+") == 0 ||
                           strcmp(insn->text, "-") == 0)) {
          size_t a = irs_origin(e, &insn->lhs), b = irs_origin(e, &insn->rhs);
          if (a && b) {
            irs_escape(e, a);
            irs_escape(e, b);
          }
          changed |= irs_flow(e, &insn->dest, a ? a : b);
        }
        break;
      case IR_OP_SELECT:
        changed |= irs_flow(e, &insn->dest, irs_origin(e, &insn->rhs));
        if (insn->argument_count) {
          changed |=
              irs_flow(e, &insn->dest, irs_origin(e, &insn->arguments[0]));
        }
        break;
      default:
        break;
      }
    }
  }
  for (size_t i = 0; i < e->count; i++) {
    const IRInstruction *insn = &e->insns[i];
    switch (insn->op) {
    case IR_OP_LOAD:
    case IR_OP_ADDRESS_OF:
    case IR_OP_DECLARE_LOCAL:
    case IR_OP_ASSIGN:
    case IR_OP_CAST:
    case IR_OP_BRANCH_ZERO:
    case IR_OP_BRANCH_EQ:
    case IR_OP_LABEL:
    case IR_OP_JUMP:
    case IR_OP_NOP:
      break;
    case IR_OP_STORE:
      if (insn->rhs.kind == IR_OPERAND_INT &&
          (irs_is_region_decl(e, &insn->lhs) ||
           (insn->rhs.int_value != 1 && insn->rhs.int_value != 2 &&
            insn->rhs.int_value != 4 && insn->rhs.int_value != 8))) {
        break;
      }
      irs_escape(e, irs_origin(e, &insn->lhs));
      break;
    case IR_OP_BINARY:
      if (!insn->text || (strcmp(insn->text, "+") != 0 &&
                          strcmp(insn->text, "-") != 0 &&
                          !irs_is_compare(insn->text))) {
        irs_escape(e, irs_origin(e, &insn->lhs));
        irs_escape(e, irs_origin(e, &insn->rhs));
      }
      break;
    case IR_OP_SELECT:
      break;
    case IR_OP_CALL:
      if (insn->text && (strcmp(insn->text, IR_SMT_PROVE_CALL) == 0 ||
                         strcmp(insn->text, IR_SMT_INVARIANT_CALL) == 0)) {
        break;
      }
      for (size_t a = 0; a < insn->argument_count; a++) {
        irs_escape(e, irs_origin(e, &insn->arguments[a]));
      }
      break;
    default:
      irs_escape(e, irs_origin(e, &insn->dest));
      irs_escape(e, irs_origin(e, &insn->lhs));
      irs_escape(e, irs_origin(e, &insn->rhs));
      for (size_t a = 0; a < insn->argument_count; a++) {
        irs_escape(e, irs_origin(e, &insn->arguments[a]));
      }
      break;
    }
  }
}

static int irs_run(IrsEnc *e) {
  SmtCtx *ctx = e->ctx;
  const IRFunction *fn = e->fn;
  for (size_t i = 0; i < fn->parameter_count; i++) {
    if (!fn->parameter_names[i]) {
      irs_fail(e, "a parameter has no name", NULL);
      return 0;
    }
    size_t id = irs_var_lookup(e, fn->parameter_names[i], 1);
    int size = 8, is_unsigned = 0;
    const char *type = fn->parameter_types ? fn->parameter_types[i] : NULL;
    if (type && !irs_parse_scalar(type, &size, &is_unsigned)) {
      const MtlcType *pt =
          e->frame->program ? ir_program_lookup_type(e->frame->program, type)
                            : NULL;
      if (!pt || pt->size == 0 ||
          (pt->kind != MTLC_TYPE_STRUCT && pt->kind != MTLC_TYPE_ARRAY &&
           pt->kind != MTLC_TYPE_TAGGED_ENUM)) {
        irs_fail(e, "a parameter has type", type);
        return 0;
      }
      size = 8;
      is_unsigned = 1;
      e->vars[id].aggregate = 1;
      e->vars[id].region_size = (uint64_t)pt->size;
    }
    e->vars[id].param = 1;
    e->vars[id].size = size;
    e->vars[id].is_unsigned = is_unsigned;
  }
  for (size_t i = 0; i < e->count; i++) {
    const IRInstruction *insn = &e->insns[i];
    irs_note_operand(e, &insn->dest);
    irs_note_operand(e, &insn->lhs);
    irs_note_operand(e, &insn->rhs);
    for (size_t a = 0; a < insn->argument_count; a++) {
      irs_note_operand(e, &insn->arguments[a]);
    }
    if (insn->op == IR_OP_DECLARE_LOCAL && insn->dest.name) {
      e->vars[irs_var_lookup(e, insn->dest.name, 1)].declared = 1;
    }
    if (insn->op == IR_OP_ADDRESS_OF && insn->lhs.kind == IR_OPERAND_SYMBOL &&
        insn->lhs.name) {
      e->vars[irs_var_lookup(e, insn->lhs.name, 1)].addressed = 1;
    }
  }
  if (!e->frame->assume) {
    e->frame->assume = smt_true(ctx);
  }
  irs_find_escapes(e);
  e->mem_id = irs_var_lookup(e, "\x01memory", 1);
  e->vars[e->mem_id].declared = 1;
  e->env = calloc(e->nvars ? e->nvars : 1u, sizeof(SmtTerm));
  e->env[e->mem_id] =
      e->mem_in ? e->mem_in
                : irs_shared_sorted(e, "memory", "", irs_memory_sort(ctx));
  e->mem_out = e->env[e->mem_id];
  if (e->frame->havoc_loops && !e->prefix) {
    e->loop_end = calloc(e->count ? e->count : 1u, sizeof(size_t));
    for (size_t pc = 0; pc < e->count; pc++) {
      const IRInstruction *insn = &e->insns[pc];
      if ((insn->op != IR_OP_JUMP && insn->op != IR_OP_BRANCH_ZERO &&
           insn->op != IR_OP_BRANCH_EQ) ||
          !insn->text) {
        continue;
      }
      size_t target = irs_label(e, insn->text);
      if (target != (size_t)-1 && target <= pc && e->loop_end[target] < pc) {
        e->loop_end[target] = pc;
      }
    }
    if (e->frame->record_proves) {
      irs_find_invariants(e);
    }
  }
  for (size_t i = 0; i < fn->parameter_count; i++) {
    size_t id = irs_var_lookup(e, fn->parameter_names[i], 0);
    e->env[id] = ir_smt_narrow(ctx, e->params[i], e->vars[id].size,
                               e->vars[id].is_unsigned);
    if (e->vars[id].addressed && !e->vars[id].aggregate) {
      e->vars[id].addr =
          irs_region(e, fn->parameter_names[i], (uint64_t)e->vars[id].size,
                     !e->vars[id].escaped);
      e->vars[id].region_size = (uint64_t)e->vars[id].size;
      irs_store_bytes(e, e->vars[id].addr, e->vars[id].size, e->env[id]);
      e->vars[id].slot = 1;
      e->env[id] = 0;
    }
  }
  e->reach = smt_true(ctx);
  e->result = smt_bv_u64(ctx, 64, 0);
  e->trap = smt_false(ctx);
  e->returned = smt_false(ctx);
  for (size_t pc = 0; pc < e->count && !e->failed; pc++) {
    irs_step(e, pc);
    const char *err;
    if (smt_ctx_error(ctx, &err)) {
      irs_fail(e, "the encoder built an ill-sorted term:", err);
    }
  }
  if (!e->failed && e->nedges) {
    irs_fail(e, "a jump never reaches its label", NULL);
  }
  if (!e->failed) {
    e->mem_out = smt_ite(ctx, e->reach, e->env[e->mem_id], e->mem_out);
    e->returned = smt_or(ctx, e->returned, e->reach);
  }
  return !e->failed;
}

void ir_smt_frame_init(IRSmtFrame *frame, SmtCtx *ctx, const IRFunction *fn,
                       const IRProgram *program) {
  memset(frame, 0, sizeof(*frame));
  frame->ctx = ctx;
  frame->program = program;
  frame->param_count = fn ? fn->parameter_count : 0;
  frame->params = calloc(frame->param_count ? frame->param_count : 1u,
                         sizeof(SmtTerm));
  for (size_t i = 0; i < frame->param_count; i++) {
    char name[160];
    snprintf(name, sizeof(name), "%s",
             fn->parameter_names[i] ? fn->parameter_names[i] : "p");
    frame->params[i] = smt_var(ctx, name, smt_bv_sort(ctx, 64));
  }
}

void ir_smt_frame_free(IRSmtFrame *frame) {
  for (size_t i = 0; i < frame->shared_count; i++) {
    mettle_free_string((char *)frame->shared[i].name);
  }
  free(frame->shared);
  free(frame->params);
  free(frame->calls);
  free(frame->proves);
  free(frame->region_base);
  free(frame->region_size);
  free(frame->region_private);
  free(frame->access_addr);
  free(frame->access_size);
  free(frame->foreign);
  memset(frame, 0, sizeof(*frame));
}

int ir_smt_encode(IRSmtFrame *frame, const IRFunction *fn,
                  const IRInstruction *insns, size_t count,
                  IRSmtOutcome *out) {
  IrsEnc e;
  memset(&e, 0, sizeof(e));
  e.frame = frame;
  e.ctx = frame->ctx;
  e.fn = fn;
  e.insns = insns;
  e.count = count;
  e.params = frame->params;
  int ok = irs_run(&e);
  out->result = e.result;
  out->trap = e.trap;
  out->returned = e.returned;
  out->memory = e.mem_out;
  snprintf(out->why, sizeof(out->why), "%s", e.why);
  free(e.vars);
  free(e.buckets);
  free(e.env);
  free(e.loop_end);
  free(e.inv_marker);
  free(e.inv_start);
  for (size_t k = 0; k < e.nedges; k++) {
    free(e.edges[k].env);
  }
  free(e.edges);
  return ok;
}

void ir_smt_check_equivalence(const IRProgram *program, const IRFunction *a,
                              const IRInstruction *a_insns, size_t a_count,
                              const IRFunction *b,
                              const IRInstruction *b_insns, size_t b_count,
                              const IRFunction *guard, IRSmtReport *report) {
  memset(report, 0, sizeof(*report));
  report->verdict = IR_SMT_UNKNOWN;
  if (a->parameter_count != b->parameter_count ||
      (guard && guard->parameter_count != a->parameter_count)) {
    snprintf(report->why, sizeof(report->why),
             "the two sides take different parameters");
    return;
  }
  double t0 = mettle_now_ms();
  SmtCtx *ctx = smt_ctx_new();
  IRSmtFrame frame;
  ir_smt_frame_init(&frame, ctx, a, program);
  IRSmtOutcome oa, ob, og;
  if (!ir_smt_encode(&frame, a, a_insns, a_count, &oa)) {
    snprintf(report->why, sizeof(report->why), "%s", oa.why);
    goto done;
  }
  if (!ir_smt_encode(&frame, b, b_insns, b_count, &ob)) {
    snprintf(report->why, sizeof(report->why), "%s", ob.why);
    goto done;
  }
  SmtTerm assume = smt_true(ctx);
  if (guard) {
    if (!ir_smt_encode(&frame, guard, guard->instructions,
                       guard->instruction_count, &og)) {
      snprintf(report->why, sizeof(report->why), "the guard: %s", og.why);
      goto done;
    }
    assume = smt_and(ctx, smt_not(ctx, og.trap),
                     irs_nonzero(ctx, og.result));
  }
  assume = smt_and(ctx, assume, ir_smt_frame_assumption(&frame));
  SmtTerm same_trap = smt_iff(ctx, oa.trap, ob.trap);
  SmtTerm same_result = smt_eq(ctx, oa.result, ob.result);
  int frames_dead = !irs_returns_aggregate(a) && !irs_returns_aggregate(b);
  if (oa.memory != ob.memory && frame.region_count && frames_dead) {
    IrsMemo memo;
    memset(&memo, 0, sizeof(memo));
    oa.memory = irs_strip_dead_stores(&frame, ctx, oa.memory, &memo);
    ob.memory = irs_strip_dead_stores(&frame, ctx, ob.memory, &memo);
    free(memo.from);
    free(memo.to);
  }
  if (oa.memory != ob.memory) {
    SmtTerm k = smt_fresh(ctx, "address", smt_bv_sort(ctx, 64));
    SmtTerm live = smt_true(ctx);
    for (size_t i = 0; frames_dead && i < frame.region_count; i++) {
      SmtTerm base = frame.region_base[i];
      SmtTerm last = smt_mk2(ctx, SMT_K_BVADD, base,
                             smt_bv_u64(ctx, 64, frame.region_size[i] - 1u));
      live = smt_and(ctx, live,
                     smt_or(ctx, smt_mk2(ctx, SMT_K_BVULT, k, base),
                            smt_mk2(ctx, SMT_K_BVULT, last, k)));
    }
    same_result = smt_and(
        ctx, same_result,
        smt_implies(ctx, live,
                    smt_eq(ctx, smt_select(ctx, oa.memory, k),
                           smt_select(ctx, ob.memory, k))));
  }
  SmtTerm same_value = smt_implies(ctx, smt_not(ctx, oa.trap), same_result);
  SmtTerm claim = smt_implies(ctx, assume, smt_and(ctx, same_trap, same_value));
  SmtSolver *solver = NULL;
  SmtResult r = smt_prove(ctx, claim, 1, 10000, &solver);
  SmtStats st;
  smt_solver_stats(solver, &st);
  report->clauses = st.clauses;
  report->conflicts = st.conflicts;
  report->lemmas = st.lemmas_checked;
  if (r == SMT_UNSAT) {
    report->verdict = IR_SMT_PROVED;
  } else if (r == SMT_SAT) {
    report->verdict = IR_SMT_REFUTED;
    report->input_count = frame.param_count < 16u ? frame.param_count : 16u;
    for (size_t i = 0; i < report->input_count; i++) {
      uint64_t w[4];
      smt_solver_value(solver, frame.params[i], w);
      report->inputs[i] = (long long)w[0];
    }
  } else {
    snprintf(report->why, sizeof(report->why), "%s",
             smt_solver_reason(solver) ? smt_solver_reason(solver)
                                       : "the solver gave up");
  }
  smt_solver_free(solver);
done:
  report->seconds = (mettle_now_ms() - t0) / 1000.0;
  ir_smt_frame_free(&frame);
  smt_ctx_free(ctx);
}

static const char *irs_pattern_text(int kind, long long value, char *buf,
                                    size_t cap) {
  switch (kind) {
  case IR_IDPAT_VAR:
    return "x";
  case IR_IDPAT_INT:
    snprintf(buf, cap, "%lld", value);
    return buf;
  case IR_IDPAT_NONZERO:
    return "c!=0";
  case IR_IDPAT_POW2:
    return "2^k";
  case IR_IDPAT_SAME:
    return "x";
  case IR_IDPAT_NONNEG:
    return "x>=0";
  default:
    return "?";
  }
}

static const char *irs_action_text(const IRIdentityRow *row, char *buf,
                                   size_t cap) {
  switch (row->act_kind) {
  case IR_IDACT_KEEP:
    return "x";
  case IR_IDACT_CONST:
    snprintf(buf, cap, "%lld", row->act_value);
    return buf;
  case IR_IDACT_SHL:
    return "x << k";
  case IR_IDACT_SHR:
    return "x >> k";
  case IR_IDACT_AND_MASK:
    return "x & (2^k - 1)";
  default:
    return "?";
  }
}

static SmtTerm irs_operand_for(SmtCtx *ctx, int kind, long long value,
                               SmtTerm x, SmtTerm *assume, SmtTerm *shift,
                               int tag, long long kval) {
  SmtSort word = smt_bv_sort(ctx, 64);
  switch (kind) {
  case IR_IDPAT_VAR:
    return x;
  case IR_IDPAT_SAME:
    return x;
  case IR_IDPAT_INT:
    return smt_bv_i64(ctx, 64, value);
  case IR_IDPAT_NONZERO: {
    SmtTerm c = smt_var(ctx, tag ? "c1" : "c0", word);
    *assume = smt_and(ctx, *assume,
                      smt_not(ctx, smt_eq(ctx, c, smt_bv_u64(ctx, 64, 0))));
    return c;
  }
  case IR_IDPAT_POW2: {
    SmtTerm k = smt_bv_u64(ctx, 64, (uint64_t)kval);
    *shift = k;
    return smt_mk2(ctx, SMT_K_BVSHL, smt_bv_u64(ctx, 64, 1), k);
  }
  case IR_IDPAT_NONNEG:
    *assume = smt_and(ctx, *assume,
                      smt_mk2(ctx, SMT_K_BVSLE, smt_bv_u64(ctx, 64, 0), x));
    return x;
  default:
    return x;
  }
}

int ir_smt_prove_identity_table(FILE *out, int verbose) {
  size_t rows = ir_rewrite_identity_count();
  int failures = 0;
  size_t obligations = 0;
  double t0 = mettle_now_ms();
  for (size_t k = 0; k < rows; k++) {
    IRIdentityRow row;
    ir_rewrite_identity_row(k, &row);
    char abuf[32], bbuf[32], actbuf[32];
    const char *a_text =
        irs_pattern_text(row.a_kind, row.a_value, abuf, sizeof(abuf));
    const char *b_text =
        irs_pattern_text(row.b_kind, row.b_value, bbuf, sizeof(bbuf));
    if (row.a_kind != IR_IDPAT_VAR && row.a_kind != IR_IDPAT_NONNEG &&
        row.b_kind == IR_IDPAT_VAR) {
      a_text = irs_pattern_text(row.a_kind, row.a_value, abuf, sizeof(abuf));
      b_text = "x";
    }
    int row_ok = 1;
    char detail[256] = "";
    int has_pow2 = row.a_kind == IR_IDPAT_POW2 || row.b_kind == IR_IDPAT_POW2;
    long long k_last = has_pow2 ? 62 : 1;
    for (long long kval = 1; kval <= k_last; kval++)
    for (int sign = 0; sign < 2; sign++) {
      for (int swapped = 0; swapped < (row.commutative ? 2 : 1); swapped++) {
        SmtCtx *ctx = smt_ctx_new();
        SmtSort word = smt_bv_sort(ctx, 64);
        SmtTerm assume = smt_true(ctx);
        SmtTerm shift = 0;
        int var_on_left = row.a_kind == IR_IDPAT_VAR ||
                          row.a_kind == IR_IDPAT_NONNEG ||
                          row.b_kind != IR_IDPAT_VAR;
        SmtTerm x = smt_var(ctx, "x", word);
        SmtTerm av = irs_operand_for(ctx, row.a_kind, row.a_value, x, &assume,
                                     &shift, 0, kval);
        SmtTerm bv = irs_operand_for(ctx, row.b_kind, row.b_value, x, &assume,
                                     &shift, 1, kval);
        SmtTerm lhs = swapped ? bv : av;
        SmtTerm rhs = swapped ? av : bv;
        SmtTerm trap;
        SmtTerm before = ir_smt_binary(ctx, row.op, sign, lhs, rhs, &trap);
        SmtTerm var_operand = var_on_left ? av : bv;
        SmtTerm after = 0;
        switch (row.act_kind) {
        case IR_IDACT_KEEP:
          after = av;
          break;
        case IR_IDACT_CONST:
          after = smt_bv_i64(ctx, 64, row.act_value);
          break;
        case IR_IDACT_SHL:
          after = ir_smt_binary(ctx, "<<", sign, av, shift, NULL);
          break;
        case IR_IDACT_SHR:
          after = ir_smt_binary(ctx, ">>", sign, av, shift, NULL);
          break;
        case IR_IDACT_AND_MASK:
          after = smt_mk2(ctx, SMT_K_BVAND, av,
                          smt_mk2(ctx, SMT_K_BVSUB, bv, smt_bv_u64(ctx, 64, 1)));
          break;
        }
        (void)var_operand;
        obligations++;
        if (!before || !after) {
          row_ok = 0;
          snprintf(detail, sizeof(detail), "operator %s is not modelled",
                   row.op);
          smt_ctx_free(ctx);
          continue;
        }
        SmtTerm claim = smt_implies(
            ctx, assume,
            smt_and(ctx, smt_not(ctx, trap), smt_eq(ctx, before, after)));
        SmtSolver *solver = NULL;
        SmtResult r = smt_prove(ctx, claim, 1, 500000, &solver);
        if (r != SMT_UNSAT) {
          row_ok = 0;
          if (r == SMT_SAT) {
            uint64_t w[4];
            smt_solver_value(solver, x, w);
            snprintf(detail, sizeof(detail),
                     "%s, %s: fails at x = %lld%s",
                     sign ? "unsigned" : "signed",
                     swapped ? "operands swapped" : "as written",
                     (long long)w[0], has_pow2 ? " for that k" : "");
          } else {
            snprintf(detail, sizeof(detail), "%s",
                     smt_solver_reason(solver) ? smt_solver_reason(solver)
                                               : "unknown");
          }
        }
        smt_solver_free(solver);
        smt_ctx_free(ctx);
      }
    }
    if (!row_ok) {
      failures++;
    }
    if (verbose || !row_ok) {
      fprintf(out, "  %-8s %s %s %s  =>  %-14s %s%s%s\n",
              row_ok ? "proved" : "REFUTED", a_text, row.op, b_text,
              irs_action_text(&row, actbuf, sizeof(actbuf)),
              row.commutative ? "(both orders) " : "",
              row_ok ? "" : ": ", detail);
    }
  }
  fprintf(out,
          "%zu identities, %zu obligations over 64-bit signed and unsigned "
          "operands, %d refuted, %.1f ms\n",
          rows, obligations, failures, mettle_now_ms() - t0);
  return failures == 0;
}
