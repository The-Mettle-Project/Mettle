#include "codegen/asm/x86_asm.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int failures = 0;

static void check(int bits, const char *text, const char *expected_hex) {
  X86AsmConfig config;
  X86AsmResult result;
  char error[256];
  int line = 0;
  char got[512];
  size_t i;
  memset(&config, 0, sizeof(config));
  config.bits = bits;
  config.allow_bits_directive = 1;
  error[0] = 0;
  if (!x86_asm_assemble(text, &config, &result, error, sizeof(error), &line)) {
    printf("FAIL [%d] %-38s -> error line %d: %s\n", bits, text, line, error);
    failures++;
    return;
  }
  got[0] = 0;
  for (i = 0; i < result.size; i++) {
    sprintf(got + strlen(got), "%02x", result.code[i]);
  }
  if (strcmp(got, expected_hex) != 0) {
    printf("FAIL [%d] %-38s -> got %s want %s\n", bits, text, got, expected_hex);
    failures++;
  }
  x86_asm_result_destroy(&result);
}


static void check_narrow_frame_shapes(void) {
  check(16, "push bp", "55");
  check(16, "mov bp, sp", "89e5");
  check(16, "sub sp, 12", "83ec0c");
  check(16, "mov ax, word ptr [bp - 2]", "8b46fe");
  check(16, "mov word ptr [bp - 2], ax", "8946fe");
  check(16, "mov bx, word ptr [bp + 4]", "8b5e04");
  check(16, "lea ax, [bp - 4]", "8d46fc");
  check(16, "mov [bx], ax", "8907");
  check(16, "mov [bx + 4], ax", "894704");
  check(16, "movzx ax, byte ptr [bx]", "0fb607");
  check(16, "cwd", "99");
  check(16, "idiv bx", "f7fb");
  check(16, "mul bx", "f7e3");
  check(16, "mov cx, 4", "b90400");
  check(16, "dec cx", "49");
  check(16, "iret", "cf");
  check(16, "push ds", "1e");
  check(16, "pop es", "07");
  check(16, "mov sp, bp", "89ec");
  check(16, "pop bp", "5d");

  check(32, "push ebp", "55");
  check(32, "mov ebp, esp", "89e5");
  check(32, "sub esp, 12", "83ec0c");
  check(32, "mov eax, dword ptr [ebp - 4]", "8b45fc");
  check(32, "mov dword ptr [ebp - 4], eax", "8945fc");
  check(32, "mov ebx, dword ptr [ebp + 8]", "8b5d08");
  check(32, "lea eax, [ebp - 8]", "8d45f8");
  check(32, "mov [ebx], eax", "8903");
  check(32, "mov [ebx + 4], eax", "894304");
  check(32, "movzx eax, byte ptr [ebx]", "0fb603");
  check(32, "movsx eax, word ptr [ebx]", "0fbf03");
  check(32, "cdq", "99");
  check(32, "idiv ebx", "f7fb");
  check(32, "mul ebx", "f7e3");
  check(32, "mov ecx, 4", "b904000000");
  check(32, "dec ecx", "49");
  check(32, "iretd", "cf");
  check(32, "push ds", "1e");
  check(32, "pop es", "07");
  check(32, "mov esp, ebp", "89ec");
  check(32, "pop ebp", "5d");
  check(32, "add ebx, 4", "83c304");
}

static void check_systems_extensions(void) {
  check(64, "bswap eax", "0fc8");
  check(64, "bswap rax", "480fc8");
  check(64, "bswap r8", "490fc8");
  check(32, "bswap ecx", "0fc9");
  check(64, "shld rax, rbx, 4", "480fa4d804");
  check(64, "shrd rax, rbx, 4", "480facd804");
  check(64, "shld rax, rbx, cl", "480fa5d8");
  check(64, "shrd rax, rbx, cl", "480fadd8");
  check(32, "shld eax, ebx, 1", "0fa4d801");
  check(64, "fxsave [rax]", "0fae00");
  check(64, "fxrstor [rax]", "0fae08");
  check(64, "xsave [rax]", "0fae20");
  check(64, "xrstor [rax]", "0fae28");
  check(64, "clflush [rax]", "0fae38");
  check(64, "prefetchnta [rax]", "0f1800");
  check(64, "prefetcht0 [rax]", "0f1808");
  check(64, "prefetcht1 [rax]", "0f1810");
  check(64, "prefetcht2 [rax]", "0f1818");
  check(64, "cmpxchg16b [rax]", "480fc708");
  check(32, "cmpxchg8b [eax]", "0fc708");
  check(32, "arpl ax, bx", "6663d8");
  check(64, "lar eax, ebx", "0f02c3");
  check(64, "lsl eax, ebx", "0f03c3");
  check(64, "movbe eax, [rbx]", "0f38f003");
  check(64, "movbe [rbx], eax", "0f38f103");
  check(64, "nop dword ptr [rax]", "0f1f00");
  check(64, "endbr64", "f30f1efa");
  check(32, "endbr32", "f30f1efb");

}

static void accepts(int bits, const char *text) {
  X86AsmConfig config;
  X86AsmResult result;
  char error[256];
  int line = 0;
  memset(&config, 0, sizeof(config));
  config.bits = bits;
  config.allow_bits_directive = 1;
  error[0] = 0;
  if (!x86_asm_assemble(text, &config, &result, error, sizeof(error), &line)) {
    printf("REJECTED [%d] %-34s -> %s\n", bits, text, error);
    failures++;
    return;
  }
  x86_asm_result_destroy(&result);
}

static void check_documented_instructions(void) {
  const char *sixty_four[] = {
    "sgdt [rax]", "sidt [rax]", "lgdt [rax]", "lidt [rax]", "smsw ax",
    "lmsw ax", "invlpg [rax]", "lldt ax", "sldt ax", "ltr ax", "str ax",
    "verr ax", "verw ax", "clts", "invd", "wbinvd", "rdpmc", "swapgs",
    "xgetbv", "xsetbv", "monitor", "mwait", "pause", "emms", "mfence",
    "lfence", "sfence", "rdtscp", "sysenter", "sysexit", "syscall", "sysret",
    "cpuid", "rdmsr", "wrmsr", "rdtsc", "ud2", "int3", "into", "sahf", "lahf",
    "xlatb", "leave", "cmc", "stc", "clc", "std", "cld", "hlt", "cli", "sti",
    "movsb", "movsq", "stosb", "stosq", "lodsb", "scasb", "cmpsb", "insb",
    "outsb", "rep movsb", "repne scasb", "lock inc dword ptr [rax]",
    "bt rax, 1", "bts rax, 1", "btr rax, 1", "btc rax, 1", "bsf rax, rbx",
    "bsr rax, rbx", "popcnt rax, rbx", "lzcnt rax, rbx", "tzcnt rax, rbx",
    "xadd [rax], rbx", "cmpxchg [rax], rbx", "movsxd rax, ecx",
    "cmovz rax, rbx", "setz al", "enter 16, 0", "ret 8", "retf", "retf 4",
    "in al, 0x60", "out 0x60, al", "in eax, dx", "out dx, eax",
    "mov rax, cr0", "mov cr0, rax", "mov rax, dr7", "mov dr7, rax",
    "movups xmm0, [rax]", "movups [rax], xmm0", "movdqa xmm1, xmm2",
    "movdqu [rax], xmm1", "addsd xmm0, xmm1", "sqrtss xmm0, xmm1",
    "ucomisd xmm0, xmm1", "cvtsi2sd xmm0, rax", "cvttsd2si rax, xmm0",
    "pxor xmm0, xmm0", "paddd xmm0, xmm1", "movd eax, xmm0", "movq rax, xmm0",
    "rol rax, 3", "ror rax, cl", "rcl al, 1", "rcr al, 1", "sal rax, 2",
    "neg rax", "not rax", "mul rbx", "div rbx", "idiv rbx", "imul rbx",
    "align 16", "db 1, 2, 3", "dw 1", "dd 1", "dq 1", "resb 4",
    "a: jrcxz a", "a: loope a", "a: loopne a", "jmp qword ptr [rax]",
    "call qword ptr [rax]", "mov rax, fs:[0x30]", "lea rax, [rip + 8]",
    "bswap eax", "bswap rax", "shld rax, rbx, 4", "shrd rax, rbx, cl",
    "movbe eax, [rbx]", "cmpxchg16b [rax]", "fxsave [rax]", "fxrstor [rax]",
    "xsave [rax]", "xrstor [rax]", "clflush [rax]", "prefetchnta [rax]",
    "prefetcht0 [rax]", "prefetcht1 [rax]", "prefetcht2 [rax]",
    "lar eax, ebx", "lsl eax, ebx", "endbr64",
  };
  const char *sixteen[] = {
    "jmp 0x08:0x7c00", "call 0x08:0x1234", "iret", "pusha", "popa",
    "mov al, [bx + si + 4]", "mov ax, [bp + di]", "times 4 db 0x90",
    "bits 32", "bits 16", "in al, dx", "cbw", "cwd",
    "arpl ax, bx", "endbr32", "cmpxchg8b [bx]",
  };
  size_t i;
  for (i = 0; i < sizeof(sixty_four) / sizeof(sixty_four[0]); i++) {
    accepts(64, sixty_four[i]);
  }
  for (i = 0; i < sizeof(sixteen) / sizeof(sixteen[0]); i++) {
    accepts(16, sixteen[i]);
  }
}

int main(void) {
  check_narrow_frame_shapes();
  check_systems_extensions();
  check(64, "nop", "90");
  check(64, "ret", "c3");
  check(64, "mov rax, rbx", "4889d8");
  check(64, "mov eax, ebx", "89d8");
  check(64, "mov ax, bx", "6689d8");
  check(64, "mov al, bl", "88d8");
  check(64, "mov rax, 1", "48c7c001000000");
  check(64, "mov eax, 1", "b801000000");
  check(64, "movabs rax, 0x123456789abcdef0", "48b8f0debc9a78563412");
  check(64, "mov r8, r9", "4d89c8");
  check(64, "mov r8b, 5", "41b005");
  check(64, "add rax, rbx", "4801d8");
  check(64, "add rsp, 8", "4883c408");
  check(64, "sub rsp, 0x100", "4881ec00010000");
  check(64, "xor eax, eax", "31c0");
  check(64, "cmp rax, rbx", "4839d8");
  check(64, "push rbp", "55");
  check(64, "pop rbp", "5d");
  check(64, "push r12", "4154");
  check(64, "mov rbp, rsp", "4889e5");
  check(64, "mov qword ptr [rbp - 8], rax", "488945f8");
  check(64, "mov rax, [rbp - 8]", "488b45f8");
  check(64, "mov eax, [rsp]", "8b0424");
  check(64, "mov eax, [rsp + 16]", "8b442410");
  check(64, "mov rax, [rbx + rcx*8 + 32]", "488b44cb20");
  check(64, "lea rax, [rbx + 8]", "488d4308");
  check(64, "inc qword ptr [rax]", "48ff00");
  check(64, "neg rax", "48f7d8");
  check(64, "imul rax, rbx", "480fafc3");
  check(64, "imul eax, ebx, 7", "6bc307");
  check(64, "shl rax, 3", "48c1e003");
  check(64, "shr eax, 1", "d1e8");
  check(64, "sar rax, cl", "48d3f8");
  check(64, "test rax, rax", "4885c0");
  check(64, "test al, 1", "a801");
  check(64, "sete al", "0f94c0");
  check(64, "cmovne rax, rbx", "480f45c3");
  check(64, "movzx eax, byte ptr [rbx]", "0fb603");
  check(64, "movsx rax, byte ptr [rbx]", "480fbe03");
  check(64, "movsxd rax, ecx", "4863c1");
  check(64, "cqo", "4899");
  check(64, "cdq", "99");
  check(64, "leave", "c9");
  check(64, "hlt", "f4");
  check(64, "cli", "fa");
  check(64, "pushfq", "9c");
  check(64, "popfq", "9d");
  check(64, "pushf", "669c");
  check(32, "pushfd", "9c");
  check(32, "pushf", "669c");
  check(16, "pushf", "9c");
  check(16, "pushfd", "669c");
  check(16, "popfd", "669d");
  check(64, "sti", "fb");
  check(64, "cpuid", "0fa2");
  check(64, "syscall", "0f05");
  check(64, "iretq", "48cf");
  check(64, "int 0x80", "cd80");
  check(64, "int3", "cc");
  check(64, "in al, dx", "ec");
  check(64, "out dx, al", "ee");
  check(64, "out 0x20, al", "e620");
  check(64, "rep movsb", "f3a4");
  check(64, "rep stosq", "f348ab");
  check(64, "lock xadd [rax], rbx", "f0480fc118");
  check(64, "mov cr0, rax", "0f22c0");
  check(64, "mov rax, cr3", "0f20d8");
  check(64, "lgdt [rax]", "0f0110");
  check(64, "lidt [rbx]", "0f011b");
  check(64, "mov rax, gs:[16]", "65488b042510000000");
  check(64, "mov rax, [rip + tbl]\ntbl: dq 0", "488b05000000000000000000000000");
  check(64, "mov rax, [tbl]\ntbl: dq 0", "488b05000000000000000000000000");
  check(64, "mov eax, [rsp + 4]", "8b442404");

  check(64, "mov eax, [rsp + 4]", "8b442404");
  check(64, "xchg rax, rbx", "4887c3");
  check(64, "bt rax, 3", "480fbae003");
  check(64, "movaps xmm0, xmm1", "0f28c1");
  check(64, "movsd xmm0, [rax]", "f20f1000");
  check(64, "pxor xmm0, xmm0", "660fefc0");
  check(64, "movd xmm0, eax", "660f6ec0");
  check(64, "movq xmm0, rax", "66480f6ec0");

  check(64, "a: jmp a", "ebfe");
  check(64, "a: jmp short a", "ebfe");
  check(64, "a: jne a", "75fe");
  check(64, "jmp b\nb: nop", "eb0090");
  check(64, "a: loop a", "e2fe");

  check(32, "mov eax, ebx", "89d8");
  check(32, "mov ax, bx", "6689d8");
  check(32, "push eax", "50");
  check(32, "inc eax", "40");
  check(32, "mov eax, [ebx + esi*4 + 8]", "8b44b308");
  check(32, "jmp 0x08:0x1234", "ea341200000800");
  check(32, "iretd", "cf");

  check(16, "mov ax, bx", "89d8");
  check(16, "mov eax, ebx", "6689d8");
  check(16, "mov al, 0x0e", "b00e");
  check(16, "int 0x10", "cd10");
  check(16, "mov si, 0x7c00", "be007c");
  check(16, "mov al, [si]", "8a04");
  check(16, "mov al, [bx + si]", "8a00");
  check(16, "mov al, [bp + 4]", "8a4604");
  check(16, "mov word ptr [0x1234], ax", "89063412");
  check(16, "cli", "fa");
  check(16, "hlt", "f4");
  check(16, "a: jmp a", "ebfe");
  check(16, "a: jmp short a", "ebfe");
  check(16, "lgdt [0x7c00]", "0f0116007c");
  check(16, "db 0x55, 0xaa", "55aa");
  check(16, "db 'AB', 0", "414200");
  check(16, "dw 0xaa55", "55aa");
  check(16, "times 4 db 0x90", "90909090");

  check(64, "a: times 200 nop\njmp a", "9090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090e933ffffff");
  check(16, "jmp far_target\ntimes 200 nop\nfar_target: hlt", "e9c8009090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090909090f4");
  check_documented_instructions();

  printf("%s (%d failures)\n", failures ? "FAILURES" : "all encodings match",
         failures);
  return failures != 0;
}
