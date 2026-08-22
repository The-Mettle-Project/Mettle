#!/usr/bin/env bash
# Build every example in several codegen modes and require identical output.
#
# Debug is the oracle. When --release disagrees with debug but `-s --release`
# agrees, the fault is in the MIR backend; when both release modes agree with
# each other and not with debug, the fault is an IR pass.
#
# Usage: tools/examples-differential.sh [-j N] [-t SECONDS] [-m TAG:FLAGS]... [PATH]

set -u

JOBS=0
TIMEOUT=180
ROOT="examples"
MODES=()

while [ $# -gt 0 ]; do
  case "$1" in
    -j) JOBS="$2"; shift 2 ;;
    -t) TIMEOUT="$2"; shift 2 ;;
    -m) MODES+=("$2"); shift 2 ;;
    -h|--help) sed -n '2,8p' "$0"; exit 0 ;;
    *) ROOT="$1"; shift ;;
  esac
done

if [ ${#MODES[@]} -eq 0 ]; then
  MODES=("debug:" "release:--release" "fallback:-s --release")
fi
if [ "$JOBS" -eq 0 ]; then
  JOBS=$(nproc 2>/dev/null || echo 4)
fi

REPO=$(cd "$(dirname "$0")/.." && pwd)
METTLE="$REPO/bin/mettle.exe"
[ -x "$METTLE" ] || METTLE="$REPO/bin/mettle"
if [ ! -x "$METTLE" ]; then
  echo "no compiler at $METTLE" >&2
  exit 2
fi

WORK=$(mktemp -d 2>/dev/null || echo "/tmp/exdiff.$$")
mkdir -p "$WORK/results"
trap 'rm -rf "$WORK"' EXIT

# Every benchmark prints durations that never match run to run. Keep this tight:
# a loose pattern silently eats real output, which is how two timing-only
# divergences were mistaken for clean runs.
NOISE='(^|[^a-zA-Z])[Tt]ime[_[:alnum:]]*[[:space:]]*[=:]|[Pp]er (pass|call|op|iter|element)[[:space:]]*[=:]|[0-9][[:space:]]*(us|ns|ms)$|elapsed|cycles/|MB/s|[Tt]hroughput'

# Examples whose output legitimately drifts because a vectorized `+` reduction
# reassociates (docs/translation-validation.md). These are still compared, but
# number by number within RELTOL instead of byte for byte, so a real miscompile
# -- which moves results far further than a rounding step -- still fails.
RELAXED='examples/float32_sum/float32_sum.mettle'
RELTOL='0.000001'

MODE_LIST=$(printf '%s\n' "${MODES[@]}")
export WORK METTLE TIMEOUT NOISE MODE_LIST RELAXED RELTOL

# Non-numeric tokens must match exactly; numeric ones to within RELTOL.
numeric_match() {
  awk -v tol="$RELTOL" '
    function abs(x) { return x < 0 ? -x : x }
    NR == FNR { a[FNR] = $0; an = FNR; next }
    { b[FNR] = $0; bn = FNR }
    END {
      if (an != bn) { exit 1 }
      for (i = 1; i <= an; i++) {
        na = split(a[i], ta, /[[:space:]]+/)
        nb = split(b[i], tb, /[[:space:]]+/)
        if (na != nb) { exit 1 }
        for (j = 1; j <= na; j++) {
          if (ta[j] == tb[j]) { continue }
          if (ta[j] !~ /^[-+]?[0-9]/ || tb[j] !~ /^[-+]?[0-9]/) { exit 1 }
          x = ta[j] + 0
          y = tb[j] + 0
          d = abs(x - y)
          m = (abs(x) > abs(y)) ? abs(x) : abs(y)
          if (m == 0) { if (d > 0) { exit 1 } else { continue } }
          if (d / m > tol) { exit 1 }
        }
      }
      exit 0
    }
  ' "$1" "$2"
}
export -f numeric_match

run_one() {
  local source="$1"
  local slug
  slug=$(printf '%s' "$source" | tr '/\\.' '___')
  local report="$WORK/results/$slug"
  local out="$WORK/build/$slug"
  mkdir -p "$out"
  local outw
  outw=$(cygpath -w "$out" 2>/dev/null || printf '%s' "$out")

  # A GUI example runs a message loop until someone closes the window, so every
  # mode just burns the timeout and the comparison proves nothing.
  if grep -q '^[[:space:]]*import[[:space:]]*"std/ui"' "$source"; then
    printf 'SKIP\t%s\tinteractive (std/ui)\n' "$source" > "$report"
    return
  fi

  local reference="" reference_rc="" reference_tag=""
  local spec tag mode exe build raw got rc
  local began
  began=$(date +%s)
  while IFS= read -r spec; do
    [ -n "$spec" ] || continue
    tag="${spec%%:*}"
    mode="${spec#*:}"
    exe="$out/$tag.exe"

    # shellcheck disable=SC2086
    build=$(MSYS_NO_PATHCONV=1 "$METTLE" --build $mode "$source" -o "$outw/$tag.exe" 2>&1)
    if [ ! -s "$exe" ]; then
      local why
      why=$(printf '%s' "$build" |
            grep -m1 -E '^(error|warning\[|Code generation|Mettle internal)' |
            cut -c1-110)
      if [ -z "$reference_tag" ]; then
        printf 'SKIP\t%s\t%s\n' "$source" "$why" > "$report"
      else
        printf 'BUILD\t%s\t%s\t%s\n' "$source" "$tag" "$why" > "$report"
      fi
      return
    fi

    raw=$(timeout "$TIMEOUT" "$exe" 2>&1)
    rc=$?
    got=$(printf '%s\n' "$raw" | grep -viE "$NOISE")

    if [ -z "$reference_tag" ]; then
      reference="$got"; reference_rc="$rc"; reference_tag="$tag"
      continue
    fi
    # A timed-out run proves nothing about codegen, and under twenty parallel
    # jobs a benchmark that takes ten seconds alone can take minutes. Report it
    # as its own outcome rather than as an output divergence.
    if [ "$rc" = 124 ] || [ "$reference_rc" = 124 ]; then
      printf 'SLOW\t%s\t%s vs %s\ttimed out after %ss\n' \
        "$source" "$reference_tag" "$tag" "$TIMEOUT" > "$report"
      printf '%s\t%s\n' "$(( $(date +%s) - began ))" "$source" > "$report.time"
      return
    fi
    if [ "$got" != "$reference" ] || [ "$rc" != "$reference_rc" ]; then
      case " $RELAXED " in
        *" $source "*)
          if [ "$rc" = "$reference_rc" ]; then
            printf '%s\n' "$reference" > "$out/.ref"
            printf '%s\n' "$got" > "$out/.got"
            if numeric_match "$out/.ref" "$out/.got"; then
              printf 'NEAR\t%s\t%s vs %s\twithin %s (reduction reassociates)\n' \
                "$source" "$reference_tag" "$tag" "$RELTOL" > "$report"
              printf '%s\t%s\n' "$(( $(date +%s) - began ))" "$source" \
                > "$report.time"
              return
            fi
          fi
          ;;
      esac
      {
        printf 'DIFF\t%s\t%s vs %s\trc %s -> %s\n' \
          "$source" "$reference_tag" "$tag" "$reference_rc" "$rc"
        diff <(printf '%s\n' "$reference") <(printf '%s\n' "$got") |
          head -10 | sed 's/^/    /'
      } > "$report"
      printf '%s\t%s\n' "$(( $(date +%s) - began ))" "$source" > "$report.time"
      return
    fi
  done <<< "$MODE_LIST"

  printf 'OK\t%s\n' "$source" > "$report"
  printf '%s\t%s\n' "$(( $(date +%s) - began ))" "$source" > "$report.time"
}
export -f run_one

start=$(date +%s)
find "$ROOT" -name "*.mettle" | sort |
  xargs -P "$JOBS" -I{} bash -c 'run_one "$@"' _ {}
finish=$(date +%s)

ok=0; diverged=0; failed=0; skipped=0; near=0; slow=0
for report in "$WORK"/results/*; do
  [ -f "$report" ] || continue
  case "$report" in *.time) continue ;; esac
  case "$(head -c 4 "$report")" in
    OK*)   ok=$((ok + 1)); continue ;;
    DIFF)  diverged=$((diverged + 1)) ;;
    NEAR)  near=$((near + 1)) ;;
    SLOW)  slow=$((slow + 1)) ;;
    BUIL)  failed=$((failed + 1)) ;;
    SKIP)  skipped=$((skipped + 1)) ;;
  esac
  cat "$report"
done

if [ -n "${EXDIFF_SLOWEST:-}" ]; then
  echo
  echo "slowest (seconds, all modes):"
  cat "$WORK"/results/*.time 2>/dev/null | sort -rn | head -10 | sed 's/^/    /'
fi

echo
echo "examples: $ok ok, $near near, $slow timed-out, $diverged diverged," \
     "$failed build-failed, $skipped skipped ($JOBS jobs, $((finish - start))s)"

[ "$diverged" -eq 0 ] && [ "$failed" -eq 0 ]
