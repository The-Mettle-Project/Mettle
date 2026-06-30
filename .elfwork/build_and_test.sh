#!/usr/bin/env bash
# Native-ELF end-to-end test harness for WSL. Builds the compiler, a small
# startup-object emitter harness, then compiles + links + runs Mettle programs
# with bare `ld` (no libc) to validate the self-contained _start.
#
# Usage: bash .elfwork/build_and_test.sh
set -u
cd "$(dirname "$0")/.."
W="$PWD/.elfwork"

echo "== building compiler =="
gcc -std=c99 -O1 -D_GNU_SOURCE -Isrc -fno-omit-frame-pointer \
  src/common.c src/lexer/*.c src/parser/*.c src/semantic/*.c src/ir/*.c \
  src/codegen/*.c src/codegen/binary/*.c src/linker/*.c src/error/*.c \
  src/debug/*.c src/compiler/*.c src/main.c src/tracy_build.c \
  -o "$W/mettle" -rdynamic 2>"$W/cc.log" || { echo "COMPILER BUILD FAILED"; cat "$W/cc.log"; exit 1; }

echo "== building startup-object harnesses =="
# noargs (main_wants_argc_argv=0) and argv (=1) variants
for variant in 0 1; do
  cat > "$W/h_$variant.c" <<EOF
int binary_write_program_startup_object(const char*,int,int,int);
int main(void){return binary_write_program_startup_object("$W/start_$variant.o",0,0,$variant);}
EOF
  gcc -std=c99 -O1 -D_GNU_SOURCE -Isrc -Isrc/codegen "$W/h_$variant.c" \
    src/common.c src/lexer/lexer.c src/error/error_reporter.c \
    src/codegen/binary_emitter.c src/codegen/elf_emitter.c \
    src/codegen/binary/encoders.c src/codegen/binary/support.c src/codegen/binary/startup.c \
    -o "$W/h_$variant" 2>"$W/h_$variant.log" || { echo "HARNESS $variant FAILED"; cat "$W/h_$variant.log"; exit 1; }
  "$W/h_$variant" || { echo "startup emit $variant failed"; exit 1; }
done

fail=0
run_case() {
  # $1 = name, $2 = startup variant, $3 = expected exit, $4.. = run args
  local name="$1" sv="$2" want="$3"; shift 3
  "$W/mettle" --emit-obj "$W/$name.mettle" -o "$W/$name.o" 2>"$W/$name.emit.log" \
    || { echo "[$name] EMIT FAILED"; cat "$W/$name.emit.log"; fail=1; return; }
  ld "$W/start_$sv.o" "$W/$name.o" -o "$W/$name.bin" 2>"$W/$name.ld.log" \
    || { echo "[$name] LD FAILED"; cat "$W/$name.ld.log"; fail=1; return; }
  "$W/$name.bin" "$@"; local got=$?
  if [ "$got" = "$want" ]; then echo "[$name] PASS (exit $got)"; else echo "[$name] FAIL got $got want $want"; fail=1; fi
}

cat > "$W/loop.mettle" <<'EOF'
fn compute(n: int32) -> int32 {
  var acc: int32 = 0; var i: int32 = 0;
  while (i < n) { acc = acc + i; i = i + 1; }
  return acc;
}
fn main() -> int32 { return compute(10); }
EOF
run_case loop 0 45

cat > "$W/argc.mettle" <<'EOF'
fn main(argc: int32, argv: int8**) -> int32 { return argc; }
EOF
run_case argc 1 4 a b c

# >6 args (SysV register overflow to stack) and a recursive call.
cat > "$W/many.mettle" <<'EOF'
fn sum8(a: int64, b: int64, c: int64, d: int64, e: int64, f: int64, g: int64, h: int64) -> int64 {
  return a + b + c + d + e + f + g + h;
}
fn fib(n: int32) -> int32 {
  if (n < 2) { return n; }
  return fib(n - 1) + fib(n - 2);
}
fn main() -> int32 {
  return (int32)sum8(1, 2, 3, 4, 5, 6, 7, 8) + fib(10);
}
EOF
# sum8(1..8)=36, fib(10)=55, total 91
run_case many 0 91

echo "== also exercise the full 'mettle --build' driver =="
if "$W/mettle" --build "$W/many.mettle" -o "$W/many.driver.bin" >"$W/many.driver.log" 2>&1; then
  "$W/many.driver.bin"; g=$?
  if [ "$g" = 91 ]; then echo "[driver] PASS (exit 91)"; else echo "[driver] FAIL got $g want 91"; fail=1; fi
else
  echo "[driver] BUILD FAILED"; cat "$W/many.driver.log"; fail=1
fi

echo "== summary =="
file "$W/loop.bin"
[ "$fail" = 0 ] && echo "ALL NATIVE ELF TESTS PASSED" || echo "SOME TESTS FAILED"
exit $fail
