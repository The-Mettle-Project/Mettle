#ifndef CODE_GENERATOR_INTERNAL_H
#define CODE_GENERATOR_INTERNAL_H

#include "code_generator.h"

void code_generator_set_error(CodeGenerator *generator, const char *format, ...);
const char *code_generator_get_link_symbol_name(CodeGenerator *generator,
                                                const char *symbol_name);
int code_generator_generate_program_binary_object(CodeGenerator *generator,
                                                  ASTNode *program);

/* Microsoft x64 ABI classification.
 *
 * INDIRECT: aggregate (struct/array-by-value) where sizeof > 8, OR
 *           aggregate where sizeof <= 8 but not a power of two (1, 2, 4, 8).
 * DIRECT:   everything else (scalars; small power-of-2 aggregates).
 *
 * The caller copies INDIRECT arguments to a stack temp and passes the address;
 * INDIRECT returns use a caller-allocated hidden first-argument pointer. See
 * docs/struct-abi-design.md for the full contract. */
typedef enum {
  ABI_PASS_DIRECT = 0,
  ABI_PASS_INDIRECT = 1,
} AbiPassKind;

AbiPassKind code_generator_abi_classify(const Type *type);
int code_generator_type_is_aggregate(const Type *type);
size_t code_generator_abi_type_size(const Type *type);

#endif // CODE_GENERATOR_INTERNAL_H
