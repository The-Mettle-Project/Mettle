#include <stdint.h>

typedef struct {
  int32_t a;
  int32_t b;
  int32_t c;
} ThreeI32;

typedef struct {
  uint8_t a;
  uint8_t b;
  uint8_t c;
} Odd3;

int32_t mettle_struct_abi_c_sum_three(ThreeI32 t) {
  return t.a + t.b + t.c;
}

ThreeI32 mettle_struct_abi_c_make_three(int32_t a, int32_t b, int32_t c) {
  ThreeI32 t;
  t.a = a;
  t.b = b;
  t.c = c;
  return t;
}

int32_t mettle_struct_abi_c_sum_odd3(Odd3 o) {
  return (int32_t)o.a + (int32_t)o.b + (int32_t)o.c;
}

Odd3 mettle_struct_abi_c_make_odd3(uint8_t a, uint8_t b, uint8_t c) {
  Odd3 o;
  o.a = a;
  o.b = b;
  o.c = c;
  return o;
}

/* System V cuts an aggregate into eightbytes and classifies each one, so these
 * three shapes travel differently from each other and differently again from
 * Microsoft x64, which passes all of them by pointer. Big is MEMORY (on the
 * stack by value), TwoF64 is two SSE eightbytes (two XMM registers), and Mixed
 * is one of each. */
typedef struct {
  int32_t a, b, c, d, e, f, g, h;
} Big32;

typedef struct {
  double x, y;
} TwoF64;

typedef struct {
  int64_t i;
  double d;
} MixedI64F64;

int32_t mettle_struct_abi_c_sum_big32(Big32 v) {
  return v.a + v.b + v.c + v.d + v.e + v.f + v.g + v.h;
}

int32_t mettle_struct_abi_c_sum_two_f64(TwoF64 v) {
  return (int32_t)(v.x + v.y);
}

int32_t mettle_struct_abi_c_sum_mixed(MixedI64F64 v) {
  return (int32_t)(v.i + (int64_t)v.d);
}
