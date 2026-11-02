import base64
import html
import json
import math
import os
import re
import shutil
import subprocess
import sys

import live_session

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
EXHIBITS = os.path.join(HERE, "exhibits")
WORK = os.path.join(REPO, ".tmp", "website-build")
METTLE = os.environ.get("METTLE", os.path.join(REPO, "bin", "mettle.exe" if os.name == "nt" else "mettle"))
EXE = ".exe" if os.name == "nt" else ""
CUT = "⋯"


def read(path):
    with open(path, "rb") as f:
        return f.read().decode("utf-8").replace("\r\n", "\n")


def write(path, text):
    with open(path, "wb") as f:
        f.write(text.encode("utf-8"))


def read_b64(*parts):
    with open(os.path.join(HERE, *parts), "rb") as f:
        return base64.b64encode(f.read()).decode("ascii")


def esc(text):
    return html.escape(text, quote=False)


def require(cond, what):
    if not cond:
        sys.exit("build.py: expected " + what)


def run(cwd, argv):
    proc = subprocess.run(argv, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return proc.stdout.decode("utf-8", "replace").replace("\r\n", "\n"), proc.returncode


def stage(run_id, sources):
    cwd = os.path.join(WORK, run_id)
    shutil.rmtree(cwd, ignore_errors=True)
    os.makedirs(cwd)
    for source, name in sources:
        shutil.copyfile(source, os.path.join(cwd, name))
    return cwd


def compile_in(run_id, source, args, name="checksum.mettle"):
    cwd = stage(run_id, [(os.path.join(EXHIBITS, source), name)])
    out, code = run(cwd, [METTLE] + args + [name])
    return out, code, cwd


def explain_lines(out):
    rows = []
    for line in out.split("\n"):
        m = re.match(r"^  (\w+) \((loop) @ line (\d+)\): (.+)$", line)
        if not m:
            continue
        head = esc(line[: m.start(4)].strip())
        verdict = esc(m.group(4))
        if m.group(4).startswith("NOT"):
            verdict = '<span class="warn">' + verdict + "</span>"
        elif m.group(4).startswith("vectorized"):
            verdict = '<span class="ok">' + verdict + "</span>"
        rows.append(head + " " + verdict)
    return rows


def main():
    if not os.path.exists(METTLE):
        sys.exit("build.py: no compiler at " + METTLE + "; set METTLE")
    os.makedirs(WORK, exist_ok=True)

    potentia = os.environ.get("POTENTIA_README")
    if potentia:
        readme = read(potentia)
        for figure in ("35.7 ms against 87.1 ms", "code 140.5 against 99.3", "1.8 against 21.5 GB", "8 requests 0.38 against 0.61"):
            require(figure in readme, "the Potentia README to still report " + figure)
        print("potentia figures: match the README")
    else:
        print("potentia figures: NOT CHECKED, set POTENTIA_README")

    perf = os.path.join(WORK, "..", "perf", "fair.json")
    if os.path.exists(perf):
        rows = json.loads(read(perf))
        geo = lambda xs: math.exp(sum(math.log(x) for x in xs) / len(xs))
        by = {r["name"]: r for r in rows}
        require(len(rows) == 16 and round(geo([r["m"] / r["o3"] for r in rows]), 2) == 1.09, "Suite 3 to still average 1.09x clang -O3")
        require(round(geo([r["m"] / r["native"] for r in rows]), 2) == 1.13, "Suite 3 to still average 1.13x clang -march=native")
        require(round(by["bytecode_vm"]["o3"] / by["bytecode_vm"]["m"], 2) == 1.42, "bytecode_vm to still run 1.42x faster")
        require(round(by["sudoku_solve"]["o3"] / by["sudoku_solve"]["m"], 2) == 1.22, "sudoku_solve to still run 1.22x faster")
        print("c figures: match .tmp/perf/fair.json")
    else:
        print("c figures: NOT CHECKED, no .tmp/perf/fair.json")

    explain, code, _ = compile_in("explain", "checksum.mettle", ["--build", "--release", "--explain"])
    require(code == 0, "--explain to build")
    rows = explain_lines(explain)
    require(any("vpsadbw" in r for r in rows), "the checksum loop to vectorize to vpsadbw")
    trust = re.search(r"^  (\d+ things this build took on trust)\.", explain, re.M)
    require(trust, "the beliefs section to count what the build took on trust")
    rows.append(esc("beliefs: " + trust.group(1)))

    broken, code, _ = compile_in("contract", "checksum-early-exit.mettle", ["--build", "--release"])
    refusal = re.search(r"error: @simd! loop was not vectorized.*$", broken, re.M)
    require(code != 0 and refusal, "@simd! to stop the build on an early exit")
    demand = read(os.path.join(EXHIBITS, "checksum-early-exit.mettle")).split("\n")
    require(demand[6].strip().startswith("if (data[i] == 0)"), "the early exit on line 7")

    early, code, _ = compile_in("free", "checksum-free-early.mettle", ["--build", "--release"])
    warning = re.search(r"^warning\[M0101\]: .*$", early, re.M)
    finished = re.search(r"^Built executable .*$", early, re.M)
    require(code == 0 and warning and finished, "M0101 to warn and the build to finish")
    freed = read(os.path.join(EXHIBITS, "checksum-free-early.mettle")).split("\n")
    require(freed[16].strip() == "free(data);" and freed[17].strip().startswith("println"), "the early free on lines 17 and 18")

    rule_src = read(os.path.join(EXHIBITS, "rules.mettle")).split("\n")
    ruled, code, _ = compile_in("rule", "rules.mettle", ["--build"], "rules.mettle")
    rule_error = re.search(r"^error\[R0002\]: .*$", ruled, re.M)
    require(code != 0 and rule_error, "R0002 from the recursion rule")

    cwd = stage("target", [(os.path.join(EXHIBITS, "add.mettle"), "add.mettle")])
    stock, code = run(cwd, [METTLE, "target", "x86_64-none"])
    require(code == 0, "mettle target to print x86_64-none")
    forward = 'int_args: ["rdi", "rsi", "rdx", "rcx", "r8", "r9"]'
    backward = 'int_args: ["r9", "r8", "rcx", "rdx", "rsi", "rdi"]'
    require(forward in stock, "the printed target to list its integer argument registers")
    write(os.path.join(cwd, "stock.mettle"), stock)
    write(os.path.join(cwd, "mine.mettle"), stock.replace(forward, backward).replace('indirect_return: "rdi"', 'indirect_return: "r9"'))
    _, code = run(cwd, [METTLE, "add.mettle", "--target", "stock.mettle", "--emit-obj", "-o", "stock.o"])
    require(code == 0, "the printed target to build")
    _, code = run(cwd, [METTLE, "add.mettle", "--target", "mine.mettle", "--emit-obj", "-o", "add.o"])
    require(code == 0, "the edited target to build")
    disasm, _ = run(cwd, ["objdump", "-d", "-M", "intel", "add.o"])
    moves = re.findall(r"\t(mov\s+\w+,\s*(?:r9|r8|rcx))\s*$", disasm, re.M)
    moves = [re.sub(r"\s+", " ", m).replace(",", ", ").replace(",  ", ", ") for m in moves]
    require(len(moves) >= 3 and "r9" in moves[0] and "r8" in moves[1] and "rcx" in moves[2], "the edited target to read arguments from r9, r8 and rcx, found " + "; ".join(moves))

    little_path = os.path.join(REPO, "examples", "machine", "little.mettle")
    cwd = stage("machine", [(little_path, "little.mettle")])
    emulated, code = run(cwd, [METTLE, "emulate", "little.mettle"])
    require(code == 0, "mettle emulate to run little.mettle")
    little = read(little_path).split("\n")
    at_add = next(i for i, l in enumerate(little) if l.startswith("fn ins_add"))
    at_program = next(i for i, l in enumerate(little) if l.startswith("const PROGRAM"))
    machine_code = little[at_add:at_add + 4] + [""] + little[at_program:at_program + 3] + ["  " + CUT]
    machine_out = [l.rstrip() for l in emulated.split("\n") if re.match(r"^r\d+ = ", l)]
    require(machine_out, "the invented machine to print its registers")

    gpu_dir = os.path.join(REPO, "examples", "gpu_vadd")
    kernel_src = read(os.path.join(gpu_dir, "vadd_kernel.mettle")).strip()
    launch = next(l.strip() for l in read(os.path.join(gpu_dir, "vadd_host.mettle")).split("\n") if l.strip().startswith("dispatch "))
    cwd = stage("ptx", [(os.path.join(gpu_dir, "vadd_kernel.mettle"), "vadd_kernel.mettle")])
    _, code = run(cwd, [METTLE, "--emit-ptx", "vadd_kernel.mettle", "-o", "vadd_kernel.ptx"])
    ptx_path = os.path.join(cwd, "vadd_kernel.ptx")
    require(code == 0 and os.path.exists(ptx_path) and ".entry vadd" in read(ptx_path), "vadd_kernel to compile to a PTX entry")

    cached = os.path.join(WORK, "live-session.json")
    if "--reuse-live" in sys.argv and os.path.exists(cached):
        session = json.loads(read(cached))
    else:
        session = live_session.record(REPO, METTLE, WORK)
        write(cached, json.dumps(session, ensure_ascii=False))

    version = run(REPO, [METTLE, "--version"])[0].split("\n")[0]
    data = {k: session[k] for k in ("duration", "points", "events", "plateau")}
    facts = {
        "{{VERSION}}": version,
        "{{DEMAND_CODE}}": esc("\n".join(demand[3:7] + ["    " + CUT])),
        "{{DEMAND_NOTE}}": esc(refusal.group(0)),
        "{{EXPLAIN_LINES}}": "\n".join(rows),
        "{{RULE_CODE}}": esc("\n".join(rule_src[3:7] + ["  " + CUT])),
        "{{RULE_NOTE}}": esc(rule_error.group(0)),
        "{{FREE_CODE}}": esc(freed[13].strip() + "\n" + freed[16].strip() + "\n" + freed[17].strip()),
        "{{FREE_NOTE}}": esc(warning.group(0)),
        "{{FREE_BUILT}}": esc(finished.group(0)),
        "{{TARGET_LINE}}": esc(backward + ","),
        "{{TARGET_DUMP}}": esc("\n".join(moves[:3])),
        "{{MACHINE_CODE}}": esc("\n".join(machine_code)),
        "{{MACHINE_OUT}}": esc("$ mettle emulate little.mettle\n" + "\n".join(machine_out[-3:])),
        "{{GPU_CODE}}": esc(kernel_src.replace("b: float32*, c:", "b: float32*,\n" + " " * 25 + "c:") + "\n\n" + launch),
        "{{LIVE_FIX_MS}}": session["facts"]["{{LIVE_FIX_MS}}"],
        "{{LIVE_WAIT}}": session["facts"]["{{LIVE_WAIT}}"],
    }

    page = read(os.path.join(HERE, "template.html"))
    page = page.replace("{{DATA}}", json.dumps(data, separators=(",", ":")))
    page = page.replace("{{SERIF_WOFF2}}", read_b64("fonts", "GeistSerif-Regular.woff2"))
    page = page.replace("{{HERO_POSTER}}", read_b64("media", "hero-poster.jpg"))
    for k, v in facts.items():
        page = page.replace(k, v)
    left = re.findall(r"\{\{[A-Z0-9_]+\}\}", page)
    require(not left, "every placeholder filled, found " + ", ".join(sorted(set(left))))
    write(os.path.join(HERE, "index.html"), page)
    print("wrote website/index.html")
    for k in ("{{TARGET_DUMP}}", "{{MACHINE_OUT}}", "{{DEMAND_NOTE}}"):
        print(k.strip("{}").lower() + ": " + facts[k].replace("\n", " | ")[:150])
    print("explain: " + " | ".join(re.sub(r"<[^>]+>", "", r) for r in rows)[:420])


if __name__ == "__main__":
    main()
