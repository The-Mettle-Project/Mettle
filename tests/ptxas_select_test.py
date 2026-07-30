#!/usr/bin/env python3
"""Host-only tests for the offline PTX resource selector."""

from __future__ import annotations

import importlib.util
import json
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "tools" / "gpu" / "ptxas_select.py"
SPEC = importlib.util.spec_from_file_location("ptxas_select", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
SELECT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SELECT)


MODEL = {
    "threads_per_block": 32,
    "dynamic_shared_bytes": 0,
    "registers_per_sm": 65536,
    "shared_memory_per_sm": 65536,
    "max_warps_per_sm": 64,
    "max_blocks_per_sm": 32,
    "warp_size": 32,
    "register_allocation_unit_per_warp": 256,
    "shared_memory_allocation_unit": 256,
}


def function(registers: int, loads: int, stores: int,
             spill_bytes: int = 0) -> dict[str, object]:
    return {
        "registers": registers,
        "spill_store_bytes": spill_bytes,
        "spill_load_bytes": 0,
        "shared_memory_bytes": None,
        "static_instructions": {
            "instructions": 40,
            "global_loads": loads,
            "global_stores": stores,
            "shared_loads": 0,
            "shared_stores": 0,
            "tensor_compute": 4,
            "barriers_and_fences": 0,
            "async_copies": 0,
        },
    }


def item(name: str, order: int, profile: dict[str, object]) -> dict[str, object]:
    return {
        "name": name,
        "order": order,
        "analysis": SELECT.analyze_entry(profile, MODEL, False),
    }


def test_pareto_refuses_to_invent_winner() -> None:
    resident = item("resident", 0, function(58, 6, 1))
    replay = item("replay", 1, function(42, 9, 4))
    frontier, selected, reason = SELECT.select_entry([resident, replay], "pareto")
    assert frontier == ["resident", "replay"]
    assert selected is None
    assert "measurements" in reason


def test_explicit_policies_are_auditable() -> None:
    resident = item("resident", 0, function(58, 6, 1))
    replay = item("replay", 1, function(42, 9, 4))
    _, occupancy, _ = SELECT.select_entry([resident, replay], "occupancy")
    _, traffic, _ = SELECT.select_entry([resident, replay], "traffic")
    assert occupancy == "replay"
    assert traffic == "resident"


def test_spills_fail_closed_and_dominance_selects() -> None:
    clean = item("clean", 0, function(32, 4, 1))
    expensive = item("expensive", 1, function(48, 6, 2))
    spilled = item("spilled", 2, function(24, 2, 1, spill_bytes=8))
    frontier, selected, _ = SELECT.select_entry(
        [clean, expensive, spilled], "pareto"
    )
    assert spilled["analysis"]["eligible"] is False
    assert frontier == ["clean"]
    assert selected == "clean"


def test_measured_selection_requires_effect_and_significance() -> None:
    analyses = [
        item("resident", 0, function(58, 6, 1)),
        item("replay", 1, function(42, 9, 4)),
    ]
    strong = {
        "candidates": {
            "resident": {"samples_ns": [100 + (index % 3) for index in range(21)]},
            "replay": {"samples_ns": [120 + (index % 3) for index in range(21)]},
        }
    }
    selected, reason, evidence = SELECT.select_measured(
        analyses, strong, 21, 0.01, 1.01
    )
    assert selected == "resident"
    assert "clears" in reason
    assert evidence["comparisons"][0]["passes"] is True

    weak = {
        "candidates": {
            "resident": {"samples_ns": [100 + (index % 2) for index in range(21)]},
            "replay": {"samples_ns": [101 - (index % 2) for index in range(21)]},
        }
    }
    selected, reason, evidence = SELECT.select_measured(
        analyses, weak, 21, 0.01, 1.01
    )
    assert selected is None
    assert "does not clear" in reason
    assert evidence["comparisons"][0]["passes"] is False


def write_profile(path: Path, entry: dict[str, object]) -> None:
    function_record = {
        "name": "tensor_chain4",
        "entry": True,
        "barriers": 0,
        "stack_frame_bytes": 0,
        "constant_memory_bytes": {},
        **entry,
    }
    path.write_text(
        json.dumps({
            "schema": "mettle.ptxas-resource-profile.v2",
            "architecture": "sm_121a",
            "inputs": [{
                "path": "fixture.ptx",
                "sha256": "0" * 64,
                "functions": [function_record],
            }],
        }),
        encoding="utf-8",
    )


def test_cli_document_and_fail_closed_selection() -> None:
    schema = json.loads(
        (ROOT / "tools" / "gpu" / "variant_measurements.schema.json")
        .read_text(encoding="utf-8")
    )
    assert schema["properties"]["schema"]["const"] == SELECT.MEASUREMENT_SCHEMA
    with tempfile.TemporaryDirectory(prefix="mettle-select-test-") as directory:
        root = Path(directory)
        resident_path = root / "resident.json"
        replay_path = root / "replay.json"
        output_path = root / "selection.json"
        write_profile(resident_path, function(58, 6, 1))
        write_profile(replay_path, function(42, 9, 4))
        arguments = [
            "--candidate", f"resident={resident_path}",
            "--candidate", f"replay={replay_path}",
            "--entry", "tensor_chain4",
            "--threads-per-block", "32",
            "--registers-per-sm", "65536",
            "--shared-memory-per-sm", "102400",
            "--max-warps-per-sm", "48",
            "--max-blocks-per-sm", "24",
            "--policy", "pareto",
            "--output", str(output_path),
        ]
        assert SELECT.main(arguments) == 0
        document = json.loads(output_path.read_text(encoding="utf-8"))
        result = document["entries"]["tensor_chain4"]
        assert document["performance_claim"] is False
        assert result["selected"] is None
        assert result["pareto_frontier"] == ["resident", "replay"]
        assert SELECT.main(arguments + ["--require-selection"]) == 1

        measurements_path = root / "measurements.json"
        measured_output_path = root / "measured-selection.json"
        measurements_path.write_text(
            json.dumps({
                "schema": "mettle.gpu-variant-measurements.v1",
                "architecture": "sm_121a",
                "device": {
                    "name": "recoverable-fixture",
                    "uuid": "fixture-uuid",
                    "host_arch": "aarch64",
                    "compute_capability": "12.1",
                    "integrated_memory": True,
                    "driver_version": "fixture",
                },
                "entries": {
                    "tensor_chain4": {
                        "workload_sha256": "a" * 64,
                        "threads_per_block": 32,
                        "dynamic_shared_bytes": 0,
                        "candidates": {
                            "resident": {
                                "profile_sha256": SELECT._sha256(resident_path),
                                "samples_ns": [100 + (index % 3) for index in range(21)],
                            },
                            "replay": {
                                "profile_sha256": SELECT._sha256(replay_path),
                                "samples_ns": [120 + (index % 3) for index in range(21)],
                            },
                        },
                    }
                },
            }),
            encoding="utf-8",
        )
        common_arguments = arguments[:-4]
        measured_arguments = common_arguments + [
            "--policy", "measured",
            "--measurements", str(measurements_path),
            "--output", str(measured_output_path),
            "--require-selection",
        ]
        assert SELECT.main(measured_arguments) == 0
        measured = json.loads(measured_output_path.read_text(encoding="utf-8"))
        assert measured["measurement_evidence"] is True
        assert measured["performance_claim"] is False
        assert measured["entries"]["tensor_chain4"]["selected"] == "resident"

        invalid = json.loads(measurements_path.read_text(encoding="utf-8"))
        invalid["entries"]["tensor_chain4"]["candidates"]["resident"][
            "profile_sha256"
        ] = "0" * 64
        measurements_path.write_text(json.dumps(invalid), encoding="utf-8")
        assert SELECT.main(measured_arguments) == 1


def main() -> None:
    test_pareto_refuses_to_invent_winner()
    test_explicit_policies_are_auditable()
    test_spills_fail_closed_and_dominance_selects()
    test_measured_selection_requires_effect_and_significance()
    test_cli_document_and_fail_closed_selection()
    print("ptxas_select_test: all resource-selection cases OK")


if __name__ == "__main__":
    main()
