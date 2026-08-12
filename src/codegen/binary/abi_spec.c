/* Calling-convention descriptor selection and argument-layout computation for
 * the binary backend. Centralizes every place the MS-x64 and SysV AMD64
 * conventions differ so the rest of the backend is convention-agnostic. */

#include "codegen/binary/internal.h"

#include <stddef.h>

/* MS-x64: first four args by position in RCX/RDX/R8/R9 (or XMM0..3), 32-byte
 * shadow space, INDIRECT out-pointer in RCX. */
static const BinaryGpRegister MS_X64_INT_PARAMS[] = {
    BINARY_GP_RCX, BINARY_GP_RDX, BINARY_GP_R8, BINARY_GP_R9};
static const BinaryXmmRegister MS_X64_FLOAT_PARAMS[] = {
    BINARY_XMM0, BINARY_XMM1, BINARY_XMM2, BINARY_XMM3};

static const BinaryAbi MS_X64_ABI = {
    MS_X64_INT_PARAMS,        4,
    MS_X64_FLOAT_PARAMS,      4,
    32,                       /* shadow space */
    BINARY_GP_RCX,            /* INDIRECT return out-pointer */
    0,                        /* shared positional slot for int+float */
};

/* SysV AMD64: up to six integer args in RDI/RSI/RDX/RCX/R8/R9 and up to eight
 * float args in XMM0..7, counted independently; no shadow space; INDIRECT
 * out-pointer in RDI. */
static const BinaryGpRegister SYSV_INT_PARAMS[] = {
    BINARY_GP_RDI, BINARY_GP_RSI, BINARY_GP_RDX,
    BINARY_GP_RCX, BINARY_GP_R8,  BINARY_GP_R9};
static const BinaryXmmRegister SYSV_FLOAT_PARAMS[] = {
    BINARY_XMM0, BINARY_XMM1, BINARY_XMM2, BINARY_XMM3,
    BINARY_XMM4, BINARY_XMM5, BINARY_XMM6, BINARY_XMM7};

static const BinaryAbi SYSV_ABI = {
    SYSV_INT_PARAMS,          6,
    SYSV_FLOAT_PARAMS,        8,
    0,                        /* no shadow space */
    BINARY_GP_RDI,            /* INDIRECT return out-pointer */
    1,                        /* separate int/float register sequences */
};

static const BinaryAbi *g_active_abi = &MS_X64_ABI;

void code_generator_binary_select_abi(BinaryTargetFormat format) {
  switch (format) {
  case BINARY_TARGET_FORMAT_ELF_X64:
    g_active_abi = &SYSV_ABI;
    break;
  case BINARY_TARGET_FORMAT_COFF_WIN64:
  default:
    g_active_abi = &MS_X64_ABI;
    break;
  }
}

const BinaryAbi *code_generator_binary_active_abi(void) { return g_active_abi; }

int code_generator_binary_compute_arg_layout(const BinaryAbi *abi,
                                             const int *is_float, size_t count,
                                             BinaryArgLocation *locations_out,
                                             int *stack_bytes_out) {
  if (!abi || (!is_float && count > 0) || (!locations_out && count > 0)) {
    return 0;
  }

  size_t int_used = 0;
  size_t float_used = 0;
  /* Positional slot index for the MS-x64 shared sequence. */
  size_t positional = 0;
  int stack_cursor = 0;

  for (size_t i = 0; i < count; i++) {
    int wants_float = is_float[i] ? 1 : 0;
    BinaryArgLocation *loc = &locations_out[i];

    if (abi->counts_classes_separately) {
      /* SysV: each class draws from its own register pool; overflow spills to
       * the stack in argument order. */
      if (wants_float) {
        if (float_used < abi->float_param_count) {
          loc->kind = BINARY_ARG_IN_XMM_REGISTER;
          loc->xmm_register = abi->float_param_registers[float_used++];
          continue;
        }
      } else {
        if (int_used < abi->int_param_count) {
          loc->kind = BINARY_ARG_IN_GP_REGISTER;
          loc->gp_register = abi->int_param_registers[int_used++];
          continue;
        }
      }
      loc->kind = BINARY_ARG_ON_STACK;
      loc->stack_offset = stack_cursor;
      stack_cursor += BINARY_FUNCTION_STACK_SLOT_SIZE;
    } else {
      /* MS-x64: one positional slot indexes both register files; slots beyond
       * the register count go on the stack at (slot - regcount) * 8. The int
       * and float register tables have the same length here. */
      size_t reg_count = abi->int_param_count;
      if (positional < reg_count) {
        if (wants_float) {
          loc->kind = BINARY_ARG_IN_XMM_REGISTER;
          loc->xmm_register = abi->float_param_registers[positional];
        } else {
          loc->kind = BINARY_ARG_IN_GP_REGISTER;
          loc->gp_register = abi->int_param_registers[positional];
        }
      } else {
        loc->kind = BINARY_ARG_ON_STACK;
        loc->stack_offset =
            (int)((positional - reg_count) * BINARY_FUNCTION_STACK_SLOT_SIZE);
        if (loc->stack_offset + BINARY_FUNCTION_STACK_SLOT_SIZE > stack_cursor) {
          stack_cursor = loc->stack_offset + BINARY_FUNCTION_STACK_SLOT_SIZE;
        }
      }
      positional++;
    }
  }

  if (stack_bytes_out) {
    *stack_bytes_out = stack_cursor;
  }
  return 1;
}
