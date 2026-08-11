/* Entry code for the reference compiler itself. No platform C startup runs. */

#if defined(_WIN32)

typedef unsigned int mt_host_u32;

__declspec(dllimport) __declspec(noreturn) void __stdcall
ExitProcess(mt_host_u32 status);
int mtlc_host_rt_getmainargs(int *argc_out, char ***argv_out);
int main(int argc, char **argv);

__declspec(noreturn) void mettle_start(void) {
  int argc = 0;
  char **argv = (char **)0;
  if (!mtlc_host_rt_getmainargs(&argc, &argv)) {
    ExitProcess(127);
  }
  ExitProcess((mt_host_u32)main(argc, argv));
}

#elif defined(__x86_64__)

__asm__(".text\n"
        ".p2align 4\n"
        ".globl _start\n"
        ".type _start,@function\n"
        "_start:\n"
        "movq %rsp, %r12\n"
        "movq 0(%r12), %rdi\n"
        "leaq 8(%r12), %rsi\n"
        "call mtlc_host_rt_startup\n"
        "movq 0(%r12), %rdi\n"
        "leaq 8(%r12), %rsi\n"
        "call main\n"
        "movl %eax, %edi\n"
        "movl $60, %eax\n"
        "syscall\n"
        ".size _start, .-_start\n");

#elif defined(__aarch64__)

__asm__(".text\n"
        ".p2align 4\n"
        ".globl _start\n"
        ".type _start,%function\n"
        "_start:\n"
        "mov x19, sp\n"
        "ldr x0, [x19]\n"
        "add x1, x19, #8\n"
        "bl mtlc_host_rt_startup\n"
        "ldr x0, [x19]\n"
        "add x1, x19, #8\n"
        "bl main\n"
        "mov x8, #93\n"
        "svc #0\n"
        ".size _start, .-_start\n");

#else
#error The Mettle host startup does not support this target yet
#endif
