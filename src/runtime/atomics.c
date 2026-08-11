#include "atomics.h"

#if !defined(METTLE_ATOMICS_IN_FREESTANDING)

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#endif

int32_t mettle_atomic_compare_exchange_i32(int32_t *target, int32_t exchange,
                                         int32_t comparand) {
#if defined(_WIN32) || defined(_WIN64)
  return (int32_t)InterlockedCompareExchange((volatile LONG *)target,
                                             (LONG)exchange, (LONG)comparand);
#else
  return (int32_t)__sync_val_compare_and_swap(target, comparand, exchange);
#endif
}

int32_t mettle_atomic_exchange_i32(int32_t *target, int32_t value) {
#if defined(_WIN32) || defined(_WIN64)
  return (int32_t)InterlockedExchange((volatile LONG *)target, (LONG)value);
#else
  return (int32_t)__sync_lock_test_and_set(target, value);
#endif
}

int32_t mettle_atomic_inc_i32(int32_t *target) {
#if defined(_WIN32) || defined(_WIN64)
  return (int32_t)InterlockedIncrement((volatile LONG *)target);
#else
  return (int32_t)__sync_add_and_fetch(target, 1);
#endif
}

int32_t mettle_atomic_dec_i32(int32_t *target) {
#if defined(_WIN32) || defined(_WIN64)
  return (int32_t)InterlockedDecrement((volatile LONG *)target);
#else
  return (int32_t)__sync_sub_and_fetch(target, 1);
#endif
}

#endif
