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
#include "codegen/binary/arm64_mir.h"

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

/* ---- MIR-lowered bodies (Brick 3): build scalar MIR, lower to A64 ------- */

static MirOperand P(int r) {
  MirOperand o;
  memset(&o, 0, sizeof o);
  o.kind = MIR_OPK_PHYS;
  o.phys = r;
  o.rclass = MIR_RC_GP;
  return o;
}
static MirOperand IMM(long long v) {
  MirOperand o;
  memset(&o, 0, sizeof o);
  o.kind = MIR_OPK_IMM;
  o.imm = v;
  return o;
}
static MirOperand V(int id) {
  MirOperand o;
  memset(&o, 0, sizeof o);
  o.kind = MIR_OPK_VREG;
  o.vreg = id;
  o.rclass = MIR_RC_GP;
  return o;
}
static MirOperand LBL(const char *s) {
  MirOperand o;
  memset(&o, 0, sizeof o);
  o.kind = MIR_OPK_LABEL;
  o.sym = s;
  return o;
}
static MirOperand NONE(void) {
  MirOperand o;
  memset(&o, 0, sizeof o);
  o.kind = MIR_OPK_NONE;
  return o;
}
static MirInst I(MirOpcode op, MirOperand dst, MirOperand a, MirOperand b) {
  MirInst in;
  memset(&in, 0, sizeof in);
  in.op = op;
  in.dst = dst;
  in.a = a;
  in.b = b;
  in.ir_index = -1;
  return in;
}

static void body_mir_add(Arm64Emit *e) {
  MirInst seq[] = {
      I(MIR_ADD, P(ARM64_X0), P(ARM64_X0), P(ARM64_X1)),
      I(MIR_RET, NONE(), NONE(), NONE()),
  };
  arm64_mir_encode_seq(e, seq, sizeof(seq) / sizeof(seq[0]));
}

/* sum_to_n(n) lowered from MIR: loop with a fused compare-and-branch. */
static void body_mir_sum(Arm64Emit *e) {
  MirInst seq[] = {
      I(MIR_MOV, P(ARM64_X9), IMM(0), NONE()),
      I(MIR_MOV, P(ARM64_X10), IMM(1), NONE()),
      I(MIR_LABEL, LBL("Lcond"), NONE(), NONE()),
      I(MIR_CMPBR, LBL("Ldone"), P(ARM64_X10), P(ARM64_X0)),
      I(MIR_ADD, P(ARM64_X9), P(ARM64_X9), P(ARM64_X10)),
      I(MIR_ADD, P(ARM64_X10), P(ARM64_X10), IMM(1)),
      I(MIR_JMP, LBL("Lcond"), NONE(), NONE()),
      I(MIR_LABEL, LBL("Ldone"), NONE(), NONE()),
      I(MIR_MOV, P(ARM64_X0), P(ARM64_X9), NONE()),
      I(MIR_RET, NONE(), NONE(), NONE()),
  };
  seq[3].cc = 0x8F; /* x86 JG -> AArch64 GT */
  arm64_mir_encode_seq(e, seq, sizeof(seq) / sizeof(seq[0]));
}

/* is_gt(a,b) = (a > b) via CMP + SETCC, exercising the x86-cc translation. */
static void body_mir_isgt(Arm64Emit *e) {
  MirInst seq[] = {
      I(MIR_CMP, NONE(), P(ARM64_X0), P(ARM64_X1)),
      I(MIR_SETCC, P(ARM64_X0), NONE(), NONE()),
      I(MIR_RET, NONE(), NONE(), NONE()),
  };
  seq[1].cc = 0x9F; /* x86 SETG -> AArch64 GT */
  arm64_mir_encode_seq(e, seq, sizeof(seq) / sizeof(seq[0]));
}

/* pack(a,b) = (a << 4) | b, exercising shift-immediate + OR. */
static void body_mir_pack(Arm64Emit *e) {
  MirInst seq[] = {
      I(MIR_SHL, P(ARM64_X9), P(ARM64_X0), IMM(4)),
      I(MIR_OR, P(ARM64_X0), P(ARM64_X9), P(ARM64_X1)),
      I(MIR_RET, NONE(), NONE(), NONE()),
  };
  arm64_mir_encode_seq(e, seq, sizeof(seq) / sizeof(seq[0]));
}

static void body_mir_div(Arm64Emit *e) {
  MirInst seq[] = {
      I(MIR_IDIV, P(ARM64_X0), P(ARM64_X0), P(ARM64_X1)),
      I(MIR_RET, NONE(), NONE(), NONE()),
  };
  arm64_mir_encode_seq(e, seq, sizeof(seq) / sizeof(seq[0]));
}

static void body_mir_mod(Arm64Emit *e) {
  MirInst seq[] = {
      I(MIR_IDIV, P(ARM64_X0), P(ARM64_X0), P(ARM64_X1)),
      I(MIR_RET, NONE(), NONE(), NONE()),
  };
  seq[0].cc = 1; /* RDX-result flag: modulo */
  arm64_mir_encode_seq(e, seq, sizeof(seq) / sizeof(seq[0]));
}

/* max(a,b) via CMP + conditional move (csel). */
static void body_mir_max(Arm64Emit *e) {
  MirInst seq[] = {
      I(MIR_CMP, NONE(), P(ARM64_X0), P(ARM64_X1)),
      I(MIR_CMOVCC, P(ARM64_X0), P(ARM64_X1), NONE()),
      I(MIR_RET, NONE(), NONE(), NONE()),
  };
  seq[1].cc = 0x8C; /* x86 JL -> LT: a<b ? b : a */
  arm64_mir_encode_seq(e, seq, sizeof(seq) / sizeof(seq[0]));
}

/* zero-extend the low byte (MOVZX width 1). */
static void body_mir_uxtb(Arm64Emit *e) {
  MirInst seq[] = {
      I(MIR_MOVZX, P(ARM64_X0), P(ARM64_X0), NONE()),
      I(MIR_RET, NONE(), NONE(), NONE()),
  };
  seq[0].width = 1;
  arm64_mir_encode_seq(e, seq, sizeof(seq) / sizeof(seq[0]));
}

/* is_even(n) via TEST + SETCC. */
static void body_mir_iseven(Arm64Emit *e) {
  MirInst seq[] = {
      I(MIR_MOV, P(ARM64_X9), IMM(1), NONE()),
      I(MIR_TEST, NONE(), P(ARM64_X0), P(ARM64_X9)),
      I(MIR_SETCC, P(ARM64_X0), NONE(), NONE()),
      I(MIR_RET, NONE(), NONE(), NONE()),
  };
  seq[2].cc = 0x94; /* x86 SETE -> EQ: (n & 1) == 0 */
  arm64_mir_encode_seq(e, seq, sizeof(seq) / sizeof(seq[0]));
}

/* ---- vreg-MIR bodies (Brick 4): the encoder allocates stack slots -------- *
 * These use MIR_OPK_VREG operands (the form mir_lower emits). The first N vregs
 * are parameters, homed from x0.. by arm64_mir_encode_vregs. */

/* sum_to_n(n): vregs v0=n(param), v1=acc, v2=i. */
static void body_vmir_sum(Arm64Emit *e) {
  MirInst seq[] = {
      I(MIR_MOV, V(1), IMM(0), NONE()),
      I(MIR_MOV, V(2), IMM(1), NONE()),
      I(MIR_LABEL, LBL("Lc"), NONE(), NONE()),
      I(MIR_CMPBR, LBL("Ld"), V(2), V(0)),
      I(MIR_ADD, V(1), V(1), V(2)),
      I(MIR_ADD, V(2), V(2), IMM(1)),
      I(MIR_JMP, LBL("Lc"), NONE(), NONE()),
      I(MIR_LABEL, LBL("Ld"), NONE(), NONE()),
      I(MIR_RET, NONE(), V(1), NONE()),
  };
  seq[3].cc = 0x8F; /* JG -> GT */
  arm64_mir_encode_vregs(e, seq, sizeof(seq) / sizeof(seq[0]), 3, 1);
}

/* poly(a,b) = a*a + b: v0=a, v1=b (params), v2=tmp. */
static void body_vmir_poly(Arm64Emit *e) {
  MirInst seq[] = {
      I(MIR_IMUL, V(2), V(0), V(0)),
      I(MIR_ADD, V(2), V(2), V(1)),
      I(MIR_RET, NONE(), V(2), NONE()),
  };
  arm64_mir_encode_vregs(e, seq, sizeof(seq) / sizeof(seq[0]), 3, 2);
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

  /* Brick 3: the same shapes, but lowered from real MIR instructions. */
  build_case(out_dir, manifest, "mir_add", 12, 5, 7, 2, body_mir_add);
  build_case(out_dir, manifest, "mir_sum", 55, 10, 0, 1, body_mir_sum);
  build_case(out_dir, manifest, "mir_isgt", 1, 20, 7, 2, body_mir_isgt);
  build_case(out_dir, manifest, "mir_pack", 35, 2, 3, 2, body_mir_pack);
  build_case(out_dir, manifest, "mir_div", 3, 17, 5, 2, body_mir_div);
  build_case(out_dir, manifest, "mir_mod", 2, 17, 5, 2, body_mir_mod);
  build_case(out_dir, manifest, "mir_max", 20, 7, 20, 2, body_mir_max);
  build_case(out_dir, manifest, "mir_uxtb", 255, 0x1FF, 0, 1, body_mir_uxtb);
  build_case(out_dir, manifest, "mir_iseven", 1, 6, 0, 1, body_mir_iseven);

  /* Brick 4: vreg MIR with encoder-driven stack allocation + param homing. */
  build_case(out_dir, manifest, "vmir_sum", 55, 10, 0, 1, body_vmir_sum);
  build_case(out_dir, manifest, "vmir_poly", 32, 5, 7, 2, body_vmir_poly);

  if (manifest) {
    fclose(manifest);
  }
  printf("\n%s\n", g_fail ? "RESULT: FAIL" : "RESULT: PASS");
  return g_fail ? 1 : 0;
}
