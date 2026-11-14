#include "smt_sat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CREF_NONE 0xffffffffu
#define WATCH_BINARY 0x80000000u
#define HEADER_WORDS 3u
#define FLAG_LEARNT 1u
#define FLAG_DELETED 2u

typedef struct {
  uint32_t cref;
  SatLit blocker;
} Watcher;

typedef struct {
  Watcher *items;
  uint32_t count;
  uint32_t cap;
} WatchList;

typedef struct {
  uint32_t *items;
  size_t count;
  size_t cap;
} U32Vec;

struct SatSolver {
  uint32_t vars;
  uint32_t var_cap;
  int8_t *lit_val;
  uint32_t *level;
  uint32_t *reason;
  double *activity;
  uint8_t *polarity;
  uint8_t *seen;
  uint32_t *heap;
  int32_t *heap_index;
  uint32_t heap_size;
  WatchList *watches;
  uint32_t *arena;
  size_t arena_size;
  size_t arena_cap;
  size_t arena_wasted;
  U32Vec clauses;
  U32Vec learnts;
  SatLit *trail;
  uint32_t trail_size;
  uint32_t qhead;
  uint32_t *trail_lim;
  uint32_t decision_level;
  double var_inc;
  double var_decay;
  float cla_inc;
  int unsat;
  U32Vec learnt_buf;
  U32Vec stack;
  U32Vec to_clear;
  uint32_t *level_stamp;
  uint32_t level_stamp_cap;
  uint32_t stamp;
  double ema_fast;
  double ema_slow;
  uint64_t conflicts_since_restart;
  uint64_t next_reduce;
  uint64_t reduce_increment;
  uint32_t simplify_trail;
  int proof_enabled;
  U32Vec originals;
  U32Vec proof;
  SatStats stats;
};

static void vec_push(U32Vec *v, uint32_t x) {
  if (v->count == v->cap) {
    v->cap = v->cap ? v->cap * 2u : 64u;
    v->items = realloc(v->items, v->cap * sizeof(uint32_t));
  }
  v->items[v->count++] = x;
}

static void watch_push(WatchList *w, uint32_t cref, SatLit blocker) {
  if (w->count == w->cap) {
    w->cap = w->cap ? w->cap * 2u : 4u;
    w->items = realloc(w->items, w->cap * sizeof(Watcher));
  }
  w->items[w->count].cref = cref;
  w->items[w->count].blocker = blocker;
  w->count++;
}

static inline uint32_t clause_size(const SatSolver *s, uint32_t c) {
  return s->arena[c] >> 3;
}
static inline uint32_t clause_flags(const SatSolver *s, uint32_t c) {
  return s->arena[c] & 7u;
}
static inline SatLit *clause_lits(SatSolver *s, uint32_t c) {
  return &s->arena[c + HEADER_WORDS];
}
static inline float *clause_activity(SatSolver *s, uint32_t c) {
  return (float *)&s->arena[c + 2];
}

static inline int lit_value(const SatSolver *s, SatLit l) {
  return s->lit_val[l];
}

SatSolver *sat_new(void) {
  SatSolver *s = calloc(1, sizeof(SatSolver));
  s->var_inc = 1.0;
  s->var_decay = 0.95;
  s->cla_inc = 1.0f;
  s->next_reduce = 2000;
  s->reduce_increment = 300;
  return s;
}

void sat_free(SatSolver *s) {
  if (!s) {
    return;
  }
  for (uint32_t i = 0; i < s->vars * 2u; i++) {
    free(s->watches[i].items);
  }
  free(s->watches);
  free(s->lit_val);
  free(s->level);
  free(s->reason);
  free(s->activity);
  free(s->polarity);
  free(s->seen);
  free(s->heap);
  free(s->heap_index);
  free(s->arena);
  free(s->clauses.items);
  free(s->learnts.items);
  free(s->trail);
  free(s->trail_lim);
  free(s->learnt_buf.items);
  free(s->stack.items);
  free(s->to_clear.items);
  free(s->level_stamp);
  free(s->originals.items);
  free(s->proof.items);
  free(s);
}

void sat_enable_proof(SatSolver *s) { s->proof_enabled = 1; }

uint32_t sat_var_count(const SatSolver *s) { return s->vars; }

static int heap_less(const SatSolver *s, uint32_t a, uint32_t b) {
  return s->activity[a] > s->activity[b];
}

static void heap_up(SatSolver *s, uint32_t i) {
  uint32_t v = s->heap[i];
  while (i > 0) {
    uint32_t p = (i - 1u) >> 1;
    if (!heap_less(s, v, s->heap[p])) {
      break;
    }
    s->heap[i] = s->heap[p];
    s->heap_index[s->heap[i]] = (int32_t)i;
    i = p;
  }
  s->heap[i] = v;
  s->heap_index[v] = (int32_t)i;
}

static void heap_down(SatSolver *s, uint32_t i) {
  uint32_t v = s->heap[i];
  for (;;) {
    uint32_t c = 2u * i + 1u;
    if (c >= s->heap_size) {
      break;
    }
    if (c + 1u < s->heap_size && heap_less(s, s->heap[c + 1u], s->heap[c])) {
      c++;
    }
    if (!heap_less(s, s->heap[c], v)) {
      break;
    }
    s->heap[i] = s->heap[c];
    s->heap_index[s->heap[i]] = (int32_t)i;
    i = c;
  }
  s->heap[i] = v;
  s->heap_index[v] = (int32_t)i;
}

static void heap_insert(SatSolver *s, uint32_t v) {
  if (s->heap_index[v] >= 0) {
    return;
  }
  s->heap[s->heap_size] = v;
  s->heap_index[v] = (int32_t)s->heap_size;
  s->heap_size++;
  heap_up(s, s->heap_size - 1u);
}

static uint32_t heap_pop(SatSolver *s) {
  uint32_t v = s->heap[0];
  s->heap_size--;
  s->heap_index[v] = -1;
  if (s->heap_size) {
    s->heap[0] = s->heap[s->heap_size];
    s->heap_index[s->heap[0]] = 0;
    heap_down(s, 0);
  }
  return v;
}

uint32_t sat_new_var(SatSolver *s) {
  uint32_t v = s->vars;
  if (v == s->var_cap) {
    uint32_t cap = s->var_cap ? s->var_cap * 2u : 256u;
    s->lit_val = realloc(s->lit_val, (size_t)cap * 2u);
    s->level = realloc(s->level, cap * sizeof(uint32_t));
    s->reason = realloc(s->reason, cap * sizeof(uint32_t));
    s->activity = realloc(s->activity, cap * sizeof(double));
    s->polarity = realloc(s->polarity, cap);
    s->seen = realloc(s->seen, cap);
    s->heap = realloc(s->heap, cap * sizeof(uint32_t));
    s->heap_index = realloc(s->heap_index, cap * sizeof(int32_t));
    s->watches = realloc(s->watches, (size_t)cap * 2u * sizeof(WatchList));
    s->trail = realloc(s->trail, cap * sizeof(SatLit));
    s->trail_lim = realloc(s->trail_lim, (cap + 1u) * sizeof(uint32_t));
    s->var_cap = cap;
  }
  s->vars++;
  s->lit_val[2u * v] = 0;
  s->lit_val[2u * v + 1u] = 0;
  s->level[v] = 0;
  s->reason[v] = CREF_NONE;
  s->activity[v] = 0.0;
  s->polarity[v] = 1;
  s->seen[v] = 0;
  s->heap_index[v] = -1;
  memset(&s->watches[2u * v], 0, 2u * sizeof(WatchList));
  heap_insert(s, v);
  return v;
}

static uint32_t clause_alloc(SatSolver *s, const SatLit *lits, uint32_t n,
                             int learnt) {
  size_t need = s->arena_size + HEADER_WORDS + n;
  if (need > s->arena_cap) {
    size_t cap = s->arena_cap ? s->arena_cap : 4096u;
    while (cap < need) {
      cap *= 2u;
    }
    s->arena = realloc(s->arena, cap * sizeof(uint32_t));
    s->arena_cap = cap;
  }
  uint32_t c = (uint32_t)s->arena_size;
  s->arena[c] = (n << 3) | (learnt ? FLAG_LEARNT : 0u);
  s->arena[c + 1] = 0;
  float zero = 0.0f;
  memcpy(&s->arena[c + 2], &zero, sizeof(float));
  memcpy(&s->arena[c + HEADER_WORDS], lits, n * sizeof(SatLit));
  s->arena_size = need;
  return c;
}

static void attach(SatSolver *s, uint32_t c) {
  SatLit *lits = clause_lits(s, c);
  uint32_t n = clause_size(s, c);
  uint32_t tag = n == 2 ? WATCH_BINARY : 0u;
  watch_push(&s->watches[lits[0]], c | tag, lits[1]);
  watch_push(&s->watches[lits[1]], c | tag, lits[0]);
}

static void proof_log(SatSolver *s, const SatLit *lits, uint32_t n,
                      int deletion) {
  if (!s->proof_enabled) {
    return;
  }
  vec_push(&s->proof, n | (deletion ? 0x80000000u : 0u));
  for (uint32_t i = 0; i < n; i++) {
    vec_push(&s->proof, lits[i]);
  }
}

static inline void enqueue(SatSolver *s, SatLit l, uint32_t reason) {
  uint32_t v = SAT_VAR(l);
  s->lit_val[l] = 1;
  s->lit_val[l ^ 1u] = -1;
  s->level[v] = s->decision_level;
  s->reason[v] = reason;
  s->trail[s->trail_size++] = l;
}

static int lit_compare(const void *a, const void *b) {
  SatLit x = *(const SatLit *)a, y = *(const SatLit *)b;
  return x < y ? -1 : x > y;
}

void sat_add_clause(SatSolver *s, const SatLit *in, unsigned n) {
  SatLit stack_buf[64];
  SatLit *lits = n <= 64u ? stack_buf : malloc(n * sizeof(SatLit));
  memcpy(lits, in, n * sizeof(SatLit));
  qsort(lits, n, sizeof(SatLit), lit_compare);
  unsigned m = 0;
  for (unsigned i = 0; i < n; i++) {
    if (m && lits[m - 1u] == lits[i]) {
      continue;
    }
    if (m && lits[m - 1u] == (lits[i] ^ 1u)) {
      if (lits != stack_buf) {
        free(lits);
      }
      return;
    }
    lits[m++] = lits[i];
  }
  s->stats.original_clauses++;
  if (s->proof_enabled) {
    vec_push(&s->originals, m);
    for (unsigned i = 0; i < m; i++) {
      vec_push(&s->originals, lits[i]);
    }
  }
  if (s->unsat) {
    goto done;
  }
  unsigned free_count = 0;
  for (unsigned i = 0; i < m; i++) {
    int val = lit_value(s, lits[i]);
    if (val == 1) {
      goto done;
    }
    if (val == 0) {
      SatLit t = lits[free_count];
      lits[free_count] = lits[i];
      lits[i] = t;
      free_count++;
    }
  }
  if (free_count == 0) {
    s->unsat = 1;
    goto done;
  }
  if (m == 1) {
    enqueue(s, lits[0], CREF_NONE);
    goto done;
  }
  {
    uint32_t c = clause_alloc(s, lits, m, 0);
    vec_push(&s->clauses, c);
    attach(s, c);
    if (free_count == 1) {
      enqueue(s, lits[0], c);
    }
  }
done:
  if (lits != stack_buf) {
    free(lits);
  }
}

static uint32_t propagate(SatSolver *s) {
  uint32_t conflict = CREF_NONE;
  while (s->qhead < s->trail_size) {
    SatLit p = s->trail[s->qhead++];
    SatLit false_lit = p ^ 1u;
    WatchList *wl = &s->watches[false_lit];
    Watcher *ws = wl->items;
    uint32_t n = wl->count, i = 0, j = 0;
    s->stats.propagations++;
    while (i < n) {
      Watcher w = ws[i];
      int bv = lit_value(s, w.blocker);
      if (bv == 1) {
        ws[j++] = ws[i++];
        continue;
      }
      if (w.cref & WATCH_BINARY) {
        uint32_t c = w.cref & ~WATCH_BINARY;
        if (clause_flags(s, c) & FLAG_DELETED) {
          i++;
          continue;
        }
        ws[j++] = ws[i++];
        if (bv == -1) {
          conflict = c;
          while (i < n) {
            ws[j++] = ws[i++];
          }
          break;
        }
        enqueue(s, w.blocker, c);
        continue;
      }
      uint32_t c = w.cref;
      if (clause_flags(s, c) & FLAG_DELETED) {
        i++;
        continue;
      }
      SatLit *lits = clause_lits(s, c);
      if (lits[0] == false_lit) {
        lits[0] = lits[1];
        lits[1] = false_lit;
      }
      i++;
      SatLit first = lits[0];
      if (first != w.blocker && lit_value(s, first) == 1) {
        ws[j].cref = c;
        ws[j].blocker = first;
        j++;
        continue;
      }
      uint32_t size = clause_size(s, c);
      int moved = 0;
      for (uint32_t k = 2; k < size; k++) {
        if (lit_value(s, lits[k]) != -1) {
          lits[1] = lits[k];
          lits[k] = false_lit;
          watch_push(&s->watches[lits[1]], c, first);
          moved = 1;
          break;
        }
      }
      if (moved) {
        ws = wl->items;
        continue;
      }
      ws[j].cref = c;
      ws[j].blocker = first;
      j++;
      if (lit_value(s, first) == -1) {
        conflict = c;
        while (i < n) {
          ws[j++] = ws[i++];
        }
        break;
      }
      enqueue(s, first, c);
    }
    wl->count = j;
    if (conflict != CREF_NONE) {
      s->qhead = s->trail_size;
      return conflict;
    }
  }
  return conflict;
}

static void var_bump(SatSolver *s, uint32_t v) {
  s->activity[v] += s->var_inc;
  if (s->activity[v] > 1e100) {
    for (uint32_t i = 0; i < s->vars; i++) {
      s->activity[i] *= 1e-100;
    }
    s->var_inc *= 1e-100;
  }
  if (s->heap_index[v] >= 0) {
    heap_up(s, (uint32_t)s->heap_index[v]);
  }
}

static void clause_bump(SatSolver *s, uint32_t c) {
  float *a = clause_activity(s, c);
  *a += s->cla_inc;
  if (*a > 1e20f) {
    for (size_t i = 0; i < s->learnts.count; i++) {
      *clause_activity(s, s->learnts.items[i]) *= 1e-20f;
    }
    s->cla_inc *= 1e-20f;
  }
}

static uint32_t abstract_level(const SatSolver *s, uint32_t v) {
  return 1u << (s->level[v] & 31u);
}

static int lit_redundant(SatSolver *s, SatLit p, uint32_t abstract_levels) {
  size_t top = s->to_clear.count;
  s->stack.count = 0;
  vec_push(&s->stack, p);
  while (s->stack.count) {
    SatLit q = s->stack.items[--s->stack.count];
    uint32_t c = s->reason[SAT_VAR(q)];
    SatLit *lits = clause_lits(s, c);
    uint32_t size = clause_size(s, c);
    for (uint32_t i = 0; i < size; i++) {
      SatLit r = lits[i];
      uint32_t v = SAT_VAR(r);
      if (v == SAT_VAR(q) || s->seen[v] || s->level[v] == 0) {
        continue;
      }
      if (s->reason[v] != CREF_NONE &&
          (abstract_level(s, v) & abstract_levels)) {
        s->seen[v] = 1;
        vec_push(&s->stack, r);
        vec_push(&s->to_clear, r);
      } else {
        for (size_t k = top; k < s->to_clear.count; k++) {
          s->seen[SAT_VAR(s->to_clear.items[k])] = 0;
        }
        s->to_clear.count = top;
        return 0;
      }
    }
  }
  return 1;
}

static uint32_t compute_lbd(SatSolver *s, const SatLit *lits, uint32_t n) {
  if (s->level_stamp_cap <= s->decision_level) {
    uint32_t cap = s->decision_level + 64u;
    s->level_stamp = realloc(s->level_stamp, cap * sizeof(uint32_t));
    memset(s->level_stamp + s->level_stamp_cap, 0,
           (cap - s->level_stamp_cap) * sizeof(uint32_t));
    s->level_stamp_cap = cap;
  }
  s->stamp++;
  uint32_t lbd = 0;
  for (uint32_t i = 0; i < n; i++) {
    uint32_t l = s->level[SAT_VAR(lits[i])];
    if (s->level_stamp[l] != s->stamp) {
      s->level_stamp[l] = s->stamp;
      lbd++;
    }
  }
  return lbd;
}

static void analyze(SatSolver *s, uint32_t conflict, uint32_t *out_level,
                    uint32_t *out_lbd) {
  U32Vec *learnt = &s->learnt_buf;
  learnt->count = 0;
  vec_push(learnt, 0);
  int path = 0;
  SatLit p = 0;
  int have_p = 0;
  int64_t index = (int64_t)s->trail_size - 1;
  s->to_clear.count = 0;
  for (;;) {
    uint32_t c = conflict;
    if (clause_flags(s, c) & FLAG_LEARNT) {
      clause_bump(s, c);
    }
    SatLit *lits = clause_lits(s, c);
    uint32_t size = clause_size(s, c);
    for (uint32_t i = 0; i < size; i++) {
      SatLit q = lits[i];
      if (have_p && q == p) {
        continue;
      }
      uint32_t v = SAT_VAR(q);
      if (!s->seen[v] && s->level[v] > 0) {
        var_bump(s, v);
        s->seen[v] = 1;
        vec_push(&s->to_clear, q);
        if (s->level[v] >= s->decision_level) {
          path++;
        } else {
          vec_push(learnt, q);
        }
      }
    }
    while (!s->seen[SAT_VAR(s->trail[index])]) {
      index--;
    }
    p = s->trail[index];
    index--;
    have_p = 1;
    conflict = s->reason[SAT_VAR(p)];
    s->seen[SAT_VAR(p)] = 0;
    path--;
    if (path <= 0) {
      break;
    }
  }
  learnt->items[0] = p ^ 1u;
  uint32_t abstract_levels = 0;
  for (size_t i = 1; i < learnt->count; i++) {
    abstract_levels |= abstract_level(s, SAT_VAR(learnt->items[i]));
  }
  size_t before = learnt->count;
  size_t j = 1;
  for (size_t i = 1; i < learnt->count; i++) {
    SatLit q = learnt->items[i];
    if (s->reason[SAT_VAR(q)] == CREF_NONE ||
        !lit_redundant(s, q, abstract_levels)) {
      learnt->items[j++] = q;
    }
  }
  learnt->count = j;
  s->stats.minimized_literals += before - j;
  s->stats.learnt_literals += j;
  uint32_t level = 0;
  if (learnt->count > 1) {
    size_t max_i = 1;
    for (size_t i = 2; i < learnt->count; i++) {
      if (s->level[SAT_VAR(learnt->items[i])] >
          s->level[SAT_VAR(learnt->items[max_i])]) {
        max_i = i;
      }
    }
    SatLit t = learnt->items[max_i];
    learnt->items[max_i] = learnt->items[1];
    learnt->items[1] = t;
    level = s->level[SAT_VAR(t)];
  }
  for (size_t i = 0; i < s->to_clear.count; i++) {
    s->seen[SAT_VAR(s->to_clear.items[i])] = 0;
  }
  s->to_clear.count = 0;
  *out_level = level;
  *out_lbd = compute_lbd(s, learnt->items, (uint32_t)learnt->count);
}

static void backtrack(SatSolver *s, uint32_t level) {
  if (s->decision_level <= level) {
    return;
  }
  uint32_t lim = s->trail_lim[level];
  for (uint32_t i = s->trail_size; i-- > lim;) {
    SatLit l = s->trail[i];
    uint32_t v = SAT_VAR(l);
    s->lit_val[l] = 0;
    s->lit_val[l ^ 1u] = 0;
    s->reason[v] = CREF_NONE;
    s->polarity[v] = (uint8_t)SAT_SIGN(l);
    heap_insert(s, v);
  }
  s->trail_size = lim;
  s->qhead = lim;
  s->decision_level = level;
}

static int clause_locked(SatSolver *s, uint32_t c) {
  SatLit *lits = clause_lits(s, c);
  uint32_t v = SAT_VAR(lits[0]);
  if (s->reason[v] == c && lit_value(s, lits[0]) == 1) {
    return 1;
  }
  if (clause_size(s, c) == 2) {
    uint32_t v1 = SAT_VAR(lits[1]);
    return s->reason[v1] == c && lit_value(s, lits[1]) == 1;
  }
  return 0;
}

static void clause_delete(SatSolver *s, uint32_t c) {
  proof_log(s, clause_lits(s, c), clause_size(s, c), 1);
  s->arena[c] |= FLAG_DELETED;
  s->arena_wasted += HEADER_WORDS + clause_size(s, c);
}

static SatSolver *g_sort_solver;

static int learnt_compare(const void *a, const void *b) {
  uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
  SatSolver *s = g_sort_solver;
  uint32_t lx = s->arena[x + 1], ly = s->arena[y + 1];
  if (lx != ly) {
    return lx > ly ? -1 : 1;
  }
  float ax = *clause_activity(s, x), ay = *clause_activity(s, y);
  return ax < ay ? -1 : ax > ay;
}

static void garbage_collect(SatSolver *s) {
  size_t cap = s->arena_size - s->arena_wasted + 1024u;
  uint32_t *fresh = malloc(cap * sizeof(uint32_t));
  size_t size = 0;
  U32Vec *lists[2] = {&s->clauses, &s->learnts};
  for (int l = 0; l < 2; l++) {
    U32Vec *v = lists[l];
    size_t j = 0;
    for (size_t i = 0; i < v->count; i++) {
      uint32_t c = v->items[i];
      if (clause_flags(s, c) & FLAG_DELETED) {
        continue;
      }
      uint32_t words = HEADER_WORDS + clause_size(s, c);
      memcpy(&fresh[size], &s->arena[c], words * sizeof(uint32_t));
      s->arena[c + 1] = (uint32_t)size;
      s->arena[c] |= 4u;
      v->items[j++] = (uint32_t)size;
      size += words;
    }
    v->count = j;
  }
  for (uint32_t i = 0; i < s->trail_size; i++) {
    uint32_t v = SAT_VAR(s->trail[i]);
    uint32_t r = s->reason[v];
    if (r != CREF_NONE) {
      if (s->arena[r] & 4u) {
        s->reason[v] = s->arena[r + 1];
      } else {
        s->reason[v] = CREF_NONE;
      }
    }
  }
  free(s->arena);
  s->arena = fresh;
  s->arena_size = size;
  s->arena_cap = cap;
  s->arena_wasted = 0;
  for (uint32_t i = 0; i < s->vars * 2u; i++) {
    s->watches[i].count = 0;
  }
  for (int l = 0; l < 2; l++) {
    for (size_t i = 0; i < lists[l]->count; i++) {
      attach(s, lists[l]->items[i]);
    }
  }
}

static void reduce_db(SatSolver *s) {
  s->stats.reductions++;
  g_sort_solver = s;
  qsort(s->learnts.items, s->learnts.count, sizeof(uint32_t), learnt_compare);
  size_t limit = s->learnts.count / 2u;
  size_t j = 0;
  for (size_t i = 0; i < s->learnts.count; i++) {
    uint32_t c = s->learnts.items[i];
    if (i < limit && s->arena[c + 1] > 2u && clause_size(s, c) > 2u &&
        !clause_locked(s, c)) {
      clause_delete(s, c);
    } else {
      s->learnts.items[j++] = c;
    }
  }
  s->learnts.count = j;
  if (s->arena_wasted * 2u > s->arena_size) {
    garbage_collect(s);
  }
}

static void simplify_db(SatSolver *s) {
  if (s->decision_level != 0 || s->trail_size == s->simplify_trail) {
    return;
  }
  s->simplify_trail = s->trail_size;
  U32Vec *lists[2] = {&s->clauses, &s->learnts};
  for (int l = 0; l < 2; l++) {
    U32Vec *v = lists[l];
    size_t j = 0;
    for (size_t i = 0; i < v->count; i++) {
      uint32_t c = v->items[i];
      SatLit *lits = clause_lits(s, c);
      uint32_t size = clause_size(s, c);
      int satisfied = 0;
      for (uint32_t k = 0; k < size && !satisfied; k++) {
        satisfied = lit_value(s, lits[k]) == 1;
      }
      if (satisfied && !clause_locked(s, c)) {
        clause_delete(s, c);
      } else {
        v->items[j++] = c;
      }
    }
    v->count = j;
  }
  if (s->arena_wasted * 2u > s->arena_size) {
    garbage_collect(s);
  }
}

static SatLit pick_branch(SatSolver *s) {
  while (s->heap_size) {
    uint32_t v = heap_pop(s);
    if (s->lit_val[2u * v] == 0) {
      return SAT_LIT(v, s->polarity[v]);
    }
  }
  return 0xffffffffu;
}

int sat_solve(SatSolver *s, uint64_t conflict_limit) {
  if (s->unsat) {
    return SAT_RESULT_UNSAT;
  }
  uint64_t start_conflicts = s->stats.conflicts;
  if (propagate(s) != CREF_NONE) {
    s->unsat = 1;
    return SAT_RESULT_UNSAT;
  }
  for (;;) {
    uint32_t conflict = propagate(s);
    if (conflict != CREF_NONE) {
      s->stats.conflicts++;
      s->conflicts_since_restart++;
      if (s->decision_level == 0) {
        s->unsat = 1;
        proof_log(s, NULL, 0, 0);
        return SAT_RESULT_UNSAT;
      }
      uint32_t level, lbd;
      analyze(s, conflict, &level, &lbd);
      backtrack(s, level);
      SatLit *lits = s->learnt_buf.items;
      uint32_t n = (uint32_t)s->learnt_buf.count;
      proof_log(s, lits, n, 0);
      if (n == 1) {
        enqueue(s, lits[0], CREF_NONE);
      } else {
        uint32_t c = clause_alloc(s, lits, n, 1);
        s->arena[c + 1] = lbd;
        vec_push(&s->learnts, c);
        attach(s, c);
        clause_bump(s, c);
        enqueue(s, lits[0], c);
      }
      s->var_inc /= s->var_decay;
      s->cla_inc /= 0.999f;
      double l = (double)lbd;
      if (s->stats.conflicts == 1) {
        s->ema_fast = l;
        s->ema_slow = l;
      } else {
        s->ema_fast += (l - s->ema_fast) / 32.0;
        s->ema_slow += (l - s->ema_slow) / 4096.0;
      }
      continue;
    }
    if (conflict_limit &&
        s->stats.conflicts - start_conflicts >= conflict_limit) {
      backtrack(s, 0);
      return SAT_RESULT_UNKNOWN;
    }
    if (s->conflicts_since_restart >= 50 &&
        s->ema_fast > 1.25 * s->ema_slow) {
      s->conflicts_since_restart = 0;
      s->stats.restarts++;
      backtrack(s, 0);
      continue;
    }
    if (s->decision_level == 0) {
      simplify_db(s);
    }
    if (s->stats.conflicts >= s->next_reduce) {
      s->next_reduce = s->stats.conflicts + 2000 +
                       s->reduce_increment * s->stats.reductions;
      reduce_db(s);
    }
    SatLit next = pick_branch(s);
    if (next == 0xffffffffu) {
      return SAT_RESULT_SAT;
    }
    s->stats.decisions++;
    s->trail_lim[s->decision_level++] = s->trail_size;
    enqueue(s, next, CREF_NONE);
  }
}

int sat_value(const SatSolver *s, uint32_t var) {
  return s->lit_val[2u * var] == 1;
}

void sat_get_stats(const SatSolver *s, SatStats *stats) { *stats = s->stats; }

void sat_proof_view(const SatSolver *s, SatProofView *view) {
  view->originals = s->originals.items;
  view->original_words = s->originals.count;
  view->proof = s->proof.items;
  view->proof_words = s->proof.count;
  view->vars = s->vars;
}
