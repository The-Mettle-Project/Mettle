#ifndef IR_INTERP_H
#define IR_INTERP_H

#include "ir.h"
#include "ir_numerics.h"
#include <stdint.h>

typedef enum {
  IR_INTERP_OK = 0,
  IR_INTERP_UNSUPPORTED,
  IR_INTERP_TRAP,
  IR_INTERP_FUEL,
  IR_INTERP_DEPTH,
  IR_INTERP_GUARD_TRAP,
  IR_INTERP_ASSERT_FAIL
} IRInterpStatus;

typedef struct {
  long long i;
  double f;
  int is_float;
  int undefined;
  uint32_t term;
} IRInterpValue;

#define IR_INTERP_EXTERN_MEM_CAP 96

typedef struct {
  char name[64];
  IRInterpValue args[8];
  size_t arg_count;
  unsigned char arg_mem[8][IR_INTERP_EXTERN_MEM_CAP];
  unsigned short arg_mem_len[8];
  unsigned char arg_is_pointer[8];
  unsigned char modelled;
} IRInterpExternCall;

typedef struct IRInterpMachine IRInterpMachine;

typedef struct {
  size_t line;
  long long rows;
  long long cols;
  long long row;
  long long column;
  NumTerm left;
  NumTerm right;
  int equal;
  int summary;
} IRInterpClaim;

typedef void (*IRInterpClaimHook)(void *ctx, const IRInterpClaim *claim);
typedef void (*IRInterpLaunchHook)(void *ctx, const char *kernel,
                                   const long long grid[3],
                                   const long long block[3]);

void ir_interp_set_launch_hook(IRInterpMachine *machine,
                               IRInterpLaunchHook hook, void *ctx);

void ir_interp_enable_numerics(IRInterpMachine *machine, NumStore *store,
                               IRInterpClaimHook hook, void *ctx);
const IRInstruction *ir_interp_symbolic_site(const IRInterpMachine *machine,
                                             const char **function);

IRInterpMachine *ir_interp_create(IRProgram *program);
void ir_interp_destroy(IRInterpMachine *machine);

void ir_interp_set_override(IRInterpMachine *machine, const char *name,
                            IRFunction *fn);

unsigned long long ir_interp_add_buffer(IRInterpMachine *machine,
                                        const void *init, long long size);

IRInterpStatus ir_interp_run(IRInterpMachine *machine, IRFunction *function,
                             const IRInterpValue *args, size_t arg_count,
                             IRInterpValue *result, long long fuel);

size_t ir_interp_buffer_count(const IRInterpMachine *machine);
const unsigned char *ir_interp_buffer_data(const IRInterpMachine *machine,
                                           size_t index, long long *size);
int ir_interp_branched_on_undefined(const IRInterpMachine *machine);

size_t ir_interp_extern_trace_count(const IRInterpMachine *machine);
const IRInterpExternCall *ir_interp_extern_trace(const IRInterpMachine *machine,
                                                 size_t index);
size_t ir_interp_global_count(const IRInterpMachine *machine);
const char *ir_interp_global_name(const IRInterpMachine *machine, size_t index);
IRInterpValue ir_interp_global_value(const IRInterpMachine *machine,
                                     size_t index);

long long ir_interp_pointee_window(IRInterpMachine *machine,
                                   unsigned long long value,
                                   unsigned char *out, size_t capacity);

unsigned long long ir_interp_function_address(IRInterpMachine *machine,
                                              const char *name);

int ir_interp_buffer_is_literal(const IRInterpMachine *machine, size_t index);

unsigned long long
ir_interp_next_buffer_address(const IRInterpMachine *machine);

long long ir_interp_read_bytes(IRInterpMachine *machine,
                               unsigned long long address, unsigned char *out,
                               size_t length);

long long ir_interp_fuel_remaining(const IRInterpMachine *machine);

const char *ir_interp_status_detail(const IRInterpMachine *machine);

int ir_interp_assert_info(const IRInterpMachine *machine, size_t *line,
                          size_t *column, IRInterpValue *left,
                          IRInterpValue *right, int *is_eq);

size_t ir_interp_buffer_alloc_line(const IRInterpMachine *machine,
                                   size_t index);
SourceLocation ir_interp_buffer_alloc_location(const IRInterpMachine *machine,
                                               size_t index);
int ir_interp_buffer_freed(const IRInterpMachine *machine, size_t index);

typedef void (*IRInterpValueHook)(void *ctx, size_t line, const char *name,
                                  IRInterpValue value,
                                  const char *expansion_note);
void ir_interp_set_value_hook(IRInterpMachine *machine, IRInterpValueHook hook,
                              void *ctx, const IRFunction *only_in);

void ir_interp_recheck_inferred_purity(IRInterpMachine *machine, int on);
void ir_interp_set_purity_fault(int on);
int ir_interp_purity_fault_enabled(void);
void ir_interp_enable_counting(IRInterpMachine *machine);
const long long *ir_interp_get_counts(const IRInterpMachine *machine,
                                      const IRFunction *function,
                                      size_t *count_out);

#endif
