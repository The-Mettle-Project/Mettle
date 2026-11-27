#ifndef __METTLE_C_COMMON_H
#define __METTLE_C_COMMON_H
#define __METTLE_C(name) __attribute__((__mettle__(#name, "std/c")))
#define __METTLE_MATH(name) __attribute__((__mettle__(#name, "std/math")))
#define __METTLE_NORETURN __attribute__((__noreturn__))
#endif
