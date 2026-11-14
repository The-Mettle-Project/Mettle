#include "smt_sat.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
  uint32_t *items;
  uint32_t count;
  uint32_t cap;
} DrupList;

typedef struct {
  uint32_t vars;
  int8_t *val;
  uint32_t *reason;
  SatLit *trail;
  uint32_t trail_size;
  uint32_t qhead;
  DrupList *watches;
  uint32_t *arena;
  size_t arena_size;
  size_t arena_cap;
  uint32_t *buckets;
  uint32_t *bucket_next;
  uint32_t bucket_cap;
  uint32_t *clause_hash;
  uint32_t *clause_ref;
  uint32_t clause_count;
  uint32_t clause_cap;
  int conflict;
} Drup;

#define DRUP_NONE 0xffffffffu

static void drup_list_push(DrupList *l, uint32_t x) {
  if (l->count == l->cap) {
    l->cap = l->cap ? l->cap * 2u : 4u;
    l->items = realloc(l->items, l->cap * sizeof(uint32_t));
  }
  l->items[l->count++] = x;
}

static int drup_sort_cmp(const void *a, const void *b) {
  SatLit x = *(const SatLit *)a, y = *(const SatLit *)b;
  return x < y ? -1 : x > y;
}

static uint32_t drup_hash(const SatLit *lits, uint32_t n) {
  uint32_t sum = 0, xr = 0, prod = 1;
  for (uint32_t i = 0; i < n; i++) {
    sum += lits[i];
    xr ^= lits[i] * 2654435761u;
    prod *= (lits[i] | 1u);
  }
  return sum * 1000003u ^ xr ^ prod * 31u ^ n;
}

static void drup_assign(Drup *d, SatLit l, uint32_t reason) {
  d->val[l] = 1;
  d->val[l ^ 1u] = -1;
  d->reason[SAT_VAR(l)] = reason;
  d->trail[d->trail_size++] = l;
}

static int drup_propagate(Drup *d) {
  while (d->qhead < d->trail_size) {
    SatLit p = d->trail[d->qhead++];
    SatLit f = p ^ 1u;
    DrupList *wl = &d->watches[f];
    uint32_t i = 0, j = 0, n = wl->count;
    while (i < n) {
      uint32_t c = wl->items[i++];
      uint32_t header = d->arena[c];
      if (header & 1u) {
        continue;
      }
      uint32_t size = header >> 1;
      SatLit *lits = &d->arena[c + 1];
      if (lits[0] == f) {
        lits[0] = lits[1];
        lits[1] = f;
      }
      if (d->val[lits[0]] == 1) {
        wl->items[j++] = c;
        continue;
      }
      int moved = 0;
      for (uint32_t k = 2; k < size; k++) {
        if (d->val[lits[k]] != -1) {
          lits[1] = lits[k];
          lits[k] = f;
          drup_list_push(&d->watches[lits[1]], c);
          moved = 1;
          break;
        }
      }
      if (moved) {
        continue;
      }
      wl->items[j++] = c;
      if (d->val[lits[0]] == -1) {
        while (i < n) {
          wl->items[j++] = wl->items[i++];
        }
        wl->count = j;
        d->qhead = d->trail_size;
        return 1;
      }
      drup_assign(d, lits[0], c);
    }
    wl->count = j;
  }
  return 0;
}

static void drup_backtrack(Drup *d, uint32_t to) {
  while (d->trail_size > to) {
    SatLit l = d->trail[--d->trail_size];
    d->val[l] = 0;
    d->val[l ^ 1u] = 0;
    d->reason[SAT_VAR(l)] = DRUP_NONE;
  }
  d->qhead = to;
}

static void drup_index(Drup *d, uint32_t c, uint32_t h) {
  if ((d->clause_count + 1u) * 2u > d->bucket_cap) {
    uint32_t cap = d->bucket_cap ? d->bucket_cap * 2u : 1024u;
    uint32_t *buckets = malloc(cap * sizeof(uint32_t));
    for (uint32_t i = 0; i < cap; i++) {
      buckets[i] = DRUP_NONE;
    }
    d->bucket_next =
        realloc(d->bucket_next, (cap / 2u + 1u) * sizeof(uint32_t));
    for (uint32_t i = 0; i < d->clause_count; i++) {
      uint32_t slot = d->clause_hash[i] & (cap - 1u);
      d->bucket_next[i] = buckets[slot];
      buckets[slot] = i;
    }
    free(d->buckets);
    d->buckets = buckets;
    d->bucket_cap = cap;
  }
  if (d->clause_count == d->clause_cap) {
    d->clause_cap = d->clause_cap ? d->clause_cap * 2u : 1024u;
    d->clause_hash = realloc(d->clause_hash, d->clause_cap * sizeof(uint32_t));
    d->clause_ref = realloc(d->clause_ref, d->clause_cap * sizeof(uint32_t));
  }
  uint32_t id = d->clause_count++;
  d->clause_hash[id] = h;
  d->clause_ref[id] = c;
  uint32_t slot = h & (d->bucket_cap - 1u);
  d->bucket_next[id] = d->buckets[slot];
  d->buckets[slot] = id;
}

static void drup_add(Drup *d, const SatLit *in, uint32_t n) {
  if (d->conflict) {
    return;
  }
  if (n == 0) {
    d->conflict = drup_propagate(d) || 1;
    return;
  }
  if (d->arena_size + n + 1u > d->arena_cap) {
    size_t cap = d->arena_cap ? d->arena_cap : 4096u;
    while (cap < d->arena_size + n + 1u) {
      cap *= 2u;
    }
    d->arena = realloc(d->arena, cap * sizeof(uint32_t));
    d->arena_cap = cap;
  }
  uint32_t c = (uint32_t)d->arena_size;
  d->arena[c] = n << 1;
  SatLit *lits = &d->arena[c + 1];
  memcpy(lits, in, n * sizeof(SatLit));
  qsort(lits, n, sizeof(SatLit), drup_sort_cmp);
  d->arena_size += n + 1u;
  drup_index(d, c, drup_hash(lits, n));
  uint32_t nonfalse = 0;
  int satisfied = 0;
  for (uint32_t i = 0; i < n; i++) {
    int v = d->val[lits[i]];
    if (v == 1) {
      satisfied = 1;
    }
    if (v != -1 && nonfalse < 2u) {
      SatLit t = lits[nonfalse];
      lits[nonfalse] = lits[i];
      lits[i] = t;
      nonfalse++;
    }
  }
  if (n == 1) {
    if (d->val[lits[0]] == -1) {
      d->conflict = 1;
    } else if (d->val[lits[0]] == 0) {
      drup_assign(d, lits[0], c);
      if (drup_propagate(d)) {
        d->conflict = 1;
      }
    }
    return;
  }
  drup_list_push(&d->watches[lits[0]], c);
  drup_list_push(&d->watches[lits[1]], c);
  if (satisfied) {
    return;
  }
  if (nonfalse == 0) {
    d->conflict = 1;
    return;
  }
  if (nonfalse == 1) {
    drup_assign(d, lits[0], c);
    if (drup_propagate(d)) {
      d->conflict = 1;
    }
  }
}

static void drup_delete(Drup *d, const SatLit *in, uint32_t n) {
  if (n <= 1u || d->conflict) {
    return;
  }
  SatLit buf[256];
  SatLit *sorted = n <= 256u ? buf : malloc(n * sizeof(SatLit));
  memcpy(sorted, in, n * sizeof(SatLit));
  qsort(sorted, n, sizeof(SatLit), drup_sort_cmp);
  uint32_t h = drup_hash(sorted, n);
  uint32_t slot = h & (d->bucket_cap - 1u);
  for (uint32_t id = d->buckets[slot]; id != DRUP_NONE;
       id = d->bucket_next[id]) {
    if (d->clause_hash[id] != h) {
      continue;
    }
    uint32_t c = d->clause_ref[id];
    if ((d->arena[c] & 1u) || (d->arena[c] >> 1) != n) {
      continue;
    }
    SatLit tmp[256];
    SatLit *cl = n <= 256u ? tmp : malloc(n * sizeof(SatLit));
    memcpy(cl, &d->arena[c + 1], n * sizeof(SatLit));
    qsort(cl, n, sizeof(SatLit), drup_sort_cmp);
    int same = memcmp(cl, sorted, n * sizeof(SatLit)) == 0;
    if (cl != tmp) {
      free(cl);
    }
    if (!same) {
      continue;
    }
    int is_reason = 0;
    for (uint32_t i = 0; i < n && !is_reason; i++) {
      SatLit l = d->arena[c + 1 + i];
      is_reason = d->val[l] == 1 && d->reason[SAT_VAR(l)] == c;
    }
    if (!is_reason) {
      d->arena[c] |= 1u;
    }
    break;
  }
  if (sorted != buf) {
    free(sorted);
  }
}

static int drup_rup(Drup *d, const SatLit *lits, uint32_t n) {
  if (d->conflict) {
    return 1;
  }
  uint32_t saved = d->trail_size;
  for (uint32_t i = 0; i < n; i++) {
    int v = d->val[lits[i]];
    if (v == 1) {
      drup_backtrack(d, saved);
      return 1;
    }
    if (v == 0) {
      drup_assign(d, lits[i] ^ 1u, DRUP_NONE);
    }
  }
  int ok = drup_propagate(d);
  drup_backtrack(d, saved);
  return ok;
}

int sat_drup_check(const SatProofView *view, uint64_t *lemmas_checked) {
  Drup d;
  memset(&d, 0, sizeof(d));
  d.vars = view->vars;
  size_t lits = (size_t)d.vars * 2u + 2u;
  d.val = calloc(lits, 1);
  d.reason = malloc(((size_t)d.vars + 1u) * sizeof(uint32_t));
  for (uint32_t i = 0; i <= d.vars; i++) {
    d.reason[i] = DRUP_NONE;
  }
  d.trail = malloc(((size_t)d.vars + 1u) * sizeof(SatLit));
  d.watches = calloc(lits, sizeof(DrupList));
  uint64_t checked = 0;
  int ok = 1;
  size_t i = 0;
  while (i < view->original_words) {
    uint32_t n = view->originals[i++];
    drup_add(&d, &view->originals[i], n);
    i += n;
  }
  if (!d.conflict && drup_propagate(&d)) {
    d.conflict = 1;
  }
  i = 0;
  while (i < view->proof_words && ok) {
    uint32_t header = view->proof[i++];
    uint32_t n = header & 0x7fffffffu;
    const SatLit *l = &view->proof[i];
    i += n;
    if (header & 0x80000000u) {
      drup_delete(&d, l, n);
      continue;
    }
    checked++;
    if (!drup_rup(&d, l, n)) {
      ok = 0;
      break;
    }
    drup_add(&d, l, n);
  }
  if (ok && !d.conflict) {
    ok = drup_propagate(&d);
  }
  if (lemmas_checked) {
    *lemmas_checked = checked;
  }
  for (size_t k = 0; k < lits; k++) {
    free(d.watches[k].items);
  }
  free(d.watches);
  free(d.val);
  free(d.reason);
  free(d.trail);
  free(d.arena);
  free(d.buckets);
  free(d.bucket_next);
  free(d.clause_hash);
  free(d.clause_ref);
  return ok;
}
