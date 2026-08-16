#!/usr/bin/env bash
# Native-ELF backend regression test for Linux.
#
# Compiles small Mettle programs through the owned static ELF path and checks
# their exit codes, output, file format, and dependency state.
#
# Expects the compiler at bin/mettle (build it with `make`). Requires gcc.
# Usage: tools/test-elf-native.sh [path-to-mettle]
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
METTLE="${1:-$ROOT/bin/mettle}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

if [ ! -x "$METTLE" ]; then
  echo "error: compiler not found at $METTLE (run 'make' first)" >&2
  exit 2
fi

fail=0

audit_owned() {
  local name="$1" product="$2"
  if readelf -l "$product" | grep -qE 'INTERP|DYNAMIC'; then
    echo "[$name] FAIL contains a loader or dynamic segment"; fail=1; return 1
  fi
  if readelf -d "$product" 2>&1 | grep -q NEEDED; then
    echo "[$name] FAIL contains a shared dependency"; fail=1; return 1
  fi
  if [ -n "$(nm -u "$product" 2>/dev/null)" ]; then
    echo "[$name] FAIL contains unresolved symbols"; fail=1; return 1
  fi
  return 0
}

# run_case <name> <expected-exit> <source> [run-args...]
run_case() {
  local name="$1" want="$2" src="$3"; shift 3
  printf '%s' "$src" > "$WORK/$name.mettle"
  if ! "$METTLE" --build "$WORK/$name.mettle" -o "$WORK/$name.bin" \
        >"$WORK/$name.log" 2>&1; then
    echo "[$name] BUILD FAILED"; sed 's/^/    /' "$WORK/$name.log"; fail=1; return
  fi
  if ! file "$WORK/$name.bin" | grep -q "ELF.*executable"; then
    echo "[$name] FAIL not an ELF executable"; fail=1; return
  fi
  audit_owned "$name" "$WORK/$name.bin" || return
  "$WORK/$name.bin" "$@"; local got=$?
  if [ "$got" = "$want" ]; then
    echo "[$name] PASS (exit $got)"
  else
    echo "[$name] FAIL got $got want $want"; fail=1
  fi
}

run_case loop 45 'fn compute(n: int32) -> int32 {
  var acc: int32 = 0; var i: int32 = 0;
  while (i < n) { acc = acc + i; i = i + 1; }
  return acc;
}
fn main() -> int32 { return compute(10); }'

run_case recursion 55 'fn fib(n: int32) -> int32 {
  if (n < 2) { return n; }
  return fib(n - 1) + fib(n - 2);
}
fn main() -> int32 { return fib(10); }'

# 8 integer args: 6 in SysV registers, 2 spilled to the stack.
run_case stackargs 36 'fn sum8(a: int64, b: int64, c: int64, d: int64, e: int64, f: int64, g: int64, h: int64) -> int64 {
  return a + b + c + d + e + f + g + h;
}
fn main() -> int32 { return (int32)sum8(1, 2, 3, 4, 5, 6, 7, 8); }'

# argc read off the kernel stack at _start; 3 args + argv[0] => 4.
run_case argcount 4 'fn main(argc: int32, argv: int8**) -> int32 { return argc; }' a b c

# run_case_out <name> <expected-exit> <expected-stdout> <source> [run-args...]
# Like run_case but also asserts the program's stdout. Exercises the syscall-based
# std/io, std/bench and std/process (.linux) modules — still bare static ELF, no libc.
run_case_out() {
  local name="$1" want="$2" want_out="$3" src="$4"; shift 4
  printf '%s' "$src" > "$WORK/$name.mettle"
  if ! "$METTLE" --build "$WORK/$name.mettle" -o "$WORK/$name.bin" \
        >"$WORK/$name.log" 2>&1; then
    echo "[$name] BUILD FAILED"; sed 's/^/    /' "$WORK/$name.log"; fail=1; return
  fi
  if ! file "$WORK/$name.bin" | grep -q "ELF.*executable"; then
    echo "[$name] FAIL not an ELF executable"; fail=1; return
  fi
  audit_owned "$name" "$WORK/$name.bin" || return
  local got_out; got_out="$("$WORK/$name.bin" "$@")"; local got=$?
  if [ "$got" = "$want" ] && [ "$got_out" = "$want_out" ]; then
    echo "[$name] PASS (exit $got)"
  else
    echo "[$name] FAIL got exit=$got out=[$got_out] want exit=$want out=[$want_out]"; fail=1
  fi
}

# std/io console output via the write syscall (no libc): println + print_int.
run_case_out stdio_console 0 'value=42
-7' 'import "std/io";
fn main() -> int32 {
  print("value="); print_int(42); newline();
  print_int(-7); newline();
  return 0;
}'

# std/io file I/O round-trip via open/write/read/close syscalls. fgets reading a
# parameter buffer across a call exercises the SysV RSI/RDI promotion fix.
run_case_out stdio_file 0 'line one' 'import "std/io";
fn main() -> int32 {
  var w: cstring = fopen("/tmp/mettle_elf_test.txt", "w");
  if (w == 0) { return 1; }
  fputs("line one\n", w);
  fclose(w);
  var r: cstring = fopen("/tmp/mettle_elf_test.txt", "r");
  if (r == 0) { return 2; }
  var buf: uint8[64];
  if (fgets(&buf[0], 64, r) == 0) { return 3; }
  print_cstr(&buf[0]);
  fclose(r);
  return 0;
}'

# std/bench monotonic timing via clock_gettime; std/process exit via the exit
# syscall. Returns 0 only if the second timestamp is >= the first.
run_case bench_monotonic 0 'import "std/bench";
import "std/process";
fn main() -> int32 {
  var t0: int64 = bench_time_us();
  var i: int64 = 0; var acc: int64 = 0;
  while (i < 1000000) { acc = acc + i; i = i + 1; }
  var t1: int64 = bench_time_us();
  if (t1 < t0) { exit(1); }
  if (acc == 0) { exit(2); }
  return 0;
}'

# std/process exit code through the owned exit system call.
run_case proc_exit 7 'import "std/process";
fn main() -> int32 { exit(7); return 0; }'

# Heap allocation through Mettle's owned zeroed allocator.
run_case heap_new 30 'struct Pair {
  a: int32;
  b: int32;
}
fn main() -> int32 {
  var p: Pair* = new Pair;
  p->a = 12;
  p->b = 18;
  return p->a + p->b;
}'

# Direct owned malloc and free from std/mem.
run_case heap_malloc 42 'import "std/mem";
fn main() -> int32 {
  var buf: cstring = malloc(16);
  if (buf == 0) { return 1; }
  buf[0] = 42;
  var v: int32 = (int32)buf[0];
  free(buf);
  return v;
}'

# memcpy and memset become the inline string operation in the register
# allocating backend, taking their arguments from wherever the call marshalling
# left them. Naming Win64's registers reads a SysV memset's fill byte as its
# destination, so this walks the bytes and checks the returned pointer, which is
# the destination and comes back in RAX. The live values either side of the copy
# are there to make the allocator hold registers across it.
run_case block_copy 0 'import "std/mem";
@noinline fn fill_and_copy(dst: cstring, src: cstring, n: int64) -> int64 {
  var live: int64 = n * 3 + 1;
  if (memset(src, 65, n) != src) { return 0; }
  if (memcpy(dst, src, n) != dst) { return 0; }
  return live;
}
fn main() -> int32 {
  var dst: uint8[64];
  var src: uint8[64];
  var i: int32 = 0;
  while (i < 64) { dst[i] = 0; src[i] = 0; i = i + 1; }
  if (fill_and_copy((cstring)((int64)&dst[0]), (cstring)((int64)&src[0]), 64)
      != 193) { return 1; }
  i = 0;
  while (i < 64) {
    if (dst[i] != 65) { return 2; }
    i = i + 1;
  }
  return 0;
}'

# std/thread with the Win32 API (CreateThread / WaitForSingleObject /
# CloseHandle) works on Linux through clone and futex.
run_case_out unified_thread 0 'worker ran
main joined' 'import "std/io";
import "std/thread";
fn worker(arg: cstring) -> uint32 {
  println("worker ran");
  return 0;
}
fn main() -> int32 {
  var h: int64 = CreateThread(0, 0, &worker, 0, 0, 0);
  if (h == 0) { return 1; }
  if (WaitForSingleObject(h, INFINITE()) != WAIT_OBJECT_0()) { return 2; }
  CloseHandle(h);
  println("main joined");
  return 0;
}'

# std/net with the Winsock-flavoured API (socket_tcp / closesocket / net_init /
# net_cleanup) works on Linux through socket system calls.
run_case_out unified_net 0 'tcp socket ok' 'import "std/io";
import "std/net";
fn main() -> int32 {
  net_init();
  var s: int64 = socket_tcp();
  if (s < 0) { net_cleanup(); return 1; }
  closesocket(s);
  net_cleanup();
  println("tcp socket ok");
  return 0;
}'

# The standard prelude resolves through the owned runtime.
prelude_name=prelude_build
printf '%s' 'fn main() -> int32 { println("preludes work"); return 0; }' \
  > "$WORK/$prelude_name.mettle"
if "$METTLE" --build --prelude "$WORK/$prelude_name.mettle" \
      -o "$WORK/$prelude_name.bin" >"$WORK/$prelude_name.log" 2>&1 \
   && audit_owned "$prelude_name" "$WORK/$prelude_name.bin" \
   && [ "$("$WORK/$prelude_name.bin")" = "preludes work" ]; then
  echo "[$prelude_name] PASS (exit 0)"
else
  echo "[$prelude_name] FAILED"; sed 's/^/    /' "$WORK/$prelude_name.log"; fail=1
fi

if [ "$fail" = 0 ]; then
  echo "ALL NATIVE ELF TESTS PASSED"
else
  echo "SOME NATIVE ELF TESTS FAILED"
fi
exit $fail
