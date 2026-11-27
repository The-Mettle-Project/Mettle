#ifndef __METTLE_TIME_H
#define __METTLE_TIME_H
#include <stddef.h>
typedef long long time_t;
typedef long long clock_t;
#define CLOCKS_PER_SEC 1000000LL
struct tm {
  int tm_sec;
  int tm_min;
  int tm_hour;
  int tm_mday;
  int tm_mon;
  int tm_year;
  int tm_wday;
  int tm_yday;
  int tm_isdst;
};
struct timespec {
  time_t tv_sec;
  long tv_nsec;
};
clock_t clock(void);
time_t time(time_t *out);
double difftime(time_t end, time_t start);
time_t mktime(struct tm *tm);
struct tm *gmtime(const time_t *t);
struct tm *localtime(const time_t *t);
char *asctime(const struct tm *tm);
char *ctime(const time_t *t);
size_t strftime(char *buffer, size_t size, const char *format, const struct tm *tm);
#endif
