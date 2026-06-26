/* AArch64 emit-layer + execution test. Emits complete AAPCS64 functions, then
 * (1) validates each by decoding every word with our disassembler, and (2)
 * writes each as a minimal static AArch64 ELF executable. Each program is
 * [entry stub: set args, bl func, exit(x0)] ++ [func body], so running it under
 * qemu-aarch64 returns the function's result as the process exit code.
 *
 * Build: gcc -Isrc tests/arm64_emit_test.c src/codegen/binary/arm64_encode.c
 *            src/codegen/binary/arm64_emit.c src/codegen/binary/arm64_disasm.c
 * Run:   arm64_emit_test <out_dir>  (then tests/arm64_qemu_run.sh) */

#include "codegen/binary/arm64.h"
#include "codegen/binary/arm64_emit.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_fail = 0;

/* SVC #0: supervisor call. AArch64 Linux syscall: x8=number, args in x0.. */
static uint32_t arm64_svc0(void) { return 0xD4000001u; }
/* exit syscall number on AArch64 Linux. */
#define NR_EXIT 93

/* ---- minimal static AArch64 ELF executable ------------------------------ */

#define ELF_BASE 0x400000u
#define ELF_HDRS 120u /* 64-byte ELF header + 56-byte program header */

static void put16(unsigned char *p, uint16_t v) { memcpy(p, &v, 2); }
static void put32(unsigned char *p, uint32_t v) { memcpy(p, &v, 4); }
static void put64(unsigned char *p, uint64_t v) { memcpy(p, &v, 8); }

static int write_elf(const char *path, const unsigned char *code,
                     size_t code_len) {
  unsigned char hdr[ELF_HDRS];
  memset(hdr, 0, sizeof(hdr));
  uint64_t total = ELF_HDRS + code_len;
  uint64_t entry = ELF_BASE + ELF_HDRS;

  /* ELF header */
  hdr[0] = 0x7F; hdr[1] = 'E'; hdr[2] = 'L'; hdr[3] = 'F';
  hdr[4] = 2; /* ELFCLASS64 */
  hdr[5] = 1; /* ELFDATA2LSB */
  hdr[6] = 1; /* EV_CURRENT */
  put16(hdr + 16, 2);    /* e_type = ET_EXEC */
  put16(hdr + 18, 183);  /* e_machine = EM_AARCH64 */
  put32(hdr + 20, 1);    /* e_version */
  put64(hdr + 24, entry);/* e_entry */
  put64(hdr + 32, 64);   /* e_phoff */
  put64(hdr + 40, 0);    /* e_shoff */
  put32(hdr + 48, 0);    /* e_flags */
  put16(hdr + 52, 64);   /* e_ehsize */
  put16(hdr + 54, 56);   /* e_phentsize */
  put16(hdr + 56, 1);    /* e_phnum */
  put16(hdr + 58, 0);    /* e_shentsize */
  put16(hdr + 60, 0);    /* e_shnum */
  put16(hdr + 62, 0);    /* e_shstrndx */

  /* one PT_LOAD program header covering headers + code, R+X */
  unsigned char *ph = hdr + 64;
  put32(ph + 0, 1);          /* p_type = PT_LOAD */
  put32(ph + 4, 5);          /* p_flags = R|X */
  put64(ph + 8, 0);          /* p_offset */
  put64(ph + 16, ELF_BASE);  /* p_vaddr */
  put64(ph + 24, ELF_BASE);  /* p_paddr */
  put64(ph + 32, total);     /* p_filesz */
  put64(ph + 40, total);     /* p_memsz */
  put64(ph + 48, 0x1000);    /* p_align */

  FILE *f = fopen(path, "wb");
  if (!f) {
    return 0;
  }
  int ok = fwrite(hdr, 1, ELF_HDRS, f) == ELF_HDRS &&
           fwrite(code, 1, code_len, f) == code_len;
  fclose(f);
  return ok;
}

/* ---- function bodies (args already in x0,x1; result in x0) -------------- */

typedef void (*BodyFn)(Arm64Emit *);

static void body_add(Arm64Emit *e) {
  arm64_emit_prologue(e, 0, NULL, 0);
  arm64_emit_word(e, arm64_add_reg(1, ARM64_X0, ARM64_X0, ARM64_X1));
  arm64_emit_epilogue(e, 0, NULL, 0);
}

/* sum_to_n(n) = 1+2+...+n */
static void body_sum_to_n(Arm64Emit *e) {
  int cond = arm64_new_label(e), done = arm64_new_label(e);
  arm64_emit_prologue(e, 0, NULL, 0);
  arm64_emit_word(e, arm64_movz(1, ARM64_X9, 0, 0));   /* acc */
  arm64_emit_word(e, arm64_movz(1, ARM64_X10, 1, 0));  /* i */
  arm64_bind_label(e, cond);
  arm64_emit_word(e, arm64_cmp_reg(1, ARM64_X10, ARM64_X0));
  arm64_emit_bcond(e, ARM64_GT, done);
  arm64_emit_word(e, arm64_add_reg(1, ARM64_X9, ARM64_X9, ARM64_X10));
  arm64_emit_word(e, arm64_add_imm(1, ARM64_X10, ARM64_X10, 1, 0));
  arm64_emit_b(e, cond);
  arm64_bind_label(e, done);
  arm64_emit_word(e, arm64_mov_reg(1, ARM64_X0, ARM64_X9));
  arm64_emit_epilogue(e, 0, NULL, 0);
}

/* fact(n) = n! */
static void body_fact(Arm64Emit *e) {
  int cond = arm64_new_label(e), done = arm64_new_label(e);
  arm64_emit_prologue(e, 0, NULL, 0);
  arm64_emit_word(e, arm64_movz(1, ARM64_X9, 1, 0));   /* acc */
  arm64_emit_word(e, arm64_movz(1, ARM64_X10, 1, 0));  /* i */
  arm64_bind_label(e, cond);
  arm64_emit_word(e, arm64_cmp_reg(1, ARM64_X10, ARM64_X0));
  arm64_emit_bcond(e, ARM64_GT, done);
  arm64_emit_word(e, arm64_mul(1, ARM64_X9, ARM64_X9, ARM64_X10));
  arm64_emit_word(e, arm64_add_imm(1, ARM64_X10, ARM64_X10, 1, 0));
  arm64_emit_b(e, cond);
  arm64_bind_label(e, done);
  arm64_emit_word(e, arm64_mov_reg(1, ARM64_X0, ARM64_X9));
  arm64_emit_epilogue(e, 0, NULL, 0);
}

/* mod(a,b) = a % b  via  q=a/b ; r = a - q*b */
static void body_mod(Arm64Emit *e) {
  arm64_emit_prologue(e, 0, NULL, 0);
  arm64_emit_word(e, arm64_sdiv(1, ARM64_X9, ARM64_X0, ARM64_X1));
  arm64_emit_word(e, arm64_msub(1, ARM64_X0, ARM64_X9, ARM64_X1, ARM64_X0));
  arm64_emit_epilogue(e, 0, NULL, 0);
}

/* popcount(n): number of set bits, exercising cbz / and / lsr-immediate */
static void body_popcount(Arm64Emit *e) {
  int loop = arm64_new_label(e), done = arm64_new_label(e);
  arm64_emit_prologue(e, 0, NULL, 0);
  arm64_emit_word(e, arm64_movz(1, ARM64_X9, 0, 0));   /* acc */
  arm64_emit_word(e, arm64_movz(1, ARM64_X11, 1, 0));  /* mask */
  arm64_bind_label(e, loop);
  arm64_emit_cbz(e, 1, ARM64_X0, done);
  arm64_emit_word(e, arm64_and_reg(1, ARM64_X10, ARM64_X0, ARM64_X11));
  arm64_emit_word(e, arm64_add_reg(1, ARM64_X9, ARM64_X9, ARM64_X10));
  arm64_emit_word(e, arm64_lsr_imm(1, ARM64_X0, ARM64_X0, 1));
  arm64_emit_b(e, loop);
  arm64_bind_label(e, done);
  arm64_emit_word(e, arm64_mov_reg(1, ARM64_X0, ARM64_X9));
  arm64_emit_epilogue(e, 0, NULL, 0);
}

/* max(a,b) via cmp + csel (signed greater-than) */
static void body_max(Arm64Emit *e) {
  arm64_emit_prologue(e, 0, NULL, 0);
  arm64_emit_word(e, arm64_cmp_reg(1, ARM64_X0, ARM64_X1));
  arm64_emit_word(e, arm64_csel(1, ARM64_X0, ARM64_X0, ARM64_X1, ARM64_GT));
  arm64_emit_epilogue(e, 0, NULL, 0);
}

/* ---- harness ------------------------------------------------------------ */

/* Assemble [entry stub: set args, bl func, exit(x0)] ++ [func body], validate
 * by disassembly, and write the ELF. Returns 1 on structural success. */
static int build_case(const char *out_dir, FILE *manifest, const char *name,
                      int expected, uint16_t a, uint16_t b, int nargs,
                      BodyFn body) {
  Arm64Emit e;
  arm64_emit_init(&e);
  int func = arm64_new_label(&e);

  /* entry stub */
  arm64_emit_word(&e, arm64_movz(1, ARM64_X0, a, 0));
  if (nargs >= 2) {
    arm64_emit_word(&e, arm64_movz(1, ARM64_X1, b, 0));
  }
  arm64_emit_bl(&e, func);
  arm64_emit_word(&e, arm64_movz(1, ARM64_X8, NR_EXIT, 0)); /* exit syscall */
  arm64_emit_word(&e, arm64_svc0());

  /* function */
  arm64_bind_label(&e, func);
  body(&e);

  if (!arm64_emit_finalize(&e)) {
    printf("  FAIL %-12s emit/finalize error\n", name);
    g_fail++;
    arm64_emit_free(&e);
    return 0;
  }

  /* offline validation: every word decodes, and a RET exists */
  int n_words = (int)(e.code.len / 4);
  int saw_ret = 0, saw_unknown = 0;
  for (int i = 0; i < n_words; i++) {
    uint32_t w;
    memcpy(&w, e.code.data + (size_t)i * 4, 4);
    Arm64Inst d = arm64_decode(w);
    if (d.op == ARM64_DIS_UNKNOWN && w != arm64_svc0()) {
      saw_unknown = 1;
    }
    if (d.op == ARM64_DIS_RET) {
      saw_ret = 1;
    }
  }
  if (saw_unknown || !saw_ret) {
    printf("  FAIL %-12s decode (unknown=%d ret=%d)\n", name, saw_unknown,
           saw_ret);
    g_fail++;
    arm64_emit_free(&e);
    return 0;
  }

  char path[1024];
  snprintf(path, sizeof(path), "%s/%s.elf", out_dir, name);
  if (!write_elf(path, e.code.data, e.code.len)) {
    printf("  FAIL %-12s write_elf %s\n", name, path);
    g_fail++;
    arm64_emit_free(&e);
    return 0;
  }

  /* manifest line consumed by the qemu run step (tests/arm64_qemu_run.sh);
   * written LF-only so it parses cleanly under WSL/sh. */
  if (manifest) {
    fprintf(manifest, "%s %d\n", name, expected);
  }
  printf("  EXEC %-12s expect %-4d %s\n", name, expected, path);
  arm64_emit_free(&e);
  return 1;
}

int main(int argc, char **argv) {
  const char *out_dir = (argc > 1) ? argv[1] : ".";
  printf("=== AArch64 emit + ELF self-test ===\n");

  char manifest_path[1024];
  snprintf(manifest_path, sizeof(manifest_path), "%s/manifest.txt", out_dir);
  FILE *manifest = fopen(manifest_path, "wb"); /* binary: LF-only line endings */

  build_case(out_dir, manifest, "add", 12, 5, 7, 2, body_add);
  build_case(out_dir, manifest, "sum_to_n", 55, 10, 0, 1, body_sum_to_n);
  build_case(out_dir, manifest, "fact", 120, 5, 0, 1, body_fact);
  build_case(out_dir, manifest, "mod", 2, 17, 5, 2, body_mod);
  build_case(out_dir, manifest, "popcount", 3, 0xB, 0, 1, body_popcount);
  build_case(out_dir, manifest, "max", 20, 7, 20, 2, body_max);

  if (manifest) {
    fclose(manifest);
  }
  printf("\n%s\n", g_fail ? "RESULT: FAIL" : "RESULT: PASS");
  return g_fail ? 1 : 0;
}
