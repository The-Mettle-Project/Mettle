#!/usr/bin/env python3
"""Fix binary codegen modules: remove forward decls, regenerate internal.h."""

from __future__ import annotations

import re
from pathlib import Path

BINARY = Path(__file__).resolve().parents[1] / "src" / "codegen" / "binary"

C_KEYWORDS = frozenset(
    {"if", "for", "while", "switch", "return", "else", "case", "default", "do", "const", "sizeof", "strcmp"}
)

VALID_RETURN_TOKENS = (
    "int",
    "void",
    "char",
    "size_t",
    "Type",
    "Binary",
    "IRFunction",
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
    "BinaryLabelEntry",
    "BinaryGlobalConstEntry",
)


def remove_forward_declarations(text: str) -> str:
    lines = text.splitlines(keepends=True)
    out: list[str] = []
    i = 0
    while i < len(lines):
        line = lines[i]
        if re.match(
            r"^(?:static\s+)?(?:int|void|size_t|Type \*|const char \*|IRFunction \*|BinaryGlobalConstEntry \*)\s+\w+\s*\(",
            line,
        ):
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


def parse_function_start(line: str) -> tuple[str, str] | None:
    m = re.match(r"^(?:static\s+)?(.+?\*)\s*(\w+)\s*\(", line)
    if m:
        return m.group(1).strip(), m.group(2)
    m = re.match(r"^(?:static\s+)?([\w\s]+)\s+(\w+)\s*\(", line)
    if m:
        return m.group(1).strip(), m.group(2)
    return None


def find_function_definitions(source: str) -> list[tuple[str, str]]:
    lines = source.splitlines()
    results: list[tuple[str, str]] = []
    seen: set[str] = set()
    i = 0
    while i < len(lines):
        line = lines[i]
        prev = lines[i - 1].strip() if i > 0 else ""
        is_static_function = line.startswith("static ") or bool(
            re.match(r"^static\s+.+\*\s*$", prev)
        )
        if is_static_function:
            parsed = parse_function_start(line)
            if not parsed:
                m = re.match(r"^(\w+)\s*\(", line)
                if m and i > 0 and lines[i - 1].strip().startswith("static "):
                    parsed = ("", m.group(1))
            if parsed:
                parts = [line.strip()]
                depth = line.count("(") - line.count(")")
                i += 1
                while i < len(lines) and depth > 0:
                    parts.append(lines[i].strip())
                    depth += lines[i].count("(") - lines[i].count(")")
                    i += 1
                continue

        multiline_ret = False
        parsed = parse_function_start(line)
        if not parsed:
            m = re.match(r"^(\w+)\s*\(", line)
            if m and i > 0:
                prev = lines[i - 1].strip()
                if prev.endswith("*") or prev.endswith("*;"):
                    ret_prefix = prev.rstrip(";").strip()
                    parsed = (ret_prefix, m.group(1))
                    multiline_ret = True
        if not parsed:
            i += 1
            continue
        ret_prefix, name = parsed
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
        sig = joined[:-1].strip()
        sig = " ".join(sig.split())
        if multiline_ret:
            paren = sig.index("(")
            sig = f"{ret_prefix} {name}{sig[paren:]}"
        if name in seen:
            continue
        seen.add(name)
        results.append((name, sig))
    return results


def build_internal_header(module_sources: dict[str, str]) -> str:
    type_block = (BINARY / "internal.h").read_text(encoding="utf-8")
    start = type_block.index("typedef enum {")
    end = type_block.index("extern const BinaryGpRegister")
    typedef_block = type_block[start:end]

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


def main() -> None:
    module_sources: dict[str, str] = {}
    for path in sorted(BINARY.glob("*.c")):
        cleaned = remove_forward_declarations(path.read_text(encoding="utf-8"))
        path.write_text(cleaned, encoding="utf-8")
        module_sources[path.name] = cleaned
        print(f"Cleaned {path.name}")

    header = build_internal_header(module_sources)
    (BINARY / "internal.h").write_text(header, encoding="utf-8")
    print(f"Regenerated internal.h ({len(header.splitlines())} lines)")


if __name__ == "__main__":
    main()
