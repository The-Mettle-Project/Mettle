

#include "codegen/binary/arm64.h"

#include <stdio.h>

static int g_fail = 0;
static int g_pass = 0;

static void check_word(const char *what, uint32_t got, uint32_t want) {
  if (got == want) {
    g_pass++;
  } else {
    g_fail++;
    printf("  FAIL %-28s got 0x%08X  want 0x%08X\n", what, got, want);
  }
}

static void check_int(const char *what, long long got, long long want) {
  if (got == want) {
    g_pass++;
  } else {
    g_fail++;
    printf("  FAIL %-28s got %lld  want %lld\n", what, got, want);
  }
}

static void test_known_good(void) {
  printf("known-good encodings (ARM ARM ground truth):\n");

  check_word("nop", arm64_nop(), 0xD503201Fu);
  check_word("ret x30", arm64_ret(ARM64_X30), 0xD65F03C0u);
  check_word("ret x0", arm64_ret(ARM64_X0), 0xD65F0000u);
  check_word("br x0", arm64_br(ARM64_X0), 0xD61F0000u);
  check_word("blr x0", arm64_blr(ARM64_X0), 0xD63F0000u);

  check_word("mov x0, x1", arm64_mov_reg(1, ARM64_X0, ARM64_X1), 0xAA0103E0u);
  check_word("mov x1, x0", arm64_mov_reg(1, ARM64_X1, ARM64_X0), 0xAA0003E1u);

  check_word("add x0, x1, x2",
             arm64_add_reg(1, ARM64_X0, ARM64_X1, ARM64_X2), 0x8B020020u);
  check_word("sub x0, x1, x2",
             arm64_sub_reg(1, ARM64_X0, ARM64_X1, ARM64_X2), 0xCB020020u);
  check_word("neg x0, x1", arm64_neg(1, ARM64_X0, ARM64_X1), 0xCB0103E0u);
  check_word("mvn x0, x1", arm64_mvn(1, ARM64_X0, ARM64_X1), 0xAA2103E0u);

  check_word("add x0, x0, #1",
             arm64_add_imm(1, ARM64_X0, ARM64_X0, 1, 0), 0x91000400u);
  check_word("sub sp, sp, #16",
             arm64_sub_imm(1, ARM64_SP, ARM64_SP, 16, 0), 0xD10043FFu);
  check_word("add sp, sp, #16",
             arm64_add_imm(1, ARM64_SP, ARM64_SP, 16, 0), 0x910043FFu);

  check_word("movz x0, #1", arm64_movz(1, ARM64_X0, 1, 0), 0xD2800020u);
  check_word("movz x0,#0xffff,lsl16",
             arm64_movz(1, ARM64_X0, 0xFFFF, 1), 0xD2BFFFE0u);
  check_word("movk x0, #1", arm64_movk(1, ARM64_X0, 1, 0), 0xF2800020u);
  check_word("movn x0, #0", arm64_movn(1, ARM64_X0, 0, 0), 0x92800000u);

  check_word("cmp x0, x1", arm64_cmp_reg(1, ARM64_X0, ARM64_X1), 0xEB01001Fu);
  check_word("cmp x0, #1", arm64_cmp_imm(1, ARM64_X0, 1, 0), 0xF100041Fu);
  check_word("tst x0, x1", arm64_tst(1, ARM64_X0, ARM64_X1), 0xEA01001Fu);
  check_word("uxtb w0, w0", arm64_uxtb(ARM64_X0, ARM64_X0), 0x53001C00u);
  check_word("sxtb x0, w0", arm64_sxtb(ARM64_X0, ARM64_X0), 0x93401C00u);
  check_word("sxtw x0, w0", arm64_sxtw(ARM64_X0, ARM64_X0), 0x93407C00u);

  check_word("mul x0, x1, x2",
             arm64_mul(1, ARM64_X0, ARM64_X1, ARM64_X2), 0x9B027C20u);
  check_word("smulh x0, x1, x2",
             arm64_smulh(ARM64_X0, ARM64_X1, ARM64_X2), 0x9B427C20u);
  check_word("umulh x0, x1, x2",
             arm64_umulh(ARM64_X0, ARM64_X1, ARM64_X2), 0x9BC27C20u);
  {
    struct {
      const char *text;
      uint64_t value;
      int kind;
      uint32_t word;
    } logical[] = {
        {"and x0, x1, #0x7", 0x7ull, 0, 0x92400820u},
        {"and x0, x1, #0xf", 0xFull, 0, 0x92400C20u},
        {"and x0, x1, #0xff00", 0xFF00ull, 0, 0x92781C20u},
        {"and x0, x1, #0x5555555555555555", 0x5555555555555555ull, 0,
         0x9200F020u},
        {"orr x0, x1, #0xffffffff", 0xFFFFFFFFull, 1, 0xB2407C20u},
        {"eor x0, x1, #0x8000000000000000", 0x8000000000000000ull, 2,
         0xD2410020u},
    };
    for (size_t i = 0; i < sizeof(logical) / sizeof(logical[0]); i++) {
      uint32_t fields = 0;
      if (!arm64_logical_imm(logical[i].value, &fields)) {
        check_word(logical[i].text, 0u, logical[i].word);
        continue;
      }
      uint32_t word = logical[i].kind == 0
                          ? arm64_and_imm(ARM64_X0, ARM64_X1, fields)
                      : logical[i].kind == 1
                          ? arm64_orr_imm(ARM64_X0, ARM64_X1, fields)
                          : arm64_eor_imm(ARM64_X0, ARM64_X1, fields);
      check_word(logical[i].text, word, logical[i].word);
    }
    uint32_t unused = 0;
    check_word("0x5 has no logical immediate",
               (uint32_t)arm64_logical_imm(0x5ull, &unused), 0u);
    check_word("0 has no logical immediate",
               (uint32_t)arm64_logical_imm(0ull, &unused), 0u);
  }
  check_word("msub x0,x1,x2,x3",
             arm64_msub(1, ARM64_X0, ARM64_X1, ARM64_X2, ARM64_X3),
             0x9B028C20u);
  check_word("sdiv x0, x1, x2",
             arm64_sdiv(1, ARM64_X0, ARM64_X1, ARM64_X2), 0x9AC20C20u);
  check_word("udiv x0, x1, x2",
             arm64_udiv(1, ARM64_X0, ARM64_X1, ARM64_X2), 0x9AC20820u);
  check_word("lslv x0, x1, x2",
             arm64_lslv(1, ARM64_X0, ARM64_X1, ARM64_X2), 0x9AC22020u);

  check_word("ldr x0, [x1]", arm64_ldr_imm(1, ARM64_X0, ARM64_X1, 0),
             0xF9400020u);
  check_word("ldr x0, [sp, #8]", arm64_ldr_imm(1, ARM64_X0, ARM64_SP, 8),
             0xF94007E0u);
  check_word("str x0, [x1]", arm64_str_imm(1, ARM64_X0, ARM64_X1, 0),
             0xF9000020u);
  check_word("strb w0, [x1]", arm64_strb_imm(ARM64_X0, ARM64_X1, 0),
             0x39000020u);
  check_word("ldrb w0, [x1]", arm64_ldrb_imm(ARM64_X0, ARM64_X1, 0),
             0x39400020u);
  check_word("strh w0, [x1]", arm64_strh_imm(ARM64_X0, ARM64_X1, 0),
             0x79000020u);
  check_word("ldrh w0, [x1]", arm64_ldrh_imm(ARM64_X0, ARM64_X1, 0),
             0x79400020u);

  check_word("fadd d0, d1, d2", arm64_fadd(1, 0, 1, 2), 0x1E622820u);
  check_word("fmul d0, d1, d2", arm64_fmul(1, 0, 1, 2), 0x1E620820u);
  check_word("fsub d0, d1, d2", arm64_fsub(1, 0, 1, 2), 0x1E623820u);
  check_word("fdiv d0, d1, d2", arm64_fdiv(1, 0, 1, 2), 0x1E621820u);
  check_word("fmov d0, x0", arm64_fmov_gp(1, 0, ARM64_X0), 0x9E670000u);
  check_word("fmov x0, d0", arm64_fmov_to_gp(1, ARM64_X0, 0), 0x9E660000u);
  check_word("scvtf d0, x0", arm64_scvtf(1, 0, ARM64_X0), 0x9E620000u);
  check_word("fcvtzs x0, d0", arm64_fcvtzs(1, ARM64_X0, 0), 0x9E780000u);
  check_word("fcmp d0, d1", arm64_fcmp(1, 0, 1), 0x1E612000u);
  check_word("ldr d0, [x1]", arm64_ldr_fp(1, 0, ARM64_X1, 0), 0xFD400020u);
  check_word("str d0, [x1]", arm64_str_fp(1, 0, ARM64_X1, 0), 0xFD000020u);
  check_word("str x0, [sp, #8]", arm64_str_imm(1, ARM64_X0, ARM64_SP, 8),
             0xF90007E0u);

  check_word("stp x29,x30,[sp,#-16]!",
             arm64_stp_pre(1, ARM64_X29, ARM64_X30, ARM64_SP, -16),
             0xA9BF7BFDu);
  check_word("ldp x29,x30,[sp],#16",
             arm64_ldp_post(1, ARM64_X29, ARM64_X30, ARM64_SP, 16),
             0xA8C17BFDu);

  check_word("mov x29, sp", arm64_mov_sp(ARM64_X29, ARM64_SP), 0x910003FDu);
  check_word("mov sp, x29", arm64_mov_sp(ARM64_SP, ARM64_X29), 0x910003BFu);
  check_int("mov_sp differs from orr-mov",
            arm64_mov_sp(ARM64_X29, ARM64_SP) !=
                arm64_mov_reg(1, ARM64_X29, ARM64_SP),
            1);

  check_word("cset x0, eq", arm64_cset(1, ARM64_X0, ARM64_EQ), 0x9A9F17E0u);
  check_word("csel x0,x1,x2,eq",
             arm64_csel(1, ARM64_X0, ARM64_X1, ARM64_X2, ARM64_EQ),
             0x9A820020u);

  check_word("b .", arm64_b(0), 0x14000000u);
  check_word("bl .", arm64_bl(0), 0x94000000u);
  check_word("b.eq .", arm64_bcond(ARM64_EQ, 0), 0x54000000u);
  check_word("b.ne .", arm64_bcond(ARM64_NE, 0), 0x54000001u);

  check_word("b .+8", arm64_b(8), 0x14000002u);
  check_word("b .-8", arm64_b(-8), 0x17FFFFFEu);
}

static const int kRegs[] = {0, 1, 2, 7, 15, 19, 28, 30};
static const int kNReg = (int)(sizeof(kRegs) / sizeof(kRegs[0]));

static void test_roundtrip(void) {
  printf("round-trip (encode -> decode -> compare):\n");
  for (int ai = 0; ai < kNReg; ai++) {
    for (int bi = 0; bi < kNReg; bi++) {
      int rd = kRegs[ai], rn = kRegs[bi], rm = kRegs[(ai + bi) % kNReg];

      Arm64Inst d = arm64_decode(arm64_add_reg(1, rd, rn, rm));
      check_int("add.op", d.op, ARM64_DIS_ADD_REG);
      check_int("add.rd", d.rd, rd);
      check_int("add.rn", d.rn, rn);
      check_int("add.rm", d.rm, rm);

      d = arm64_decode(arm64_sub_reg(1, rd, rn, rm));
      check_int("sub.rd", d.rd, rd);
      check_int("sub.rm", d.rm, rm);

      d = arm64_decode(arm64_orr_reg(1, rd, rn, rm));
      check_int("orr.op", (rn == 31) ? ARM64_DIS_MOV : ARM64_DIS_ORR_REG,
                d.op);

      d = arm64_decode(arm64_mul(1, rd, rn, rm));
      check_int("mul.op", d.op, ARM64_DIS_MADD);
      check_int("mul.rd", d.rd, rd);
      check_int("mul.rn", d.rn, rn);
      check_int("mul.rm", d.rm, rm);

      d = arm64_decode(arm64_sdiv(1, rd, rn, rm));
      check_int("sdiv.op", d.op, ARM64_DIS_SDIV);
      check_int("sdiv.rd", d.rd, rd);

      for (int off = 0; off <= 32760; off += 4088) {
        d = arm64_decode(arm64_ldr_imm(1, rd, rn, off));
        check_int("ldr.op", d.op, ARM64_DIS_LDR_IMM);
        check_int("ldr.rt", d.rt, rd);
        check_int("ldr.rn", d.rn, rn);
        check_int("ldr.imm", d.imm, off);

        d = arm64_decode(arm64_str_imm(1, rd, rn, off));
        check_int("str.imm", d.imm, off);
      }
    }
  }

  for (int imm = 0; imm <= 4095; imm += 273) {
    Arm64Inst d = arm64_decode(arm64_add_imm(1, ARM64_X3, ARM64_X5, imm, 0));
    check_int("addimm.op", d.op, ARM64_DIS_ADD_IMM);
    check_int("addimm.imm", d.imm, imm);
    check_int("addimm.rd", d.rd, 3);
    check_int("addimm.rn", d.rn, 5);

    d = arm64_decode(arm64_sub_imm(1, ARM64_X3, ARM64_X5, imm, 0));
    check_int("subimm.imm", d.imm, imm);
  }

  for (int hw = 0; hw < 4; hw++) {
    Arm64Inst d = arm64_decode(arm64_movz(1, ARM64_X9, 0xABCD, hw));
    check_int("movz.op", d.op, ARM64_DIS_MOVZ);
    check_int("movz.imm", d.imm, 0xABCD);
    check_int("movz.hw", d.hw, hw);
    check_int("movz.rd", d.rd, 9);
  }

  for (int s = 0; s < 64; s += 7) {
    Arm64Inst d = arm64_decode(arm64_lsl_imm(1, ARM64_X0, ARM64_X1, s));
    check_int("lsl.op", d.op, ARM64_DIS_UBFM);
    check_int("lsl.immr", d.immr, (-s) & 63);
    check_int("lsl.imms", d.imms, 63 - s);

    d = arm64_decode(arm64_lsr_imm(1, ARM64_X0, ARM64_X1, s));
    check_int("lsr.op", d.op, ARM64_DIS_UBFM);
    check_int("lsr.immr", d.immr, s);
    check_int("lsr.imms", d.imms, 63);

    d = arm64_decode(arm64_asr_imm(1, ARM64_X0, ARM64_X1, s));
    check_int("asr.op", d.op, ARM64_DIS_SBFM);
    check_int("asr.imms", d.imms, 63);
  }

  for (int c = 0; c <= 13; c++) {
    Arm64Inst d = arm64_decode(arm64_bcond((Arm64Cond)c, 0));
    check_int("bcond.op", d.op, ARM64_DIS_BCOND);
    check_int("bcond.cond", d.cond, c);

    d = arm64_decode(arm64_cset(1, ARM64_X4, (Arm64Cond)c));
    check_int("cset.op", d.op, ARM64_DIS_CSINC);
    check_int("cset.rd", d.rd, 4);

    check_int("cset.cond", d.cond, c ^ 1);
  }

  int offs[] = {0, 4, -4, 8, -8, 4096, -4096, 1 << 20, -(1 << 20)};
  for (int i = 0; i < (int)(sizeof(offs) / sizeof(offs[0])); i++) {
    Arm64Inst d = arm64_decode(arm64_b(offs[i]));
    check_int("b.off", d.imm, offs[i]);
    d = arm64_decode(arm64_bl(offs[i]));
    check_int("bl.off", d.imm, offs[i]);
  }

  int pairoffs[] = {-16, -32, -64, 16, 32, 64, 0};
  for (int i = 0; i < (int)(sizeof(pairoffs) / sizeof(pairoffs[0])); i++) {
    Arm64Inst d =
        arm64_decode(arm64_stp_pre(1, ARM64_X19, ARM64_X20, ARM64_SP,
                                   pairoffs[i]));
    check_int("stp.op", d.op, ARM64_DIS_STP);
    check_int("stp.rt", d.rt, 19);
    check_int("stp.rt2", d.rt2, 20);
    check_int("stp.imm", d.imm, pairoffs[i]);

    d = arm64_decode(
        arm64_ldp_post(1, ARM64_X19, ARM64_X20, ARM64_SP, pairoffs[i]));
    check_int("ldp.op", d.op, ARM64_DIS_LDP);
    check_int("ldp.imm", d.imm, pairoffs[i]);
  }
}

static void test_abi(void) {
  printf("AAPCS64 register-file + ABI:\n");
  const Arm64Abi *abi = arm64_aapcs64();

  check_int("gp_arg_count", abi->gp_arg_count, 8);
  check_int("vec_arg_count", abi->vec_arg_count, 8);
  check_int("gp_arg[0]", abi->gp_arg_regs[0], ARM64_X0);
  check_int("gp_arg[7]", abi->gp_arg_regs[7], ARM64_X7);
  check_int("indirect_result", abi->indirect_result_reg, ARM64_X8);
  check_int("stack_slot_bytes", abi->stack_slot_bytes, 8);

  check_int("fp", abi->fp, ARM64_X29);
  check_int("lr", abi->lr, ARM64_X30);
  check_int("sp", abi->sp, 31);
  check_int("scratch0", abi->scratch0, ARM64_X16);
  check_int("scratch1", abi->scratch1, ARM64_X17);
  check_int("platform", abi->platform, ARM64_X18);

  check_int("callee_saved_count", abi->gp_callee_saved_count, 10);
  check_int("gp_temps_count", abi->gp_temps_count, 7);
  check_int("vec_volatile_count", abi->vec_volatile_count, 8);
  check_int("vec_callee_count", abi->vec_callee_saved_count, 8);

  check_int("x0 arg-index", arm64_reg_arg_index(ARM64_X0), 0);
  check_int("x7 arg-index", arm64_reg_arg_index(ARM64_X7), 7);
  check_int("x8 arg-index", arm64_reg_arg_index(ARM64_X8), -1);
  check_int("x19 callee-saved", arm64_reg_is_callee_saved(ARM64_X19), 1);
  check_int("x28 callee-saved", arm64_reg_is_callee_saved(ARM64_X28), 1);
  check_int("x29(FP) callee-saved", arm64_reg_is_callee_saved(ARM64_X29), 1);
  check_int("x18 not callee-saved", arm64_reg_is_callee_saved(ARM64_X18), 0);
  check_int("x9 volatile", arm64_reg_is_volatile(ARM64_X9), 1);
  check_int("x19 not volatile", arm64_reg_is_volatile(ARM64_X19), 0);

  check_int("sp not allocatable", arm64_reg_is_allocatable(ARM64_SP), 0);
  check_int("fp not allocatable", arm64_reg_is_allocatable(ARM64_X29), 0);
  check_int("lr not allocatable", arm64_reg_is_allocatable(ARM64_X30), 0);
  check_int("x16 not allocatable", arm64_reg_is_allocatable(ARM64_X16), 0);
  check_int("x17 not allocatable", arm64_reg_is_allocatable(ARM64_X17), 0);
  check_int("x18 not allocatable", arm64_reg_is_allocatable(ARM64_X18), 0);
  for (int i = 0; i < abi->gp_callee_saved_count; i++) {
    check_int("callee-saved allocatable",
              arm64_reg_is_allocatable(abi->gp_callee_saved[i]), 1);
    check_int("callee-saved not volatile",
              arm64_reg_is_volatile(abi->gp_callee_saved[i]), 0);
  }
  for (int i = 0; i < abi->gp_temps_count; i++) {
    check_int("temp allocatable", arm64_reg_is_allocatable(abi->gp_temps[i]),
              1);
    check_int("temp is volatile", arm64_reg_is_volatile(abi->gp_temps[i]), 1);
    check_int("temp not an arg reg", arm64_reg_arg_index(abi->gp_temps[i]), -1);
  }

  {
    int isf[12] = {0};
    Arm64ArgLocation locs[12];
    int sb = -1;
    check_int("layout ok", arm64_compute_arg_layout(isf, 12, locs, &sb), 1);
    check_int("arg0 in x0", locs[0].reg, ARM64_X0);
    check_int("arg0 kind", locs[0].kind, ARM64_ARG_IN_GP_REGISTER);
    check_int("arg7 in x7", locs[7].reg, ARM64_X7);
    check_int("arg8 on stack", locs[8].kind, ARM64_ARG_ON_STACK);
    check_int("arg8 offset", locs[8].stack_offset, 0);
    check_int("arg9 offset", locs[9].stack_offset, 8);
    check_int("arg11 offset", locs[11].stack_offset, 24);
    check_int("stack bytes", sb, 32);
  }

  {
    int isf[6] = {0, 1, 0, 1, 0, 1};
    Arm64ArgLocation locs[6];
    int sb = -1;
    arm64_compute_arg_layout(isf, 6, locs, &sb);
    check_int("mixed a0 x0", locs[0].reg, ARM64_X0);
    check_int("mixed a1 v0", locs[1].reg, 0);
    check_int("mixed a1 kind", locs[1].kind, ARM64_ARG_IN_VEC_REGISTER);
    check_int("mixed a2 x1", locs[2].reg, ARM64_X1);
    check_int("mixed a3 v1", locs[3].reg, 1);
    check_int("mixed a4 x2", locs[4].reg, ARM64_X2);
    check_int("mixed a5 v2", locs[5].reg, 2);
    check_int("mixed no stack", sb, 0);
  }

  {
    int isf[10];
    Arm64ArgLocation locs[10];
    int sb = -1;
    for (int i = 0; i < 10; i++) isf[i] = 1;
    arm64_compute_arg_layout(isf, 10, locs, &sb);
    check_int("fp a7 v7", locs[7].reg, 7);
    check_int("fp a8 stack", locs[8].kind, ARM64_ARG_ON_STACK);
    check_int("fp stack bytes", sb, 16);
  }

  {
    int isf[10] = {0};
    int sizes[10] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 2};
    Arm64ArgLocation locs[10];
    int sb = -1;
    arm64_compute_arg_layout_packed(isf, sizes, 10, locs, &sb);
    check_int("darwin char s0 offset", locs[8].stack_offset, 0);
    check_int("darwin short s1 offset", locs[9].stack_offset, 2);
    check_int("darwin two stack args bytes", sb, 8);
  }

  {
    int isf[14] = {0};
    int sizes[14] = {8, 8, 8, 8, 8, 8, 8, 8, 1, 2, 4, 8, 1, 4};
    Arm64ArgLocation locs[14];
    int sb = -1;
    arm64_compute_arg_layout_packed(isf, sizes, 14, locs, &sb);
    check_int("darwin mixed char", locs[8].stack_offset, 0);
    check_int("darwin mixed short", locs[9].stack_offset, 2);
    check_int("darwin mixed int", locs[10].stack_offset, 4);
    check_int("darwin mixed long", locs[11].stack_offset, 8);
    check_int("darwin mixed char after long", locs[12].stack_offset, 16);
    check_int("darwin mixed int after char", locs[13].stack_offset, 20);
    check_int("darwin mixed bytes", sb, 24);
  }

  {
    int isf[10];
    int sizes[10];
    Arm64ArgLocation locs[10];
    int sb = -1;
    for (int i = 0; i < 10; i++) {
      isf[i] = 1;
      sizes[i] = 4;
    }
    arm64_compute_arg_layout_packed(isf, sizes, 10, locs, &sb);
    check_int("darwin float s0", locs[8].stack_offset, 0);
    check_int("darwin float s1", locs[9].stack_offset, 4);
    check_int("darwin float bytes", sb, 8);
    arm64_compute_arg_layout(isf, 10, locs, &sb);
    check_int("linux float s1 keeps 8-byte slots", locs[9].stack_offset, 8);
  }
}

int main(void) {
  printf("=== AArch64 encoder self-test ===\n");
  test_known_good();
  test_roundtrip();
  test_abi();
  printf("\n%d passed, %d failed\n", g_pass, g_fail);
  if (g_fail) {
    printf("RESULT: FAIL\n");
    return 1;
  }
  printf("RESULT: PASS\n");
  return 0;
}
