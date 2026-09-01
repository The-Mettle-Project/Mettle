#!/usr/bin/env bash
set -euo pipefail

usage() {
	cat <<EOF
Usage: local-setup-linux.bash [--clean] [--jobs N]

Configures and builds Release and Debug side by side in build-release/ and
build-debug/. Incremental by default; --clean discards both trees first.
EOF
}

clean=0
jobs=""

while [ $# -gt 0 ]; do
	case "$1" in
	--clean) clean=1; shift ;;
	--jobs) jobs="${2:-}"; shift 2 ;;
	-h | --help) usage; exit 0 ;;
	*) echo "unknown option: $1" >&2; usage >&2; exit 1 ;;
	esac
done

command -v cmake >/dev/null || { echo "need cmake" >&2; exit 1; }

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$root"

if [ -z "$jobs" ]; then
	if command -v nproc >/dev/null; then jobs="$(nproc)"; else jobs=4; fi
fi

half=$((jobs / 2))
[ "$half" -ge 1 ] || half=1

if [ "$clean" -eq 1 ]; then
	rm -rf build-release build-debug
fi

cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release &
pid1=$!
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug &
pid2=$!
wait "$pid1"
wait "$pid2"

start=$SECONDS

cmake --build build-release --parallel "$half" &
pid1=$!
cmake --build build-debug --parallel "$half" &
pid2=$!

rc=0
wait "$pid1" || rc=$?
wait "$pid2" || rc=$?

elapsed=$((SECONDS - start))

echo
if [ "$rc" -ne 0 ]; then
	echo "Build FAILED after ${elapsed}s"
	exit "$rc"
fi

printf 'Built build-release and build-debug in %dm %02ds with %d jobs each\n' \
	$((elapsed / 60)) $((elapsed % 60)) "$half"
