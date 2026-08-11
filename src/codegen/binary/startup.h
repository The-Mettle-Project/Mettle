#ifndef BINARY_STARTUP_H
#define BINARY_STARTUP_H

#include "codegen/binary_emitter.h"

/* Emit the freestanding program entry object for an executable build. */
int binary_write_program_startup_object(const char *path, int profile_runtime,
                                        int stack_trace_init,
                                        int main_wants_argc_argv);

/* Target specific form used by cross target tests and packaging tools. */
int binary_write_program_startup_object_for_target(
    const char *path, BinaryTargetFormat target, int profile_runtime,
    int stack_trace_init, int main_wants_argc_argv);

#endif /* BINARY_STARTUP_H */
