#include "smt_sat.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
  uint32_t kind;
  uint32_t n;
  size_t lits;
  uint32_t nh;
  size_t hints;
} ProofEntry;

typedef struct {
  const uint32_t *words;
  ProofEntry *entries;
  size_t count;
  int8_t *val;
  SatLit *trail;
  size_t trail_size;
  size_t lit_count;
} Checker;

static int lit_ok(const Checker *c, SatLit l) { return l < c->lit_count; }

static void assign(Checker *c, SatLit l) {
  c->val[l] = 1;
  c->val[l ^ 1u] = -1;
  c->trail[c->trail_size++] = l;
}

static void undo(Checker *c, size_t to) {
  while (c->trail_size > to) {
    SatLit l = c->trail[--c->trail_size];
    c->val[l] = 0;
    c->val[l ^ 1u] = 0;
  }
}

static int hint_refutes(Checker *c, const ProofEntry *lemma) {
  size_t mark = c->trail_size;
  const uint32_t *lits = c->words + lemma->lits;
  for (uint32_t i = 0; i < lemma->n; i++) {
    SatLit l = lits[i];
    if (!lit_ok(c, l)) {
      undo(c, mark);
      return 0;
    }
    if (c->val[l] == 1) {
      undo(c, mark);
      return 1;
    }
    if (c->val[l] == 0) {
      assign(c, l ^ 1u);
    }
  }
  const uint32_t *hints = c->words + lemma->hints;
  for (int changed = 1; changed;) {
    changed = 0;
    for (uint32_t h = lemma->nh; h-- > 0;) {
      uint32_t id = hints[h];
      if (id == 0 || id > c->count) {
        undo(c, mark);
        return 0;
      }
      const ProofEntry *e = &c->entries[id - 1];
      if (e == lemma || e > lemma) {
        undo(c, mark);
        return 0;
      }
      const uint32_t *cl = c->words + e->lits;
      SatLit unit = 0;
      uint32_t open = 0;
      int satisfied = 0;
      for (uint32_t k = 0; k < e->n && !satisfied; k++) {
        SatLit l = cl[k];
        if (!lit_ok(c, l)) {
          undo(c, mark);
          return 0;
        }
        if (c->val[l] == 1) {
          satisfied = 1;
        } else if (c->val[l] == 0) {
          open++;
          unit = l;
        }
      }
      if (satisfied) {
        continue;
      }
      if (open == 0) {
        undo(c, mark);
        return 1;
      }
      if (open == 1) {
        assign(c, unit);
        changed = 1;
      }
    }
  }
  undo(c, mark);
  return 0;
}

int sat_drup_check(const SatProofView *view, uint64_t *lemmas_checked) {
  Checker c;
  memset(&c, 0, sizeof(c));
  c.words = view->proof;
  c.lit_count = (size_t)view->vars * 2u;
  size_t cap = 0;
  size_t i = 0;
  int ok = 1;
  while (i < view->proof_words) {
    if (i + 2 > view->proof_words) {
      ok = 0;
      break;
    }
    ProofEntry e;
    e.kind = view->proof[i++];
    e.n = view->proof[i++];
    e.lits = i;
    i += e.n;
    if (i >= view->proof_words) {
      ok = 0;
      break;
    }
    e.nh = view->proof[i++];
    e.hints = i;
    i += e.nh;
    if (i > view->proof_words) {
      ok = 0;
      break;
    }
    if (c.count == cap) {
      cap = cap ? cap * 2 : 1024;
      c.entries = realloc(c.entries, cap * sizeof(ProofEntry));
    }
    c.entries[c.count++] = e;
  }
  size_t last = c.count;
  for (size_t k = c.count; ok && k-- > 0;) {
    if (c.entries[k].kind == 1 && c.entries[k].n == 0) {
      last = k;
      break;
    }
  }
  if (!ok || last == c.count) {
    free(c.entries);
    if (lemmas_checked) {
      *lemmas_checked = 0;
    }
    return 0;
  }
  uint8_t *needed = calloc(c.count, 1);
  needed[last] = 1;
  for (size_t k = 0; k < last; k++) {
    if (c.entries[k].kind == 1 && c.entries[k].n <= 1) {
      needed[k] = 1;
    }
  }
  for (size_t k = last + 1; k-- > 0;) {
    if (!needed[k] || c.entries[k].kind != 1) {
      continue;
    }
    const uint32_t *hints = c.words + c.entries[k].hints;
    for (uint32_t h = 0; h < c.entries[k].nh; h++) {
      uint32_t id = hints[h];
      if (id >= 1 && id <= k) {
        needed[id - 1] = 1;
      }
    }
  }
  c.val = calloc(c.lit_count + 2, 1);
  c.trail = malloc((c.lit_count / 2 + 2) * sizeof(SatLit));
  uint64_t checked = 0;
  int refuted = 0;
  for (size_t k = 0; k <= last && ok && !refuted; k++) {
    ProofEntry *e = &c.entries[k];
    const uint32_t *lits = c.words + e->lits;
    if (e->kind == 0) {
      if (e->n == 1 && lit_ok(&c, lits[0])) {
        if (c.val[lits[0]] == -1) {
          refuted = 1;
        } else if (c.val[lits[0]] == 0) {
          assign(&c, lits[0]);
        }
      }
      continue;
    }
    if (!needed[k]) {
      continue;
    }
    checked++;
    if (!hint_refutes(&c, e)) {
      ok = 0;
      break;
    }
    if (e->n == 0) {
      refuted = 1;
    } else if (e->n == 1 && c.val[lits[0]] == 0) {
      assign(&c, lits[0]);
    } else if (e->n == 1 && c.val[lits[0]] == -1) {
      refuted = 1;
    }
  }
  if (lemmas_checked) {
    *lemmas_checked = checked;
  }
  free(needed);
  free(c.val);
  free(c.trail);
  free(c.entries);
  return ok && refuted;
}
