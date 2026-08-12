/* Observational-equivalence node features (OBS) -- the C side of the contract
 * defined by tools/mlopt/obs.py.
 *
 * Each pure instruction is evaluated on NPROBE deterministic pseudo-random
 * assignments to the function's leaves; the resulting NPROBE*64 bits are the
 * node's fingerprint. Nodes computing the same value get bit-identical
 * fingerprints however they are written, so `x*2`, `x+x`, and `x<<1` become one
 * node as far as the model is concerned. A frozen +-1 projection reduces the 512
 * bits to NPROJ bounded floats; four derived scalars follow.
 *
 * THIS FILE MUST AGREE WITH obs.py BIT FOR BIT. The model is trained on features
 * produced there and runs on features produced here; a divergence does not fail
 * loudly, it just means the model reads different inputs at compile time than it
 * trained on. That is why the PRNG, the name hash, the projection matrix, the
 * arithmetic, and the opaque-value rules are all pinned, and why
 * tools/mlopt/obs_golden.txt exists: ml_obs_selftest() replays it and reports
 * the first disagreement rather than leaving it to be discovered as a quiet
 * accuracy loss.
 *
 * This is a FEATURE, not a proof. Fingerprint agreement is evidence of value
 * equality; the interpreter differential in ml_opt.c remains the only authority
 * on whether a rewrite is sound.
 */
#include "ml_obs.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SMALL_MOD 4u
#define NSMALL 5 /* probes 0..NSMALL-1 are structured; the rest full width */

static const uint64_t PROJ_SEED = 0x9E3779B97F4A7C15ULL;
static const uint64_t PROBE_SEED = 0xD1B54A32D192ED03ULL;
static const uint64_t OPAQUE_SEED = 0xA24BAED4963EE407ULL;
static const uint64_t GOLDEN = 0x9E3779B97F4A7C15ULL;

/* ---------------- primitives (pinned; see obs.py) ---------------- */

uint64_t ml_obs_splitmix64(uint64_t x) {
  x += GOLDEN;
  uint64_t z = x;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

uint64_t ml_obs_fnv1a64(const char *s) {
  uint64_t h = 0xCBF29CE484222325ULL;
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    h = (h ^ (uint64_t)*p) * 0x100000001B3ULL;
  }
  return h;
}

static uint64_t probe_salt(int k) {
  return ml_obs_splitmix64(PROBE_SEED ^ ((uint64_t)(k + 1) * GOLDEN));
}

/* Probe k for a leaf with name-hash h. Probes 0 and 1 are degenerate on purpose:
 * with only full-width random leaves, two random 64-bit values are never equal
 * and never ordered close, so every comparison evaluates to 0 on every probe and
 * `x == y`, `x < y`, and the literal 0 collapse to one fingerprint. */
static uint64_t mix(uint64_t h, int k) {
  if (k == 0) return 0;
  if (k == 1) return 1;
  uint64_t v = ml_obs_splitmix64(h ^ probe_salt(k));
  return k < NSMALL ? v % SMALL_MOD : v;
}

static void leaf_values(const char *name, uint64_t out[ML_OBS_NPROBE]) {
  uint64_t h = ml_obs_fnv1a64(name);
  for (int k = 0; k < ML_OBS_NPROBE; k++) out[k] = mix(h, k);
}

/* Keyed by name AND index: two calls to the same function must not be assumed
 * to return the same value, since either may have side effects. */
static void opaque_values(const char *name, int idx, uint64_t out[ML_OBS_NPROBE]) {
  uint64_t h = ml_obs_fnv1a64(name) ^ ml_obs_splitmix64(OPAQUE_SEED ^ (uint64_t)idx);
  for (int k = 0; k < ML_OBS_NPROBE; k++) out[k] = mix(h, k);
}

/* ---------------- projection ---------------- */

static uint64_t g_proj[ML_OBS_NPROJ][ML_OBS_NPROBE];
static int g_proj_ready = 0;

/* Generated from a fixed seed rather than shipped in the weight blob: both sides
 * regenerate the identical matrix from the same splitmix64 stream. */
static void proj_init(void) {
  if (g_proj_ready) return;
  uint64_t state = PROJ_SEED;
  for (int r = 0; r < ML_OBS_NPROJ; r++) {
    for (int w = 0; w < ML_OBS_NPROBE; w++) {
      state += 1;
      g_proj[r][w] = ml_obs_splitmix64(state);
    }
  }
  g_proj_ready = 1;
}

void ml_obs_projection_row(int r, uint64_t out[ML_OBS_NPROBE]) {
  proj_init();
  for (int w = 0; w < ML_OBS_NPROBE; w++) out[w] = g_proj[r][w];
}

static int popcount64(uint64_t x) {
#if defined(__GNUC__) || defined(__clang__)
  return __builtin_popcountll(x);
#else
  x = x - ((x >> 1) & 0x5555555555555555ULL);
  x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
  x = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
  return (int)((x * 0x0101010101010101ULL) >> 56);
#endif
}

/* Row and fingerprint read as +-1 vectors: their dot product is
 * 512 - 2*popcount(row XOR fp). Scale by 1/sqrt(512), bound with tanh. */
static void project(const uint64_t fp[ML_OBS_NPROBE], float *out) {
  proj_init();
  for (int r = 0; r < ML_OBS_NPROJ; r++) {
    int pc = 0;
    for (int w = 0; w < ML_OBS_NPROBE; w++) pc += popcount64(g_proj[r][w] ^ fp[w]);
    int dot = ML_OBS_NBITS - 2 * pc;
    /* Double precision then narrow, matching Python's math.tanh followed by the
     * float32 store. tanhf() on the already-narrowed quotient differs in the
     * last ulp, which is invisible against the golden vectors' 2e-6 tolerance
     * but is enough to flip a near-tied argmax: it cost exactly one node out of
     * 17043 in the trained-model cross-check. */
    out[r] = (float)tanh((double)dot / 22.627416997969522);
  }
}

/* ---------------- lexing ---------------- */

static char *dupstr(const char *s) {
  size_t n = strlen(s) + 1;
  char *d = malloc(n);
  if (d) memcpy(d, s, n);
  return d;
}

static int starts(const char *s, const char *p) {
  return strncmp(s, p, strlen(p)) == 0;
}

static int is_control(const char *s) {
  return starts(s, "label ") || starts(s, "jump ") || starts(s, "branch") ||
         starts(s, "return ") || starts(s, "local ") || s[0] == '*';
}

static int is_lit(const char *s) {
  if (!*s) return 0;
  const char *p = s;
  if (*p == '-') p++;
  if (!*p) return 0;
  for (; *p; p++)
    if (!isdigit((unsigned char)*p)) return 0;
  return 1;
}

/* ^[%@][A-Za-z0-9_.$]*$ -- a bare IR name. Deliberately NOT "any token without
 * whitespace": `__acrt_iob_func(2)` contains no space, and accepting it as a
 * copy source makes two distinct calls alias to one leaf value. */
static int is_name(const char *s) {
  if (s[0] != '%' && s[0] != '@') return 0;
  for (const char *p = s + 1; *p; p++) {
    if (!isalnum((unsigned char)*p) && *p != '_' && *p != '.' && *p != '$')
      return 0;
  }
  return 1;
}

/* ^(\S+)\s*(=|<-|\+=)\s*(.*)$ over a non-control line. */
static int split_def(const char *s, char *dest, size_t dcap, char *eq,
                     size_t ecap, char *rhs, size_t rcap) {
  if (is_control(s)) return 0;
  const char *p = s;
  while (*p && !isspace((unsigned char)*p)) p++;
  size_t dlen = (size_t)(p - s);
  if (dlen == 0 || dlen >= dcap) return 0;
  const char *q = p;
  while (*q && isspace((unsigned char)*q)) q++;
  const char *op = NULL;
  size_t oplen = 0;
  if (starts(q, "<-")) { op = "<-"; oplen = 2; }
  else if (starts(q, "+=")) { op = "+="; oplen = 2; }
  else if (q[0] == '=' && q[1] != '=') { op = "="; oplen = 1; }
  if (!op) {
    /* no space before the operator: `@x=1` */
    const char *e = memchr(s, '=', dlen);
    if (!e || e == s || e[1] == '=') return 0;
    size_t nd = (size_t)(e - s);
    if (nd >= dcap) return 0;
    memcpy(dest, s, nd);
    dest[nd] = 0;
    snprintf(eq, ecap, "=");
    const char *r = e + 1;
    while (*r && isspace((unsigned char)*r)) r++;
    snprintf(rhs, rcap, "%s", r);
    return 1;
  }
  memcpy(dest, s, dlen);
  dest[dlen] = 0;
  snprintf(eq, ecap, "%s", op);
  const char *r = q + oplen;
  while (*r && isspace((unsigned char)*r)) r++;
  snprintf(rhs, rcap, "%s", r);
  return 1;
}

static const char *const OPS[] = {"<<", ">>", "+",  "-",  "*",  "/",
                                  "%",  "&",  "|",  "^",  "==", "!=",
                                  "<=", ">=", "<",  ">"};
#define NOPS ((int)(sizeof(OPS) / sizeof(OPS[0])))

/* ^(\S+) (op) (\S+)$ -- exactly three whitespace-separated tokens. */
static int three_token(const char *rhs, char *a, size_t acap, char *op,
                       size_t ocap, char *b, size_t bcap) {
  const char *p = rhs;
  const char *a0 = p;
  while (*p && !isspace((unsigned char)*p)) p++;
  size_t alen = (size_t)(p - a0);
  if (!alen || alen >= acap || !*p) return 0;
  while (*p && isspace((unsigned char)*p)) p++;
  const char *o0 = p;
  while (*p && !isspace((unsigned char)*p)) p++;
  size_t olen = (size_t)(p - o0);
  if (!olen || olen >= ocap || !*p) return 0;
  while (*p && isspace((unsigned char)*p)) p++;
  const char *b0 = p;
  while (*p && !isspace((unsigned char)*p)) p++;
  size_t blen = (size_t)(p - b0);
  if (!blen || blen >= bcap) return 0;
  while (*p && isspace((unsigned char)*p)) p++;
  if (*p) return 0; /* a fourth token: not a binary form */
  memcpy(a, a0, alen); a[alen] = 0;
  memcpy(op, o0, olen); op[olen] = 0;
  memcpy(b, b0, blen); b[blen] = 0;
  for (int i = 0; i < NOPS; i++)
    if (strcmp(op, OPS[i]) == 0) return 1;
  return 0;
}

static int64_t to_signed(uint64_t v) { return (int64_t)v; }

/* uint64 semantics matching sopt.evalop, plus signed comparisons. Division and
 * modulo by zero yield 0 (the interpreter's convention), so no fingerprint
 * depends on trap behaviour. */
static int apply_op(const char *op, uint64_t a, uint64_t b, uint64_t *out) {
  if (!strcmp(op, "+")) { *out = a + b; return 1; }
  if (!strcmp(op, "-")) { *out = a - b; return 1; }
  if (!strcmp(op, "*")) { *out = a * b; return 1; }
  if (!strcmp(op, "&")) { *out = a & b; return 1; }
  if (!strcmp(op, "|")) { *out = a | b; return 1; }
  if (!strcmp(op, "^")) { *out = a ^ b; return 1; }
  if (!strcmp(op, "<<")) { *out = a << (b & 63); return 1; }
  if (!strcmp(op, ">>")) { *out = a >> (b & 63); return 1; }
  if (!strcmp(op, "/")) { *out = b ? a / b : 0; return 1; }
  if (!strcmp(op, "%")) { *out = b ? a % b : 0; return 1; }
  if (!strcmp(op, "==")) { *out = a == b; return 1; }
  if (!strcmp(op, "!=")) { *out = a != b; return 1; }
  if (!strcmp(op, "<")) { *out = to_signed(a) < to_signed(b); return 1; }
  if (!strcmp(op, "<=")) { *out = to_signed(a) <= to_signed(b); return 1; }
  if (!strcmp(op, ">")) { *out = to_signed(a) > to_signed(b); return 1; }
  if (!strcmp(op, ">=")) { *out = to_signed(a) >= to_signed(b); return 1; }
  return 0;
}

/* ---------------- environment ---------------- */

/* Open-addressing index over the environment. A linear scan is the obvious
 * implementation and it is quadratic: every operand of every instruction walks
 * every name defined so far, which on an 800-node function is over a million
 * string comparisons. Measured before this index, featurization cost grew from
 * 0.7 to 4.6 microseconds per node between n=50 and n=800. Compile speed is a
 * headline number for this compiler, so the index is not optional. */
typedef struct {
  char **name;
  uint64_t (*val)[ML_OBS_NPROBE];
  unsigned char *is_leaf;
  int n, cap;
  int *slot;          /* hash -> index+1, 0 = empty */
  int nslot;
} Env;

static void env_free(Env *e) {
  for (int i = 0; i < e->n; i++) free(e->name[i]);
  free(e->name); free(e->val); free(e->is_leaf); free(e->slot);
  memset(e, 0, sizeof *e);
}

static void env_rehash(Env *e, int nslot) {
  free(e->slot);
  e->slot = calloc((size_t)nslot, sizeof(int));
  e->nslot = e->slot ? nslot : 0;
  if (!e->slot) return;
  for (int i = 0; i < e->n; i++) {
    uint64_t h = ml_obs_fnv1a64(e->name[i]);
    int m = e->nslot - 1;
    int j = (int)(h & (uint64_t)m);
    while (e->slot[j]) j = (j + 1) & m;
    e->slot[j] = i + 1;
  }
}

static int env_find(Env *e, const char *name) {
  if (!e->slot) return -1;
  uint64_t h = ml_obs_fnv1a64(name);
  int m = e->nslot - 1;
  int j = (int)(h & (uint64_t)m);
  while (e->slot[j]) {
    int i = e->slot[j] - 1;
    if (strcmp(e->name[i], name) == 0) return i;
    j = (j + 1) & m;
  }
  return -1;
}

static int env_put(Env *e, const char *name, const uint64_t v[ML_OBS_NPROBE],
                   int leaf) {
  int i = env_find(e, name);
  if (i < 0) {
    if (e->n == e->cap) {
      int nc = e->cap ? e->cap * 2 : 32;
      char **nn = realloc(e->name, (size_t)nc * sizeof *nn);
      uint64_t (*nv)[ML_OBS_NPROBE] = realloc(e->val, (size_t)nc * sizeof *nv);
      unsigned char *nl = realloc(e->is_leaf, (size_t)nc * sizeof *nl);
      if (!nn || !nv || !nl) {
        free(nn); free(nv); free(nl);
        return -1;
      }
      e->name = nn; e->val = nv; e->is_leaf = nl; e->cap = nc;
    }
    i = e->n++;
    e->name[i] = dupstr(name);
    if (!e->name[i]) { e->n--; return -1; }
    if (e->nslot < 2 * e->n + 8) {
      int ns = 16;
      while (ns < 4 * (e->n + 1)) ns <<= 1;
      env_rehash(e, ns);
    } else {
      uint64_t h = ml_obs_fnv1a64(name);
      int m = e->nslot - 1;
      int j = (int)(h & (uint64_t)m);
      while (e->slot[j]) j = (j + 1) & m;
      e->slot[j] = i + 1;
    }
  }
  memcpy(e->val[i], v, sizeof e->val[i]);
  e->is_leaf[i] = (unsigned char)leaf;
  return i;
}

/* ---------------- mutable-name analysis ---------------- */

/* Same story as Env: hashed, because the mutable-name pass tests every `@` token
 * of every call and store against the whole set. */
typedef struct { char **a; int n, cap; int *slot; int nslot; } StrSet;

static void ss_rehash(StrSet *s, int nslot) {
  free(s->slot);
  s->slot = calloc((size_t)nslot, sizeof(int));
  s->nslot = s->slot ? nslot : 0;
  if (!s->slot) return;
  for (int i = 0; i < s->n; i++) {
    int m = s->nslot - 1;
    int j = (int)(ml_obs_fnv1a64(s->a[i]) & (uint64_t)m);
    while (s->slot[j]) j = (j + 1) & m;
    s->slot[j] = i + 1;
  }
}

static int ss_has(StrSet *s, const char *x) {
  if (!s->slot) return 0;
  int m = s->nslot - 1;
  int j = (int)(ml_obs_fnv1a64(x) & (uint64_t)m);
  while (s->slot[j]) {
    if (strcmp(s->a[s->slot[j] - 1], x) == 0) return 1;
    j = (j + 1) & m;
  }
  return 0;
}

static void ss_add(StrSet *s, const char *x) {
  if (ss_has(s, x)) return;
  if (s->n == s->cap) {
    int nc = s->cap ? s->cap * 2 : 16;
    char **na = realloc(s->a, (size_t)nc * sizeof *na);
    if (!na) return;
    s->a = na; s->cap = nc;
  }
  s->a[s->n] = dupstr(x);
  if (!s->a[s->n]) return;
  s->n++;
  if (s->nslot < 2 * s->n + 8) {
    int ns = 16;
    while (ns < 4 * (s->n + 1)) ns <<= 1;
    ss_rehash(s, ns);
  } else {
    int m = s->nslot - 1;
    int j = (int)(ml_obs_fnv1a64(x) & (uint64_t)m);
    while (s->slot[j]) j = (j + 1) & m;
    s->slot[j] = s->n;
  }
}

static void ss_free(StrSet *s) {
  for (int i = 0; i < s->n; i++) free(s->a[i]);
  free(s->a); free(s->slot);
  memset(s, 0, sizeof *s);
}

static int has_call(const char *s) {
  for (const char *p = s; *p; p++) {
    if (*p != '(') continue;
    const char *q = p;
    while (q > s && isspace((unsigned char)q[-1])) q--;
    if (q > s && (isalnum((unsigned char)q[-1]) || q[-1] == '_')) {
      const char *r = q - 1;
      while (r > s && (isalnum((unsigned char)r[-1]) || r[-1] == '_')) r--;
      if (isalpha((unsigned char)*r) || *r == '_') return 1;
    }
  }
  return 0;
}

/* Names whose value straight-line evaluation cannot be trusted to know:
 * defined more than once (a scan cannot model a loop back edge), or passed to a
 * call / touched by a store (a callee writing through a pointer is invisible to
 * a def-only scan, so `@counter <- 0` followed by such a call would leave the
 * evaluator believing @counter is still 0 and collapse everything downstream of
 * it to zero). */
static void mutable_names(char **texts, int n, StrSet *mut) {
  StrSet seen = {0};
  for (int i = 0; i < n; i++) {
    const char *s = texts[i];
    if (s[0] == '*' || has_call(s)) {
      for (const char *p = s; *p; p++) {
        if (*p != '@') continue;
        const char *q = p + 1;
        while (*q && (isalnum((unsigned char)*q) || *q == '_' || *q == '.' ||
                      *q == '$')) q++;
        size_t len = (size_t)(q - p);
        char tok[256];
        if (len < sizeof tok) {
          memcpy(tok, p, len); tok[len] = 0;
          ss_add(mut, tok);
          char *dot = strchr(tok + 1, '.');
          if (dot) { *dot = 0; ss_add(mut, tok); }
        }
        p = q - 1;
      }
    }
    char dest[256], eq[8], rhs[1024];
    if (!split_def(s, dest, sizeof dest, eq, sizeof eq, rhs, sizeof rhs))
      continue;
    if (dest[0] != '%' && dest[0] != '@') continue;
    if (ss_has(&seen, dest)) ss_add(mut, dest);
    ss_add(&seen, dest);
  }
  ss_free(&seen);
}

/* ---------------- fingerprints ---------------- */

static int operand(Env *env, const char *tok, uint64_t out[ML_OBS_NPROBE]) {
  if (is_lit(tok)) {
    long long sv = atoll(tok);
    uint64_t v = (uint64_t)sv;
    for (int k = 0; k < ML_OBS_NPROBE; k++) out[k] = v;
    return 1;
  }
  if (!is_name(tok)) return 0;
  int i = env_find(env, tok);
  if (i < 0) {
    uint64_t lv[ML_OBS_NPROBE];
    leaf_values(tok, lv);
    i = env_put(env, tok, lv, 1);
    if (i < 0) return 0;
  }
  memcpy(out, env->val[i], sizeof(uint64_t) * ML_OBS_NPROBE);
  return 1;
}

int ml_obs_fingerprints(char **texts, int n, MlObsFp *fps, MlObsFp **leaves,
                        int *nleaves) {
  Env env = {0};
  StrSet mut = {0};
  mutable_names(texts, n, &mut);
  for (int i = 0; i < n; i++) fps[i].valid = 0;

  for (int i = 0; i < n; i++) {
    char dest[256], eq[8], rhs[1024];
    if (!split_def(texts[i], dest, sizeof dest, eq, sizeof eq, rhs, sizeof rhs))
      continue;
    if (dest[0] != '%' && dest[0] != '@') continue;

    if (ss_has(&mut, dest)) { /* loop-carried or aliased: one stable leaf value */
      if (env_find(&env, dest) < 0) {
        uint64_t lv[ML_OBS_NPROBE];
        leaf_values(dest, lv);
        env_put(&env, dest, lv, 1);
      }
      continue;
    }

    uint64_t vals[ML_OBS_NPROBE];
    int have = 0;
    char a[256], op[8], b[256];
    if (three_token(rhs, a, sizeof a, op, sizeof op, b, sizeof b)) {
      uint64_t av[ML_OBS_NPROBE], bv[ML_OBS_NPROBE];
      if (operand(&env, a, av) && operand(&env, b, bv)) {
        have = 1;
        for (int k = 0; k < ML_OBS_NPROBE && have; k++)
          have = apply_op(op, av[k], bv[k], &vals[k]);
      }
    } else if (strcmp(eq, "+=") == 0) {
      uint64_t cv[ML_OBS_NPROBE], av[ML_OBS_NPROBE];
      if (operand(&env, dest, cv) && operand(&env, rhs, av)) {
        have = 1;
        for (int k = 0; k < ML_OBS_NPROBE; k++) vals[k] = cv[k] + av[k];
      }
    } else {
      have = operand(&env, rhs, vals);
    }

    if (!have) { /* call, load, or a form we cannot read */
      uint64_t ov[ML_OBS_NPROBE];
      opaque_values(dest, i, ov);
      env_put(&env, dest, ov, 1);
      continue;
    }
    env_put(&env, dest, vals, 0);
    memcpy(fps[i].v, vals, sizeof vals);
    fps[i].valid = 1;
  }

  /* leaf fingerprints, for the eq_leaf scalar */
  if (leaves && nleaves) {
    int nleaf = 0;
    for (int i = 0; i < env.n; i++) nleaf += env.is_leaf[i] ? 1 : 0;
    *leaves = nleaf ? malloc((size_t)nleaf * sizeof(MlObsFp)) : NULL;
    *nleaves = *leaves ? nleaf : 0;
    int j = 0;
    for (int i = 0; i < env.n && *leaves; i++) {
      if (!env.is_leaf[i]) continue;
      memcpy((*leaves)[j].v, env.val[i], sizeof env.val[i]);
      (*leaves)[j].valid = 1;
      j++;
    }
  }

  ss_free(&mut);
  env_free(&env);
  return 1;
}

static uint64_t fp_hash(const MlObsFp *f) {
  uint64_t h = 0xCBF29CE484222325ULL;
  for (int k = 0; k < ML_OBS_NPROBE; k++)
    h = (h ^ f->v[k]) * 0x100000001B3ULL;
  return h ? h : 1;
}

static int fp_eq(const MlObsFp *a, const MlObsFp *b) {
  return memcmp(a->v, b->v, sizeof a->v) == 0;
}

int ml_obs_edge_eligible(const MlObsFp *f) {
  if (!f->valid) return 0;
  int allsame = 1;
  for (int k = 1; k < ML_OBS_NPROBE; k++)
    if (f->v[k] != f->v[0]) { allsame = 0; break; }
  if (allsame) return 0;            /* constant: would form a clique */
  for (int k = 0; k < ML_OBS_NPROBE; k++)
    if (f->v[k] > 1) return 1;
  return 0;                         /* boolean: one bit, collides constantly */
}

void ml_obs_features(char **texts, int n, float *out) {
  memset(out, 0, (size_t)n * ML_OBS_NOBS * sizeof(float));
  MlObsFp *fps = calloc((size_t)n, sizeof *fps);
  if (!fps) return;
  MlObsFp *leaves = NULL;
  int nleaves = 0;
  ml_obs_fingerprints(texts, n, fps, &leaves, &nleaves);

  /* `eq_leaf` and `dup_earlier` are both set-membership questions over 512-bit
   * fingerprints. Done directly they are O(n * leaves) and O(n^2); hashing the
   * fingerprint makes both linear. */
  uint64_t *lh = nleaves ? malloc((size_t)nleaves * sizeof(uint64_t)) : NULL;
  for (int l = 0; l < nleaves; l++) lh[l] = fp_hash(&leaves[l]);
  uint64_t *sh = malloc((size_t)(n ? n : 1) * sizeof(uint64_t));
  int *sidx = malloc((size_t)(n ? n : 1) * sizeof(int));
  int nseen = 0;

  for (int i = 0; i < n; i++) {
    if (!fps[i].valid) continue;
    float *row = out + (size_t)i * ML_OBS_NOBS;
    project(fps[i].v, row);
    int allsame = 1;
    for (int k = 1; k < ML_OBS_NPROBE; k++)
      if (fps[i].v[k] != fps[i].v[0]) { allsame = 0; break; }
    uint64_t hh = fp_hash(&fps[i]);
    int eqleaf = 0;
    for (int l = 0; l < nleaves; l++)
      if (lh[l] == hh && fp_eq(&fps[i], &leaves[l])) { eqleaf = 1; break; }
    int zero = 1;
    for (int k = 0; k < ML_OBS_NPROBE; k++)
      if (fps[i].v[k] != 0) { zero = 0; break; }
    int dup = 0;
    for (int p = 0; p < nseen; p++)
      if (sh[p] == hh && fp_eq(&fps[i], &fps[sidx[p]])) { dup = 1; break; }
    sh[nseen] = hh; sidx[nseen] = i; nseen++;
    row[ML_OBS_NPROJ + 0] = allsame ? 1.0f : 0.0f;
    row[ML_OBS_NPROJ + 1] = eqleaf ? 1.0f : 0.0f;
    row[ML_OBS_NPROJ + 2] = zero ? 1.0f : 0.0f;
    row[ML_OBS_NPROJ + 3] = dup ? 1.0f : 0.0f;
  }
  free(lh); free(sh); free(sidx);
  free(leaves);
  free(fps);
}

/* ---------------- golden-vector self-test ---------------- */

/* Replays tools/mlopt/obs_golden.txt, which obs_golden.py generates from the
 * Python featurizer. A mismatch here means the model is about to read different
 * inputs at compile time than it trained on, which is otherwise invisible: it
 * shows up as unexplained accuracy loss, not as a crash. */
static int gold_fail(int *bad, const char *what, const char *detail) {
  if (*bad < 5) fprintf(stderr, "ml_obs: GOLDEN MISMATCH %s: %s\n", what, detail);
  (*bad)++;
  return 0;
}

int ml_obs_selftest(const char *golden_path) {
  FILE *f = fopen(golden_path, "r");
  if (!f) {
    fprintf(stderr, "ml_obs: cannot open %s\n", golden_path);
    return -1;
  }
  char line[8192];
  int bad = 0, checked = 0;
  enum { S_NONE, S_SPLIT, S_FNV, S_LEAF, S_OPAQUE, S_PROJ, S_BODY } sec = S_NONE;

  char *body[512];
  int nbody = 0;
  MlObsFp *fps = NULL;
  float *feats = NULL;

  while (fgets(line, sizeof line, f)) {
    char *nl = strchr(line, '\n');
    if (nl) *nl = 0;
    if (!line[0] || line[0] == '#') continue;

    if (line[0] == '[') {
      /* finishing a body section: run the featurizer and compare below */
      if (sec == S_BODY && nbody) {
        for (int i = 0; i < nbody; i++) free(body[i]);
        nbody = 0;
        free(fps); fps = NULL;
        free(feats); feats = NULL;
      }
      if (starts(line, "[splitmix64]")) sec = S_SPLIT;
      else if (starts(line, "[fnv1a64]")) sec = S_FNV;
      else if (starts(line, "[leaf_values]")) sec = S_LEAF;
      else if (starts(line, "[opaque_values]")) sec = S_OPAQUE;
      else if (starts(line, "[projection]")) sec = S_PROJ;
      else if (starts(line, "[body")) sec = S_BODY;
      else sec = S_NONE;
      continue;
    }

    if (sec == S_SPLIT) {
      uint64_t in, want;
      if (sscanf(line, "%16llx %16llx", (unsigned long long *)&in,
                 (unsigned long long *)&want) == 2) {
        checked++;
        if (ml_obs_splitmix64(in) != want) {
          char d[128];
          snprintf(d, sizeof d, "splitmix64(%016llx)=%016llx want %016llx",
                   (unsigned long long)in,
                   (unsigned long long)ml_obs_splitmix64(in),
                   (unsigned long long)want);
          gold_fail(&bad, "splitmix64", d);
        }
      }
    } else if (sec == S_FNV) {
      /* `'@x' 090c5007b5a4b485` -- the name is a Python repr */
      char name[256];
      uint64_t want = 0;
      char *open = strchr(line, '\'');
      char *close = open ? strrchr(line, '\'') : NULL;
      if (open && close && close > open &&
          (size_t)(close - open - 1) < sizeof name) {
        size_t len = (size_t)(close - open - 1);
        memcpy(name, open + 1, len);
        name[len] = 0;
        if (sscanf(close + 1, " %16llx", (unsigned long long *)&want) == 1) {
          checked++;
          if (ml_obs_fnv1a64(name) != want) {
            char d[320];
            snprintf(d, sizeof d, "fnv1a64('%s')=%016llx want %016llx", name,
                     (unsigned long long)ml_obs_fnv1a64(name),
                     (unsigned long long)want);
            gold_fail(&bad, "fnv1a64", d);
          }
        }
      }
    } else if (sec == S_LEAF || sec == S_OPAQUE) {
      /* `@x v0 v1 ...` or `%q 0 v0 v1 ...` */
      char name[256];
      int idx = 0;
      int got = sec == S_OPAQUE
                    ? sscanf(line, "%255s %d", name, &idx)
                    : sscanf(line, "%255s", name);
      if (got >= 1) {
        uint64_t v[ML_OBS_NPROBE];
        if (sec == S_OPAQUE) opaque_values(name, idx, v);
        else leaf_values(name, v);
        const char *p = strchr(line, ' ');
        if (sec == S_OPAQUE && p) p = strchr(p + 1, ' ');
        for (int k = 0; k < ML_OBS_NPROBE && p; k++) {
          uint64_t want = 0;
          if (sscanf(p + 1, "%16llx", (unsigned long long *)&want) != 1) break;
          checked++;
          if (v[k] != want) {
            char d[400];
            snprintf(d, sizeof d, "%s %s probe %d = %016llx want %016llx",
                     sec == S_OPAQUE ? "opaque" : "leaf", name, k,
                     (unsigned long long)v[k], (unsigned long long)want);
            gold_fail(&bad, "values", d);
            break;
          }
          p = strchr(p + 1, ' ');
        }
      }
    } else if (sec == S_PROJ) {
      int r;
      if (sscanf(line, "%d", &r) == 1 && r >= 0 && r < ML_OBS_NPROJ) {
        uint64_t row[ML_OBS_NPROBE];
        ml_obs_projection_row(r, row);
        const char *p = strchr(line, ' ');
        for (int w = 0; w < ML_OBS_NPROBE && p; w++) {
          uint64_t want = 0;
          if (sscanf(p + 1, "%16llx", (unsigned long long *)&want) != 1) break;
          checked++;
          if (row[w] != want) {
            char d[128];
            snprintf(d, sizeof d, "proj[%d][%d]=%016llx want %016llx", r, w,
                     (unsigned long long)row[w], (unsigned long long)want);
            gold_fail(&bad, "projection", d);
          }
          p = strchr(p + 1, ' ');
        }
      }
    } else if (sec == S_BODY) {
      if (starts(line, "| ")) {
        if (nbody < (int)(sizeof body / sizeof body[0]))
          body[nbody++] = dupstr(line + 2);
        continue;
      }
      if (!fps && nbody) {
        fps = calloc((size_t)nbody, sizeof *fps);
        feats = calloc((size_t)nbody * ML_OBS_NOBS, sizeof *feats);
        if (fps) ml_obs_fingerprints(body, nbody, fps, NULL, NULL);
        if (feats) ml_obs_features(body, nbody, feats);
      }
      int idx;
      if (starts(line, "fp ") && fps &&
          sscanf(line + 3, "%d", &idx) == 1 && idx >= 0 && idx < nbody) {
        const char *rest = strchr(line + 3, ' ');
        if (rest && starts(rest + 1, "none")) {
          checked++;
          if (fps[idx].valid) {
            char d[64];
            snprintf(d, sizeof d, "instr %d has a fingerprint, want none", idx);
            gold_fail(&bad, "fp", d);
          }
        } else if (rest) {
          checked++;
          if (!fps[idx].valid) {
            char d[64];
            snprintf(d, sizeof d, "instr %d has no fingerprint, want one", idx);
            gold_fail(&bad, "fp", d);
          } else {
            const char *p = rest;
            for (int k = 0; k < ML_OBS_NPROBE && p; k++) {
              uint64_t want = 0;
              if (sscanf(p + 1, "%16llx", (unsigned long long *)&want) != 1) break;
              if (fps[idx].v[k] != want) {
                char d[160];
                snprintf(d, sizeof d,
                         "instr %d probe %d = %016llx want %016llx", idx, k,
                         (unsigned long long)fps[idx].v[k],
                         (unsigned long long)want);
                gold_fail(&bad, "fp", d);
                break;
              }
              p = strchr(p + 1, ' ');
            }
          }
        }
      } else if (starts(line, "sedge") && nbody) {
        /* Value-equality edges. Features agreeing does not imply edges agreeing,
         * and the edges are what message passing actually moves information
         * along, so they need their own check. */
        int *es = malloc((size_t)nbody * sizeof(int));
        int *ed = malloc((size_t)nbody * sizeof(int));
        int ne = (es && ed) ? ml_obs_semantic_edges(body, nbody, es, ed) : 0;
        int want_n = 0;
        const char *p = line + 5;
        while (*p) {
          while (*p == ' ') p++;
          if (!*p) break;
          int a = 0, b = 0;
          if (sscanf(p, "%d->%d", &a, &b) == 2) {
            checked++;
            if (want_n >= ne || es[want_n] != a || ed[want_n] != b) {
              char d[160];
              snprintf(d, sizeof d, "edge %d: got %d->%d want %d->%d", want_n,
                       want_n < ne ? es[want_n] : -1,
                       want_n < ne ? ed[want_n] : -1, a, b);
              gold_fail(&bad, "sedge", d);
              break;
            }
            want_n++;
          }
          while (*p && *p != ' ') p++;
        }
        checked++;
        if (want_n != ne) {
          char d[96];
          snprintf(d, sizeof d, "edge count: got %d want %d", ne, want_n);
          gold_fail(&bad, "sedge", d);
        }
        free(es); free(ed);
      } else if (starts(line, "ft ") && feats &&
                 sscanf(line + 3, "%d", &idx) == 1 && idx >= 0 && idx < nbody) {
        const char *p = strchr(line + 3, ' ');
        for (int c = 0; c < ML_OBS_NOBS && p; c++) {
          double want = atof(p + 1);
          double got = feats[(size_t)idx * ML_OBS_NOBS + c];
          checked++;
          if (fabs(got - want) > 2e-6) {
            char d[160];
            snprintf(d, sizeof d, "instr %d feat %d = %+.6f want %+.6f", idx, c,
                     got, want);
            gold_fail(&bad, "ft", d);
            break;
          }
          p = strchr(p + 1, ' ');
        }
      }
    }
  }
  for (int i = 0; i < nbody; i++) free(body[i]);
  free(fps);
  free(feats);
  fclose(f);
  if (bad)
    fprintf(stderr, "ml_obs: %d mismatches over %d golden values\n", bad, checked);
  else
    fprintf(stderr, "ml_obs: golden self-test OK (%d values)\n", checked);
  return bad;
}

int ml_obs_semantic_edges(char **texts, int n, int *src, int *dst) {
  MlObsFp *fps = calloc((size_t)n, sizeof *fps);
  if (!fps) return 0;
  ml_obs_fingerprints(texts, n, fps, NULL, NULL);
  int ne = 0;
  for (int i = 0; i < n; i++) {
    if (!ml_obs_edge_eligible(&fps[i])) continue;
    for (int p = i - 1; p >= 0; p--) {      /* nearest earlier identical value */
      if (!ml_obs_edge_eligible(&fps[p])) continue;
      if (fp_eq(&fps[i], &fps[p])) {
        src[ne] = p; dst[ne] = i; ne++;
        break;
      }
    }
  }
  free(fps);
  return ne;
}
