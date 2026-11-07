#ifndef METTLE_IR_HOST_IO_H
#define METTLE_IR_HOST_IO_H

long long ir_host_win32(const char *name, const long long *args, int *known,
                        unsigned *error);
long long ir_host_syscall(long long number, const long long *args,
                          int *known);
long long ir_host_runtime(const char *name, const long long *args, int *known);
long long ir_host_stdio(const char *name, const long long *args, int *known);

#endif
