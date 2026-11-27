#ifndef __METTLE_SIGNAL_H
#define __METTLE_SIGNAL_H
typedef int sig_atomic_t;
typedef void (*__mettle_sighandler)(int);
#define SIG_DFL ((__mettle_sighandler)0)
#define SIG_IGN ((__mettle_sighandler)1)
#define SIG_ERR ((__mettle_sighandler)-1)
#define SIGINT 2
#define SIGILL 4
#define SIGABRT 22
#define SIGFPE 8
#define SIGSEGV 11
#define SIGTERM 15
__mettle_sighandler signal(int sig, __mettle_sighandler handler);
int raise(int sig);
#endif
