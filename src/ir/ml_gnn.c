#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NKIND 10
#define NOPS 17
#define NFEAT 9
#define NEDGE 8
#define DELETE_CLASS 1
#define AFFINE_CLASS 3
#define GVN_CLASS 4
#define COLLAPSE_CLASS 5

typedef struct { int *a; int n, cap; } VecI;
static void vi_push(VecI *v, int x) {
  if (v->n == v->cap) { v->cap = v->cap ? v->cap * 2 : 8;
    v->a = realloc(v->a, (size_t)v->cap * sizeof(int)); }
  v->a[v->n++] = x;
}
static void vi_free(VecI *v) { free(v->a); v->a = NULL; v->n = v->cap = 0; }

typedef struct { char **a; int n, cap; } VecS;
static void vs_push(VecS *v, char *s) {
  if (v->n == v->cap) { v->cap = v->cap ? v->cap * 2 : 8;
    v->a = realloc(v->a, (size_t)v->cap * sizeof(char *)); }
  v->a[v->n++] = s;
}
static void vs_free(VecS *v) {
  for (int i = 0; i < v->n; i++) free(v->a[i]);
  free(v->a); v->a = NULL; v->n = v->cap = 0;
}

typedef struct { char *s; size_t n, cap; } Buf;
static void buf_add(Buf *b, const char *t) {
  size_t l = strlen(t);
  if (b->n + l + 1 > b->cap) { b->cap = (b->n + l + 1) * 2 + 64;
    b->s = realloc(b->s, b->cap); }
  memcpy(b->s + b->n, t, l + 1); b->n += l;
}

static int starts(const char *s, const char *p) { return strncmp(s, p, strlen(p)) == 0; }
static int is_word(int c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
         (c >= '0' && c <= '9') || c == '_';
}

static void nth_token(const char *s, int n, char *out, size_t cap) {
  out[0] = 0;
  const char *p = s;
  for (int i = 0; ; i++) {
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) return;
    const char *st = p;
    while (*p && *p != ' ' && *p != '\t') p++;
    if (i == n) { size_t l = (size_t)(p - st); if (l >= cap) l = cap - 1;
      memcpy(out, st, l); out[l] = 0; return; }
  }
}

static int is_control(const char *s) {
  return starts(s, "label ") || starts(s, "jump ") || starts(s, "branch") ||
         starts(s, "return ") || starts(s, "local ") || s[0] == '*';
}

static int split_def(const char *s, char *dest, size_t dcap, char *rhs, size_t rcap) {
  if (is_control(s)) return 0;
  const char *p = s;
  while (*p == ' ') p++;
  const char *st = p;
  while (*p && *p != ' ') p++;
  size_t dl = (size_t)(p - st); if (dl == 0 || dl >= dcap) return 0;
  while (*p == ' ') p++;
  const char *op;
  if (p[0] == '<' && p[1] == '-') { op = p; p += 2; }
  else if (p[0] == '=') { op = p; p += 1; }
  else return 0;
  (void)op;
  while (*p == ' ') p++;
  size_t rl = strlen(p);
  while (rl > 0 && (p[rl - 1] == ' ' || p[rl - 1] == '\t')) rl--;
  if (rl >= rcap) return 0;
  memcpy(dest, st, dl); dest[dl] = 0;
  memcpy(rhs, p, rl); rhs[rl] = 0;
  return 1;
}

static const char *BIN_OPS[] = {"+", "-", "*", "&", "|", "^", "<<", ">>", "/", "%"};
static const char *CMP_OPS[] = {"==", "!=", "<", "<=", ">", ">="};

static int three_token(const char *rhs, char *a, size_t acap, char *op, size_t ocap,
                       char *b, size_t bcap) {
  char t0[256], t1[16], t2[256], extra[8];
  nth_token(rhs, 0, t0, sizeof t0);
  nth_token(rhs, 1, t1, sizeof t1);
  nth_token(rhs, 2, t2, sizeof t2);
  nth_token(rhs, 3, extra, sizeof extra);
  if (!t0[0] || !t1[0] || !t2[0] || extra[0]) return 0;
  int grp = 0;
  for (size_t i = 0; i < sizeof BIN_OPS / sizeof *BIN_OPS; i++)
    if (strcmp(t1, BIN_OPS[i]) == 0) grp = 1;
  if (!grp) for (size_t i = 0; i < sizeof CMP_OPS / sizeof *CMP_OPS; i++)
    if (strcmp(t1, CMP_OPS[i]) == 0) grp = 2;
  if (!grp) return 0;
  if (strlen(t0) >= acap || strlen(t1) >= ocap || strlen(t2) >= bcap) return 0;
  strcpy(a, t0); strcpy(op, t1); strcpy(b, t2);
  return grp;
}

/* True if `s` is a decimal literal that is a power of two >= 2 (shift/mask hint).
 * Mirrors gnn_model.py _operand_feats.pow2 for train/inference feature parity. */
static int is_pow2_str(const char *s) {
  if (!s[0]) return 0;
  for (const char *p = s; *p; p++) if (*p < '0' || *p > '9') return 0;
  long long v = atoll(s);
  return v >= 2 && (v & (v - 1)) == 0;
}

/* Five binary-operand features matching gnn_model.py _operand_feats EXACTLY (the
 * op embedding cannot express them): operands equal (a^a, (a^b)^b cancellation),
 * and whether an operand is the literal -1 (NOT mask), 0, 1, or a power of two.
 * Writes 5 floats into out[]. */
static void operand_feats(const char *ins, float out[5]) {
  for (int i = 0; i < 5; i++) out[i] = 0.f;
  char dest[256], rhs[512], a[256], o[16], b[256];
  if (!split_def(ins, dest, sizeof dest, rhs, sizeof rhs)) return;
  if (!three_token(rhs, a, sizeof a, o, sizeof o, b, sizeof b)) return;
  out[0] = strcmp(a, b) == 0 ? 1.f : 0.f;
  out[1] = (strcmp(a, "-1") == 0 || strcmp(b, "-1") == 0) ? 1.f : 0.f;
  out[2] = (strcmp(a, "0") == 0 || strcmp(b, "0") == 0) ? 1.f : 0.f;
  out[3] = (strcmp(a, "1") == 0 || strcmp(b, "1") == 0) ? 1.f : 0.f;
  out[4] = (is_pow2_str(a) || is_pow2_str(b)) ? 1.f : 0.f;
}

static int op_is_comm(const char *op) {
  return strcmp(op, "+") == 0 || strcmp(op, "*") == 0 || strcmp(op, "&") == 0 ||
         strcmp(op, "|") == 0 || strcmp(op, "^") == 0;
}

static int op_index(const char *op) {
  static const char *T[NOPS] = {"none", "+", "-", "*", "&", "|", "^", "<<", ">>",
                                "/", "%", "==", "!=", "<", "<=", ">", ">="};
  for (int i = 0; i < NOPS; i++) if (strcmp(op, T[i]) == 0) return i;
  return 0;
}

static int has_call(const char *rhs) {
  for (const char *p = rhs; *p; p++) {
    if (*p != '(') continue;
    const char *q = p;
    while (q > rhs && q[-1] == ' ') q--;
    if (q == rhs || !is_word((unsigned char)q[-1])) continue;
    const char *r = q;
    while (r > rhs && is_word((unsigned char)r[-1])) r--;
    if (r[0] >= '0' && r[0] <= '9') continue;
    return 1;
  }
  return 0;
}

static void classify(const char *s, int *kind, int *op) {
  *op = 0;
  if (starts(s, "label ")) { *kind = 0; return; }
  if (starts(s, "jump ")) { *kind = 1; return; }
  if (starts(s, "branch")) { *kind = 2; return; }
  if (starts(s, "return ")) { *kind = 3; return; }
  if (starts(s, "local ")) { *kind = 4; return; }
  if (s[0] == '*') { *kind = 5; return; }
  char dest[256], rhs[512];
  if (!split_def(s, dest, sizeof dest, rhs, sizeof rhs)) { *kind = 9; return; }
  if (has_call(rhs)) { *kind = 8; return; }
  char a[256], o[16], b[256];
  if (three_token(rhs, a, sizeof a, o, sizeof o, b, sizeof b)) {
    *kind = 6; *op = op_index(o); return;
  }
  *kind = 7;
}

static void names_collect(const char *s, VecS *out) {
  for (const char *p = s; *p; ) {
    if (*p == '%' || *p == '@') {
      const char *st = p++;
      while (*p && ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                    (*p >= '0' && *p <= '9') || *p == '_' || *p == '.' || *p == '$'))
        p++;
      size_t l = (size_t)(p - st);
      char *t = malloc(l + 1); memcpy(t, st, l); t[l] = 0; vs_push(out, t);
    } else p++;
  }
}

static char *parse_instr(const char *s, VecS *uses) {
  uses->n = 0;
  if (starts(s, "label ") || starts(s, "jump ")) return NULL;
  if (starts(s, "branch_zero ")) {
    char tok[256]; nth_token(s, 1, tok, sizeof tok); names_collect(tok, uses);
    return NULL;
  }
  if (starts(s, "branch_eq ")) {
    char t1[256], t2[256]; nth_token(s, 1, t1, sizeof t1); nth_token(s, 2, t2, sizeof t2);
    size_t l = strlen(t1); if (l && t1[l - 1] == ',') t1[l - 1] = 0;
    size_t l2 = strlen(t2); if (l2 && t2[l2 - 1] == ',') t2[l2 - 1] = 0;
    names_collect(t1, uses); names_collect(t2, uses);
    return NULL;
  }
  if (starts(s, "return ")) { names_collect(s + 7, uses); return NULL; }
  if (starts(s, "local ")) {
    char tok[256]; nth_token(s, 1, tok, sizeof tok);
    return tok[0] == '@' ? strdup(tok) : NULL;
  }
  if (s[0] == '*') { names_collect(s, uses); return NULL; }
  const char *p = s; while (*p == ' ') p++;
  const char *dst = p; while (*p && *p != ' ') p++;
  size_t dl = (size_t)(p - dst);
  const char *q = p; while (*q == ' ') q++;
  int plus_eq = 0, isdef = 0;
  if (q[0] == '<' && q[1] == '-') { isdef = 1; q += 2; }
  else if (q[0] == '+' && q[1] == '=') { isdef = 1; plus_eq = 1; q += 2; }
  else if (q[0] == '=') { isdef = 1; q += 1; }
  if (!isdef) { names_collect(s, uses); return NULL; }
  char dbuf[256]; if (dl >= sizeof dbuf) dl = sizeof dbuf - 1;
  memcpy(dbuf, dst, dl); dbuf[dl] = 0;
  VecS dn = {0}; names_collect(dbuf, &dn);
  if (dn.n == 0) { vs_free(&dn); names_collect(s, uses); return NULL; }
  char *defn = strdup(dn.a[0]); vs_free(&dn);
  while (*q == ' ') q++;
  names_collect(q, uses);
  if (plus_eq) {
    VecS tmp = {0}; vs_push(&tmp, strdup(defn));
    for (int i = 0; i < uses->n; i++) vs_push(&tmp, uses->a[i]);
    free(uses->a); *uses = tmp;
  }
  return defn;
}

static int count_consts(const char *s) {
  int n = 0;
  size_t len = strlen(s);
  for (size_t i = 0; i < len; ) {
    char c = s[i];
    int prev_block = (i > 0) && (is_word((unsigned char)s[i - 1]) ||
                                 s[i - 1] == '%' || s[i - 1] == '@');
    int matched = 0;
    if (!prev_block) {
      size_t j = i;
      if (c == '-' && j + 1 < len && s[j + 1] >= '0' && s[j + 1] <= '9') j++;
      if (s[j] >= '0' && s[j] <= '9') {
        while (j < len && s[j] >= '0' && s[j] <= '9') j++;
        n++; i = j; matched = 1;
      }
    }
    if (!matched) i++;
  }
  return n;
}

typedef struct {
  int d, layers, nclass, ok;
  float *kind_emb, *op_emb, *feat_w, *feat_b;
  float **msg_w, **msg_b;
  float *selfw_w, *selfw_b;
  float *norm_w, *norm_b;
  float *head0_w, *head0_b, *head2_w, *head2_b;
} Weights;

static Weights G;    /* unified genius model (6 actions incl. COLLAPSE) */

static float *rd(FILE *f, size_t n) {
  float *p = malloc(n * sizeof(float));
  if (fread(p, sizeof(float), n, f) != n) { free(p); return NULL; }
  return p;
}

static int load_weights_into(Weights *W, const char *path) {
  if (W->ok) return 1;
  FILE *f = fopen(path, "rb");
  if (!f) { fprintf(stderr, "--ml-opt: model weights not found (%s)\n", path); return 0; }
  char magic[4]; int hdr[4];
  if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "MLGN", 4) != 0 ||
      fread(hdr, sizeof(int), 4, f) != 4) { fclose(f); return 0; }
  int d = hdr[1], L = hdr[2], C = hdr[3];
  W->d = d; W->layers = L; W->nclass = C;
  int fail = 0;
  W->kind_emb = rd(f, (size_t)NKIND * d);
  W->op_emb = rd(f, (size_t)NOPS * d);
  W->feat_w = rd(f, (size_t)d * NFEAT);
  W->feat_b = rd(f, d);
  W->msg_w = calloc((size_t)L * 8, sizeof(float *));
  W->msg_b = calloc((size_t)L * 8, sizeof(float *));
  W->selfw_w = malloc((size_t)L * d * d * sizeof(float));
  W->selfw_b = malloc((size_t)L * d * sizeof(float));
  W->norm_w = malloc((size_t)L * d * sizeof(float));
  W->norm_b = malloc((size_t)L * d * sizeof(float));
  for (int li = 0; li < L && !fail; li++) {
    for (int t = 0; t < 8; t++) {
      W->msg_w[li * 8 + t] = rd(f, (size_t)d * d);
      W->msg_b[li * 8 + t] = rd(f, d);
      if (!W->msg_w[li * 8 + t] || !W->msg_b[li * 8 + t]) fail = 1;
    }
    if (fread(W->selfw_w + (size_t)li * d * d, sizeof(float), (size_t)d * d, f) != (size_t)d * d) fail = 1;
    if (fread(W->selfw_b + (size_t)li * d, sizeof(float), d, f) != (size_t)d) fail = 1;
    if (fread(W->norm_w + (size_t)li * d, sizeof(float), d, f) != (size_t)d) fail = 1;
    if (fread(W->norm_b + (size_t)li * d, sizeof(float), d, f) != (size_t)d) fail = 1;
  }
  W->head0_w = rd(f, (size_t)d * d);
  W->head0_b = rd(f, d);
  W->head2_w = rd(f, (size_t)C * d);
  W->head2_b = rd(f, C);
  fclose(f);
  if (fail || !W->kind_emb || !W->op_emb || !W->feat_w || !W->feat_b ||
      !W->head0_w || !W->head0_b || !W->head2_w || !W->head2_b) {
    fprintf(stderr, "--ml-opt: failed to read model weights\n");
    return 0;
  }
  W->ok = 1;
  return 1;
}

static int load_weights(void) {
  const char *path = getenv("METTLE_ML_MODEL");
  if (!path) path = "tools/mlopt/gnn_genius.bin";
  return load_weights_into(&G, path);
}

/* Dense matvec y = W.x + b. Computes eight output rows at a time: the eight dot
 * products are independent, so their mul/add chains pipeline instead of stalling
 * on a single accumulator's latency, and x[i] is loaded once and reused across
 * the eight rows. The eight parallel accumulators also SLP-vectorize cleanly
 * (two 128-bit lanes on SSE2, one 256-bit on AVX). Each row is still summed in
 * ascending i order, so every y[o] is bit-identical to the plain sequential dot
 * product -- no FP reassociation, so model decisions are unchanged. */
static void linear(float *restrict y, const float *restrict x,
                   const float *restrict W, const float *restrict b,
                   int out, int in) {
  int o = 0;
  for (; o + 8 <= out; o += 8) {
    const float *w0 = W + (size_t)o * in;
    const float *w1 = w0 + in, *w2 = w1 + in, *w3 = w2 + in;
    const float *w4 = w3 + in, *w5 = w4 + in, *w6 = w5 + in, *w7 = w6 + in;
    float a0 = b ? b[o] : 0.0f, a1 = b ? b[o + 1] : 0.0f;
    float a2 = b ? b[o + 2] : 0.0f, a3 = b ? b[o + 3] : 0.0f;
    float a4 = b ? b[o + 4] : 0.0f, a5 = b ? b[o + 5] : 0.0f;
    float a6 = b ? b[o + 6] : 0.0f, a7 = b ? b[o + 7] : 0.0f;
    int i = 0;
#define LINEAR8_STEP(I) do { \
      float xi = x[(I)]; \
      a0 += w0[(I)] * xi; a1 += w1[(I)] * xi; \
      a2 += w2[(I)] * xi; a3 += w3[(I)] * xi; \
      a4 += w4[(I)] * xi; a5 += w5[(I)] * xi; \
      a6 += w6[(I)] * xi; a7 += w7[(I)] * xi; \
    } while (0)
    for (; i + 4 <= in; i += 4) {
      LINEAR8_STEP(i);
      LINEAR8_STEP(i + 1);
      LINEAR8_STEP(i + 2);
      LINEAR8_STEP(i + 3);
    }
    for (; i < in; i++) {
      float xi = x[i];
      a0 += w0[i] * xi; a1 += w1[i] * xi; a2 += w2[i] * xi; a3 += w3[i] * xi;
      a4 += w4[i] * xi; a5 += w5[i] * xi; a6 += w6[i] * xi; a7 += w7[i] * xi;
    }
#undef LINEAR8_STEP
    y[o] = a0; y[o + 1] = a1; y[o + 2] = a2; y[o + 3] = a3;
    y[o + 4] = a4; y[o + 5] = a5; y[o + 6] = a6; y[o + 7] = a7;
  }
  for (; o < out; o++) {
    float acc = b ? b[o] : 0.0f;
    const float *w = W + (size_t)o * in;
    int i = 0;
    for (; i + 4 <= in; i += 4) {
      acc += w[i] * x[i];
      acc += w[i + 1] * x[i + 1];
      acc += w[i + 2] * x[i + 2];
      acc += w[i + 3] * x[i + 3];
    }
    for (; i < in; i++) acc += w[i] * x[i];
    y[o] = acc;
  }
}

static void layernorm(float *x, const float *g, const float *b, int d) {
  float mean = 0; for (int i = 0; i < d; i++) mean += x[i]; mean /= d;
  float var = 0; for (int i = 0; i < d; i++) { float t = x[i] - mean; var += t * t; }
  var /= d;
  float inv = 1.0f / sqrtf(var + 1e-5f);
  for (int i = 0; i < d; i++) x[i] = (x[i] - mean) * inv * g[i] + b[i];
}

/* Relational GNN forward over the typed-edge graph; writes per-node argmax class
 * to out[]. Reusable across both models (genius 5-class, collapse 2-class). */
static void gnn_forward(Weights *W, int n, int *kind, int *op, float *feat,
                        int *esrc[NEDGE], int *edst[NEDGE], int *ecnt, int *out) {
  int d = W->d, L = W->layers, C = W->nclass;
  float *h = malloc((size_t)n * d * sizeof(float));
  for (int i = 0; i < n; i++) {
    float *hi = h + (size_t)i * d;
    linear(hi, feat + (size_t)i * NFEAT, W->feat_w, W->feat_b, d, NFEAT);
    const float *ke = W->kind_emb + (size_t)kind[i] * d;
    const float *oe = W->op_emb + (size_t)op[i] * d;
    for (int c = 0; c < d; c++) hi[c] += ke[c] + oe[c];
  }
  float *agg = malloc((size_t)n * d * sizeof(float));
  float *acc = malloc((size_t)n * d * sizeof(float));
  int *deg = calloc((size_t)n, sizeof(int));
  /* Per-source message cache: W_t.h[s] depends only on (layer, edge-type, source),
   * so compute it once per distinct source and scatter to every destination.
   * The touched list keeps relation accumulation sparse: most edge types touch
   * only a slice of the function graph, so avoid clearing and folding n*d floats. */
  float *msgbuf = malloc((size_t)n * d * sizeof(float));
  int *computed_gen = calloc((size_t)n, sizeof(int));
  int *touched = malloc((size_t)n * sizeof(int));
  for (int li = 0; li < L; li++) {
    for (int i = 0; i < n; i++)
      linear(agg + (size_t)i * d, h + (size_t)i * d,
             W->selfw_w + (size_t)li * d * d, W->selfw_b + (size_t)li * d, d, d);
    for (int t = 0; t < NEDGE; t++) {
      if (ecnt[t] == 0) continue;
      int ntouched = 0;
      int gen = li * NEDGE + t + 1;
      const float *mw = W->msg_w[li * 8 + t], *mb = W->msg_b[li * 8 + t];
      for (int e = 0; e < ecnt[t]; e++) {
        int s = esrc[t][e], dd = edst[t][e];
        float *ms = msgbuf + (size_t)s * d;
        if (computed_gen[s] != gen) {
          linear(ms, h + (size_t)s * d, mw, mb, d, d);
          computed_gen[s] = gen;
        }
        if (deg[dd] == 0) {
          touched[ntouched++] = dd;
          memset(acc + (size_t)dd * d, 0, (size_t)d * sizeof(float));
        }
        float *ad = acc + (size_t)dd * d;
        for (int c = 0; c < d; c++) ad[c] += ms[c];
        deg[dd]++;
      }
      for (int ti = 0; ti < ntouched; ti++) {
        int i = touched[ti];
        float dv = (float)deg[i];
        float *ai = agg + (size_t)i * d, *ci = acc + (size_t)i * d;
        for (int c = 0; c < d; c++) ai[c] += ci[c] / dv;
        deg[i] = 0;
      }
    }
    for (int i = 0; i < n; i++) {
      float *hi = h + (size_t)i * d, *ai = agg + (size_t)i * d;
      for (int c = 0; c < d; c++) hi[c] += ai[c] > 0 ? ai[c] : 0.0f;
      layernorm(hi, W->norm_w + (size_t)li * d, W->norm_b + (size_t)li * d, d);
    }
  }
  float *t0 = malloc((size_t)d * sizeof(float)), *lg = malloc((size_t)C * sizeof(float));
  for (int i = 0; i < n; i++) {
    linear(t0, h + (size_t)i * d, W->head0_w, W->head0_b, d, d);
    for (int c = 0; c < d; c++) if (t0[c] < 0) t0[c] = 0;
    linear(lg, t0, W->head2_w, W->head2_b, C, d);
    int best = 0; for (int c = 1; c < C; c++) if (lg[c] > lg[best]) best = c;
    out[i] = best;
  }
  free(t0); free(lg); free(h); free(agg); free(acc); free(deg);
  free(msgbuf); free(computed_gen); free(touched);
}

typedef struct { VecS keys; } Intern;
static int intern(Intern *t, const char *s) {
  for (int i = 0; i < t->keys.n; i++) if (strcmp(t->keys.a[i], s) == 0) return i;
  vs_push(&t->keys, strdup(s)); return t->keys.n - 1;
}

static char *expr_key(const char *rhs) {
  char a[256], o[16], b[256];
  if (!three_token(rhs, a, sizeof a, o, sizeof o, b, sizeof b)) return NULL;
  if (op_is_comm(o) && strcmp(a, b) > 0) { char tmp[256]; strcpy(tmp, a); strcpy(a, b); strcpy(b, tmp); }
  char *k = malloc(strlen(a) + strlen(o) + strlen(b) + 3);
  sprintf(k, "%s|%s|%s", o, a, b);
  return k;
}

static char *dominators(VecI *preds, int n) {
  char *dom = malloc((size_t)n * n);
  for (int i = 0; i < n; i++)
    for (int j = 0; j < n; j++) dom[(size_t)i * n + j] = (i == 0) ? (j == 0) : 1;
  int changed = 1;
  char *neu = malloc(n);
  while (changed) {
    changed = 0;
    for (int i = 1; i < n; i++) {
      for (int j = 0; j < n; j++) neu[j] = 1;
      if (preds[i].n == 0) for (int j = 0; j < n; j++) neu[j] = 1;
      else for (int pi = 0; pi < preds[i].n; pi++) {
        char *dp = dom + (size_t)preds[i].a[pi] * n;
        for (int j = 0; j < n; j++) neu[j] &= dp[j];
      }
      neu[i] = 1;
      char *di = dom + (size_t)i * n;
      for (int j = 0; j < n; j++) if (neu[j] != di[j]) { changed = 1; break; }
      if (changed) memcpy(di, neu, n);
    }
  }
  free(neu);
  return dom;
}

/* Affine-form simplification (port of tools/mlopt/affine.py): track each value as
 * an SSA-versioned affine form over straight-line regions; emit CONST or COPY when
 * it collapses. Sound: unmasked re-emit only when exact in Z/2^64. */
#define AFF_BITS 40
static const long long AFF_MOD = 1LL << AFF_BITS;
static const long long AFF_M40 = (1LL << AFF_BITS) - 1;

static int is_lit_tok(const char *s) {
  if (!*s) return 0;
  const char *p = s; if (*p == '-') p++;
  if (!*p) return 0;
  for (; *p; p++) if (*p < '0' || *p > '9') return 0;
  return 1;
}

/* `& b` is identity mod 2^40 iff b's low 40 bits are all set (b == 2^k-1, k>=40) */
static int is_widemask(const char *b) {
  if (!is_lit_tok(b)) return 0;
  long long v = atoll(b);
  return v >= AFF_M40 && ((v + 1) & v) == 0;
}

static int has_float_lit(const char *s) {            /* \d+\.\d+ */
  for (const char *p = s; *p; p++)
    if (*p >= '0' && *p <= '9') {
      const char *q = p; while (*q >= '0' && *q <= '9') q++;
      if (*q == '.' && q[1] >= '0' && q[1] <= '9') return 1;
    }
  return 0;
}

/* infer_params: symbols used before declared (port of ml_pass.infer_params) */
static void infer_params(char **texts, int n, VecS *params) {
  VecS declared = {0};
  for (int i = 0; i < n; i++) {
    const char *s = texts[i];
    if (starts(s, "local ")) {
      char tok[256]; nth_token(s, 1, tok, sizeof tok);
      if (tok[0] == '@') vs_push(&declared, strdup(tok));
      continue;
    }
    char t0[256], t1[16]; nth_token(s, 0, t0, sizeof t0); nth_token(s, 1, t1, sizeof t1);
    const char *tgt = (t0[0] == '@' && (strcmp(t1, "<-") == 0 || strcmp(t1, "=") == 0)) ? t0 : NULL;
    for (const char *p = s; *p; ) {                  /* findall @\w+ */
      if (*p == '@' && is_word((unsigned char)p[1])) {
        const char *st = p++; while (is_word((unsigned char)*p)) p++;
        size_t l = (size_t)(p - st); char sym[256]; if (l >= sizeof sym) l = sizeof sym - 1;
        memcpy(sym, st, l); sym[l] = 0;
        int seen = 0;
        for (int k = 0; k < declared.n; k++) if (strcmp(declared.a[k], sym) == 0) { seen = 1; break; }
        if (tgt && strcmp(sym, tgt) == 0) seen = 1;
        for (int k = 0; k < params->n; k++) if (strcmp(params->a[k], sym) == 0) { seen = 1; break; }
        if (!seen) vs_push(params, strdup(sym));
      } else p++;
    }
    if (tgt) vs_push(&declared, strdup(tgt));
  }
  vs_free(&declared);
}

typedef struct { char **v; long long *c; int n; long long cst; int exact; } Form;
static void form_free(Form *f) { for (int i = 0; i < f->n; i++) free(f->v[i]); free(f->v); free(f->c); f->v = NULL; f->c = NULL; f->n = 0; }
static Form form_copy(const Form *s) {
  Form f = {0}; f.n = s->n; f.cst = s->cst; f.exact = s->exact;
  if (f.n) { f.v = malloc(f.n * sizeof(char *)); f.c = malloc(f.n * sizeof(long long));
    for (int i = 0; i < f.n; i++) { f.v[i] = strdup(s->v[i]); f.c[i] = s->c[i]; } }
  return f;
}
static void form_addterm(Form *f, const char *v, long long c) {
  for (int i = 0; i < f->n; i++) if (strcmp(f->v[i], v) == 0) {
    f->c[i] += c;
    if (f->c[i] == 0) { free(f->v[i]); for (int k = i + 1; k < f->n; k++) { f->v[k - 1] = f->v[k]; f->c[k - 1] = f->c[k]; } f->n--; }
    return;
  }
  if (c == 0) return;
  f->v = realloc(f->v, (f->n + 1) * sizeof(char *)); f->c = realloc(f->c, (f->n + 1) * sizeof(long long));
  f->v[f->n] = strdup(v); f->c[f->n] = c; f->n++;
}
static Form form_add(const Form *x, const Form *y, long long s) {
  Form r = form_copy(x); r.cst = x->cst + s * y->cst; r.exact = x->exact && y->exact;
  for (int i = 0; i < y->n; i++) form_addterm(&r, y->v[i], s * y->c[i]);
  return r;
}
static Form form_scale(const Form *x, long long k) {
  Form r = form_copy(x); r.cst = x->cst * k;
  for (int i = 0; i < r.n; i++) r.c[i] *= k;
  if (k == 0) { for (int i = 0; i < r.n; i++) free(r.v[i]); free(r.v); free(r.c); r.v = NULL; r.c = NULL; r.n = 0; }
  return r;
}

typedef struct {
  VecS ver_name; VecI ver_v;
  VecS env_tok; Form *env_f; int env_n, env_cap;
  VecS bits40, floats;
} AffCtx;

static int aff_ver(AffCtx *c, const char *name) {
  for (int i = 0; i < c->ver_name.n; i++) if (strcmp(c->ver_name.a[i], name) == 0) return c->ver_v.a[i];
  return 0;
}
static void aff_bump(AffCtx *c, const char *name) {
  for (int i = 0; i < c->ver_name.n; i++) if (strcmp(c->ver_name.a[i], name) == 0) { c->ver_v.a[i]++; return; }
  vs_push(&c->ver_name, strdup(name)); vi_push(&c->ver_v, 1);
}
static char *aff_curtok(AffCtx *c, const char *name) {
  char *t = malloc(strlen(name) + 16); sprintf(t, "%s#%d", name, aff_ver(c, name)); return t;
}
static int aff_env_idx(AffCtx *c, const char *tok) {
  for (int i = 0; i < c->env_n; i++) if (strcmp(c->env_tok.a[i], tok) == 0) return i;
  return -1;
}
static void aff_env_set(AffCtx *c, const char *tok, Form f) {     /* takes ownership of f */
  int i = aff_env_idx(c, tok);
  if (i >= 0) { form_free(&c->env_f[i]); c->env_f[i] = f; return; }
  if (c->env_n == c->env_cap) { c->env_cap = c->env_cap ? c->env_cap * 2 : 16;
    c->env_f = realloc(c->env_f, c->env_cap * sizeof(Form)); }
  vs_push(&c->env_tok, strdup(tok)); c->env_f[c->env_n++] = f;
}
static int set_has(VecS *s, const char *x) { for (int i = 0; i < s->n; i++) if (strcmp(s->a[i], x) == 0) return 1; return 0; }
static void set_add(VecS *s, const char *x) { if (!set_has(s, x)) vs_push(s, strdup(x)); }
static void set_del(VecS *s, const char *x) {
  for (int i = 0; i < s->n; i++) if (strcmp(s->a[i], x) == 0) { free(s->a[i]); for (int k = i + 1; k < s->n; k++) s->a[k - 1] = s->a[k]; s->n--; return; }
}

static void aff_reset(AffCtx *c, VecS *params) {
  for (int i = 0; i < c->env_n; i++) { free(c->env_tok.a[i]); form_free(&c->env_f[i]); }
  c->env_tok.n = 0; c->env_n = 0;
  vs_free(&c->ver_name); c->ver_name = (VecS){0}; vi_free(&c->ver_v); c->ver_v = (VecI){0};
  vs_free(&c->bits40); c->bits40 = (VecS){0};
  for (int i = 0; i < params->n; i++) { char *t = aff_curtok(c, params->a[i]); set_add(&c->bits40, t); free(t); }
}

/* atom(tok): the form of an operand. Returns owned Form. */
static Form aff_atom(AffCtx *c, const char *tok) {
  if (is_lit_tok(tok)) { Form f = {0}; f.cst = atoll(tok); f.exact = 1; return f; }
  char *bt = aff_curtok(c, tok);
  int i = aff_env_idx(c, bt);
  if (i >= 0) { free(bt); return form_copy(&c->env_f[i]); }
  Form f = {0}; f.n = 1; f.v = malloc(sizeof(char *)); f.c = malloc(sizeof(long long));
  f.v[0] = bt; f.c[0] = 1; f.cst = 0; f.exact = 1; return f;
}

static int aff_fits40(AffCtx *c, const char *tok) {
  if (is_lit_tok(tok)) { long long v = atoll(tok); return v >= 0 && v < AFF_MOD; }
  char *bt = aff_curtok(c, tok); int r = set_has(&c->bits40, bt); free(bt); return r;
}

/* rhs_form: returns 1 + owned Form, or 0 (no form). */
static int aff_rhs_form(AffCtx *c, const char *rhs, Form *out) {
  char a[256], o[16], b[256];
  int grp = three_token(rhs, a, sizeof a, o, sizeof o, b, sizeof b);
  if (grp != 1) {                                  /* not arithmetic: maybe a bare atom */
    if (is_lit_tok(rhs)) { *out = aff_atom(c, rhs); return 1; }
    /* single name token? */
    int ok = (rhs[0] == '@' || rhs[0] == '%');
    for (const char *p = rhs; ok && *p; p++)
      if (!(is_word((unsigned char)*p) || *p == '.' || *p == '$' || *p == '@' || *p == '%')) ok = 0;
    if (ok && rhs[0]) { *out = aff_atom(c, rhs); return 1; }
    return 0;
  }
  if (strcmp(o, "&") == 0 && is_widemask(b)) {
    Form fa = aff_atom(c, a);
    if (aff_fits40(c, a)) { *out = fa; return 1; }
    fa.exact = 0; *out = fa; return 1;             /* lossy: only value mod 2^40 known */
  }
  Form fa = aff_atom(c, a), fb = aff_atom(c, b);
  if (strcmp(o, "+") == 0) { *out = form_add(&fa, &fb, 1); form_free(&fa); form_free(&fb); return 1; }
  if (strcmp(o, "-") == 0) { *out = form_add(&fa, &fb, -1); form_free(&fa); form_free(&fb); return 1; }
  if (strcmp(o, "*") == 0) {
    if (fa.n == 0) { *out = form_scale(&fb, fa.cst); form_free(&fa); form_free(&fb); return 1; }
    if (fb.n == 0) { *out = form_scale(&fa, fb.cst); form_free(&fa); form_free(&fb); return 1; }
    form_free(&fa); form_free(&fb); return 0;
  }
  if (strcmp(o, "<<") == 0 && is_lit_tok(b)) { *out = form_scale(&fa, 1LL << (atoll(b) & 63)); form_free(&fa); form_free(&fb); return 1; }
  form_free(&fa); form_free(&fb); return 0;
}

static int aff_rhs_fits40(AffCtx *c, const char *rhs) {
  if (is_lit_tok(rhs)) { long long v = atoll(rhs); return v >= 0 && v < AFF_MOD; }
  { int ok = (rhs[0] == '@' || rhs[0] == '%'); const char *p = rhs;
    for (; ok && *p; p++) if (!(is_word((unsigned char)*p) || *p == '.' || *p == '$' || *p == '@' || *p == '%')) ok = 0;
    if (ok && rhs[0]) return aff_fits40(c, rhs);
  }
  char a[256], o[16], b[256];
  if (three_token(rhs, a, sizeof a, o, sizeof o, b, sizeof b) == 1) {
    if (strcmp(o, "&") == 0 && is_lit_tok(b)) { long long v = atoll(b); if (v >= 0 && v < AFF_MOD) return 1; }
    if (strcmp(o, "&") == 0 && is_widemask(b)) return aff_fits40(c, a);
  }
  return 0;
}

/* emit: classify the collapsed form as a disposition. kind 1=CONST,2=COPY,0=none.
 * arg receives the constant (signed decimal, round-trips through atoll) or name. */
static int aff_emit(AffCtx *c, const char *dest, const Form *form, int orig_is_mask,
                    const char *orig_rhs, char *arg, size_t acap) {
  (void)dest;
  int nv = 0; for (int i = 0; i < form->n; i++) if (form->c[i] != 0) nv++;
  if (nv == 0) {
    if (!form->exact) return 0;
    char buf[32]; snprintf(buf, sizeof buf, "%lld", form->cst);
    if (strcmp(orig_rhs, buf) == 0) return 0;       /* already this constant */
    snprintf(arg, acap, "%lld", form->cst);
    return 1;                                        /* CONST */
  }
  if (nv != 1) return 0;
  int idx = -1; for (int i = 0; i < form->n; i++) if (form->c[i] != 0) { idx = i; break; }
  long long k = form->c[idx];
  char name[256]; const char *hash = strrchr(form->v[idx], '#');
  size_t nl = hash ? (size_t)(hash - form->v[idx]) : strlen(form->v[idx]);
  if (nl >= sizeof name) return 0;
  memcpy(name, form->v[idx], nl); name[nl] = 0;
  char *ct = aff_curtok(c, name); int cur = strcmp(ct, form->v[idx]) == 0; free(ct);
  if (!cur) return 0;                                /* value no longer held by this name */
  if (k == 1 && form->cst == 0) {
    int copy_ok = form->exact || (orig_is_mask && set_has(&c->bits40, form->v[idx]));
    if (!copy_ok) return 0;
    if (strcmp(orig_rhs, name) == 0) return 0;
    snprintf(arg, acap, "%s", name);
    return 2;                                        /* COPY */
  }
  return 0;     /* name+const / name*k: real but not a single COPY/CONST disposition */
}

/* Fill aff_kind[j] (0/1/2) and aff_arg[j] for each instruction. */
static void affine_run(char **texts, int n, VecS *params, int *aff_kind, char **aff_arg) {
  AffCtx c = {0};
  aff_reset(&c, params);
  for (int i = 0; i < n; i++) {
    const char *s = texts[i];
    aff_kind[i] = 0; aff_arg[i] = NULL;
    if (starts(s, "local ") && strstr(s, "float")) {   /* ^local @x : ... float */
      char tok[256]; nth_token(s, 1, tok, sizeof tok);
      if (tok[0] == '@' && strstr(s, ":")) set_add(&c.floats, tok);
    }
    if (is_control(s) || starts(s, "branch") || s[0] == '*') { aff_reset(&c, params); continue; }
    char dest[256], rhs[512];
    if (!split_def(s, dest, sizeof dest, rhs, sizeof rhs)) continue;
    int is_float = (strstr(rhs, "(float") != NULL) || has_float_lit(rhs);
    if (!is_float) { VecS rn = {0}; names_collect(rhs, &rn);
      for (int t = 0; t < rn.n; t++) if (set_has(&c.floats, rn.a[t])) { is_float = 1; break; }
      vs_free(&rn);
    }
    if (is_float) {
      set_add(&c.floats, dest); aff_bump(&c, dest);
      char *bt = aff_curtok(&c, dest); set_del(&c.bits40, bt);
      Form f = {0}; f.n = 1; f.v = malloc(sizeof(char *)); f.c = malloc(sizeof(long long));
      f.v[0] = strdup(bt); f.c[0] = 1; f.exact = 1; aff_env_set(&c, bt, f); free(bt);
      continue;
    }
    char a[256], o[16], b[256];
    int grp = three_token(rhs, a, sizeof a, o, sizeof o, b, sizeof b);
    int orig_is_mask = (grp == 1 && strcmp(o, "&") == 0 && is_widemask(b));
    Form form; int have = aff_rhs_form(&c, rhs, &form);
    int fits = aff_rhs_fits40(&c, rhs);
    aff_bump(&c, dest);
    char *nbt = aff_curtok(&c, dest);
    if (have) aff_env_set(&c, nbt, form_copy(&form));
    else { Form f = {0}; f.n = 1; f.v = malloc(sizeof(char *)); f.c = malloc(sizeof(long long));
           f.v[0] = strdup(nbt); f.c[0] = 1; f.exact = 1; aff_env_set(&c, nbt, f); }
    set_del(&c.bits40, nbt);
    if (fits) set_add(&c.bits40, nbt);
    if (have) {
      char arg[256];
      int k = aff_emit(&c, dest, &form, orig_is_mask, rhs, arg, sizeof arg);
      if (k) { aff_kind[i] = k; aff_arg[i] = strdup(arg); }
      form_free(&form);
    }
    free(nbt);
  }
  for (int i = 0; i < c.env_n; i++) { free(c.env_tok.a[i]); form_free(&c.env_f[i]); }
  free(c.env_tok.a); free(c.env_f);
  vs_free(&c.ver_name); vi_free(&c.ver_v); vs_free(&c.bits40); vs_free(&c.floats);
}

/* Semantic collapse: a tangled pure expr whose value is always 0 (CONST) or a
 * single leaf (COPY), confirmed over many 64-bit vectors. Leaves must have <=1
 * def (SSA-equivalent) so the free-variable abstraction is exact. */
static const char *PURE8[] = {"+", "-", "*", "&", "|", "^", "<<", ">>"};
static int is_pure_op(const char *o) { for (int i = 0; i < 8; i++) if (strcmp(o, PURE8[i]) == 0) return 1; return 0; }

static int pure_def_of(char **texts, int n, const char *name, char *op, char *a, char *b) {
  for (int i = 0; i < n; i++) {
    char dest[256], rhs[512];
    if (!split_def(texts[i], dest, sizeof dest, rhs, sizeof rhs)) continue;
    if (strcmp(dest, name) != 0) continue;
    char ta[256], to[16], tb[256];
    if (three_token(rhs, ta, sizeof ta, to, sizeof to, tb, sizeof tb) == 1 && is_pure_op(to)) {
      strcpy(op, to); strcpy(a, ta); strcpy(b, tb); return 1;
    }
    return 0;            /* defined, but not a pure binop -> treat as a leaf */
  }
  return 0;              /* not defined here (param) -> leaf */
}

static int collect_leaves(char **texts, int n, const char *name, VecS *leaves, VecS *visited, int depth) {
  if (depth > 24) return 0;
  if (is_lit_tok(name)) return 1;
  char op[16], a[256], b[256];
  if ((name[0] == '%' || name[0] == '@') && pure_def_of(texts, n, name, op, a, b)) {
    if (set_has(visited, name)) return 1;
    if (visited->n > 16) return 0;
    set_add(visited, name);
    return collect_leaves(texts, n, a, leaves, visited, depth + 1) &&
           collect_leaves(texts, n, b, leaves, visited, depth + 1);
  }
  if (!set_has(leaves, name)) vs_push(leaves, strdup(name));
  return 1;
}

static unsigned long long eval_dag(char **texts, int n, const char *name,
                                   VecS *leaves, unsigned long long *vals, int depth) {
  if (depth > 24 || is_lit_tok(name)) return is_lit_tok(name) ? (unsigned long long)atoll(name) : 0;
  for (int k = 0; k < leaves->n; k++) if (strcmp(leaves->a[k], name) == 0) return vals[k];
  char op[16], a[256], b[256];
  if (pure_def_of(texts, n, name, op, a, b)) {
    unsigned long long x = eval_dag(texts, n, a, leaves, vals, depth + 1);
    unsigned long long y = eval_dag(texts, n, b, leaves, vals, depth + 1);
    if (strcmp(op, "+") == 0) return x + y;
    if (strcmp(op, "-") == 0) return x - y;
    if (strcmp(op, "*") == 0) return x * y;
    if (strcmp(op, "&") == 0) return x & y;
    if (strcmp(op, "|") == 0) return x | y;
    if (strcmp(op, "^") == 0) return x ^ y;
    if (strcmp(op, "<<") == 0) return x << (y & 63);
    if (strcmp(op, ">>") == 0) return x >> (y & 63);
  }
  return 0;
}

static int def_count(char **texts, int n, const char *name) {
  int c = 0; for (int i = 0; i < n; i++) { char d[256], r[512];
    if (split_def(texts[i], d, sizeof d, r, sizeof r) && strcmp(d, name) == 0) c++; }
  return c;
}

static unsigned long long g_lcg;
static unsigned long long lcg64(void) {
  g_lcg = g_lcg * 6364136223846793005ULL + 1442695040888963407ULL;
  return g_lcg;
}

/* returns 1 + kind(1=CONST,2=COPY) + arg if the flagged temp collapses */
static int collapse_check(char **texts, int n, int ri, char *arg, size_t acap,
                          int *out_kind, int *saved) {
  char dest[256], rhs[512];
  if (!split_def(texts[ri], dest, sizeof dest, rhs, sizeof rhs) || dest[0] != '%') return 0;
  char ta[256], to[16], tb[256];
  if (three_token(rhs, ta, sizeof ta, to, sizeof to, tb, sizeof tb) != 1 || !is_pure_op(to)) return 0;
  VecS leaves = {0}, visited = {0};
  int ok = collect_leaves(texts, n, dest, &leaves, &visited, 0);
  int internal = visited.n;
  vs_free(&visited);
  if (!ok || leaves.n == 0 || leaves.n > 8) { vs_free(&leaves); return 0; }
  for (int k = 0; k < leaves.n; k++)
    if (is_lit_tok(leaves.a[k]) || def_count(texts, n, leaves.a[k]) > 1) { vs_free(&leaves); return 0; }
  const unsigned long long MASK = ~0ULL;
  unsigned long long base[6] = {0ULL, 1ULL, 2ULL, MASK, MASK - 1, (1ULL << 63)};
  g_lcg = 0x9e3779b97f4a7c15ULL ^ (unsigned long long)(n * 2654435761u);
  int const0 = 1; int *copyk = malloc(leaves.n * sizeof(int));
  for (int k = 0; k < leaves.n; k++) copyk[k] = 1;
  unsigned long long *vals = malloc(leaves.n * sizeof(unsigned long long));
  for (int t = 0; t < 256; t++) {
    for (int k = 0; k < leaves.n; k++) vals[k] = (t < 6) ? base[t] : lcg64();
    unsigned long long r = eval_dag(texts, n, dest, &leaves, vals, 0);
    if (r != 0) const0 = 0;
    for (int k = 0; k < leaves.n; k++) if (r != vals[k]) copyk[k] = 0;
  }
  int kind = 0;
  if (const0) { snprintf(arg, acap, "0"); kind = 1; }
  else for (int k = 0; k < leaves.n; k++) if (copyk[k]) { snprintf(arg, acap, "%s", leaves.a[k]); kind = 2; break; }
  free(vals); free(copyk); vs_free(&leaves);
  *out_kind = kind; if (saved) *saved = internal > 0 ? internal : 1;
  return kind != 0;
}

/* Bitwise superoptimizer realizer: a pure-&|^~ expr over <=4 leaves is an exact
 * bitwise function; evaluating leaves at their 2^k column constants yields its
 * truth table (a proof, since &|^~ are bit-independent). Look it up in bw_lib.txt
 * and REWRITE to the optimum when cheaper. Non-bitwise sub-values are opaque
 * leaves; other-than-0/-1 literals or >4 leaves bail to plain collapse. */
typedef struct { int k; unsigned long long fp; char post[48]; } BwEnt;
static BwEnt *g_bw; static int g_bw_n; static int g_bw_loaded;

static int bw_cmp(const void *a, const void *b) {
  const BwEnt *x = a, *y = b;
  if (x->k != y->k) return x->k - y->k;
  return x->fp < y->fp ? -1 : x->fp > y->fp ? 1 : 0;
}
static void bw_load(void) {
  if (g_bw_loaded) return;
  g_bw_loaded = 1;
  const char *path = getenv("METTLE_ML_BWLIB");
  if (!path) path = "tools/mlopt/bw_lib.txt";
  FILE *f = fopen(path, "r");
  if (!f) return;
  int cap = 4096; g_bw = malloc(cap * sizeof(BwEnt));
  char line[128];
  while (fgets(line, sizeof line, f)) {
    if (g_bw_n == cap) { cap *= 2; g_bw = realloc(g_bw, cap * sizeof(BwEnt)); }
    BwEnt e; char post[48];
    if (sscanf(line, "%d %llu %47s", &e.k, &e.fp, post) == 3) {
      e.k = e.k; strncpy(e.post, post, sizeof e.post - 1); e.post[sizeof e.post - 1] = 0;
      g_bw[g_bw_n++] = e;
    }
  }
  fclose(f);
  qsort(g_bw, g_bw_n, sizeof(BwEnt), bw_cmp);
}
static const char *bw_lookup(int k, unsigned long long fp) {
  int lo = 0, hi = g_bw_n - 1;
  while (lo <= hi) { int mid = (lo + hi) / 2;
    BwEnt key = {k, fp, {0}}; int c = bw_cmp(&g_bw[mid], &key);
    if (c == 0) return g_bw[mid].post;
    if (c < 0) lo = mid + 1; else hi = mid - 1;
  }
  return NULL;
}

/* classify a def for the bitwise walk: 1=binary &|^ (op,a,b), 2=unary ~ (a),
 * 0=opaque leaf. Non-bitwise defs are opaque leaves. */
static int bw_def_of(char **texts, int n, const char *name, char *op, char *a, char *b) {
  for (int i = 0; i < n; i++) {
    char dest[256], rhs[512];
    if (!split_def(texts[i], dest, sizeof dest, rhs, sizeof rhs)) continue;
    if (strcmp(dest, name) != 0) continue;
    char ta[256], to[16], tb[256];
    if (three_token(rhs, ta, sizeof ta, to, sizeof to, tb, sizeof tb) == 1 &&
        (strcmp(to, "&") == 0 || strcmp(to, "|") == 0 || strcmp(to, "^") == 0)) {
      strcpy(op, to); strcpy(a, ta); strcpy(b, tb); return 1;
    }
    if (rhs[0] == '~' && rhs[1] && !strchr(rhs + 1, ' ')) { strcpy(a, rhs + 1); return 2; }
    return 0;
  }
  return 0;
}

#define BW_BAIL (-999)
/* Walk the DAG: collect distinct variable leaves, count internal &|^~ nodes
 * (cost). Returns cost, or BW_BAIL on a non-0/-1 literal or >4 leaves. */
static int bw_walk(char **texts, int n, const char *name, VecS *leaves, VecS *vis, int depth) {
  if (depth > 24) return BW_BAIL;
  if (is_lit_tok(name)) {                       /* literal: only 0 / -1 (all-ones) allowed */
    long long v = atoll(name);
    return (v == 0 || v == -1) ? 0 : BW_BAIL;
  }
  char op[16], a[256], b[256];
  int kd = bw_def_of(texts, n, name, op, a, b);
  if (kd == 1) {
    if (set_has(vis, name)) return 0;
    set_add(vis, name);
    int ca = bw_walk(texts, n, a, leaves, vis, depth + 1);
    int cb = bw_walk(texts, n, b, leaves, vis, depth + 1);
    if (ca == BW_BAIL || cb == BW_BAIL) return BW_BAIL;
    return ca + cb + 1;
  }
  if (kd == 2) {
    if (set_has(vis, name)) return 0;
    set_add(vis, name);
    int ca = bw_walk(texts, n, a, leaves, vis, depth + 1);
    if (ca == BW_BAIL) return BW_BAIL;
    return ca + 1;
  }
  if (!set_has(leaves, name)) {                 /* opaque variable leaf */
    if (leaves->n >= 4) return BW_BAIL;
    vs_push(leaves, strdup(name));
  }
  return 0;
}

static int leaf_index(VecS *leaves, const char *name) {
  for (int i = 0; i < leaves->n; i++) if (strcmp(leaves->a[i], name) == 0) return i;
  return -1;
}
static unsigned long long bw_col(int j, int k) {
  unsigned long long v = 0;
  for (int r = 0; r < (1 << k); r++) if ((r >> j) & 1) v |= (1ULL << r);
  return v;
}
static unsigned long long bw_eval(char **texts, int n, const char *name,
                                  VecS *leaves, int k, int depth) {
  unsigned long long full = ~0ULL;
  if (is_lit_tok(name)) return atoll(name) == 0 ? 0 : full;
  char op[16], a[256], b[256];
  int kd = bw_def_of(texts, n, name, op, a, b);
  if (kd == 1) {
    unsigned long long x = bw_eval(texts, n, a, leaves, k, depth + 1);
    unsigned long long y = bw_eval(texts, n, b, leaves, k, depth + 1);
    return op[0] == '&' ? (x & y) : op[0] == '|' ? (x | y) : (x ^ y);
  }
  if (kd == 2) return ~bw_eval(texts, n, a, leaves, k, depth + 1);
  int idx = leaf_index(leaves, name);
  return idx >= 0 ? bw_col(idx, k) : 0;
}

/* If the flagged root is a pure-bitwise expr with a strictly cheaper optimum,
 * fill `out` with a REWRITE postfix (space-separated, leaves substituted) and
 * return 1. */
static int superopt_check(char **texts, int n, int ri, char *out, size_t cap, int *saved) {
  bw_load();
  if (!g_bw_n) return 0;
  char dest[256], rhs[512];
  if (!split_def(texts[ri], dest, sizeof dest, rhs, sizeof rhs) || dest[0] != '%') return 0;
  char ta[256], to[16], tb[256];
  if (three_token(rhs, ta, sizeof ta, to, sizeof to, tb, sizeof tb) != 1 ||
      !(strcmp(to, "&") == 0 || strcmp(to, "|") == 0 || strcmp(to, "^") == 0)) return 0;
  VecS leaves = {0}, vis = {0};
  int cost = bw_walk(texts, n, dest, &leaves, &vis, 0);
  vs_free(&vis);
  int k = leaves.n;
  if (cost == BW_BAIL || k == 0 || k > 4 || cost < 1) { vs_free(&leaves); return 0; }
  unsigned long long m = (k >= 6) ? ~0ULL : ((1ULL << (1 << k)) - 1);
  unsigned long long fp = bw_eval(texts, n, dest, &leaves, k, 0) & m;
  const char *post = bw_lookup(k, fp);
  if (!post) { vs_free(&leaves); return 0; }
  int ocost = 0; for (const char *p = post; *p; p++) if (*p == '~' || *p == '&' || *p == '|' || *p == '^') ocost++;
  if (ocost >= cost) { vs_free(&leaves); return 0; }      /* not strictly cheaper */
  if (saved) *saved = cost - ocost;
  if (ocost == 0) {                                       /* leaf / constant -> COPY / CONST */
    if (post[0] == 'L') snprintf(out, cap, "COPY %s", leaves.a[post[1] - '0']);
    else snprintf(out, cap, "CONST %s", post[0] == 'Z' ? "0" : "-1");
    vs_free(&leaves); return 1;
  }
  size_t off = (size_t)snprintf(out, cap, "REWRITE");      /* serialize optimal postfix */
  for (const char *p = post; *p; ) {
    char tok[260];
    if (*p == 'L') { int j = p[1] - '0'; p += 2; snprintf(tok, sizeof tok, "%s", leaves.a[j]); }
    else if (*p == 'Z') { p++; strcpy(tok, "0"); }
    else if (*p == 'O') { p++; strcpy(tok, "-1"); }
    else { tok[0] = *p; tok[1] = 0; p++; }
    int w = snprintf(out + off, cap - off, " %s", tok);
    if (w < 0 || (size_t)w >= cap - off) { vs_free(&leaves); return 0; }
    off += w;
  }
  vs_free(&leaves);
  return 1;
}

/* GF(2)-affine superoptimizer (handles shifts): a single-variable {^,~,<<c,>>c}
 * expr is f(v)=M.v^b, exact via f(0) + 64 basis evals (a proof that sees through
 * shifts). Look (M,b) up in gf2_lib1.txt and REWRITE when cheaper. */
typedef struct { unsigned long long b, cols[64]; char post[40]; } Gf2Ent;
static Gf2Ent *g_gf2; static int g_gf2_n, g_gf2_loaded;

static int gf2_cmp(const void *a, const void *b) {
  const Gf2Ent *x = a, *y = b;
  if (x->b != y->b) return x->b < y->b ? -1 : 1;
  for (int i = 0; i < 64; i++) if (x->cols[i] != y->cols[i]) return x->cols[i] < y->cols[i] ? -1 : 1;
  return 0;
}
static void gf2_load(void) {
  if (g_gf2_loaded) return;
  g_gf2_loaded = 1;
  const char *path = getenv("METTLE_ML_GF2LIB");
  if (!path) path = "tools/mlopt/gf2_lib1.txt";
  FILE *f = fopen(path, "r");
  if (!f) return;
  int cap = 8192; g_gf2 = malloc(cap * sizeof(Gf2Ent));
  char *ln = malloc(1 << 16);
  while (fgets(ln, 1 << 16, f)) {
    if (g_gf2_n == cap) { cap *= 2; g_gf2 = realloc(g_gf2, cap * sizeof(Gf2Ent)); }
    Gf2Ent e; char *p = strtok(ln, " ");
    if (!p) continue;
    e.b = strtoull(p, NULL, 10); int ok = 1;
    for (int i = 0; i < 64; i++) { p = strtok(NULL, " "); if (!p) { ok = 0; break; } e.cols[i] = strtoull(p, NULL, 10); }
    p = strtok(NULL, " \n"); if (!ok || !p) continue;
    strncpy(e.post, p, sizeof e.post - 1); e.post[sizeof e.post - 1] = 0;
    g_gf2[g_gf2_n++] = e;
  }
  free(ln); fclose(f);
  qsort(g_gf2, g_gf2_n, sizeof(Gf2Ent), gf2_cmp);
}
static const char *gf2_lookup(unsigned long long b, unsigned long long *cols) {
  int lo = 0, hi = g_gf2_n - 1;
  Gf2Ent key; key.b = b; memcpy(key.cols, cols, sizeof key.cols);
  while (lo <= hi) { int mid = (lo + hi) / 2; int c = gf2_cmp(&g_gf2[mid], &key);
    if (c == 0) return g_gf2[mid].post;
    if (c < 0) lo = mid + 1; else hi = mid - 1; }
  return NULL;
}

/* def classify for GF(2): 1=xor(a,b) 2=not(a) 3=shl(a,cnt) 4=shr(a,cnt) 0=leaf,
 * -1=non-GF(2) (bail). */
static int gf2_def_of(char **texts, int n, const char *name, char *a, char *b, int *cnt) {
  for (int i = 0; i < n; i++) {
    char dest[256], rhs[512];
    if (!split_def(texts[i], dest, sizeof dest, rhs, sizeof rhs)) continue;
    if (strcmp(dest, name) != 0) continue;
    char ta[256], to[16], tb[256];
    int g = three_token(rhs, ta, sizeof ta, to, sizeof to, tb, sizeof tb);
    if (g == 1) {
      if (strcmp(to, "^") == 0) { strcpy(a, ta); strcpy(b, tb); return 1; }
      if ((strcmp(to, "<<") == 0 || strcmp(to, ">>") == 0) && is_lit_tok(tb)) {
        strcpy(a, ta); *cnt = (int)atoll(tb); return to[0] == '<' ? 3 : 4;
      }
      return -1;
    }
    if (rhs[0] == '~' && rhs[1] && !strchr(rhs + 1, ' ')) { strcpy(a, rhs + 1); return 2; }
    return 0;
  }
  return 0;
}

static int gf2_walk(char **texts, int n, const char *name, char *var, int *havevar, int depth) {
  if (depth > 40) return BW_BAIL;
  if (is_lit_tok(name)) return 0;
  char a[256], b[256]; int cnt;
  int kd = gf2_def_of(texts, n, name, a, b, &cnt);
  if (kd == -1) return BW_BAIL;
  if (kd == 1) { int x = gf2_walk(texts, n, a, var, havevar, depth + 1);
    int y = gf2_walk(texts, n, b, var, havevar, depth + 1);
    return (x == BW_BAIL || y == BW_BAIL) ? BW_BAIL : x + y + 1; }
  if (kd == 2 || kd == 3 || kd == 4) { int x = gf2_walk(texts, n, a, var, havevar, depth + 1);
    return x == BW_BAIL ? BW_BAIL : x + 1; }
  if (*havevar) { if (strcmp(var, name) != 0) return BW_BAIL; }   /* 2nd distinct var -> bail */
  else { strcpy(var, name); *havevar = 1; }
  return 0;
}

static unsigned long long gf2_eval(char **texts, int n, const char *name,
                                   const char *var, unsigned long long vv, int depth) {
  if (is_lit_tok(name)) return (unsigned long long)atoll(name);
  char a[256], b[256]; int cnt;
  int kd = gf2_def_of(texts, n, name, a, b, &cnt);
  if (kd == 1) return gf2_eval(texts, n, a, var, vv, depth + 1) ^ gf2_eval(texts, n, b, var, vv, depth + 1);
  if (kd == 2) return ~gf2_eval(texts, n, a, var, vv, depth + 1);
  if (kd == 3) return gf2_eval(texts, n, a, var, vv, depth + 1) << (cnt & 63);
  if (kd == 4) return gf2_eval(texts, n, a, var, vv, depth + 1) >> (cnt & 63);
  return strcmp(name, var) == 0 ? vv : 0;
}

static int gf2_check(char **texts, int n, int ri, char *out, size_t cap, int *saved) {
  gf2_load();
  if (!g_gf2_n) return 0;
  char dest[256], rhs[512];
  if (!split_def(texts[ri], dest, sizeof dest, rhs, sizeof rhs) || dest[0] != '%') return 0;
  char var[256] = {0}; int have = 0;
  int cost = gf2_walk(texts, n, dest, var, &have, 0);
  if (cost == BW_BAIL || !have || cost < 1) return 0;
  unsigned long long b = gf2_eval(texts, n, dest, var, 0, 0);
  unsigned long long cols[64];
  for (int p = 0; p < 64; p++) cols[p] = gf2_eval(texts, n, dest, var, 1ULL << p, 0) ^ b;
  const char *post = gf2_lookup(b, cols);
  if (!post) return 0;
  int ocost = 0; for (const char *q = post; *q; q++) if (*q == '~' || *q == '^' || *q == '<' || *q == '>') ocost++;
  if (ocost >= cost) return 0;
  if (saved) *saved = cost - ocost;
  if (ocost == 0) {                                        /* leaf / constant */
    if (post[0] == 'L') snprintf(out, cap, "COPY %s", var);
    else snprintf(out, cap, "CONST %s", post[0] == 'Z' ? "0" : "-1");
    return 1;
  }
  size_t off = (size_t)snprintf(out, cap, "REWRITE");
  for (const char *q = post; *q; ) {
    char tok[260];
    if (*q == 'L') { q += 2; snprintf(tok, sizeof tok, "%s", var); }
    else if (*q == 'Z') { q++; strcpy(tok, "0"); }
    else if (*q == 'O') { q++; strcpy(tok, "-1"); }
    else if (*q == '^' || *q == '~') { tok[0] = *q; tok[1] = 0; q++; }
    else if (*q == '<' || *q == '>') { char c = *q; q++; int v = 0; while (*q >= '0' && *q <= '9') v = v * 10 + (*q++ - '0');
      snprintf(tok, sizeof tok, "%c%c%d", c, c, v); }                 /* <<N / >>N */
    else { q++; continue; }
    int w = snprintf(out + off, cap - off, " %s", tok);
    if (w < 0 || (size_t)w >= cap - off) return 0;
    off += w;
  }
  return off > 0;
}

/* Pretty-print a REWRITE postfix (operands, ~ & | ^, <<N, >>N) as infix, for the
 * --explain "after" column. Leading @ on operands is dropped (reads like source). */
static void infix_from_postfix(const char *pf, char *out, size_t cap) {
  char buf[600]; snprintf(buf, sizeof buf, "%s", pf);
  static char pool[64][256]; char *st[64]; int sp = 0, pn = 0;
  for (char *t = strtok(buf, " "); t && sp < 64 && pn < 64; t = strtok(NULL, " ")) {
    if (strcmp(t, "~") == 0) {
      if (sp < 1) break;
      snprintf(pool[pn], 256, "~%s", st[sp - 1]); st[sp - 1] = pool[pn++];
    } else if ((t[0] == '<' || t[0] == '>') && t[1] == t[0]) {
      if (sp < 1) break;
      snprintf(pool[pn], 256, "%s%c%c%s", st[sp - 1], t[0], t[0], t + 2); st[sp - 1] = pool[pn++];
    } else if (strcmp(t, "&") == 0 || strcmp(t, "|") == 0 || strcmp(t, "^") == 0) {
      if (sp < 2) break;
      snprintf(pool[pn], 256, "(%s %s %s)", st[sp - 2], t, st[sp - 1]); sp--; st[sp - 1] = pool[pn++];
    } else {
      snprintf(pool[pn], 256, "%s", t[0] == '@' ? t + 1 : t); st[sp++] = pool[pn++];
    }
  }
  snprintf(out, cap, "%s", sp > 0 ? st[sp - 1] : pf);
}

/* Reconstruct a source-level infix string for the expression rooted at `name`,
 * expanding pure-op %-temps (binary, unary ~, shift), stopping at leaves. Used for
 * the --explain "before" column so it reads like source, not raw IR temps. */
static void dag_infix(char **texts, int n, const char *name, char *out, size_t cap, int depth) {
  if (depth > 12 || cap < 8) { snprintf(out, cap, "%s", name[0] == '@' ? name + 1 : name); return; }
  if (name[0] == '%') {
    for (int i = 0; i < n; i++) {
      char dest[256], rhs[512];
      if (!split_def(texts[i], dest, sizeof dest, rhs, sizeof rhs) || strcmp(dest, name) != 0) continue;
      char a[256], o[16], b[256];
      if (three_token(rhs, a, sizeof a, o, sizeof o, b, sizeof b) == 1) {
        char as[256], bs[256];
        dag_infix(texts, n, a, as, sizeof as, depth + 1);
        dag_infix(texts, n, b, bs, sizeof bs, depth + 1);
        snprintf(out, cap, "(%s %s %s)", as, o, bs);
        return;
      }
      if (rhs[0] == '~' && rhs[1] && !strchr(rhs + 1, ' ')) {
        char as[256]; dag_infix(texts, n, rhs + 1, as, sizeof as, depth + 1);
        snprintf(out, cap, "~%s", as);
        return;
      }
      break;     /* defined here but not a pure op -> treat as a leaf */
    }
  }
  snprintf(out, cap, "%s", name[0] == '@' ? name + 1 : name);
}

static void process_function(const char *fname, char **texts, int *gidx, int n,
                             Buf *out, Buf *explain) {
  if (n <= 0 || n > 6000) return;

  /* Memory-bearing functions are processed: GVN only numbers pure 3-token
   * arithmetic (loads/stores/address-of are never numbered), so reuse is sound
   * regardless of surrounding memory. */
  int *kind = calloc(n, sizeof(int)), *op = calloc(n, sizeof(int));
  float *feat = calloc((size_t)n * NFEAT, sizeof(float));
  char **defn = calloc(n, sizeof(char *));
  VecS *uses = calloc(n, sizeof(VecS));
  for (int i = 0; i < n; i++) {
    classify(texts[i], &kind[i], &op[i]);
    defn[i] = parse_instr(texts[i], &uses[i]);
    feat[(size_t)i * NFEAT + 0] = (defn[i] && defn[i][0] == '%') ? 1.f : 0.f;
    feat[(size_t)i * NFEAT + 1] = (defn[i] && defn[i][0] == '@') ? 1.f : 0.f;
    int nc = count_consts(texts[i]); feat[(size_t)i * NFEAT + 2] = nc < 3 ? nc : 3;
    int nu = uses[i].n; feat[(size_t)i * NFEAT + 3] = nu < 4 ? nu : 4;
    float of[5]; operand_feats(texts[i], of);
    for (int j = 0; j < 5; j++) feat[(size_t)i * NFEAT + 4 + j] = of[j];
  }

  VecI du_s = {0}, du_d = {0};
  { VecS ld_name = {0}; VecI ld_idx = {0};
    for (int i = 0; i < n; i++) {
      for (int u = 0; u < uses[i].n; u++) {
        for (int k = ld_name.n - 1; k >= 0; k--)
          if (strcmp(ld_name.a[k], uses[i].a[u]) == 0) { vi_push(&du_s, ld_idx.a[k]); vi_push(&du_d, i); break; }
      }
      if (defn[i]) { int found = -1;
        for (int k = 0; k < ld_name.n; k++) if (strcmp(ld_name.a[k], defn[i]) == 0) { found = k; break; }
        if (found >= 0) ld_idx.a[found] = i; else { vs_push(&ld_name, strdup(defn[i])); vi_push(&ld_idx, i); }
      }
    }
    vs_free(&ld_name); vi_free(&ld_idx);
  }

  VecI *succ = calloc(n, sizeof(VecI)), *preds = calloc(n, sizeof(VecI));
  { VecS lbl_name = {0}; VecI lbl_idx = {0};
    for (int i = 0; i < n; i++) if (starts(texts[i], "label ")) {
      char t[256]; nth_token(texts[i], 1, t, sizeof t); vs_push(&lbl_name, strdup(t)); vi_push(&lbl_idx, i);
    }
    for (int i = 0; i < n; i++) {
      const char *s = texts[i];
      if (starts(s, "jump ")) {
        char t[256]; nth_token(s, 1, t, sizeof t);
        for (int k = 0; k < lbl_name.n; k++) if (strcmp(lbl_name.a[k], t) == 0) { vi_push(&succ[i], lbl_idx.a[k]); break; }
      } else if (starts(s, "branch_zero ") || starts(s, "branch_eq ")) {
        const char *ar = strstr(s, "-> ");
        if (ar) { char t[256]; nth_token(ar + 3, 0, t, sizeof t);
          for (int k = 0; k < lbl_name.n; k++) if (strcmp(lbl_name.a[k], t) == 0) { vi_push(&succ[i], lbl_idx.a[k]); break; } }
        if (i + 1 < n) vi_push(&succ[i], i + 1);
      } else if (starts(s, "return ")) {
      } else if (i + 1 < n) vi_push(&succ[i], i + 1);
    }
    vs_free(&lbl_name); vi_free(&lbl_idx);
  }
  for (int i = 0; i < n; i++) for (int k = 0; k < succ[i].n; k++) vi_push(&preds[succ[i].a[k]], i);

  VecI c_s = {0}, c_d = {0};
  for (int i = 0; i < n; i++) for (int k = 0; k < succ[i].n; k++) { vi_push(&c_s, i); vi_push(&c_d, succ[i].a[k]); }

  Intern gk = {0}; int *gkey = malloc(n * sizeof(int));
  for (int i = 0; i < n; i++) { gkey[i] = -1;
    char dest[256], rhs[512];
    if (split_def(texts[i], dest, sizeof dest, rhs, sizeof rhs)) {
      char *k = expr_key(rhs); if (k) { gkey[i] = intern(&gk, k); free(k); }
    }
  }
  VecI se_s = {0}, se_d = {0};
  { int *last = malloc(gk.keys.n ? gk.keys.n * sizeof(int) : sizeof(int));
    for (int i = 0; i < gk.keys.n; i++) last[i] = -1;
    for (int i = 0; i < n; i++) if (gkey[i] >= 0) {
      if (last[gkey[i]] >= 0) { vi_push(&se_s, last[gkey[i]]); vi_push(&se_d, i); }
      last[gkey[i]] = i;
    }
    free(last);
  }

  VecI dse_s = {0}, dse_d = {0};
  char *dom = NULL;
  { int multi = 0; if (gk.keys.n) { int *cnt = calloc(gk.keys.n, sizeof(int));
      for (int i = 0; i < n; i++) if (gkey[i] >= 0) cnt[gkey[i]]++;
      for (int k = 0; k < gk.keys.n; k++) if (cnt[k] > 1) multi = 1;
      free(cnt); }
    if (multi) {
      dom = dominators(preds, n);
      for (int k = 0; k < gk.keys.n; k++) {
        VecI idxs = {0};
        for (int i = 0; i < n; i++) if (gkey[i] == k) vi_push(&idxs, i);
        if (idxs.n >= 2) for (int ai = 0; ai < idxs.n; ai++) {
          int i = idxs.a[ai];
          for (int bi = ai - 1; bi >= 0; bi--) { int j = idxs.a[bi];
            if (dom[(size_t)i * n + j]) { vi_push(&dse_s, j); vi_push(&dse_d, i); break; } }
        }
        vi_free(&idxs);
      }
    }
  }

  int *esrc[NEDGE], *edst[NEDGE], ecnt[NEDGE];
  esrc[0] = du_s.a; edst[0] = du_d.a; ecnt[0] = du_s.n;
  esrc[1] = du_d.a; edst[1] = du_s.a; ecnt[1] = du_s.n;
  esrc[2] = c_s.a; edst[2] = c_d.a; ecnt[2] = c_s.n;
  esrc[3] = c_d.a; edst[3] = c_s.a; ecnt[3] = c_s.n;
  esrc[4] = se_s.a; edst[4] = se_d.a; ecnt[4] = se_s.n;
  esrc[5] = se_d.a; edst[5] = se_s.a; ecnt[5] = se_s.n;
  esrc[6] = dse_s.a; edst[6] = dse_d.a; ecnt[6] = dse_s.n;
  esrc[7] = dse_d.a; edst[7] = dse_s.a; ecnt[7] = dse_s.n;

  int *action = malloc(n * sizeof(int));
  gnn_forward(&G, n, kind, op, feat, esrc, edst, ecnt, action);
  /* COLLAPSE is the model's 6th action (same forward pass). METTLE_ML_COLLAPSE_ALL
   * runs the verifier on every root (diagnostic). */
  int *collapse_flag = calloc(n, sizeof(int));
  if (getenv("METTLE_ML_COLLAPSE_ALL")) {
    for (int i = 0; i < n; i++) collapse_flag[i] = 1;
  } else {
    for (int i = 0; i < n; i++) collapse_flag[i] = (action[i] == COLLAPSE_CLASS);
  }

  char **gvn_src = calloc(n, sizeof(char *));
  {
    Intern vk = {0}; VecS vk_a = {0}, vk_b = {0};
    int *keyid = malloc(n * sizeof(int));
    char **vdefn = calloc(n, sizeof(char *));
    for (int i = 0; i < n; i++) { keyid[i] = -1;
      char dest[256], rhs[512];
      if (split_def(texts[i], dest, sizeof dest, rhs, sizeof rhs)) {
        vdefn[i] = strdup(dest);
        if (dest[0] == '%') {
          char a[256], o[16], b[256];
          if (three_token(rhs, a, sizeof a, o, sizeof o, b, sizeof b)) {
            if (op_is_comm(o) && strcmp(a, b) > 0) { char tmp[256]; strcpy(tmp, a); strcpy(a, b); strcpy(b, tmp); }
            char key[600]; sprintf(key, "%s|%s|%s", o, a, b);
            int before = vk.keys.n; int id = intern(&vk, key);
            if (id == before) { vs_push(&vk_a, strdup(a)); vs_push(&vk_b, strdup(b)); }
            keyid[i] = id;
          }
        }
      }
    }
    int K = vk.keys.n;
    if (K > 0) {
      char *gen = calloc((size_t)n * K, 1), *kill = calloc((size_t)n * K, 1);
      for (int i = 0; i < n; i++) {
        if (vdefn[i]) for (int k = 0; k < K; k++)
          if (strcmp(vdefn[i], vk_a.a[k]) == 0 || strcmp(vdefn[i], vk_b.a[k]) == 0) kill[(size_t)i * K + k] = 1;
        /* a call or store may write a global / address-taken local through memory,
         * so it kills every expression that reads an @-symbol (%-temps are SSA and
         * unaffected). Without this, `x == 0` survives a call that writes x. */
        if (texts[i][0] == '*' || has_call(texts[i])) {
          for (int k = 0; k < K; k++)
            if (vk_a.a[k][0] == '@' || vk_b.a[k][0] == '@') kill[(size_t)i * K + k] = 1;
        }
        if (keyid[i] >= 0 && !kill[(size_t)i * K + keyid[i]]) gen[(size_t)i * K + keyid[i]] = 1;
      }
      char *ain = calloc((size_t)n * K, 1), *aout = malloc((size_t)n * K);
      memset(aout, 1, (size_t)n * K);
      int changed = 1;
      char *tmp = malloc(K);
      while (changed) { changed = 0;
        for (int i = 0; i < n; i++) {
          if (i == 0 || preds[i].n == 0) memset(tmp, 0, K);
          else { memset(tmp, 1, K);
            for (int pi = 0; pi < preds[i].n; pi++) { char *ao = aout + (size_t)preds[i].a[pi] * K;
              for (int k = 0; k < K; k++) tmp[k] &= ao[k]; } }
          if (i == 0) memset(tmp, 0, K);
          char *ai = ain + (size_t)i * K;
          int diff = memcmp(ai, tmp, K) != 0;
          if (diff) memcpy(ai, tmp, K);
          char *ao = aout + (size_t)i * K, *ki = kill + (size_t)i * K, *gi = gen + (size_t)i * K;
          for (int k = 0; k < K; k++) { char v = (ai[k] & !ki[k]) | gi[k];
            if (v != ao[k]) { ao[k] = v; changed = 1; } }
          if (diff) changed = 1;
        }
      }
      if (!dom) dom = dominators(preds, n);
      for (int i = 0; i < n; i++) {
        int e = keyid[i];
        if (e < 0 || !ain[(size_t)i * K + e]) continue;
        for (int j = 0; j < n; j++) if (j != i && keyid[j] == e && dom[(size_t)i * n + j]) {
          gvn_src[i] = strdup(vdefn[j]); break;
        }
      }
      free(gen); free(kill); free(ain); free(aout); free(tmp);
    }
    for (int i = 0; i < n; i++) free(vdefn[i]);
    free(vdefn); free(keyid); vs_free(&vk.keys); vs_free(&vk_a); vs_free(&vk_b);
  }

  /* AFFINE is opt-in (METTLE_ML_AFFINE): ~0 real wins post-classical and ~2x
   * compile time, so the default stays GVN+collapse. */
  VecS params = {0};
  int *aff_kind = calloc(n, sizeof(int)); char **aff_arg = calloc(n, sizeof(char *));
  if (getenv("METTLE_ML_AFFINE")) {
    infer_params(texts, n, &params);
    affine_run(texts, n, &params, aff_kind, aff_arg);
  }

  for (int j = 0; j < n; j++) {
    char drhs[512] = "", ddst[256] = "";
    split_def(texts[j], ddst, sizeof ddst, drhs, sizeof drhs);
    /* explain record (TSV): fn, gidx, kind, before-expr, after-expr, ops-saved.
     * `bexpr` is the source-level reconstruction of the original expression. */
    char ex[1200], after[600], bexpr[512]; int saved = 0;
    if (ddst[0]) dag_infix(texts, n, ddst, bexpr, sizeof bexpr, 0);
    else snprintf(bexpr, sizeof bexpr, "%s", drhs);
    if (collapse_flag[j]) {
      char rw[600];
      int bw = superopt_check(texts, n, j, rw, sizeof rw, &saved);
      int gf = bw ? 0 : gf2_check(texts, n, j, rw, sizeof rw, &saved);
      if (bw || gf) {
        char line[700];
        snprintf(line, sizeof line, "%s %d %s\n", fname, gidx[j], rw);
        buf_add(out, line);
        if (strncmp(rw, "REWRITE ", 8) == 0) infix_from_postfix(rw + 8, after, sizeof after);
        else snprintf(after, sizeof after, "%s", strchr(rw, ' ') ? strchr(rw, ' ') + 1 : rw);
        snprintf(ex, sizeof ex, "%s\t%d\t%s superoptimize\t%s\t%s\t%d\n",
                 fname, gidx[j], bw ? "bitwise" : "xor-shift", bexpr, after, saved);
        buf_add(explain, ex);
        continue;
      }
      char arg[256]; int ck;
      if (collapse_check(texts, n, j, arg, sizeof arg, &ck, &saved)) {
        char line[700];
        snprintf(line, sizeof line, "%s %d %s %s\n", fname, gidx[j],
                 ck == 1 ? "CONST" : "COPY", arg);
        buf_add(out, line);
        snprintf(ex, sizeof ex, "%s\t%d\tcollapse\t%s\t%s\t%d\n",
                 fname, gidx[j], bexpr, arg, saved);
        buf_add(explain, ex);
        continue;
      }
    }
    /* Speculative dead-code delete (--ml-opt-speculative): the model's DELETE
     * action carries no construction-time proof, so the disposition is a bare
     * NOP that ml_opt.c applies ONLY behind the interpreter-differential gate.
     * Control flow and locals are never proposed; stores/defs/calls are fair
     * game because the validator observes buffers, globals, and the extern
     * trace. */
    static int speculative = -1;
    if (speculative < 0) {
      const char *e = getenv("METTLE_ML_SPECULATIVE");
      speculative = (e && e[0] && strcmp(e, "0") != 0) ? 1 : 0;
    }
    if (speculative && action[j] == DELETE_CLASS &&
        (kind[j] == 5 || kind[j] == 6 || kind[j] == 7 || kind[j] == 8)) {
      char line[700];
      snprintf(line, sizeof line, "%s %d NOP\n", fname, gidx[j]);
      buf_add(out, line);
      snprintf(ex, sizeof ex, "%s\t%d\tmodel delete\t%s\t(deleted)\t1\n", fname,
               gidx[j], bexpr[0] ? bexpr : texts[j]);
      buf_add(explain, ex);
      continue;
    }
    if (action[j] == AFFINE_CLASS && aff_kind[j]) {
      char line[700];
      snprintf(line, sizeof line, "%s %d %s %s\n", fname, gidx[j],
               aff_kind[j] == 1 ? "CONST" : "COPY", aff_arg[j]);
      buf_add(out, line);
      snprintf(ex, sizeof ex, "%s\t%d\taffine\t%s\t%s\t1\n", fname, gidx[j], bexpr, aff_arg[j]);
      buf_add(explain, ex);
      continue;
    }
    if (gvn_src[j] && action[j] == GVN_CLASS) {
      char a[256], o[16], b[256];
      if (drhs[0] == 0 || three_token(drhs, a, sizeof a, o, sizeof o, b, sizeof b) == 0) continue;
      char line[700];
      snprintf(line, sizeof line, "%s %d COPY %s\n", fname, gidx[j], gvn_src[j]);
      buf_add(out, line);
      snprintf(ex, sizeof ex, "%s\t%d\tGVN reuse\t%s\t%s\t1\n", fname, gidx[j], bexpr, gvn_src[j]);
      buf_add(explain, ex);
    }
  }

  for (int i = 0; i < n; i++) free(aff_arg[i]);
  free(aff_kind); free(aff_arg); vs_free(&params); free(collapse_flag);
  for (int i = 0; i < n; i++) { free(defn[i]); free(gvn_src[i]); vs_free(&uses[i]); }
  free(defn); free(gvn_src); free(uses); free(kind); free(op); free(feat);
  free(action); free(gkey); vs_free(&gk.keys);
  vi_free(&du_s); vi_free(&du_d); vi_free(&c_s); vi_free(&c_d);
  vi_free(&se_s); vi_free(&se_d); vi_free(&dse_s); vi_free(&dse_d);
  for (int i = 0; i < n; i++) { vi_free(&succ[i]); vi_free(&preds[i]); }
  free(succ); free(preds); free(dom);
}

static char *slurp(const char *path, long *len) {
  FILE *f = fopen(path, "rb"); if (!f) return NULL;
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  char *b = malloc(n + 1); if (!b) { fclose(f); return NULL; }
  size_t got = fread(b, 1, n, f); fclose(f); b[got] = 0; if (len) *len = (long)got;
  return b;
}

int ml_gnn_run(const char *ir_dump_path, char **out_disp) {
  if (!load_weights()) return 0;
  long len; char *text = slurp(ir_dump_path, &len);
  if (!text) return 0;

  Buf out = {0}, explain = {0};
  char fname[256] = {0}; int in_fn = 0;
  VecS texts = {0}; VecI gidx = {0};

  char *p = text;
  while (*p) {
    char *nl = strchr(p, '\n'); if (nl) *nl = 0;
    char *line = p;
    if (starts(line, "function ")) {
      nth_token(line, 1, fname, sizeof fname);
      in_fn = 1; texts.n = 0; gidx.n = 0;
    } else if (in_fn && line[0] == '}') {
      if (texts.n > 0) process_function(fname, texts.a, gidx.a, texts.n, &out, &explain);
      for (int i = 0; i < texts.n; i++) free(texts.a[i]);
      texts.n = 0; gidx.n = 0; in_fn = 0;
    } else if (in_fn) {
      const char *q = line; while (*q == ' ' || *q == '\t') q++;
      if (*q >= '0' && *q <= '9') {
        int gi = 0; const char *r = q; while (*r >= '0' && *r <= '9') { gi = gi * 10 + (*r - '0'); r++; }
        if (*r == ':') { r++; while (*r == ' ' || *r == '\t') r++;
          size_t l = strlen(r); while (l && (r[l - 1] == ' ' || r[l - 1] == '\t' || r[l - 1] == '\r')) l--;
          /* drop dead NOP nodes: noise the model wasn't trained on (gidx keeps
           * the original index so dispositions still target the right instr) */
          if (!(l == 3 && r[0] == 'n' && r[1] == 'o' && r[2] == 'p')) {
            char *t = malloc(l + 1); memcpy(t, r, l); t[l] = 0;
            vs_push(&texts, t); vi_push(&gidx, gi);
          }
        }
      }
    }
    if (!nl) break;
    p = nl + 1;
  }
  for (int i = 0; i < texts.n; i++) free(texts.a[i]);
  vs_free(&texts); vi_free(&gidx); free(text);

  if (explain.s) {
    FILE *ef = fopen("_mlopt.explain", "w");
    if (ef) { fputs(explain.s, ef); fclose(ef); }
    free(explain.s);
  } else {
    remove("_mlopt.explain");                 /* no transforms this run */
  }

  if (!out.s) out.s = calloc(1, 1);
  *out_disp = out.s;
  return 1;
}
