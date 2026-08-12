#!/usr/bin/env python3
"""Regenerate internal.h prototypes from binary/*.c sources."""

from fix_binary_codegen import BINARY, build_internal_header

def main() -> None:
    module_sources = {
        path.name: path.read_text(encoding="utf-8") for path in sorted(BINARY.glob("*.c"))
    }
    header = build_internal_header(module_sources)
    (BINARY / "internal.h").write_text(header, encoding="utf-8")
    print(f"Regenerated internal.h ({len(header.splitlines())} lines)")

if __name__ == "__main__":
    main()
