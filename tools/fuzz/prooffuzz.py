import argparse
import os
import random
import re
import subprocess
import sys

INPUTS = [-2147483647, -1000, -129, -51, -50, -1, 0, 1, 4, 5, 9, 10, 49, 50, 51,
          60, 61, 99, 100, 101, 127, 128, 200, 255, 256, 1000, 2147483647]


class Gen:
    def __init__(self, seed):
        self.r = random.Random(seed)
        self.lines = []
        self.types = []
        self.verdicts = []
        self.demands = []
        self.maps = []
        self.claims = 0
        self.claim_lines = {}
        self.focus = None
        self.loops = 0
        self.signs = False
        self.shape_locals = False
        self.with_demands = self.r.random() < 0.3
        self.inputs = list(INPUTS)
        self.r.shuffle(self.inputs)

    def emit(self, text=""):
        self.lines.append(text)

    def bound(self):
        return self.r.choice([-50, -1, 0, 1, 2, 5, 9, 10, 50, 60, 100, 127, 200, 255])

    def pair(self):
        a, b = self.bound(), self.bound()
        return (min(a, b), max(a, b))

    def make_types(self):
        for k in range(self.r.randint(3, 5)):
            lo, hi = self.pair()
            base = "int32"
            if lo >= 0 and hi <= 255 and self.r.random() < 0.3:
                base = "uint8"
            name = "T%d" % k
            if base == "int32" and self.r.random() < 0.25:
                hi = 2147483647
                self.emit("type %s = %s where value >= %d;" % (name, base, lo))
            else:
                self.emit("type %s = %s where value >= %d && value <= %d;" % (name, base, lo, hi))
            self.types.append((name, base, lo, hi))
        self.emit()

    def helper_cond(self, names):
        r = self.r
        v = r.choice(names)
        kind = r.randrange(6)
        if kind == 0:
            return "%s %s %d" % (v, r.choice(["<", "<=", ">", ">=", "==", "!="]), self.bound())
        if kind == 1 and len(names) > 1:
            return "%s %s %s" % (v, r.choice(["<", "<=", ">", ">="]), r.choice(names))
        if kind == 2:
            lo, hi = self.pair()
            return "%s >= %d && %s <= %d" % (v, lo, v, hi)
        if kind == 3:
            lo, hi = self.pair()
            return "%s < %d || %s > %d" % (v, lo, v, hi)
        if kind == 4 and self.verdicts:
            name, arity = r.choice(self.verdicts)
            args = ", ".join(r.choice(names) for _ in range(arity))
            return "%s%s(%s)" % (r.choice(["", "!"]), name, args)
        return "%d %s %s" % (self.bound(), r.choice(["<", "<=", ">", ">="]), v)

    def helper_value(self, names):
        r = self.r
        kind = r.randrange(7)
        v = r.choice(names)
        if kind == 0:
            return str(self.bound())
        if kind == 1:
            return v
        if kind == 2:
            return "%s + %d" % (v, r.choice([1, 2, 10, 200, 1000]))
        if kind == 3:
            return "%s - %d" % (v, r.choice([1, 2, 10, 200]))
        if kind == 4:
            return "%s / %d" % (v, r.choice([2, 3, 10]))
        wide = [m for m in self.maps if m[2] == "int32"]
        if kind == 5 and wide:
            name, arity, _ = r.choice(wide)
            return "%s(%s)" % (name, ", ".join(r.choice(names) for _ in range(arity)))
        return "%s %% %d" % (v, r.choice([7, 10, 100]))

    def make_matched_helpers(self):
        for k, (name, base, lo, hi) in enumerate(self.types):
            self.emit("fn is_%s(n: int32) -> bool { return n >= %d && n <= %d; }" % (name, lo, hi))
            self.emit("fn split_%s(n: int32) -> bool {" % name)
            self.emit("  if (n < %d) { return false; }" % lo)
            self.emit("  return n <= %d;" % hi)
            self.emit("}")
            self.emit("fn clamp_%s(n: int32) -> int32 {" % name)
            self.emit("  if (n < %d) { return %d; }" % (lo, lo))
            self.emit("  if (n > %d) { return %d; }" % (hi, hi))
            self.emit("  return n;")
            self.emit("}")
            if self.with_demands:
                self.emit("fn must_%s(n: int32) { if (n < %d || n > %d) { bail(); } }" % (name, lo, hi))
            self.emit("fn deep_%s(n: int32, d: int32) -> bool {" % name)
            self.emit("  if (n < %d || n > %d) { return false; }" % (lo, hi))
            self.emit("  if (d > 0) { return deep_%s(n, d - 1); }" % name)
            self.emit("  return true;")
            self.emit("}")
            self.emit("fn ping_%s(n: int32, d: int32) -> bool {" % name)
            self.emit("  if (n < %d) { return false; }" % lo)
            self.emit("  if (d <= 0) { return n <= %d; }" % hi)
            self.emit("  return pong_%s(n, d - 1);" % name)
            self.emit("}")
            self.emit("fn pong_%s(n: int32, d: int32) -> bool {" % name)
            self.emit("  if (n > %d) { return false; }" % hi)
            self.emit("  if (d <= 0) { return n >= %d; }" % lo)
            self.emit("  return ping_%s(n, d - 1);" % name)
            self.emit("}")
            self.emit("fn opt_%s(n: int32) -> Option<int32> {" % name)
            if self.r.random() < 0.2:
                self.emit("  if (n == %d) { return Some(n); }" % self.bound())
            self.emit("  if (n < %d || n > %d) { return None; }" % (lo, hi))
            if self.r.random() < 0.15:
                self.emit("  n = n + %d;" % self.r.choice([1, 500]))
            self.emit("  return Some(%s);" % self.r.choice(["n", "n", "n", "n / 2", "n + 1"]))
            self.emit("}")
            self.emit()
        self.emit("fn between(n: int32, lo: int32, hi: int32) -> bool { return n >= lo && n <= hi; }")
        self.emit()

    def matched(self, names, depth, indent):
        r = self.r
        name, base, lo, hi = r.choice(self.types)
        v = r.choice([n for n in names])
        self.focus = (name, v)
        shape = r.randrange(9)
        opened = True
        if shape == 0:
            self.emit("%sif (%s >= %d && %s <= %d) {" % (indent, v, lo, v, hi))
        elif shape == 1:
            self.emit("%sif (is_%s(%s)) {" % (indent, name, v))
        elif shape == 2:
            self.emit("%sif (split_%s(%s)) {" % (indent, name, v))
        elif shape == 3:
            self.emit("%sif (between(%s, %d, %d)) {" % (indent, v, lo, hi))
        elif shape == 4:
            self.emit("%sif (!is_%s(%s)) { return; }" % (indent, name, v))
            opened = False
        elif shape == 5:
            self.emit("%sif (%s < %d || %s > %d) { return; }" % (indent, v, lo, v, hi))
            opened = False
        elif shape == 6 and self.with_demands:
            self.emit("%smust_%s(%s);" % (indent, name, v))
            opened = False
        elif shape == 7:
            self.emit("%s%s = clamp_%s(%s);" % (indent, v, name, r.choice(names)))
            opened = False
        elif r.random() < 0.5:
            self.emit("%sif (%s_%s(%s, %d)) {" % (indent, r.choice(["deep", "ping", "pong"]), name, v, r.randint(0, 3)))
        else:
            self.emit("%sif (%d <= %s && is_%s(%s)) {" % (indent, lo, v, name, v))
        inner = indent + "  " if opened else indent
        for _ in range(r.randint(1, 4)):
            pick = r.randrange(10)
            if pick < 5:
                self.claim(names, inner)
            elif pick < 7:
                self.write(names, inner)
            elif pick == 7 and depth < 2:
                self.emit("%sif (%s) {" % (inner, self.cond(names)))
                self.block(names, depth + 2, inner + "  ")
                self.emit("%s}" % inner)
            elif pick == 8 and depth < 2:
                counter = "i%d" % len(self.lines)
                self.emit("%svar %s: int32 = 0;" % (inner, counter))
                self.emit("%swhile (%s < 2) {" % (inner, counter))
                self.claim(names, inner + "  ")
                self.write(names, inner + "  ")
                self.emit("%s  %s = %s + 1;" % (inner, counter, counter))
                self.emit("%s}" % inner)
            else:
                self.emit("%squiet();" % inner)
        if opened:
            self.emit("%s}" % indent)
        self.focus = None

    def make_helper(self, k):
        r = self.r
        arity = r.choice([1, 1, 2, 3])
        names = ["p%d" % i for i in range(arity)]
        kinds = ["verdict", "verdict", "map", "map"]
        if self.with_demands:
            kinds.append("demand")
        kind = r.choice(kinds)
        ptype = "int32"
        rtype = {"verdict": "bool", "demand": "", "map": "int32"}[kind]
        name = "%s%d" % (kind[0], k)
        if kind == "map" and r.random() < 0.2:
            op = r.choice(["+", "-", "/"])
            body = "p0 %s %d" % (op, r.choice([1, 2, 3, 100, 200]))
            self.emit("fn %s(p0: uint8) -> uint8 { return %s; }" % (
                name, "(uint8)(%s)" % body if op == "/" else body))
            self.emit()
            self.maps.append((name, 1, "uint8"))
            return
        sig = ", ".join("%s: %s" % (n, ptype) for n in names)
        self.emit("fn %s(%s)%s {" % (name, sig, (" -> " + rtype) if rtype else ""))
        if r.random() < 0.12:
            v = r.choice(names)
            self.emit("  %s = %s %s %d;" % (v, v, r.choice(["+", "-"]), r.choice([1, 100, 1000])))
        if r.random() < 0.08:
            self.emit("  defer quiet();")
        for _ in range(r.randint(0, 3)):
            cond = self.helper_cond(names)
            if kind == "verdict":
                self.emit("  if (%s) { return %s; }" % (cond, r.choice(["true", "false", "false"])))
            elif kind == "demand":
                self.emit("  if (%s) { bail(); }" % cond)
            else:
                self.emit("  if (%s) { return %s; }" % (cond, self.helper_value(names)))
            if r.random() < 0.1:
                self.emit("  %s();" % r.choice(["quiet", "touch"]))
            if r.random() < 0.06:
                v = r.choice(names)
                self.emit("  %s = %s + %d;" % (v, v, r.choice([1, 500])))
        if kind == "verdict":
            self.emit("  return %s;" % r.choice([self.helper_cond(names), self.helper_cond(names), "true"]))
            if ptype == "int32":
                self.verdicts.append((name, arity))
        elif kind == "map":
            self.emit("  return %s;" % self.helper_value(names))
            self.maps.append((name, arity, ptype))
        else:
            if ptype == "int32":
                self.demands.append((name, arity))
        self.emit("}")
        self.emit()

    def sign_cond(self, names):
        r = self.r
        v = r.choice(names)
        form = r.randrange(5)
        op = r.choice(["<", "<=", ">", ">=", "==", "!="])
        if form == 0:
            return "w %s %s" % (op, v)
        if form == 1:
            return "%s %s w" % (v, op)
        if form == 2:
            return "w %s %d" % (op, r.choice([0, 1, 50, 100, 255]))
        if form == 3:
            return "g_reg %s %d" % (op, self.bound())
        return "(int32)w %s %d" % (op, self.bound())

    def cond(self, names, depth=0):
        r = self.r
        kind = r.randrange(10)
        v = r.choice(names)
        if self.signs and r.random() < 0.12:
            return self.sign_cond(names)
        if kind < 3:
            return "%s %s %d" % (v, r.choice(["<", "<=", ">", ">=", "==", "!="]), self.bound())
        if kind == 3:
            lo, hi = self.pair()
            return "%s >= %d && %s <= %d" % (v, lo, v, hi)
        if kind == 4 and self.verdicts:
            name, arity = r.choice(self.verdicts)
            args = ", ".join(self.arg(names) for _ in range(arity))
            return "%s%s(%s)" % (r.choice(["", "", "!"]), name, args)
        if kind == 5:
            return "%s %s %s" % (v, r.choice(["<", "<=", ">", ">="]), r.choice(names))
        if kind == 6 and depth < 2:
            return "%s %s %s" % (self.cond(names, depth + 1), r.choice(["&&", "||"]), self.cond(names, depth + 1))
        if kind == 7 and depth < 2:
            return "!(%s)" % self.cond(names, depth + 1)
        if kind == 8:
            return "%d %s %s" % (self.bound(), r.choice(["<", "<=", ">", ">="]), v)
        if self.maps:
            name, arity, ptype = r.choice(self.maps)
            if ptype == "int32":
                args = ", ".join(self.arg(names) for _ in range(arity))
                return "%s(%s) %s %d" % (name, args, r.choice(["<", "<=", ">", ">="]), self.bound())
        return "%s != %d" % (v, self.bound())

    def arg(self, names):
        r = self.r
        if r.random() < 0.8:
            return r.choice(names)
        if r.random() < 0.5:
            return str(self.bound())
        return "%s + %d" % (r.choice(names), r.choice([1, 2, 10]))

    def value(self, names):
        r = self.r
        kind = r.randrange(9)
        v = r.choice(names)
        if self.signs and r.random() < 0.08:
            return r.choice(["(int32)w", "g_reg", "(int32)(w & 255)", "(int32)(w / 2)"])
        if kind < 3:
            return v
        if kind == 3:
            return "%s + %d" % (v, r.choice([1, 2, 10, 200]))
        if kind == 4:
            return "%s - %d" % (v, r.choice([1, 2, 10]))
        if kind == 5 and self.maps:
            name, arity, ptype = r.choice(self.maps)
            args = ", ".join(self.arg(names) for _ in range(arity)) if ptype == "int32" else "(uint8)(%s & 255)" % v
            return "%s(%s)" % (name, args)
        if kind == 6:
            return "%s / %d" % (v, r.choice([2, 3, 10]))
        if kind == 7:
            return "%s & %d" % (v, r.choice([7, 63, 255]))
        return "%s %% %d" % (v, r.choice([10, 100]))

    def claim(self, names, indent):
        name, base, lo, hi = self.r.choice(self.types)
        value = self.value(names)
        if self.focus and self.r.random() < 0.75:
            name, base, lo, hi = [t for t in self.types if t[0] == self.focus[0]][0]
            value = self.focus[1]
        cid = self.claims
        self.claims += 1
        form = "cast" if base != "int32" else self.r.choice(["plain", "cast"])
        if form == "plain":
            text = "%svar c%d: %s = %s;" % (indent, cid, name, value)
        else:
            text = "%svar c%d: %s = (%s)(%s);" % (indent, cid, name, name, value)
        self.claim_lines[len(self.lines) + 1] = cid
        self.emit(text)
        self.emit("%sif ((int64)c%d < %d || (int64)c%d > %d) { g_bad = g_bad + 1; g_first = %d; }" % (
            indent, cid, lo, cid, hi, cid))

    def write(self, names, indent):
        r = self.r
        v = r.choice(names)
        kind = r.randrange(10)
        if kind < 2:
            self.emit("%s%s = %s;" % (indent, v, self.value(names)))
        elif kind == 2:
            self.emit("%s%s = %s + %d;" % (indent, v, v, r.choice([1, 3, 500])))
        elif kind == 3:
            self.emit("%s%s = %s - %d;" % (indent, v, v, r.choice([1, 3, 500])))
        elif kind == 4 and v != "g_level":
            self.emit("%spoke(&%s);" % (indent, v))
        elif kind == 5:
            self.emit("%stouch();" % indent)
        elif kind == 6:
            self.emit("%squiet();" % indent)
        elif kind == 7:
            self.emit("%s*at = *at + %d;" % (indent, r.choice([1, 300])))
        elif kind == 8 and self.demands:
            name, arity = r.choice(self.demands)
            self.emit("%s%s(%s);" % (indent, name, ", ".join(self.arg(names) for _ in range(arity))))
        else:
            self.emit("%sg_level = %s;" % (indent, self.value(names)))

    def fresh(self, names, indent):
        r = self.r
        name, base, lo, hi = r.choice(self.types)
        v = r.choice(names)
        k = r.choice([lo, hi, lo - 1, hi + 1, (lo + hi) // 2, self.bound()])
        k = max(-2000000000, min(2000000000, k))
        form = r.randrange(11)
        if form == 9:
            has_default = r.random() < 0.7
            self.emit("%sswitch (%s) {" % (indent, r.choice(names)))
            seen = set()
            for _ in range(r.randint(1, 3)):
                c = self.bound()
                if c in seen:
                    continue
                seen.add(c)
                extra = r.choice(["", "", "", " return;", " %s = %s + 1;" % (v, v),
                                  " fallthrough;" if has_default else ""])
                self.emit("%s  case %d: { %s = %d;%s }" % (indent, c, v, r.choice([lo, hi, k]), extra))
            if has_default:
                self.emit("%s  default: { %s = %d; }" % (indent, v, (lo + hi) // 2))
            self.emit("%s}" % indent)
        elif form == 10:
            tname = r.choice(self.types)[0]
            held = "m%d" % len(self.lines)
            self.emit("%smatch (opt_%s(%s)) {" % (indent, tname, self.arg(names)))
            self.emit("%s  case Some(%s): { %s = %s; }" % (indent, held, v, r.choice([str(k), held, str(lo)])))
            self.emit("%s  case None: { %s = %d; }" % (indent, v, hi if hi < 2000000000 else lo))
            self.emit("%s}" % indent)
        elif form == 6:
            a, b = (lo, hi) if r.random() < 0.8 else (lo - 1, min(hi + 1, 2000000000))
            first = "%sif (%s < %d) { %s = %d; }" % (indent, v, lo, v, a)
            second = "%sif (%s > %d) { %s = %d; }" % (indent, v, hi, v, b)
            if r.random() < 0.8:
                self.emit(first)
            if r.random() < 0.2:
                self.write(names, indent)
            if r.random() < 0.8:
                self.emit(second)
        elif form == 7:
            other = r.choice([lo, hi, hi + 1, (lo + hi) // 2, self.bound()])
            other = max(-2000000000, min(2000000000, other))
            self.emit("%sif (%s) { %s = %d; } else { %s = %d; }" % (indent, self.cond(names), v, k, v, other))
        elif form == 8:
            self.emit("%sif (%s) {" % (indent, self.cond(names)))
            self.emit("%s  %s = %d;" % (indent, v, k))
            if r.random() < 0.4:
                self.emit("%s  %s" % (indent, r.choice(["return;", "poke(&%s);" % v if v != "g_level" else "touch();",
                                                         "%s = %s + 1;" % (v, v)])))
            self.emit("%s} else if (%s) {" % (indent, self.cond(names)))
            self.emit("%s  %s = %s;" % (indent, v, r.choice([str(lo), str(hi), self.value(names)])))
            if r.random() < 0.7:
                self.emit("%s} else {" % indent)
                self.emit("%s  %s = %d;" % (indent, v, (lo + hi) // 2))
            self.emit("%s}" % indent)
        elif form == 0:
            self.emit("%s%s = %d;" % (indent, v, k))
        elif form == 1:
            self.emit("%s%s = %s;" % (indent, v, self.value(names)))
        elif form == 2:
            self.emit("%s%s = %d;" % (indent, v, k))
            self.emit("%s%s = %s %s %d;" % (indent, v, v, r.choice(["+", "-"]), r.choice([1, 3, 500])))
        elif form == 3:
            self.emit("%sdefer %s = %d;" % (indent, v, k))
        elif form == 4:
            held = "f%d" % len(self.lines)
            self.emit("%svar %s: int32 = %s;" % (indent, held, r.choice([str(k), self.value(names)])))
            names = names + [held]
            v = held
        else:
            self.emit("%s%s = %d;" % (indent, v, k))
            self.emit("%sif (%s) { %s = %s; }" % (indent, self.cond(names), v, self.value(names)))
        if r.random() < 0.3:
            self.write(names, indent)
        self.focus = (name, v)
        self.claim(names, indent)
        self.focus = None

    def shadow(self, names, depth, indent):
        v = self.r.choice(["x", "y", "g_level", "CEIL"])
        self.emit("%s{" % indent)
        held = self.signs
        self.signs = False
        self.emit("%s  var %s: int32 = %s;" % (indent, v, self.value([n for n in names if n != v])))
        self.signs = held
        self.block(names, depth + 1, indent + "  ")
        self.emit("%s}" % indent)

    def rising(self, names, depth, indent):
        r = self.r
        k = "k%d" % len(self.lines)
        n = "n%d" % len(self.lines)
        step = r.choice([1, 1, 3, 1000000000, 2000000000])
        sign = r.choice(["+", "+", "+", "-"])
        limit = r.choice([str(self.bound()), r.choice(names), "CEIL", "%s + 3" % r.choice(names)])
        test = "%s %s %s" % (k, "<" if sign == "+" else ">", limit)
        self.emit("%svar %s: int32 = %d;" % (indent, k, self.bound()))
        self.emit("%svar %s: int32 = 0;" % (indent, n))
        if r.random() < 0.3:
            self.emit("%s%s = %s %s %d;" % (indent, k, k, sign, step))
        self.emit("%swhile (%s) {" % (indent, r.choice([test, test, "%s < 4" % n, "%s || %s < 2" % (test, n)])))
        held = self.loops
        self.loops = 0
        if r.random() < 0.5:
            self.block(names + [k], depth + 2, indent + "  ")
        self.loops = held
        self.emit("%s  %s = %s %s %d;" % (indent, k, k, sign, step))
        self.emit("%s  %s = %s + 1;" % (indent, n, n))
        self.emit("%s  if (%s > 3) { break; }" % (indent, n))
        self.emit("%s}" % indent)
        name = r.choice(self.types)[0]
        self.focus = (name, k)
        self.claim(names + [k], indent)
        self.focus = None

    def block(self, names, depth, indent):
        r = self.r
        for _ in range(r.randint(1, 4)):
            kind = r.randrange(12)
            if r.random() < 0.08:
                self.fresh(names, indent)
            elif r.random() < 0.03:
                self.emit("%sdefer {" % indent)
                if r.random() < 0.3:
                    self.emit("%s  if (%s) {" % (indent, self.cond(names)))
                    self.claim(names, indent + "    ")
                    self.emit("%s  }" % indent)
                else:
                    self.claim(names, indent + "  ")
                self.emit("%s}" % indent)
                self.write(names, indent)
            elif depth < 3 and r.random() < 0.05:
                self.shadow(names, depth, indent)
            elif depth < 2 and r.random() < 0.05:
                self.rising(names, depth, indent)
            elif depth < 3 and r.random() < 0.35:
                self.matched(names, depth, indent)
            elif kind < 3:
                self.claim(names, indent)
            elif kind < 5:
                self.write(names, indent)
            elif kind < 8 and depth < 3:
                self.emit("%sif (%s) {" % (indent, self.cond(names)))
                self.block(names, depth + 1, indent + "  ")
                if r.random() < 0.3:
                    self.emit("%s} else {" % indent)
                    self.block(names, depth + 1, indent + "  ")
                self.emit("%s}" % indent)
            elif kind == 8:
                leave = r.choice(["return;", "return;", "bail();"] if self.with_demands else ["return;"])
                if self.loops > 0 and r.random() < 0.5:
                    leave = r.choice(["break;", "continue;"])
                self.emit("%sif (%s) {" % (indent, self.cond(names)))
                for _ in range(r.randint(0, 2)):
                    self.write(names, indent + "  ")
                self.emit("%s  %s" % (indent, leave))
                self.emit("%s}" % indent)
            elif kind == 9 and depth < 2 and r.random() < 0.5:
                counter = "j%d" % len(self.lines)
                self.emit("%sfor %s in 0..%d {" % (indent, counter, r.randint(1, 3)))
                self.loops += 1
                self.block(names, depth + 2, indent + "  ")
                self.loops -= 1
                self.emit("%s}" % indent)
            elif kind == 9 and depth < 2:
                counter = "i%d" % len(self.lines)
                self.emit("%svar %s: int32 = 0;" % (indent, counter))
                self.emit("%swhile (%s < %d) {" % (indent, counter, r.randint(1, 3)))
                held = self.loops
                self.loops = 0
                self.block(names, depth + 2, indent + "  ")
                self.loops = held
                self.emit("%s  %s = %s + 1;" % (indent, counter, counter))
                self.emit("%s}" % indent)
            elif kind == 10 and depth < 2:
                v = r.choice(names)
                self.emit("%sswitch (%s) {" % (indent, v))
                seen = set()
                for _ in range(r.randint(1, 3)):
                    c = self.bound()
                    if c in seen:
                        continue
                    seen.add(c)
                    if r.random() < 0.15:
                        self.emit("%s  case %d:" % (indent, c))
                        continue
                    self.emit("%s  case %d: {" % (indent, c))
                    self.block(names, depth + 2, indent + "    ")
                    if r.random() < 0.15:
                        self.emit("%s    fallthrough;" % indent)
                    self.emit("%s  }" % indent)
                self.emit("%s  default: {" % indent)
                self.block(names, depth + 2, indent + "    ")
                self.emit("%s  }" % indent)
                self.emit("%s}" % indent)
            elif depth < 2 and r.random() < 0.6:
                name = r.choice(self.types)[0]
                held = "w%d" % len(self.lines)
                if r.random() < 0.25:
                    held = r.choice(["x", "y", "g_level"])
                self.emit("%smatch (opt_%s(%s)) {" % (indent, name, self.arg(names)))
                self.emit("%s  case Some(%s): {" % (indent, held))
                self.focus = (name, held)
                for _ in range(r.randint(1, 3)):
                    if r.random() < 0.7:
                        self.claim(names + [held], indent + "    ")
                    else:
                        self.write(names + [held], indent + "    ")
                self.focus = None
                self.emit("%s  }" % indent)
                self.emit("%s  case None: { }" % indent)
                self.emit("%s}" % indent)
            elif self.demands:
                name, arity = r.choice(self.demands)
                self.emit("%s%s(%s);" % (indent, name, ", ".join(self.arg(names) for _ in range(arity))))

    def make_test(self, k):
        self.emit("fn t%d(a: int32, b: int32) {" % k)
        self.emit("  var x: int32 = a;")
        self.emit("  var y: int32 = b - a;")
        target = self.r.choice(["a", "x", "y"])
        self.emit("  var at: int32* = &%s;" % target if self.r.random() < 0.5 else "  var at: int32* = &g_spare;")
        self.emit("  var w: uint32 = (uint32)b;")
        self.signs = True
        self.block(["a", "b", "x", "y", "g_level"], 0, "  ")
        self.signs = False
        self.emit("}")
        self.emit()

    SHAPES = '''struct Node {
  len: int32;
  next: Node*;
}

struct Span {
  lo: int32;
  hi: int32;
}

struct Clearer {
  target: Node*;
  method drop() { if (this.target != 0) { this.target->len = 0; } }
}

type Live = Node* where value != 0;
type Long = Node* where value != 0 && value->len > 4;
type Ordered = Span where value.lo <= value.hi;
type Unit = float64 where value >= 0.0 && value <= 1.0;
type Index = int64 where value >= 0 && value < buf.length;

fn in_bounds(buf: int32[], i: int64) -> bool {
  if (i < 0) { return false; }
  return i < buf.length;
}
fn is_live(p: Node*) -> bool { return p != 0; }
fn is_long(p: Node*) -> bool { return p != 0 && p->len > 4; }
fn is_ordered(s: Span) -> bool { return s.lo <= s.hi; }
fn Span_ordered(s: Span) -> bool { return s.lo <= s.hi; }
fn in_unit(x: float64) -> bool { return x >= 0.0 && x <= 1.0; }
fn shrink(p: Node*) { if (p != 0) { p->len = 0; } }
fn grow(p: Node*) { if (p != 0) { p->len = 9; } }
fn peek(p: Node*) -> int32 { if (p == 0) { return 0; } return p->len; }
fn make_clearer(p: Node*) -> Clearer { var c: Clearer = { target: p }; return c; }
fn halve(x: float64) -> float64 { return x * 0.5; }
fn twice(x: float64) -> float64 { return x * 2.0; }
fn clamp_unit(x: float64) -> float64 {
  if (!(x >= 0.0)) { return 0.0; }
  if (!(x <= 1.0)) { return 1.0; }
  return x;
}
fn loose_unit(x: float64) -> float64 {
  if (x < 0.0) { return 0.0; }
  if (x > 1.0) { return 1.0; }
  return x;
}
'''

    def shape_atom(self, who):
        r = self.r
        p = r.choice(who)
        return r.choice([
            "%s != 0" % p, "%s == 0" % p, "%s->len > 4" % p, "%s->len <= 4" % p,
            "%s->len > %d" % (p, r.choice([0, 4, 8])), "is_live(%s)" % p, "is_long(%s)" % p,
        ])

    def make_shape_helper(self, k):
        r = self.r
        kind = r.choice(["verdict", "verdict", "demand"]) if self.with_demands else "verdict"
        name = "%s%d" % ("sv" if kind == "verdict" else "sd", k)
        self.emit("fn %s(p: Node*)%s {" % (name, " -> bool" if kind == "verdict" else ""))
        if r.random() < 0.1:
            self.emit("  defer shrink(p);")
        if r.random() < 0.1:
            self.emit("  var c: Clearer = { target: p };")
        self.emit("  if (p == 0) { %s }" % ("return false;" if kind == "verdict" else "bail();"))
        for _ in range(r.randint(0, 2)):
            cond = r.choice(["p->len <= 4", "p->len > 8", "p->len == 0", "p->next == 0"])
            self.emit("  if (%s) { %s }" % (cond, r.choice(["return false;", "return true;"]) if kind == "verdict" else "bail();"))
            if r.random() < 0.15:
                self.emit("  %s" % r.choice(["shrink(p);", "p->len = 0;", "grow(p);", "quiet();", "touch();",
                                             "if (p->next != 0) { p = p->next; }"]))
        if kind == "verdict":
            self.emit("  return %s;" % r.choice(["true", "p->len > 4", "p->len > 4", "p->next != 0"]))
            self.shape_verdicts.append(name)
        else:
            self.emit("  if (p->len <= 4) { bail(); }")
            self.shape_demands.append(name)
        self.emit("}")
        self.emit()

    def shape_cond(self):
        r = self.r
        kind = r.randrange(11)
        p = r.choice(["p", "q"])
        if kind == 0:
            return "is_live(%s)" % p
        if kind == 1:
            return "is_long(%s)" % p
        if kind == 2:
            return "%s != 0 && %s->len > 4" % (p, p)
        if kind == 3:
            return "%s != 0" % p
        if kind == 4:
            return r.choice(["is_ordered(s)", "s.ordered()", "s.lo <= s.hi"])
        if kind == 5:
            return r.choice(["in_unit(x)", "x >= 0.0 && x <= 1.0", "!(x < 0.0 || x > 1.0)",
                             "!(x < 0.0) && !(x > 1.0)", "x < 0.0 || x > 1.0", "!in_unit(x)"])
        if kind == 6 and self.shape_verdicts:
            return "%s%s(%s)" % (r.choice(["", "!"]), r.choice(self.shape_verdicts), p)
        if kind == 7:
            return "!(%s)" % self.shape_cond()
        if kind == 8:
            return "%s && %s" % (self.shape_cond(), self.shape_cond())
        if kind == 9:
            return "peek(%s) > 4" % p
        if r.random() < 0.6:
            return r.choice(["in_bounds(buf, i)", "i >= 0 && i < buf.length", "in_bounds(other, i)",
                             "!in_bounds(buf, i)", "i < buf.length", "i >= 0"])
        return "%s == %s" % (p, r.choice(["p", "q", "0"]))

    def shape_fresh(self, indent):
        r = self.r
        form = r.randrange(4)
        if form == 0:
            self.emit("%sx = %s;" % (indent, r.choice(["0.0", "0.25", "1.0", "1.5", "-0.5", "x * 0.5",
                                                        "clamp_unit(x)", "twice(x)"])))
        elif form == 1:
            self.emit("%sx = %s;" % (indent, r.choice(["0.25", "0.5"])))
            self.emit("%sx = x %s;" % (indent, r.choice(["* 2.0", "+ 0.5", "* 0.5", "- 0.5"])))
        elif form == 2:
            self.emit("%sdefer x = 0.5;" % indent)
        else:
            self.emit("%sx = 0.5;" % indent)
            self.emit("%sif (%s) { x = %s; }" % (indent, self.shape_cond(), r.choice(["2.0", "x + 1.0", "0.75"])))
        self.shape_claim(indent, 4)

    def shape_shadow(self, depth, indent):
        r = self.r
        self.emit("%s{" % indent)
        self.emit(indent + "  " + r.choice([
            "var buf: int32[] = other;", "var i: int64 = %d;" % r.choice([-1, 0, 1, 3]),
            "var x: float64 = %s;" % r.choice(["2.5", "0.5", "1.5"]),
            "var p: Node* = q;", "var q: Node* = 0;",
            "var s: Span = { lo: 9, hi: 1 };",
        ]))
        self.shape_block(depth + 1, indent + "  ")
        self.emit("%s}" % indent)

    def shape_claim(self, indent, kind=None):
        r = self.r
        cid = self.claims
        self.claims += 1
        if kind is None:
            kind = r.randrange(7)
        p = r.choice(["p", "q"])
        self.claim_lines[len(self.lines) + 1] = cid
        fail = "{ g_bad = g_bad + 1; g_first = %d; }" % cid
        if kind >= 5:
            value = r.choice(["i", "i", "i", "i + 1", "i - 1"])
            self.emit("%svar c%d: Index = (Index)(%s);" % (indent, cid, value))
            self.emit("%sif ((int64)c%d < 0 || (int64)c%d >= buf.length) %s" % (indent, cid, cid, fail))
        elif kind == 0:
            self.emit("%svar c%d: Live = (Live)%s;" % (indent, cid, p))
            self.emit("%sif ((Node*)c%d == 0) %s" % (indent, cid, fail))
        elif kind == 1 or kind == 2:
            self.emit("%svar c%d: Long = (Long)%s;" % (indent, cid, p))
            self.emit("%sif (peek((Node*)c%d) <= 4) %s" % (indent, cid, fail))
        elif kind == 3:
            self.emit("%svar c%d: Ordered = (Ordered)s;" % (indent, cid))
            self.emit("%sif (c%d.lo > c%d.hi) %s" % (indent, cid, cid, fail))
        else:
            value = r.choice(["x", "x", "halve(x)", "twice(x)", "clamp_unit(x)", "loose_unit(x)",
                              "halve(clamp_unit(x))", "x * 0.5", "clamp_unit(twice(x))"])
            self.emit("%svar c%d: Unit = (Unit)(%s);" % (indent, cid, value))
            self.emit("%sif (!((float64)c%d >= 0.0 && (float64)c%d <= 1.0)) %s" % (indent, cid, cid, fail))

    def shape_write(self, indent):
        r = self.r
        p = r.choice(["p", "q"])
        self.emit(indent + r.choice([
            "if (%s != 0) { %s->len = %d; }" % (p, p, r.choice([0, 9])),
            "shrink(%s);" % p, "grow(%s);" % p, "quiet();", "touch();",
            "%s = %s;" % (p, r.choice(["p", "q"])),
            "s.lo = %d;" % r.choice([0, 50]), "s.hi = s.lo - 1;", "s.hi = s.hi + 1;",
            "x = x + 0.75;", "x = x * 0.5;", "x = 0.25;",
            "i = i + 1;", "i = i - 1;", "i = 0;", "buf = other;", "i = i + 1;",
            "{ var c: Clearer = { target: %s }; }" % p,
            "var k%d: Clearer = make_clearer(%s);" % (len(self.lines), p),
            "if (%s != 0) { %s = %s->next; }" % (p, p, p),
        ]))
        if self.shape_demands and r.random() < 0.2:
            self.emit("%s%s(%s);" % (indent, r.choice(self.shape_demands), p))

    def shape_block(self, depth, indent):
        r = self.r
        for _ in range(r.randint(1, 4)):
            kind = r.randrange(10)
            if r.random() < 0.06:
                self.shape_fresh(indent)
            elif self.shape_locals and depth < 3 and r.random() < 0.08:
                self.shape_shadow(depth, indent)
            elif kind < 4:
                self.shape_claim(indent)
            elif kind < 6:
                self.shape_write(indent)
            elif kind < 8 and depth < 3:
                self.emit("%sif (%s) {" % (indent, self.shape_cond()))
                self.shape_block(depth + 1, indent + "  ")
                if r.random() < 0.25:
                    self.emit("%s} else {" % indent)
                    self.shape_block(depth + 1, indent + "  ")
                self.emit("%s}" % indent)
            elif kind == 8:
                leave = "return;"
                if self.loops > 0 and r.random() < 0.5:
                    leave = r.choice(["break;", "continue;"])
                self.emit("%sif (%s) {" % (indent, self.shape_cond()))
                for _ in range(r.randint(0, 2)):
                    self.shape_write(indent + "  ")
                self.emit("%s  %s" % (indent, leave))
                self.emit("%s}" % indent)
            elif depth < 2 and r.random() < 0.5:
                counter = "j%d" % len(self.lines)
                self.emit("%sfor %s in 0..2 {" % (indent, counter))
                self.loops += 1
                self.shape_block(depth + 2, indent + "  ")
                self.loops -= 1
                self.emit("%s}" % indent)
            elif depth < 2:
                counter = "i%d" % len(self.lines)
                self.emit("%svar %s: int32 = 0;" % (indent, counter))
                self.emit("%swhile (%s < 2) {" % (indent, counter))
                self.shape_block(depth + 2, indent + "  ")
                self.emit("%s  %s = %s + 1;" % (indent, counter, counter))
                self.emit("%s}" % indent)

    def make_shape_test(self, k):
        self.shape_locals = self.r.random() < 0.5
        if self.shape_locals:
            self.emit("fn u%d(p0: Node*, q0: Node*, s0: Span, x0: float64, buf0: int32[], other: int32[], i0: int64) {" % k)
            self.emit("  var p: Node* = p0;")
            self.emit("  var q: Node* = q0;")
            self.emit("  var s: Span = s0;")
            self.emit("  var x: float64 = x0;")
            self.emit("  var buf: int32[] = buf0;")
            self.emit("  var i: int64 = i0;")
        else:
            self.emit("fn u%d(p: Node*, q: Node*, s: Span, x: float64, buf: int32[], other: int32[], i: int64) {" % k)
        self.shape_block(0, "  ")
        self.emit("}")
        self.emit()

    def program(self, tests):
        self.emit('import "std/io";')
        self.emit('import "std/process";')
        self.emit('import "std/core";')
        self.emit()
        self.emit("var g_level: int32 = 5;")
        self.emit("var g_spare: int32 = 0;")
        self.emit("var g_bad: int64 = 0;")
        self.emit("var g_first: int64 = -1;")
        self.emit("var g_calls: int64 = 0;")
        self.emit("const CEIL: int32 = 100;")
        self.emit("var g_reg: volatile int32 = 7;")
        self.emit()
        self.emit("fn bail() {")
        self.emit('  println("calls {g_calls} bad {g_bad} first {g_first}");')
        self.emit("  exit(0);")
        self.emit("}")
        self.emit()
        self.emit("fn poke(p: int32*) { *p = *p + 500; }")
        self.emit("fn touch() { g_level = g_level + 777; }")
        self.emit("fn quiet() { }")
        self.emit()
        self.make_types()
        self.make_matched_helpers()
        for k in range(self.r.randint(3, 7)):
            self.make_helper(k)
        for k in range(tests):
            self.make_test(k)
        for line in self.SHAPES.split("\n"):
            self.emit(line)
        self.shape_verdicts = []
        self.shape_demands = []
        for k in range(self.r.randint(2, 5)):
            self.make_shape_helper(k)
        for k in range(tests):
            self.make_shape_test(k)
        self.emit("fn pick_node(k: int32, a: Node*, b: Node*, c: Node*) -> Node* {")
        self.emit("  if (k == 1) { return a; }")
        self.emit("  if (k == 2) { return b; }")
        self.emit("  if (k == 3) { return c; }")
        self.emit("  return 0;")
        self.emit("}")
        self.emit()
        self.emit("const INPUTS: int32[%d] = [%s];" % (len(INPUTS), ", ".join(str(v) for v in self.inputs)))
        self.emit()
        self.emit("fn run_numbers() {")
        self.emit("  for i in 0..%d {" % len(INPUTS))
        self.emit("    for j in 0..%d {" % len(INPUTS))
        for k in range(tests):
            self.emit("      g_level = 5;")
            self.emit("      g_calls = g_calls + 1;")
            self.emit("      t%d(INPUTS[i], INPUTS[j]);" % k)
        self.emit("    }")
        self.emit("  }")
        self.emit("}")
        self.emit()
        self.emit("fn run_shapes() {")
        self.emit("  var n0 = new Node;")
        self.emit("  var n1 = new Node;")
        self.emit("  var n2 = new Node;")
        self.emit("  var los: int32[3] = [3, 8, 5];")
        self.emit("  var his: int32[3] = [11, 2, 5];")
        self.emit("  var zero: float64 = 0.0;")
        self.emit("  var xs: float64[5] = [-1.0, 0.0, 0.5, 1.0, 2.5];")
        self.emit("  xs[4] = zero / zero;")
        self.emit("  var wide: int32[] = new int32[4];")
        self.emit("  var thin: int32[] = new int32[2];")
        self.emit("  var at: int64[5] = [-1, 0, 1, 3, 4];")
        self.emit("  for a in 0..4 {")
        self.emit("    for b in 0..4 {")
        self.emit("      for c in 0..3 {")
        self.emit("        for d in 0..5 {")
        self.emit("         for e in 0..5 {")
        for k in range(tests):
            self.emit("          n0->len = 0;")
            self.emit("          n0->next = n1;")
            self.emit("          n1->len = 9;")
            self.emit("          n1->next = n2;")
            self.emit("          n2->len = 5;")
            self.emit("          n2->next = 0;")
            self.emit("          g_level = 5;")
            self.emit("          g_calls = g_calls + 1;")
            self.emit("          var s%d: Span = { lo: los[c], hi: his[c] };" % k)
            self.emit("          u%d(pick_node(a, n0, n1, n2), pick_node(b, n0, n1, n2), s%d, xs[d], %s, at[e]);" % (
                k, k, self.r.choice(["wide, thin", "thin, wide"])))
        self.emit("         }")
        self.emit("        }")
        self.emit("      }")
        self.emit("    }")
        self.emit("  }")
        self.emit("}")
        self.emit()
        self.emit("fn main() -> int32 {")
        order = ["run_numbers();", "run_shapes();"]
        self.r.shuffle(order)
        for call in order:
            self.emit("  " + call)
        self.emit('  println("calls {g_calls} bad {g_bad} first {g_first}");')
        self.emit("  return 0;")
        self.emit("}")
        return self.lines


def compile_program(compiler, extra, source, exe):
    run = subprocess.run([compiler] + extra + ["--build", source, "-o", exe],
                         capture_output=True, text=True, errors="replace")
    return run.returncode, run.stdout + run.stderr


def one(seed, compiler, extra, work, tests, modes=()):
    gen = Gen(seed)
    lines = gen.program(tests)
    source = os.path.join(work, "p%d.mettle" % seed)
    exe = os.path.join(work, "p%d.exe" % seed)
    refused = 0
    for _ in range(14):
        with open(source, "w", newline="\n") as out:
            out.write("\n".join(lines) + "\n")
        if os.path.exists(exe):
            os.remove(exe)
        code, text = compile_program(compiler, extra, source, exe)
        if code == 0 and os.path.exists(exe):
            break
        hit = set()
        for m in re.finditer(r"-- .*?:(\d+):\d+ -+\s*\n\s*error\[P0001\]", text):
            hit.add(int(m.group(1)))
        others = [l for l in text.splitlines() if "error[" in l and "P0001" not in l]
        if not hit or others:
            return ("generator", seed, (others or text.splitlines()[-3:])[:2])
        changed = 0
        for at in hit:
            if at in gen.claim_lines:
                lines[at - 1] = ""
                lines[at] = ""
                del gen.claim_lines[at]
                changed += 1
        refused += changed
        if not changed:
            return ("generator", seed, ["a refusal on a line that holds no claim: %s" % sorted(hit)])
    else:
        return ("generator", seed, ["did not settle"])
    try:
        run = subprocess.run([exe], capture_output=True, text=True, timeout=60, errors="replace")
    except subprocess.TimeoutExpired:
        return ("timeout", seed, [])
    m = re.search(r"calls (\d+) bad (\d+) first (-?\d+)", run.stdout)
    if not m:
        return ("crash", seed, (run.stdout + run.stderr).splitlines()[-2:])
    if int(m.group(2)) > 0:
        return ("FALSE PROOF", seed, ["claim c%s, %s violations" % (m.group(3), m.group(2))])
    for mode in modes:
        other = os.path.join(work, "p%d_%s.exe" % (seed, mode.strip("-").replace("-", "_")))
        code, text = compile_program(compiler, extra + [mode], source, other)
        if code != 0 or not os.path.exists(other):
            return ("mode build", seed, [mode] + text.splitlines()[-2:])
        try:
            again = subprocess.run([other], capture_output=True, text=True, timeout=60, errors="replace")
        except subprocess.TimeoutExpired:
            return ("mode timeout", seed, [mode])
        os.remove(other)
        if again.stdout != run.stdout:
            return ("MODE DIFFERS", seed, [mode, run.stdout.strip()[-60:], (again.stdout + again.stderr).strip()[-120:]])
    return ("ok", seed, [gen.claims - refused, refused])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("compiler")
    ap.add_argument("--seeds", type=int, default=200)
    ap.add_argument("--start", type=int, default=1)
    ap.add_argument("--tests", type=int, default=6)
    ap.add_argument("--work", default=".tmp/prooffuzz")
    ap.add_argument("--mode", action="append", default=[],
                    help="also build with this flag and compare what the program prints")
    ap.add_argument("extra", nargs="*")
    args = ap.parse_args()
    os.makedirs(args.work, exist_ok=True)
    proven = refused = 0
    counts = {}
    for seed in range(args.start, args.start + args.seeds):
        kind, seed, detail = one(seed, args.compiler, args.extra, args.work, args.tests, args.mode)
        counts[kind] = counts.get(kind, 0) + 1
        if kind == "ok":
            proven += detail[0]
            refused += detail[1]
            for ext in (".mettle", ".exe", ".obj"):
                path = os.path.join(args.work, "p%d%s" % (seed, ext))
                if os.path.exists(path):
                    os.remove(path)
        else:
            print(kind, "seed", seed, detail)
            sys.stdout.flush()
    print("seeds %d: %s; claims proven %d, refused %d" % (args.seeds, counts, proven, refused))


if __name__ == "__main__":
    main()
