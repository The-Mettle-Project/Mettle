#!/bin/sh
set -e
compiler="$1"
work="$2"
source_dir="$(dirname "$0")"
mkdir -p "$work"
cp "$source_dir/walk.mettle" "$work/walk.mettle"
cd "$work"
"$compiler" --build -g walk.mettle -o walk
readelf -SW walk | grep -q ' .debug_info '
readelf -SW walk | grep -q ' .debug_line '
readelf --debug-dump=decodedline walk | grep -q 'walk.mettle *14 '
nm walk | grep -q ' add$'
if command -v gdb >/dev/null 2>&1; then
  gdb -q -batch -ex 'break add' -ex run -ex bt -ex kill walk >gdb.out 2>&1
  grep -q 'add () at walk.mettle:7' gdb.out
  grep -q 'walk () at walk.mettle:14' gdb.out
  grep -q 'main () at walk.mettle:20' gdb.out
fi
./walk | grep -q '^12$'
echo debuginfo ok
