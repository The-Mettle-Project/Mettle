#!/usr/bin/env bash

if [ -d "./build-release/" ]; then
	rm -r build-release/
fi

if [ -d "./build-debug/" ]; then
	rm -r build-debug/
fi

cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release &
pid1=$!

cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug &
pid2=$!

wait $pid1 $pid2

start=$(date +%s.%N)

cmake --build build-release --parallel &
pid1=$!

cmake --build build-debug --parallel &
pid2=$!

wait $pid1 $pid2

end=$(date +%s.%N)

format_time() {
	local val="$1"
	local sec="${val%%.*}"
	local ns="${val#*.}"
	# Pad to 9 digits (in case bc drops trailing zeros)
	ns=$(printf "%-9s" "$ns" | tr ' ' '0' | cut -c1-9)
	printf "%ss %sns\n" "$sec" "$ns"
}

echo ""
echo "Build local complete [build-release, build-debug]"
echo ""
echo "build time: $(format_time "$(echo "$end - $start" | bc)")"
