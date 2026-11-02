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
        for figure in ("2.4 times faster", "8192 tokens 2.29 against 3.85 s, 1.7 times", "alone 53.1 against 49.7", "code 140.5 against 99.3, 1.41 times", "prose 91.6 against 77.5, 1.18 times", "23.5 times less", "12.1 times less", "8.5 times less", "19 percent less", "2 requests 1.07 against 1.35", "4 requests 0.65 against 0.89", "0.38 against 0.61, 38 percent less", "16 requests 473 against 446"):
            require(figure in readme, "the Potentia README to still report " + figure)
        print("potentia figures: match the README")
    else:
        print("potentia figures: NOT CHECKED, set POTENTIA_README")

    suite3 = json.loads(read(os.path.join(EXHIBITS, "suite3.json")))
    require(len(suite3) == 16, "16 programs in suite3.json")
    geo = lambda xs: math.exp(sum(math.log(x) for x in xs) / len(xs))
    c_o3 = "%.2f" % geo([r["m"] / r["o3"] for r in suite3])
    c_native = "%.2f" % geo([r["m"] / r["native"] for r in suite3])
    readme = read(os.path.join(REPO, "README.md"))
    require(c_o3 + "\u00d7 the run time of clang -O3" in readme and c_native + "\u00d7 the run time of clang -O3 -march=native" in readme, "the README to carry the same averages")
    span = 0.35
    c_rows = ""
    for r in sorted(suite3, key=lambda x: x["m"] / x["o3"]):
        ratio = r["m"] / r["o3"]
        native = r["m"] / r["native"]
        require("| `%s` | %.2f\u00d7 | %.2f\u00d7 |" % (r["name"], ratio, native) in readme, "the README table to list " + r["name"])
        width = abs(ratio - 1) / span * 50
        kind = "down ahead" if ratio < 1 else "up"
        tick = 50 + (native - 1) / span * 50
        c_rows += '      <div class="s-row"><span>%s</span><span class="s-track"><i class="o3 %s" style="width:%.2f%%"></i><i class="nat" style="left:%.2f%%"></i></span><span class="val">%.2f&times;</span></div>\n' % (r["name"], kind, width, tick, ratio)

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
        "{{C_O3}}": c_o3,
        "{{C_NATIVE}}": c_native,
        "{{C_ROWS}}": c_rows,
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
    print("c averages: " + c_o3 + " " + c_native)
    for k in ("{{TARGET_DUMP}}", "{{MACHINE_OUT}}", "{{DEMAND_NOTE}}"):
        print(k.strip("{}").lower() + ": " + facts[k].replace("\n", " | ")[:150])
    print("explain: " + " | ".join(re.sub(r"<[^>]+>", "", r) for r in rows)[:420])


if __name__ == "__main__":
    main()
