#!/usr/bin/env bash
# Dedicated Mettle benchmark harness against C — Linux port of run-benchmarks.ps1.
#
# - Reads benchmark matrix from docs/benchmarks/harness.json
# - Compiles Mettle and C binaries (timed)
# - Records executable sizes
# - Executes binaries (warmup + median of N runs)
# - Parses "Time: <N> us" output (falls back to ms, then wall clock)
# - Optionally profiles large compile-only fixtures (parse_stress, profiler)
# - Writes canonical JSON to docs/benchmarks/latest.json
# - Mirrors JSON to web/benchmarks.json for the web server
#
# Differences from the PowerShell harness:
# - `.exe` suffixes in harness.json are stripped for the Linux binaries.
# - `-lkernel32` in c_flags is dropped; `-lm` is appended (after the source
#   file, where GNU ld requires libraries).
# - The HTML report is only generated when `pwsh` is on PATH.
#
# Requires: bash, jq, gcc (or clang with --clang), a built bin/mettle.
#
# Usage:
#   ./tools/benchmark/run-benchmarks.sh
#   ./tools/benchmark/run-benchmarks.sh --build-compiler
#   ./tools/benchmark/run-benchmarks.sh --runs 7 --warmup 2
#   ./tools/benchmark/run-benchmarks.sh --benchmark fib,grep
#   ./tools/benchmark/run-benchmarks.sh --compile-only
#   ./tools/benchmark/run-benchmarks.sh --skip-compile-benchmarks
#   ./tools/benchmark/run-benchmarks.sh --cflags "-O3 -march=native"
#   ./tools/benchmark/run-benchmarks.sh --quiet

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT"

BUILD_COMPILER=0
COMPILE_ONLY=0
SKIP_COMPILE_BENCHMARKS=0
QUIET=0
NO_REPORT=0
OPEN_REPORT=0
USE_CLANG=0
USE_GCC=0
CONFIG_PATH="docs/benchmarks/harness.json"
COMPILER_PATH=""
BENCH_FILTER=()
USER_CFLAGS=()
RUNS=0
WARMUP=-1

usage() {
    sed -n '2,30p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-compiler) BUILD_COMPILER=1 ;;
        --compile-only) COMPILE_ONLY=1 ;;
        --skip-compile-benchmarks) SKIP_COMPILE_BENCHMARKS=1 ;;
        --quiet) QUIET=1 ;;
        --no-report) NO_REPORT=1 ;;
        --open-report) OPEN_REPORT=1 ;;
        --clang) USE_CLANG=1 ;;
        --gcc) USE_GCC=1 ;;
        --config) CONFIG_PATH="$2"; shift ;;
        --compiler) COMPILER_PATH="$2"; shift ;;
        --benchmark)
            IFS=',' read -r -a _parts <<<"$2"
            BENCH_FILTER+=("${_parts[@]}")
            shift ;;
        --cflags)
            # shellcheck disable=SC2206
            USER_CFLAGS+=($2)
            shift ;;
        --runs) RUNS="$2"; shift ;;
        --warmup) WARMUP="$2"; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage >&2; exit 1 ;;
    esac
    shift
done

if [[ $USE_CLANG -eq 1 && $USE_GCC -eq 1 ]]; then
    echo "Error: --clang and --gcc are mutually exclusive." >&2
    exit 1
fi

if ! command -v jq >/dev/null 2>&1; then
    echo "Error: jq is required (apt install jq / dnf install jq)." >&2
    exit 1
fi

log() { [[ $QUIET -eq 1 ]] || echo "$@"; }

# Convert a harness.json path to a Linux one: forward slashes, no .exe suffix.
linux_exe_path() {
    local p="${1//\\//}"
    printf '%s' "${p%.exe}"
}

linux_path() {
    printf '%s' "${1//\\//}"
}

bench_selected() {
    local name="$1" item
    [[ ${#BENCH_FILTER[@]} -eq 0 ]] && return 0
    for item in "${BENCH_FILTER[@]}"; do
        [[ "$item" == "$name" ]] && return 0
    done
    return 1
}

# --- timing helpers ---------------------------------------------------------

RUN_OUTPUT=""
RUN_EXIT=0
RUN_ELAPSED_MS=""

run_captured() {
    local start end
    start=$(date +%s%N)
    RUN_OUTPUT=$("$@" 2>&1)
    RUN_EXIT=$?
    end=$(date +%s%N)
    RUN_ELAPSED_MS=$(awk -v s="$start" -v e="$end" 'BEGIN{printf "%.3f", (e-s)/1000000.0}')
}

# Parse "Time: <N> us" (preferred) or "Time:/Elapsed: <N> ms" from program
# output; prints microseconds or nothing.
parse_time_us() {
    local line
    line=$(printf '%s\n' "$1" | grep -iE '^[[:space:]]*Time:[[:space:]]*[0-9]+[[:space:]]*us([^a-z]|$)' | head -n1)
    if [[ -n "$line" ]]; then
        printf '%s' "$line" | grep -oE '[0-9]+' | head -n1
        return 0
    fi
    line=$(printf '%s\n' "$1" | grep -iE '^[[:space:]]*(Time|Elapsed([[:space:]]+time)?)[[:space:]]*:[[:space:]]*[0-9]+(\.[0-9]+)?[[:space:]]*ms([^a-z]|$)' | head -n1)
    if [[ -n "$line" ]]; then
        local ms
        ms=$(printf '%s' "$line" | grep -oE '[0-9]+(\.[0-9]+)?' | head -n1)
        awk -v v="$ms" 'BEGIN{printf "%.3f", v * 1000.0}'
        return 0
    fi
    return 0
}

parse_profile_total_ms() {
    local line
    line=$(printf '%s\n' "$1" | grep -iE '^[[:space:]]*total[[:space:]]+[0-9]+(\.[0-9]+)?[[:space:]]*ms([^a-z]|$)' | head -n1)
    [[ -n "$line" ]] && printf '%s' "$line" | grep -oE '[0-9]+(\.[0-9]+)?' | head -n1
    return 0
}

# --- stats helpers ----------------------------------------------------------

# stdin: one value per line. Prints "median min max stddev count"
# (stddev is "null" when count < 2).
compute_stats() {
    sort -n | awk '
        { v[NR] = $1; s += $1; ss += $1 * $1 }
        END {
            n = NR
            if (n == 0) { exit 1 }
            if (n % 2 == 1) { m = v[(n + 1) / 2] }
            else { m = (v[n / 2] + v[n / 2 + 1]) / 2.0 }
            sd = "null"
            if (n >= 2) {
                var = (ss - s * s / n) / (n - 1)
                if (var < 0) { var = 0 }
                sd = sprintf("%.1f", sqrt(var))
            }
            printf "%.6f %.0f %.0f %s %d\n", m, v[1], v[n], sd, n
        }'
}

ratio_or_null() { # numerator denominator -> 2dp ratio or "null"
    awk -v a="${1:-}" -v b="${2:-}" 'BEGIN {
        if (a == "" || b == "" || b + 0 <= 0) { print "null" }
        else { printf "%.2f\n", a / b }
    }'
}

format_number() { # value digits -> fixed-point or ""
    if [[ -z "${1:-}" || "$1" == "null" ]]; then printf ''; return; fi
    awk -v v="$1" -v d="${2:-2}" 'BEGIN{printf "%.*f", d, v}'
}

format_bench_time() { # microseconds -> "X ms (N us)"
    if [[ -z "${1:-}" ]]; then printf 'FAIL'; return; fi
    awk -v us="$1" 'BEGIN{
        ms = us / 1000.0
        digits = (ms >= 100.0) ? 2 : 3
        printf "%.*f ms (%d us)", digits, ms, us + 0.5
    }'
}

format_file_size() { # bytes
    if [[ -z "${1:-}" ]]; then printf 'FAIL'; return; fi
    awk -v b="$1" 'BEGIN{
        if (b >= 1048576) { printf "%.2f MB", b / 1048576.0 }
        else if (b >= 1024) { printf "%.1f KB", b / 1024.0 }
        else { printf "%d B", b }
    }'
}

exe_size_bytes() {
    [[ -f "$1" ]] || { printf ''; return; }
    stat -c %s "$1" 2>/dev/null || wc -c <"$1"
}

# --- config -----------------------------------------------------------------

CONFIG_FULL="$ROOT/$(linux_path "$CONFIG_PATH")"
if [[ ! -f "$CONFIG_FULL" ]]; then
    echo "Error: Harness config not found: $CONFIG_FULL" >&2
    exit 1
fi
CONFIG=$(cat "$CONFIG_FULL")

EFFECTIVE_RUNS=$RUNS
if [[ $EFFECTIVE_RUNS -le 0 ]]; then
    EFFECTIVE_RUNS=$(jq -r '.defaults.runs // 5' <<<"$CONFIG")
fi
EFFECTIVE_WARMUP=$WARMUP
if [[ $EFFECTIVE_WARMUP -lt 0 ]]; then
    EFFECTIVE_WARMUP=$(jq -r '.defaults.warmup // 1' <<<"$CONFIG")
fi

if [[ $EFFECTIVE_RUNS -lt 1 ]]; then
    echo "Error: --runs must be at least 1." >&2
    exit 1
fi

if [[ $BUILD_COMPILER -eq 1 ]]; then
    log "Building compiler..."
    make -C "$ROOT" || exit 1
fi

if [[ -z "$COMPILER_PATH" ]]; then
    COMPILER="$ROOT/bin/mettle"
else
    COMPILER="$(linux_path "$COMPILER_PATH")"
    [[ "$COMPILER" != /* ]] && COMPILER="$ROOT/$COMPILER"
fi

if [[ ! -x "$COMPILER" ]]; then
    echo "Error: Compiler not found: $COMPILER (build it with 'make' or pass --compiler)." >&2
    exit 1
fi

C_COMPILER="gcc"
[[ $USE_CLANG -eq 1 ]] && C_COMPILER="clang"
if ! command -v "$C_COMPILER" >/dev/null 2>&1; then
    echo "Error: $C_COMPILER is required for the C benchmark builds but was not found on PATH." >&2
    exit 1
fi
C_VERSION_LINE=$("$C_COMPILER" --version 2>&1 | head -n1)

# Default Mettle build args (per-bench mettle_flags replaces these entirely,
# mirroring the PowerShell harness).
DEFAULT_METTLE_FLAGS=()
while IFS= read -r flag; do
    DEFAULT_METTLE_FLAGS+=("$flag")
done < <(jq -r '(.defaults.mettle_flags // ["--build","--emit-obj","--linker","internal","--release"])[]' <<<"$CONFIG")

# C flags: CLI override > config defaults > -O3. Drop the Windows-only
# -lkernel32; collect -l* libraries separately so they land after the source
# file (GNU ld resolves symbols left to right), and make sure -lm is present.
RAW_C_FLAGS=()
if [[ ${#USER_CFLAGS[@]} -gt 0 ]]; then
    RAW_C_FLAGS=("${USER_CFLAGS[@]}")
else
    while IFS= read -r flag; do
        RAW_C_FLAGS+=("$flag")
    done < <(jq -r '(.defaults.c_flags // ["-O3"])[]' <<<"$CONFIG")
fi

C_FLAGS=()
C_LIBS=()
for flag in "${RAW_C_FLAGS[@]}"; do
    case "$flag" in
        -lkernel32) ;;
        -l*) C_LIBS+=("$flag") ;;
        *) C_FLAGS+=("$flag") ;;
    esac
done
_has_lm=0
for lib in "${C_LIBS[@]+"${C_LIBS[@]}"}"; do
    [[ "$lib" == "-lm" ]] && _has_lm=1
done
[[ $_has_lm -eq 0 ]] && C_LIBS+=("-lm")

# Recorded in the JSON payload as the effective C flags.
C_FLAGS_JSON=$(printf '%s\n' "${C_FLAGS[@]}" "${C_LIBS[@]}" | jq -R . | jq -s -c .)
METTLE_FLAGS_JSON=$(printf '%s\n' "${DEFAULT_METTLE_FLAGS[@]}" | jq -R . | jq -s -c .)

# --- host info --------------------------------------------------------------

os_pretty=$(. /etc/os-release 2>/dev/null && printf '%s' "${PRETTY_NAME:-}")
[[ -z "$os_pretty" ]] && os_pretty=$(uname -sr)
cpu_name=$(awk -F': *' '/^model name/{print $2; exit}' /proc/cpuinfo 2>/dev/null)
logical_procs=$(nproc 2>/dev/null || echo "")
ram_gb=$(awk '/^MemTotal/{printf "%.1f", $2 / 1048576.0}' /proc/meminfo 2>/dev/null)

HOST_JSON=$(jq -n \
    --arg machine "$(hostname)" \
    --arg os "$os_pretty" \
    --arg cpu "$cpu_name" \
    --arg lp "$logical_procs" \
    --arg ram "$ram_gb" \
    --arg cc "$C_COMPILER" \
    --arg ccver "$C_VERSION_LINE" \
    --arg compiler "$COMPILER" \
    '{
        machine_name: $machine,
        os: $os,
        cpu: (if $cpu == "" then null else $cpu end),
        logical_processors: (if $lp == "" then null else ($lp | tonumber) end),
        ram_gb: (if $ram == "" then null else ($ram | tonumber) end),
        c_compiler: $cc,
        c_compiler_version: $ccver,
        compiler: $compiler
    }')

# --- per-benchmark machinery ------------------------------------------------

TMP_DIR=$(mktemp -d)
trap 'rm -rf "$TMP_DIR"' EXIT
RESULTS_NDJSON="$TMP_DIR/results.ndjson"
COMPILE_NDJSON="$TMP_DIR/compile.ndjson"
: >"$RESULTS_NDJSON"
: >"$COMPILE_NDJSON"
FAILED=()

# Sets MEASURE_MEDIAN_US, MEASURE_RUNS_JSON, MEASURE_STATS_JSON; returns 1 on failure.
measure_exe() {
    local exe="$1" i us values=()
    MEASURE_MEDIAN_US=""
    MEASURE_RUNS_JSON="null"
    MEASURE_STATS_JSON="null"
    [[ -x "$exe" ]] || return 1

    for ((i = 0; i < EFFECTIVE_WARMUP; i++)); do
        run_captured "$exe"
        [[ $RUN_EXIT -ne 0 ]] && return 1
    done

    for ((i = 0; i < EFFECTIVE_RUNS; i++)); do
        run_captured "$exe"
        [[ $RUN_EXIT -ne 0 ]] && return 1
        us=$(parse_time_us "$RUN_OUTPUT")
        if [[ -n "$us" ]]; then
            values+=("$us")
        else
            values+=("$(awk -v ms="$RUN_ELAPSED_MS" 'BEGIN{printf "%.3f", ms * 1000.0}')")
        fi
    done

    [[ ${#values[@]} -eq 0 ]] && return 1

    local stats median min max sd count
    stats=$(printf '%s\n' "${values[@]}" | compute_stats) || return 1
    read -r median min max sd count <<<"$stats"
    MEASURE_MEDIAN_US="$median"
    MEASURE_RUNS_JSON=$(printf '%s\n' "${values[@]}" | awk '{printf "%.0f\n", $1}' | jq -s -c .)
    MEASURE_STATS_JSON=$(jq -n -c \
        --argjson median "$(awk -v v="$median" 'BEGIN{printf "%.0f", v}')" \
        --argjson min "$min" --argjson max "$max" \
        --argjson sd "$sd" --argjson n "$count" \
        '{median_us: $median, min_us: $min, max_us: $max, stddev_us: $sd, samples: $n}')
    return 0
}

# Both compile helpers report elapsed ms via the COMPILE_MS global (not via
# command substitution, which would run them in a subshell and lose
# RUN_OUTPUT for error reporting).
COMPILE_MS=""

compile_mettle() { # src exe extra-flags...
    local src="$1" exe="$2"
    shift 2
    mkdir -p "$(dirname "$exe")"
    rm -f "$exe"
    run_captured "$COMPILER" "$@" "$src" -o "$exe"
    if [[ $RUN_EXIT -ne 0 || ! -f "$exe" ]]; then
        return 1
    fi
    COMPILE_MS="$RUN_ELAPSED_MS"
    return 0
}

compile_c() { # src exe flags... (libs appended)
    local src="$1" exe="$2"
    shift 2
    mkdir -p "$(dirname "$exe")"
    rm -f "$exe"
    run_captured "$C_COMPILER" "$@" -o "$exe" "$src" "${C_LIBS[@]}"
    if [[ $RUN_EXIT -ne 0 || ! -f "$exe" ]]; then
        return 1
    fi
    COMPILE_MS="$RUN_ELAPSED_MS"
    return 0
}

export_asm_snapshot() { # name src mettle-flags...
    local name="$1" src="$2"
    shift 2
    local asm_dir="$ROOT/.tmp/bench-asm"
    mkdir -p "$asm_dir"
    # No .o suffix: the Linux build pipeline names its intermediate object
    # "<output minus extension>.o", which would collide with the output itself.
    local obj="$asm_dir/${name}_mettle"
    local asm="$asm_dir/${name}_mettle.asm.txt"
    rm -f "$obj"
    run_captured "$COMPILER" "$@" "$src" -o "$obj"
    if [[ $RUN_EXIT -ne 0 ]]; then
        log "  asm snapshot skipped for ${name}: compile failed"
        return
    fi
    if ! command -v objdump >/dev/null 2>&1; then
        log "  asm snapshot skipped for ${name}: objdump not on PATH"
        return
    fi
    if objdump -d -M intel "$obj" >"$asm" 2>/dev/null; then
        log "  asm snapshot: $asm"
    fi
}

# --- runtime benchmarks -----------------------------------------------------

bench_count=$(jq '.benchmarks | length' <<<"$CONFIG")
if [[ "$bench_count" -eq 0 ]]; then
    echo "Error: Harness config has no runtime benchmarks: $CONFIG_FULL" >&2
    exit 1
fi

for ((bi = 0; bi < bench_count; bi++)); do
    bench=$(jq -c ".benchmarks[$bi]" <<<"$CONFIG")
    name=$(jq -r '.name' <<<"$bench")
    bench_selected "$name" || continue

    kind=$(jq -r '.kind // "runtime"' <<<"$bench")
    description=$(jq -r '.description // ""' <<<"$bench")
    track_asm=$(jq -r '.track_asm // false' <<<"$bench")

    mettle_exe_rel=$(jq -r '.mettle_exe // ""' <<<"$bench")
    if [[ -z "$mettle_exe_rel" ]]; then
        log "  FAILED: Benchmark '$name' does not define mettle_exe."
        FAILED+=("$name")
        continue
    fi
    mettle_exe="$ROOT/$(linux_exe_path "$mettle_exe_rel")"

    mettle_src_rel=$(jq -r '.mettle_source // ""' <<<"$bench")
    if [[ -n "$mettle_src_rel" ]]; then
        mettle_src="$ROOT/$(linux_path "$mettle_src_rel")"
    else
        mettle_src="${mettle_exe}.mettle"
    fi

    c_src_rel=$(jq -r '.c_source // ""' <<<"$bench")
    c_exe_rel=$(jq -r '.c_exe // ""' <<<"$bench")
    if [[ -n "$c_src_rel" ]]; then
        c_src="$ROOT/$(linux_path "$c_src_rel")"
    elif [[ -n "$c_exe_rel" ]]; then
        c_src="$ROOT/$(linux_path "${c_exe_rel%_c.exe}.c")"
    else
        log "  FAILED: Benchmark '$name' does not define c_source or c_exe."
        FAILED+=("$name")
        continue
    fi
    if [[ -z "$c_exe_rel" ]]; then
        log "  FAILED: Benchmark '$name' does not define c_exe."
        FAILED+=("$name")
        continue
    fi
    c_exe="$ROOT/$(linux_exe_path "$c_exe_rel")"

    log "Building $name..."

    if [[ ! -f "$mettle_src" ]]; then
        log "  FAILED: Mettle source not found: $mettle_src"
        FAILED+=("$name")
        continue
    fi
    if [[ ! -f "$c_src" ]]; then
        log "  FAILED: C source not found: $c_src"
        FAILED+=("$name")
        continue
    fi

    # Per-bench mettle_flags replaces the defaults entirely.
    mettle_flags=("${DEFAULT_METTLE_FLAGS[@]}")
    if [[ $(jq 'has("mettle_flags")' <<<"$bench") == "true" ]]; then
        mettle_flags=()
        while IFS= read -r flag; do
            mettle_flags+=("$flag")
        done < <(jq -r '.mettle_flags[]' <<<"$bench")
    fi

    if ! compile_mettle "$mettle_src" "$mettle_exe" "${mettle_flags[@]}"; then
        log "  FAILED: Mettle compile failed for $name:"
        log "$RUN_OUTPUT"
        FAILED+=("$name")
        continue
    fi
    mettle_compile_ms="$COMPILE_MS"
    if ! compile_c "$c_src" "$c_exe" "${C_FLAGS[@]}"; then
        log "  FAILED: C compile failed for $name:"
        log "$RUN_OUTPUT"
        FAILED+=("$name")
        continue
    fi
    c_compile_ms="$COMPILE_MS"

    if [[ "$track_asm" == "true" ]]; then
        export_asm_snapshot "$name" "$mettle_src" "${mettle_flags[@]}"
    fi

    mettle_exe_bytes=$(exe_size_bytes "$mettle_exe")
    c_exe_bytes=$(exe_size_bytes "$c_exe")

    mettle_us=""
    c_us=""
    c_noinline_us=""
    mettle_runs_json="null"
    c_runs_json="null"
    mettle_stats_json="null"
    c_stats_json="null"

    if [[ $COMPILE_ONLY -eq 0 ]]; then
        if measure_exe "$mettle_exe"; then
            mettle_us="$MEASURE_MEDIAN_US"
            mettle_runs_json="$MEASURE_RUNS_JSON"
            mettle_stats_json="$MEASURE_STATS_JSON"
        fi
        if measure_exe "$c_exe"; then
            c_us="$MEASURE_MEDIAN_US"
            c_runs_json="$MEASURE_RUNS_JSON"
            c_stats_json="$MEASURE_STATS_JSON"
        fi

        # Optional extra baseline mirroring the PowerShell harness.
        if [[ "$name" == "matrix_mul" && ${#USER_CFLAGS[@]} -eq 0 ]]; then
            c_noinline_exe="${c_exe%_c}_c_noinline"
            if compile_c "$c_src" "$c_noinline_exe" "-O3" "-fno-inline"; then
                if measure_exe "$c_noinline_exe"; then
                    c_noinline_us="$MEASURE_MEDIAN_US"
                fi
            else
                log "  optional C -fno-inline baseline skipped"
            fi
        fi
    fi

    runtime_ratio=$(ratio_or_null "$mettle_us" "$c_us")
    compile_ratio=$(ratio_or_null "$mettle_compile_ms" "$c_compile_ms")
    size_ratio=$(ratio_or_null "$mettle_exe_bytes" "$c_exe_bytes")

    jq -n -c \
        --arg name "$name" \
        --arg kind "$kind" \
        --arg description "$description" \
        --argjson mettle_us "$([[ -n "$mettle_us" ]] && awk -v v="$mettle_us" 'BEGIN{printf "%.0f", v}' || echo null)" \
        --argjson c_us "$([[ -n "$c_us" ]] && awk -v v="$c_us" 'BEGIN{printf "%.0f", v}' || echo null)" \
        --argjson c_noinline_us "$([[ -n "$c_noinline_us" ]] && awk -v v="$c_noinline_us" 'BEGIN{printf "%.0f", v}' || echo null)" \
        --argjson mettle_runs_us "$mettle_runs_json" \
        --argjson c_runs_us "$c_runs_json" \
        --argjson mettle_stats "$mettle_stats_json" \
        --argjson c_stats "$c_stats_json" \
        --argjson mettle_ms "$([[ -n "$mettle_us" ]] && awk -v v="$mettle_us" 'BEGIN{printf "%.3f", v / 1000.0}' || echo null)" \
        --argjson c_ms "$([[ -n "$c_us" ]] && awk -v v="$c_us" 'BEGIN{printf "%.3f", v / 1000.0}' || echo null)" \
        --argjson relative "$runtime_ratio" \
        --argjson mettle_compile_ms "$(awk -v v="$mettle_compile_ms" 'BEGIN{printf "%.0f", v}')" \
        --argjson c_compile_ms "$(awk -v v="$c_compile_ms" 'BEGIN{printf "%.0f", v}')" \
        --argjson compile_relative "$compile_ratio" \
        --argjson mettle_exe_bytes "${mettle_exe_bytes:-null}" \
        --argjson c_exe_bytes "${c_exe_bytes:-null}" \
        --argjson size_relative "$size_ratio" \
        '{
            name: $name, kind: $kind, description: $description,
            mettle_us: $mettle_us, c_us: $c_us, c_noinline_us: $c_noinline_us,
            mettle_runs_us: $mettle_runs_us, c_runs_us: $c_runs_us,
            mettle_stats: $mettle_stats, c_stats: $c_stats,
            mettle_ms: $mettle_ms, c_ms: $c_ms, relative: $relative,
            mettle_compile_ms: $mettle_compile_ms, c_compile_ms: $c_compile_ms,
            compile_relative: $compile_relative,
            mettle_exe_bytes: $mettle_exe_bytes, c_exe_bytes: $c_exe_bytes,
            size_relative: $size_relative
        }' >>"$RESULTS_NDJSON"

    if [[ $COMPILE_ONLY -eq 0 ]]; then
        ratio_text=""
        [[ "$runtime_ratio" != "null" ]] && ratio_text=" | ${runtime_ratio}x vs C"
        log "  runtime  Mettle: $(format_bench_time "$mettle_us") | C: $(format_bench_time "$c_us")$ratio_text | median of $EFFECTIVE_RUNS run(s), $EFFECTIVE_WARMUP warmup"
    fi
    ratio_text=""
    [[ "$compile_ratio" != "null" ]] && ratio_text=" | ${compile_ratio}x vs C"
    log "  compile  Mettle: $(format_number "$mettle_compile_ms" 0) ms | C: $(format_number "$c_compile_ms" 0) ms$ratio_text"
    if [[ $COMPILE_ONLY -eq 0 ]]; then
        ratio_text=""
        [[ "$size_ratio" != "null" ]] && ratio_text=" | ${size_ratio}x vs C"
        log "  size     Mettle: $(format_file_size "$mettle_exe_bytes") | C: $(format_file_size "$c_exe_bytes")$ratio_text"
    fi
done

# --- compile-only benchmarks ------------------------------------------------

if [[ $SKIP_COMPILE_BENCHMARKS -eq 0 ]]; then
    compile_count=$(jq '.compile_benchmarks // [] | length' <<<"$CONFIG")
    for ((ci = 0; ci < compile_count; ci++)); do
        bench=$(jq -c ".compile_benchmarks[$ci]" <<<"$CONFIG")
        name=$(jq -r '.name' <<<"$bench")
        bench_selected "$name" || continue

        description=$(jq -r '.description // ""' <<<"$bench")
        src_rel=$(jq -r '.mettle_source' <<<"$bench")
        src="$ROOT/$(linux_path "$src_rel")"

        log "Profiling compile: $name..."

        if [[ ! -f "$src" ]]; then
            log "  FAILED: Mettle source not found: $src"
            FAILED+=("$name")
            continue
        fi

        run_captured "$COMPILER" --profile "$src"
        if [[ $RUN_EXIT -ne 0 ]]; then
            log "  FAILED: Mettle profile compile failed for $name:"
            log "$RUN_OUTPUT"
            FAILED+=("$name")
            continue
        fi

        total_ms=$(parse_profile_total_ms "$RUN_OUTPUT")
        if [[ -z "$total_ms" ]]; then
            log "  FAILED: Could not parse profile total for $name"
            FAILED+=("$name")
            continue
        fi

        jq -n -c \
            --arg name "$name" \
            --arg description "$description" \
            --arg mettle_source "$src_rel" \
            --argjson mettle_compile_ms "$(awk -v v="$total_ms" 'BEGIN{printf "%.0f", v}')" \
            '{name: $name, description: $description, mettle_source: $mettle_source, mettle_compile_ms: $mettle_compile_ms}' \
            >>"$COMPILE_NDJSON"

        log "  compile  Mettle profile total: $(format_number "$total_ms" 0) ms"
    done
fi

# --- summary ----------------------------------------------------------------

RESULTS_JSON=$(jq -s -c . "$RESULTS_NDJSON")
COMPILE_JSON=$(jq -s -c . "$COMPILE_NDJSON")

SUMMARY_JSON=$(jq -c '
    def geomean: [.[] | select(. != null and . > 0)] |
        if length == 0 then null
        else ((map(log) | add / length | exp) * 1000 | round) / 1000 end;
    {
        runtime_wins: ([.[] | select(.relative != null and .relative < 0.95)] | length),
        runtime_losses: ([.[] | select(.relative != null and .relative > 1.05)] | length),
        runtime_parity: ([.[] | select(.relative != null and .relative >= 0.95 and .relative <= 1.05)] | length),
        runtime_geomean: (map(.relative) | geomean),
        compile_geomean: (map(.compile_relative) | geomean),
        size_geomean: (map(.size_relative) | geomean),
        fastest_win: ([.[] | select(.relative != null and .relative < 0.95)] |
            if length == 0 then null else (min_by(.relative) | {name, relative}) end),
        slowest_loss: ([.[] | select(.relative != null and .relative > 1.05)] |
            if length == 0 then null else (max_by(.relative) | {name, relative}) end)
    }' <<<"$RESULTS_JSON")

if [[ $QUIET -eq 0 ]]; then
    echo ""
    echo "=== Runtime summary (Mettle vs C, median) ==="
    printf '%-14s %12s %12s %8s %8s %8s\n' "benchmark" "mettle" "c" "runtime" "compile" "size"
    jq -r '.[] | [
        .name,
        (if .mettle_ms != null then (.mettle_ms | tostring) + " ms" else "FAIL" end),
        (if .c_ms != null then (.c_ms | tostring) + " ms" else "FAIL" end),
        (if .relative != null then (.relative | tostring) + "x" else "FAIL" end),
        (if .compile_relative != null then (.compile_relative | tostring) + "x" else "FAIL" end),
        (if .size_relative != null then (.size_relative | tostring) + "x" else "FAIL" end)
    ] | @tsv' <<<"$RESULTS_JSON" |
    while IFS=$'\t' read -r n m c r cr sr; do
        printf '%-14s %12s %12s %8s %8s %8s\n' "$n" "$m" "$c" "$r" "$cr" "$sr"
    done

    if [[ $(jq 'length' <<<"$COMPILE_JSON") -gt 0 ]]; then
        echo ""
        echo "=== Compile-only summary (Mettle --profile total) ==="
        printf '%-14s %12s\n' "benchmark" "compile_ms"
        jq -r '.[] | [.name, (.mettle_compile_ms | tostring)] | @tsv' <<<"$COMPILE_JSON" |
        while IFS=$'\t' read -r n ms; do
            printf '%-14s %12s\n' "$n" "$ms"
        done
    fi
fi

# --- output -----------------------------------------------------------------

FAILED_JSON=$(printf '%s\n' "${FAILED[@]+"${FAILED[@]}"}" | jq -R . | jq -s -c 'map(select(. != ""))')

PAYLOAD=$(jq -n \
    --arg generated "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
    --argjson host "$HOST_JSON" \
    --argjson summary "$SUMMARY_JSON" \
    --arg config "$CONFIG_PATH" \
    --arg mode "$(jq -r '.mode // "mettle-vs-c"' <<<"$CONFIG")" \
    --argjson runs "$EFFECTIVE_RUNS" \
    --argjson warmup "$EFFECTIVE_WARMUP" \
    --argjson compile_only "$([[ $COMPILE_ONLY -eq 1 ]] && echo true || echo false)" \
    --arg compiler "$COMPILER" \
    --arg c_compiler "$C_COMPILER" \
    --argjson mettle_flags "$METTLE_FLAGS_JSON" \
    --argjson c_flags "$C_FLAGS_JSON" \
    --argjson benchmarks "$RESULTS_JSON" \
    --argjson compile_benchmarks "$COMPILE_JSON" \
    --argjson failed "$FAILED_JSON" \
    '{
        generated: $generated,
        host: $host,
        summary: $summary,
        harness: {
            config: $config, mode: $mode, runs: $runs, warmup: $warmup,
            compile_only: $compile_only, compiler: $compiler,
            c_compiler: $c_compiler, mettle_flags: $mettle_flags, c_flags: $c_flags
        },
        benchmarks: $benchmarks,
        compile_benchmarks: $compile_benchmarks,
        failed: $failed
    }')

primary_rel=$(jq -r '.outputs.primary' <<<"$CONFIG")
primary_path="$ROOT/$(linux_path "$primary_rel")"
mkdir -p "$(dirname "$primary_path")"
printf '%s\n' "$PAYLOAD" >"$primary_path"

mirror_rel=$(jq -r '.outputs.mirror_web // ""' <<<"$CONFIG")
if [[ -n "$mirror_rel" ]]; then
    mirror_path="$ROOT/$(linux_path "$mirror_rel")"
    mkdir -p "$(dirname "$mirror_path")"
    printf '%s\n' "$PAYLOAD" >"$mirror_path"
fi

log ""
log "Wrote $primary_path"
[[ -n "$mirror_rel" ]] && log "Mirrored to $mirror_path"

# --- HTML report (needs PowerShell Core) -------------------------------------

if [[ $NO_REPORT -eq 0 ]]; then
    if command -v pwsh >/dev/null 2>&1; then
        report_rel=$(jq -r '.outputs.report_html // ""' <<<"$CONFIG")
        [[ -z "$report_rel" ]] && report_rel="${primary_rel%.json}.html"
        report_args=(-File "$SCRIPT_DIR/generate-report.ps1" -InputPath "$primary_rel" -OutputPath "$report_rel")
        [[ $OPEN_REPORT -eq 1 ]] && report_args+=(-OpenReport)
        if ! pwsh "${report_args[@]}"; then
            log "Report generation failed."
        fi
    else
        log "HTML report skipped (pwsh not on PATH); JSON output is complete."
    fi
fi

if [[ $QUIET -eq 0 && $(jq '.runtime_geomean != null' <<<"$SUMMARY_JSON") == "true" ]]; then
    echo ""
    printf 'Summary: %s faster, %s parity, %s slower | runtime geomean %sx | compile geomean %sx | size geomean %sx\n' \
        "$(jq -r '.runtime_wins' <<<"$SUMMARY_JSON")" \
        "$(jq -r '.runtime_parity' <<<"$SUMMARY_JSON")" \
        "$(jq -r '.runtime_losses' <<<"$SUMMARY_JSON")" \
        "$(format_number "$(jq -r '.runtime_geomean // empty' <<<"$SUMMARY_JSON")" 2)" \
        "$(format_number "$(jq -r '.compile_geomean // empty' <<<"$SUMMARY_JSON")" 2)" \
        "$(format_number "$(jq -r '.size_geomean // empty' <<<"$SUMMARY_JSON")" 2)"
fi

if [[ ${#FAILED[@]} -gt 0 ]]; then
    log ""
    log "Failed benchmarks: $(IFS=', '; echo "${FAILED[*]}")"
    exit 1
fi
