#ifndef CODEGEN_BINARY_STRENGTH_RULES_H
#define CODEGEN_BINARY_STRENGTH_RULES_H

/* Target-neutral integer strength-reduction rules.
 *
 * One table answers, for every backend, the question "does `x <op> C` have a
 * cheaper form, and with what parameters?". The decision is the same on every
 * architecture; only the emission differs, so each backend consumes the
 * classification and emits its own instructions. Teaching every backend a new
 * reduction is adding ONE ROW to the table in strength_rules.c and handling
 * its kind where a backend can profit from it; a backend that does not handle
 * a kind simply keeps its general form.
 *
 * The Granlund-Montgomery magic parameters live here too, as the single
 * canonical implementation (Hacker's Delight Fig. 10-1 / 10-3 widened to
 * 64-bit), so a backend never carries its own drifting copy. */

#include <stdint.h>

typedef enum {
  CG_SR_NONE = 0,
  CG_SR_MUL_SHL,     /* x * 2^k         -> x << k                 (shift = k) */
  CG_SR_MUL_SHL_ADD, /* x * (2^k + 1)   -> (x << k) + x           (shift = k) */
  CG_SR_MUL_SHL_SUB, /* x * (2^k - 1)   -> (x << k) - x           (shift = k) */
  CG_SR_UDIV_SHR,    /* unsigned x / 2^k -> x >> k                (shift = k) */
  CG_SR_UREM_AND,    /* unsigned x % 2^k -> x & (2^k - 1)         (mask)      */
  CG_SR_SDIV_POW2,   /* signed x / 2^k: shift with the rounding fixup
                        (add 2^k - 1 when x < 0 before the arithmetic shift) */
  CG_SR_SREM_POW2,   /* signed x % 2^k: mask with the sign fixup */
  CG_SR_DIV_MAGIC,   /* x / C -> mulhi(x, magic) + shifts (magic, shift,
                        magic_add for the unsigned overflow reconstruction) */
  CG_SR_REM_MAGIC    /* x % C via the magic quotient and one multiply-subtract */
} CgStrengthKind;

typedef struct {
  CgStrengthKind kind;
  int shift;        /* k for the shift forms; post-shift s for the magic forms */
  long long mask;   /* 2^k - 1 for the mask forms */
  long long magic;  /* multiplier for the magic forms */
  int magic_add;    /* unsigned magic: use the overflow-safe reconstruction */
} CgStrengthRewrite;

/* Classify `x <op> C` (op one of '*', '/', '%'; C a compile-time constant;
 * is_unsigned the signedness of x). Returns 1 and fills *out when a cheaper
 * form exists, 0 when the general instruction should be kept. C == 0 always
 * returns 0 so a divide-by-zero trap still fires at runtime. */
int cg_strength_classify(char op, long long c, int is_unsigned,
                         CgStrengthRewrite *out);

/* Magic parameters for SIGNED 64-bit division by d (|d| >= 2, not a power of
 * two): n / d == mulhi_s(n, M) [+ n if M and d disagree in sign] >> s,
 * + the sign-bit correction. */
void cg_magic_s64(int64_t d, int64_t *magic_out, int *shift_out);

/* Magic parameters for UNSIGNED 64-bit division by d (d >= 2, not a power of
 * two). *add_out selects the overflow-safe reconstruction. */
void cg_magic_u64(uint64_t d, uint64_t *magic_out, int *shift_out,
                  int *add_out);

#endif /* CODEGEN_BINARY_STRENGTH_RULES_H */
