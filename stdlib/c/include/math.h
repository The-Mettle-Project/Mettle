#ifndef __METTLE_MATH_H
#define __METTLE_MATH_H
#include <_mettle.h>

#define HUGE_VAL (__builtin_huge_val())
#define HUGE_VALF (__builtin_huge_valf())
#define HUGE_VALL (__builtin_huge_val())
#define INFINITY (__builtin_inff())
#define NAN (__builtin_nanf(""))
#define M_E 2.7182818284590452354
#define M_LOG2E 1.4426950408889634074
#define M_LOG10E 0.43429448190325182765
#define M_LN2 0.69314718055994530942
#define M_LN10 2.30258509299404568402
#define M_PI 3.14159265358979323846
#define M_PI_2 1.57079632679489661923
#define M_PI_4 0.78539816339744830962
#define M_1_PI 0.31830988618379067154
#define M_2_PI 0.63661977236758134308
#define M_2_SQRTPI 1.12837916709551257390
#define M_SQRT2 1.41421356237309504880
#define M_SQRT1_2 0.70710678118654752440
#define FP_NAN 0
#define FP_INFINITE 1
#define FP_ZERO 2
#define FP_SUBNORMAL 3
#define FP_NORMAL 4

double sqrt(double x) __METTLE_C(sqrt);
double sin(double x) __METTLE_MATH(sin);
double cos(double x) __METTLE_MATH(cos);
double tan(double x) __METTLE_MATH(tan);
double asin(double x) __METTLE_MATH(asin);
double acos(double x) __METTLE_MATH(acos);
double atan(double x) __METTLE_MATH(atan);
double atan2(double y, double x) __METTLE_MATH(atan2);
double sinh(double x) __METTLE_MATH(sinh);
double cosh(double x) __METTLE_MATH(cosh);
double tanh(double x) __METTLE_MATH(tanh);
double asinh(double x) __METTLE_MATH(asinh);
double acosh(double x) __METTLE_MATH(acosh);
double atanh(double x) __METTLE_MATH(atanh);
double exp(double x) __METTLE_MATH(exp);
double exp2(double x) __METTLE_MATH(exp2);
double expm1(double x) __METTLE_MATH(expm1);
double log(double x) __METTLE_MATH(log);
double log2(double x) __METTLE_MATH(log2);
double log10(double x) __METTLE_MATH(log10);
double log1p(double x) __METTLE_MATH(log1p);
double cbrt(double x) __METTLE_MATH(cbrt);
double pow(double x, double y) __METTLE_MATH(pow);
double hypot(double x, double y) __METTLE_MATH(hypot);
double fabs(double x) __METTLE_MATH(fabs);
double floor(double x) __METTLE_MATH(floor);
double ceil(double x) __METTLE_MATH(ceil);
double round(double x) __METTLE_MATH(round);
double trunc(double x) __METTLE_MATH(trunc);
double fmod(double x, double y) __METTLE_MATH(fmod);
double ldexp(double x, int n) __METTLE_MATH(ldexp);
double frexp(double x, int *exponent) __METTLE_MATH(frexp);
double copysign(double x, double y) __METTLE_MATH(copysign);
double fmin(double x, double y) __METTLE_MATH(fmin);
double fmax(double x, double y) __METTLE_MATH(fmax);
double modf(double x, double *integral) __METTLE_C(modf);
double fdim(double x, double y) __METTLE_C(fdim);
double rint(double x) __METTLE_C(rint);
double nearbyint(double x) __METTLE_C(rint);
long long lround(double x) __METTLE_C(lround);
long long llround(double x) __METTLE_C(llround);
long long lrint(double x) __METTLE_C(lrint);
double remainder(double x, double y) __METTLE_C(remainder);
double scalbn(double x, int n) __METTLE_MATH(ldexp);
int ilogb(double x) __METTLE_C(ilogb);
double logb(double x) __METTLE_C(logb);
double nextafter(double x, double y) __METTLE_C(nextafter);

float sqrtf(float x);
float sinf(float x);
float cosf(float x);
float expf(float x);
float logf(float x);
float powf(float x, float y);
float tanhf(float x);
float tanf(float x) __METTLE_MATH(f32_tan);
float fabsf(float x) __METTLE_C(fabsf);
float floorf(float x) __METTLE_C(floorf);
float ceilf(float x) __METTLE_C(ceilf);
float roundf(float x) __METTLE_C(roundf);
float truncf(float x) __METTLE_C(truncf);
float fmodf(float x, float y) __METTLE_C(fmodf);
float atan2f(float y, float x) __METTLE_C(atan2f);
float atanf(float x) __METTLE_C(atanf);
float asinf(float x) __METTLE_C(asinf);
float acosf(float x) __METTLE_C(acosf);
float log10f(float x) __METTLE_C(log10f);
float log2f(float x) __METTLE_C(log2f);
float exp2f(float x) __METTLE_C(exp2f);
float hypotf(float x, float y) __METTLE_C(hypotf);
float fminf(float x, float y) __METTLE_C(fminf);
float fmaxf(float x, float y) __METTLE_C(fmaxf);
float copysignf(float x, float y) __METTLE_C(copysignf);
float ldexpf(float x, int n) __METTLE_C(ldexpf);
float cbrtf(float x) __METTLE_C(cbrtf);

int __mettle_isnan(double x) __METTLE_C(isnan);
int __mettle_isinf(double x) __METTLE_C(isinf);
int __mettle_isfinite(double x) __METTLE_C(isfinite);
int __mettle_signbit(double x) __METTLE_C(signbit);
int __mettle_fpclassify(double x) __METTLE_C(fpclassify);
#define isnan(x) __mettle_isnan((double)(x))
#define isinf(x) __mettle_isinf((double)(x))
#define isfinite(x) __mettle_isfinite((double)(x))
#define signbit(x) __mettle_signbit((double)(x))
#define fpclassify(x) __mettle_fpclassify((double)(x))
#define isnormal(x) (fpclassify(x) == FP_NORMAL)
#define isgreater(x, y) ((x) > (y))
#define isgreaterequal(x, y) ((x) >= (y))
#define isless(x, y) ((x) < (y))
#define islessequal(x, y) ((x) <= (y))
#define islessgreater(x, y) ((x) < (y) || (x) > (y))
#define isunordered(x, y) (isnan(x) || isnan(y))
#endif
