#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <inttypes.h>

#include "../bench_time.h"


#define DOCS 20
#define LINES_A 620
#define MAX_LINES 800
#define LINE_CAP 64
#define DP_STRIDE 801
#define PASSES 3

typedef struct {
    uint8_t *text;
    int32_t *off;
    int32_t *len;
    int32_t *hash;
    int32_t count;
} Doc;

static int32_t emit_cstr(uint8_t *dst, int32_t pos, const char *s) {
  int32_t i = 0;
  while (s[i] != 0) {
    dst[pos + i] = (uint8_t)s[i];
    i += 1;
  }
  return pos + i;
}

static int32_t emit_int(uint8_t *dst, int32_t pos, int32_t v) {
  uint8_t tmp[16];
  int32_t n = 0;
  int32_t x = v;
  if (x == 0) {
    tmp[0] = 48;
    n = 1;
  }
  while (x > 0) {
    tmp[n] = (uint8_t)(48 + x % 10);
    x = x / 10;
    n += 1;
  }
  while (n > 0) {
    n -= 1;
    dst[pos] = tmp[n];
    pos += 1;
  }
  return pos;
}

static const char *keyword(int32_t k) {
  switch (k) {
    case 0: return "resolve";
    case 1: return "commit";
    case 2: return "rebase";
    case 3: return "collect";
    case 4: return "verify";
    case 5: return "expand";
    case 6: return "reduce";
    default: return "settle";
  }
}

static uint32_t next_rand(uint32_t *state) {
  uint32_t s = *state;
  s = s ^ (s << 13);
  s = s ^ (s >> 17);
  s = s ^ (s << 5);
  *state = s;
  return s;
}

static int32_t line_hash(uint8_t *text, int32_t off, int32_t len) {
  uint32_t h = 2166136261ULL;
  int32_t i = 0;
  while (i < len) {
    h = h ^ (uint32_t)text[off + i];
    h = h * 16777619;
    i += 1;
  }
  return (int32_t)(h & 2147483647);
}

static void build_doc(Doc *d, uint32_t *state, int32_t lines) {
  int32_t pos = 0;
  int32_t i = 0;
  while (i < lines) {
    d->off[i] = pos;
    uint32_t r = next_rand(state);
    pos = emit_cstr(d->text, pos, "  ");
    pos = emit_cstr(d->text, pos, keyword((int32_t)(r % 8)));
    pos = emit_cstr(d->text, pos, "(node_");
    pos = emit_int(d->text, pos, (int32_t)((r >> 3) % 4096));
    pos = emit_cstr(d->text, pos, ", depth=");
    pos = emit_int(d->text, pos, (int32_t)((r >> 15) % 64));
    pos = emit_cstr(d->text, pos, ");");
    d->len[i] = pos - d->off[i];
    d->hash[i] = line_hash(d->text, d->off[i], d->len[i]);
    i += 1;
  }
  d->count = lines;
}

static void derive_doc(Doc *dst, Doc *src, uint32_t *state) {
  int32_t pos = 0;
  int32_t out = 0;
  int32_t i = 0;
  while (i < src->count && out < MAX_LINES) {
    uint32_t r = next_rand(state) % 100;
    if (r < 8) {
      i += 1;
    } else if (r < 14) {
      dst->off[out] = pos;
      uint32_t q = next_rand(state);
      pos = emit_cstr(dst->text, pos, "  inserted_");
      pos = emit_int(dst->text, pos, (int32_t)(q % 9999));
      pos = emit_cstr(dst->text, pos, ";");
      dst->len[out] = pos - dst->off[out];
      dst->hash[out] = line_hash(dst->text, dst->off[out], dst->len[out]);
      out += 1;
    } else {
      dst->off[out] = pos;
      int32_t j = 0;
      while (j < src->len[i]) {
        dst->text[pos + j] = src->text[src->off[i] + j];
        j += 1;
      }
      pos = pos + src->len[i];
      dst->len[out] = src->len[i];
      dst->hash[out] = src->hash[i];
      out += 1;
      i += 1;
    }
  }
  dst->count = out;
}

static int32_t lcs_fill(int32_t *dp, Doc *a, Doc *b) {
  int32_t na = a->count;
  int32_t nb = b->count;
  int32_t j = 0;
  while (j <= nb) {
    dp[j] = 0;
    j += 1;
  }
  int32_t i = 1;
  while (i <= na) {
    dp[i * DP_STRIDE] = 0;
    int32_t ah = a->hash[i - 1];
    int32_t row = i * DP_STRIDE;
    int32_t prev = (i - 1) * DP_STRIDE;
    j = 1;
    while (j <= nb) {
      if (ah == b->hash[j - 1]) {
        dp[row + j] = dp[prev + j - 1] + 1;
      } else {
        int32_t up = dp[prev + j];
        int32_t left = dp[row + j - 1];
        if (up >= left) {
          dp[row + j] = up;
        } else {
          dp[row + j] = left;
        }
      }
      j += 1;
    }
    i += 1;
  }
  return dp[na * DP_STRIDE + nb];
}

static uint64_t backtrack(int32_t *dp, Doc *a, Doc *b, int32_t *out_same, int32_t *out_del, int32_t *out_ins) {
  uint64_t h = 14695981039346656037ULL;
  int32_t i = a->count;
  int32_t j = b->count;
  int32_t same = 0;
  int32_t del = 0;
  int32_t ins = 0;
  while (i > 0 && j > 0) {
    if (a->hash[i - 1] == b->hash[j - 1]) {
      h = h ^ (uint64_t)a->hash[i - 1];
      h = h * 1099511628211ULL;
      same += 1;
      i -= 1;
      j -= 1;
    } else if (dp[(i - 1) * DP_STRIDE + j] >= dp[i * DP_STRIDE + j - 1]) {
      h = h ^ (uint64_t)(a->hash[i - 1] + 1);
      h = h * 1099511628211ULL;
      del += 1;
      i -= 1;
    } else {
      h = h ^ (uint64_t)(b->hash[j - 1] + 2);
      h = h * 1099511628211ULL;
      ins += 1;
      j -= 1;
    }
  }
  del += i;
  ins += j;
  *out_same = same;
  *out_del = del;
  *out_ins = ins;
  return h;
}

static uint64_t round_trip(Doc *a, Doc *b, int32_t *dp, int32_t *out_lcs) {
  uint32_t state = 2463534242ULL;
  uint64_t h = 1469598103934665603ULL;
  int32_t total_lcs = 0;
  int32_t doc = 0;
  while (doc < DOCS) {
    build_doc(a, &state, LINES_A);
    derive_doc(b, a, &state);
    int32_t lcs = lcs_fill(dp, a, b);
    total_lcs += lcs;
    int32_t same = 0;
    int32_t del = 0;
    int32_t ins = 0;
    uint64_t bh = backtrack(dp, a, b, &same, &del, &ins);
    h = h * 1000003 + bh;
    h = h * 31 + (uint64_t)lcs;
    h = h * 31 + (uint64_t)(same * 7 + del * 13 + ins * 17);
    h = h * 31 + (uint64_t)b->count;
    doc += 1;
  }
  *out_lcs = total_lcs;
  return h;
}

int main(void) {
    Doc a;
    Doc b;
    a.text = (uint8_t *)malloc((size_t)MAX_LINES * LINE_CAP);
    a.off = (int32_t *)malloc((size_t)MAX_LINES * 4);
    a.len = (int32_t *)malloc((size_t)MAX_LINES * 4);
    a.hash = (int32_t *)malloc((size_t)MAX_LINES * 4);
    a.count = 0;
    b.text = (uint8_t *)malloc((size_t)MAX_LINES * LINE_CAP);
    b.off = (int32_t *)malloc((size_t)MAX_LINES * 4);
    b.len = (int32_t *)malloc((size_t)MAX_LINES * 4);
    b.hash = (int32_t *)malloc((size_t)MAX_LINES * 4);
    b.count = 0;
    int32_t *dp = (int32_t *)malloc((size_t)DP_STRIDE * DP_STRIDE * 4);
    if (a.text == NULL || b.text == NULL || dp == NULL) {
        printf("malloc failed\n");
        return 1;
    }

    printf("Diff: %d document pairs, %d lines, LCS table %dx%d\n",
           DOCS, LINES_A, DP_STRIDE, DP_STRIDE);

    int32_t lcs = 0;
    uint64_t check = round_trip(&a, &b, dp, &lcs);
    printf("  total LCS = %d, derived lines = %d\n", lcs, b.count);
    printf("Checksum = %" PRIu64 "\n", check);

    printf("Benchmark: %d passes\n", PASSES);

    uint64_t t0 = bench_time_us();
    uint64_t bench_hash = 0;
    int32_t pass = 0;
    while (pass < PASSES) {
        int32_t l2 = 0;
        bench_hash = bench_hash * 1000003 + round_trip(&a, &b, dp, &l2);
        pass += 1;
    }
    uint64_t elapsed_us = bench_time_us() - t0;

    printf("Bench hash = %" PRIu64 "\n", bench_hash);
    printf("Time: %" PRIu64 " us\n", elapsed_us);

    uint64_t per_pass_us = elapsed_us / (uint64_t)PASSES;
    printf("Per pass: ~%" PRIu64 " us\n", per_pass_us);

    free(a.text);
    free(b.text);
    free(dp);
    return 0;
}
