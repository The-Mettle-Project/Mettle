from pathlib import Path

p = Path("src/ir/ir_optimize.c")
text = p.read_text(encoding="utf-8")
needle = "static const char *ir_find_ptr_step_with_suffix"
first = text.index(needle)
second = text.index(needle, first + 1)
end = text.index("static int ir_symbol_is_i32_ptr_param", second)
block = text[second:end]
text = text[:second] + text[end:]
marker = "static int ir_simd_minmax_i32_pass(IRFunction *function, int *changed) {"
if marker not in text:
    raise SystemExit("marker not found")
text = text.replace(marker, block + marker, 1)
p.write_text(text, encoding="utf-8")
print(f"moved {len(block)} chars")
