#ifndef METTLE_IR_DEBUG_HOOKS_H
#define METTLE_IR_DEBUG_HOOKS_H

#include "ir.h"

/* --debug-hooks instrumentation for the source-level debugger: inserts
 * mettle_dbg_enter/exit/line hooks and per-variable live-pointer
 * registrations (mettle_dbg_local) into every program function, and fills
 * the profile registry so the binary embeds function name/file/line tables.
 * Run BEFORE optimization; intended for -O0 builds. */
int ir_debug_hooks_instrument_program(IRProgram *program);

#endif /* METTLE_IR_DEBUG_HOOKS_H */
