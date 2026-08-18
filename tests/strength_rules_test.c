/* Gate for the shared strength-reduction table (src/codegen/binary/
 * strength_rules.c).
 *
 * Every backend now takes its "is there a cheaper form of x <op> C" answer
 * from one table, so that table is worth proving rather than trusting. Two
 * things are checked here, against the real module (no copies):
 *
 *   1. Every rewrite the classifier names computes what the original
 *      operation computes. The shift/mask forms are simulated exactly as the
 *      backends emit them, over a probe set that includes the signs, the
 *      boundaries, and the values that break naive lowerings.
 *   2. The Granlund-Montgomery parameters divide correctly, for signed and
 *      unsigned, over an exhaustive small range plus a sparse sweep of large
 *      and adversarial divisors.
 *
 * This is what lets a backend delete its private copy of the magic math: the
 * remaining implementation is the one under test. */
#include "codegen/binary/strength_rules.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

typedef unsigned __int128 TestU128;
typedef __int128 TestS128;

static int g_failures = 0;
/* Per-kind exercise counts. A gate that silently covers nothing is worse
 * than no gate, so the run asserts every kind the table can name was
 * actually simulated. */
static long long g_kind_hits[CG_SR_REM_MAGIC + 1];
static const char *const g_kind_names[] = {
    "NONE",      "MUL_SHL",   "MUL_SHL_ADD", "MUL_SHL_SUB", "UDIV_SHR",
    "UREM_AND",  "SDIV_POW2", "SREM_POW2",   "DIV_MAGIC",   "REM_MAGIC"};

static void fail(const char *what, long long c, long long n) {
  if (g_failures < 20) {
    fprintf(stderr, "FAIL %s: c=%lld n=%lld\n", what, c, n);
  }
  g_failures++;
}

/* ---- simulate what a backend emits for each rewrite kind ---------------- */

static int64_t sim_mulhi_s(int64_t a, int64_t b) {
  return (int64_t)(((TestS128)a * (TestS128)b) >> 64);
}

static uint64_t sim_mulhi_u(uint64_t a, uint64_t b) {
  return (uint64_t)(((TestU128)a * (TestU128)b) >> 64);
}

/* The signed magic sequence: mulhi, the sign-disagreement correction, the
 * post-shift, then add the sign bit. */
static int64_t sim_div_magic_s(int64_t n, int64_t d, const CgStrengthRewrite *r) {
  int64_t q = sim_mulhi_s(n, (int64_t)r->magic);
  if (d > 0 && r->magic < 0) {
    q += n;
  }
  if (d < 0 && r->magic > 0) {
    q -= n;
  }
  q >>= r->shift;
  q += (int64_t)((uint64_t)q >> 63);
  return q;
}

/* The unsigned magic sequence, including the overflow-safe reconstruction
 * that magic_add selects. */
static uint64_t sim_div_magic_u(uint64_t n, const CgStrengthRewrite *r) {
  uint64_t hi = sim_mulhi_u(n, (uint64_t)r->magic);
  if (!r->magic_add) {
    return hi >> r->shift;
  }
  uint64_t t = ((n - hi) >> 1) + hi;
  return t >> (r->shift - 1);
}

/* Apply whatever the classifier named, exactly as a backend would. Returns 0
 * when the kind is one no backend simulates here. */
static int apply_rewrite(const CgStrengthRewrite *r, char op, int64_t n,
                         int64_t c, int is_unsigned, int64_t *out) {
  if (r->kind >= 0 && r->kind <= CG_SR_REM_MAGIC) {
    g_kind_hits[r->kind]++;
  }
  switch (r->kind) {
  case CG_SR_MUL_SHL:
    *out = (int64_t)((uint64_t)n << r->shift);
    return 1;
  case CG_SR_MUL_SHL_ADD:
    *out = (int64_t)(((uint64_t)n << r->shift) + (uint64_t)n);
    return 1;
  case CG_SR_MUL_SHL_SUB:
    *out = (int64_t)(((uint64_t)n << r->shift) - (uint64_t)n);
    return 1;
  case CG_SR_UDIV_SHR:
    *out = (int64_t)((uint64_t)n >> r->shift);
    return 1;
  case CG_SR_UREM_AND:
    *out = (int64_t)((uint64_t)n & (uint64_t)r->mask);
    return 1;
  case CG_SR_SDIV_POW2: {
    /* bias = (n < 0) ? (2^k - 1) : 0; q = (n + bias) >> k */
    int64_t bias = (int64_t)(((uint64_t)(n >> 63)) >> (64 - r->shift));
    *out = (n + bias) >> r->shift;
    return 1;
  }
  case CG_SR_SREM_POW2: {
    int64_t bias = (int64_t)(((uint64_t)(n >> 63)) >> (64 - r->shift));
    int64_t q = (n + bias) >> r->shift;
    *out = n - (q << r->shift);
    return 1;
  }
  case CG_SR_DIV_MAGIC:
    *out = is_unsigned ? (int64_t)sim_div_magic_u((uint64_t)n, r)
                       : sim_div_magic_s(n, c, r);
    return 1;
  case CG_SR_REM_MAGIC: {
    int64_t q = is_unsigned ? (int64_t)sim_div_magic_u((uint64_t)n, r)
                            : sim_div_magic_s(n, c, r);
    *out = is_unsigned
               ? (int64_t)((uint64_t)n - (uint64_t)q * (uint64_t)c)
               : n - q * c;
    return 1;
  }
  case CG_SR_NONE:
  default:
    (void)op;
    return 0;
  }
}

/* ---- probes ------------------------------------------------------------- */

static const int64_t g_signed_probes[] = {
    0, 1, -1, 2, -2, 3, -3, 7, -7, 10, -10, 63, 64, 65, -63, -64, -65,
    100, -100, 1000, -1000, 32767, 32768, -32768, 65535, 65536, -65536,
    123456789, -123456789, 2147483647LL, -2147483648LL, 4294967296LL,
    -4294967296LL, 1000000007LL, -1000000007LL, 1234567890123LL,
    -1234567890123LL, 4611686018427387904LL, -4611686018427387904LL,
    9223372036854775807LL};

static const uint64_t g_unsigned_probes[] = {
    0, 1, 2, 3, 7, 10, 63, 64, 65, 100, 1000, 32767, 32768, 65535, 65536,
    123456789, 2147483647ULL, 2147483648ULL, 4294967295ULL, 4294967296ULL,
    1000000007ULL, 1234567890123ULL, 9223372036854775807ULL,
    9223372036854775808ULL, 18446744073709551615ULL};

#define SIGNED_PROBE_COUNT                                                    \
  ((int)(sizeof(g_signed_probes) / sizeof(g_signed_probes[0])))
#define UNSIGNED_PROBE_COUNT                                                  \
  ((int)(sizeof(g_unsigned_probes) / sizeof(g_unsigned_probes[0])))

/* Check every rule the classifier offers for this divisor/multiplier. */
static void check_constant(int64_t c) {
  CgStrengthRewrite r;
  int64_t got;

  /* multiply, signed and unsigned operands alike (the shift forms agree mod
   * 2^64, which is what both signednesses observe) */
  if (c > 0 && cg_strength_classify('*', c, 0, &r)) {
    for (int i = 0; i < SIGNED_PROBE_COUNT; i++) {
      int64_t n = g_signed_probes[i];
      int64_t want = (int64_t)((uint64_t)n * (uint64_t)c);
      if (apply_rewrite(&r, '*', n, c, 0, &got) && got != want) {
        fail("mul", c, n);
      }
    }
  }

  /* signed divide and remainder */
  if (cg_strength_classify('/', c, 0, &r)) {
    for (int i = 0; i < SIGNED_PROBE_COUNT; i++) {
      int64_t n = g_signed_probes[i];
      if (n == INT64_MIN && c == -1) {
        continue; /* the one case native division traps on too */
      }
      if (apply_rewrite(&r, '/', n, c, 0, &got) && got != n / c) {
        fail("sdiv", c, n);
      }
    }
  }
  if (cg_strength_classify('%', c, 0, &r)) {
    for (int i = 0; i < SIGNED_PROBE_COUNT; i++) {
      int64_t n = g_signed_probes[i];
      if (n == INT64_MIN && c == -1) {
        continue;
      }
      if (apply_rewrite(&r, '%', n, c, 0, &got) && got != n % c) {
        fail("srem", c, n);
      }
    }
  }

  /* unsigned divide and remainder */
  if (c > 0) {
    uint64_t uc = (uint64_t)c;
    if (cg_strength_classify('/', c, 1, &r)) {
      for (int i = 0; i < UNSIGNED_PROBE_COUNT; i++) {
        uint64_t n = g_unsigned_probes[i];
        if (apply_rewrite(&r, '/', (int64_t)n, c, 1, &got) &&
            (uint64_t)got != n / uc) {
          fail("udiv", c, (int64_t)n);
        }
      }
    }
    if (cg_strength_classify('%', c, 1, &r)) {
      for (int i = 0; i < UNSIGNED_PROBE_COUNT; i++) {
        uint64_t n = g_unsigned_probes[i];
        if (apply_rewrite(&r, '%', (int64_t)n, c, 1, &got) &&
            (uint64_t)got != n % uc) {
          fail("urem", c, (int64_t)n);
        }
      }
    }
  }
}

int main(void) {
  CgStrengthRewrite r;

  /* Divide by zero must never be reduced: the runtime trap has to fire. */
  if (cg_strength_classify('/', 0, 0, &r) ||
      cg_strength_classify('%', 0, 0, &r) ||
      cg_strength_classify('/', 0, 1, &r)) {
    fprintf(stderr, "FAIL: divide by zero was strength-reduced\n");
    g_failures++;
  }

  /* Exhaustive over the small divisors real code actually uses. */
  for (int64_t c = 1; c <= 20000; c++) {
    check_constant(c);
    check_constant(-c);
  }

  /* Adversarial shapes: powers of two and their neighbours, where the
   * shift/mask rows and the magic rows meet. */
  for (int k = 1; k < 62; k++) {
    int64_t p = (int64_t)1 << k;
    check_constant(p);
    check_constant(p - 1);
    check_constant(p + 1);
    check_constant(-p);
    check_constant(-(p - 1));
    check_constant(-(p + 1));
  }

  /* Sparse large sweep, including past the 32-bit boundary where a backend's
   * own magic helper used to give up. */
  for (int64_t c = 1000000; c < 4000000000LL; c += 7654321) {
    check_constant(c);
    check_constant(-c);
  }
  static const int64_t big[] = {2147483647LL,          2147483648LL,
                                4294967295LL,          4294967296LL,
                                1000000007LL,          999999937LL,
                                1234567890123LL,       1000000000000LL,
                                4611686018427387903LL, 4611686018427387904LL,
                                9223372036854775806LL, 9223372036854775807LL};
  for (int i = 0; i < (int)(sizeof(big) / sizeof(big[0])); i++) {
    check_constant(big[i]);
    check_constant(-big[i]);
  }

  for (int k = CG_SR_MUL_SHL; k <= CG_SR_REM_MAGIC; k++) {
    printf("  %-12s %lld checks\n", g_kind_names[k], g_kind_hits[k]);
    if (g_kind_hits[k] == 0) {
      fprintf(stderr, "FAIL: rewrite kind %s was never exercised\n",
              g_kind_names[k]);
      g_failures++;
    }
  }

  if (g_failures) {
    fprintf(stderr, "RESULT: FAIL (%d)\n", g_failures);
    return 1;
  }
  printf("RESULT: PASS\n");
  return 0;
}
