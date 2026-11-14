#ifndef IR_DEVICE_REPORT_H
#define IR_DEVICE_REPORT_H

#include "ir.h"

int ir_device_report_storage_reason(const IRFunction *function,
                                    const char *local, int is_aggregate,
                                    char *reason, size_t size);
int ir_device_report_value_subject(const char *name, const char *what,
                                   char *subject, size_t size);
const char *ir_device_report_address_root(const IRFunction *function,
                                          size_t index,
                                          const IROperand *address);
size_t ir_device_report_code_line(const IRFunction *function,
                                  const IRInstruction *instruction);

#endif
