#include "ir_optimize_internal.h"

/* ---- Scalar Replacement of Aggregates (SROA) --------------------------------
 *
 * An aggregate local whose address is taken only to load/store its own fields
 * at constant offsets never escapes, yet the address-of marks it non-promotable
 * so every field access is a memory round-trip. SROA rewrites such a local into
 * one scalar local per accessed (offset,size) slot. Those scalars are not
 * address-taken, so the register allocator can promote them directly, turning
 * the struct into registers (the struct_byval 42x case).
 *
 * The IR optimizer has no type information, so eligibility and field shapes are
 * derived purely from the IR: ADDRESS_OF gives the base, `base + CONST` gives
 * offsets, and the LOAD/STORE size operand gives each slot's width. The local
 * is eligible only when EVERY use fits the disciplined field-access pattern; any
 * other use of the symbol or an address temp (escape, whole-struct copy, call
 * argument, dynamic offset) disqualifies it and it is left untouched. */

static const char *ir_builtin_scalar_type_for_slot(int size, int is_float,
                                                    int float_bits) {
  if (is_float) {
    return float_bits == 32 ? "float32" : "float64";
  }
  switch (size) {
  case 1:
    return "int8";
  case 2:
    return "int16";
  case 4:
    return "int32";
  default:
    return "int64";
  }
}

/* Find the addr-record for a temp name, or NULL. */
static IRSroaAddr *ir_sroa_find_addr(IRSroaAddr *addrs, size_t count,
                                     const char *name) {
  if (!name) {
    return NULL;
  }
  for (size_t i = 0; i < count; i++) {
    if (addrs[i].temp && strcmp(addrs[i].temp, name) == 0) {
      return &addrs[i];
    }
  }
  return NULL;
}

/* Count how many of an instruction's operands (dest/lhs/rhs/args) name `temp`. */
static int ir_sroa_temp_operand_uses(const IRInstruction *insn,
                                     const char *temp) {
  int n = 0;
  if (ir_operand_is_temp_named(&insn->dest, temp)) {
    n++;
  }
  if (ir_operand_is_temp_named(&insn->lhs, temp)) {
    n++;
  }
  if (ir_operand_is_temp_named(&insn->rhs, temp)) {
    n++;
  }
  for (size_t a = 0; a < insn->argument_count; a++) {
    if (ir_operand_is_temp_named(&insn->arguments[a], temp)) {
      n++;
    }
  }
  return n;
}

static IRSroaSlot *ir_sroa_find_slot(IRSroaSlot *slots, size_t slot_count,
                                     long long offset) {
  for (size_t i = 0; i < slot_count; i++) {
    if (slots[i].offset == offset) {
      return &slots[i];
    }
  }
  return NULL;
}

static const IRSroaSlot *ir_sroa_find_const_slot(const IRSroaSlot *slots,
                                                 size_t slot_count,
                                                 long long offset) {
  for (size_t i = 0; i < slot_count; i++) {
    if (slots[i].offset == offset) {
      return &slots[i];
    }
  }
  return NULL;
}

static int ir_sroa_note_slot(IRSroaSlot *slots, size_t *slot_count,
                             long long offset, int size, int is_float,
                             int float_bits) {
  IRSroaSlot *slot = ir_sroa_find_slot(slots, *slot_count, offset);
  if (!slot) {
    if (*slot_count >= IR_SROA_MAX_SLOTS) {
      return 0;
    }
    slot = &slots[(*slot_count)++];
    slot->offset = offset;
    slot->size = size;
    slot->is_float = is_float;
    slot->float_bits = float_bits;
    slot->name = NULL;
    return 1;
  }

  return slot->size == size && slot->is_float == is_float &&
         slot->float_bits == float_bits;
}

/* Analyze a single aggregate local `sym` declared at decl_index: discover its
 * field slots (offset/size/float) and the set of whole-aggregate copy partners
 * (`@sym <- @other` or `@other <- @sym`). Fills slots/slot_count and
 * partners/partner_count. Returns 1 if `sym` is SROA-eligible in isolation
 * (clean field access + only whole-aggregate-copy non-field uses), else 0.
 * "Eligible in isolation" does NOT yet require partners to be eligible here;
 * the group driver checks that. */
static int ir_sroa_analyze_local(IRFunction *function, size_t decl_index,
                                 const char *sym, IRSroaSlot *slots,
                                 size_t *slot_count_out,
                                 const char **partners,
                                 size_t *partner_count_out,
                                 size_t max_partners) {
  IRSroaAddr addrs[IR_SROA_MAX_SLOTS * 2];
  size_t addr_count = 0;
  size_t slot_count = 0;
  size_t partner_count = 0;
  int eligible = 1;

  *slot_count_out = 0;
  *partner_count_out = 0;

  /* discover address temps (&sym and &sym + CONST). A whole-aggregate
   * copy ASSIGN that references sym is recorded as a partner (not a
   * disqualifier). Any OTHER bare-symbol use means the value escapes. */
  for (size_t i = 0; i < function->instruction_count && eligible; i++) {
    const IRInstruction *insn = &function->instructions[i];
    if (insn->op == IR_OP_ADDRESS_OF &&
        ir_operand_is_symbol_named(&insn->lhs, sym) &&
        insn->dest.kind == IR_OPERAND_TEMP && insn->dest.name) {
      if (addr_count >= sizeof(addrs) / sizeof(addrs[0])) {
        eligible = 0;
        break;
      }
      addrs[addr_count].temp = insn->dest.name;
      addrs[addr_count].offset = 0;
      addrs[addr_count].valid = 1;
      addr_count++;
      continue;
    }
    if (i == decl_index) {
      continue;
    }
    /* Whole-aggregate copy: ASSIGN where one side is sym and the other is a
     * plain symbol (the partner). Both operands must be bare symbols; a
     * scalar-typed assign would not have sym as an aggregate value. */
    if (insn->op == IR_OP_ASSIGN &&
        insn->dest.kind == IR_OPERAND_SYMBOL && insn->dest.name &&
        insn->lhs.kind == IR_OPERAND_SYMBOL && insn->lhs.name &&
        (ir_operand_is_symbol_named(&insn->dest, sym) ||
         ir_operand_is_symbol_named(&insn->lhs, sym))) {
      const char *other = ir_operand_is_symbol_named(&insn->dest, sym)
                              ? insn->lhs.name
                              : insn->dest.name;
      /* Self-copy (@sym <- @sym) is degenerate; treat as escape to be safe. */
      if (strcmp(other, sym) == 0) {
        eligible = 0;
        break;
      }
      int found = 0;
      for (size_t p = 0; p < partner_count; p++) {
        if (strcmp(partners[p], other) == 0) {
          found = 1;
          break;
        }
      }
      if (!found) {
        if (partner_count >= max_partners) {
          eligible = 0;
          break;
        }
        partners[partner_count++] = other;
      }
      continue;
    }
    /* Any other reference to the bare symbol escapes. */
    if (ir_operand_is_symbol_named(&insn->dest, sym) ||
        ir_operand_is_symbol_named(&insn->lhs, sym) ||
        ir_operand_is_symbol_named(&insn->rhs, sym)) {
      eligible = 0;
    }
    for (size_t a = 0; a < insn->argument_count && eligible; a++) {
      if (ir_operand_is_symbol_named(&insn->arguments[a], sym)) {
        eligible = 0;
      }
    }
  }
  if (!eligible || addr_count == 0) {
    return 0;
  }

  /* derive `base + CONST` offset temps from the base temps. A single
   * sweep suffices because ir_lowering emits the add right after the addr-of. */
  for (size_t i = 0; i < function->instruction_count && eligible; i++) {
    const IRInstruction *insn = &function->instructions[i];
    if (insn->op == IR_OP_BINARY && insn->text &&
        strcmp(insn->text, "+") == 0 && !insn->is_float &&
        insn->dest.kind == IR_OPERAND_TEMP && insn->dest.name &&
        insn->lhs.kind == IR_OPERAND_TEMP && insn->lhs.name &&
        insn->rhs.kind == IR_OPERAND_INT) {
      IRSroaAddr *base = ir_sroa_find_addr(addrs, addr_count, insn->lhs.name);
      if (base) {
        if (addr_count >= sizeof(addrs) / sizeof(addrs[0])) {
          eligible = 0;
          break;
        }
        addrs[addr_count].temp = insn->dest.name;
        addrs[addr_count].offset = base->offset + insn->rhs.int_value;
        addrs[addr_count].valid = 1;
        addr_count++;
      }
    }
  }
  if (!eligible) {
    return 0;
  }

  /* every use of every address temp must be exactly one of:
   *  - producer (ADDRESS_OF, or the `+CONST` that defined it): allowed once,
   *  - LOAD with the temp as the address (lhs),
   *  - STORE with the temp as the address (dest),
   *  - a `+CONST` deriving another (already-recorded) address temp.
   * Anything else (used as a value, passed to a call, stored into memory,
   * dynamic index) means the address escapes -> bail. Collect field slots. */
  for (size_t i = 0; i < function->instruction_count && eligible; i++) {
    const IRInstruction *insn = &function->instructions[i];

    /* Skip the defining instructions of address temps. */
    if ((insn->op == IR_OP_ADDRESS_OF || insn->op == IR_OP_BINARY) &&
        insn->dest.kind == IR_OPERAND_TEMP &&
        ir_sroa_find_addr(addrs, addr_count, insn->dest.name)) {
      continue;
    }

    if (insn->op == IR_OP_LOAD) {
      IRSroaAddr *a = ir_sroa_find_addr(addrs, addr_count, insn->lhs.name);
      if (a) {
        /* The address temp must appear ONLY as the load address. */
        if (ir_operand_is_temp_named(&insn->dest, a->temp) ||
            ir_operand_is_temp_named(&insn->rhs, a->temp)) {
          eligible = 0;
          break;
        }
        int size = (insn->rhs.kind == IR_OPERAND_INT)
                       ? (int)insn->rhs.int_value
                       : 8;
        if (!ir_sroa_note_slot(slots, &slot_count, a->offset, size,
                               insn->is_float, insn->float_bits)) {
          /* Mixed-width / mixed-class access of the same offset: not a clean
           * scalar field. Decline rather than guess. */
          eligible = 0;
          break;
        }
        continue;
      }
    }

    if (insn->op == IR_OP_STORE) {
      IRSroaAddr *a = ir_sroa_find_addr(addrs, addr_count, insn->dest.name);
      if (a) {
        if (ir_operand_is_temp_named(&insn->lhs, a->temp)) {
          eligible = 0; /* address used as the stored value -> escapes */
          break;
        }
        int size = (insn->rhs.kind == IR_OPERAND_INT)
                       ? (int)insn->rhs.int_value
                       : 8;
        if (!ir_sroa_note_slot(slots, &slot_count, a->offset, size,
                               insn->is_float, insn->float_bits)) {
          eligible = 0;
          break;
        }
        continue;
      }
    }

    /* Any other instruction must not reference an address temp at all. */
    for (size_t k = 0; k < addr_count; k++) {
      if (ir_sroa_temp_operand_uses(insn, addrs[k].temp) > 0) {
        eligible = 0;
        break;
      }
    }
  }

  if (!eligible || slot_count == 0) {
    return 0;
  }

  *slot_count_out = slot_count;
  *partner_count_out = partner_count;
  return 1;
}

/* True when two slot layouts are identical (same offsets/sizes/float class in
 * the same order). Group members must match exactly so a whole-aggregate copy
 * can be rewritten field-for-field. */
static int ir_sroa_slots_match(const IRSroaSlot *a, size_t an,
                               const IRSroaSlot *b, size_t bn) {
  if (an != bn) {
    return 0;
  }
  for (size_t i = 0; i < an; i++) {
    if (a[i].offset != b[i].offset || a[i].size != b[i].size ||
        a[i].is_float != b[i].is_float) {
      return 0;
    }
  }
  return 1;
}

/* Build the address records for `sym` again (cheap, bounded) so the transform
 * can map each LOAD/STORE address temp back to a field offset. Mirrors passes
 * 1-2 of the analyzer but only records addrs. */
static size_t ir_sroa_collect_addrs(IRFunction *function, const char *sym,
                                    IRSroaAddr *addrs, size_t cap) {
  size_t addr_count = 0;
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *insn = &function->instructions[i];
    if (insn->op == IR_OP_ADDRESS_OF &&
        ir_operand_is_symbol_named(&insn->lhs, sym) &&
        insn->dest.kind == IR_OPERAND_TEMP && insn->dest.name &&
        addr_count < cap) {
      addrs[addr_count].temp = insn->dest.name;
      addrs[addr_count].offset = 0;
      addrs[addr_count].valid = 1;
      addr_count++;
    }
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *insn = &function->instructions[i];
    if (insn->op == IR_OP_BINARY && insn->text &&
        strcmp(insn->text, "+") == 0 && !insn->is_float &&
        insn->dest.kind == IR_OPERAND_TEMP && insn->dest.name &&
        insn->lhs.kind == IR_OPERAND_TEMP && insn->lhs.name &&
        insn->rhs.kind == IR_OPERAND_INT && addr_count < cap) {
      IRSroaAddr *base = ir_sroa_find_addr(addrs, addr_count, insn->lhs.name);
      if (base) {
        addrs[addr_count].temp = insn->dest.name;
        addrs[addr_count].offset = base->offset + insn->rhs.int_value;
        addrs[addr_count].valid = 1;
        addr_count++;
      }
    }
  }
  return addr_count;
}

/* Return the group member that `name` is an address temp of, or NULL, also
 * writing the matching slot. */
static IRSroaMemberCtx *ir_sroa_addr_owner(IRSroaMemberCtx *members,
                                           size_t member_count,
                                           const char *name,
                                           long long *offset_out) {
  for (size_t m = 0; m < member_count; m++) {
    IRSroaAddr *a =
        ir_sroa_find_addr(members[m].addrs, members[m].addr_count, name);
    if (a) {
      *offset_out = a->offset;
      return &members[m];
    }
  }
  return NULL;
}

/* Build a scalar name "<member>$<offset>" into a fresh allocation. */
static char *ir_sroa_scalar_name(const char *member, long long offset) {
  int len = snprintf(NULL, 0, "%s$%lld", member, offset);
  char *s = (char *)malloc((size_t)len + 1);
  if (s) {
    snprintf(s, (size_t)len + 1, "%s$%lld", member, offset);
  }
  return s;
}

/* Transform a whole group of layout-compatible aggregates at once. `members`
 * lists each aggregate name + decl index; `slots` is the shared layout. Returns
 * 1 on success, 0 on allocation failure (stream left intact on failure). */
static int ir_sroa_transform_group(IRFunction *function,
                                    IRSroaMemberCtx *members,
                                    size_t member_count, const IRSroaSlot *slots,
                                    size_t slot_count) {
  IRInstructionVector vec = {0};
  int ok = 1;

  for (size_t i = 0; i < function->instruction_count && ok; i++) {
    IRInstruction *insn = &function->instructions[i];

    /* Replace each member's aggregate decl with per-slot scalar decls. */
    int is_member_decl = 0;
    for (size_t m = 0; m < member_count; m++) {
      if (i == members[m].decl_index) {
        is_member_decl = 1;
        for (size_t s = 0; s < slot_count && ok; s++) {
          IRInstruction decl = {0};
          decl.op = IR_OP_DECLARE_LOCAL;
          decl.location = insn->location;
          char *nm = ir_sroa_scalar_name(members[m].name, slots[s].offset);
          decl.dest = nm ? ir_operand_symbol(nm) : ir_operand_none();
          decl.text = mettle_strdup(ir_builtin_scalar_type_for_slot(
              slots[s].size, slots[s].is_float, slots[s].float_bits));
          free(nm);
          if (!decl.dest.name || !decl.text ||
              !ir_instruction_vector_append_move(&vec, &decl)) {
            ir_instruction_destroy_storage(&decl);
            ok = 0;
          }
        }
        break;
      }
    }
    if (is_member_decl) {
      continue;
    }

    /* Drop address-temp producers belonging to any member. */
    if ((insn->op == IR_OP_ADDRESS_OF || insn->op == IR_OP_BINARY) &&
        insn->dest.kind == IR_OPERAND_TEMP) {
      long long off = 0;
      if (ir_sroa_addr_owner(members, member_count, insn->dest.name, &off)) {
        continue;
      }
    }

    /* Whole-aggregate copy between two members -> per-slot scalar assigns. */
    if (insn->op == IR_OP_ASSIGN && insn->dest.kind == IR_OPERAND_SYMBOL &&
        insn->dest.name && insn->lhs.kind == IR_OPERAND_SYMBOL &&
        insn->lhs.name) {
      const char *dst_m = NULL;
      const char *src_m = NULL;
      for (size_t m = 0; m < member_count; m++) {
        if (strcmp(members[m].name, insn->dest.name) == 0) {
          dst_m = members[m].name;
        }
        if (strcmp(members[m].name, insn->lhs.name) == 0) {
          src_m = members[m].name;
        }
      }
      if (dst_m && src_m) {
        for (size_t s = 0; s < slot_count && ok; s++) {
          IRInstruction a = {0};
          a.op = IR_OP_ASSIGN;
          a.location = insn->location;
          a.is_float = slots[s].is_float;
          a.float_bits = slots[s].float_bits;
          char *dn = ir_sroa_scalar_name(dst_m, slots[s].offset);
          char *sn = ir_sroa_scalar_name(src_m, slots[s].offset);
          a.dest = dn ? ir_operand_symbol(dn) : ir_operand_none();
          a.lhs = sn ? ir_operand_symbol(sn) : ir_operand_none();
          if (a.dest.name) {
            a.dest.float_bits = slots[s].float_bits;
          }
          if (a.lhs.name) {
            a.lhs.float_bits = slots[s].float_bits;
          }
          free(dn);
          free(sn);
          if (!a.dest.name || !a.lhs.name ||
              !ir_instruction_vector_append_move(&vec, &a)) {
            ir_instruction_destroy_storage(&a);
            ok = 0;
          }
        }
        continue;
      }
    }

    /* LOAD via a member address temp -> ASSIGN dest <- scalar. */
    if (insn->op == IR_OP_LOAD && insn->lhs.kind == IR_OPERAND_TEMP) {
      long long off = 0;
      IRSroaMemberCtx *owner =
          ir_sroa_addr_owner(members, member_count, insn->lhs.name, &off);
      if (owner) {
        const IRSroaSlot *slot =
            ir_sroa_find_const_slot(slots, slot_count, off);
        if (!slot) {
          ok = 0;
          continue;
        }
        IRInstruction assign = {0};
        assign.op = IR_OP_ASSIGN;
        assign.location = insn->location;
        assign.is_float = slot->is_float;
        assign.float_bits = slot->float_bits;
        char *nm = ir_sroa_scalar_name(owner->name, off);
        if (!nm || !ir_operand_clone(&insn->dest, &assign.dest)) {
          free(nm);
          ir_instruction_destroy_storage(&assign);
          ok = 0;
        } else {
          assign.lhs = ir_operand_symbol(nm);
          assign.lhs.float_bits = slot->float_bits;
          free(nm);
          if (!assign.lhs.name ||
              !ir_instruction_vector_append_move(&vec, &assign)) {
            ir_instruction_destroy_storage(&assign);
            ok = 0;
          }
        }
        continue;
      }
    }

    /* STORE via a member address temp -> ASSIGN scalar <- value. */
    if (insn->op == IR_OP_STORE && insn->dest.kind == IR_OPERAND_TEMP) {
      long long off = 0;
      IRSroaMemberCtx *owner =
          ir_sroa_addr_owner(members, member_count, insn->dest.name, &off);
      if (owner) {
        const IRSroaSlot *slot =
            ir_sroa_find_const_slot(slots, slot_count, off);
        if (!slot) {
          ok = 0;
          continue;
        }
        IRInstruction assign = {0};
        assign.op = IR_OP_ASSIGN;
        assign.location = insn->location;
        assign.is_float = slot->is_float;
        assign.float_bits = slot->float_bits;
        char *nm = ir_sroa_scalar_name(owner->name, off);
        assign.dest = nm ? ir_operand_symbol(nm) : ir_operand_none();
        if (assign.dest.name) {
          assign.dest.float_bits = slot->float_bits;
        }
        free(nm);
        if (!assign.dest.name || !ir_operand_clone(&insn->lhs, &assign.lhs)) {
          ir_instruction_destroy_storage(&assign);
          ok = 0;
        } else if (!ir_instruction_vector_append_move(&vec, &assign)) {
          ir_instruction_destroy_storage(&assign);
          ok = 0;
        }
        continue;
      }
    }

    /* Everything else: copy verbatim. */
    {
      IRInstruction cloned = {0};
      if (!ir_clone_instruction_plain(insn, &cloned) ||
          !ir_instruction_vector_append_move(&vec, &cloned)) {
        ir_instruction_destroy_storage(&cloned);
        ok = 0;
      }
    }
  }

  if (!ok) {
    ir_instruction_vector_destroy(&vec);
    return 0;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    ir_instruction_destroy_storage(&function->instructions[i]);
  }
  free(function->instructions);
  function->instructions = vec.items;
  function->instruction_count = vec.count;
  function->instruction_capacity = vec.capacity;
  return 1;
}

/* Is `t` a builtin scalar type name (so the local is not an aggregate)? */
static int ir_sroa_is_scalar_type_name(const char *t) {
  return t && (strcmp(t, "int8") == 0 || strcmp(t, "int16") == 0 ||
               strcmp(t, "int32") == 0 || strcmp(t, "int64") == 0 ||
               strcmp(t, "uint8") == 0 || strcmp(t, "uint16") == 0 ||
               strcmp(t, "uint32") == 0 || strcmp(t, "uint64") == 0 ||
               strcmp(t, "float32") == 0 || strcmp(t, "float64") == 0 ||
               strcmp(t, "bool") == 0);
}

/* Find an aggregate local's DECLARE_LOCAL index by name, or SIZE_MAX. */
static size_t ir_sroa_find_decl(IRFunction *function, const char *name) {
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *insn = &function->instructions[i];
    if (insn->op == IR_OP_DECLARE_LOCAL &&
        insn->dest.kind == IR_OPERAND_SYMBOL && insn->dest.name &&
        insn->text && !ir_sroa_is_scalar_type_name(insn->text) &&
        strcmp(insn->dest.name, name) == 0) {
      return i;
    }
  }
  return SIZE_MAX;
}

int ir_sroa_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }

  for (int iter = 0; iter < 32; iter++) {
    int did_any = 0;

    for (size_t i = 0; i < function->instruction_count && !did_any; i++) {
      const IRInstruction *insn = &function->instructions[i];
      if (insn->op != IR_OP_DECLARE_LOCAL ||
          insn->dest.kind != IR_OPERAND_SYMBOL || !insn->dest.name ||
          !insn->text || ir_sroa_is_scalar_type_name(insn->text)) {
        continue;
      }

      /* Analyze the seed aggregate. */
      IRSroaSlot seed_slots[IR_SROA_MAX_SLOTS];
      size_t seed_slot_count = 0;
      const char *seed_partners[IR_SROA_MAX_GROUP];
      size_t seed_partner_count = 0;
      if (!ir_sroa_analyze_local(function, i, insn->dest.name, seed_slots,
                                 &seed_slot_count, seed_partners,
                                 &seed_partner_count, IR_SROA_MAX_GROUP)) {
        continue;
      }

      /* Build the connected group via whole-aggregate copies. Every member must
       * analyze cleanly AND share the seed's exact slot layout; otherwise the
       * whole group is abandoned (correctness over coverage). */
      IRSroaMember group[IR_SROA_MAX_GROUP];
      size_t group_count = 0;
      group[group_count].name = insn->dest.name;
      group[group_count].decl_index = i;
      group_count++;

      const char *worklist[IR_SROA_MAX_GROUP];
      size_t worklist_count = 0;
      for (size_t p = 0; p < seed_partner_count; p++) {
        worklist[worklist_count++] = seed_partners[p];
      }

      int group_ok = 1;
      while (worklist_count > 0 && group_ok) {
        const char *cand = worklist[--worklist_count];
        int already = 0;
        for (size_t g = 0; g < group_count; g++) {
          if (strcmp(group[g].name, cand) == 0) {
            already = 1;
            break;
          }
        }
        if (already) {
          continue;
        }
        if (group_count >= IR_SROA_MAX_GROUP) {
          group_ok = 0;
          break;
        }
        size_t cand_decl = ir_sroa_find_decl(function, cand);
        if (cand_decl == SIZE_MAX) {
          group_ok = 0; /* partner is a param/return temp, not a local: bail */
          break;
        }
        IRSroaSlot cand_slots[IR_SROA_MAX_SLOTS];
        size_t cand_slot_count = 0;
        const char *cand_partners[IR_SROA_MAX_GROUP];
        size_t cand_partner_count = 0;
        if (!ir_sroa_analyze_local(function, cand_decl, cand, cand_slots,
                                   &cand_slot_count, cand_partners,
                                   &cand_partner_count, IR_SROA_MAX_GROUP) ||
            !ir_sroa_slots_match(seed_slots, seed_slot_count, cand_slots,
                                 cand_slot_count)) {
          group_ok = 0;
          break;
        }
        group[group_count].name = cand;
        group[group_count].decl_index = cand_decl;
        group_count++;
        for (size_t p = 0; p < cand_partner_count; p++) {
          if (worklist_count < IR_SROA_MAX_GROUP) {
            worklist[worklist_count++] = cand_partners[p];
          } else {
            group_ok = 0;
          }
        }
      }

      if (!group_ok) {
        continue;
      }

      /* Build per-member address records and transform the whole group. */
      IRSroaMemberCtx *ctx =
          (IRSroaMemberCtx *)calloc(group_count, sizeof(IRSroaMemberCtx));
      if (!ctx) {
        return 0;
      }
      for (size_t g = 0; g < group_count; g++) {
        ctx[g].name = group[g].name;
        ctx[g].decl_index = group[g].decl_index;
        ctx[g].addr_count = ir_sroa_collect_addrs(
            function, group[g].name, ctx[g].addrs,
            sizeof(ctx[g].addrs) / sizeof(ctx[g].addrs[0]));
      }

      int rc = ir_sroa_transform_group(function, ctx, group_count, seed_slots,
                                       seed_slot_count);
      free(ctx);
      if (!rc) {
        return 0;
      }
      did_any = 1;
      if (changed) {
        *changed = 1;
      }
    }

    if (!did_any) {
      break;
    }
  }
  return 1;
}

