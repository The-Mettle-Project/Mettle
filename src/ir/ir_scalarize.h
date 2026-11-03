#ifndef IR_SCALARIZE_H
#define IR_SCALARIZE_H

#include "ir.h"

int ir_scalarize_opcode_is_kernel(IROpcode op);
IRFunction *ir_scalarize_function(IRFunction *function, char *why,
                                  size_t why_size);

#endif
