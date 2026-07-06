"""Extract EVERY function from EVERY example program, labeled with the sound
teacher (liveness DCE + type-safe affine + GVN). Tagged by program so we can
split by program (no leakage). The teacher is sound on real code (calls/floats/
arrays included: GVN only reuses pure arithmetic, affine is float-type-gated,
liveness preserves side effects), so no manual labels are needed.

Output: real_all_act.jsonl  (one record per unique function body)
Train on functions whose program is NOT in the held-out TEST set; the existing
real_nocall_act.jsonl (from TEST programs) stays the unseen evaluation set."""
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "fuzz"))
import canonicalize as C
import fn_verify as V
from build_pairs import COMPILER
from make_action_labels import action_labels

# programs the held-out real_nocall test set is drawn from -> never train on them
TEST_PROGS = {"aos_sum", "byte_hash", "collatz", "func_ptr", "global_acc",
              "hexdump", "inference", "lcg_rng", "matrix_mul", "memcmp_bench",
              "popcount", "prime_count", "quant_matmul", "sort_insertion",
              "struct_byval", "sum_squares", "switch_vm", "tracy_demo",
              "transpose", "ui_demo", "word_count"}

progs = sorted(d for d in os.listdir(os.path.join(ROOT, "examples"))
               if os.path.isdir(os.path.join(ROOT, "examples", d)))
tmp = tempfile.mkdtemp(prefix="realall_")
out = open(os.path.join(HERE, "real_all_act.jsonl"), "w", encoding="utf-8")
seen = set()
n = ntrain = ngvn = 0
counts = [0] * 5
for ex in progs:
    src = os.path.join(ROOT, "examples", ex, f"{ex}.mettle")
    if not os.path.exists(src):
        continue
    exe = os.path.join(tmp, f"{ex}.exe")
    try:
        subprocess.run([COMPILER, "--build", "--emit-obj", "--linker", "internal",
                        "--dump-ir", "--release", src, "-o", exe],
                       capture_output=True, text=True, timeout=180)
    except Exception:
        continue
    stem = os.path.splitext(exe)[0]
    dump = None
    for cand in (stem + ".obj.ir", exe + ".ir", stem + ".ir"):
        if os.path.exists(cand):
            dump = open(cand, encoding="utf-8", errors="replace").read()
    if not dump:
        continue
    try:
        funcs = C.canonical_program(dump)
    except Exception:
        continue
    for fn, body in funcs.items():
        if fn == "print_int" or len(body) < 5:
            continue
        key = "\n".join(body)
        if key in seen:
            continue
        seen.add(key)
        try:
            params = V.func_params({fn: body}, fn)
            acts = action_labels(body, params)
        except Exception:
            continue
        for a in acts:
            counts[a] += 1
        ngvn += sum(1 for a in acts if a == 4)
        rec = {"seed": f"{ex}::{fn}", "prog": ex, "params": params,
               "funcs": [{"instrs": body, "action": acts}]}
        out.write(json.dumps(rec) + "\n")
        n += 1
        if ex not in TEST_PROGS:
            ntrain += 1
out.close()
tot = max(1, sum(counts))
print(f"extracted {n} unique real functions ({ntrain} from train programs)")
print(f"GVN-labeled instructions: {ngvn}")
print({nm: f"{counts[i]} ({100*counts[i]//tot}%)" for i, nm in
       enumerate(["KEEP", "DELETE", "FOLD", "AFFINE", "GVN"])})
