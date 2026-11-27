#ifndef __METTLE_SETJMP_H
#define __METTLE_SETJMP_H
typedef long long jmp_buf[32];
int mettle_setjmp(jmp_buf env);
__attribute__((__noreturn__)) void mettle_longjmp(jmp_buf env, int value);
#define setjmp(env) mettle_setjmp(env)
#define _setjmp(env) mettle_setjmp(env)
#define longjmp(env, value) mettle_longjmp(env, value)
#define _longjmp(env, value) mettle_longjmp(env, value)
#endif
