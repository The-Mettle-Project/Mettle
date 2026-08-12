/* mtlc/target.h - backend target selection (architecture, object format, link).
 *
 * These enums name the code generators and output formats libmtlc can drive.
 * The x86-64, ARM64, PTX, and SPIR-V paths all exist today.
 */
#ifndef MTLC_TARGET_H
#define MTLC_TARGET_H

#ifdef __cplusplus
extern "C" {
#endif

/* Code-generation architecture / backend. */
typedef enum {
  MTLC_ARCH_X86_64 = 0, /* native x86-64 + AVX2 (default host path) */
  MTLC_ARCH_ARM64,      /* AArch64 */
  MTLC_ARCH_PTX,        /* NVIDIA PTX (GPU offload) */
  MTLC_ARCH_SPIRV       /* SPIR-V, OpenCL 2.0 environment (GPU offload) */
} MtlcArch;

/* Relocatable object container format. */
typedef enum {
  MTLC_OBJ_COFF = 0, /* Windows COFF (x64) */
  MTLC_OBJ_ELF       /* System V ELF (x64) */
} MtlcObjectFormat;

/* Final executable format produced by the linker stage. */
typedef enum {
  MTLC_LINK_PE = 0, /* Windows PE (native internal linker) */
  MTLC_LINK_ELF     /* ELF (system toolchain / direct) */
} MtlcLinkTarget;

/* Host defaults, resolved at build time (COFF/PE on Windows, ELF elsewhere). */
MtlcObjectFormat mtlc_host_object_format(void);
MtlcLinkTarget mtlc_host_link_target(void);
const char *mtlc_arch_name(MtlcArch arch);

#ifdef __cplusplus
}
#endif

#endif /* MTLC_TARGET_H */
