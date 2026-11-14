#ifndef IR_EXPLAIN_DEVICE_H
#define IR_EXPLAIN_DEVICE_H

#include <stddef.h>

typedef enum {
  IR_DEVICE_GOOD = 0,
  IR_DEVICE_MISS,
  IR_DEVICE_FACT,
  IR_DEVICE_REFUSED
} IRDeviceVerdict;

typedef enum {
  IR_DEVICE_VALUES_IN_REGISTERS = 0,
  IR_DEVICE_VALUES_IN_MEMORY,
  IR_DEVICE_MEMORY_BYTES,
  IR_DEVICE_WORKGROUP_BYTES,
  IR_DEVICE_WORKGROUP_LAUNCH_SIZED,
  IR_DEVICE_ACCESSES_NAMED,
  IR_DEVICE_ACCESSES_GENERIC,
  IR_DEVICE_VECTOR_GROUPS,
  IR_DEVICE_LOADS_SAVED,
  IR_DEVICE_CALLS_KEPT,
  IR_DEVICE_MEASURED_REGISTERS,
  IR_DEVICE_MEASURED_SPILL_STORES,
  IR_DEVICE_MEASURED_SPILL_LOADS,
  IR_DEVICE_MEASURED_STACK_FRAME,
  IR_DEVICE_MEASURED_SHARED_BYTES,
  IR_DEVICE_MEASURED_WARPS,
  IR_DEVICE_MEASURED_WARP_LIMIT,
  IR_DEVICE_MEASURED_THREAD_WIDTH,
  IR_DEVICE_MEASURED_MAX_THREADS,
  IR_DEVICE_COUNTER_COUNT
} IRDeviceCounter;

int ir_explain_device_enabled(void);
void ir_explain_device_begin(const char *target, const char *profile);
int ir_explain_device_function(const char *function, const char *filename,
                               size_t line, int is_kernel, const int *block);
void ir_explain_device_note(const char *function, size_t line,
                            const char *kind, const char *subject,
                            IRDeviceVerdict verdict, const char *headline,
                            const char *reason, const char *fix,
                            const char *code);
void ir_explain_device_note_site(const char *function, size_t line,
                                 const char *kind, const char *subject,
                                 IRDeviceVerdict verdict, const char *headline,
                                 const char *reason, const char *fix,
                                 const char *code);
void ir_explain_device_note_quantity(const char *name, long long value);
void ir_explain_device_add(const char *function, IRDeviceCounter counter,
                           long long amount);
void ir_explain_device_set(const char *function, IRDeviceCounter counter,
                           long long value);
void ir_explain_device_limiter(const char *function, const char *limiter);
void ir_explain_device_code(const char *function, size_t line,
                            const char *text);
void ir_explain_device_measured_by(const char *tool);
size_t ir_explain_device_function_count(void);
const char *ir_explain_device_function_name(size_t index, int *is_kernel);

#endif
