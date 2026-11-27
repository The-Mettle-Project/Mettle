#ifndef __METTLE_STDDEF_H
#define __METTLE_STDDEF_H
#ifdef _WIN32
typedef unsigned long long size_t;
typedef long long ptrdiff_t;
typedef unsigned short wchar_t;
#else
typedef unsigned long size_t;
typedef long ptrdiff_t;
typedef int wchar_t;
#endif
typedef double max_align_t;
#define NULL ((void *)0)
#define offsetof(type, member) __builtin_offsetof(type, member)
#endif
