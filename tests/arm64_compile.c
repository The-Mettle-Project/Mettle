/* Real-source -> AArch64 compiler harness. Drives the actual Mettle frontend
 * (lexer -> parser -> type checker -> ir_lower_program), then lowers a chosen
 * function's IR to AArch64 with arm64_ir_encode_function and writes a static
 * ELF executable: [_start: bl func; exit(x0)] ++ [func body]. Running it under
 * qemu-aarch64 yields the function's return value as the process exit code.
 *
 * Usage: arm64_compile <src.mettle> <out.elf> [func-name]   (default: main) */

#include "codegen/binary/arm64_emit.h"
#include "codegen/binary/arm64_ir.h"

#include "codegen/binary_emitter.h"
#include "error/error_reporter.h"
#include "ir/ir.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
#include "semantic/symbol_table.h"
#include "semantic/type_checker.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_all(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    return NULL;
  }
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *b = malloc((size_t)n + 1);
  if (b && fread(b, 1, (size_t)n, f) == (size_t)n) {
    b[n] = 0;
  } else {
    free(b);
    b = NULL;
  }
  fclose(f);
  return b;
}

/* minimal static AArch64 ELF executable */
#define ELF_BASE 0x400000u
#define ELF_HDRS 120u
static void p16(unsigned char *p, uint16_t v) { memcpy(p, &v, 2); }
static void p32(unsigned char *p, uint32_t v) { memcpy(p, &v, 4); }
static void p64(unsigned char *p, uint64_t v) { memcpy(p, &v, 8); }

static int write_elf(const char *path, const unsigned char *code, size_t len) {
  unsigned char h[ELF_HDRS];
  memset(h, 0, sizeof(h));
  uint64_t total = ELF_HDRS + len;
  h[0] = 0x7F; h[1] = 'E'; h[2] = 'L'; h[3] = 'F';
  h[4] = 2; h[5] = 1; h[6] = 1;
  p16(h + 16, 2); p16(h + 18, 183); p32(h + 20, 1);
  p64(h + 24, ELF_BASE + ELF_HDRS); p64(h + 32, 64); p64(h + 40, 0);
  p32(h + 48, 0); p16(h + 52, 64); p16(h + 54, 56); p16(h + 56, 1);
  unsigned char *ph = h + 64;
  p32(ph + 0, 1); p32(ph + 4, 5); p64(ph + 8, 0);
  p64(ph + 16, ELF_BASE); p64(ph + 24, ELF_BASE);
  p64(ph + 32, total); p64(ph + 40, total); p64(ph + 48, 0x1000);
  FILE *f = fopen(path, "wb");
  if (!f) {
    return 0;
  }
  int ok = fwrite(h, 1, ELF_HDRS, f) == ELF_HDRS &&
           fwrite(code, 1, len, f) == len;
  fclose(f);
  return ok;
}

/* import_resolver references this codegen helper for host-target detection; we
 * stub it so the ARM tool need not link the x86 backend it is replacing. */
BinaryTargetFormat binary_target_format_host_default(void) {
  return BINARY_TARGET_FORMAT_COFF_WIN64;
}

/* The IR optimizer's internal-compiler-error reporters live in compiler_crash.c
 * (with its Windows dbghelp deps); stub them so this tool stays lean. */
void mettle_compiler_ice_report(const char *reason, const char *detail) {
  fprintf(stderr, "ICE: %s%s%s\n", reason ? reason : "", detail ? ": " : "",
          detail ? detail : "");
  abort();
}
void mettle_compiler_ice(const char *reason) {
  mettle_compiler_ice_report(reason, NULL);
}

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: arm64_compile <src.mettle> <out.elf> [func]\n");
    return 2;
  }
  const char *src_path = argv[1], *out_path = argv[2];
  const char *want = (argc > 3) ? argv[3] : "main";

  char *source = read_all(src_path);
  if (!source) {
    fprintf(stderr, "cannot read %s\n", src_path);
    return 2;
  }

  ErrorReporter *er = error_reporter_create(src_path, source);
  Lexer *lex = lexer_create(source);
  SymbolTable *st = symbol_table_create();
  Parser *parser = parser_create_with_error_reporter(lex, er);
  TypeChecker *tc = type_checker_create_with_error_reporter(st, er);

  ASTNode *prog = parser_parse_program(parser);
  if (!prog) {
    fprintf(stderr, "parse failed\n");
    error_reporter_print_errors(er);
    return 1;
  }
  if (!type_checker_check_program(tc, prog)) {
    fprintf(stderr, "type check failed\n");
    error_reporter_print_errors(er);
    return 1;
  }
  char *ir_err = NULL;
  IRProgram *ir = ir_lower_program(prog, tc, st, &ir_err, 0);
  if (!ir) {
    fprintf(stderr, "IR lowering failed: %s\n", ir_err ? ir_err : "(unknown)");
    return 1;
  }

  IRFunction *fn = NULL;
  for (size_t i = 0; i < ir->function_count; i++) {
    if (strcmp(ir->functions[i]->name, want) == 0) {
      fn = ir->functions[i];
      break;
    }
  }
  if (!fn && argc <= 3 && ir->function_count > 0) {
    fn = ir->functions[0];
  }
  if (!fn) {
    fprintf(stderr, "function '%s' not found\n", want);
    return 1;
  }

  Arm64Emit e;
  arm64_emit_init(&e);
  int lfn = arm64_new_label(&e);
  arm64_emit_bl(&e, lfn);                                  /* call the function */
  arm64_emit_word(&e, arm64_movz(1, ARM64_X8, 93, 0));    /* exit syscall */
  arm64_emit_word(&e, 0xD4000001u);                       /* svc #0 */
  arm64_bind_label(&e, lfn);
  if (!arm64_ir_encode_function(&e, fn)) {
    fprintf(stderr, "arm64 lowering unsupported for '%s'\n", fn->name);
    return 1;
  }
  if (!arm64_emit_finalize(&e)) {
    fprintf(stderr, "branch finalize failed\n");
    return 1;
  }
  if (!write_elf(out_path, e.code.data, e.code.len)) {
    fprintf(stderr, "cannot write %s\n", out_path);
    return 1;
  }
  fprintf(stderr, "OK: %s -> %s  (%zu bytes, function '%s')\n", src_path,
          out_path, e.code.len, fn->name);
  return 0;
}
