#ifndef __METTLE_STDLIB_H
#define __METTLE_STDLIB_H
#include <_mettle.h>
#include <stddef.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 2147483647
#define MB_CUR_MAX 4

typedef struct {
  int quot;
  int rem;
} div_t;
typedef struct {
  long quot;
  long rem;
} ldiv_t;
typedef struct {
  long long quot;
  long long rem;
} lldiv_t;

void *malloc(size_t size);
void *calloc(size_t count, size_t size);
void *realloc(void *pointer, size_t size);
void free(void *pointer);
void *aligned_alloc(size_t alignment, size_t size);

int atoi(const char *text);
long atol(const char *text);
long long atoll(const char *text);
double atof(const char *text);
long strtol(const char *text, char **end, int base);
unsigned long strtoul(const char *text, char **end, int base);
long long strtoll(const char *text, char **end, int base);
unsigned long long strtoull(const char *text, char **end, int base);
double strtod(const char *text, char **end);
float strtof(const char *text, char **end);
long double strtold(const char *text, char **end);

int rand(void);
void srand(unsigned int seed);

__METTLE_NORETURN void abort(void);
__METTLE_NORETURN void exit(int status);
__METTLE_NORETURN void _Exit(int status);
int atexit(void (*function)(void));
char *getenv(const char *name);
int system(const char *command);

void qsort(void *base, size_t count, size_t size, int (*compare)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t count, size_t size,
              int (*compare)(const void *, const void *));

int abs(int n);
long labs(long n);
long long llabs(long long n);
div_t div(int numerator, int denominator);
ldiv_t ldiv(long numerator, long denominator);
lldiv_t lldiv(long long numerator, long long denominator);

int mblen(const char *s, size_t n);
char *realpath(const char *path, char *resolved);
int putenv(char *assignment);
#endif
