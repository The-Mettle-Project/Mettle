#ifndef CODE_GENERATOR_INTERNAL_H
#define CODE_GENERATOR_INTERNAL_H

#include "code_generator.h"

/* Symbol kinds a code-generator symbol view can take (backend-owned; the
 * frontend's SymbolKind is no longer visible to codegen). */
enum {
  CG_SYM_FUNCTION,
  CG_SYM_VARIABLE,
  CG_SYM_CONSTANT,
  CG_SYM_PARAMETER /* never produced by a module lookup; kept for dead paths */
};
/* Scope kinds. Module symbols are always at global scope. */
enum { CG_SCOPE_GLOBAL };

/* Minimal scope view: codegen only ever checks `scope->type == CG_SCOPE_GLOBAL`. */
typedef struct {
  int type;
} CgScope;

/* A frontend-shaped VIEW of an IR module symbol (IRModuleSymbol), built on
 * demand by code_generator_lookup_symbol. Its field layout deliberately mirrors
 * the Symbol members codegen reads, so the call sites that used to hold a
 * `Symbol *` keep their field accesses (s->kind, s->type,
 * s->data.function.return_type, ...) unchanged after switching the lookup. This
 * is the seam that let codegen stop calling the frontend symbol table. */
typedef struct {
  int kind;               /* CG_SYM_FUNCTION / CG_SYM_VARIABLE / CG_SYM_CONSTANT */
  const MtlcType *type;   /* the symbol's type */
  int is_extern;
  const char *link_name;  /* object-file linkage name, or NULL */
  /* Module symbols all live at global scope; this points at a shared sentinel
   * whose ->type is CG_SCOPE_GLOBAL so the `scope->type == CG_SCOPE_GLOBAL`
   * guards in codegen keep working unchanged. NULL only for an absent symbol. */
  const CgScope *scope;
  union {
    struct {
      const MtlcType *return_type;
      MtlcType **parameter_types;
      size_t parameter_count;
    } function;
    struct {
      long long value;
    } constant;
    /* Register-residency info; always zero for a module symbol (globals are
     * never register-resident), which keeps the register-promotion fast paths
     * in codegen correctly inert. */
    struct {
      int register_id;
      int memory_offset;
      int is_in_register;
      int is_indirect_param;
    } variable;
  } data;
} CgSym;

/* Resolve a module symbol (global/function/constant) by name via the IR module
 * symbol table, as a frontend-shaped CgSym view. Returns NULL when absent (e.g.
 * locals/params, which codegen resolves from the IR instead). The view is cached
 * on the IRModuleSymbol for the lifetime of the program. */
const CgSym *code_generator_lookup_symbol(CodeGenerator *generator,
                                          const char *name);
/* Resolve a type name (primitive/struct/enum/cast target) to its MtlcType via
 * the IR type registry. Replaces type_checker_get_type_by_name for the backend. */
const MtlcType *code_generator_named_type(CodeGenerator *generator,
                                          const char *name);

void code_generator_set_error(CodeGenerator *generator, const char *format, ...);
const char *code_generator_get_link_symbol_name(CodeGenerator *generator,
                                                const char *symbol_name);
int code_generator_generate_program_binary_object(CodeGenerator *generator);

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

AbiPassKind code_generator_abi_classify(const MtlcType *type);
int code_generator_type_is_aggregate(const MtlcType *type);
size_t code_generator_abi_type_size(const MtlcType *type);

#endif // CODE_GENERATOR_INTERNAL_H
