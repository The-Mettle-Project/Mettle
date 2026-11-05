#ifndef METTLE_RUNTIME_SAFETY_H
#define METTLE_RUNTIME_SAFETY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define METTLE_SAFETY_GRANULE 16u

typedef enum {
  METTLE_SAFETY_ACCESS_READ = 0,
  METTLE_SAFETY_ACCESS_WRITE = 1
} MettleSafetyAccessKind;

void mettle_safety_register(void *pointer, uint64_t size);
void mettle_safety_register_static(void *pointer, uint64_t size);

void mettle_safety_check_affine(const void *base, int64_t offset, int64_t length,
    uint32_t kind, uint32_t line, uint64_t identity, int64_t width, int64_t step);

int64_t mettle_safety_loop_length(int64_t bound, int64_t first_bound,
                                 int64_t counter_step, int64_t byte_step,
                                 int64_t access_size);

void mettle_safety_unregister(void *pointer);

void mettle_safety_enter_allocator(void);
void mettle_safety_leave_allocator(void);

void mettle_safety_reregister(void *old_pointer, void *new_pointer,
                              uint64_t size);

void mettle_safety_check(const void *base, int64_t offset, int64_t size,
                         uint32_t access_kind, uint32_t line);

int64_t mettle_safety_span(const void *base);

uint64_t mettle_safety_identity(const void *pointer);
void mettle_safety_check_identity(const void *base, int64_t offset, int64_t size,
                                  uint32_t kind, uint32_t line, uint64_t identity);
int64_t mettle_safety_span_identity(const void *base, uint64_t identity);
uint64_t mettle_safety_merge_identity(uint64_t a, uint64_t b);
uint64_t mettle_safety_subtract_identity(uint64_t a, uint64_t b);
uint64_t mettle_safety_value_load(const void *slot, uint64_t value, uint64_t size);
void mettle_safety_value_store(void *slot, uint64_t value, uint64_t identity,
                               uint64_t size);
void mettle_safety_value_copy(void *destination, const void *source, uint64_t size);
void mettle_safety_value_clear(void *destination, uint64_t size);
void mettle_safety_free_identity(void *pointer, uint64_t identity);
void mettle_safety_entry_arguments(void *vector);
void mettle_safety_region_begin(void *pointer, int64_t size);
void mettle_safety_region_end(void *pointer);
uint64_t mettle_safety_literal_identity(const void *pointer, uint64_t size);
uint64_t mettle_safety_string_identity(const void *record, uint64_t size);
void mettle_safety_string_contents(void *record, uint64_t chars);
void mettle_safety_string_owned(void *record);
void mettle_safety_string_release(void *record);
void mettle_safety_buffer_check(void *pointer, int64_t size, uint32_t kind,
                                uint32_t line, uint64_t identity);
void *mettle_safety_call_push(void *callee, uint64_t count);
void *mettle_safety_call_enter(void *callee);
void mettle_safety_call_arg(void *call, uint64_t index, uint64_t identity);
uint64_t mettle_safety_call_param(void *call, uint64_t index);
void mettle_safety_call_return(void *call, uint64_t identity);
uint64_t mettle_safety_call_pop(void *call);
void mettle_safety_call_arg_copy(void *call, uint64_t index, const void *source, uint64_t size);
void mettle_safety_call_param_copy(void *call, uint64_t index, void *destination, uint64_t size);
void mettle_safety_call_return_copy(void *call, const void *source, uint64_t size);
void mettle_safety_call_result_copy(void *call, void *destination, uint64_t size);
void mettle_safety_global_pointer(void *slot, uint64_t mode, uint64_t size);

uint64_t mettle_safety_live_region_count(void);

uint64_t mettle_safety_descriptor_high_water(void);

void mettle_safety_reset(void);

void mettle_safety_task_capture_check(const void *pointer, const char *task,
                               const char *sender, uint32_t line);

void mettle_safety_deadline_enter(const char *name, int64_t limit,
                                  int64_t proven);
void mettle_safety_deadline_step(int64_t cost);
void mettle_safety_deadline_leave(void);

#ifdef __cplusplus
}
#endif

#endif
