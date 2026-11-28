#ifndef MTLC_HOST_REDIRECT_H
#define MTLC_HOST_REDIRECT_H

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <io.h>
#endif

void *mtlc_host_memset(void *, int, size_t);
void *mtlc_host_memcpy(void *, const void *, size_t);
void *mtlc_host_memmove(void *, const void *, size_t);
void *mtlc_host_memchr(const void *, int, size_t);
int mtlc_host_memcmp(const void *, const void *, size_t);
size_t mtlc_host_strlen(const char *);
int mtlc_host_strcmp(const char *, const char *);
int mtlc_host_strncmp(const char *, const char *, size_t);
char *mtlc_host_strchr(const char *, int);
char *mtlc_host_strrchr(const char *, int);
char *mtlc_host_strncpy(char *, const char *, size_t);
char *mtlc_host_strcpy(char *, const char *);
char *mtlc_host_strcat(char *, const char *);
char *mtlc_host_strstr(const char *, const char *);
char *mtlc_host_strpbrk(const char *, const char *);
int mtlc_host_strcasecmp(const char *, const char *);
char *mtlc_host_strtok(char *, const char *);
char *mtlc_host_strdup(const char *);
char *mtlc_host_strerror(int);

void mtlc_host_alloc_report(void);
void *mtlc_host_malloc(size_t);
void *mtlc_host_calloc(size_t, size_t);
void *mtlc_host_realloc(void *, size_t);
void mtlc_host_free(void *);

extern FILE *mtlc_host_stdin;
extern FILE *mtlc_host_stdout;
extern FILE *mtlc_host_stderr;
FILE *mtlc_host_fopen(const char *, const char *);
int mtlc_host_fclose(FILE *);
size_t mtlc_host_fread(void *, size_t, size_t, FILE *);
size_t mtlc_host_fwrite(const void *, size_t, size_t, FILE *);
int mtlc_host_fputs(const char *, FILE *);
int mtlc_host_puts(const char *);
int mtlc_host_fputc(int, FILE *);
int mtlc_host_putchar(int);
int mtlc_host_getchar(void);
char *mtlc_host_fgets(char *, int, FILE *);
int mtlc_host_fflush(FILE *);
int mtlc_host_ferror(FILE *);
int mtlc_host_fseek(FILE *, long, int);
long mtlc_host_ftell(FILE *);
void mtlc_host_rewind(FILE *);
int mtlc_host_setvbuf(FILE *, char *, int, size_t);
int mtlc_host_fileno(FILE *);
int mtlc_host_isatty(int);
int mtlc_host_remove(const char *);
int mtlc_host_access(const char *, int);
#if defined(_WIN32)
char *mtlc_host_getcwd(char *, int);
#else
char *mtlc_host_getcwd(char *, size_t);
#endif
char *mtlc_host_realpath(const char *, char *);
int mtlc_host_unlink(const char *);
int mtlc_host_rename(const char *, const char *);
int mtlc_host_putenv(char *);
int mtlc_host_system(const char *);
FILE *mtlc_host_popen(const char *, const char *);
int mtlc_host_pclose(FILE *);

int mtlc_host_printf(const char *, ...);
int mtlc_host_fprintf(FILE *, const char *, ...);
int mtlc_host_vfprintf(FILE *, const char *, va_list);
int mtlc_host_sprintf(char *, const char *, ...);
int mtlc_host_snprintf(char *, size_t, const char *, ...);
int mtlc_host_vsnprintf(char *, size_t, const char *, va_list);
int mtlc_host_sscanf(const char *, const char *, ...);
int mtlc_host_vsscanf(const char *, const char *, va_list);

int mtlc_host_atoi(const char *);
long mtlc_host_atol(const char *);
long long mtlc_host_atoll(const char *);
double mtlc_host_atof(const char *);
double mtlc_host_strtod(const char *, char **);
unsigned long mtlc_host_strtoul(const char *, char **, int);
unsigned long long mtlc_host_strtoull(const char *, char **, int);
long long mtlc_host_strtoll(const char *, char **, int);
void mtlc_host_qsort(void *, size_t, size_t,
                     int (*)(const void *, const void *));
void *mtlc_host_bsearch(const void *, const void *, size_t, size_t,
                        int (*)(const void *, const void *));
char *mtlc_host_getenv(const char *);
clock_t mtlc_host_clock(void);
int mtlc_host_clock_gettime(int, void *);
void mtlc_host_abort(void);

int mtlc_host_isspace(int);
int mtlc_host_isalpha(int);
int mtlc_host_isalnum(int);
int mtlc_host_isdigit(int);
int mtlc_host_isxdigit(int);
int mtlc_host_isupper(int);
int mtlc_host_islower(int);
int mtlc_host_isprint(int);
int mtlc_host_isgraph(int);
int mtlc_host_ispunct(int);
int mtlc_host_iscntrl(int);
int mtlc_host_tolower(int);
int mtlc_host_toupper(int);
float mtlc_host_sqrtf(float);
double mtlc_host_sqrt(double);
double mtlc_host_fabs(double);
double mtlc_host_exp(double);
float mtlc_host_expf(float);
double mtlc_host_tanh(double);
float mtlc_host_tanhf(float);
int *mtlc_host_errno_location(void);

#include <locale.h>
#include <wchar.h>

__attribute__((noreturn)) void mtlc_host_exit(int);
int mtlc_host_feof(FILE *);
void mtlc_host_clearerr(FILE *);
int mtlc_host_fgetc(FILE *);
int mtlc_host_getc(FILE *);
int mtlc_host_ungetc(int, FILE *);
int mtlc_host_putc(int, FILE *);
void mtlc_host_setbuf(FILE *, char *);
int mtlc_host_fgetpos(FILE *, fpos_t *);
int mtlc_host_fsetpos(FILE *, const fpos_t *);
void mtlc_host_perror(const char *);
long mtlc_host_strtol(const char *, char **, int);
float mtlc_host_strtof(const char *, char **);
long double mtlc_host_strtold(const char *, char **);
intmax_t mtlc_host_strtoimax(const char *, char **, int);
uintmax_t mtlc_host_strtoumax(const char *, char **, int);
int mtlc_host_abs(int);
long mtlc_host_labs(long);
long long mtlc_host_llabs(long long);
intmax_t mtlc_host_imaxabs(intmax_t);
div_t mtlc_host_div(int, int);
ldiv_t mtlc_host_ldiv(long, long);
lldiv_t mtlc_host_lldiv(long long, long long);
int mtlc_host_atexit(void (*)(void));
void mtlc_host_run_atexit(void);
void *mtlc_host_aligned_alloc(size_t, size_t);
int mtlc_host_mblen(const char *, size_t);
char *mtlc_host_strncat(char *, const char *, size_t);
int mtlc_host_strcoll(const char *, const char *);
size_t mtlc_host_strxfrm(char *, const char *, size_t);
size_t mtlc_host_strspn(const char *, const char *);
size_t mtlc_host_strcspn(const char *, const char *);
size_t mtlc_host_strnlen(const char *, size_t);
char *mtlc_host_strndup(const char *, size_t);
int mtlc_host_strncasecmp(const char *, const char *, size_t);
int mtlc_host_isblank(int);
int mtlc_host_vprintf(const char *, va_list);
int mtlc_host_vsprintf(char *, const char *, va_list);
int mtlc_host_vfscanf(FILE *, const char *, va_list);
int mtlc_host_vscanf(const char *, va_list);
int mtlc_host_fscanf(FILE *, const char *, ...);
int mtlc_host_scanf(const char *, ...);
time_t mtlc_host_time(time_t *);
double mtlc_host_difftime(time_t, time_t);
struct tm *mtlc_host_gmtime(const time_t *);
struct tm *mtlc_host_localtime(const time_t *);
time_t mtlc_host_mktime(struct tm *);
char *mtlc_host_asctime(const struct tm *);
char *mtlc_host_ctime(const time_t *);
size_t mtlc_host_strftime(char *, size_t, const char *, const struct tm *);
void (*mtlc_host_signal(int, void (*)(int)))(int);
int mtlc_host_raise(int);
char *mtlc_host_setlocale(int, const char *);
struct lconv *mtlc_host_localeconv(void);
size_t mtlc_host_wcslen(const wchar_t *);
wchar_t *mtlc_host_wcscpy(wchar_t *, const wchar_t *);
int mtlc_host_wcscmp(const wchar_t *, const wchar_t *);
int mtlc_host_wcsncmp(const wchar_t *, const wchar_t *, size_t);
wchar_t *mtlc_host_wcsncpy(wchar_t *, const wchar_t *, size_t);
wchar_t *mtlc_host_wcscat(wchar_t *, const wchar_t *);
wchar_t *mtlc_host_wcsncat(wchar_t *, const wchar_t *, size_t);
wchar_t *mtlc_host_wcschr(const wchar_t *, wchar_t);
wchar_t *mtlc_host_wcsrchr(const wchar_t *, wchar_t);
wchar_t *mtlc_host_wcsstr(const wchar_t *, const wchar_t *);
wchar_t *mtlc_host_wmemcpy(wchar_t *, const wchar_t *, size_t);
wchar_t *mtlc_host_wmemmove(wchar_t *, const wchar_t *, size_t);
wchar_t *mtlc_host_wmemset(wchar_t *, wchar_t, size_t);
int mtlc_host_wmemcmp(const wchar_t *, const wchar_t *, size_t);
wchar_t *mtlc_host_wmemchr(const wchar_t *, wchar_t, size_t);

#undef feof
#undef clearerr
#undef fgetc
#undef getc
#undef ungetc
#undef putc
#undef setbuf
#undef perror
#undef strtol
#undef strtof
#undef abs
#undef labs
#undef isblank
#undef vprintf
#undef vsprintf
#undef vfscanf
#undef vscanf
#undef fscanf
#undef scanf
#undef time
#undef difftime
#undef gmtime
#undef localtime
#undef mktime
#undef asctime
#undef ctime
#undef strftime
#undef signal
#undef setlocale
#undef wcslen
#undef wcscpy
#undef wcscmp
#undef wcsncmp
#undef wcsncpy
#undef wcscat
#undef wcsncat
#undef wcschr
#undef wcsrchr
#undef wcsstr
#undef wmemcpy
#undef wmemmove
#undef wmemset
#undef wmemcmp
#undef wmemchr

#undef stdin
#undef stdout
#undef stderr
#undef printf
#undef fprintf
#undef sprintf
#undef snprintf
#undef vsnprintf
#undef sscanf
#undef vsscanf
#undef popen
#undef pclose
#undef strtod
#undef fileno
#undef isatty
#undef errno
#undef isspace
#undef isalpha
#undef isalnum
#undef isdigit
#undef isxdigit
#undef isupper
#undef islower
#undef isprint
#undef isgraph
#undef ispunct
#undef iscntrl
#undef tolower
#undef toupper

#include "host_prefix.h"
#define errno (*mtlc_host_errno_location())

#endif
