#!/usr/bin/env python3
"""Select or expose tradeoffs between offline PTX resource profiles.

The tool reads JSON produced by ptxas_profile.py.  It never invokes ptxas,
loads a cubin, creates a CUDA context, queries a driver, or launches a kernel.
Its occupancy result is a bound under the explicit resource model supplied on
the command line, not a device measurement or a performance prediction.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import statistics
import sys
from pathlib import Path
from typing import Any


SCHEMA = "mettle.ptxas-resource-selection.v1"
PROFILE_SCHEMAS = {
    "mettle.ptxas-resource-profile.v1",
    "mettle.ptxas-resource-profile.v2",
}
MEASUREMENT_SCHEMA = "mettle.gpu-variant-measurements.v1"


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _round_up(value: int, unit: int) -> int:
    return ((value + unit - 1) // unit) * unit


def _parse_candidate(value: str) -> tuple[str, Path]:
    name, separator, path = value.partition("=")
    if not separator or not name or not path:
        raise argparse.ArgumentTypeError("candidate must have the form NAME=PROFILE.json")
    if any(character.isspace() for character in name):
        raise argparse.ArgumentTypeError("candidate name must not contain whitespace")
    return name, Path(path)


def _load_candidate(name: str, path: Path) -> dict[str, Any]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise RuntimeError(f"cannot read candidate {name!r} from {path}: {error}") from error
    if document.get("schema") not in PROFILE_SCHEMAS:
        raise RuntimeError(f"candidate {name!r} has an unsupported resource-profile schema")
    architecture = document.get("architecture")
    if not isinstance(architecture, str) or not architecture:
        raise RuntimeError(f"candidate {name!r} has no architecture")
    entries: dict[str, dict[str, Any]] = {}
    for source in document.get("inputs", []):
        for function in source.get("functions", []):
            if not function.get("entry"):
                continue
            entry_name = function.get("name")
            if not isinstance(entry_name, str) or not entry_name:
                raise RuntimeError(f"candidate {name!r} has an unnamed entry")
            if entry_name in entries:
                raise RuntimeError(f"candidate {name!r} repeats entry {entry_name!r}")
            entries[entry_name] = function
    if not entries:
        raise RuntimeError(f"candidate {name!r} has no entry resource records")
    return {
        "name": name,
        "path": path.as_posix(),
        "sha256": _sha256(path),
        "schema": document["schema"],
        "architecture": architecture,
        "entries": entries,
    }


def analyze_entry(function: dict[str, Any], model: dict[str, int],
                  allow_spills: bool) -> dict[str, Any]:
    disqualifiers: list[str] = []
    registers = function.get("registers")
    spill_stores = function.get("spill_store_bytes")
    spill_loads = function.get("spill_load_bytes")
    if not isinstance(registers, int) or registers < 0:
        disqualifiers.append("register count unavailable")
        registers = 0
    if not isinstance(spill_stores, int) or not isinstance(spill_loads, int):
        disqualifiers.append("spill accounting unavailable")
        spill_stores = spill_loads = 0
    spill_bytes = spill_stores + spill_loads
    if spill_bytes and not allow_spills:
        disqualifiers.append(f"{spill_bytes} spill bytes per thread")

    warps_per_block = math.ceil(model["threads_per_block"] / model["warp_size"])
    registers_per_warp = _round_up(
        registers * model["warp_size"],
        model["register_allocation_unit_per_warp"],
    )
    registers_per_block = registers_per_warp * warps_per_block
    static_shared = function.get("shared_memory_bytes")
    if static_shared is None:
        static_shared = 0
    if not isinstance(static_shared, int) or static_shared < 0:
        disqualifiers.append("shared-memory count is invalid")
        static_shared = 0
    shared_per_block = _round_up(
        static_shared + model["dynamic_shared_bytes"],
        model["shared_memory_allocation_unit"],
    )

    blocks_by_registers = (
        model["registers_per_sm"] // registers_per_block
        if registers_per_block else model["max_blocks_per_sm"]
    )
    blocks_by_shared = (
        model["shared_memory_per_sm"] // shared_per_block
        if shared_per_block else model["max_blocks_per_sm"]
    )
    blocks_by_warps = model["max_warps_per_sm"] // warps_per_block
    resident_blocks = min(
        model["max_blocks_per_sm"], blocks_by_registers,
        blocks_by_shared, blocks_by_warps,
    )
    resident_warps = resident_blocks * warps_per_block
    if resident_blocks <= 0:
        disqualifiers.append("resource model admits no resident block")

    static = function.get("static_instructions")
    global_memory_instructions: int | None = None
    if isinstance(static, dict):
        loads = static.get("global_loads")
        stores = static.get("global_stores")
        if isinstance(loads, int) and isinstance(stores, int):
            global_memory_instructions = loads + stores

    return {
        "eligible": not disqualifiers,
        "disqualifiers": disqualifiers,
        "registers_per_thread": registers,
        "registers_per_warp_allocated": registers_per_warp,
        "registers_per_block_allocated": registers_per_block,
        "static_shared_memory_bytes": static_shared,
        "shared_memory_per_block_allocated": shared_per_block,
        "spill_bytes_per_thread": spill_bytes,
        "warps_per_block": warps_per_block,
        "blocks_by_registers": blocks_by_registers,
        "blocks_by_shared_memory": blocks_by_shared,
        "blocks_by_warps": blocks_by_warps,
        "resident_blocks_per_sm": resident_blocks,
        "resident_warps_per_sm": resident_warps,
        "occupancy": round(resident_warps / model["max_warps_per_sm"], 8),
        "global_memory_instructions": global_memory_instructions,
        "static_instructions": static,
    }


def _dominates(lhs: dict[str, Any], rhs: dict[str, Any]) -> bool:
    """Return whether lhs is no worse on every known resource objective."""
    objectives: list[tuple[float, float, bool]] = [
        (lhs["occupancy"], rhs["occupancy"], True),
        (lhs["spill_bytes_per_thread"], rhs["spill_bytes_per_thread"], False),
        (lhs["registers_per_thread"], rhs["registers_per_thread"], False),
        (lhs["shared_memory_per_block_allocated"],
         rhs["shared_memory_per_block_allocated"], False),
    ]
    if (lhs["global_memory_instructions"] is not None and
            rhs["global_memory_instructions"] is not None):
        objectives.append((lhs["global_memory_instructions"],
                           rhs["global_memory_instructions"], False))
    strictly_better = False
    for left, right, higher_is_better in objectives:
        if higher_is_better:
            if left < right:
                return False
            strictly_better |= left > right
        else:
            if left > right:
                return False
            strictly_better |= left < right
    return strictly_better


def select_entry(analyses: list[dict[str, Any]], policy: str) -> tuple[list[str], str | None, str]:
    eligible = [item for item in analyses if item["analysis"]["eligible"]]
    frontier = [
        item for item in eligible
        if not any(_dominates(other["analysis"], item["analysis"])
                   for other in eligible if other is not item)
    ]
    frontier_names = [item["name"] for item in frontier]
    if not frontier:
        return frontier_names, None, "no candidate is eligible under the resource model"
    if policy == "pareto":
        if len(frontier) == 1:
            return frontier_names, frontier[0]["name"], "one candidate remains on the resource Pareto frontier"
        return frontier_names, None, "resource objectives trade off; measurements or an explicit policy are required"
    if policy == "traffic":
        if any(item["analysis"]["global_memory_instructions"] is None
               for item in eligible):
            return frontier_names, None, "traffic policy requires static PTX metrics for every eligible candidate"
        winner = min(
            eligible,
            key=lambda item: (
                item["analysis"]["global_memory_instructions"],
                item["analysis"]["spill_bytes_per_thread"],
                -item["analysis"]["occupancy"],
                item["analysis"]["registers_per_thread"],
                item["order"],
            ),
        )
        return frontier_names, winner["name"], "explicit traffic policy minimizes static global-memory instructions"
    winner = min(
        eligible,
        key=lambda item: (
            -item["analysis"]["occupancy"],
            item["analysis"]["spill_bytes_per_thread"],
            item["analysis"]["registers_per_thread"],
            item["analysis"]["shared_memory_per_block_allocated"],
            item["order"],
        ),
    )
    return frontier_names, winner["name"], "explicit occupancy policy maximizes modeled resident warps, then resource headroom"


def _sign_test_pvalue(wins: int, losses: int) -> float:
    observations = wins + losses
    if observations == 0:
        return 1.0
    tail = min(wins, losses)
    numerator = 2 * sum(math.comb(observations, index)
                        for index in range(tail + 1))
    return min(1.0, numerator / (2 ** observations))


def _measurement_samples(record: Any, minimum: int) -> list[float] | None:
    if not isinstance(record, dict):
        return None
    samples = record.get("samples_ns")
    if not isinstance(samples, list) or len(samples) < minimum:
        return None
    converted: list[float] = []
    for value in samples:
        if (not isinstance(value, (int, float)) or isinstance(value, bool) or
                not math.isfinite(value) or value <= 0):
            return None
        converted.append(float(value))
    return converted


def select_measured(
    analyses: list[dict[str, Any]], measurement_entry: dict[str, Any],
    minimum_samples: int, alpha: float, minimum_speedup: float,
) -> tuple[str | None, str, dict[str, Any]]:
    eligible = [item for item in analyses if item["analysis"]["eligible"]]
    records = measurement_entry.get("candidates")
    if not isinstance(records, dict):
        return None, "measurement entry has no candidate records", {}

    samples_by_name: dict[str, list[float]] = {}
    statistics_by_name: dict[str, Any] = {}
    for item in eligible:
        samples = _measurement_samples(records.get(item["name"]), minimum_samples)
        if samples is None:
            return None, f"candidate {item['name']} lacks valid paired timing samples", {}
        samples_by_name[item["name"]] = samples
        statistics_by_name[item["name"]] = {
            "sample_count": len(samples),
            "median_ns": statistics.median(samples),
            "minimum_ns": min(samples),
            "maximum_ns": max(samples),
        }
    if not eligible:
        return None, "no resource-eligible candidate has timing evidence", {}
    sample_counts = {len(samples) for samples in samples_by_name.values()}
    if len(sample_counts) != 1:
        return None, "paired candidate sample counts differ", {
            "candidates": statistics_by_name,
        }
    if len(eligible) == 1:
        return eligible[0]["name"], "only one resource-eligible measured candidate remains", {
            "candidates": statistics_by_name,
            "comparisons": [],
        }

    winner = min(
        eligible,
        key=lambda item: (statistics_by_name[item["name"]]["median_ns"],
                          item["order"]),
    )
    winner_name = winner["name"]
    winner_samples = samples_by_name[winner_name]
    corrected_alpha = alpha / (len(eligible) - 1)
    comparisons: list[dict[str, Any]] = []
    qualified = True
    for other in eligible:
        if other is winner:
            continue
        other_name = other["name"]
        other_samples = samples_by_name[other_name]
        wins = sum(left < right for left, right in zip(winner_samples,
                                                       other_samples))
        losses = sum(left > right for left, right in zip(winner_samples,
                                                         other_samples))
        ties = len(winner_samples) - wins - losses
        pvalue = _sign_test_pvalue(wins, losses)
        median_speedup = statistics.median(
            right / left for left, right in zip(winner_samples, other_samples)
        )
        passes = pvalue <= corrected_alpha and median_speedup >= minimum_speedup
        qualified &= passes
        comparisons.append({
            "winner": winner_name,
            "other": other_name,
            "paired_wins": wins,
            "paired_losses": losses,
            "paired_ties": ties,
            "two_sided_sign_test_pvalue": pvalue,
            "bonferroni_alpha": corrected_alpha,
            "median_speedup": median_speedup,
            "minimum_speedup": minimum_speedup,
            "passes": passes,
        })
    evidence = {
        "candidates": statistics_by_name,
        "comparisons": comparisons,
    }
    if not qualified:
        return None, "timing leader does not clear the corrected significance and effect thresholds", evidence
    return winner_name, "paired timing leader clears corrected sign-test and minimum-speedup thresholds", evidence


def _load_measurements(path: Path, architecture: str,
                       candidates: list[dict[str, Any]]) -> dict[str, Any]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise RuntimeError(f"cannot read measurements from {path}: {error}") from error
    if document.get("schema") != MEASUREMENT_SCHEMA:
        raise RuntimeError("measurement document has an unsupported schema")
    if document.get("architecture") != architecture:
        raise RuntimeError("measurement architecture does not match the candidates")
    device = document.get("device")
    required_device = {
        "name": str,
        "uuid": str,
        "host_arch": str,
        "compute_capability": str,
        "integrated_memory": bool,
        "driver_version": str,
    }
    if (not isinstance(device, dict) or any(
            not isinstance(device.get(key), kind)
            for key, kind in required_device.items()) or any(
            not device[key] for key, kind in required_device.items()
            if kind is str) or
            not re.fullmatch(r"[0-9]+\.[0-9]+", device["compute_capability"])):
        raise RuntimeError("measurement document has an incomplete device identity")
    entries = document.get("entries")
    if not isinstance(entries, dict):
        raise RuntimeError("measurement document has no entries")
    hashes = {candidate["name"]: candidate["sha256"] for candidate in candidates}
    for entry_name, entry in entries.items():
        if not isinstance(entry_name, str) or not isinstance(entry, dict):
            raise RuntimeError("measurement entry is malformed")
        workload_hash = entry.get("workload_sha256")
        if (not isinstance(workload_hash, str) or len(workload_hash) != 64 or
                any(character not in "0123456789abcdef" for character in workload_hash)):
            raise RuntimeError(f"measurement entry {entry_name!r} has no lowercase SHA-256 workload binding")
        records = entry.get("candidates")
        if not isinstance(records, dict):
            raise RuntimeError(f"measurement entry {entry_name!r} has no candidates")
        for candidate_name, record in records.items():
            if candidate_name not in hashes:
                raise RuntimeError(f"measurement entry {entry_name!r} names unknown candidate {candidate_name!r}")
            if (not isinstance(record, dict) or
                    record.get("profile_sha256") != hashes[candidate_name]):
                raise RuntimeError(f"measurement entry {entry_name!r} is not bound to candidate {candidate_name!r}")
    return {
        "path": path.as_posix(),
        "sha256": _sha256(path),
        "device": device,
        "entries": entries,
    }


def _positive(parser: argparse.ArgumentParser, name: str, value: int) -> None:
    if value <= 0:
        parser.error(f"{name} must be positive")


def _parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Compare offline PTX variants under an explicit resource model."
    )
    parser.add_argument("--candidate", action="append", required=True,
                        type=_parse_candidate, metavar="NAME=PROFILE.json")
    parser.add_argument("--entry", action="append",
                        help="entry to compare (repeatable; default: every candidate entry)")
    parser.add_argument("--threads-per-block", type=int, required=True)
    parser.add_argument("--dynamic-shared-bytes", type=int, default=0)
    parser.add_argument("--registers-per-sm", type=int, required=True)
    parser.add_argument("--shared-memory-per-sm", type=int, required=True)
    parser.add_argument("--max-warps-per-sm", type=int, required=True)
    parser.add_argument("--max-blocks-per-sm", type=int, required=True)
    parser.add_argument("--warp-size", type=int, default=32)
    parser.add_argument("--register-allocation-unit-per-warp", type=int, default=1)
    parser.add_argument("--shared-memory-allocation-unit", type=int, default=1)
    parser.add_argument("--allow-spills", action="store_true")
    parser.add_argument("--policy", choices=("pareto", "occupancy", "traffic", "measured"),
                        default="pareto")
    parser.add_argument("--measurements", type=Path,
                        help="profile-hash-bound paired timing JSON for --policy=measured")
    parser.add_argument("--minimum-samples", type=int, default=21)
    parser.add_argument("--alpha", type=float, default=0.01)
    parser.add_argument("--minimum-speedup", type=float, default=1.01)
    parser.add_argument("--require-selection", action="store_true")
    parser.add_argument("--pretty", action="store_true")
    parser.add_argument("-o", "--output", type=Path)
    args = parser.parse_args(argv)
    for name in ("threads_per_block", "registers_per_sm", "shared_memory_per_sm",
                 "max_warps_per_sm", "max_blocks_per_sm", "warp_size",
                 "register_allocation_unit_per_warp",
                 "shared_memory_allocation_unit"):
        _positive(parser, "--" + name.replace("_", "-"), getattr(args, name))
    if args.dynamic_shared_bytes < 0:
        parser.error("--dynamic-shared-bytes must be non-negative")
    if args.minimum_samples < 3:
        parser.error("--minimum-samples must be at least 3")
    if not math.isfinite(args.alpha) or not 0 < args.alpha < 1:
        parser.error("--alpha must be between 0 and 1")
    if not math.isfinite(args.minimum_speedup) or args.minimum_speedup < 1:
        parser.error("--minimum-speedup must be at least 1")
    if (args.policy == "measured") != (args.measurements is not None):
        parser.error("--policy=measured and --measurements must be used together")
    names = [name for name, _ in args.candidate]
    if len(names) != len(set(names)):
        parser.error("candidate names must be unique")
    return args


def main(argv: list[str] | None = None) -> int:
    args = _parse_args(sys.argv[1:] if argv is None else argv)
    try:
        candidates = [_load_candidate(name, path) for name, path in args.candidate]
        architectures = {candidate["architecture"] for candidate in candidates}
        if len(architectures) != 1:
            raise RuntimeError("candidate profiles target different architectures")
        all_entries = sorted({entry for candidate in candidates
                              for entry in candidate["entries"]})
        entries = args.entry or all_entries
        if len(entries) != len(set(entries)):
            raise RuntimeError("entry filters must be unique")
        unknown = sorted(set(entries) - set(all_entries))
        if unknown:
            raise RuntimeError("unknown entry filter(s): " + ", ".join(unknown))
        measurements = (
            _load_measurements(args.measurements, next(iter(architectures)),
                               candidates)
            if args.measurements else None
        )
    except RuntimeError as error:
        print(f"ptxas_select: {error}", file=sys.stderr)
        return 1

    model = {
        "threads_per_block": args.threads_per_block,
        "dynamic_shared_bytes": args.dynamic_shared_bytes,
        "registers_per_sm": args.registers_per_sm,
        "shared_memory_per_sm": args.shared_memory_per_sm,
        "max_warps_per_sm": args.max_warps_per_sm,
        "max_blocks_per_sm": args.max_blocks_per_sm,
        "warp_size": args.warp_size,
        "register_allocation_unit_per_warp": args.register_allocation_unit_per_warp,
        "shared_memory_allocation_unit": args.shared_memory_allocation_unit,
    }
    results: dict[str, Any] = {}
    unresolved = False
    for entry in entries:
        analyses: list[dict[str, Any]] = []
        for order, candidate in enumerate(candidates):
            function = candidate["entries"].get(entry)
            if function is None:
                analysis = {
                    "eligible": False,
                    "disqualifiers": ["entry is absent from this candidate"],
                }
            else:
                analysis = analyze_entry(function, model, args.allow_spills)
            analyses.append({"name": candidate["name"], "order": order,
                             "analysis": analysis})
        frontier, selected, reason = select_entry(
            analyses, "pareto" if args.policy == "measured" else args.policy
        )
        measurement_evidence = None
        if args.policy == "measured":
            measurement_entry = measurements["entries"].get(entry)
            if not isinstance(measurement_entry, dict):
                selected = None
                reason = "selected entry has no timing evidence"
            elif (measurement_entry.get("threads_per_block") !=
                  model["threads_per_block"] or
                  measurement_entry.get("dynamic_shared_bytes") !=
                  model["dynamic_shared_bytes"]):
                selected = None
                reason = "measurement launch shape does not match the resource model"
            else:
                selected, reason, measurement_evidence = select_measured(
                    analyses, measurement_entry, args.minimum_samples,
                    args.alpha, args.minimum_speedup,
                )
        unresolved |= selected is None
        results[entry] = {
            "selected": selected,
            "reason": reason,
            "pareto_frontier": frontier,
            "candidates": {item["name"]: item["analysis"] for item in analyses},
        }
        if measurement_evidence is not None:
            results[entry]["measurement_evidence"] = measurement_evidence

    document = {
        "schema": SCHEMA,
        "architecture": next(iter(architectures)),
        "policy": args.policy,
        "performance_claim": False,
        "measurement_evidence": measurements is not None,
        "resource_model": model,
        "candidates": [
            {key: candidate[key] for key in ("name", "path", "sha256", "schema")}
            for candidate in candidates
        ],
        "entries": results,
        "limitations": [
            "occupancy is a static bound under the supplied model, not a device query",
            "static instruction counts do not predict latency or throughput",
            "a multi-candidate Pareto frontier requires measurements or an explicit policy",
        ],
    }
    if measurements is not None:
        document["measurements"] = {
            "path": measurements["path"],
            "sha256": measurements["sha256"],
            "device": measurements["device"],
            "minimum_samples": args.minimum_samples,
            "alpha": args.alpha,
            "minimum_speedup": args.minimum_speedup,
        }
    encoded = json.dumps(
        document, indent=2 if args.pretty else None, sort_keys=True,
        separators=None if args.pretty else (",", ":"),
    ) + "\n"
    try:
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(encoded, encoding="utf-8", newline="\n")
        else:
            sys.stdout.write(encoded)
    except OSError as error:
        print(f"ptxas_select: cannot write output: {error}", file=sys.stderr)
        return 1
    if args.require_selection and unresolved:
        print("ptxas_select: at least one entry has no defensible selection", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
