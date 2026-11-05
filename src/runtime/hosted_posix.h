#ifndef METTLE_HOSTED_POSIX_H
#define METTLE_HOSTED_POSIX_H

#define MT_HOSTED_O_RDONLY 0
#define MT_HOSTED_O_WRONLY 1
#define MT_HOSTED_O_RDWR 2
#define MT_HOSTED_O_ACCMODE 3
#define MT_HOSTED_O_CREAT 0100
#define MT_HOSTED_O_TRUNC 01000
#define MT_HOSTED_O_APPEND 02000
#define MT_HOSTED_O_DIRECTORY 00200000

#define MT_HOSTED_WAIT_OBJECT_0 0u
#define MT_HOSTED_WAIT_TIMEOUT 258u
#define MT_HOSTED_WAIT_FAILED 0xffffffffu
#define MT_HOSTED_INFINITE 0xffffffffu

void mt_hosted_startup(int argc, char **argv);
int *mt_hosted_errno_location(void);
char **mt_hosted_environ(void);
void *mt_hosted_thread_stack_high(void);
char *mt_hosted_getcwd(char *buffer, unsigned long long size);

int mt_hosted_open(const char *path, int mt_flags, int mode);
int mt_hosted_is_directory(const char *path);
void *mt_hosted_map(unsigned long long bytes);
int mt_hosted_unmap(void *address, unsigned long long bytes);
int mt_hosted_purge(void *address, unsigned long long bytes);
unsigned long long mt_hosted_page_size(void);
long long mt_hosted_executable_path(char *buffer, unsigned long long size);
int mt_hosted_monotonic(long long *timespec_value);

void *mt_hosted_dir_open(const char *path);
const char *mt_hosted_dir_next(void *directory, int *entry_type);
void mt_hosted_dir_close(void *directory);

int mt_hosted_install_signal_handler(int signal_number,
                                     void (*handler)(int, void *, void *));
int mt_hosted_address_is_readable(const void *address,
                                  unsigned long long length);

int mt_hosted_run_process(const char *program, const char *const *arguments,
                          char *const *environment, const char *search_path);
long long mt_hosted_spawn_shell_reader(const char *command,
                                       char *const *environment,
                                       int *read_descriptor);
int mt_hosted_wait_process(long long pid, int *status);
long long mt_hosted_spawn_process(const char *program,
                                  const char *const *arguments);
int mt_hosted_poll_process(long long pid, int *exit_code);
void mt_hosted_sleep_ms(unsigned milliseconds);
int mt_hosted_file_mtime(const char *path, long long *seconds,
                         long long *nanoseconds);

void mt_hosted_yield(void);
unsigned mt_hosted_hardware_threads(void);
int mt_hosted_spawn_detached(void *(*start)(void *), void *argument,
                             unsigned long long stack_size);
int mt_hosted_wait_on(volatile int *address, int expected,
                      unsigned milliseconds);
void mt_hosted_wake(volatile int *address);

long long mt_hosted_thread_create(unsigned long long stack_size,
                                  unsigned (*start)(void *), void *argument,
                                  unsigned *thread_id);
unsigned mt_hosted_thread_wait(long long handle, unsigned milliseconds);
int mt_hosted_thread_close(long long handle);
unsigned mt_hosted_thread_current_id(void);
void mt_hosted_thread_sleep_ms(unsigned milliseconds);
long long mt_hosted_mutex_create(int initial_owner);
unsigned mt_hosted_mutex_wait(long long handle, unsigned milliseconds);
int mt_hosted_mutex_release(long long handle);
int mt_hosted_mutex_close(long long handle);

#endif
