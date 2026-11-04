import json
import math
import os

EXHIBITS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "exhibits")
EXPECTED_NATIVE_DIFFERENCES = ["physics_grid"]


def load(directory=EXHIBITS):
    with open(os.path.join(directory, "suite3.json"), "rb") as f:
        rows = json.loads(f.read().decode("utf-8"))
    with open(os.path.join(directory, "build_speed.json"), "rb") as f:
        build = json.loads(f.read().decode("utf-8"))
    if len(rows) != 16:
        raise SystemExit("suite3_facts: expected 16 programs, found %d" % len(rows))
    wrong = [r["name"] for r in rows if not r["same_o3"]]
    if wrong:
        raise SystemExit("suite3_facts: different answers from clang -O3: " + ", ".join(wrong))
    differs = sorted(r["name"] for r in rows if not r["same_native"])
    if differs != EXPECTED_NATIVE_DIFFERENCES:
        raise SystemExit("suite3_facts: different answers from clang -march=native: " + ", ".join(differs))
    geo = lambda xs: math.exp(sum(math.log(x) for x in xs) / len(xs))
    ratios = [r["m"] / r["o3"] for r in rows]
    leads = sum(1 for r in rows if r["native"] < r["m"])
    return {
        "rows": sorted(rows, key=lambda r: r["m"] / r["o3"]),
        "c_o3": "%.2f" % geo(ratios),
        "c_native": "%.2f" % geo([r["m"] / r["native"] for r in rows]),
        "c_low": "%.2f" % min(ratios),
        "c_high": "%.2f" % max(ratios),
        "native_leads": "all 16" if leads == 16 else "%d of 16" % leads,
        "build_m": "%.1f" % build["mettle_s"],
        "build_c": "%.1f" % build["clang_s"],
        "build_ratio": "%.1f" % (build["clang_s"] / build["mettle_s"]),
    }
