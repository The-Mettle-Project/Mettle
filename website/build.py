import base64
import html
import json
import os
import re
import shutil
import subprocess
import sys

import live_session
import suite3_facts

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

    suite3 = suite3_facts.load(EXHIBITS)
    c_o3 = suite3["c_o3"]
    c_native = suite3["c_native"]
    readme = read(os.path.join(REPO, "README.md"))
    require(c_o3 + "\u00d7 the run time of clang -O3" in readme and c_native + "\u00d7 the run time of clang -O3 -march=native" in readme, "the README to carry the same averages")
    require("-march=native, which leads on " + suite3["native_leads"] + "." in readme, "the README to say clang leads on " + suite3["native_leads"])
    require("%s times faster to build a generated 90,000-line file, %s seconds against %s." % (suite3["build_ratio"], suite3["build_m"], suite3["build_c"]) in readme, "the README to carry the same build times")
    for r in suite3["rows"]:
        require("| `%s` | %.2f\u00d7 | %.2f\u00d7 |" % (r["name"], r["m"] / r["o3"], r["m"] / r["native"]) in readme, "the README table to list " + r["name"])

    built, code, cwd = compile_in("reflect", "reflect.mettle", ["--build"], "reflect.mettle")
    require(code == 0, "reflect.mettle to build")
    reflect_out, code = run(cwd, [os.path.join(cwd, "reflect" + EXE)])
    require(code == 0 and "OFFSET_len = 4" in reflect_out and "OFFSET_flags = 6" in reflect_out, "the generated constants, found " + reflect_out)
    reflect_src = read(os.path.join(EXHIBITS, "reflect.mettle")).split("\n")

    cwd = stage("trace", [(os.path.join(EXHIBITS, "trace.mettle"), "trace.mettle")])
    traced, code = run(cwd, [METTLE, "trace", "trace.mettle", "total", "4"])
    require(code == 0 and "returns 6" in traced and "sum = 0, 1, 3, 6" in traced, "mettle trace to follow total(4), found " + traced)
    trace_lines = [l.rstrip() for l in traced.split("\n") if re.match(r"^\s+\d+ \||^returns ", l)]

    percent, code, _ = compile_in("percent", "percent.mettle", ["--build"], "percent.mettle")
    percent_error = re.search(r"^error\[P0001\]: .*$", percent, re.M)
    require(code != 0 and percent_error, "P0001 from the unproven Percent")
    percent_src = read(os.path.join(EXHIBITS, "percent.mettle")).split("\n")

    effected, code, _ = compile_in("effects", "effects.mettle", ["--build"], "effects.mettle")
    effect_error = re.search(r"^error\[F0001\]: .*$", effected, re.M)
    require(code != 0 and effect_error and "mix_audio -> gain -> log_line" in effect_error.group(0), "F0001 with the chain to log_line")
    effect_src = read(os.path.join(EXHIBITS, "effects.mettle")).split("\n")

    late, code, _ = compile_in("deadline", "deadline.mettle", ["--build"], "deadline.mettle")
    late_error = re.search(r"^error\[D0001\]: .*$", late, re.M)
    require(code != 0 and late_error, "D0001 from the 20 cycle deadline")

    twin, code, _ = compile_in("twins", "twins.mettle", ["--build"], "twins.mettle")
    twin_error = re.search(r"^error\[T0001\]: .*$", twin, re.M)
    require(code != 0 and twin_error, "T0001 from the disagreeing twins")

    rewritten, code, _ = compile_in("rewrite", "rewrite.mettle", ["--build", "--release"], "rewrite.mettle")
    rewrite_error = re.search(r"^.*error: rewrite `drop_clamp` changes meaning.*\n\s*(counterexample .*)\n\s*(divergence: .*)$", rewritten, re.M)
    require(code != 0 and rewrite_error, "the rewrite to be refused with a counterexample, found " + rewritten)

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

    live_lines = []
    for pattern, kind in ((r"^Fatal error", "err"), (r"main stopped at the fault", "dim"), (r"^saved ", "ok"), (r"main continues from", "dim"), (r"^record 1500000 held", ""), (r"^checksum \d+", "")):
        line = next((l for l in session["lines"] if re.search(pattern, l["s"])), None)
        require(line, "the recording to hold /" + pattern + "/")
        text = "%6.2f  %s" % (line["t"], esc(line["s"]))
        live_lines.append('<span class="%s">%s</span>' % (kind, text) if kind else text)

    version = run(REPO, [METTLE, "--version"])[0].split("\n")[0]
    facts = {
        "{{VERSION}}": version,
        "{{DOCS}}": "https://github.com/The-Mettle-Project/Mettle/blob/master/docs/",
        "{{C_O3}}": c_o3,
        "{{C_NATIVE}}": c_native,
        "{{C_LOW}}": suite3["c_low"],
        "{{C_HIGH}}": suite3["c_high"],
        "{{C_NATIVE_LEADS}}": suite3["native_leads"],
        "{{BUILD_M}}": suite3["build_m"],
        "{{BUILD_C}}": suite3["build_c"],
        "{{REFLECT_CODE}}": esc("\n".join(reflect_src[2:7] + [""] + [l[2:] for l in reflect_src[9:11]])),
        "{{REFLECT_OUT}}": esc(reflect_out.strip()),
        "{{TRACE_OUT}}": esc("\n".join(trace_lines)),
        "{{PERCENT_CODE}}": esc("\n".join([percent_src[2], ""] + [l[2:] for l in percent_src[5:7]])),
        "{{PERCENT_NOTE}}": esc(percent_error.group(0)),
        "{{EFFECT_CODE}}": esc("\n".join(effect_src[2:4] + [effect_src[5]])),
        "{{EFFECT_NOTE}}": esc(effect_error.group(0)),
        "{{DEADLINE_NOTE}}": esc(late_error.group(0)),
        "{{TWIN_NOTE}}": esc(twin_error.group(0)),
        "{{REWRITE_NOTE}}": esc(rewrite_error.group(0).split(": ", 1)[1].split("\n")[0] + "\n" + rewrite_error.group(1) + "\n" + rewrite_error.group(2)),
        "{{LIVE_LINES}}": "\n".join(live_lines),
        "{{DEMAND_NOTE}}": esc(refusal.group(0)),
        "{{EXPLAIN_LINES}}": "\n".join(rows),
        "{{RULE_CODE}}": esc("\n".join(rule_src[3:7] + ["  " + CUT])),
        "{{RULE_NOTE}}": esc(rule_error.group(0)),
        "{{FREE_CODE}}": esc(freed[13].strip() + "\n" + freed[16].strip() + "\n" + freed[17].strip()),
        "{{FREE_NOTE}}": esc(warning.group(0)),
        "{{MACHINE_CODE}}": esc("\n".join(machine_code)),
        "{{MACHINE_OUT}}": esc("$ mettle emulate little.mettle\n" + machine_out[0]),
        "{{GPU_CODE}}": esc(kernel_src.replace("b: float32*, c:", "b: float32*,\n" + " " * 25 + "c:") + "\n\n" + launch),
        "{{LIVE_FIX_MS}}": session["facts"]["{{LIVE_FIX_MS}}"],
    }

    page = read(os.path.join(HERE, "template.html"))
    page = page.replace("{{SERIF_WOFF2}}", read_b64("fonts", "GeistSerif-Regular.woff2"))
    page = page.replace("{{HERO_POSTER}}", read_b64("media", "hero-poster.jpg"))
    for k, v in facts.items():
        page = page.replace(k, v)
    left = re.findall(r"\{\{[A-Z0-9_]+\}\}", page)
    require(not left, "every placeholder filled, found " + ", ".join(sorted(set(left))))
    write(os.path.join(HERE, "index.html"), page)
    print("wrote website/index.html")
    print("c averages: " + c_o3 + " " + c_native)
    for k in ("{{MACHINE_OUT}}", "{{DEMAND_NOTE}}", "{{REFLECT_OUT}}", "{{PERCENT_NOTE}}", "{{EFFECT_NOTE}}", "{{DEADLINE_NOTE}}", "{{TWIN_NOTE}}", "{{REWRITE_NOTE}}"):
        print(k.strip("{}").lower() + ": " + facts[k].replace("\n", " | ")[:150])
    print("explain: " + " | ".join(re.sub(r"<[^>]+>", "", r) for r in rows)[:420])


if __name__ == "__main__":
    main()
