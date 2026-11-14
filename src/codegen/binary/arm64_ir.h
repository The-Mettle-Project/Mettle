#ifndef CODEGEN_BINARY_ARM64_IR_H
#define CODEGEN_BINARY_ARM64_IR_H

#include "codegen/binary/arm64_emit.h"
#include "ir/ir.h"

int arm64_ir_encode_function(Arm64Emit *e, const IRFunction *fn);

int arm64_ir_function_is_intercepted(const char *name);

int arm64_ir_encode_program(Arm64Emit *e, const IRProgram *prog,
                            const char *entry, unsigned char **data_out,
                            size_t *data_len_out);

int arm64_ir_write_object(const IRProgram *prog, const char *path, char *error,
                          size_t error_capacity);

int arm64_write_elf(const char *path, const unsigned char *code, size_t len,
                    const unsigned char *data, size_t data_len);

#endif
