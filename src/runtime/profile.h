#ifndef METTLE_PROFILE_H
#define METTLE_PROFILE_H

#include <stdint.h>

/* Opt-in runtime function profiler. The compiler emits calls to these symbols
 * when --profile-runtime is active. Programs compiled without that flag do not
 * link this object. */

void mettle_profile_enter(uint32_t fn_id);
void mettle_profile_exit(void);
void mettle_profile_op(uint32_t op_class, uint64_t amount);
/* Basic-block execution counter. The compiler emits one call to this at every
 * basic-block leader when --profile-blocks is active; block_id is the literal
 * the codegen annotator reads to fuse measured counts with the static cost
 * model. Counts grow dynamically, indexed by block_id. */
void mettle_profile_block(uint32_t block_id);
void mettle_profile_report(void);

typedef enum {
  METTLE_PROFILE_OP_LOAD = 0,
  METTLE_PROFILE_OP_STORE,
  METTLE_PROFILE_OP_BRANCH,
  METTLE_PROFILE_OP_CALL,
  METTLE_PROFILE_OP_ADD,
  METTLE_PROFILE_OP_MUL,
  METTLE_PROFILE_OP_DIV,
  METTLE_PROFILE_OP_MOD,
  METTLE_PROFILE_OP_SHIFT,
  METTLE_PROFILE_OP_BITWISE,
  METTLE_PROFILE_OP_MEM_PRIMITIVE,
  METTLE_PROFILE_OP_SIMD,
  METTLE_PROFILE_OP_POPCNT,
  METTLE_PROFILE_OP_CLASS_COUNT
} MettleProfileOpClass;

#endif /* METTLE_PROFILE_H */
