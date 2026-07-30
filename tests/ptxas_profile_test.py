#!/usr/bin/env python3
"""Host-only parser tests for tools/gpu/ptxas_profile.py."""

from __future__ import annotations

import importlib.util
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "tools" / "gpu" / "ptxas_profile.py"
SPEC = importlib.util.spec_from_file_location("ptxas_profile", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
PROFILE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PROFILE)


def test_multiple_functions_and_resources() -> None:
    log = """
ptxas info    : 0 bytes gmem
ptxas info    : Compiling entry function 'resident' for 'sm_121a'
ptxas info    : Function properties for resident
    16 bytes stack frame, 8 bytes spill stores, 4 bytes spill loads
ptxas info    : Used 58 registers, used 1 barriers, 4096 bytes smem, 400 bytes cmem[0]
ptxas info    : Function properties for helper
    0 bytes stack frame, 0 bytes spill stores, 0 bytes spill loads
ptxas info    : Used 22 registers, used 0 barriers, 24 bytes cmem[2]
"""
    parsed = PROFILE.parse_ptxas_verbose(log)
    assert parsed["global_memory_bytes"] == 0
    assert parsed["max_registers"] == 58
    assert parsed["total_spill_bytes"] == 12
    assert len(parsed["functions"]) == 2
    resident, helper = parsed["functions"]
    assert resident == {
        "name": "resident",
        "entry": True,
        "registers": 58,
        "barriers": 1,
        "stack_frame_bytes": 16,
        "spill_store_bytes": 8,
        "spill_load_bytes": 4,
        "shared_memory_bytes": 4096,
        "constant_memory_bytes": {"0": 400},
    }
    assert helper["entry"] is False
    assert helper["registers"] == 22
    assert helper["constant_memory_bytes"] == {"2": 24}


def test_missing_optional_resource_line_is_explicit() -> None:
    log = """
ptxas info    : Compiling entry function 'leaf' for 'sm_75'
ptxas info    : Function properties for leaf
    0 bytes stack frame, 0 bytes spill stores, 0 bytes spill loads
"""
    parsed = PROFILE.parse_ptxas_verbose(log)
    assert parsed["global_memory_bytes"] is None
    assert parsed["max_registers"] is None
    assert parsed["total_spill_bytes"] == 0
    assert parsed["functions"][0]["entry"] is True
    assert parsed["functions"][0]["registers"] is None


def test_static_entry_metrics() -> None:
    ptx = r"""
.visible .entry resident(
    .param .u64 resident_p0
)
{
    .reg .b64 %rd<2>;
    ld.global.u32 %r0, [%rd0];
    wmma.load.c.sync.aligned.m16n16k16.global.row.f32 {%f0}, [%rd0], %r0;
    wmma.mma.sync.aligned.m16n16k16.row.col.f32.f32 {%f1}, {%r1}, {%r2}, {%f0};
    @%p0 st.shared.u32 [%rd1], %r0;
    wmma.store.d.sync.aligned.m16n16k16.global.row.f32 [%rd0], {%f1}, %r0;
    bar.warp.sync 0xffffffff;
    cp.async.ca.shared.global [%rd1], [%rd0], 16;
    ret;
}
"""
    assert PROFILE.parse_ptx_static(ptx) == {
        "resident": {
            "instructions": 8,
            "global_loads": 2,
            "global_stores": 1,
            "shared_loads": 0,
            "shared_stores": 1,
            "tensor_compute": 1,
            "barriers_and_fences": 1,
            "async_copies": 1,
        }
    }


def main() -> None:
    test_multiple_functions_and_resources()
    test_missing_optional_resource_line_is_explicit()
    test_static_entry_metrics()
    print("ptxas_profile_test: all parser cases OK")


if __name__ == "__main__":
    main()
