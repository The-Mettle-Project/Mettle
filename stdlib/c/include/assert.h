#include <_mettle.h>
#undef assert
#ifdef NDEBUG
#define assert(e) ((void)0)
#else
__METTLE_NORETURN void __mettle_assert_fail(const char *expr, const char *file, int line) __METTLE_C(assert_fail);
#define assert(e) ((e) ? (void)0 : __mettle_assert_fail(#e, __FILE__, __LINE__))
#endif
#ifndef static_assert
#define static_assert _Static_assert
#endif
