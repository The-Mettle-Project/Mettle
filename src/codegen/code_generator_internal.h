#ifndef CODE_GENERATOR_INTERNAL_H
#define CODE_GENERATOR_INTERNAL_H

#include "code_generator.h"

enum {
  CG_SYM_FUNCTION,
  CG_SYM_VARIABLE,
  CG_SYM_CONSTANT,
  CG_SYM_PARAMETER
};
enum { CG_SCOPE_GLOBAL };

typedef struct {
  int type;
} CgScope;

typedef struct {
  int kind;
  const MtlcType *type;
  int is_extern;
  const char *link_name;
  const CgScope *scope;
  union {
    struct {
      const MtlcType *return_type;
      MtlcType **parameter_types;
      size_t parameter_count;
      int is_c_variadic;
    } function;
    struct {
      long long value;
    } constant;
    struct {
      int register_id;
      int memory_offset;
      int is_in_register;
      int is_indirect_param;
    } variable;
  } data;
} CgSym;

const CgSym *code_generator_lookup_symbol(CodeGenerator *generator,
                                          const char *name);
const MtlcType *code_generator_named_type(CodeGenerator *generator,
                                          const char *name);

void code_generator_set_error(CodeGenerator *generator, const char *format, ...);
const char *code_generator_get_link_symbol_name(CodeGenerator *generator,
                                                const char *symbol_name);
int code_generator_generate_program_binary_object(CodeGenerator *generator);

typedef enum {
  ABI_PASS_DIRECT = 0,
  ABI_PASS_INDIRECT = 1,
} AbiPassKind;

AbiPassKind code_generator_abi_classify(const MtlcType *type);
int code_generator_type_is_aggregate(const MtlcType *type);
size_t code_generator_abi_type_size(const MtlcType *type);

#endif
