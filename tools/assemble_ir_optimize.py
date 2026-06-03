#!/usr/bin/env python3
"""Surgical restore: working base + recovered optimizer passes."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / "src/ir/recovery/ir_optimize.c.from_disk.bak"
OUT = ROOT / "src/ir/ir_optimize.c"
RECON = ROOT / "src/ir/recovery/ir_optimize_reconstructed.c"
IDIOMS = ROOT / "src/ir/recovery/idioms_popcount_collatz.c"
HELPERS = ROOT / "src/ir/recovery/helpers_missing.c"
PATCH_MINMAX = ROOT / "src/ir/recovery/extracted_patches/0007_StrReplace_143b215f-8f76-40ef-8515-6315f91ba863.jsonl_46.txt"
PATCH_MEMMAP = ROOT / "src/ir/recovery/extracted_patches/0057_StrReplace_98597754-fe35-4e44-8d45-8a8f013d787a.jsonl_237.txt"
PATCH_DRIVER = ROOT / "src/ir/recovery/extracted_patches/0030_StrReplace_40533783-9944-4a31-bcf8-94b63e5e82a4.jsonl_71.txt"
INSERT_MOVE = ROOT / "src/ir/opt/util.c"

DOT_PASS = r'''
static int ir_try_vectorize_dot_i32_at(IRFunction *function, size_t header_index,
                                       int *changed) {
  size_t compare_index = 0;
  size_t branch_index = 0;
  size_t jump_index = (size_t)-1;
  size_t increment_index = 0;
  const char *iv_symbol = NULL;
  const char *sum_symbol = NULL;
  const char *a_symbol = NULL;
  const char *b_symbol = NULL;
  const char *sum_type = NULL;
  const char *loop_label = NULL;
  IRInstruction fused = {0};
  IROperand len = {0};
  int has_mul_add = 0;

  if (!function || header_index + 4 >= function->instruction_count) {
    return 1;
  }

  IRInstruction *header = &function->instructions[header_index];
  if (header->op != IR_OP_LABEL || !ir_label_is_while_header(header->text)) {
    return 1;
  }
  loop_label = header->text;

  if (!ir_find_next_non_nop(function, header_index + 1, &compare_index) ||
      !ir_find_next_non_nop(function, compare_index + 1, &branch_index)) {
    return 1;
  }

  IRInstruction *compare = &function->instructions[compare_index];
  IRInstruction *branch = &function->instructions[branch_index];
  if (compare->op != IR_OP_BINARY || compare->is_float || !compare->text ||
      strcmp(compare->text, "<") != 0 ||
      compare->dest.kind != IR_OPERAND_TEMP || !compare->dest.name ||
      compare->lhs.kind != IR_OPERAND_SYMBOL || !compare->lhs.name ||
      compare->rhs.kind != IR_OPERAND_SYMBOL || !compare->rhs.name ||
      branch->op != IR_OP_BRANCH_ZERO ||
      !ir_operand_is_temp_named(&branch->lhs, compare->dest.name) ||
      !branch->text) {
    return 1;
  }

  iv_symbol = compare->lhs.name;
  if (!ir_operand_clone(&compare->rhs, &len)) {
    return 0;
  }
  if (!ir_symbol_is_sum_loop_bound(function, compare->rhs.name)) {
    ir_operand_destroy(&len);
    return 1;
  }

  for (size_t i = branch_index + 1; i < function->instruction_count; i++) {
    if (function->instructions[i].op == IR_OP_JUMP &&
        function->instructions[i].text &&
        strcmp(function->instructions[i].text, loop_label) == 0) {
      jump_index = i;
      break;
    }
    if (function->instructions[i].op == IR_OP_LABEL &&
        function->instructions[i].text &&
        strcmp(function->instructions[i].text, branch->text) == 0) {
      break;
    }
  }
  if (jump_index == (size_t)-1) {
    ir_operand_destroy(&len);
    return 1;
  }

  if (ir_loop_body_has_nested_while(function, branch_index + 1, jump_index)) {
    ir_operand_destroy(&len);
    return 1;
  }

  increment_index = jump_index;
  while (increment_index > branch_index + 1) {
    increment_index--;
    if (function->instructions[increment_index].op != IR_OP_NOP) {
      break;
    }
  }
  if (!ir_try_parse_direct_unit_increment(
          &function->instructions[increment_index], iv_symbol)) {
    ir_operand_destroy(&len);
    return 1;
  }

  for (size_t i = branch_index + 1; i < jump_index; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_NOP) {
      continue;
    }
    if (ins->op == IR_OP_BINARY && ins->text && strcmp(ins->text, "+") == 0 &&
        !ins->is_float && ins->dest.kind == IR_OPERAND_SYMBOL &&
        ins->dest.name && ins->lhs.kind == IR_OPERAND_SYMBOL &&
        ir_operand_is_symbol_named(&ins->lhs, ins->dest.name) &&
        ins->rhs.kind == IR_OPERAND_TEMP) {
      const IRInstruction *mul =
          ir_find_temp_producer_before(function, i, ins->rhs.name);
      if (mul && mul->op == IR_OP_BINARY && mul->text &&
          strcmp(mul->text, "*") == 0 && !mul->is_float) {
        has_mul_add = 1;
        sum_symbol = ins->dest.name;
      }
    }
    if (ins->op == IR_OP_BINARY && ins->text && strcmp(ins->text, "<<") == 0 &&
        ins->rhs.kind == IR_OPERAND_INT && ins->rhs.int_value == 2 &&
        ir_operand_is_symbol_named(&ins->lhs, iv_symbol)) {
      for (size_t j = i + 1; j < jump_index; j++) {
        const IRInstruction *load = &function->instructions[j];
        if (load->op != IR_OP_LOAD || load->rhs.kind != IR_OPERAND_INT ||
            load->rhs.int_value != 4) {
          continue;
        }
        const IRInstruction *addr = ir_find_temp_producer_before(
            function, j, load->lhs.name);
        if (addr && addr->op == IR_OP_BINARY && addr->text &&
            strcmp(addr->text, "+") == 0 &&
            ir_operand_is_temp_named(&addr->rhs, ins->dest.name) &&
            addr->lhs.kind == IR_OPERAND_SYMBOL && addr->lhs.name && !a_symbol) {
          a_symbol = addr->lhs.name;
        }
      }
      for (size_t j = i + 1; j < jump_index; j++) {
        const IRInstruction *load = &function->instructions[j];
        if (load->op != IR_OP_LOAD || load->rhs.kind != IR_OPERAND_INT ||
            load->rhs.int_value != 4 || load->dest.kind != IR_OPERAND_TEMP) {
          continue;
        }
        const IRInstruction *mul =
            ir_find_temp_producer_before(function, j, load->dest.name);
        if (!mul || mul->op != IR_OP_BINARY || !mul->text ||
            strcmp(mul->text, "*") != 0) {
          continue;
        }
        for (size_t k = j + 1; k < jump_index; k++) {
          const IRInstruction *load2 = &function->instructions[k];
          if (load2->op != IR_OP_LOAD || load2->rhs.kind != IR_OPERAND_INT ||
              load2->rhs.int_value != 4) {
            continue;
          }
          const IRInstruction *addr2 = ir_find_temp_producer_before(
              function, k, load2->lhs.name);
          if (addr2 && addr2->op == IR_OP_BINARY && addr2->text &&
              strcmp(addr2->text, "+") == 0 &&
              ir_operand_is_temp_named(&addr2->rhs, ins->dest.name) &&
              addr2->lhs.kind == IR_OPERAND_SYMBOL && addr2->lhs.name && !b_symbol) {
            b_symbol = addr2->lhs.name;
          }
        }
      }
    }
  }

  if (!has_mul_add || !sum_symbol || !a_symbol || !b_symbol) {
    ir_operand_destroy(&len);
    return 1;
  }

  sum_type = ir_function_local_declared_type(function, sum_symbol);
  if (!sum_type || strcmp(sum_type, "int64") != 0) {
    ir_operand_destroy(&len);
    return 1;
  }
  if (!ir_symbol_is_sum_array_base(function, a_symbol) ||
      !ir_symbol_is_sum_array_base(function, b_symbol)) {
    ir_operand_destroy(&len);
    return 1;
  }

  fused.op = IR_OP_SIMD_DOT_I32;
  fused.location = header->location;
  fused.dest = ir_operand_symbol(sum_symbol);
  fused.lhs = ir_operand_symbol(a_symbol);
  fused.rhs = ir_operand_symbol(b_symbol);
  fused.arguments = calloc(1, sizeof(IROperand));
  if (!fused.arguments) {
    ir_operand_destroy(&len);
    ir_instruction_destroy_storage(&fused);
    return 0;
  }
  fused.argument_count = 1;
  fused.arguments[0] = len;

  ir_instruction_destroy_storage(header);
  *header = fused;
  for (size_t i = header_index + 1; i <= jump_index; i++) {
    ir_instruction_make_nop(&function->instructions[i]);
  }
  if (changed) {
    *changed = 1;
  }
  return 1;
}

static int ir_simd_dot_i32_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (function->instructions[i].op == IR_OP_LABEL &&
        ir_label_is_while_header(function->instructions[i].text)) {
      if (!ir_try_vectorize_dot_i32_at(function, i, changed)) {
        return 0;
      }
    }
  }
  return 1;
}
'''

PASS_NAMES = '''static const char *g_ir_pass_names[IR_OPT_PASS_COUNT] = {
    "reduction_unroll", "copy_and_constant_propagation", "fuse_rotate_add",
    "strength_reduce_rotate_loops", "unroll_small_const_bound_loops",
    "positive_loop_div2_to_shift", "fold_popcount_byte_loop",
    "fuse_popcount_buffer_loop", "collatz_odd_step_fold",
    "coalesce_single_use_temp_assign", "common_subexpression_elimination",
    "constant_and_branch_simplify", "count_word_starts",
    "eliminate_dead_temp_writes", "thread_jump_targets", "null_check_licm",
    "remove_empty_conditional_diamonds", "remove_redundant_fallthrough_branches",
    "remove_redundant_jumps", "eliminate_unreachable_straightline",
    "eliminate_unreachable_blocks", "remove_unused_labels", "memcpy_inline",
    "eliminate_load_symbol_copy", "simd_sum_i32", "simd_dot_i32",
    "simd_insertion_sort_i32",
};
'''

PROGRAM = r'''int ir_optimize_program(IRProgram *program,
                        const IROptimizeOptions *options) {
  if (!program) {
    return 0;
  }

  ir_function_index_reset();

  for (size_t pre = 0; pre < program->function_count; pre++) {
    IRFunction *function = program->functions[pre];
    int pre_changed = 0;
    if (function) {
      mettle_compiler_ctx_set_function_name(
          function->name ? function->name : "<anonymous>");
    }
    mettle_compiler_ctx_set_pass_name("simd_dot_i32");
    if (!ir_simd_dot_i32_pass(function, &pre_changed)) {
      mettle_compiler_ice("IR optimization pre-inline pass failed");
    }
    pre_changed = 0;
    mettle_compiler_ctx_set_pass_name("simd_insertion_sort_i32");
    if (!ir_simd_insertion_sort_i32_pass(function, &pre_changed)) {
      mettle_compiler_ice("IR optimization pre-inline pass failed");
    }
    pre_changed = 0;
    mettle_compiler_ctx_set_pass_name("simd_minmax_i32");
    if (!ir_simd_minmax_i32_pass(function, &pre_changed)) {
      mettle_compiler_ice("IR optimization pre-inline pass failed");
    }
    pre_changed = 0;
    mettle_compiler_ctx_set_pass_name("prefix_sum_i32");
    if (!ir_prefix_sum_i32_pass(function, &pre_changed)) {
      mettle_compiler_ice("IR optimization pre-inline pass failed");
    }
  }

  if (!options || !options->preserve_function_boundaries) {
    int inlining_changed = 0;
    mettle_compiler_ctx_set_pass_name("inline_small_functions");
    if (!ir_inline_small_functions_pass(program, &inlining_changed)) {
      mettle_compiler_ice("IR optimization inlining pass failed");
    }
  }

  for (size_t i = 0; i < program->function_count; i++) {
    IRFunction *function = program->functions[i];
    if (function) {
      mettle_compiler_ctx_set_function_name(
          function->name ? function->name : "<anonymous>");
    }
    if (!ir_optimize_function(function)) {
      mettle_compiler_ice_report("IR optimization failed", NULL);
      ir_function_index_reset();
      return 0;
    }
  }

  ir_function_index_reset();
  return 1;
}
'''

POST_PASSES = '''  {
    int ptr_changed = 0;
    mettle_compiler_ctx_set_pass_name("induction_pointer");
    if (!ir_pointer_induction_pass(function, &ptr_changed)) {
      mettle_compiler_ice("IR optimization pass failed");
    }
  }

  {
    int map_changed = 0;
    mettle_compiler_ctx_set_pass_name("simd_memory_map");
    if (!ir_simd_memory_map_pass(function, &map_changed)) {
      mettle_compiler_ice("IR optimization pass failed");
    }
  }

  {
    int detect_changed = 0;
    mettle_compiler_ctx_set_pass_name("detect_shift_loops");
    if (!ir_detect_shift_loops_pass(function, &detect_changed)) {
      mettle_compiler_ice("IR optimization pass failed");
    }
  }'''


def slice_file(path: Path, start_marker: str, end_marker: str | None = None) -> str:
    text = path.read_text(encoding="utf-8")
    start = text.index(start_marker)
    if end_marker:
        end = text.index(end_marker, start)
        return text[start:end].rstrip() + "\n"
    return text[start:].rstrip() + "\n"


def trim_patch(text: str, start_marker: str, end_marker: str | None = None) -> str:
    start = text.index(start_marker)
    if end_marker:
        end = text.index(end_marker, start)
        return text[start:end].rstrip() + "\n"
    return text[start:].rstrip() + "\n"


def main() -> None:
    text = BASE.read_text(encoding="utf-8")

    if '#include "ir_profile.h"' not in text:
        text = text.replace(
            '#include "ir_optimize.h"\n',
            '#include "ir_optimize.h"\n#include "ir_profile.h"\n'
            '#include "compiler/compiler_context.h"\n'
            '#include "compiler/compiler_crash.h"\n',
        )

    insert_move = slice_file(
        INSERT_MOVE,
        "int ir_instruction_insert_move(IRFunction *function, size_t index,",
        "\n\nvoid ir_instruction_vector_destroy",
    ).replace("int ir_instruction_insert_move", "static int ir_instruction_insert_move")

    ptr_induction = slice_file(
        RECON,
        "static int ir_operand_uses_symbol(const IROperand *operand,",
        "static int ir_elide_redundant_integral_cast_pass",
    )

    minmax = trim_patch(
        PATCH_MINMAX.read_text(encoding="utf-8"),
        "static int ir_make_simd_minmax_i32",
        "\n\nstatic int ir_simd_memory_map_pass",
    )

    memmap = trim_patch(
        PATCH_MEMMAP.read_text(encoding="utf-8"),
        "typedef struct {",
        "\n\nstatic int ir_detect_shift_loops_pass",
    )

    block = (
        "\n/* ---- recovered optimizer passes ---- */\n"
        + insert_move
        + "\n"
        + HELPERS.read_text(encoding="utf-8")
        + "\n"
        + ptr_induction
        + "\n"
        + IDIOMS.read_text(encoding="utf-8")
        + "\n"
        + memmap
        + "\n"
        + minmax
        + "\n"
        + DOT_PASS
        + "\n"
    )

    marker = "#define IR_OPT_PASS_COUNT"
    if "ir_fold_popcount_byte_loop_pass" not in text:
        idx = text.index(marker)
        text = text[:idx] + block + text[idx:]

    text = text.replace("#define IR_OPT_PASS_COUNT 24", "#define IR_OPT_PASS_COUNT 28")
    if "g_ir_pass_names" not in text:
        text = text.replace(
            "#define IR_OPT_PASS_COUNT 28\n",
            "#define IR_OPT_PASS_COUNT 28\n\n" + PASS_NAMES + "\n",
        )

    text = text.replace(
        "  if (!ir_make_simd_with_len_and_two_ints(\n"
        "          &fused, function->instructions[header_index].location,\n"
        "          IR_OP_SIMD_SCALE_I32, ir_operand_symbol(sum_symbol), src_base,\n"
        "          dst_base, &len, mul_val, add_val)) {\n"
        "    return 0;\n"
        "  }",
        "  {\n"
        "    IROperand dest = ir_operand_symbol(sum_symbol);\n"
        "    if (!ir_make_simd_with_len_and_two_ints(\n"
        "            &fused, function->instructions[header_index].location,\n"
        "            IR_OP_SIMD_SCALE_I32, &dest, src_base,\n"
        "            dst_base, &len, mul_val, add_val)) {\n"
        "      return 0;\n"
        "    }\n"
        "  }",
    )
    text = text.replace(
        "  if (!ir_make_simd_with_len(\n"
        "          &fused, function->instructions[header_index].location,\n"
        "          IR_OP_SIMD_REVERSE_COPY_I32, ir_operand_symbol(sum_symbol), src_base,\n"
        "          dst_base, &len)) {\n"
        "    return 0;\n"
        "  }",
        "  {\n"
        "    IROperand dest = ir_operand_symbol(sum_symbol);\n"
        "    if (!ir_make_simd_with_len(\n"
        "            &fused, function->instructions[header_index].location,\n"
        "            IR_OP_SIMD_REVERSE_COPY_I32, &dest, src_base,\n"
        "            dst_base, &len)) {\n"
        "      return 0;\n"
        "    }\n"
        "  }",
    )
    text = text.replace(
        "  if (!ir_make_simd_with_len_and_two_ints(\n"
        "          &fused, function->instructions[header_index].location,\n"
        "          IR_OP_SIMD_CLAMP_I32, ir_operand_symbol(sum_symbol), src_base,\n"
        "          dst_base, &compare->rhs, (int)lo, (int)hi)) {\n"
        "    return 0;\n"
        "  }",
        "  {\n"
        "    IROperand dest = ir_operand_symbol(sum_symbol);\n"
        "    if (!ir_make_simd_with_len_and_two_ints(\n"
        "            &fused, function->instructions[header_index].location,\n"
        "            IR_OP_SIMD_CLAMP_I32, &dest, src_base,\n"
        "            dst_base, &compare->rhs, (int)lo, (int)hi)) {\n"
        "      return 0;\n"
        "    }\n"
        "  }",
    )

    driver = PATCH_DRIVER.read_text(encoding="utf-8")
    dstart = text.index("    DRIVE_PASS_IF(0, features.has_label && features.has_jump,")
    dend = text.index("    if (!changed) {", dstart)
    text = text[:dstart] + driver + "\n\n" + text[dend:]

    old_post = (
        "  {\n"
        "    int detect_changed = 0;\n"
        "    if (!ir_detect_shift_loops_pass(function, &detect_changed)) {\n"
        "      return 0;\n"
        "    }\n"
        "  }"
    )
    if old_post not in text:
        raise RuntimeError("post-fixpoint detect_shift block not found")
    text = text.replace(old_post, POST_PASSES, 1)

    prog_old = "int ir_optimize_program(IRProgram *program) {"
    if prog_old not in text:
        raise RuntimeError("ir_optimize_program not found")
    prog_start = text.index(prog_old)
    text = text[:prog_start] + PROGRAM

    OUT.write_text(text, encoding="utf-8", newline="\n")
    print(f"Wrote {OUT} ({text.count(chr(10)) + 1} lines)")


if __name__ == "__main__":
    main()
