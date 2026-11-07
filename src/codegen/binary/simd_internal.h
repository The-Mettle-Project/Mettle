#ifndef CODEGEN_BINARY_SIMD_INTERNAL_H
#define CODEGEN_BINARY_SIMD_INTERNAL_H

#include "codegen/binary/internal.h"

int wcs_vex3(BinaryCodeBuffer *b, int map, int pp, int len256, int w, int reg,
             int rm, int vvvv);
int wcs_avx_modrm_mem_disp(BinaryCodeBuffer *b, int reg, int base,
                           int displacement);

int wcs_sse_0f(BinaryCodeBuffer *b, unsigned char op, int dst, int src);
int wcs_sse_f2(BinaryCodeBuffer *b, unsigned char op, int dst, int src);
int wcs_avx_vpd_ymm(BinaryCodeBuffer *b, unsigned char op, int dst, int src1,
                    int src2);
int wcs_avx_vps_ymm(BinaryCodeBuffer *b, unsigned char op, int dst, int src1,
                    int src2);
int wcs_avx_0f38_ymm(BinaryCodeBuffer *b, unsigned char op, int dst, int src1,
                     int src2);

int wcs_avx_vpxor_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2);
int wcs_avx_vpsadbw_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2);
int wcs_avx_vpaddb_xmm(BinaryCodeBuffer *b, int d, int s1, int s2);
int wcs_avx_vpsubb_xmm(BinaryCodeBuffer *b, int d, int s1, int s2);
int wcs_avx_vpand_xmm(BinaryCodeBuffer *b, int d, int s1, int s2);
int wcs_avx_vpor_xmm(BinaryCodeBuffer *b, int d, int s1, int s2);
int wcs_avx_vpxor_xmm(BinaryCodeBuffer *b, int d, int s1, int s2);
int wcs_avx_vpaddd_xmm(BinaryCodeBuffer *b, int d, int s1, int s2);
int wcs_avx_vpaddq_xmm(BinaryCodeBuffer *b, int d, int s1, int s2);
int wcs_avx_vpshufd_xmm(BinaryCodeBuffer *b, int dst, int src,
                        unsigned char imm);
int wcs_avx_vpslldq_xmm(BinaryCodeBuffer *b, int dst, int src,
                        unsigned char imm);
int wcs_avx_vpmullw_xmm(BinaryCodeBuffer *b, int d, int s1, int s2);
int wcs_avx_vpunpcklbw_xmm(BinaryCodeBuffer *b, int d, int s1, int s2);
int wcs_avx_vpunpckhbw_xmm(BinaryCodeBuffer *b, int d, int s1, int s2);
int wcs_avx_vpackuswb_xmm(BinaryCodeBuffer *b, int d, int s1, int s2);
int wcs_avx_vmovdqu_xmm_mem(BinaryCodeBuffer *b, int dst, int base, int disp);
int wcs_avx_vmovdqu_mem_xmm(BinaryCodeBuffer *b, int base, int disp, int src);
int wcs_avx_vpaddq_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2);
int wcs_avx_vpaddd_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2);
int wcs_avx_vpmulld_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2);
int wcs_avx_vpmuldq_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2);
int wcs_avx_vpminsd_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2);
int wcs_avx_vpmaxsd_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2);
int wcs_avx_vpsrlq_ymm_imm(BinaryCodeBuffer *b, int dst, int src,
                           unsigned char imm);
int wcs_avx_vpshufd_ymm(BinaryCodeBuffer *b, int dst, int src,
                        unsigned char imm);
int wcs_avx_vperm2i128(BinaryCodeBuffer *b, int dst, int s1, int s2,
                       unsigned char imm);
int wcs_avx_vpbroadcastd_ymm(BinaryCodeBuffer *b, int dst, int src_xmm);
int wcs_avx_vpbroadcastd_ymm_mem(BinaryCodeBuffer *b, int dst, int base, int displacement);
int wcs_avx_vbroadcastss_ymm_mem(BinaryCodeBuffer *b, int dst, int base, int displacement);
int wcs_avx_vpmaddwd_ymm(BinaryCodeBuffer *b, int dst, int src1, int src2);
int wcs_avx_vpmovsxbw_ymm_mem(BinaryCodeBuffer *b, int dst, int base, int displacement);
int wcs_avx_vpmovzxbw_ymm_mem(BinaryCodeBuffer *b, int dst, int base, int displacement);
int wcs_avx_vpmovzxbd_xmm_mem(BinaryCodeBuffer *b, int dst, int base, int displacement);
int wcs_avx_vpmovzxbd_ymm_mem(BinaryCodeBuffer *b, int dst, int base, int displacement);
int wcs_avx_vpmovsxbd_ymm_mem(BinaryCodeBuffer *b, int dst, int base, int displacement);
int wcs_avx_vpackusdw_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2);
int wcs_avx_vpackuswb_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2);
int wcs_avx_vpermq_ymm(BinaryCodeBuffer *b, int dst, int src, unsigned char imm);
int wcs_avx_vpextrb_mem_xmm(BinaryCodeBuffer *b, int base, int displacement, int xmm);
int wcs_avx_vbroadcastsd_ymm_xmm(BinaryCodeBuffer *b, int dst, int src_xmm);
int wcs_avx_vextracti128(BinaryCodeBuffer *b, int dst_xmm, int src_ymm,
                         unsigned char lane);
int wcs_avx_vextractf128(BinaryCodeBuffer *b, int dst_xmm, int src_ymm,
                         unsigned char lane);
int wcs_avx_vzeroupper(BinaryCodeBuffer *b);

int wcs_avx_vaddpd_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2);
int wcs_avx_vmulpd_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2);
int wcs_avx_vsubpd_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2);
int wcs_avx_vdivpd_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2);
int wcs_avx_vcvtdq2pd_ymm_xmm(BinaryCodeBuffer *b, int dst, int src);
int wcs_avx_vcvttpd2dq_xmm_ymm(BinaryCodeBuffer *b, int dst, int src);
int wcs_avx_vpmovsxdq_ymm_xmm(BinaryCodeBuffer *b, int dst, int src);
int wcs_avx_vpunpcklqdq_xmm(BinaryCodeBuffer *b, int dst, int src1, int src2);
int wcs_avx_vaddsd(BinaryCodeBuffer *b, int dst, int s1, int s2);
int wcs_avx_vsubsd(BinaryCodeBuffer *b, int dst, int s1, int s2);
int wcs_avx_vmulsd(BinaryCodeBuffer *b, int dst, int s1, int s2);
int wcs_avx_vdivsd(BinaryCodeBuffer *b, int dst, int s1, int s2);
int wcs_avx_vcvtsi2sd(BinaryCodeBuffer *b, int dst, int s1, int gpr);
int wcs_avx_vmovsd_xmm_mem(BinaryCodeBuffer *b, int dst, int base, int disp);
int wcs_avx_vmovsd_mem_xmm(BinaryCodeBuffer *b, int base, int disp, int src);
int wcs_avx_vunpckhpd_xmm(BinaryCodeBuffer *b, int dst, int s1, int s2);
int wcs_avx_vaddps_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2);
int wcs_avx_vmulps_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2);
int wcs_avx_vsubps_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2);
int wcs_avx_vdivps_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2);
int wcs_avx_vminps_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2);
int wcs_avx_vmaxps_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2);
int wcs_avx_vminpd_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2);
int wcs_avx_vmaxpd_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2);
int wcs_avx_vroundps_ymm(BinaryCodeBuffer *b, int dst, int src, unsigned char imm);
int wcs_avx_vcvttps2dq_ymm(BinaryCodeBuffer *b, int dst, int src);
int wcs_avx_vcvtdq2ps_ymm(BinaryCodeBuffer *b, int dst, int src);
int wcs_avx_vcvtph2ps_xmm(BinaryCodeBuffer *b, int dst, int src);
int wcs_avx_vcvtps2ph_xmm(BinaryCodeBuffer *b, int dst, int src,
                           unsigned char imm);
int wcs_avx_vpslld_ymm_imm(BinaryCodeBuffer *b, int dst, int src, unsigned char imm);
int wcs_avx_vpsrld_ymm_imm(BinaryCodeBuffer *b, int dst, int src, unsigned char imm);
int wcs_avx_vpsrad_ymm_imm(BinaryCodeBuffer *b, int dst, int src, unsigned char imm);
int wcs_avx_vfmadd231pd_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2);
int wcs_avx_vfmadd231ps_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2);
int wcs_fmadd231sd(BinaryCodeBuffer *b, int dst, int s1, int s2);
int wcs_fmadd231ss(BinaryCodeBuffer *b, int dst, int s1, int s2);

int wcs_avx_vmovdqu_ymm_mem(BinaryCodeBuffer *b, int dst, int base, int disp);
int wcs_avx_vmovdqu_mem_ymm(BinaryCodeBuffer *b, int base, int disp, int src);
int wcs_avx_vmovups_ymm_mem(BinaryCodeBuffer *b, int dst, int base, int disp);
int wcs_avx_vmovups_mem_ymm(BinaryCodeBuffer *b, int base, int disp, int src);
int wcs_avx_vpmovsxdq_ymm_mem(BinaryCodeBuffer *b, int dst, int base, int disp);
int wcs_movsd_xmm_mem(BinaryCodeBuffer *b, int xmm, int gpr, int disp);
int wcs_movsd_mem_xmm(BinaryCodeBuffer *b, int gpr, int disp, int xmm);
int wcs_movss_xmm_mem(BinaryCodeBuffer *b, int xmm, int gpr, int disp);
int wcs_movss_mem_xmm(BinaryCodeBuffer *b, int gpr, int disp, int xmm);
int wcs_vex_movss_xmm_mem(BinaryCodeBuffer *b, int xmm, int gpr, int disp);
int wcs_vex_movss_mem_xmm(BinaryCodeBuffer *b, int gpr, int disp, int xmm);
int wcs_vex_movsd_xmm_mem(BinaryCodeBuffer *b, int xmm, int gpr, int disp);
int wcs_vex_movsd_mem_xmm(BinaryCodeBuffer *b, int gpr, int disp, int xmm);
int wcs_vex_addss_xmm_xmm(BinaryCodeBuffer *b, int dst, int src);
int wcs_vex_mulss_xmm_xmm(BinaryCodeBuffer *b, int dst, int src);
int wcs_vex_subss_xmm_xmm(BinaryCodeBuffer *b, int dst, int src);
int wcs_vex_divss_xmm_xmm(BinaryCodeBuffer *b, int dst, int src);
int wcs_vex_addsd_xmm_xmm(BinaryCodeBuffer *b, int dst, int src);
int wcs_vex_mulsd_xmm_xmm(BinaryCodeBuffer *b, int dst, int src);
int wcs_vex_subsd_xmm_xmm(BinaryCodeBuffer *b, int dst, int src);
int wcs_vex_divsd_xmm_xmm(BinaryCodeBuffer *b, int dst, int src);
int wcs_vex_movsd_xmm_xmm(BinaryCodeBuffer *b, int dst, int src);
int wcs_vex_cvtsi2sd_xmm_reg(BinaryCodeBuffer *b, int dst, int gpr);
int wcs_vex_cvtsi2ss_xmm_reg(BinaryCodeBuffer *b, int dst, int gpr);
int wcs_vex_cvttsd2si_reg_xmm(BinaryCodeBuffer *b, int gpr, int xmm);
int wcs_vex_movd_xmm_reg(BinaryCodeBuffer *b, int xmm, int gpr);
int wcs_vex_movq_xmm_reg(BinaryCodeBuffer *b, int xmm, int gpr);
int wcs_vex_movd_reg_xmm(BinaryCodeBuffer *b, int gpr, int xmm);
int wcs_vex_movq_reg_xmm(BinaryCodeBuffer *b, int gpr, int xmm);
int simd_emit_prefixed_xmm_mem_disp(BinaryCodeBuffer *b, unsigned char prefix,
                                    unsigned char opcode, int xmm, int gpr,
                                    int displacement);

int wcs_cmp_reg_reg64(BinaryCodeBuffer *b, int dst, int src);
int wcs_xor_reg_imm32(BinaryCodeBuffer *b, int gpr, uint32_t imm);
int wcs_bsf_reg_reg32(BinaryCodeBuffer *b, int dst, int src);
int wcs_movzx_reg_byte_mem(BinaryCodeBuffer *b, int gpr, int base);
int wcs_avx_vpcmpeqd_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2);
int wcs_avx_vpcmpgtd_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2);
int wcs_avx_vpcmpeqb_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2);
int wcs_avx_vpbroadcastb_ymm(BinaryCodeBuffer *b, int dst, int src_xmm);
int wcs_avx_vmovmskps_reg_ymm(BinaryCodeBuffer *b, int gpr, int ymm);
int wcs_avx_vpmovmskb_reg_ymm(BinaryCodeBuffer *b, int gpr, int ymm);
int wcs_sse42_pcmpistri_ranges_mem(BinaryCodeBuffer *b, int ranges_xmm,
                                   int base, unsigned char control);

int wcs_avx_vmovd_xmm_reg(BinaryCodeBuffer *b, int xmm, int gpr);
int wcs_avx_vmovd_xmm_mem(BinaryCodeBuffer *b, int dst, int base, int disp);
int wcs_avx_vmovd_mem_xmm(BinaryCodeBuffer *b, int base, int disp, int src);
int wcs_avx_vpsubd_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2);
int wcs_avx_vpand_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2);
int wcs_avx_vpor_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2);
int wcs_broadcast_i32_to_ymm(BinaryCodeBuffer *b, int ymm, int gpr);
int wcs_reduce_ymm_i32_sum_to_rax(BinaryCodeBuffer *b, int src);
int wcs_reduce_pd_acc_to_rax(BinaryCodeBuffer *b);
int wcs_reduce_ps_acc_to_rax(BinaryCodeBuffer *b);
int wcs_reduce_pd_minmax_to_rax(BinaryCodeBuffer *b, int is_max);
int wcs_reduce_ps_minmax_to_rax(BinaryCodeBuffer *b, int is_max);
int wcs_reduce_ymm_i32_minmax_to_rax(BinaryCodeBuffer *b, int is_max);
int wcs_horizontal_pminsd_to_reg(BinaryCodeBuffer *b, int xmm, int gpr);
int wcs_horizontal_pmaxsd_to_reg(BinaryCodeBuffer *b, int xmm, int gpr);

#endif
