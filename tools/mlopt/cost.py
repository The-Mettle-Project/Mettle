#!/usr/bin/env python3
"""A simple latency cost model for IR, so superoptimization wins (e.g. replacing a
multiply with a couple of shifts/adds) are measurable — pure instruction count
would call that a regression. Costs are rough relative latencies on a modern
out-of-order core; the point is the ORDERING (mul/div >> alu), not exactness."""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from sopt import split_def, _BIN

OP_COST = {
    "*": 4, "/": 20, "%": 20,
    "+": 1, "-": 1, "&": 1, "|": 1, "^": 1, "<<": 1, ">>": 1,
    "==": 1, "!=": 1, "<": 1, "<=": 1, ">": 1, ">=": 1,
}
COPY = 0          # reg-reg moves usually fold away
LOAD = 3          # `%t <- * addr [n]`
DEFAULT = 1


def instr_cost(ins):
    if ins.startswith(("label ", "jump ", "local ")):
        return 0
    if ins.startswith("branch") or ins.startswith("return "):
        return 1
    d = split_def(ins)
    if not d:
        return DEFAULT
    rhs = d[2]
    if rhs.lstrip().startswith("*") or "<- *" in ins or re.search(r"\*\s*%", rhs):
        return LOAD
    m = _BIN.match(rhs) or re.match(r"^(\S+) (==|!=|<|<=|>|>=) (\S+)$", rhs)
    if m:
        return OP_COST.get(m.group(2), DEFAULT)
    if re.fullmatch(r"[@%][\w.$]+|-?\d+", rhs.strip()):
        return COPY
    if re.search(r"[A-Za-z_]\w*\s*\(", rhs):     # call
        return 5
    return DEFAULT


def body_cost(instrs):
    return sum(instr_cost(s) for s in instrs)


if __name__ == "__main__":
    import json
    for line in sys.stdin:
        r = json.loads(line)
        print(body_cost(r["funcs"][0]["instrs"]))
