#include "smt.h"

typedef struct {
  SmtCtx *ctx;
  unsigned eb;
  unsigned sb;
  unsigned w;
  unsigned ew;
  int64_t bias;
} FpFormat;

typedef struct {
  SmtTerm sign;
  SmtTerm nan;
  SmtTerm inf;
  SmtTerm zero;
  SmtTerm e;
  SmtTerm s;
} FpParts;

static FpFormat fp_format(SmtCtx *ctx, unsigned eb, unsigned sb) {
  FpFormat f;
  f.ctx = ctx;
  f.eb = eb;
  f.sb = sb;
  f.w = eb + sb;
  f.ew = eb + 10u;
  f.bias = ((int64_t)1 << (eb - 1u)) - 1;
  return f;
}

static SmtTerm k(SmtCtx *ctx, unsigned width, uint64_t value) {
  return smt_bv_u64(ctx, width, value);
}

static SmtTerm ks(SmtCtx *ctx, unsigned width, int64_t value) {
  return smt_bv_i64(ctx, width, value);
}

static SmtTerm add(SmtCtx *ctx, SmtTerm a, SmtTerm b) {
  return smt_mk2(ctx, SMT_K_BVADD, a, b);
}

static SmtTerm sub(SmtCtx *ctx, SmtTerm a, SmtTerm b) {
  return smt_mk2(ctx, SMT_K_BVSUB, a, b);
}

static SmtTerm shl(SmtCtx *ctx, SmtTerm a, SmtTerm b) {
  return smt_mk2(ctx, SMT_K_BVSHL, a, b);
}

static SmtTerm lshr(SmtCtx *ctx, SmtTerm a, SmtTerm b) {
  return smt_mk2(ctx, SMT_K_BVLSHR, a, b);
}

static SmtTerm ult(SmtCtx *ctx, SmtTerm a, SmtTerm b) {
  return smt_mk2(ctx, SMT_K_BVULT, a, b);
}

static SmtTerm slt(SmtCtx *ctx, SmtTerm a, SmtTerm b) {
  return smt_mk2(ctx, SMT_K_BVSLT, a, b);
}

static SmtTerm is_zero_bv(SmtCtx *ctx, SmtTerm a) {
  return smt_eq(ctx, a, k(ctx, smt_sort_width(ctx, smt_term_sort(ctx, a)), 0));
}

static unsigned width_of(SmtCtx *ctx, SmtTerm a) {
  return smt_sort_width(ctx, smt_term_sort(ctx, a));
}

static SmtTerm resize_u(SmtCtx *ctx, SmtTerm a, unsigned width) {
  unsigned have = width_of(ctx, a);
  if (have == width) {
    return a;
  }
  return have < width ? smt_zext(ctx, a, width - have)
                      : smt_extract(ctx, a, width - 1u, 0);
}

static SmtTerm resize_s(SmtCtx *ctx, SmtTerm a, unsigned width) {
  unsigned have = width_of(ctx, a);
  if (have == width) {
    return a;
  }
  return have < width ? smt_sext(ctx, a, width - have)
                      : smt_extract(ctx, a, width - 1u, 0);
}

static SmtTerm count_leading_zeros(SmtCtx *ctx, SmtTerm x, unsigned out_width,
                                   SmtTerm *shifted) {
  unsigned w = width_of(ctx, x);
  unsigned step = 1;
  while (step * 2u < w) {
    step *= 2u;
  }
  SmtTerm count = k(ctx, out_width, 0);
  for (; step >= 1u; step /= 2u) {
    SmtTerm top = smt_extract(ctx, x, w - 1u, w - step);
    SmtTerm empty = is_zero_bv(ctx, top);
    x = smt_ite(ctx, empty, shl(ctx, x, k(ctx, w, step)), x);
    count =
        smt_ite(ctx, empty, add(ctx, count, k(ctx, out_width, step)), count);
    if (step == 1u) {
      break;
    }
  }
  *shifted = x;
  return count;
}

static FpParts fp_unpack(const FpFormat *f, SmtTerm x) {
  SmtCtx *ctx = f->ctx;
  FpParts p;
  SmtTerm sign = smt_extract(ctx, x, f->w - 1u, f->w - 1u);
  SmtTerm ef = smt_extract(ctx, x, f->w - 2u, f->sb - 1u);
  SmtTerm mf = smt_extract(ctx, x, f->sb - 2u, 0);
  SmtTerm ef_zero = is_zero_bv(ctx, ef);
  SmtTerm ef_ones = smt_eq(ctx, ef, k(ctx, f->eb, (1ull << f->eb) - 1u));
  SmtTerm mf_zero = is_zero_bv(ctx, mf);
  p.sign = smt_eq(ctx, sign, k(ctx, 1, 1));
  p.nan = smt_and(ctx, ef_ones, smt_not(ctx, mf_zero));
  p.inf = smt_and(ctx, ef_ones, mf_zero);
  p.zero = smt_and(ctx, ef_zero, mf_zero);
  SmtTerm subnormal = smt_and(ctx, ef_zero, smt_not(ctx, mf_zero));
  SmtTerm normal_s = smt_mk2(ctx, SMT_K_CONCAT, k(ctx, 1, 1), mf);
  SmtTerm normal_e =
      sub(ctx, resize_u(ctx, ef, f->ew), k(ctx, f->ew, (uint64_t)f->bias));
  SmtTerm raw = smt_mk2(ctx, SMT_K_CONCAT, k(ctx, 1, 0), mf);
  SmtTerm sub_s;
  SmtTerm lz = count_leading_zeros(ctx, raw, f->ew, &sub_s);
  SmtTerm sub_e = sub(ctx, ks(ctx, f->ew, 1 - f->bias), lz);
  p.s = smt_ite(ctx, subnormal, sub_s, normal_s);
  p.e = smt_ite(ctx, subnormal, sub_e, normal_e);
  return p;
}

static SmtTerm fp_pack_special(const FpFormat *f, SmtTerm sign, int inf) {
  SmtCtx *ctx = f->ctx;
  uint64_t magnitude = inf ? (((1ull << f->eb) - 1u) << (f->sb - 1u)) : 0;
  SmtTerm body = k(ctx, f->w - 1u, magnitude);
  return smt_mk2(ctx, SMT_K_CONCAT,
                 smt_ite(ctx, sign, k(ctx, 1, 1), k(ctx, 1, 0)), body);
}

static SmtTerm fp_round(const FpFormat *f, SmtTerm sign, SmtTerm e, SmtTerm s,
                        SmtTerm sticky) {
  SmtCtx *ctx = f->ctx;
  unsigned sw = width_of(ctx, s);
  if (sw < f->sb + 2u) {
    unsigned pad = f->sb + 2u - sw;
    s = smt_mk2(ctx, SMT_K_CONCAT, s, k(ctx, pad, 0));
    sw += pad;
  }
  SmtTerm emin = ks(ctx, f->ew, 1 - f->bias);
  SmtTerm emax = ks(ctx, f->ew, f->bias);
  SmtTerm e_eff = smt_ite(ctx, slt(ctx, e, emin), emin, e);
  SmtTerm extra = sub(ctx, e_eff, e);
  unsigned xw = sw + 2u;
  if (xw < f->ew) {
    xw = f->ew;
  }
  SmtTerm x = resize_u(ctx, s, xw);
  SmtTerm limit = k(ctx, f->ew, sw + 1u);
  SmtTerm amt_e = add(ctx, extra, k(ctx, f->ew, sw - f->sb));
  amt_e = smt_ite(ctx, ult(ctx, limit, amt_e), limit, amt_e);
  SmtTerm amt = resize_u(ctx, amt_e, xw);
  SmtTerm one = k(ctx, xw, 1);
  SmtTerm m = lshr(ctx, x, amt);
  SmtTerm amt1 = sub(ctx, amt, one);
  SmtTerm guard =
      smt_eq(ctx, smt_mk2(ctx, SMT_K_BVAND, lshr(ctx, x, amt1), one), one);
  SmtTerm low_mask = sub(ctx, shl(ctx, one, amt1), one);
  SmtTerm rest = smt_or(
      ctx,
      smt_not(ctx, is_zero_bv(ctx, smt_mk2(ctx, SMT_K_BVAND, x, low_mask))),
      sticky);
  SmtTerm lsb = smt_eq(ctx, smt_mk2(ctx, SMT_K_BVAND, m, one), one);
  SmtTerm up = smt_and(ctx, guard, smt_or(ctx, rest, lsb));
  SmtTerm rounded = add(ctx, m, smt_ite(ctx, up, one, k(ctx, xw, 0)));
  unsigned pw = f->w + 2u;
  if (pw < xw) {
    pw = xw;
  }
  SmtTerm field =
      resize_u(ctx, add(ctx, e_eff, ks(ctx, f->ew, f->bias - 1)), pw);
  SmtTerm packed = add(ctx, shl(ctx, field, k(ctx, pw, f->sb - 1u)),
                       resize_u(ctx, rounded, pw));
  SmtTerm inf_pattern = k(ctx, pw, ((1ull << f->eb) - 1u) << (f->sb - 1u));
  SmtTerm overflow = smt_or(ctx, slt(ctx, emax, e_eff),
                            smt_not(ctx, ult(ctx, packed, inf_pattern)));
  packed = smt_ite(ctx, overflow, inf_pattern, packed);
  packed = smt_ite(ctx, is_zero_bv(ctx, s), k(ctx, pw, 0), packed);
  SmtTerm body = smt_extract(ctx, packed, f->w - 2u, 0);
  return smt_mk2(ctx, SMT_K_CONCAT,
                 smt_ite(ctx, sign, k(ctx, 1, 1), k(ctx, 1, 0)), body);
}

SmtTerm smt_fp_quiet_nan(SmtCtx *ctx, unsigned eb, unsigned sb, SmtTerm any) {
  uint64_t exponent = ((1ull << eb) - 1u) << (sb - 1u);
  uint64_t quiet = 1ull << (sb - 2u);
  return smt_mk2(ctx, SMT_K_BVOR, any, k(ctx, eb + sb, exponent | quiet));
}

SmtTerm smt_fp_add(SmtCtx *ctx, unsigned eb, unsigned sb, SmtTerm x, SmtTerm y,
                   SmtTerm nan) {
  FpFormat f = fp_format(ctx, eb, sb);
  FpParts a = fp_unpack(&f, x), b = fp_unpack(&f, y);
  SmtTerm a_bigger = smt_or(
      ctx, slt(ctx, b.e, a.e),
      smt_and(ctx, smt_eq(ctx, a.e, b.e), smt_not(ctx, ult(ctx, a.s, b.s))));
  SmtTerm big_s = smt_ite(ctx, a_bigger, a.s, b.s);
  SmtTerm small_s = smt_ite(ctx, a_bigger, b.s, a.s);
  SmtTerm big_e = smt_ite(ctx, a_bigger, a.e, b.e);
  SmtTerm small_e = smt_ite(ctx, a_bigger, b.e, a.e);
  SmtTerm big_sign = smt_ite(ctx, a_bigger, a.sign, b.sign);
  unsigned sw = sb + 4u;
  SmtTerm big =
      smt_mk2(ctx, SMT_K_CONCAT,
              smt_mk2(ctx, SMT_K_CONCAT, k(ctx, 1, 0), big_s), k(ctx, 3, 0));
  SmtTerm small =
      smt_mk2(ctx, SMT_K_CONCAT,
              smt_mk2(ctx, SMT_K_CONCAT, k(ctx, 1, 0), small_s), k(ctx, 3, 0));
  SmtTerm d = sub(ctx, big_e, small_e);
  SmtTerm cap = k(ctx, f.ew, sw);
  d = smt_ite(ctx, ult(ctx, cap, d), cap, d);
  SmtTerm dd = resize_u(ctx, d, sw);
  SmtTerm one = k(ctx, sw, 1);
  SmtTerm shifted = lshr(ctx, small, dd);
  SmtTerm lost_mask = sub(ctx, shl(ctx, one, dd), one);
  SmtTerm lost = smt_not(
      ctx, is_zero_bv(ctx, smt_mk2(ctx, SMT_K_BVAND, small, lost_mask)));
  shifted =
      smt_mk2(ctx, SMT_K_BVOR, shifted, smt_ite(ctx, lost, one, k(ctx, sw, 0)));
  SmtTerm same = smt_iff(ctx, a.sign, b.sign);
  SmtTerm r =
      smt_ite(ctx, same, add(ctx, big, shifted), sub(ctx, big, shifted));
  SmtTerm normalized;
  SmtTerm lz = count_leading_zeros(ctx, r, f.ew, &normalized);
  SmtTerm e = sub(ctx, add(ctx, big_e, k(ctx, f.ew, 1)), lz);
  SmtTerm general = fp_round(&f, big_sign, e, normalized, smt_false(ctx));
  general = smt_ite(ctx, is_zero_bv(ctx, r), k(ctx, f.w, 0), general);
  SmtTerm both_zero = fp_pack_special(&f, smt_and(ctx, a.sign, b.sign), 0);
  SmtTerm out = general;
  out = smt_ite(ctx, b.zero, x, out);
  out = smt_ite(ctx, a.zero, y, out);
  out = smt_ite(ctx, smt_and(ctx, a.zero, b.zero), both_zero, out);
  out = smt_ite(ctx, b.inf, y, out);
  out = smt_ite(ctx, a.inf, x, out);
  SmtTerm invalid =
      smt_or(ctx, smt_or(ctx, a.nan, b.nan),
             smt_and(ctx, smt_and(ctx, a.inf, b.inf), smt_not(ctx, same)));
  return smt_ite(ctx, invalid, nan, out);
}

SmtTerm smt_fp_neg(SmtCtx *ctx, unsigned eb, unsigned sb, SmtTerm x) {
  unsigned w = eb + sb;
  return smt_mk2(ctx, SMT_K_BVXOR, x, k(ctx, w, 1ull << (w - 1u)));
}

SmtTerm smt_fp_sub(SmtCtx *ctx, unsigned eb, unsigned sb, SmtTerm x, SmtTerm y,
                   SmtTerm nan) {
  return smt_fp_add(ctx, eb, sb, x, smt_fp_neg(ctx, eb, sb, y), nan);
}

SmtTerm smt_fp_mul(SmtCtx *ctx, unsigned eb, unsigned sb, SmtTerm x, SmtTerm y,
                   SmtTerm nan) {
  FpFormat f = fp_format(ctx, eb, sb);
  FpParts a = fp_unpack(&f, x), b = fp_unpack(&f, y);
  SmtTerm sign = smt_xor(ctx, a.sign, b.sign);
  unsigned pw = 2u * sb;
  SmtTerm product =
      smt_mk2(ctx, SMT_K_BVMUL, resize_u(ctx, a.s, pw), resize_u(ctx, b.s, pw));
  SmtTerm top =
      smt_eq(ctx, smt_extract(ctx, product, pw - 1u, pw - 1u), k(ctx, 1, 1));
  SmtTerm e = add(ctx, a.e, b.e);
  SmtTerm s = smt_ite(ctx, top, product, shl(ctx, product, k(ctx, pw, 1)));
  e = smt_ite(ctx, top, add(ctx, e, k(ctx, f.ew, 1)), e);
  SmtTerm out = fp_round(&f, sign, e, s, smt_false(ctx));
  out = smt_ite(ctx, smt_or(ctx, a.zero, b.zero), fp_pack_special(&f, sign, 0),
                out);
  out = smt_ite(ctx, smt_or(ctx, a.inf, b.inf), fp_pack_special(&f, sign, 1),
                out);
  SmtTerm invalid = smt_or(
      ctx, smt_or(ctx, a.nan, b.nan),
      smt_or(ctx, smt_and(ctx, a.inf, b.zero), smt_and(ctx, a.zero, b.inf)));
  return smt_ite(ctx, invalid, nan, out);
}

SmtTerm smt_fp_div(SmtCtx *ctx, unsigned eb, unsigned sb, SmtTerm x, SmtTerm y,
                   SmtTerm nan) {
  FpFormat f = fp_format(ctx, eb, sb);
  FpParts a = fp_unpack(&f, x), b = fp_unpack(&f, y);
  SmtTerm sign = smt_xor(ctx, a.sign, b.sign);
  unsigned qw = 2u * sb + 3u;
  SmtTerm num = shl(ctx, resize_u(ctx, a.s, qw), k(ctx, qw, sb + 2u));
  SmtTerm den = resize_u(ctx, b.s, qw);
  SmtTerm q = smt_mk2(ctx, SMT_K_BVUDIV, num, den);
  SmtTerm r = smt_mk2(ctx, SMT_K_BVUREM, num, den);
  SmtTerm qs = smt_extract(ctx, q, sb + 2u, 0);
  SmtTerm top =
      smt_eq(ctx, smt_extract(ctx, qs, sb + 2u, sb + 2u), k(ctx, 1, 1));
  SmtTerm e = sub(ctx, a.e, b.e);
  SmtTerm s = smt_ite(ctx, top, qs, shl(ctx, qs, k(ctx, sb + 3u, 1)));
  e = smt_ite(ctx, top, e, sub(ctx, e, k(ctx, f.ew, 1)));
  SmtTerm out = fp_round(&f, sign, e, s, smt_not(ctx, is_zero_bv(ctx, r)));
  out = smt_ite(ctx, smt_or(ctx, a.zero, b.inf), fp_pack_special(&f, sign, 0),
                out);
  out = smt_ite(ctx, smt_or(ctx, a.inf, b.zero), fp_pack_special(&f, sign, 1),
                out);
  SmtTerm invalid = smt_or(
      ctx, smt_or(ctx, a.nan, b.nan),
      smt_or(ctx, smt_and(ctx, a.zero, b.zero), smt_and(ctx, a.inf, b.inf)));
  return smt_ite(ctx, invalid, nan, out);
}

SmtTerm smt_fp_sqrt(SmtCtx *ctx, unsigned eb, unsigned sb, SmtTerm x,
                    SmtTerm nan) {
  FpFormat f = fp_format(ctx, eb, sb);
  FpParts a = fp_unpack(&f, x);
  SmtTerm t = sub(ctx, a.e, k(ctx, f.ew, sb - 1u));
  SmtTerm odd = smt_eq(ctx, smt_extract(ctx, t, 0, 0), k(ctx, 1, 1));
  unsigned m = (sb + 5u) / 2u;
  unsigned nw = sb + 1u + 2u * m;
  if (nw % 2u) {
    nw++;
  }
  SmtTerm wide = resize_u(ctx, a.s, nw);
  wide = smt_ite(ctx, odd, shl(ctx, wide, k(ctx, nw, 1)), wide);
  t = smt_ite(ctx, odd, sub(ctx, t, k(ctx, f.ew, 1)), t);
  SmtTerm n = shl(ctx, wide, k(ctx, nw, 2u * m));
  unsigned rw = nw / 2u + 2u;
  SmtTerm root = k(ctx, rw, 0), rem = k(ctx, rw, 0);
  for (unsigned i = nw / 2u; i-- > 0;) {
    SmtTerm pair = resize_u(ctx, smt_extract(ctx, n, 2u * i + 1u, 2u * i), rw);
    rem = smt_mk2(ctx, SMT_K_BVOR, shl(ctx, rem, k(ctx, rw, 2)), pair);
    SmtTerm trial =
        smt_mk2(ctx, SMT_K_BVOR, shl(ctx, root, k(ctx, rw, 2)), k(ctx, rw, 1));
    SmtTerm fits = smt_not(ctx, ult(ctx, rem, trial));
    rem = smt_ite(ctx, fits, sub(ctx, rem, trial), rem);
    root = smt_mk2(ctx, SMT_K_BVOR, shl(ctx, root, k(ctx, rw, 1)),
                   smt_ite(ctx, fits, k(ctx, rw, 1), k(ctx, rw, 0)));
  }
  SmtTerm normalized;
  SmtTerm lz = count_leading_zeros(ctx, root, f.ew, &normalized);
  SmtTerm half = smt_mk2(ctx, SMT_K_BVASHR, t, k(ctx, f.ew, 1));
  SmtTerm e = add(ctx, sub(ctx, k(ctx, f.ew, rw - 1u), lz),
                  sub(ctx, half, k(ctx, f.ew, m)));
  SmtTerm out = fp_round(&f, smt_false(ctx), e, normalized,
                         smt_not(ctx, is_zero_bv(ctx, rem)));
  out = smt_ite(ctx, a.zero, x, out);
  out = smt_ite(ctx, smt_and(ctx, a.inf, smt_not(ctx, a.sign)), x, out);
  SmtTerm invalid =
      smt_or(ctx, a.nan, smt_and(ctx, a.sign, smt_not(ctx, a.zero)));
  return smt_ite(ctx, invalid, nan, out);
}

SmtTerm smt_fp_is_nan(SmtCtx *ctx, unsigned eb, unsigned sb, SmtTerm x) {
  FpFormat f = fp_format(ctx, eb, sb);
  return fp_unpack(&f, x).nan;
}

SmtTerm smt_fp_eq(SmtCtx *ctx, unsigned eb, unsigned sb, SmtTerm x, SmtTerm y) {
  FpFormat f = fp_format(ctx, eb, sb);
  FpParts a = fp_unpack(&f, x), b = fp_unpack(&f, y);
  SmtTerm ordered = smt_not(ctx, smt_or(ctx, a.nan, b.nan));
  return smt_and(ctx, ordered,
                 smt_or(ctx, smt_eq(ctx, x, y), smt_and(ctx, a.zero, b.zero)));
}

SmtTerm smt_fp_lt(SmtCtx *ctx, unsigned eb, unsigned sb, SmtTerm x, SmtTerm y) {
  FpFormat f = fp_format(ctx, eb, sb);
  FpParts a = fp_unpack(&f, x), b = fp_unpack(&f, y);
  SmtTerm ordered = smt_not(ctx, smt_or(ctx, a.nan, b.nan));
  SmtTerm mixed = smt_and(ctx, a.sign, smt_not(ctx, b.sign));
  SmtTerm positive = smt_and(ctx, smt_not(ctx, a.sign), smt_not(ctx, b.sign));
  SmtTerm negative = smt_and(ctx, a.sign, b.sign);
  SmtTerm less = smt_or(ctx, mixed,
                        smt_or(ctx, smt_and(ctx, positive, ult(ctx, x, y)),
                               smt_and(ctx, negative, ult(ctx, y, x))));
  return smt_and(
      ctx, ordered,
      smt_and(ctx, smt_not(ctx, smt_and(ctx, a.zero, b.zero)), less));
}

SmtTerm smt_fp_le(SmtCtx *ctx, unsigned eb, unsigned sb, SmtTerm x, SmtTerm y) {
  return smt_or(ctx, smt_fp_lt(ctx, eb, sb, x, y),
                smt_fp_eq(ctx, eb, sb, x, y));
}

SmtTerm smt_fp_from_int(SmtCtx *ctx, unsigned eb, unsigned sb, SmtTerm v,
                        int is_signed) {
  FpFormat f = fp_format(ctx, eb, sb);
  unsigned vw = width_of(ctx, v);
  SmtTerm negative = is_signed ? slt(ctx, v, k(ctx, vw, 0)) : smt_false(ctx);
  SmtTerm magnitude = smt_ite(ctx, negative, smt_mk1(ctx, SMT_K_BVNEG, v), v);
  SmtTerm s;
  SmtTerm lz = count_leading_zeros(ctx, magnitude, f.ew, &s);
  SmtTerm e = sub(ctx, k(ctx, f.ew, vw - 1u), lz);
  SmtTerm out = fp_round(&f, negative, e, s, smt_false(ctx));
  return smt_ite(ctx, is_zero_bv(ctx, v), k(ctx, f.w, 0), out);
}

SmtTerm smt_fp_to_int(SmtCtx *ctx, unsigned eb, unsigned sb, SmtTerm x,
                      int is_signed, SmtTerm invalid_value) {
  FpFormat f = fp_format(ctx, eb, sb);
  FpParts a = fp_unpack(&f, x);
  unsigned w = 64;
  unsigned limit = is_signed ? 63u : 64u;
  SmtTerm finite = smt_not(ctx, smt_or(ctx, a.nan, a.inf));
  SmtTerm small = slt(ctx, a.e, ks(ctx, f.ew, (int64_t)limit));
  SmtTerm valid = smt_and(ctx, finite, smt_or(ctx, a.zero, small));
  if (!is_signed) {
    valid = smt_and(ctx, valid, smt_or(ctx, smt_not(ctx, a.sign), a.zero));
  }
  SmtTerm s = resize_u(ctx, a.s, w);
  SmtTerm top = ks(ctx, f.ew, (int64_t)sb - 1);
  SmtTerm left = resize_u(ctx, sub(ctx, a.e, top), w);
  SmtTerm right = resize_u(ctx, sub(ctx, top, a.e), w);
  SmtTerm cap = k(ctx, w, w);
  right = smt_ite(ctx, ult(ctx, cap, right), cap, right);
  SmtTerm magnitude =
      smt_ite(ctx, slt(ctx, a.e, top), lshr(ctx, s, right), shl(ctx, s, left));
  magnitude =
      smt_ite(ctx, slt(ctx, a.e, ks(ctx, f.ew, 0)), k(ctx, w, 0), magnitude);
  magnitude = smt_ite(ctx, a.zero, k(ctx, w, 0), magnitude);
  SmtTerm value =
      smt_ite(ctx, a.sign, smt_mk1(ctx, SMT_K_BVNEG, magnitude), magnitude);
  return smt_ite(ctx, valid, value, invalid_value);
}

SmtTerm smt_fp_convert(SmtCtx *ctx, unsigned eb, unsigned sb, unsigned to_eb,
                       unsigned to_sb, SmtTerm x, SmtTerm nan) {
  FpFormat from = fp_format(ctx, eb, sb);
  FpFormat to = fp_format(ctx, to_eb, to_sb);
  FpParts a = fp_unpack(&from, x);
  SmtTerm e = resize_s(ctx, a.e, to.ew);
  SmtTerm out = fp_round(&to, a.sign, e, a.s, smt_false(ctx));
  out = smt_ite(ctx, a.zero, fp_pack_special(&to, a.sign, 0), out);
  out = smt_ite(ctx, a.inf, fp_pack_special(&to, a.sign, 1), out);
  return smt_ite(ctx, a.nan, nan, out);
}
