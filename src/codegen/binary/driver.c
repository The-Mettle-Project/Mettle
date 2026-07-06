#include "codegen/binary/internal.h"
#include "codegen/binary/mir.h"
#include "codegen/binary/mir_annotate.h"
#include "ir/ir_pgo.h"
#include <limits.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int code_generator_emit_binary_function(CodeGenerator *generator,
                                               FunctionDeclaration *function_data,
                                               IRFunction *ir_function,
                                               ASTNode *function_declaration) {
  BinaryEmitter *emitter = NULL;
  BinaryFunctionContext context = {0};
  size_t text_section = 0;
  BinarySection *section = NULL;
  size_t function_offset = 0;
  size_t return_offset = 0;

  if (!generator || !function_data || !ir_function) {
    return 0;
  }

  if (!code_generator_binary_validate_signature(generator, function_data,
                                                ir_function)) {
    return 0;
  }

  if (!code_generator_binary_prepare_function_context(generator, function_data,
                                                      ir_function, &context)) {
    return 0;
  }

  free(generator->current_function_name);
  if (function_data->name) {
    generator->current_function_name = strdup(function_data->name);
    if (!generator->current_function_name) {
      code_generator_set_error(generator,
                               "Out of memory while tracking function name");
      binary_function_context_destroy(&context);
      return 0;
    }
  } else {
    generator->current_function_name = NULL;
  }
  generator->last_runtime_location_line = 0;
  generator->last_runtime_location_column = 0;

  if (generator->debug_info && generator->generate_stack_trace_support) {
    context.runtime_end_label =
        code_generator_generate_label(generator, "mettledbg_func_end");
    if (!context.runtime_end_label) {
      code_generator_set_error(generator,
                               "Out of memory while tracking function debug "
                               "range in '%s'",
                               function_data->name);
      binary_function_context_destroy(&context);
      return 0;
    }
    code_generator_add_runtime_function_mapping(
        generator, function_data->name, function_data->name,
        context.runtime_end_label,
        function_declaration ? function_declaration->location.line : 0,
        function_declaration ? function_declaration->location.column : 0,
        code_generator_runtime_filename(
            generator, function_declaration
                                 ? function_declaration->location.filename
                                 : NULL));
  }

  /* Route fully-supported leaf integer functions through the MIR + linear-scan
   * register allocator The MIR path fills context.code
   * with a complete prologue..epilogue and resolves its own label fixups; all
   * downstream emission (.text append, relocations, debug symbols) is shared. */
  if (mir_function_is_eligible(generator, function_data, ir_function)) {
    if (!code_generator_binary_emit_function_via_mir(generator, function_data,
                                                     ir_function, &context)) {
      binary_function_context_destroy(&context);
      return 0;
    }
    return_offset = context.code.size;
    goto mir_shared_append;
  }

  /* --annotate-asm: this function uses the BASELINE (fallback) backend, which
   * works at IR granularity. Open a capture context and record each IR
   * instruction's emitted byte span. The optimized idioms the MIR gate rejects
   * (vectorized kernels, the Fibonacci rotate) land here. */
  int annot = mir_annotate_enabled();
  size_t annot_base = context.code.size;
  if (annot) {
    mir_annotate_begin_function(
        function_data->name, ir_function,
        function_data->body ? function_data->body->location.filename : NULL,
        function_data->body ? function_data->body->location.line : 0);
    mir_annotate_note_backend("baseline (fallback)", NULL);
  }

  if (!code_generator_binary_emit_prologue(generator, &context, function_data)) {
    if (annot) mir_annotate_end_function();
    binary_function_context_destroy(&context);
    return 0;
  }
  if (annot && context.code.size > annot_base) {
    mir_annotate_record_synthetic("prologue", "frame", 0,
                                  context.code.size - annot_base,
                                  context.code.data + annot_base);
  }

  size_t annot_prev_off = context.code.size;
  int annot_prev_idx = -1;
  for (size_t i = 0; i < ir_function->instruction_count;) {
    /* Lazily record the previous instruction's span now that it is fully
     * emitted; this is robust to the cascade's many `continue` paths because
     * every one returns here. */
    if (annot && annot_prev_idx >= 0 && context.code.size > annot_prev_off) {
      mir_annotate_record_ir(ir_function, annot_prev_idx,
                             annot_prev_off - annot_base,
                             context.code.size - annot_prev_off,
                             context.code.data + annot_prev_off);
    }
    annot_prev_off = context.code.size;
    annot_prev_idx = (int)i;
    /* Labels emit no bytes, so the lazy span recorder never sees them; record a
     * zero-byte marker so loop recovery can resolve backward-branch targets. */
    if (annot && ir_function->instructions[i].op == IR_OP_LABEL) {
      mir_annotate_record_ir_label(ir_function->instructions[i].text,
                                   context.code.size - annot_base);
    }
    size_t consumed = 0;
    if (code_generator_binary_try_skip_scaled_address_shift(ir_function, i,
                                                            &consumed)) {
      i += consumed;
      continue;
    }

    if (code_generator_binary_try_emit_binary_compare_branch_chain(
            generator, &context, ir_function, i, &consumed)) {
      i += consumed;
      continue;
    }

    if (code_generator_binary_try_emit_compare_assign_diamond(
            generator, &context, ir_function, i, &consumed)) {
      i += consumed;
      continue;
    }

    if (code_generator_binary_try_emit_offset_scaled_address_load(
            generator, &context, ir_function, i, &consumed)) {
      i += consumed;
      continue;
    }

    if (code_generator_binary_try_emit_offset_scaled_address_store(
            generator, &context, ir_function, i, &consumed)) {
      i += consumed;
      continue;
    }

    if (code_generator_binary_try_emit_address_add_load(
            generator, &context, ir_function, i, &consumed)) {
      i += consumed;
      continue;
    }

    if (code_generator_binary_try_emit_address_add_store(
            generator, &context, ir_function, i, &consumed)) {
      i += consumed;
      continue;
    }

    if (code_generator_binary_try_emit_scaled_address_load(
            generator, &context, ir_function, i, &consumed)) {
      i += consumed;
      continue;
    }

    if (code_generator_binary_try_emit_scaled_address_store(
            generator, &context, ir_function, i, &consumed)) {
      i += consumed;
      continue;
    }

    if (code_generator_binary_try_emit_binary_cast_chain(
            generator, &context, ir_function, i, &consumed)) {
      i += consumed;
      continue;
    }

    if (code_generator_binary_try_emit_float_cast_binary_chain(
            generator, &context, ir_function, i, &consumed)) {
      i += consumed;
      continue;
    }

    if (code_generator_binary_try_emit_float_binary_expression_chain(
            generator, &context, ir_function, i, &consumed)) {
      i += consumed;
      continue;
    }

    if (code_generator_binary_try_emit_binary_expression_chain(
            generator, &context, ir_function, i, &consumed)) {
      i += consumed;
      continue;
    }

    if (code_generator_binary_try_emit_compare_branch_zero(
            generator, &context, ir_function, i, &consumed)) {
      i += consumed;
      continue;
    }

    {
      const IRInstruction *instruction = &ir_function->instructions[i];
      if (generator->debug_info && generator->generate_stack_trace_support &&
          instruction->location.line > 0) {
        if (!code_generator_binary_emit_runtime_location_marker(
                generator, &context, instruction->location.line,
                instruction->location.column,
                code_generator_runtime_filename(
                    generator, instruction->location.filename))) {
          binary_function_context_destroy(&context);
          return 0;
        }
      }
    }

    if (!code_generator_binary_emit_instruction(
            generator, &context, &ir_function->instructions[i])) {
      if (annot) mir_annotate_end_function();
      binary_function_context_destroy(&context);
      return 0;
    }
    i++;
  }
  /* Record the last instruction's span (no further loop top runs). */
  if (annot && annot_prev_idx >= 0 && context.code.size > annot_prev_off) {
    mir_annotate_record_ir(ir_function, annot_prev_idx,
                           annot_prev_off - annot_base,
                           context.code.size - annot_prev_off,
                           context.code.data + annot_prev_off);
  }
  if (annot) {
    mir_annotate_end_function();
  }

  return_offset = context.code.size;
  if (context.runtime_end_label &&
      !binary_label_table_define(&context.labels, context.runtime_end_label,
                                 return_offset)) {
    code_generator_set_error(
        generator,
        "Failed to define runtime function end label in function '%s'",
        context.function_name);
    binary_function_context_destroy(&context);
    return 0;
  }

  if (!code_generator_binary_emit_promoted_global_stores(generator, &context)) {
    binary_function_context_destroy(&context);
    return 0;
  }

  for (size_t i = context.saved_register_count; i > 0; i--) {
    size_t slot = i - 1;
    if (!binary_emit_mov_reg_mem(&context.code, context.saved_registers[slot],
                                 BINARY_GP_RBP,
                                 -context.saved_register_offsets[slot])) {
      code_generator_set_error(generator,
                               "Out of memory while restoring callee registers");
      binary_function_context_destroy(&context);
      return 0;
    }
  }

  if ((context.return_float_bits == 32 &&
       !binary_emit_movd_xmm_reg(&context.code, BINARY_XMM0,
                                 BINARY_GP_RAX)) ||
      (context.return_float_bits == 64 &&
       !binary_emit_movq_xmm_reg(&context.code, BINARY_XMM0,
                                 BINARY_GP_RAX)) ||
      !binary_emit_mov_reg_reg(&context.code, BINARY_GP_RSP, BINARY_GP_RBP) ||
      !binary_emit_pop_reg(&context.code, BINARY_GP_RBP) ||
      !binary_emit_ret(&context.code)) {
    code_generator_set_error(generator,
                             "Out of memory while emitting function epilogue");
    binary_function_context_destroy(&context);
    return 0;
  }

  if (!code_generator_binary_resolve_fixups(generator, &context, return_offset)) {
    binary_function_context_destroy(&context);
    return 0;
  }

mir_shared_append:
  emitter = code_generator_get_binary_emitter(generator);
  if (!emitter) {
    code_generator_set_error(generator, "Binary emitter is not initialized");
    binary_function_context_destroy(&context);
    return 0;
  }

  text_section = binary_emitter_get_or_create_section(
      emitter, ".text", BINARY_SECTION_TEXT, 0, BINARY_TEXT_SECTION_ALIGNMENT);
  if (text_section == (size_t)-1) {
    code_generator_set_error(generator, "%s",
                             binary_emitter_get_error(emitter)
                                 ? binary_emitter_get_error(emitter)
                                 : "Failed to create .text section");
    binary_function_context_destroy(&context);
    return 0;
  }

  if (!binary_emitter_align_section(emitter, text_section,
                                    BINARY_TEXT_SECTION_ALIGNMENT, 0x90)) {
    code_generator_set_error(generator, "%s",
                             binary_emitter_get_error(emitter)
                                 ? binary_emitter_get_error(emitter)
                                 : "Failed to align .text section");
    binary_function_context_destroy(&context);
    return 0;
  }

  section = binary_emitter_get_section(emitter, text_section);
  if (!section) {
    code_generator_set_error(generator, "Failed to access .text section");
    binary_function_context_destroy(&context);
    return 0;
  }

  function_offset = section->size;
  if (!binary_emitter_define_symbol(emitter, function_data->name,
                                    BINARY_SYMBOL_GLOBAL, text_section,
                                    function_offset, context.code.size) ||
      !binary_emitter_append_bytes(emitter, text_section, context.code.data,
                                   context.code.size, NULL)) {
    code_generator_set_error(generator, "%s",
                             binary_emitter_get_error(emitter)
                                 ? binary_emitter_get_error(emitter)
                                 : "Failed to emit function machine code");
    binary_function_context_destroy(&context);
    return 0;
  }

  for (size_t i = 0; i < context.call_relocations.count; i++) {
    BinaryCallRelocation *relocation = &context.call_relocations.items[i];
    if (!binary_emitter_add_relocation(
            emitter, text_section,
            function_offset + relocation->displacement_offset,
            BINARY_RELOCATION_REL32, relocation->symbol_name, 0)) {
      code_generator_set_error(generator, "%s",
                               binary_emitter_get_error(emitter)
                                   ? binary_emitter_get_error(emitter)
                                   : "Failed to record function relocation");
      binary_function_context_destroy(&context);
      return 0;
    }
  }

  if (!code_generator_binary_export_debug_symbols(generator, &context,
                                                  text_section, function_offset,
                                                  return_offset)) {
    binary_function_context_destroy(&context);
    return 0;
  }

  binary_function_context_destroy(&context);
  return 1;
}
int code_generator_generate_program_binary_object(CodeGenerator *generator,
                                                  ASTNode *program) {
  Program *program_data = NULL;

  if (!generator || !program) {
    return 0;
  }
  if (program->type != AST_PROGRAM) {
    code_generator_set_error(generator, "Expected AST_PROGRAM root node");
    return 0;
  }
  if (!generator->ir_program) {
    code_generator_set_error(generator,
                             "IR program not attached to code generator");
    return 0;
  }
  /* Pin the calling convention to the target object format before emitting any
   * code: COFF -> MS-x64, ELF -> SysV. */
  code_generator_binary_select_abi(generator->binary_emitter->target_format);

  binary_emitter_reset(generator->binary_emitter);
  program_data = (Program *)program->data;
  if (!program_data) {
    code_generator_set_error(generator, "Program node is missing data");
    return 0;
  }

  binary_global_const_table_reset();
  binary_ir_function_index_reset();
  if (!code_generator_binary_collect_global_constants(generator, program_data)) {
    return 0;
  }

  if (!code_generator_declare_binary_externs(generator, program_data)) {
    return 0;
  }

  /* --pgo code layout: emit measured-hot functions first (main leading) so
   * the hot working set shares I-cache lines and iTLB pages, cold glue sinks
   * to the tail. Zero-run: the frequencies come from the compile-time
   * interpretation of main(), no training run. Only the FUNCTION declaration
   * slots are permuted among themselves; every other declaration keeps its
   * position. Without a profile the order is untouched. */
  size_t *emit_order = NULL;
  if (ir_pgo_enabled() && program_data->declaration_count > 1) {
    size_t n = program_data->declaration_count;
    emit_order = (size_t *)malloc(n * sizeof(size_t));
    size_t *fn_slots = (size_t *)malloc(n * sizeof(size_t));
    long long *heat = (long long *)malloc(n * sizeof(long long));
    if (emit_order && fn_slots && heat) {
      size_t fn_count = 0;
      for (size_t i = 0; i < n; i++) {
        emit_order[i] = i;
        ASTNode *d = program_data->declarations[i];
        if (d && d->type == AST_FUNCTION_DECLARATION && d->data) {
          FunctionDeclaration *fd = (FunctionDeclaration *)d->data;
          if (fd->name && !fd->is_extern && fd->body) {
            fn_slots[fn_count] = i;
            heat[fn_count] =
                (strcmp(fd->name, "main") == 0)
                    ? LLONG_MAX
                    : ir_pgo_callee_calls(fd->name);
            fn_count++;
          }
        }
      }
      /* Stable insertion sort of the function indices by heat, descending:
       * ties (and cold/-1) keep declaration order. */
      for (size_t i = 1; i < fn_count; i++) {
        size_t slot = fn_slots[i];
        long long h = heat[i];
        size_t j = i;
        while (j > 0 && heat[j - 1] < h) {
          fn_slots[j] = fn_slots[j - 1];
          heat[j] = heat[j - 1];
          j--;
        }
        fn_slots[j] = slot;
        heat[j] = h;
      }
      /* Redistribute the sorted functions into the function slots. */
      size_t next = 0;
      for (size_t i = 0; i < n; i++) {
        ASTNode *d = program_data->declarations[i];
        if (d && d->type == AST_FUNCTION_DECLARATION && d->data) {
          FunctionDeclaration *fd = (FunctionDeclaration *)d->data;
          if (fd->name && !fd->is_extern && fd->body) {
            emit_order[i] = fn_slots[next++];
          }
        }
      }
    } else {
      free(emit_order);
      emit_order = NULL;
    }
    free(fn_slots);
    free(heat);
  }

  for (size_t i = 0; i < program_data->declaration_count; i++) {
    ASTNode *declaration =
        program_data->declarations[emit_order ? emit_order[i] : i];
    if (!declaration) {
      continue;
    }

    switch (declaration->type) {
    case AST_FUNCTION_DECLARATION: {
      FunctionDeclaration *function_data =
          (FunctionDeclaration *)declaration->data;
      IRFunction *ir_function = NULL;

      if (!function_data || !function_data->name) {
        code_generator_set_error(generator,
                                 "Malformed function declaration in AST");
        return 0;
      }
      if (function_data->is_extern || !function_data->body) {
        continue;
      }

      ir_function = code_generator_find_ir_function_binary(generator,
                                                           function_data->name);
      if (!ir_function) {
        /* After dead-function elimination a missing IR body means the
         * function was unreachable from main — skip it. Without that pass a
         * missing body is a real lowering bug and stays a hard error. */
        if (generator->ir_program->dead_functions_eliminated) {
          continue;
        }
        code_generator_set_error(generator,
                                 "No IR body found for function '%s'",
                                 function_data->name);
        return 0;
      }

      if (!code_generator_emit_binary_function(generator, function_data,
                                               ir_function, declaration)) {
        return 0;
      }
    } break;
    case AST_STRUCT_DECLARATION:
    case AST_ENUM_DECLARATION:
      break;
    case AST_FUNCTION_CALL: {
      CallExpression *call = (CallExpression *)declaration->data;
      if (call && call->function_name &&
          strcmp(call->function_name, "static_assert") == 0) {
        break;
      }
      code_generator_set_error(
          generator,
          "Direct object backend encountered unsupported top-level call '%s'",
          (call && call->function_name) ? call->function_name : "<unknown>");
      return 0;
    }
    case AST_VAR_DECLARATION: {
      VarDeclaration *var_data = (VarDeclaration *)declaration->data;
      if (!var_data || !var_data->name) {
        code_generator_set_error(generator,
                                 "Malformed global variable declaration in AST");
        return 0;
      }
      if (var_data->is_extern) {
        break;
      }
      // An integer `const` folds to a SYMBOL_CONSTANT at every use site and
      // carries no storage. A non-integer `const` (float/string/aggregate) is
      // registered as an immutable variable instead and DOES need storage,
      // since the IR references it via a RIP-relative load like any global.
      if (var_data->is_const) {
        Symbol *const_symbol =
            generator->symbol_table
                ? symbol_table_lookup(generator->symbol_table, var_data->name)
                : NULL;
        if (!const_symbol || const_symbol->kind == SYMBOL_CONSTANT) {
          break;
        }
      }
      if (!code_generator_emit_binary_global_variable(generator, var_data)) {
        return 0;
      }
    }
      break;
    case AST_INLINE_ASM:
      code_generator_set_error(
          generator,
          "Direct object backend does not yet support inline assembly");
      return 0;
    default:
      code_generator_set_error(
          generator,
          "Direct object backend encountered unsupported declaration type %d",
          declaration->type);
      free(emit_order);
      return 0;
    }
  }
  free(emit_order);

  if ((generator->profile_runtime || generator->debug_hooks) &&
      !code_generator_binary_emit_profile_tables(generator)) {
    return 0;
  }

  if (generator->generate_stack_trace_support &&
      !code_generator_binary_emit_runtime_debug_tables(generator)) {
    return 0;
  }

  if (generator->generate_stack_trace_support &&
      !code_generator_binary_emit_crash_startup(generator)) {
    return 0;
  }

  if ((generator->generate_stack_trace_support || generator->profile_runtime) &&
      !code_generator_binary_emit_elf_runtime_hooks(generator)) {
    return 0;
  }

  if (generator->generate_debug_info &&
      !code_generator_binary_emit_dwarf_debug_sections(generator)) {
    return 0;
  }

  int ok = generator->has_error ? 0 : 1;
  binary_global_const_table_reset();
  binary_ir_function_index_reset();
  return ok;
}
