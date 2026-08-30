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

int main(void) {
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
  printf("%s (%d failures)\n", failures ? "FAILURES" : "all encodings match",
         failures);
  return failures != 0;
}
