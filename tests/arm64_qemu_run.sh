#!/bin/sh
# Run the hand-built AArch64 ELF programs produced by tests/arm64_emit_test.c
# under a qemu-aarch64 user-mode emulator and check each program's exit code
# against its expected result. The from-scratch encoder + ELF emitter generate
# the binaries; QEMU is only the emulated CPU (no external assembler/linker is
# involved), so this is the semantic proof that the generated machine code runs
# correctly on AArch64 without ARM hardware.
#
# Usage: arm64_qemu_run.sh <elf_dir>
#   <elf_dir> contains <name>.elf files and a manifest.txt of "<name> <exit>".
#
# Emulator discovery: $METTLE_ARM64_QEMU, then qemu-aarch64 on PATH, then
# ~/arm64tools/qemu-aarch64 (the no-root extract: `apt-get download
# qemu-user-static && dpkg -x <deb> .`).
#
# Exit: 0 all passed, 1 a wrong result, 64 skipped (no emulator), 2 bad args.
set -u

dir="${1:-}"
if [ -z "$dir" ] || [ ! -f "$dir/manifest.txt" ]; then
  echo "usage: arm64_qemu_run.sh <elf_dir-with-manifest.txt>"
  exit 2
fi

qemu="${METTLE_ARM64_QEMU:-}"
if [ -z "$qemu" ]; then
  if command -v qemu-aarch64 >/dev/null 2>&1; then
    qemu="qemu-aarch64"
  elif [ -x "$HOME/arm64tools/qemu-aarch64" ]; then
    qemu="$HOME/arm64tools/qemu-aarch64"
  fi
fi
if [ -z "$qemu" ]; then
  echo "SKIP: no qemu-aarch64 emulator found"
  exit 64
fi

fail=0
# tr strips any stray CR so manifests authored on Windows still parse.
tr -d '\r' < "$dir/manifest.txt" | while IFS=' ' read -r name expect; do
  [ -n "$name" ] || continue
  elf="$dir/$name.elf"
  if [ ! -f "$elf" ]; then
    echo "  MISS $name ($elf)"
    exit 1
  fi
  # Copy into the Linux fs: executing an ELF straight off /mnt can be flaky.
  cp "$elf" /tmp/m_arm64.elf
  chmod +x /tmp/m_arm64.elf
  "$qemu" /tmp/m_arm64.elf
  got=$?
  if [ "$got" -eq "$expect" ]; then
    echo "  PASS $name -> $got"
  else
    echo "  FAIL $name -> $got (expected $expect)"
    exit 1
  fi
done
# Propagate the subshell's failure out of the pipeline.
fail=$?
exit $fail
