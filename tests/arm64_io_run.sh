#!/bin/sh
# Run the AArch64 I/O fixtures under qemu-aarch64 and diff each program's stdout
# against its committed <name>.out. Used by the arm64_source gate; the fixtures
# are produced by `mettle --emit-arm64`. Proves real Mettle I/O (println_int /
# print_int / println / print / cstr string literals) works on AArch64.
#
# Usage: arm64_io_run.sh <dir>   (dir holds <name>.elf and <name>.out pairs)
# Exit: 0 all match, 1 a stdout mismatch, 64 skipped (no emulator), 2 bad args.
set -u

dir="${1:-}"
if [ -z "$dir" ] || [ ! -d "$dir" ]; then
  echo "usage: arm64_io_run.sh <dir>"
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
for elf in "$dir"/*.elf; do
  [ -f "$elf" ] || continue
  name=$(basename "$elf" .elf)
  exp="$dir/$name.out"
  [ -f "$exp" ] || continue
  cp "$elf" /tmp/m_io.elf
  chmod +x /tmp/m_io.elf
  got=$("$qemu" /tmp/m_io.elf 2>/dev/null)
  want=$(cat "$exp")
  if [ "$got" = "$want" ]; then
    echo "  PASS $name"
  else
    echo "  FAIL $name (stdout mismatch)"
    fail=1
  fi
done
exit $fail
