#ifndef MSL_INTERP_INTERNAL_H
#define MSL_INTERP_INTERNAL_H

#include "msl_interp.h"
#include <setjmp.h>
#include <stdarg.h>

enum {
  MSL_S_BOOL, MSL_S_CHAR, MSL_S_UCHAR, MSL_S_SHORT, MSL_S_USHORT, MSL_S_INT, MSL_S_UINT,
  MSL_S_LONG, MSL_S_ULONG, MSL_S_HALF, MSL_S_BFLOAT, MSL_S_FLOAT, MSL_S_COUNT
};

enum {
  MSL_K_VOID, MSL_K_SCALAR, MSL_K_VECTOR, MSL_K_POINTER, MSL_K_RECORD, MSL_K_ARGS,
  MSL_K_ATOMIC, MSL_K_MATRIX, MSL_K_ARRAY, MSL_K_VOTE, MSL_K_NULLPTR, MSL_K_FLAGS,
  MSL_K_ORDER, MSL_K_SCOPE
};

enum { MSL_SP_NONE, MSL_SP_DEVICE, MSL_SP_CONSTANT, MSL_SP_THREADGROUP, MSL_SP_THREAD };

enum {
  MSL_BIN_ADD, MSL_BIN_SUB, MSL_BIN_MUL, MSL_BIN_DIV, MSL_BIN_MOD, MSL_BIN_AND, MSL_BIN_OR,
  MSL_BIN_XOR, MSL_BIN_SHL, MSL_BIN_SHR, MSL_BIN_EQ, MSL_BIN_NE, MSL_BIN_LT, MSL_BIN_LE,
  MSL_BIN_GT, MSL_BIN_GE
};

enum { MSL_UN_NEG, MSL_UN_BNOT, MSL_UN_LNOT };

enum {
  MSL_BI_SQRT, MSL_BI_RSQRT, MSL_BI_SIN, MSL_BI_COS, MSL_BI_LOG, MSL_BI_EXP, MSL_BI_FABS,
  MSL_BI_FMOD, MSL_BI_FMA, MSL_BI_FILL, MSL_BI_ATOMIC_LOAD, MSL_BI_ATOMIC_STORE,
  MSL_BI_ATOMIC_ADD, MSL_BI_ATOMIC_SUB, MSL_BI_ATOMIC_MIN, MSL_BI_ATOMIC_MAX,
  MSL_BI_ATOMIC_AND, MSL_BI_ATOMIC_OR, MSL_BI_ATOMIC_XOR, MSL_BI_ATOMIC_XCHG,
  MSL_BI_ATOMIC_CAS
};

enum {
  MSL_SY_BARRIER, MSL_SY_SIMD_BARRIER, MSL_SY_SUM, MSL_SY_MIN, MSL_SY_MAX, MSL_SY_PREFIX_INC,
  MSL_SY_PREFIX_EXC, MSL_SY_BROADCAST, MSL_SY_SHUFFLE, MSL_SY_BALLOT, MSL_SY_ACTIVE,
  MSL_SY_ANY, MSL_SY_ALL, MSL_SY_MAT_LOAD, MSL_SY_MAT_STORE, MSL_SY_MAT_MAC
};

enum {
  MSL_OP_CONST, MSL_OP_NULLPTR, MSL_OP_ZERO, MSL_OP_LOAD_LOCAL, MSL_OP_STORE_LOCAL,
  MSL_OP_ADDR_LOCAL, MSL_OP_UNINIT_LOCAL, MSL_OP_LOAD, MSL_OP_STORE, MSL_OP_PTR_ADD,
  MSL_OP_FIELD, MSL_OP_INDEX, MSL_OP_BINARY, MSL_OP_UNARY, MSL_OP_CAST, MSL_OP_ASTYPE,
  MSL_OP_PTR_TO_INT, MSL_OP_INT_TO_PTR, MSL_OP_EXTRACT, MSL_OP_JUMP, MSL_OP_JUMP_FALSE,
  MSL_OP_POP, MSL_OP_DUP, MSL_OP_CALL, MSL_OP_RETURN, MSL_OP_RETURN_VALUE, MSL_OP_BUILTIN,
  MSL_OP_SYNC, MSL_OP_LOG, MSL_OP_FALL_OFF, MSL_OP_LANE_ELEM
};

enum { MSL_VAR_THREAD, MSL_VAR_GROUP, MSL_VAR_ARGS };

enum {
  MSL_ATTR_NONE, MSL_ATTR_BUFFER, MSL_ATTR_ARENA, MSL_ATTR_TID, MSL_ATTR_CTAID, MSL_ATTR_NTID,
  MSL_ATTR_NCTAID, MSL_ATTR_GID, MSL_ATTR_NGRID, MSL_ATTR_LANE, MSL_ATTR_LANES,
  MSL_ATTR_SIMD_INDEX, MSL_ATTR_SIMD_COUNT, MSL_ATTR_LINEAR
};

enum { MSL_TK_EOF, MSL_TK_IDENT, MSL_TK_INT, MSL_TK_FLOAT, MSL_TK_STRING, MSL_TK_PUNCT };

typedef struct {
  const char *name;
  size_t size;
  int is_int;
  int is_signed;
  int is_float;
} MslScalarInfo;

extern const MslScalarInfo msl_scalars[MSL_S_COUNT];

typedef struct MslType MslType;

typedef struct {
  const char *name;
  const MslType *type;
  size_t offset;
} MslField;

struct MslType {
  int kind;
  int scalar;
  int count;
  int packed;
  int space;
  const MslType *elem;
  size_t size;
  size_t align;
  const char *name;
  MslField *fields;
  size_t field_count;
  int slots;
  MslType *next;
};

typedef struct {
  int kind;
  int line;
  const char *text;
  size_t len;
  uint64_t int_value;
  int int_unsigned;
  int int_long;
  int int_hex;
  uint32_t float_bits;
  char *string_value;
} MslToken;

typedef struct {
  int op;
  int line;
  int statement;
  int64_t a;
  int64_t b;
  const MslType *t1;
  const MslType *t2;
  const void *p;
} MslInsn;

typedef struct {
  const char *format;
  size_t arg_count;
  const MslType **arg_types;
  int fault;
} MslLogInfo;

typedef struct {
  const char *name;
  const MslType *type;
  int storage;
  size_t align;
  int attr;
  int line;
} MslLocal;

typedef struct MslFunction MslFunction;

struct MslFunction {
  const char *name;
  const MslType *ret;
  int is_kernel;
  int defined;
  int line;
  MslLocal *locals;
  size_t local_count;
  size_t local_cap;
  size_t param_count;
  MslInsn *code;
  size_t code_count;
  size_t code_cap;
  uint32_t max_threads;
  MslFunction **callees;
  size_t callee_count;
  size_t callee_cap;
  size_t *local_offsets;
  size_t frame_bytes;
  size_t param_slots;
  int visit;
};

typedef struct MslChunk {
  struct MslChunk *next;
  size_t used;
  size_t cap;
  unsigned char *data;
} MslChunk;

typedef struct {
  uint8_t *host;
  size_t size;
  uint64_t address;
} MslBuffer;

struct MslProgram {
  MslChunk *chunks;
  MslType *types;
  MslType *scalar_types[MSL_S_COUNT];
  MslType *void_type;
  MslType *nullptr_type;
  MslType *vote_type;
  MslType *flags_type;
  MslType *order_type;
  MslType *scope_type;
  MslType *atomic_type;
  MslType **named;
  size_t named_count;
  size_t named_cap;
  MslFunction **functions;
  size_t function_count;
  size_t function_cap;
  MslBuffer *buffers;
  size_t buffer_count;
  size_t buffer_cap;
  uint64_t next_address;
  int saw_include;
};

void *msl_arena_alloc(MslProgram *prog, size_t size);
char *msl_arena_strndup(MslProgram *prog, const char *text, size_t len);
void msl_format_error(char *error, size_t error_size, const char *fmt, ...);
void msl_vformat_error(char *error, size_t error_size, const char *fmt, va_list ap);

MslType *msl_new_type(MslProgram *prog, int kind);
const MslType *msl_pointer_type(MslProgram *prog, int space, const MslType *elem);
const MslType *msl_array_type(MslProgram *prog, const MslType *elem, int count);
const MslType *msl_vector_type(MslProgram *prog, int scalar, int count, int packed);
const MslType *msl_vector_by_name(MslProgram *prog, const char *name, size_t len);
const MslType *msl_matrix_by_name(MslProgram *prog, const char *name, size_t len);
const MslType *msl_matrix_type(MslProgram *prog, int scalar);
const MslType *msl_named_type(MslProgram *prog, const char *name, size_t len);
int msl_record_slots(size_t size);
void msl_type_name(const MslType *type, char *buffer, size_t size);
const char *msl_space_name(int space);

int msl_lex(MslProgram *prog, const char *source, size_t length, MslToken **tokens, size_t *count, char *error, size_t error_size);
int msl_compile(MslProgram *prog, MslToken *tokens, size_t count, char *error, size_t error_size);

float msl_bits_to_float(uint32_t bits);
uint32_t msl_float_to_bits(float value);
float msl_round_float(double value);
uint32_t msl_round_format(double value, int sticky, int mant_bits, int exp_bits);
uint16_t msl_float_to_half(float value);
float msl_half_to_float(uint16_t bits);
uint16_t msl_float_to_bfloat(float value);
float msl_bfloat_to_float(uint16_t bits);
double msl_float_kind_to_double(int scalar, uint64_t bits);
uint64_t msl_double_to_float_kind(int scalar, double value, int sticky);
uint64_t msl_canonical(int scalar, uint64_t raw);
int msl_int_binary(int scalar, int op, uint64_t a, uint64_t b, uint64_t *out, char *msg, size_t msg_size);
int msl_int_shift(int scalar, int left, uint64_t a, int count_scalar, uint64_t count, uint64_t *out, char *msg, size_t msg_size);
uint64_t msl_flush_subnormal(int scalar, uint64_t bits);
int msl_float_binary(int scalar, int op, uint64_t a, uint64_t b, uint64_t *out, char *msg, size_t msg_size);
int msl_unary(int scalar, int op, uint64_t a, uint64_t *out, char *msg, size_t msg_size);
int msl_convert(int from, int to, uint64_t value, uint64_t *out, char *msg, size_t msg_size);
uint16_t msl_fma_half(uint16_t a, uint16_t b, uint16_t c);
uint32_t msl_fma_float(uint32_t a, uint32_t b, uint32_t c);
double msl_math_unary(int builtin, double x);

typedef struct {
  int space;
  uint32_t object;
  uint32_t gen;
  int64_t offset;
} MslPtr;

typedef struct {
  uint8_t *bytes;
  uint8_t *init;
  size_t size;
  size_t align;
  uint64_t base;
  uint32_t gen;
  int32_t owner;
  uint8_t space;
  uint8_t alive;
  uint8_t readonly;
  uint8_t heap;
} MslObject;

typedef struct {
  const MslFunction *fn;
  size_t pc;
  size_t stack_base;
  uint32_t *objects;
  uint8_t *storage;
} MslFrame;

enum { MSL_TS_READY, MSL_TS_BARRIER, MSL_TS_SIMD, MSL_TS_DONE };

typedef struct {
  uint64_t *stack;
  size_t sp;
  size_t cap;
  MslFrame *frames;
  size_t depth;
  size_t frame_cap;
  int state;
  const MslInsn *wait;
  int wait_line;
  uint32_t lid;
  uint32_t pos[3];
  uint32_t lane;
  uint32_t simd;
} MslThread;

typedef struct {
  MslProgram *prog;
  const MslDispatch *dispatch;
  const MslFunction *kernel;
  MslObject *objects;
  size_t object_count;
  size_t object_cap;
  uint32_t *free_objects;
  size_t free_count;
  size_t free_cap;
  MslThread *threads;
  uint32_t thread_count;
  uint32_t block[3];
  uint32_t group[3];
  uint32_t simd_width;
  uint32_t args_object;
  uint32_t arena_object;
  uint32_t *group_objects;
  uint64_t steps;
  uint64_t budget;
  uint64_t cas_state;
  char *error;
  size_t error_size;
  int failed;
} MslRun;

int msl_run_fail(MslRun *run, const MslThread *thread, int line, const char *fmt, ...);
uint32_t msl_object_new(MslRun *run, int space, size_t size, size_t align, int32_t owner);
void msl_object_release(MslRun *run, uint32_t index);
uint64_t msl_ptr_encode(const MslRun *run, MslPtr ptr);
MslPtr msl_ptr_decode(const MslRun *run, uint64_t bits);
MslPtr msl_ptr_unpack(const uint64_t *slots);
void msl_ptr_pack(MslPtr ptr, uint64_t *slots);
int msl_mem_access(MslRun *run, const MslThread *thread, int line, MslPtr ptr, int space, size_t size, size_t align, int write, MslObject **out);
int msl_read_value(MslRun *run, const MslThread *thread, int line, MslPtr ptr, int space, const MslType *type, uint64_t *slots);
int msl_write_value(MslRun *run, const MslThread *thread, int line, MslPtr ptr, int space, const MslType *type, const uint64_t *slots);
int msl_stack_reserve(MslRun *run, MslThread *thread, size_t slots);
int msl_exec_thread(MslRun *run, MslThread *thread);
int msl_exec_builtin(MslRun *run, MslThread *thread, const MslInsn *insn);
int msl_exec_log(MslRun *run, MslThread *thread, const MslInsn *insn);
int msl_resolve_simd(MslRun *run, MslThread **lanes, uint32_t count, const MslInsn *insn);
uint32_t msl_matrix_lane_element(uint32_t lane);
size_t msl_sync_arg_slots(const MslInsn *insn);
uint64_t msl_set_step_budget(uint64_t budget);

typedef struct MslCase MslCase;

struct MslCase {
  const char *name;
  const char *body;
  int full;
  uint32_t block[3];
  uint32_t grid[3];
  uint32_t simd_width;
  uint32_t tg_bytes;
  int spurious;
  const uint32_t *input;
  size_t input_count;
  const uint32_t *initial;
  const uint32_t *expect;
  size_t expect_count;
  void (*prepare)(uint32_t *input, uint32_t *expect);
  const char *error;
  const char *log;
  uint64_t budget;
  uint32_t device_ulp;
  const char *device_skip;
  int (*device_accept)(const uint32_t *out, const uint32_t *expect);
};

extern const char msl_test_header[];
extern const MslCase msl_value_cases[];
extern const size_t msl_value_case_count;
extern const MslCase msl_collective_cases[];
extern const size_t msl_collective_case_count;
extern const MslCase msl_error_cases[];
extern const size_t msl_error_case_count;

#endif
