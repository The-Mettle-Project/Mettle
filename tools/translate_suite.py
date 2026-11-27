#!/usr/bin/env python3

import argparse
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = ".exe" if os.name == "nt" else ""
METTLE = os.path.join(ROOT, "bin", "mettle" + EXE)
SUITE = os.path.join(ROOT, "tests", "translate")
SCRATCH = os.path.join(ROOT, ".tmp", "tests", "translate")


def run(cmd, cwd, timeout):
    try:
        done = subprocess.run(cmd, cwd=cwd, capture_output=True, timeout=timeout)
        return done.returncode, done.stdout.decode("utf-8", "replace"), done.stderr.decode("utf-8", "replace")
    except subprocess.TimeoutExpired:
        return -1, "", "timed out"


def first_error(text):
    for line in text.splitlines():
        if "error" in line.lower():
            return line.strip()
    lines = text.strip().splitlines()
    return lines[0] if lines else ""


def normalize(text):
    return text.replace("\r\n", "\n")


def check(source, keep):
    name = os.path.splitext(os.path.basename(source))[0]
    work = os.path.join(SCRATCH, name)
    os.makedirs(work, exist_ok=True)
    out = os.path.join(work, name + ".mettle")
    code, _, err = run([METTLE, "translate", source, "-o", out], work, 120)
    if code != 0:
        return "translate", first_error(err)
    binary = os.path.join(work, name + EXE)
    code, stdout, err = run([METTLE, "--build", out, "-o", binary], work, 300)
    if code != 0 or not os.path.exists(binary):
        return "compile", first_error(stdout + err)
    code, stdout, _ = run([binary], work, 60)
    expected_path = source + ".expected"
    if code != 0:
        return "run", "exit %d" % code
    if os.path.exists(expected_path):
        with open(expected_path, "rb") as f:
            expected = normalize(f.read().decode("utf-8", "replace"))
        if normalize(stdout) != expected:
            return "run", "output differs"
    return None, None


def collect(paths):
    sources = []
    for path in [os.path.abspath(p) for p in paths]:
        if os.path.isdir(path):
            for entry in sorted(os.listdir(path)):
                if entry.endswith(".c"):
                    sources.append(os.path.join(path, entry))
        else:
            sources.append(path)
    return sources


def main():
    parser = argparse.ArgumentParser(description="Translate C programs, build them and compare their output.")
    parser.add_argument("paths", nargs="*", help="C files or directories (default: tests/translate)")
    parser.add_argument("--keep", action="store_true", help="keep the scratch directories")
    args = parser.parse_args()
    paths = args.paths or [os.path.join(SUITE, d) for d in sorted(os.listdir(SUITE))
                           if os.path.isdir(os.path.join(SUITE, d))]
    failures = []
    sources = collect(paths)
    for source in sources:
        stage, detail = check(source, args.keep)
        if stage:
            failures.append((source, stage, detail))
            print("FAIL %-9s %s: %s" % (stage, os.path.relpath(source, ROOT), detail))
    print("%d of %d programs translate, build and match" % (len(sources) - len(failures), len(sources)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
