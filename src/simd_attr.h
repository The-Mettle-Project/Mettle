/* simd_attr.h - the `@simd` vectorization-contract level, shared by frontend and
 * backend.
 *
 * A frontend parses `@simd` / `@simd!` annotations into a SimdAttr; the backend's
 * SIMD contract verifier (src/ir/optimizer/ir_optimize_simd_contract.c) reads it
 * back off the IR's SIMD markers. It lives in this dependency-free header so the
 * backend IR (ir.h) can reference it without depending on a frontend AST header. */
#ifndef MTLC_SIMD_ATTR_H
#define MTLC_SIMD_ATTR_H

typedef enum {
  SIMD_ATTR_NONE = 0,     // no attribute
  SIMD_ATTR_HINT = 1,     // `@simd`  : best-effort; warn if it can't vectorize
  SIMD_ATTR_CONTRACT = 2, // `@simd!` : hard contract; compile error if it can't
  SIMD_ATTR_REPORT = 3    // internal: `--explain` marks every unannotated loop
                          // so the verifier can report what became of it;
                          // never warns or errors, only emits notes
} SimdAttr;

#endif /* MTLC_SIMD_ATTR_H */
