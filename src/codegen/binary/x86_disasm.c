#include "x86_disasm.h"

#include <stdio.h>
#include <string.h>

typedef struct {
  const unsigned char *p;
  size_t len;
  size_t at;
  long long address;
  int rex_w;
  int rex_r;
  int rex_x;
  int rex_b;
  int has_rex;
  int opsize16;
  int rep;
  int vex;
  int vex_l;
  int vex_v;
  int map;
  int pp;
  int has_modrm;
  int mod;
  int reg;
  int rm;
  int base;
  int index;
  int scale;
  long long disp;
  int rip;
  int no_base;
  int imm8_read;
  int imm8;
} Dx;

typedef struct {
  unsigned char map;
  unsigned char pp;
  unsigned char op;
  signed char w;
  const char *name;
  const char *form;
  unsigned char vex_only;
} DxVecOp;

static const char *const GP64[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp",
                                     "rsi", "rdi", "r8",  "r9",  "r10", "r11",
                                     "r12", "r13", "r14", "r15"};
static const char *const GP32[16] = {
    "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi",
    "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d"};
static const char *const GP16[16] = {
    "ax",  "cx",  "dx",   "bx",   "sp",   "bp",   "si",   "di",
    "r8w", "r9w", "r10w", "r11w", "r12w", "r13w", "r14w", "r15w"};
static const char *const GP8_REX[16] = {
    "al",  "cl",  "dl",   "bl",   "spl",  "bpl",  "sil",  "dil",
    "r8b", "r9b", "r10b", "r11b", "r12b", "r13b", "r14b", "r15b"};
static const char *const GP8_LEGACY[8] = {"al", "cl", "dl", "bl",
                                          "ah", "ch", "dh", "bh"};
static const char *const CONDITIONS[16] = {"o",  "no", "b",  "ae", "e",  "ne",
                                           "be", "a",  "s",  "ns", "p",  "np",
                                           "l",  "ge", "le", "g"};
static const char *const ALU[8] = {"add", "or",  "adc", "sbb",
                                   "and", "sub", "xor", "cmp"};
static const char *const SHIFTS[8] = {"rol", "ror", "rcl", "rcr",
                                      "shl", "shr", "sal", "sar"};

static const DxVecOp VEC_OPS[] = {
    {1, 0, 0x10, -1, "movups", "V,W", 0},
    {1, 0, 0x11, -1, "movups", "W,V", 0},
    {1, 0, 0x14, -1, "unpcklps", "V,H,W", 0},
    {1, 0, 0x15, -1, "unpckhps", "V,H,W", 0},
    {1, 0, 0x28, -1, "movaps", "V,W", 0},
    {1, 0, 0x29, -1, "movaps", "W,V", 0},
    {1, 0, 0x2E, -1, "ucomiss", "v,w", 0},
    {1, 0, 0x2F, -1, "comiss", "v,w", 0},
    {1, 0, 0x50, -1, "movmskps", "g,W", 0},
    {1, 0, 0x51, -1, "sqrtps", "V,W", 0},
    {1, 0, 0x52, -1, "rsqrtps", "V,W", 0},
    {1, 0, 0x53, -1, "rcpps", "V,W", 0},
    {1, 0, 0x54, -1, "andps", "V,H,W", 0},
    {1, 0, 0x55, -1, "andnps", "V,H,W", 0},
    {1, 0, 0x56, -1, "orps", "V,H,W", 0},
    {1, 0, 0x57, -1, "xorps", "V,H,W", 0},
    {1, 0, 0x58, -1, "addps", "V,H,W", 0},
    {1, 0, 0x59, -1, "mulps", "V,H,W", 0},
    {1, 0, 0x5A, -1, "cvtps2pd", "V,w", 0},
    {1, 0, 0x5B, -1, "cvtdq2ps", "V,W", 0},
    {1, 0, 0x5C, -1, "subps", "V,H,W", 0},
    {1, 0, 0x5D, -1, "minps", "V,H,W", 0},
    {1, 0, 0x5E, -1, "divps", "V,H,W", 0},
    {1, 0, 0x5F, -1, "maxps", "V,H,W", 0},
    {1, 0, 0xC2, -1, "cmpps", "V,H,W,I", 0},
    {1, 0, 0xC6, -1, "shufps", "V,H,W,I", 0},
    {1, 1, 0x10, -1, "movupd", "V,W", 0},
    {1, 1, 0x11, -1, "movupd", "W,V", 0},
    {1, 1, 0x14, -1, "unpcklpd", "V,H,W", 0},
    {1, 1, 0x15, -1, "unpckhpd", "V,H,W", 0},
    {1, 1, 0x28, -1, "movapd", "V,W", 0},
    {1, 1, 0x29, -1, "movapd", "W,V", 0},
    {1, 1, 0x2E, -1, "ucomisd", "v,w", 0},
    {1, 1, 0x2F, -1, "comisd", "v,w", 0},
    {1, 1, 0x50, -1, "movmskpd", "g,W", 0},
    {1, 1, 0x51, -1, "sqrtpd", "V,W", 0},
    {1, 1, 0x54, -1, "andpd", "V,H,W", 0},
    {1, 1, 0x55, -1, "andnpd", "V,H,W", 0},
    {1, 1, 0x56, -1, "orpd", "V,H,W", 0},
    {1, 1, 0x57, -1, "xorpd", "V,H,W", 0},
    {1, 1, 0x58, -1, "addpd", "V,H,W", 0},
    {1, 1, 0x59, -1, "mulpd", "V,H,W", 0},
    {1, 1, 0x5A, -1, "cvtpd2ps", "v,W", 0},
    {1, 1, 0x5B, -1, "cvtps2dq", "V,W", 0},
    {1, 1, 0x5C, -1, "subpd", "V,H,W", 0},
    {1, 1, 0x5D, -1, "minpd", "V,H,W", 0},
    {1, 1, 0x5E, -1, "divpd", "V,H,W", 0},
    {1, 1, 0x5F, -1, "maxpd", "V,H,W", 0},
    {1, 1, 0x60, -1, "punpcklbw", "V,H,W", 0},
    {1, 1, 0x61, -1, "punpcklwd", "V,H,W", 0},
    {1, 1, 0x62, -1, "punpckldq", "V,H,W", 0},
    {1, 1, 0x63, -1, "packsswb", "V,H,W", 0},
    {1, 1, 0x64, -1, "pcmpgtb", "V,H,W", 0},
    {1, 1, 0x65, -1, "pcmpgtw", "V,H,W", 0},
    {1, 1, 0x66, -1, "pcmpgtd", "V,H,W", 0},
    {1, 1, 0x67, -1, "packuswb", "V,H,W", 0},
    {1, 1, 0x68, -1, "punpckhbw", "V,H,W", 0},
    {1, 1, 0x69, -1, "punpckhwd", "V,H,W", 0},
    {1, 1, 0x6A, -1, "punpckhdq", "V,H,W", 0},
    {1, 1, 0x6B, -1, "packssdw", "V,H,W", 0},
    {1, 1, 0x6C, -1, "punpcklqdq", "V,H,W", 0},
    {1, 1, 0x6D, -1, "punpckhqdq", "V,H,W", 0},
    {1, 1, 0x6E, 0, "movd", "v,e", 0},
    {1, 1, 0x6E, 1, "movq", "v,E", 0},
    {1, 1, 0x6F, -1, "movdqa", "V,W", 0},
    {1, 1, 0x70, -1, "pshufd", "V,W,I", 0},
    {1, 1, 0x74, -1, "pcmpeqb", "V,H,W", 0},
    {1, 1, 0x75, -1, "pcmpeqw", "V,H,W", 0},
    {1, 1, 0x76, -1, "pcmpeqd", "V,H,W", 0},
    {1, 1, 0x7C, -1, "haddpd", "V,H,W", 0},
    {1, 1, 0x7E, 0, "movd", "e,v", 0},
    {1, 1, 0x7E, 1, "movq", "E,v", 0},
    {1, 1, 0x7F, -1, "movdqa", "W,V", 0},
    {1, 1, 0xC2, -1, "cmppd", "V,H,W,I", 0},
    {1, 1, 0xC6, -1, "shufpd", "V,H,W,I", 0},
    {1, 1, 0xD1, -1, "psrlw", "V,H,w", 0},
    {1, 1, 0xD2, -1, "psrld", "V,H,w", 0},
    {1, 1, 0xD3, -1, "psrlq", "V,H,w", 0},
    {1, 1, 0xD4, -1, "paddq", "V,H,W", 0},
    {1, 1, 0xD5, -1, "pmullw", "V,H,W", 0},
    {1, 1, 0xD6, -1, "movq", "w,v", 0},
    {1, 1, 0xD7, -1, "pmovmskb", "g,W", 0},
    {1, 1, 0xD8, -1, "psubusb", "V,H,W", 0},
    {1, 1, 0xD9, -1, "psubusw", "V,H,W", 0},
    {1, 1, 0xDA, -1, "pminub", "V,H,W", 0},
    {1, 1, 0xDB, -1, "pand", "V,H,W", 0},
    {1, 1, 0xDC, -1, "paddusb", "V,H,W", 0},
    {1, 1, 0xDD, -1, "paddusw", "V,H,W", 0},
    {1, 1, 0xDE, -1, "pmaxub", "V,H,W", 0},
    {1, 1, 0xDF, -1, "pandn", "V,H,W", 0},
    {1, 1, 0xE0, -1, "pavgb", "V,H,W", 0},
    {1, 1, 0xE1, -1, "psraw", "V,H,w", 0},
    {1, 1, 0xE2, -1, "psrad", "V,H,w", 0},
    {1, 1, 0xE3, -1, "pavgw", "V,H,W", 0},
    {1, 1, 0xE4, -1, "pmulhuw", "V,H,W", 0},
    {1, 1, 0xE5, -1, "pmulhw", "V,H,W", 0},
    {1, 1, 0xE6, -1, "cvttpd2dq", "v,W", 0},
    {1, 1, 0xE7, -1, "movntdq", "W,V", 0},
    {1, 1, 0xE8, -1, "psubsb", "V,H,W", 0},
    {1, 1, 0xE9, -1, "psubsw", "V,H,W", 0},
    {1, 1, 0xEA, -1, "pminsw", "V,H,W", 0},
    {1, 1, 0xEB, -1, "por", "V,H,W", 0},
    {1, 1, 0xEC, -1, "paddsb", "V,H,W", 0},
    {1, 1, 0xED, -1, "paddsw", "V,H,W", 0},
    {1, 1, 0xEE, -1, "pmaxsw", "V,H,W", 0},
    {1, 1, 0xEF, -1, "pxor", "V,H,W", 0},
    {1, 1, 0xF1, -1, "psllw", "V,H,w", 0},
    {1, 1, 0xF2, -1, "pslld", "V,H,w", 0},
    {1, 1, 0xF3, -1, "psllq", "V,H,w", 0},
    {1, 1, 0xF4, -1, "pmuludq", "V,H,W", 0},
    {1, 1, 0xF5, -1, "pmaddwd", "V,H,W", 0},
    {1, 1, 0xF6, -1, "psadbw", "V,H,W", 0},
    {1, 1, 0xF8, -1, "psubb", "V,H,W", 0},
    {1, 1, 0xF9, -1, "psubw", "V,H,W", 0},
    {1, 1, 0xFA, -1, "psubd", "V,H,W", 0},
    {1, 1, 0xFB, -1, "psubq", "V,H,W", 0},
    {1, 1, 0xFC, -1, "paddb", "V,H,W", 0},
    {1, 1, 0xFD, -1, "paddw", "V,H,W", 0},
    {1, 1, 0xFE, -1, "paddd", "V,H,W", 0},
    {1, 2, 0x2A, 0, "cvtsi2ss", "v,h,e", 0},
    {1, 2, 0x2A, 1, "cvtsi2ss", "v,h,E", 0},
    {1, 2, 0x2C, -1, "cvttss2si", "G,w", 0},
    {1, 2, 0x2D, -1, "cvtss2si", "G,w", 0},
    {1, 2, 0x51, -1, "sqrtss", "v,h,w", 0},
    {1, 2, 0x58, -1, "addss", "v,h,w", 0},
    {1, 2, 0x59, -1, "mulss", "v,h,w", 0},
    {1, 2, 0x5A, -1, "cvtss2sd", "v,h,w", 0},
    {1, 2, 0x5B, -1, "cvttps2dq", "V,W", 0},
    {1, 2, 0x5C, -1, "subss", "v,h,w", 0},
    {1, 2, 0x5D, -1, "minss", "v,h,w", 0},
    {1, 2, 0x5E, -1, "divss", "v,h,w", 0},
    {1, 2, 0x5F, -1, "maxss", "v,h,w", 0},
    {1, 2, 0x6F, -1, "movdqu", "V,W", 0},
    {1, 2, 0x70, -1, "pshufhw", "V,W,I", 0},
    {1, 2, 0x7E, -1, "movq", "v,w", 0},
    {1, 2, 0x7F, -1, "movdqu", "W,V", 0},
    {1, 2, 0xE6, -1, "cvtdq2pd", "V,w", 0},
    {1, 3, 0x2A, 0, "cvtsi2sd", "v,h,e", 0},
    {1, 3, 0x2A, 1, "cvtsi2sd", "v,h,E", 0},
    {1, 3, 0x2C, -1, "cvttsd2si", "G,w", 0},
    {1, 3, 0x2D, -1, "cvtsd2si", "G,w", 0},
    {1, 3, 0x51, -1, "sqrtsd", "v,h,w", 0},
    {1, 3, 0x58, -1, "addsd", "v,h,w", 0},
    {1, 3, 0x59, -1, "mulsd", "v,h,w", 0},
    {1, 3, 0x5A, -1, "cvtsd2ss", "v,h,w", 0},
    {1, 3, 0x5C, -1, "subsd", "v,h,w", 0},
    {1, 3, 0x5D, -1, "minsd", "v,h,w", 0},
    {1, 3, 0x5E, -1, "divsd", "v,h,w", 0},
    {1, 3, 0x5F, -1, "maxsd", "v,h,w", 0},
    {1, 3, 0x12, -1, "movddup", "V,W", 0},
    {1, 2, 0x12, -1, "movsldup", "V,W", 0},
    {1, 2, 0x16, -1, "movshdup", "V,W", 0},
    {1, 3, 0x70, -1, "pshuflw", "V,W,I", 0},
    {1, 3, 0x7C, -1, "haddps", "V,H,W", 0},
    {1, 3, 0xE6, -1, "cvtpd2dq", "v,W", 0},
    {2, 1, 0x00, -1, "pshufb", "V,H,W", 0},
    {2, 1, 0x01, -1, "phaddw", "V,H,W", 0},
    {2, 1, 0x02, -1, "phaddd", "V,H,W", 0},
    {2, 1, 0x04, -1, "pmaddubsw", "V,H,W", 0},
    {2, 1, 0x0B, -1, "pmulhrsw", "V,H,W", 0},
    {2, 1, 0x13, -1, "cvtph2ps", "V,w", 1},
    {2, 1, 0x16, -1, "permps", "V,H,W", 1},
    {2, 1, 0x17, -1, "ptest", "V,W", 0},
    {2, 1, 0x18, -1, "broadcastss", "V,w", 1},
    {2, 1, 0x19, -1, "broadcastsd", "V,w", 1},
    {2, 1, 0x1A, -1, "broadcastf128", "V,w", 1},
    {2, 1, 0x1C, -1, "pabsb", "V,W", 0},
    {2, 1, 0x1D, -1, "pabsw", "V,W", 0},
    {2, 1, 0x1E, -1, "pabsd", "V,W", 0},
    {2, 1, 0x20, -1, "pmovsxbw", "V,w", 0},
    {2, 1, 0x21, -1, "pmovsxbd", "V,w", 0},
    {2, 1, 0x22, -1, "pmovsxbq", "V,w", 0},
    {2, 1, 0x23, -1, "pmovsxwd", "V,w", 0},
    {2, 1, 0x24, -1, "pmovsxwq", "V,w", 0},
    {2, 1, 0x25, -1, "pmovsxdq", "V,w", 0},
    {2, 1, 0x28, -1, "pmuldq", "V,H,W", 0},
    {2, 1, 0x29, -1, "pcmpeqq", "V,H,W", 0},
    {2, 1, 0x2B, -1, "packusdw", "V,H,W", 0},
    {2, 1, 0x30, -1, "pmovzxbw", "V,w", 0},
    {2, 1, 0x31, -1, "pmovzxbd", "V,w", 0},
    {2, 1, 0x32, -1, "pmovzxbq", "V,w", 0},
    {2, 1, 0x33, -1, "pmovzxwd", "V,w", 0},
    {2, 1, 0x34, -1, "pmovzxwq", "V,w", 0},
    {2, 1, 0x35, -1, "pmovzxdq", "V,w", 0},
    {2, 1, 0x36, -1, "permd", "V,H,W", 1},
    {2, 1, 0x37, -1, "pcmpgtq", "V,H,W", 0},
    {2, 1, 0x38, -1, "pminsb", "V,H,W", 0},
    {2, 1, 0x39, -1, "pminsd", "V,H,W", 0},
    {2, 1, 0x3A, -1, "pminuw", "V,H,W", 0},
    {2, 1, 0x3B, -1, "pminud", "V,H,W", 0},
    {2, 1, 0x3C, -1, "pmaxsb", "V,H,W", 0},
    {2, 1, 0x3D, -1, "pmaxsd", "V,H,W", 0},
    {2, 1, 0x3E, -1, "pmaxuw", "V,H,W", 0},
    {2, 1, 0x3F, -1, "pmaxud", "V,H,W", 0},
    {2, 1, 0x40, -1, "pmulld", "V,H,W", 0},
    {2, 1, 0x45, 0, "psrlvd", "V,H,W", 1},
    {2, 1, 0x45, 1, "psrlvq", "V,H,W", 1},
    {2, 1, 0x46, 0, "psravd", "V,H,W", 1},
    {2, 1, 0x47, 0, "psllvd", "V,H,W", 1},
    {2, 1, 0x47, 1, "psllvq", "V,H,W", 1},
    {2, 1, 0x58, -1, "pbroadcastd", "V,w", 1},
    {2, 1, 0x59, -1, "pbroadcastq", "V,w", 1},
    {2, 1, 0x5A, -1, "broadcasti128", "V,w", 1},
    {2, 1, 0x78, -1, "pbroadcastb", "V,w", 1},
    {2, 1, 0x79, -1, "pbroadcastw", "V,w", 1},
    {3, 1, 0x00, -1, "permq", "V,W,I", 1},
    {3, 1, 0x01, -1, "permpd", "V,W,I", 1},
    {3, 1, 0x02, -1, "pblendd", "V,H,W,I", 1},
    {3, 1, 0x04, -1, "permilps", "V,W,I", 1},
    {3, 1, 0x05, -1, "permilpd", "V,W,I", 1},
    {3, 1, 0x06, -1, "perm2f128", "V,H,W,I", 1},
    {3, 1, 0x08, -1, "roundps", "V,W,I", 0},
    {3, 1, 0x09, -1, "roundpd", "V,W,I", 0},
    {3, 1, 0x0A, -1, "roundss", "v,h,w,I", 0},
    {3, 1, 0x0B, -1, "roundsd", "v,h,w,I", 0},
    {3, 1, 0x0C, -1, "blendps", "V,H,W,I", 0},
    {3, 1, 0x0D, -1, "blendpd", "V,H,W,I", 0},
    {3, 1, 0x0E, -1, "pblendw", "V,H,W,I", 0},
    {3, 1, 0x0F, -1, "palignr", "V,H,W,I", 0},
    {3, 1, 0x14, -1, "pextrb", "b,v,I", 0},
    {3, 1, 0x15, -1, "pextrw", "e,v,I", 0},
    {3, 1, 0x16, 0, "pextrd", "e,v,I", 0},
    {3, 1, 0x16, 1, "pextrq", "E,v,I", 0},
    {3, 1, 0x17, -1, "extractps", "e,v,I", 0},
    {3, 1, 0x18, -1, "insertf128", "V,H,w,I", 1},
    {3, 1, 0x19, -1, "extractf128", "w,V,I", 1},
    {3, 1, 0x1D, -1, "cvtps2ph", "w,V,I", 1},
    {3, 1, 0x20, -1, "pinsrb", "v,h,e,I", 0},
    {3, 1, 0x21, -1, "insertps", "v,h,w,I", 0},
    {3, 1, 0x22, 0, "pinsrd", "v,h,e,I", 0},
    {3, 1, 0x22, 1, "pinsrq", "v,h,E,I", 0},
    {3, 1, 0x38, -1, "inserti128", "V,H,w,I", 1},
    {3, 1, 0x39, -1, "extracti128", "w,V,I", 1},
    {3, 1, 0x40, -1, "dpps", "V,H,W,I", 0},
    {3, 1, 0x42, -1, "mpsadbw", "V,H,W,I", 0},
    {3, 1, 0x44, -1, "pclmulqdq", "V,H,W,I", 0},
    {3, 1, 0x46, -1, "perm2i128", "V,H,W,I", 1},
    {3, 1, 0x4A, -1, "blendvps", "V,H,W,L", 1},
    {3, 1, 0x4B, -1, "blendvpd", "V,H,W,L", 1},
    {3, 1, 0x4C, -1, "pblendvb", "V,H,W,L", 1},
    {3, 1, 0x60, -1, "pcmpestrm", "v,w,I", 0},
    {3, 1, 0x61, -1, "pcmpestri", "v,w,I", 0},
    {3, 1, 0x62, -1, "pcmpistrm", "v,w,I", 0},
    {3, 1, 0x63, -1, "pcmpistri", "v,w,I", 0},
};

static const char *const FMA_KIND[10] = {
    "fmaddsub", "fmsubadd", "fmadd",  "fmadd",  "fmsub",
    "fmsub",    "fnmadd",   "fnmadd", "fnmsub", "fnmsub"};

static int dx_next(Dx *d, unsigned char *out) {
  if (d->at >= d->len) {
    return 0;
  }
  *out = d->p[d->at++];
  return 1;
}

static int dx_imm(Dx *d, int size, long long *out) {
  unsigned long long value = 0;
  if (d->at + (size_t)size > d->len) {
    return 0;
  }
  for (int i = 0; i < size; i++) {
    value |= (unsigned long long)d->p[d->at + (size_t)i] << (8 * i);
  }
  d->at += (size_t)size;
  if (size < 8) {
    unsigned long long sign = 1ULL << (size * 8 - 1);
    value = (value ^ sign) - sign;
  }
  *out = (long long)value;
  return 1;
}

static int dx_modrm(Dx *d) {
  unsigned char m;
  if (!dx_next(d, &m)) {
    return 0;
  }
  d->has_modrm = 1;
  d->mod = m >> 6;
  d->reg = ((m >> 3) & 7) | (d->rex_r << 3);
  d->rm = m & 7;
  d->base = -1;
  d->index = -1;
  d->scale = 1;
  d->disp = 0;
  d->rip = 0;
  d->no_base = 0;
  if (d->mod == 3) {
    d->rm |= d->rex_b << 3;
    return 1;
  }
  if (d->rm == 4) {
    unsigned char sib;
    int index;
    if (!dx_next(d, &sib)) {
      return 0;
    }
    d->scale = 1 << (sib >> 6);
    index = ((sib >> 3) & 7) | (d->rex_x << 3);
    d->index = index == 4 ? -1 : index;
    if ((sib & 7) == 5 && d->mod == 0) {
      d->no_base = 1;
      return dx_imm(d, 4, &d->disp);
    }
    d->base = (sib & 7) | (d->rex_b << 3);
  } else if (d->rm == 5 && d->mod == 0) {
    d->rip = 1;
    return dx_imm(d, 4, &d->disp);
  } else {
    d->base = d->rm | (d->rex_b << 3);
  }
  if (d->mod == 1) {
    return dx_imm(d, 1, &d->disp);
  }
  if (d->mod == 2) {
    return dx_imm(d, 4, &d->disp);
  }
  return 1;
}

static void dx_mem(const Dx *d, const char *size, char *out, size_t cap) {
  size_t n = 0;
  int first = 1;
  if (size) {
    n += (size_t)snprintf(out + n, cap - n, "%s ptr ", size);
  }
  n += (size_t)snprintf(out + n, cap - n, "[");
  if (d->rip) {
    n += (size_t)snprintf(out + n, cap - n, "rip");
    first = 0;
  } else if (!d->no_base && d->base >= 0) {
    n += (size_t)snprintf(out + n, cap - n, "%s", GP64[d->base]);
    first = 0;
  }
  if (d->index >= 0 && n < cap) {
    n += (size_t)snprintf(out + n, cap - n, "%s%s", first ? "" : "+",
                          GP64[d->index]);
    if (d->scale > 1 && n < cap) {
      n += (size_t)snprintf(out + n, cap - n, "*%d", d->scale);
    }
    first = 0;
  }
  if (n < cap && (d->disp != 0 || first)) {
    if (first) {
      n += (size_t)snprintf(out + n, cap - n, "%lld", d->disp);
    } else if (d->disp < 0) {
      n += (size_t)snprintf(out + n, cap - n, "-%lld", -d->disp);
    } else {
      n += (size_t)snprintf(out + n, cap - n, "+%lld", d->disp);
    }
  }
  if (n < cap) {
    snprintf(out + n, cap - n, "]");
  }
}

static const char *dx_gp(const Dx *d, int reg, int bytes) {
  if (bytes == 8) {
    return GP64[reg & 15];
  }
  if (bytes == 4) {
    return GP32[reg & 15];
  }
  if (bytes == 2) {
    return GP16[reg & 15];
  }
  if (d->has_rex || reg >= 8) {
    return GP8_REX[reg & 15];
  }
  return GP8_LEGACY[reg & 7];
}

static void dx_vreg(int reg, int wide, char *out, size_t cap) {
  snprintf(out, cap, "%s%d", wide ? "ymm" : "xmm", reg & 15);
}

static const char *dx_size_name(int bytes) {
  switch (bytes) {
  case 1:
    return "byte";
  case 2:
    return "word";
  case 4:
    return "dword";
  default:
    return "qword";
  }
}

static void dx_rm_gp(const Dx *d, int bytes, int sized, char *out,
                     size_t cap) {
  if (d->mod == 3) {
    snprintf(out, cap, "%s", dx_gp(d, d->rm, bytes));
  } else {
    dx_mem(d, sized ? dx_size_name(bytes) : NULL, out, cap);
  }
}

static int dx_opsize(const Dx *d) {
  return d->rex_w ? 8 : (d->opsize16 ? 2 : 4);
}

static int dx_emit(X86DisasmInsn *out, const char *mnemonic,
                   char operands[][64], int count) {
  size_t n;
  snprintf(out->mnemonic, sizeof(out->mnemonic), "%s", mnemonic);
  n = (size_t)snprintf(out->text, sizeof(out->text), "%s", mnemonic);
  for (int i = 0; i < count && n < sizeof(out->text); i++) {
    n += (size_t)snprintf(out->text + n, sizeof(out->text) - n, "%s%s",
                          i ? ", " : " ", operands[i]);
  }
  return 1;
}

static int dx_vector_form(Dx *d, const char *name, const char *form,
                          X86DisasmInsn *out) {
  char ops[5][64];
  int count = 0;
  char mnemonic[24];
  int wide = d->vex && d->vex_l;
  snprintf(mnemonic, sizeof(mnemonic), "%s%s", d->vex ? "v" : "", name);
  if (!d->has_modrm && !dx_modrm(d)) {
    return 0;
  }
  for (const char *f = form; *f; f++) {
    char *slot;
    if (*f == ',') {
      continue;
    }
    if (count >= 5) {
      return 0;
    }
    slot = ops[count];
    switch (*f) {
    case 'V':
      dx_vreg(d->reg, wide, slot, 64);
      break;
    case 'v':
      dx_vreg(d->reg, 0, slot, 64);
      break;
    case 'H':
      if (!d->vex) {
        continue;
      }
      dx_vreg(d->vex_v, wide, slot, 64);
      break;
    case 'h':
      if (!d->vex) {
        continue;
      }
      dx_vreg(d->vex_v, 0, slot, 64);
      break;
    case 'W':
      if (d->mod == 3) {
        dx_vreg(d->rm, wide, slot, 64);
      } else {
        dx_mem(d, NULL, slot, 64);
      }
      break;
    case 'w':
      if (d->mod == 3) {
        dx_vreg(d->rm, 0, slot, 64);
      } else {
        dx_mem(d, NULL, slot, 64);
      }
      break;
    case 'G':
      snprintf(slot, 64, "%s", dx_gp(d, d->reg, d->rex_w ? 8 : 4));
      break;
    case 'g':
      snprintf(slot, 64, "%s", dx_gp(d, d->reg, 4));
      break;
    case 'E':
      dx_rm_gp(d, 8, 0, slot, 64);
      break;
    case 'e':
      dx_rm_gp(d, 4, 0, slot, 64);
      break;
    case 'b':
      dx_rm_gp(d, d->mod == 3 ? 4 : 1, d->mod != 3, slot, 64);
      break;
    case 'I': {
      long long imm;
      if (!dx_imm(d, 1, &imm)) {
        return 0;
      }
      snprintf(slot, 64, "%lld", imm & 0xFF);
      break;
    }
    case 'L': {
      long long imm;
      if (!dx_imm(d, 1, &imm)) {
        return 0;
      }
      dx_vreg((int)((imm >> 4) & 15), wide, slot, 64);
      break;
    }
    default:
      return 0;
    }
    count++;
  }
  out->is_vector = 1;
  return dx_emit(out, mnemonic, ops, count);
}

static int dx_vector_shift_group(Dx *d, unsigned char op, X86DisasmInsn *out) {
  static const char *const G71[8] = {NULL, NULL, "psrlw", NULL,
                                     "psraw", NULL, "psllw", NULL};
  static const char *const G72[8] = {NULL, NULL, "psrld", NULL,
                                     "psrad", NULL, "pslld", NULL};
  static const char *const G73[8] = {NULL,  NULL, "psrlq", "psrldq",
                                     NULL, NULL, "psllq", "pslldq"};
  const char *name;
  char ops[3][64];
  char mnemonic[24];
  int count = 0;
  int wide = d->vex && d->vex_l;
  long long imm;
  if (!dx_modrm(d) || d->mod != 3) {
    return 0;
  }
  name = op == 0x71 ? G71[d->reg & 7]
         : op == 0x72 ? G72[d->reg & 7]
                      : G73[d->reg & 7];
  if (!name || !dx_imm(d, 1, &imm)) {
    return 0;
  }
  snprintf(mnemonic, sizeof(mnemonic), "%s%s", d->vex ? "v" : "", name);
  if (d->vex) {
    dx_vreg(d->vex_v, wide, ops[count++], 64);
  }
  dx_vreg(d->rm, wide, ops[count++], 64);
  snprintf(ops[count++], 64, "%lld", imm & 0xFF);
  out->is_vector = 1;
  return dx_emit(out, mnemonic, ops, count);
}

static int dx_fma(Dx *d, unsigned char op, X86DisasmInsn *out) {
  static const char *const ORDER[3] = {"132", "213", "231"};
  char name[24];
  int row = (op >> 4) - 9;
  int column = op & 15;
  int scalar = column >= 8 && (column & 1);
  const char *kind;
  if (row < 0 || row > 2 || column < 6) {
    return 0;
  }
  kind = FMA_KIND[column - 6];
  snprintf(name, sizeof(name), "%s%s%s%s", kind, ORDER[row],
           scalar ? "s" : "p", d->rex_w ? "d" : "s");
  if (scalar) {
    return dx_vector_form(d, name, "v,h,w", out);
  }
  return dx_vector_form(d, name, "V,H,W", out);
}

static int dx_scalar_move(Dx *d, unsigned char op, X86DisasmInsn *out) {
  const char *name = d->pp == 2 ? "movss" : "movsd";
  if (!dx_modrm(d)) {
    return 0;
  }
  if (d->mod != 3) {
    return dx_vector_form(d, name, op == 0x10 ? "v,w" : "w,v", out);
  }
  return dx_vector_form(d, name, op == 0x10 ? "v,h,w" : "w,h,v", out);
}

static int dx_vector(Dx *d, unsigned char op, X86DisasmInsn *out) {
  if (d->map == 1 && (op == 0x10 || op == 0x11) && (d->pp == 2 || d->pp == 3)) {
    return dx_scalar_move(d, op, out);
  }
  if (d->map == 1 && d->pp == 1 && (op == 0x71 || op == 0x72 || op == 0x73)) {
    return dx_vector_shift_group(d, op, out);
  }
  if (d->map == 2 && d->pp == 1 && op >= 0x96 && op <= 0xBF) {
    return dx_fma(d, op, out);
  }
  for (size_t i = 0; i < sizeof(VEC_OPS) / sizeof(VEC_OPS[0]); i++) {
    const DxVecOp *v = &VEC_OPS[i];
    if (v->map != d->map || v->pp != d->pp || v->op != op ||
        (v->w >= 0 && v->w != d->rex_w) || (v->vex_only && !d->vex)) {
      continue;
    }
    return dx_vector_form(d, v->name, v->form, out);
  }
  return 0;
}

static int dx_branch(Dx *d, const char *mnemonic, int size,
                     X86DisasmInsn *out) {
  long long rel;
  char ops[1][64];
  if (!dx_imm(d, size, &rel)) {
    return 0;
  }
  snprintf(ops[0], 64, "0x%llx",
           (unsigned long long)(d->address + (long long)d->at + rel));
  out->is_branch = 1;
  return dx_emit(out, mnemonic, ops, 1);
}

static int dx_gp_two(Dx *d, const char *mnemonic, int reg_first, int bytes,
                     X86DisasmInsn *out) {
  char ops[2][64];
  if (!dx_modrm(d)) {
    return 0;
  }
  snprintf(ops[reg_first ? 0 : 1], 64, "%s", dx_gp(d, d->reg, bytes));
  dx_rm_gp(d, bytes, 0, ops[reg_first ? 1 : 0], 64);
  return dx_emit(out, mnemonic, ops, 2);
}

static int dx_gp_imm(Dx *d, const char *mnemonic, int bytes, int imm_size,
                     X86DisasmInsn *out) {
  char ops[2][64];
  long long imm;
  dx_rm_gp(d, bytes, 1, ops[0], 64);
  if (!dx_imm(d, imm_size, &imm)) {
    return 0;
  }
  snprintf(ops[1], 64, "%lld", imm);
  return dx_emit(out, mnemonic, ops, 2);
}

static int dx_extend(Dx *d, const char *mnemonic, int from,
                     X86DisasmInsn *out) {
  char ops[2][64];
  if (!dx_modrm(d)) {
    return 0;
  }
  snprintf(ops[0], 64, "%s", dx_gp(d, d->reg, dx_opsize(d)));
  dx_rm_gp(d, from, 1, ops[1], 64);
  return dx_emit(out, mnemonic, ops, 2);
}

static int dx_two_byte_gp(Dx *d, unsigned char op, X86DisasmInsn *out) {
  char ops[2][64];
  if (op == 0x05) {
    return dx_emit(out, "syscall", ops, 0);
  }
  if (op == 0x0B) {
    return dx_emit(out, "ud2", ops, 0);
  }
  if (op == 0xA2) {
    return dx_emit(out, "cpuid", ops, 0);
  }
  if (op == 0x01) {
    static const char *const TABLES[4] = {"sgdt", "sidt", "lgdt", "lidt"};
    if (!dx_modrm(d) || d->mod == 3 || (d->reg & 7) > 3) {
      return 0;
    }
    dx_mem(d, NULL, ops[0], 64);
    return dx_emit(out, TABLES[d->reg & 7], ops, 1);
  }
  if (op == 0x1F) {
    if (!dx_modrm(d)) {
      return 0;
    }
    return dx_emit(out, "nop", ops, 0);
  }
  if (op == 0x18) {
    static const char *const HINTS[4] = {"prefetchnta", "prefetcht0",
                                         "prefetcht1", "prefetcht2"};
    if (!dx_modrm(d) || d->mod == 3 || (d->reg & 7) > 3) {
      return 0;
    }
    dx_mem(d, "byte", ops[0], 64);
    return dx_emit(out, HINTS[d->reg & 7], ops, 1);
  }
  if (op >= 0x40 && op <= 0x4F) {
    char name[16];
    snprintf(name, sizeof(name), "cmov%s", CONDITIONS[op & 15]);
    return dx_gp_two(d, name, 1, dx_opsize(d), out);
  }
  if (op >= 0x80 && op <= 0x8F) {
    char name[16];
    snprintf(name, sizeof(name), "j%s", CONDITIONS[op & 15]);
    return dx_branch(d, name, 4, out);
  }
  if (op >= 0x90 && op <= 0x9F) {
    char name[16];
    snprintf(name, sizeof(name), "set%s", CONDITIONS[op & 15]);
    if (!dx_modrm(d)) {
      return 0;
    }
    dx_rm_gp(d, 1, 1, ops[0], 64);
    return dx_emit(out, name, ops, 1);
  }
  switch (op) {
  case 0xAF:
    return dx_gp_two(d, "imul", 1, dx_opsize(d), out);
  case 0xB6:
    return dx_extend(d, "movzx", 1, out);
  case 0xB7:
    return dx_extend(d, "movzx", 2, out);
  case 0xBE:
    return dx_extend(d, "movsx", 1, out);
  case 0xBF:
    return dx_extend(d, "movsx", 2, out);
  case 0xA3:
    return dx_gp_two(d, "bt", 0, dx_opsize(d), out);
  case 0xB8:
    return d->rep == 0xF3 ? dx_gp_two(d, "popcnt", 1, dx_opsize(d), out) : 0;
  case 0xBC:
    return dx_gp_two(d, d->rep == 0xF3 ? "tzcnt" : "bsf", 1, dx_opsize(d), out);
  case 0xBD:
    return dx_gp_two(d, d->rep == 0xF3 ? "lzcnt" : "bsr", 1, dx_opsize(d), out);
  default:
    return 0;
  }
}

static int dx_group1(Dx *d, unsigned char op, X86DisasmInsn *out) {
  int bytes = op == 0x80 ? 1 : dx_opsize(d);
  int imm_size = op == 0x81 ? (d->opsize16 ? 2 : 4) : 1;
  if (!dx_modrm(d)) {
    return 0;
  }
  return dx_gp_imm(d, ALU[d->reg & 7], bytes, imm_size, out);
}

static int dx_group2(Dx *d, unsigned char op, X86DisasmInsn *out) {
  char ops[2][64];
  int bytes = (op & 1) ? dx_opsize(d) : 1;
  if (!dx_modrm(d)) {
    return 0;
  }
  dx_rm_gp(d, bytes, 1, ops[0], 64);
  if (op == 0xC0 || op == 0xC1) {
    long long imm;
    if (!dx_imm(d, 1, &imm)) {
      return 0;
    }
    snprintf(ops[1], 64, "%lld", imm & 0xFF);
  } else if (op == 0xD0 || op == 0xD1) {
    snprintf(ops[1], 64, "1");
  } else {
    snprintf(ops[1], 64, "cl");
  }
  return dx_emit(out, SHIFTS[d->reg & 7], ops, 2);
}

static int dx_group3(Dx *d, unsigned char op, X86DisasmInsn *out) {
  static const char *const NAMES[8] = {"test", "test", "not",  "neg",
                                       "mul",  "imul", "div", "idiv"};
  char ops[1][64];
  int bytes = op == 0xF6 ? 1 : dx_opsize(d);
  if (!dx_modrm(d)) {
    return 0;
  }
  if ((d->reg & 7) < 2) {
    return dx_gp_imm(d, "test", bytes,
                     op == 0xF6 ? 1 : (d->opsize16 ? 2 : 4), out);
  }
  dx_rm_gp(d, bytes, 1, ops[0], 64);
  return dx_emit(out, NAMES[d->reg & 7], ops, 1);
}

static int dx_group5(Dx *d, unsigned char op, X86DisasmInsn *out) {
  static const char *const NAMES[8] = {"inc", "dec", "call", NULL,
                                       "jmp", NULL,  "push", NULL};
  char ops[1][64];
  int kind;
  if (!dx_modrm(d)) {
    return 0;
  }
  kind = d->reg & 7;
  if (op == 0xFE) {
    if (kind > 1) {
      return 0;
    }
    dx_rm_gp(d, 1, 1, ops[0], 64);
    return dx_emit(out, NAMES[kind], ops, 1);
  }
  if (!NAMES[kind]) {
    return 0;
  }
  dx_rm_gp(d, kind >= 2 ? 8 : dx_opsize(d), 1, ops[0], 64);
  out->is_branch = kind == 2 || kind == 4;
  return dx_emit(out, NAMES[kind], ops, 1);
}

static int dx_reg_forms(Dx *d, unsigned char op, X86DisasmInsn *out) {
  char ops[2][64];
  long long imm;
  if (op < 0x40 && (op & 7) < 6) {
    const char *name = ALU[op >> 3];
    int form = op & 7;
    int bytes = (form & 1) ? dx_opsize(d) : 1;
    if (form < 4) {
      return dx_gp_two(d, name, form >= 2, bytes, out);
    }
    snprintf(ops[0], 64, "%s", dx_gp(d, 0, bytes));
    if (!dx_imm(d, form == 4 ? 1 : (d->opsize16 ? 2 : 4), &imm)) {
      return 0;
    }
    snprintf(ops[1], 64, "%lld", imm);
    return dx_emit(out, name, ops, 2);
  }
  if (op >= 0x50 && op <= 0x57) {
    snprintf(ops[0], 64, "%s", GP64[(op & 7) | (d->rex_b << 3)]);
    return dx_emit(out, "push", ops, 1);
  }
  if (op >= 0x58 && op <= 0x5F) {
    snprintf(ops[0], 64, "%s", GP64[(op & 7) | (d->rex_b << 3)]);
    return dx_emit(out, "pop", ops, 1);
  }
  if (op >= 0x70 && op <= 0x7F) {
    char name[16];
    snprintf(name, sizeof(name), "j%s", CONDITIONS[op & 15]);
    return dx_branch(d, name, 1, out);
  }
  if (op >= 0xB0 && op <= 0xB7) {
    snprintf(ops[0], 64, "%s", dx_gp(d, (op & 7) | (d->rex_b << 3), 1));
    if (!dx_imm(d, 1, &imm)) {
      return 0;
    }
    snprintf(ops[1], 64, "%lld", imm);
    return dx_emit(out, "mov", ops, 2);
  }
  if (op >= 0xB8 && op <= 0xBF) {
    int bytes = dx_opsize(d);
    snprintf(ops[0], 64, "%s", dx_gp(d, (op & 7) | (d->rex_b << 3), bytes));
    if (!dx_imm(d, bytes == 8 ? 8 : (bytes == 2 ? 2 : 4), &imm)) {
      return 0;
    }
    if (bytes == 4) {
      imm &= 0xFFFFFFFFLL;
    }
    snprintf(ops[1], 64, "%lld", imm);
    return dx_emit(out, bytes == 8 ? "movabs" : "mov", ops, 2);
  }
  return -1;
}

static int dx_io_forms(Dx *d, unsigned char op, X86DisasmInsn *out) {
  char ops[2][64];
  long long imm;
  if (op >= 0xA4 && op <= 0xAF && op != 0xA8 && op != 0xA9) {
    static const char *const STRINGS[12] = {"movs", "movs", "cmps", "cmps",
                                            NULL,   NULL,   "stos", "stos",
                                            "lods", "lods", "scas", "scas"};
    const char *base = STRINGS[op - 0xA4];
    char name[24];
    char suffix = (op & 1) ? (d->rex_w ? 'q' : d->opsize16 ? 'w' : 'd') : 'b';
    snprintf(name, sizeof(name), "%s%s%c",
             d->rep == 0xF3 ? (op == 0xA6 || op == 0xA7 || op == 0xAE ||
                                       op == 0xAF
                                   ? "repe "
                                   : "rep ")
             : d->rep == 0xF2 ? "repne "
                              : "",
             base, suffix);
    return dx_emit(out, name, ops, 0);
  }
  if (op >= 0x6C && op <= 0x6F) {
    char name[24];
    char suffix = (op & 1) ? (d->opsize16 ? 'w' : 'd') : 'b';
    snprintf(name, sizeof(name), "%s%s%c", d->rep ? "rep " : "",
             op < 0x6E ? "ins" : "outs", suffix);
    return dx_emit(out, name, ops, 0);
  }
  if (op == 0xCD) {
    if (!dx_imm(d, 1, &imm)) {
      return 0;
    }
    snprintf(ops[0], 64, "0x%llx", (unsigned long long)(imm & 0xFF));
    return dx_emit(out, "int", ops, 1);
  }
  if (op == 0xE4 || op == 0xE5 || op == 0xEC || op == 0xED) {
    int bytes = op & 1 ? (d->opsize16 ? 2 : 4) : 1;
    snprintf(ops[0], 64, "%s", dx_gp(d, 0, bytes));
    if (op < 0xEC) {
      if (!dx_imm(d, 1, &imm)) {
        return 0;
      }
      snprintf(ops[1], 64, "%lld", imm & 0xFF);
    } else {
      snprintf(ops[1], 64, "dx");
    }
    return dx_emit(out, "in", ops, 2);
  }
  if (op == 0xE6 || op == 0xE7 || op == 0xEE || op == 0xEF) {
    int bytes = op & 1 ? (d->opsize16 ? 2 : 4) : 1;
    if (op < 0xEE) {
      if (!dx_imm(d, 1, &imm)) {
        return 0;
      }
      snprintf(ops[0], 64, "%lld", imm & 0xFF);
    } else {
      snprintf(ops[0], 64, "dx");
    }
    snprintf(ops[1], 64, "%s", dx_gp(d, 0, bytes));
    return dx_emit(out, "out", ops, 2);
  }
  return -1;
}

static int dx_plain(Dx *d, unsigned char op, X86DisasmInsn *out) {
  char ops[1][64];
  long long imm;
  switch (op) {
  case 0x9C:
    return dx_emit(out, "pushfq", ops, 0);
  case 0x9D:
    return dx_emit(out, "popfq", ops, 0);
  case 0xCF:
    out->is_branch = 1;
    return dx_emit(out, d->rex_w ? "iretq" : "iretd", ops, 0);
  case 0xF4:
    return dx_emit(out, "hlt", ops, 0);
  case 0xFA:
    return dx_emit(out, "cli", ops, 0);
  case 0xFB:
    return dx_emit(out, "sti", ops, 0);
  case 0xFC:
    return dx_emit(out, "cld", ops, 0);
  case 0xFD:
    return dx_emit(out, "std", ops, 0);
  case 0x90:
    return dx_emit(out, d->rep == 0xF3 ? "pause" : "nop", ops, 0);
  case 0x98:
    return dx_emit(out, d->rex_w ? "cdqe" : (d->opsize16 ? "cbw" : "cwde"),
                   ops, 0);
  case 0x99:
    return dx_emit(out, d->rex_w ? "cqo" : (d->opsize16 ? "cwd" : "cdq"), ops,
                   0);
  case 0xC2:
    if (!dx_imm(d, 2, &imm)) {
      return 0;
    }
    snprintf(ops[0], 64, "%lld", imm & 0xFFFF);
    out->is_branch = 1;
    return dx_emit(out, "ret", ops, 1);
  case 0xC3:
    out->is_branch = 1;
    return dx_emit(out, "ret", ops, 0);
  case 0xCC:
    return dx_emit(out, "int3", ops, 0);
  default:
    return -1;
  }
}

static int dx_operand_ops(Dx *d, unsigned char op, X86DisasmInsn *out) {
  char ops[2][64];
  long long imm;
  switch (op) {
  case 0x63:
    return dx_extend(d, "movsxd", 4, out);
  case 0x68:
  case 0x6A:
    if (!dx_imm(d, op == 0x68 ? 4 : 1, &imm)) {
      return 0;
    }
    snprintf(ops[0], 64, "%lld", imm);
    return dx_emit(out, "push", ops, 1);
  case 0x69:
  case 0x6B: {
    char three[3][64];
    if (!dx_modrm(d)) {
      return 0;
    }
    snprintf(three[0], 64, "%s", dx_gp(d, d->reg, dx_opsize(d)));
    dx_rm_gp(d, dx_opsize(d), 0, three[1], 64);
    if (!dx_imm(d, op == 0x6B ? 1 : (d->opsize16 ? 2 : 4), &imm)) {
      return 0;
    }
    snprintf(three[2], 64, "%lld", imm);
    return dx_emit(out, "imul", three, 3);
  }
  case 0x80:
  case 0x81:
  case 0x83:
    return dx_group1(d, op, out);
  case 0x84:
  case 0x85:
    return dx_gp_two(d, "test", 0, op == 0x84 ? 1 : dx_opsize(d), out);
  case 0x86:
  case 0x87:
    return dx_gp_two(d, "xchg", 0, op == 0x86 ? 1 : dx_opsize(d), out);
  case 0x88:
  case 0x89:
    return dx_gp_two(d, "mov", 0, op == 0x88 ? 1 : dx_opsize(d), out);
  case 0x8A:
  case 0x8B:
    return dx_gp_two(d, "mov", 1, op == 0x8A ? 1 : dx_opsize(d), out);
  case 0x8D:
    if (!dx_modrm(d) || d->mod == 3) {
      return 0;
    }
    snprintf(ops[0], 64, "%s", dx_gp(d, d->reg, dx_opsize(d)));
    dx_mem(d, NULL, ops[1], 64);
    return dx_emit(out, "lea", ops, 2);
  default:
    return -1;
  }
}

static int dx_operand_high(Dx *d, unsigned char op, X86DisasmInsn *out) {
  char ops[2][64];
  long long imm;
  switch (op) {
  case 0xA8:
  case 0xA9: {
    int bytes = op == 0xA8 ? 1 : dx_opsize(d);
    snprintf(ops[0], 64, "%s", dx_gp(d, 0, bytes));
    if (!dx_imm(d, op == 0xA8 ? 1 : (d->opsize16 ? 2 : 4), &imm)) {
      return 0;
    }
    snprintf(ops[1], 64, "%lld", imm);
    return dx_emit(out, "test", ops, 2);
  }
  case 0xC0:
  case 0xC1:
  case 0xD0:
  case 0xD1:
  case 0xD2:
  case 0xD3:
    return dx_group2(d, op, out);
  case 0xC6:
  case 0xC7:
    if (!dx_modrm(d) || (d->reg & 7) != 0) {
      return 0;
    }
    return dx_gp_imm(d, "mov", op == 0xC6 ? 1 : dx_opsize(d),
                     op == 0xC6 ? 1 : (d->opsize16 ? 2 : 4), out);
  case 0xE8:
    return dx_branch(d, "call", 4, out);
  case 0xE9:
    return dx_branch(d, "jmp", 4, out);
  case 0xEB:
    return dx_branch(d, "jmp", 1, out);
  case 0xF6:
  case 0xF7:
    return dx_group3(d, op, out);
  case 0xFE:
  case 0xFF:
    return dx_group5(d, op, out);
  default:
    return 0;
  }
}

static int dx_one_byte(Dx *d, unsigned char op, X86DisasmInsn *out) {
  int decoded = dx_reg_forms(d, op, out);
  if (decoded < 0) {
    decoded = dx_io_forms(d, op, out);
  }
  if (decoded < 0) {
    decoded = dx_plain(d, op, out);
  }
  if (decoded < 0) {
    decoded = dx_operand_ops(d, op, out);
  }
  if (decoded < 0) {
    decoded = dx_operand_high(d, op, out);
  }
  return decoded;
}

static int dx_vex_prefix(Dx *d, unsigned char first) {
  unsigned char b1;
  unsigned char b2;
  if (!dx_next(d, &b1)) {
    return 0;
  }
  d->vex = 1;
  if (first == 0xC5) {
    d->rex_r = (b1 & 0x80) ? 0 : 1;
    d->vex_v = (~b1 >> 3) & 15;
    d->vex_l = (b1 >> 2) & 1;
    d->pp = b1 & 3;
    d->map = 1;
    return 1;
  }
  if (!dx_next(d, &b2)) {
    return 0;
  }
  d->rex_r = (b1 & 0x80) ? 0 : 1;
  d->rex_x = (b1 & 0x40) ? 0 : 1;
  d->rex_b = (b1 & 0x20) ? 0 : 1;
  d->map = b1 & 31;
  d->rex_w = (b2 >> 7) & 1;
  d->vex_v = (~b2 >> 3) & 15;
  d->vex_l = (b2 >> 2) & 1;
  d->pp = b2 & 3;
  return d->map >= 1 && d->map <= 3;
}

int x86_disasm_one(const unsigned char *bytes, size_t length,
                   long long address, X86DisasmInsn *out) {
  Dx d;
  unsigned char op;
  int ok;
  memset(&d, 0, sizeof(d));
  memset(out, 0, sizeof(*out));
  d.p = bytes;
  d.len = length;
  d.address = address;
  for (;;) {
    unsigned char b;
    if (d.at >= d.len) {
      return 0;
    }
    b = d.p[d.at];
    if (b == 0x66) {
      d.opsize16 = 1;
    } else if (b == 0xF2 || b == 0xF3) {
      d.rep = b;
    } else if (b == 0x2E || b == 0x3E || b == 0x26 || b == 0x36 ||
               b == 0x64 || b == 0x65 || b == 0x67 || b == 0xF0) {
    } else {
      break;
    }
    d.at++;
  }
  if (!dx_next(&d, &op)) {
    return 0;
  }
  if (op == 0xC4 || op == 0xC5) {
    if (d.opsize16 || d.rep || !dx_vex_prefix(&d, op) || !dx_next(&d, &op)) {
      return 0;
    }
    if (d.map == 1 && op == 0x77) {
      char none[1][64];
      ok = dx_emit(out, d.vex_l ? "vzeroall" : "vzeroupper", none, 0);
      out->is_vector = 1;
    } else {
      ok = dx_vector(&d, op, out);
    }
  } else {
    if ((op & 0xF0) == 0x40) {
      d.has_rex = 1;
      d.rex_w = (op >> 3) & 1;
      d.rex_r = (op >> 2) & 1;
      d.rex_x = (op >> 1) & 1;
      d.rex_b = op & 1;
      if (!dx_next(&d, &op)) {
        return 0;
      }
    }
    if (op == 0x0F) {
      unsigned char op2;
      if (!dx_next(&d, &op2)) {
        return 0;
      }
      d.pp = d.rep == 0xF3 ? 2 : d.rep == 0xF2 ? 3 : d.opsize16 ? 1 : 0;
      if (op2 == 0x38 || op2 == 0x3A) {
        d.map = op2 == 0x38 ? 2 : 3;
        if (!dx_next(&d, &op2)) {
          return 0;
        }
        ok = dx_vector(&d, op2, out);
      } else {
        size_t mark = d.at;
        d.map = 1;
        ok = dx_two_byte_gp(&d, op2, out);
        if (!ok) {
          d.at = mark;
          d.has_modrm = 0;
          memset(out, 0, sizeof(*out));
          ok = dx_vector(&d, op2, out);
        }
      }
    } else {
      ok = dx_one_byte(&d, op, out);
    }
  }
  if (!ok || d.at > d.len) {
    memset(out, 0, sizeof(*out));
    return 0;
  }
  out->length = (int)d.at;
  return out->length;
}
