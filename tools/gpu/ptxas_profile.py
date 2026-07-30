#!/usr/bin/env python3
"""Assemble PTX offline and emit a deterministic resource profile.

This tool deliberately stops at ptxas.  It never loads the resulting cubin,
creates a CUDA context, queries a device, or launches code.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any


SCHEMA = "mettle.ptxas-resource-profile.v2"
ARCH_RE = re.compile(r"sm_[0-9]+[a-z]?")
FUNCTION_RE = re.compile(r"Function properties for\s+(.+?)\s*$")
ENTRY_RE = re.compile(r"Compiling entry function\s+'([^']+)'")
PTX_ENTRY_RE = re.compile(
    r"^\s*(?:\.visible\s+)?\.entry\s+([A-Za-z_$][A-Za-z0-9_.$]*)\s*\("
)
STACK_RE = re.compile(
    r"(?P<stack>[0-9]+) bytes stack frame,\s*"
    r"(?P<stores>[0-9]+) bytes spill stores,\s*"
    r"(?P<loads>[0-9]+) bytes spill loads"
)
REGISTERS_RE = re.compile(r"\bUsed\s+([0-9]+) registers\b")
BARRIERS_RE = re.compile(r"\bused\s+([0-9]+) barriers\b")
SMEM_RE = re.compile(r"\b([0-9]+) bytes smem\b")
CMEM_RE = re.compile(r"\b([0-9]+) bytes cmem\[([0-9]+)\]")
GMEM_RE = re.compile(r"\b([0-9]+) bytes gmem\b")


def _new_function(name: str, is_entry: bool) -> dict[str, Any]:
    return {
        "name": name,
        "entry": is_entry,
        "registers": None,
        "barriers": None,
        "stack_frame_bytes": None,
        "spill_store_bytes": None,
        "spill_load_bytes": None,
        "shared_memory_bytes": None,
        "constant_memory_bytes": {},
    }


def parse_ptxas_verbose(output: str) -> dict[str, Any]:
    """Parse the stable resource fields from English ``ptxas -v`` output."""
    entry_names: set[str] = set()
    functions: list[dict[str, Any]] = []
    by_name: dict[str, dict[str, Any]] = {}
    current: dict[str, Any] | None = None
    global_memory_bytes: int | None = None

    for raw_line in output.splitlines():
        line = raw_line.strip()
        entry_match = ENTRY_RE.search(line)
        if entry_match:
            name = entry_match.group(1)
            entry_names.add(name)
            if name in by_name:
                by_name[name]["entry"] = True

        function_match = FUNCTION_RE.search(line)
        if function_match:
            name = function_match.group(1)
            current = by_name.get(name)
            if current is None:
                current = _new_function(name, name in entry_names)
                by_name[name] = current
                functions.append(current)
            continue

        global_match = GMEM_RE.search(line)
        if global_match:
            global_memory_bytes = int(global_match.group(1))

        if current is None:
            continue

        stack_match = STACK_RE.search(line)
        if stack_match:
            current["stack_frame_bytes"] = int(stack_match.group("stack"))
            current["spill_store_bytes"] = int(stack_match.group("stores"))
            current["spill_load_bytes"] = int(stack_match.group("loads"))

        register_match = REGISTERS_RE.search(line)
        if register_match:
            current["registers"] = int(register_match.group(1))
        barrier_match = BARRIERS_RE.search(line)
        if barrier_match:
            current["barriers"] = int(barrier_match.group(1))
        smem_match = SMEM_RE.search(line)
        if smem_match:
            current["shared_memory_bytes"] = int(smem_match.group(1))
        for cmem_match in CMEM_RE.finditer(line):
            current["constant_memory_bytes"][cmem_match.group(2)] = int(
                cmem_match.group(1)
            )

    registers = [
        function["registers"]
        for function in functions
        if function["registers"] is not None
    ]
    spill_bytes = sum(
        (function["spill_store_bytes"] or 0)
        + (function["spill_load_bytes"] or 0)
        for function in functions
    )
    return {
        "global_memory_bytes": global_memory_bytes,
        "max_registers": max(registers) if registers else None,
        "total_spill_bytes": spill_bytes,
        "functions": functions,
    }


def _new_static_metrics() -> dict[str, int]:
    return {
        "instructions": 0,
        "global_loads": 0,
        "global_stores": 0,
        "shared_loads": 0,
        "shared_stores": 0,
        "tensor_compute": 0,
        "barriers_and_fences": 0,
        "async_copies": 0,
    }


def parse_ptx_static(text: str) -> dict[str, dict[str, int]]:
    """Count stable instruction classes in emitted PTX entry bodies.

    This is deliberately a lexical inventory, not a latency or throughput
    model.  Tuple braces occur inside PTX instructions, so entry bodies end
    only at a standalone closing brace rather than through generic brace
    counting.
    """
    entries: dict[str, dict[str, int]] = {}
    pending_name: str | None = None
    current_name: str | None = None

    for raw_line in text.splitlines():
        if current_name is None and pending_name is None:
            match = PTX_ENTRY_RE.match(raw_line)
            if match:
                pending_name = match.group(1)
            continue
        if current_name is None:
            if re.fullmatch(r"\s*\{\s*", raw_line):
                assert pending_name is not None
                current_name = pending_name
                pending_name = None
                entries[current_name] = _new_static_metrics()
            continue
        if re.fullmatch(r"\s*\}\s*", raw_line):
            current_name = None
            continue

        line = raw_line.split("//", 1)[0].strip()
        if (not line or line.startswith(".") or line.endswith(":") or
                line in {"{", "}"}):
            continue
        if line.startswith("@"):
            pieces = line.split(None, 1)
            if len(pieces) != 2:
                continue
            line = pieces[1]
        opcode = line.split(None, 1)[0].rstrip(";")
        metrics = entries[current_name]
        metrics["instructions"] += 1

        is_load = opcode.startswith(("ld.", "ldu.", "wmma.load"))
        is_store = opcode.startswith(("st.", "wmma.store"))
        if is_load and ".global" in opcode:
            metrics["global_loads"] += 1
        if is_store and ".global" in opcode:
            metrics["global_stores"] += 1
        if is_load and ".shared" in opcode:
            metrics["shared_loads"] += 1
        if is_store and ".shared" in opcode:
            metrics["shared_stores"] += 1
        if opcode.startswith(("wmma.mma", "mma.", "tcgen05.mma")):
            metrics["tensor_compute"] += 1
        if opcode.startswith(("bar.", "barrier.", "membar.", "fence.")):
            metrics["barriers_and_fences"] += 1
        if opcode.startswith("cp.async"):
            metrics["async_copies"] += 1
    return entries


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _profile_one(ptxas: str, arch: str, ptx_path: Path) -> dict[str, Any]:
    with tempfile.TemporaryDirectory(prefix="mettle-ptxas-profile-") as tmp:
        cubin = Path(tmp) / "offline.cubin"
        process = subprocess.run(
            [ptxas, "-v", f"-arch={arch}", os.fspath(ptx_path), "-o", os.fspath(cubin)],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
            check=False,
        )
    if process.returncode != 0:
        raise RuntimeError(
            f"ptxas rejected {ptx_path} (exit {process.returncode}):\n"
            f"{process.stdout.rstrip()}"
        )
    parsed = parse_ptxas_verbose(process.stdout)
    if not parsed["functions"]:
        raise RuntimeError(f"ptxas produced no function resource records for {ptx_path}")
    static_entries = parse_ptx_static(
        ptx_path.read_text(encoding="utf-8", errors="replace")
    )
    for function in parsed["functions"]:
        function["static_instructions"] = static_entries.get(function["name"])
    return {
        "path": ptx_path.as_posix(),
        "sha256": _sha256(ptx_path),
        **parsed,
    }


def _parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Offline-assemble PTX and write deterministic ptxas resource JSON."
    )
    parser.add_argument("ptx", nargs="+", type=Path, help="PTX input file(s)")
    parser.add_argument("--arch", required=True, help="ptxas architecture, e.g. sm_121a")
    parser.add_argument("--ptxas", help="ptxas executable (defaults to PATH)")
    parser.add_argument("-o", "--output", type=Path, help="JSON output (default: stdout)")
    parser.add_argument("--pretty", action="store_true", help="indent JSON output")
    parser.add_argument(
        "--max-registers",
        type=int,
        help="fail when any reported function exceeds this register count",
    )
    parser.add_argument(
        "--require-zero-spills",
        action="store_true",
        help="fail when ptxas reports any spill load or store bytes",
    )
    args = parser.parse_args(argv)
    if not ARCH_RE.fullmatch(args.arch):
        parser.error("--arch must have the form sm_NN or sm_NNa")
    if args.max_registers is not None and args.max_registers < 0:
        parser.error("--max-registers must be non-negative")
    return args


def main(argv: list[str] | None = None) -> int:
    args = _parse_args(sys.argv[1:] if argv is None else argv)
    ptxas = args.ptxas or shutil.which("ptxas")
    if not ptxas:
        print("ptxas_profile: ptxas not found; pass --ptxas or add it to PATH", file=sys.stderr)
        return 2

    inputs: list[dict[str, Any]] = []
    try:
        for path in args.ptx:
            if not path.is_file():
                raise RuntimeError(f"PTX input does not exist: {path}")
            inputs.append(_profile_one(ptxas, args.arch, path))
    except (OSError, RuntimeError) as error:
        print(f"ptxas_profile: {error}", file=sys.stderr)
        return 1

    violations: list[str] = []
    for profile in inputs:
        if (
            args.max_registers is not None
            and profile["max_registers"] is not None
            and profile["max_registers"] > args.max_registers
        ):
            violations.append(
                f"{profile['path']}: max registers {profile['max_registers']} "
                f"> {args.max_registers}"
            )
        if args.require_zero_spills and profile["total_spill_bytes"] != 0:
            violations.append(
                f"{profile['path']}: total spill bytes {profile['total_spill_bytes']} != 0"
            )

    document = {
        "schema": SCHEMA,
        "architecture": args.arch,
        "inputs": inputs,
    }
    encoded = json.dumps(
        document,
        indent=2 if args.pretty else None,
        sort_keys=True,
        separators=None if args.pretty else (",", ":"),
    ) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(encoded, encoding="utf-8", newline="\n")
    else:
        sys.stdout.write(encoded)

    if violations:
        for violation in violations:
            print(f"ptxas_profile: resource gate failed: {violation}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
