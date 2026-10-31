import datetime
import difflib
import json
import os
import re
import shutil
import subprocess
import sys

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


def clean(text):
    lines = [line.rstrip().expandtabs(8) for line in text.replace("\r\n", "\n").split("\n")]
    while lines and not lines[-1]:
        lines.pop()
    return "\n".join(lines)


def fail(what):
    sys.exit("build.py: expected " + what)


def require(cond, what):
    if not cond:
        fail(what)


def compile_in(run_id, source, shown_name, args):
    cwd = os.path.join(WORK, run_id)
    shutil.rmtree(cwd, ignore_errors=True)
    os.makedirs(cwd)
    shutil.copyfile(os.path.join(EXHIBITS, source), os.path.join(cwd, shown_name))
    proc = subprocess.run([METTLE] + args, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return {"cmd": "mettle " + " ".join(args), "out": clean(proc.stdout.decode("utf-8", "replace")), "exit": proc.returncode}, cwd


def execute(cwd, argv, shown):
    exe = argv[0] if os.path.isabs(argv[0]) else shutil.which(argv[0])
    if not exe:
        sys.exit("build.py: " + argv[0] + " is not on PATH")
    proc = subprocess.run([exe] + argv[1:], cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return {"cmd": shown, "out": clean(proc.stdout.decode("utf-8", "replace")), "exit": proc.returncode}


def excerpt(run, keep):
    lines = run["out"].split("\n")
    picked = []
    last = -1
    for i, l in enumerate(lines):
        if keep(l):
            if last != -1 and i != last + 1:
                picked.append("  " + CUT)
            elif last == -1 and i != 0:
                picked.append(CUT)
            picked.append(l)
            last = i
    require(picked, "an excerpt from `" + run["cmd"] + "`")
    if last != len(lines) - 1:
        picked.append(CUT)
    return dict(run, out="\n".join(picked))


def diff_view(base, changed, context):
    a = base.split("\n")
    b = changed.split("\n")
    marked = []
    for tag, i1, i2, j1, j2 in difflib.SequenceMatcher(None, a, b).get_opcodes():
        if tag in ("replace", "insert"):
            marked.extend(range(j1 + 1, j2 + 1))
    require(marked, "a change between the exhibit and its base")
    lo = max(1, min(marked) - context)
    hi = min(len(b), max(marked) + context)
    return {"lines": [{"n": n, "text": b[n - 1], "mark": n in marked} for n in range(lo, hi + 1)]}


def main():
    if not os.path.exists(METTLE):
        sys.exit("build.py: no compiler at " + METTLE + "; set METTLE")
    os.makedirs(WORK, exist_ok=True)
    base = read(os.path.join(EXHIBITS, "checksum.mettle"))
    release = ["--build", "--release", "checksum.mettle"]

    built, cwd = compile_in("build", "checksum.mettle", "checksum.mettle", release)
    require(built["exit"] == 0, "checksum.mettle to build")
    ran = execute(cwd, [os.path.join(cwd, "checksum" + EXE)], "./checksum")
    require(ran["out"] == "checksum = 130560", "checksum to print 130560")
    size = os.path.getsize(os.path.join(cwd, "checksum" + EXE))

    explain, _ = compile_in("explain", "checksum.mettle", "checksum.mettle", ["--build", "--release", "--explain", "checksum.mettle"])
    require(explain["exit"] == 0, "--explain to build")
    explain = excerpt(explain, lambda l: re.match(r"^  \w+ \(loop @ line \d+\)", l) is not None)
    require("vpsadbw" in explain["out"], "the checksum loop to vectorize to vpsadbw")

    broken, _ = compile_in("contract", "checksum-early-exit.mettle", "checksum.mettle", release)
    require(broken["exit"] != 0 and "@simd!" in broken["out"], "@simd! to stop the build on an early exit")

    early, _ = compile_in("free", "checksum-free-early.mettle", "checksum.mettle", release)
    require("M0101" in early["out"], "M0101 for the early free")
    early = excerpt(early, lambda l: "M0101" in l or re.match(r"^\s+1[78] \|", l) is not None or re.match(r"^\s+:\s+\^", l) is not None)

    dump = execute(cwd, ["objdump", "-p", "checksum" + EXE], "objdump -p checksum" + EXE)
    dlines = dump["out"].split("\n")
    dlls = [l.split("DLL Name:")[1].strip() for l in dlines if "DLL Name:" in l]
    first = next(i for i, l in enumerate(dlines) if "DLL Name" in l)
    imports = []
    for l in dlines[first + 2:]:
        if not l.strip():
            break
        imports.append(l)
    require(dlls and imports, "an import table in checksum" + EXE)
    shown = [dlines[first]] + imports[:5]
    dump = dict(dump, out=CUT + "\n" + "\n".join(shown) + "\n        " + CUT + " " + str(len(imports) - 5) + " more from the same DLL\n" + CUT)

    vol_bad, _ = compile_in("types", "volume.mettle", "volume.mettle", ["--build", "volume.mettle"])
    vol_ok, _ = compile_in("types-ok", "volume-guarded.mettle", "volume.mettle", ["--build", "volume.mettle"])
    require(vol_bad["exit"] != 0 and vol_ok["exit"] == 0, "the unguarded Percent to fail and the guarded one to build")
    mixer, _ = compile_in("effects", "mixer.mettle", "mixer.mettle", ["--build", "mixer.mettle"])
    require("F0001" in mixer["out"], "F0001 for the mixer")
    rules, _ = compile_in("rules", "rules.mettle", "rules.mettle", ["--build", "rules.mettle"])
    require("R0002" in rules["out"], "R0002 for the recursion rule")
    verify, _ = compile_in("verify", "checksum.mettle", "checksum.mettle", ["--build", "--release", "--verify", "checksum.mettle"])
    require(verify["exit"] == 0 and "translation validation" in verify["out"], "--verify to report")

    version = clean(subprocess.run([METTLE, "--version"], stdout=subprocess.PIPE).stdout.decode()).split("\n")
    target = next((l.split(":", 1)[1].strip() for l in version if l.startswith("target:")), "")

    data = {
        "source": {"file": "checksum.mettle", "src": base},
        "build": {"runs": [built, ran], "link": True},
        "explain": {"runs": [explain], "link": True},
        "contract": {"diff": diff_view(base, read(os.path.join(EXHIBITS, "checksum-early-exit.mettle")), 1), "runs": [broken]},
        "free": {"diff": diff_view(base, read(os.path.join(EXHIBITS, "checksum-free-early.mettle")), 1), "runs": [early]},
        "ship": {"runs": [dump]},
        "types": {"runs": [vol_bad]},
        "effects": {"runs": [mixer]},
        "rules": {"runs": [rules]},
        "verify": {"runs": [verify]},
    }

    facts = {
        "{{VERSION_SHORT}}": version[0].split()[-1],
        "{{VERSION}}": version[0],
        "{{TARGET}}": target,
        "{{DATE}}": datetime.date.today().strftime("%d %B %Y").lstrip("0"),
        "{{SIZE}}": "{:,}".format(size),
        "{{DLLS}}": "one DLL" if len(dlls) == 1 else str(len(dlls)) + " DLLs",
        "{{DLL}}": dlls[0],
        "{{IMPORTS}}": str(len(imports)),
    }
    blob = json.dumps(data, ensure_ascii=False, separators=(",", ":")).replace("</", "<\\/")
    page = read(os.path.join(HERE, "template.html")).replace("{{DATA}}", blob)
    for k, v in facts.items():
        page = page.replace(k, v)
    left = re.findall(r"\{\{[A-Z_]+\}\}", page)
    require(not left, "every placeholder filled, found " + ", ".join(sorted(set(left))))
    with open(os.path.join(HERE, "index.html"), "wb") as f:
        f.write(page.encode("utf-8"))
    print("wrote website/index.html: " + ", ".join(k.strip("{}").lower() + "=" + v for k, v in facts.items()))


if __name__ == "__main__":
    main()
