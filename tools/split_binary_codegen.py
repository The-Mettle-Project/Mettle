#!/usr/bin/env python3
"""Split code_generator_binary.c into src/codegen/binary/ modules."""

from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src" / "codegen"
BINARY = SRC / "binary"
SOURCE = SRC / "code_generator_binary.c.bak"

MODULES = {
    "support.c": [
        (182, 204),
        (384, 400),
        (528, 915),
    ],
    "globals.c": [
        (174, 174),
        (206, 383),
        (421, 421),
        (423, 526),
        (11278, 11827),
    ],
    "encoders.c": [
        (152, 155),
        (916, 2332),
    ],
    "abi.c": [
        (2334, 3831),
    ],
    "emit.c": [
        (3832, 7583),
        (8956, 9533),
    ],
    "simd.c": [
        (7584, 8954),
    ],
    "peephole.c": [
        (9535, 11075),
    ],
    "driver.c": [
        (11076, 11276),
        (11828, 11939),
    ],
}

INCLUDE = (
    '#include "codegen/binary/internal.h"\n\n'
    "#include <limits.h>\n#include <stdint.h>\n#include <stdlib.h>\n"
    "#include <string.h>\n\n"
)

STATIC_VAR_PATTERNS = (
    re.compile(r"^static\s+(const\s+)?[\w\s\*]+\s+g_\w+"),
    re.compile(r"^static\s+const\s+Binary"),
)


def read_lines() -> list[str]:
    if not SOURCE.exists():
        raise SystemExit(f"Missing backup source: {SOURCE}")
    return SOURCE.read_text(encoding="utf-8").splitlines(keepends=True)


def strip_static_from_functions(text: str) -> str:
    out: list[str] = []
    for line in text.splitlines(keepends=True):
        if line.startswith("static "):
            stripped = line[7:]
            if any(p.match(line) for p in STATIC_VAR_PATTERNS):
                out.append(line)
            elif re.match(r"^[\w\s\*]+\([^;]*\)\s*;\s*$", stripped):
                out.append(line)
            elif "(" in stripped and not stripped.strip().endswith(";"):
                out.append(stripped)
            else:
                out.append(line)
        else:
            out.append(line)
    return "".join(out)


def extract_ranges(lines: list[str], ranges: list[tuple[int, int]]) -> str:
    chunks: list[str] = []
    for start, end in ranges:
        chunks.append("".join(lines[start - 1 : end]))
    text = strip_static_from_functions("".join(chunks))
    return remove_forward_declarations(text)


C_KEYWORDS = frozenset(
    {
        "if",
        "for",
        "while",
        "switch",
        "return",
        "else",
        "case",
        "default",
        "do",
        "const",
        "sizeof",
        "strcmp",
    }
)


VALID_RETURN_TOKENS = (
    "int",
    "void",
    "char",
    "size_t",
    "Type",
    "Binary",
    "IR",
    "uint",
    "const",
    "unsigned",
    "double",
    "long",
    "Function",
    "Program",
    "AST",
    "Symbol",
    "AbiPassKind",
    "BinaryGlobalConstEntry",
)


def remove_forward_declarations(text: str) -> str:
    """Drop file-local forward declarations; internal.h provides prototypes."""
    lines = text.splitlines(keepends=True)
    out: list[str] = []
    i = 0
    while i < len(lines):
        line = lines[i]
        if re.match(r"^static\s+", line) and "(" in line:
            block = [line]
            depth = line.count("(") - line.count(")")
            j = i + 1
            while j < len(lines) and depth > 0:
                block.append(lines[j])
                depth += lines[j].count("(") - lines[j].count(")")
                j += 1
            joined = "".join(block).strip()
            if joined.endswith(";") and "{" not in joined:
                i = j
                continue
        out.append(line)
        i += 1
    return "".join(out)


def return_type_looks_valid(prefix: str) -> bool:
    return any(token in prefix for token in VALID_RETURN_TOKENS)


def find_function_definitions(source: str) -> list[tuple[str, str]]:
    """Return (name, signature) for each top-level function in a module."""
    lines = source.splitlines()
    results: list[tuple[str, str]] = []
    seen: set[str] = set()
    i = 0
    while i < len(lines):
        line = lines[i]
        m = re.match(r"^(?:static\s+)?([\w\s\*]+?\s+(\w+)\s*\()", line)
        if not m:
            i += 1
            continue
        ret_prefix = m.group(1)
        name = m.group(2)
        if name in C_KEYWORDS or not re.match(r"^[a-z_][\w]*$", name):
            i += 1
            continue
        if not return_type_looks_valid(ret_prefix):
            i += 1
            continue
        parts = [line.strip()]
        depth = line.count("(") - line.count(")")
        i += 1
        while i < len(lines) and depth > 0:
            parts.append(lines[i].strip())
            depth += lines[i].count("(") - lines[i].count(")")
            i += 1
        joined = " ".join(parts)
        if not joined.rstrip().endswith("{"):
            continue
        sig = joined[:-1].strip() if joined.rstrip().endswith("{") else joined.strip()
        sig = " ".join(sig.split())
        if name in seen:
            continue
        seen.add(name)
        results.append((name, sig))
    return results


def build_internal_header(lines: list[str], module_sources: dict[str, str]) -> str:
    type_parts = [
        "".join(lines[12:151]),
        "".join(lines[156:172]),
        "".join(lines[175:180]),
        "".join(lines[401:419]),
    ]
    typedef_block = "".join(type_parts)

    protos: list[str] = []
    seen: set[str] = set()
    for source in module_sources.values():
        for name, sig in find_function_definitions(source):
            if name in seen:
                continue
            seen.add(name)
            protos.append(f"{sig};")

    protos.sort(key=lambda p: p.split("(")[0].split()[-1].lower())

    return f"""#ifndef CODEGEN_BINARY_INTERNAL_H
#define CODEGEN_BINARY_INTERNAL_H

#include "codegen/code_generator_internal.h"

#include <stddef.h>
#include <stdint.h>

#define BINARY_TEXT_SECTION_ALIGNMENT 16
#define BINARY_FUNCTION_STACK_SLOT_SIZE 8
#define BINARY_WIN64_REGISTER_ARG_COUNT 4
#define BINARY_WIN64_SHADOW_SPACE_SIZE 32
#define BINARY_STACK_PAGE_SIZE 4096

{typedef_block}

extern const BinaryGpRegister BINARY_WIN64_INT_PARAM_REGISTERS[];
extern const BinaryXmmRegister BINARY_WIN64_FLOAT_PARAM_REGISTERS[];

extern BinaryGlobalConstTable g_binary_global_consts;
extern BinaryIRFunctionIndex g_binary_ir_function_index;

{chr(10).join(protos)}

#endif /* CODEGEN_BINARY_INTERNAL_H */
"""


def remove_old_flat_files() -> None:
    old_names = [
        "code_generator_binary.c",
        "code_generator_binary_internal.h",
        "code_generator_binary_support.c",
        "code_generator_binary_globals.c",
        "code_generator_binary_encoders.c",
        "code_generator_binary_abi.c",
        "code_generator_binary_emit.c",
        "code_generator_binary_simd.c",
        "code_generator_binary_peephole.c",
    ]
    for name in old_names:
        path = SRC / name
        if path.exists():
            path.unlink()


def main() -> None:
    lines = read_lines()
    BINARY.mkdir(parents=True, exist_ok=True)

    module_sources: dict[str, str] = {}
    for filename, ranges in MODULES.items():
        module_sources[filename] = INCLUDE + extract_ranges(lines, ranges)

    header = build_internal_header(lines, module_sources)

    remove_old_flat_files()

    for filename, content in module_sources.items():
        (BINARY / filename).write_text(content, encoding="utf-8")
        print(f"Wrote binary/{filename} ({len(content.splitlines())} lines)")

    (BINARY / "internal.h").write_text(header, encoding="utf-8")
    print(f"Wrote binary/internal.h ({len(header.splitlines())} lines)")


if __name__ == "__main__":
    main()
