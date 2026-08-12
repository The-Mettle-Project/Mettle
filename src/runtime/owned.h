#ifndef METTLE_RUNTIME_OWNED_H
#define METTLE_RUNTIME_OWNED_H

int mettle_run_process(const char *program, const char *const *arguments);
int mettle_find_executable(const char *program);
int mettle_install_signal_handler(
    int signal_number, void (*handler)(int, void *, void *));
int mettle_address_is_readable(const void *address, unsigned long long length);
int mettle_path_exists(const char *path);
int mettle_path_is_directory(const char *path);
int mettle_make_directory(const char *path);
long long mettle_executable_path(char *buffer, unsigned long long size);
char *mettle_realpath(const char *path, char *resolved);
int mettle_getcwd(char *buffer, int size);
long long mettle_readlink(const char *path, char *buffer,
                          unsigned long long size);

#endif
