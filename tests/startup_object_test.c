#include "codegen/binary/startup.h"

#include <stdio.h>

int main(int argc, char **argv) {
  if (argc != 5) {
    fprintf(stderr,
            "usage: startup_object_test <x64.o> <arm64.o> <win64.obj> "
            "<arm64-basic.o>\n");
    return 2;
  }
  if (binary_write_program_startup_object_for_target(
          argv[1], BINARY_TARGET_FORMAT_ELF_X64, 1, 1, 1) != 0) {
    fprintf(stderr, "could not emit the x86-64 ELF startup object\n");
    return 1;
  }
  if (binary_write_program_startup_object_for_target(
          argv[2], BINARY_TARGET_FORMAT_ELF_ARM64, 1, 1, 1) != 0) {
    fprintf(stderr, "could not emit the AArch64 ELF startup object\n");
    return 1;
  }
  if (binary_write_program_startup_object_for_target(
          argv[3], BINARY_TARGET_FORMAT_COFF_WIN64, 1, 1, 1) != 0) {
    fprintf(stderr, "could not emit the Windows x86-64 startup object\n");
    return 1;
  }
  if (binary_write_program_startup_object_for_target(
          argv[4], BINARY_TARGET_FORMAT_ELF_ARM64, 0, 0, 1) != 0) {
    fprintf(stderr, "could not emit the basic AArch64 ELF startup object\n");
    return 1;
  }
  return 0;
}
