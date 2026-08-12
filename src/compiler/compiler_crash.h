#ifndef COMPILER_CRASH_H
#define COMPILER_CRASH_H

void mettle_compiler_crash_install(int argc, char **argv);

void mettle_compiler_ice_report(const char *reason, const char *detail);

void mettle_compiler_ice(const char *reason);

#endif /* COMPILER_CRASH_H */
