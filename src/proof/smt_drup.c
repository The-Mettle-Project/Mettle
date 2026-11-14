#include "smt_sat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct SatChecker {
  size_t *where;
  size_t ids;
  size_t where_cap;
  uint32_t *store;
  size_t store_size;
  size_t store_cap;
  size_t store_dead;
  int8_t *val;
  size_t lit_cap;
  SatLit *trail;
  size_t trail_size;
  size_t trail_cap;
  uint64_t lemmas;
  int failed;
  int refuted;
};

enum { VISIT_SATISFIED, VISIT_OPEN, VISIT_UNIT, VISIT_CONFLICT, VISIT_BAD };

static void *checker_grow(void *ptr, size_t bytes) {
  void *out = realloc(ptr, bytes);
  if (!out) {
    fprintf(stderr, "smt: out of memory while checking a refutation\n");
    abort();
  }
  return out;
}

SatChecker *sat_checker_new(void) {
  SatChecker *c = calloc(1, sizeof(SatChecker));
  if (!c) {
    fprintf(stderr, "smt: out of memory while checking a refutation\n");
    abort();
  }
  return c;
}

void sat_checker_free(SatChecker *c) {
  if (!c) {
    return;
  }
  free(c->where);
  free(c->store);
  free(c->val);
  free(c->trail);
  free(c);
}

int sat_checker_refuted(const SatChecker *c) {
  return c && !c->failed && c->refuted;
}

int sat_checker_failed(const SatChecker *c) { return c && c->failed; }

uint64_t sat_checker_lemmas(const SatChecker *c) { return c ? c->lemmas : 0; }

static void reserve_literal(SatChecker *c, SatLit l) {
  size_t need = (size_t)(l | 1u) + 1u;
  if (need <= c->lit_cap) {
    return;
  }
  size_t cap = c->lit_cap ? c->lit_cap : 1024u;
  while (cap < need) {
    cap *= 2u;
  }
  c->val = checker_grow(c->val, cap);
  memset(c->val + c->lit_cap, 0, cap - c->lit_cap);
  c->lit_cap = cap;
  c->trail = checker_grow(c->trail, (cap / 2u + 1u) * sizeof(SatLit));
  c->trail_cap = cap / 2u + 1u;
}

static void assign(SatChecker *c, SatLit l) {
  c->val[l] = 1;
  c->val[l ^ 1u] = -1;
  c->trail[c->trail_size++] = l;
}

static void undo(SatChecker *c, size_t to) {
  while (c->trail_size > to) {
    SatLit l = c->trail[--c->trail_size];
    c->val[l] = 0;
    c->val[l ^ 1u] = 0;
  }
}

static void compact(SatChecker *c) {
  size_t live = c->store_size - c->store_dead;
  size_t cap = live + live / 2u + 1024u;
  uint32_t *fresh = checker_grow(NULL, cap * sizeof(uint32_t));
  size_t size = 0;
  for (size_t id = 0; id < c->ids; id++) {
    if (!c->where[id]) {
      continue;
    }
    const uint32_t *old = c->store + c->where[id] - 1u;
    size_t words = (size_t)old[0] + 1u;
    memcpy(fresh + size, old, words * sizeof(uint32_t));
    c->where[id] = size + 1u;
    size += words;
  }
  free(c->store);
  c->store = fresh;
  c->store_size = size;
  c->store_cap = cap;
  c->store_dead = 0;
}

static void keep_clause(SatChecker *c, const uint32_t *lits, uint32_t n) {
  if (c->ids == c->where_cap) {
    c->where_cap = c->where_cap ? c->where_cap * 2u : 4096u;
    c->where = checker_grow(c->where, c->where_cap * sizeof(size_t));
  }
  if (c->store_size + n + 1u > c->store_cap) {
    size_t cap = c->store_cap ? c->store_cap : 4096u;
    while (cap < c->store_size + n + 1u) {
      cap *= 2u;
    }
    c->store = checker_grow(c->store, cap * sizeof(uint32_t));
    c->store_cap = cap;
  }
  c->where[c->ids++] = c->store_size + 1u;
  c->store[c->store_size] = n;
  memcpy(c->store + c->store_size + 1u, lits, n * sizeof(uint32_t));
  c->store_size += (size_t)n + 1u;
}

static void drop_clause(SatChecker *c, uint32_t id) {
  if (id == 0 || id > c->ids || !c->where[id - 1u]) {
    return;
  }
  c->store_dead += (size_t)c->store[c->where[id - 1u] - 1u] + 1u;
  c->where[id - 1u] = 0;
  if (c->store_dead > 65536u && c->store_dead * 2u > c->store_size) {
    compact(c);
  }
}

static int visit(SatChecker *c, uint32_t id) {
  if (id == 0 || id > c->ids || !c->where[id - 1u]) {
    return VISIT_BAD;
  }
  const uint32_t *clause = c->store + c->where[id - 1u] - 1u;
  uint32_t n = clause[0];
  SatLit unit = 0;
  uint32_t open = 0;
  for (uint32_t k = 1; k <= n; k++) {
    SatLit l = clause[k];
    int8_t v = c->val[l];
    if (v == 1) {
      return VISIT_SATISFIED;
    }
    if (v == 0) {
      if (open) {
        return VISIT_OPEN;
      }
      open = 1;
      unit = l;
    }
  }
  if (!open) {
    return VISIT_CONFLICT;
  }
  assign(c, unit);
  return VISIT_UNIT;
}

static int lemma_follows(SatChecker *c, const uint32_t *lits, uint32_t n,
                         const uint32_t *hints, uint32_t nh) {
  size_t mark = c->trail_size;
  for (uint32_t i = 0; i < n; i++) {
    SatLit l = lits[i];
    if (c->val[l] == 1) {
      undo(c, mark);
      return 1;
    }
    if (c->val[l] == 0) {
      assign(c, l ^ 1u);
    }
  }
  for (;;) {
    int open = 0, changed = 0;
    for (uint32_t h = 0; h < nh; h++) {
      int r = visit(c, hints[h]);
      if (r == VISIT_CONFLICT) {
        undo(c, mark);
        return 1;
      }
      if (r == VISIT_BAD) {
        undo(c, mark);
        return 0;
      }
      open |= r == VISIT_OPEN;
      changed |= r == VISIT_UNIT;
    }
    if (!open || !changed) {
      break;
    }
  }
  undo(c, mark);
  return 0;
}

static void settle_unit(SatChecker *c, SatLit l) {
  if (c->val[l] == -1) {
    c->refuted = 1;
  } else if (c->val[l] == 0) {
    assign(c, l);
  }
}

void sat_checker_feed(SatChecker *c, const uint32_t *words, size_t count) {
  size_t i = 0;
  while (i < count && !c->failed) {
    if (i + 2u > count) {
      c->failed = 1;
      break;
    }
    uint32_t kind = words[i++];
    uint32_t n = words[i++];
    if (kind == SAT_PROOF_DELETE) {
      drop_clause(c, n);
      continue;
    }
    const uint32_t *lits = words + i;
    i += n;
    if (i >= count) {
      c->failed = 1;
      break;
    }
    uint32_t nh = words[i++];
    const uint32_t *hints = words + i;
    i += nh;
    if (i > count || kind > SAT_PROOF_LEMMA) {
      c->failed = 1;
      break;
    }
    for (uint32_t k = 0; k < n; k++) {
      reserve_literal(c, lits[k]);
    }
    if (kind == SAT_PROOF_LEMMA) {
      c->lemmas++;
      if (!lemma_follows(c, lits, n, hints, nh)) {
        c->failed = 1;
        break;
      }
    }
    keep_clause(c, lits, n);
    if (n == 0) {
      c->refuted = 1;
    } else if (n == 1) {
      settle_unit(c, lits[0]);
    }
  }
}
