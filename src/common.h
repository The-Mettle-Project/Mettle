#ifndef METTLE_COMMON_H
#define METTLE_COMMON_H

#include <stddef.h>
#include <stdarg.h>

int mettle_trust_mode_active(const char **name_out);
void mettle_trust_mode_announce(void);

#if defined(_WIN32) && !defined(__MINGW32__)
#include <string.h>
#ifndef strcasecmp
#define strcasecmp _stricmp
#endif
#ifndef strncasecmp
#define strncasecmp _strnicmp
#endif
#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef ptrdiff_t ssize_t;
#endif
#endif

#if defined(_MSC_VER)
#define MTLC_THREAD_LOCAL __declspec(thread)
#elif defined(_WIN32) && defined(__GNUC__) && !defined(__clang__) && \
    __GNUC__ >= 16
#define MTLC_THREAD_LOCAL
#else
#define MTLC_THREAD_LOCAL __thread
#endif

#define METTLE_FNV1A_OFFSET_BASIS ((size_t)1469598103934665603ULL)
#define METTLE_FNV1A_PRIME        ((size_t)1099511628211ULL)

typedef struct {
  int read;
  const char *value;
} MettleEnvOnce;

char *mettle_strdup(const char *text);
size_t mettle_fnv1a_hash(const char *str);
const char *mettle_env_once(MettleEnvOnce *once, const char *name);
void mettle_set_error(char **dest, const char *fmt, ...);
void mettle_free_string(char *str);
void mettle_free_string_array(char **values, size_t count);

double mettle_now_ms(void);

#include <stdint.h>
uint16_t mettle_f32bits_to_f16bits(uint32_t u);
uint32_t mettle_f16bits_to_f32bits(uint16_t h);
uint16_t mettle_f32bits_to_bf16bits(uint32_t u);
uint32_t mettle_bf16bits_to_f32bits(uint16_t h);
float mettle_f16bits_to_f32(uint16_t h);
uint16_t mettle_f32_to_f16bits(float f);
float mettle_bf16bits_to_f32(uint16_t h);
float mettle_fmaf_exact(float a, float b, float c);
uint16_t mettle_f32_to_bf16bits(float f);
uint16_t mettle_f64bits_to_f16bits(uint64_t u);
uint16_t mettle_f64bits_to_bf16bits(uint64_t u);
int mettle_f64_is_exact_f16(double d);
int mettle_f64_is_exact_bf16(double d);
int mettle_f32_is_exact_f16(float f);
int mettle_f32_is_exact_bf16(float f);

#endif
