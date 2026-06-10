#!/usr/bin/env python3
"""Line-level delta reducer for a divergent Mettle program.

Repeatedly tries deleting individual lines (and balanced brace blocks) from a
program while the debug-vs-release exit-code divergence is preserved. Prints the
minimized program. Purely structural -- it does not understand Mettle, so it
relies on the build failing (and thus being rejected as a non-repro) when a
deletion produces invalid syntax.

Usage: python reduce.py <file.mettle> [--compiler PATH]
"""
import argparse
import os
import subprocess
import sys

DEFAULT_COMPILER = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "..", "bin", "mettle.exe")


def build_and_run(compiler, text, release):
    tmp = os.environ.get("TEMP", "/tmp")
    # PID-unique paths so concurrent reducers don't clobber each other's
    # source/exe (a shared path produces spurious "does not diverge" results).
    tag = os.getpid()
    src = os.path.join(tmp, f"reduce_{tag}.mettle")
    exe = os.path.join(tmp, "reduce_%d_%s.exe" % (tag, "rel" if release else "dbg"))
    with open(src, "w") as f:
        f.write(text)
    args = [compiler, "--build", "--emit-obj", "--linker", "internal"]
    if release:
        args.append("--release")
    args += [src, "-o", exe]
    b = subprocess.run(args, capture_output=True, text=True, timeout=120)
    if b.returncode != 0:
        return None
    try:
        r = subprocess.run([exe], capture_output=True, text=True, timeout=30)
        return r.returncode
    except subprocess.TimeoutExpired:
        return "TIMEOUT"


def structurally_sane(text):
    """Reject candidates where deletion created a value-returning function
    with no `return` in its body: the compiler accepts those (missing-return
    is currently undiagnosed) and the garbage return value produces fake
    debug-vs-release 'divergences' that aren't miscompiles."""
    import re
    for m in re.finditer(r"->\s*\w+\s*\{", text):
        depth = 1
        i = m.end()
        body_start = i
        while i < len(text) and depth:
            if text[i] == "{":
                depth += 1
            elif text[i] == "}":
                depth -= 1
            i += 1
        if "return" not in text[body_start:i]:
            return False
    return True


def diverges(compiler, text, pinned_debug=None):
    if not structurally_sane(text):
        return False
    """A program 'diverges' if it builds at both levels, debug != release, AND
    (when pinned_debug is given) the debug result still equals the original
    program's debug result. Pinning is what keeps reduction semantics-preserving:
    a deletion that changes the debug oracle changed the program's meaning (or
    introduced UB / a missing return) and must be rejected, even if it happens to
    still differ from release."""
    d = build_and_run(compiler, text, False)
    if d is None:
        return False
    if pinned_debug is not None and d != pinned_debug:
        return False
    # Reject anything that crashes/faults at debug -- not a valid oracle.
    if isinstance(d, int) and (d < 0 or d > 255):
        return False
    r = build_and_run(compiler, text, True)
    if r is None:
        return False
    return d != r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("--compiler", default=DEFAULT_COMPILER)
    args = ap.parse_args()

    with open(args.file) as f:
        lines = f.read().splitlines()

    pinned = build_and_run(args.compiler, "\n".join(lines) + "\n", False)
    if not diverges(args.compiler, "\n".join(lines) + "\n", pinned):
        print("Input does not diverge; nothing to reduce.", file=sys.stderr)
        return 1
    print(f"// pinned debug exit code = {pinned}", file=sys.stderr)

    def test(ls):
        return diverges(args.compiler, "\n".join(ls) + "\n", pinned)

    # Phase 1: ddmin-style chunk removal. Try deleting contiguous chunks,
    # halving chunk size each round. Far fewer builds than line-by-line.
    n = max(len(lines) // 2, 1)
    while n >= 1:
        i = 0
        while i < len(lines):
            candidate = lines[:i] + lines[i + n:]
            if test(candidate):
                lines = candidate           # keep i; more may follow
            else:
                i += n
        n //= 2

    # Phase 2: single-line cleanup pass to finish the job.
    changed = True
    while changed:
        changed = False
        i = 0
        while i < len(lines):
            candidate = lines[:i] + lines[i + 1:]
            if test(candidate):
                lines = candidate
                changed = True
            else:
                i += 1

    print("\n".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
