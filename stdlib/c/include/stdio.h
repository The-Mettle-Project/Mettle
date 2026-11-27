#ifndef __METTLE_STDIO_H
#define __METTLE_STDIO_H
#include <_mettle.h>
#include <stddef.h>
#include <stdarg.h>

typedef struct __mettle_file FILE;
typedef long long fpos_t;

FILE *__mettle_stdin(void) __METTLE_C(stdin);
FILE *__mettle_stdout(void) __METTLE_C(stdout);
FILE *__mettle_stderr(void) __METTLE_C(stderr);
#define stdin (__mettle_stdin())
#define stdout (__mettle_stdout())
#define stderr (__mettle_stderr())

#define EOF (-1)
#define BUFSIZ 4096
#define FILENAME_MAX 4096
#define FOPEN_MAX 64
#define L_tmpnam 260
#define TMP_MAX 32767
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2

int printf(const char *format, ...);
int fprintf(FILE *stream, const char *format, ...);
int sprintf(char *buffer, const char *format, ...);
int snprintf(char *buffer, size_t size, const char *format, ...);
int scanf(const char *format, ...);
int fscanf(FILE *stream, const char *format, ...);
int sscanf(const char *input, const char *format, ...);

int vprintf(const char *format, va_list args);
int vfprintf(FILE *stream, const char *format, va_list args);
int vsprintf(char *buffer, const char *format, va_list args);
int vsnprintf(char *buffer, size_t size, const char *format, va_list args);
int vscanf(const char *format, va_list args);
int vfscanf(FILE *stream, const char *format, va_list args);
int vsscanf(const char *input, const char *format, va_list args);

FILE *fopen(const char *path, const char *mode);
FILE *freopen(const char *path, const char *mode, FILE *stream);
int fclose(FILE *stream);
int fflush(FILE *stream);
size_t fread(void *buffer, size_t size, size_t count, FILE *stream);
size_t fwrite(const void *buffer, size_t size, size_t count, FILE *stream);
int fseek(FILE *stream, long offset, int origin);
long ftell(FILE *stream);
void rewind(FILE *stream);
int fgetpos(FILE *stream, fpos_t *pos);
int fsetpos(FILE *stream, const fpos_t *pos);
int feof(FILE *stream);
int ferror(FILE *stream);
void clearerr(FILE *stream);
int fgetc(FILE *stream);
int getc(FILE *stream);
int getchar(void);
int ungetc(int character, FILE *stream);
char *fgets(char *buffer, int size, FILE *stream);
int fputc(int character, FILE *stream);
int putc(int character, FILE *stream);
int putchar(int character);
int fputs(const char *text, FILE *stream);
int puts(const char *text);
void perror(const char *prefix);
int setvbuf(FILE *stream, char *buffer, int mode, size_t size);
void setbuf(FILE *stream, char *buffer);
int remove(const char *path);
int rename(const char *from, const char *to);
FILE *tmpfile(void);
int fileno(FILE *stream);
FILE *popen(const char *command, const char *mode);
int pclose(FILE *stream);
#ifdef _WIN32
#define _popen popen
#define _pclose pclose
#define _fileno fileno
#endif
#endif
