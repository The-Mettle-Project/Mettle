#include "optimizer/ir_optimize_internal.h"

int ir_optimize_program(IRProgram *program,
                        const IROptimizeOptions *options) {
  return ir_optimize_program_pipeline(program, options);
}
