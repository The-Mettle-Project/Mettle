#ifndef MTLC_RUNTIME_MT_MATH_H
#define MTLC_RUNTIME_MT_MATH_H

/* The freestanding math kernels, shared by the runtime that a program links
 * against and by the compiler's own interpreter.
 *
 * They live in a header because both need the SAME arithmetic. A program on a
 * freestanding target calls these for expf/logf/powf/sinf/cosf/tanhf, so an
 * interpreter with its own approximation -- or with the host's libm -- would
 * answer a slightly different number than the program it is describing. There
 * is nothing here but arithmetic: no libc, no external symbols, which is what
 * lets the compiler's archive stay self-contained.
 */

static double mt_exp(double value) {
  if (value > 709.0) {
    return 1.0 / 0.0;
  }
  if (value < -745.0) {
    return 0.0;
  }
  int power = 0;
  const double ln2 = 0.69314718055994530942;
  while (value > ln2 * 0.5) {
    value -= ln2;
    power++;
  }
  while (value < -ln2 * 0.5) {
    value += ln2;
    power--;
  }
  double term = 1.0;
  double result = 1.0;
  for (int i = 1; i <= 18; i++) {
    term *= value / (double)i;
    result += term;
  }
  while (power > 0) {
    result *= 2.0;
    power--;
  }
  while (power < 0) {
    result *= 0.5;
    power++;
  }
  return result;
}

/* Natural log. Split the value into m * 2^e with m in [1, 2), then run the
 * atanh series on s = (m - 1) / (m + 1): over that range s stays under 1/3, so
 * s^2 is under 1/9 and the terms below land well inside double precision. The
 * scaling loops mirror mt_exp's rather than reaching for frexp, which this
 * runtime does not have. */
static double mt_log(double value) {
  if (value < 0.0) {
    return 0.0 / 0.0;
  }
  if (value == 0.0) {
    return -1.0 / 0.0;
  }
  if (value != value || value == 1.0 / 0.0) {
    return value;
  }
  int power = 0;
  while (value >= 2.0) {
    value *= 0.5;
    power++;
  }
  while (value < 1.0) {
    value *= 2.0;
    power--;
  }
  double s = (value - 1.0) / (value + 1.0);
  double s2 = s * s;
  double term = s;
  double sum = s;
  for (int i = 3; i <= 25; i += 2) {
    term *= s2;
    sum += term / (double)i;
  }
  return 2.0 * sum + (double)power * 0.69314718055994530942;
}

static double mt_pow(double base, double exponent) {
  if (exponent == 0.0) {
    return 1.0;
  }
  if (base != base || exponent != exponent) {
    return 0.0 / 0.0;
  }
  if (base == 0.0) {
    return exponent > 0.0 ? 0.0 : 1.0 / 0.0;
  }
  if (base < 0.0) {
    /* Defined only for an integer exponent; the sign comes from its parity. */
    double truncated = (double)(long long)exponent;
    if (truncated != exponent) {
      return 0.0 / 0.0;
    }
    double magnitude = mt_exp(exponent * mt_log(-base));
    return ((long long)exponent & 1) ? -magnitude : magnitude;
  }
  return mt_exp(exponent * mt_log(base));
}

/* Sine, reduced to [-pi/2, pi/2] before the Taylor series so twelve terms are
 * comfortably inside double precision. The reduction is the plain subtract-a-
 * multiple-of-2pi kind, which loses low bits once the argument is large enough
 * that 2pi is no longer representable near it; past 2^52 nothing meaningful is
 * left to reduce, so that range answers NaN rather than a confident wrong
 * number. */
static double mt_sin(double value) {
  const double two_pi = 6.2831853071795864769;
  const double pi = 3.1415926535897932385;
  const double half_pi = 1.5707963267948966192;

  if (value != value) {
    return value;
  }
  if (value > 4503599627370496.0 || value < -4503599627370496.0) {
    return 0.0 / 0.0;
  }
  value -= (double)(long long)(value / two_pi) * two_pi;
  if (value > pi) {
    value -= two_pi;
  } else if (value < -pi) {
    value += two_pi;
  }
  if (value > half_pi) {
    value = pi - value;
  } else if (value < -half_pi) {
    value = -pi - value;
  }
  double squared = value * value;
  double term = value;
  double sum = value;
  for (int i = 1; i <= 12; i++) {
    term *= -squared / ((2.0 * (double)i) * (2.0 * (double)i + 1.0));
    sum += term;
  }
  return sum;
}

static double mt_cos(double value) {
  return mt_sin(value + 1.5707963267948966192);
}

static double mt_tanh(double value) {
  if (value > 20.0) {
    return 1.0;
  }
  if (value < -20.0) {
    return -1.0;
  }
  double twice = mt_exp(value * 2.0);
  return (twice - 1.0) / (twice + 1.0);
}

/* Newton iteration in single precision, which is what the program's sqrtf
 * does -- doing it in double and rounding once would land on a different
 * float for some inputs. */
static float mt_sqrtf(float value) {
  if (value <= 0.0f) {
    return value == 0.0f ? value : 0.0f / 0.0f;
  }
  float estimate = value >= 1.0f ? value : 1.0f;
  for (int i = 0; i < 16; i++) {
    estimate = 0.5f * (estimate + value / estimate);
  }
  return estimate;
}

#endif /* MTLC_RUNTIME_MT_MATH_H */
