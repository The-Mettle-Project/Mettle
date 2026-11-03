#include "ir_optimize_internal.h"

static const char *ir_builtin_scalar_type_for_slot(int size, int is_float,
                                                    int float_bits,
                                                    int is_unsigned) {
  if (is_float) {
    return float_bits == 32 ? "float32" : "float64";
  }
  switch (size) {
  case 1:
    return is_unsigned ? "uint8" : "int8";
  case 2:
    return is_unsigned ? "uint16" : "int16";
  case 4:
    return is_unsigned ? "uint32" : "int32";
  default:
    return is_unsigned ? "uint64" : "int64";
  }
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

static int ir_sroa_scalar_access_width(int size) {
  return size == 1 || size == 2 || size == 4 || size == 8;
}

static int ir_sroa_note_slot(IRSroaSlot *slots, size_t *slot_count,
                             long long offset, int size, int is_float,
                             int float_bits, int is_unsigned, int alias_class) {
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
    slot->is_unsigned = is_unsigned;
    slot->alias_class = alias_class;
    slot->name = NULL;
    return 1;
  }

  if (is_unsigned) {
    slot->is_unsigned = 1;
  }
  return slot->size == size && slot->is_float == is_float &&
         slot->float_bits == float_bits && slot->alias_class == alias_class;
}

static void ir_sroa_sort_slots(IRSroaSlot *slots, size_t count) {
  for (size_t i = 1; i < count; i++) {
    IRSroaSlot key = slots[i];
    size_t j = i;
    while (j > 0 && slots[j - 1].offset > key.offset) {
      slots[j] = slots[j - 1];
      j--;
    }
    slots[j] = key;
  }
}

typedef struct {
  IRSroaSlot slots[IR_SROA_MAX_SLOTS];
  size_t slot_count;
} IRSroaLayout;

static int ir_sroa_merge_slots(IRSroaLayout *merged, const IRSroaSlot *slots,
                               size_t slot_count) {
  for (size_t s = 0; s < slot_count; s++) {
    if (!ir_sroa_note_slot(merged->slots, &merged->slot_count, slots[s].offset,
                           slots[s].size, slots[s].is_float, slots[s].float_bits,
                           slots[s].is_unsigned, slots[s].alias_class)) {
      return 0;
    }
  }
  ir_sroa_sort_slots(merged->slots, merged->slot_count);
  return 1;
}

static int ir_sroa_slots_apart(const IRSroaLayout *merged) {
  for (size_t s = 1; s < merged->slot_count; s++) {
    if (merged->slots[s - 1].offset + merged->slots[s - 1].size >
        merged->slots[s].offset) {
      return 0;
    }
  }
  return 1;
}

typedef struct {
  const char *name;
  size_t decl_index;
  size_t layout;
  IRSroaAddr addrs[IR_SROA_MAX_SLOTS * 2];
  size_t addr_count;
} IRSroaFlatMember;

typedef struct {
  const char *key;
  size_t member;
  long long offset;
} IRSroaHashEnt;

typedef struct {
  IRSroaHashEnt *ents;
  size_t bucket_count;
} IRSroaHash;

static int ir_sroa_hash_init(IRSroaHash *h, size_t expected) {
  size_t nb = 64;
  while (nb < expected * 2) {
    nb *= 2;
  }
  h->ents = (IRSroaHashEnt *)calloc(nb, sizeof(IRSroaHashEnt));
  if (!h->ents) {
    h->bucket_count = 0;
    return 0;
  }
  h->bucket_count = nb;
  return 1;
}

static void ir_sroa_hash_put(IRSroaHash *h, const char *key, size_t member,
                             long long offset) {
  size_t b = mettle_fnv1a_hash(key) & (h->bucket_count - 1);
  while (h->ents[b].key) {
    if (strcmp(h->ents[b].key, key) == 0) {
      return;
    }
    b = (b + 1) & (h->bucket_count - 1);
  }
  h->ents[b].key = key;
  h->ents[b].member = member;
  h->ents[b].offset = offset;
}

static const IRSroaHashEnt *ir_sroa_hash_get(const IRSroaHash *h,
                                             const char *key) {
  if (!h->bucket_count || !key) {
    return NULL;
  }
  size_t b = mettle_fnv1a_hash(key) & (h->bucket_count - 1);
  while (h->ents[b].key) {
    if (strcmp(h->ents[b].key, key) == 0) {
      return &h->ents[b];
    }
    b = (b + 1) & (h->bucket_count - 1);
  }
  return NULL;
}

static void ir_sroa_collect_all_addrs(IRFunction *function,
                                      IRSroaFlatMember *members,
                                      const IRSroaHash *member_hash,
                                      IRSroaHash *addr_hash) {
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *insn = &function->instructions[i];
    if (insn->op == IR_OP_ADDRESS_OF && insn->lhs.kind == IR_OPERAND_SYMBOL &&
        insn->lhs.name && insn->dest.kind == IR_OPERAND_TEMP &&
        insn->dest.name) {
      const IRSroaHashEnt *m = ir_sroa_hash_get(member_hash, insn->lhs.name);
      if (!m) {
        continue;
      }
      IRSroaFlatMember *fm = &members[m->member];
      if (fm->addr_count >= IR_ARRAY_COUNT(fm->addrs)) {
        continue;
      }
      fm->addrs[fm->addr_count].temp = insn->dest.name;
      fm->addrs[fm->addr_count].offset = 0;
      fm->addrs[fm->addr_count].valid = 1;
      fm->addr_count++;
      ir_sroa_hash_put(addr_hash, insn->dest.name, m->member, 0);
    }
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *insn = &function->instructions[i];
    if (insn->op == IR_OP_BINARY && insn->text &&
        strcmp(insn->text, "+") == 0 && !insn->is_float &&
        ir_operand_is_temp(&insn->dest) &&
        ir_operand_is_temp(&insn->lhs) &&
        insn->rhs.kind == IR_OPERAND_INT) {
      const IRSroaHashEnt *base = ir_sroa_hash_get(addr_hash, insn->lhs.name);
      if (!base) {
        continue;
      }
      IRSroaFlatMember *fm = &members[base->member];
      if (fm->addr_count >= IR_ARRAY_COUNT(fm->addrs)) {
        continue;
      }
      long long off = base->offset + insn->rhs.int_value;
      fm->addrs[fm->addr_count].temp = insn->dest.name;
      fm->addrs[fm->addr_count].offset = off;
      fm->addrs[fm->addr_count].valid = 1;
      fm->addr_count++;
      ir_sroa_hash_put(addr_hash, insn->dest.name, base->member, off);
    }
  }
}

static char *ir_sroa_scalar_name(const char *member, long long offset) {
  int len = snprintf(NULL, 0, "%s$%lld", member, offset);
  char *s = (char *)malloc((size_t)len + 1);
  if (s) {
    snprintf(s, (size_t)len + 1, "%s$%lld", member, offset);
  }
  return s;
}

static const MtlcType *ir_sroa_integer_field_at(const MtlcType *type,
                                                long long offset, int size,
                                                int depth) {
  if (!type || depth > 8) {
    return NULL;
  }
  if (type->kind >= MTLC_TYPE_INT8 && type->kind <= MTLC_TYPE_BOOL) {
    return (offset == 0 && (long long)type->size == size) ? type : NULL;
  }
  if (type->kind != MTLC_TYPE_STRUCT || !type->field_types ||
      !type->field_offsets) {
    return NULL;
  }
  for (size_t f = 0; f < type->field_count; f++) {
    const MtlcType *field = type->field_types[f];
    long long start = (long long)type->field_offsets[f];
    if (field && offset >= start && offset < start + (long long)field->size) {
      return ir_sroa_integer_field_at(field, offset - start, size, depth + 1);
    }
  }
  return NULL;
}

static const MtlcType *ir_sroa_declared_aggregate(const IRInstruction *decl) {
  if (decl->value_type) {
    return decl->value_type;
  }
  if (decl->text && ir_optimize_get_program()) {
    return ir_program_lookup_type(ir_optimize_get_program(), decl->text);
  }
  return NULL;
}

static int ir_sroa_piece_is_unsigned(const IRFunction *function,
                                     const IRSroaFlatMember *owner,
                                     const IRSroaSlot *slot) {
  const MtlcType *field = NULL;
  if (owner->decl_index < function->instruction_count) {
    field = ir_sroa_integer_field_at(
        ir_sroa_declared_aggregate(&function->instructions[owner->decl_index]),
        slot->offset, slot->size, 0);
  }
  if (field) {
    return field->kind >= MTLC_TYPE_UINT8 && field->kind <= MTLC_TYPE_BOOL;
  }
  return slot->is_unsigned;
}

static const char *ir_sroa_int_view_name(int size, int is_unsigned) {
  switch (size) {
  case 1:
    return is_unsigned ? "uint8" : "int8";
  case 2:
    return is_unsigned ? "uint16" : "int16";
  case 4:
    return is_unsigned ? "uint32" : "int32";
  default:
    return NULL;
  }
}

static IROperand ir_sroa_fresh_temp(void) {
  static int g_sroa_temp_counter;
  char name[48];
  snprintf(name, sizeof(name), "__sroa_word_%d", g_sroa_temp_counter++);
  return ir_operand_temp(name);
}

static int ir_sroa_load_pieces(IRInstructionVector *vec,
                               const IRSroaFlatMember *owner,
                               const IRSroaLayout *layout, const IROperand *from,
                               SourceLocation location) {
  for (size_t s = 0; s < layout->slot_count; s++) {
    const IRSroaSlot *slot = &layout->slots[s];
    IRInstruction step = {0};
    step.op = IR_OP_BINARY;
    step.location = location;
    step.text = mettle_strdup("+");
    step.dest = ir_sroa_fresh_temp();
    step.lhs = ir_operand_copy(from);
    step.rhs = ir_operand_int(slot->offset);
    IRInstruction read = {0};
    read.op = IR_OP_LOAD;
    read.location = location;
    read.is_float = slot->is_float;
    read.float_bits = slot->float_bits;
    read.is_unsigned = slot->is_unsigned;
    read.alias_class = slot->alias_class;
    char *piece = ir_sroa_scalar_name(owner->name, slot->offset);
    read.dest = piece ? ir_operand_symbol(piece) : ir_operand_none();
    read.dest.float_bits = slot->float_bits;
    free(piece);
    read.lhs = ir_operand_copy(&step.dest);
    read.rhs = ir_operand_int(slot->size);
    if (!step.text || !step.dest.name || !step.lhs.name || !read.dest.name ||
        !read.lhs.name || !ir_instruction_vector_append_move(vec, &step) ||
        !ir_instruction_vector_append_move(vec, &read)) {
      return 0;
    }
  }
  return 1;
}

static int ir_sroa_transform_all(IRFunction *function,
                                 const IRSroaFlatMember *members,
                                 size_t member_count,
                                 const IRSroaLayout *layouts,
                                 const IRSroaHash *member_hash,
                                 const IRSroaHash *addr_hash) {
  IRInstructionVector vec = {0};
  int ok = 1;
  (void)member_count;
  if (!ir_instruction_vector_reserve(&vec, function->instruction_count)) {
    return 0;
  }

  for (size_t i = 0; i < function->instruction_count && ok; i++) {
    IRInstruction *insn = &function->instructions[i];

    if (insn->op == IR_OP_DECLARE_LOCAL &&
        ir_operand_is_symbol(&insn->dest)) {
      const IRSroaHashEnt *me = ir_sroa_hash_get(member_hash, insn->dest.name);
      if (me && members[me->member].decl_index == i) {
        const IRSroaFlatMember *fm = &members[me->member];
        const IRSroaSlot *slots = layouts[fm->layout].slots;
        size_t slot_count = layouts[fm->layout].slot_count;
        for (size_t s = 0; s < slot_count && ok; s++) {
          IRInstruction decl = {0};
          decl.op = IR_OP_DECLARE_LOCAL;
          decl.location = insn->location;
          char *nm = ir_sroa_scalar_name(fm->name, slots[s].offset);
          decl.dest = nm ? ir_operand_symbol(nm) : ir_operand_none();
          {
            const MtlcType *field = NULL;
            const MtlcType *aggregate = ir_sroa_declared_aggregate(insn);
            for (size_t f = 0; aggregate && aggregate->field_types &&
                               aggregate->field_offsets &&
                               f < aggregate->field_count;
                 f++) {
              if ((long long)aggregate->field_offsets[f] == slots[s].offset &&
                  aggregate->field_types[f] &&
                  aggregate->field_types[f]->kind == MTLC_TYPE_POINTER &&
                  aggregate->field_types[f]->base_type &&
                  aggregate->field_types[f]->base_type->kind <= MTLC_TYPE_BFLOAT16 &&
                  aggregate->field_types[f]->name &&
                  (long long)aggregate->field_types[f]->size == slots[s].size) {
                field = aggregate->field_types[f];
                break;
              }
            }
            if (!field && !slots[s].is_float) {
              const MtlcType *scalar = ir_sroa_integer_field_at(
                  aggregate, slots[s].offset, slots[s].size, 0);
              if (scalar && scalar->name) {
                field = scalar;
              }
            }
            if (field) {
              decl.text = mettle_strdup(field->name);
              decl.value_type = (MtlcType *)field;
            } else {
              decl.text = mettle_strdup(ir_builtin_scalar_type_for_slot(
                  slots[s].size, slots[s].is_float, slots[s].float_bits,
                  slots[s].is_unsigned));
            }
          }
          free(nm);
          if (!decl.dest.name || !decl.text ||
              !ir_instruction_vector_append_move(&vec, &decl)) {
            ir_instruction_destroy_storage(&decl);
            ok = 0;
          }
        }
        continue;
      }
    }

    if ((insn->op == IR_OP_ADDRESS_OF || insn->op == IR_OP_BINARY) &&
        insn->dest.kind == IR_OPERAND_TEMP) {
      if (ir_sroa_hash_get(addr_hash, insn->dest.name)) {
        continue;
      }
    }

    if (insn->op == IR_OP_ASSIGN && insn->dest.kind == IR_OPERAND_SYMBOL &&
        insn->dest.name && insn->lhs.kind == IR_OPERAND_SYMBOL &&
        insn->lhs.name) {
      const IRSroaHashEnt *de = ir_sroa_hash_get(member_hash, insn->dest.name);
      const IRSroaHashEnt *se = ir_sroa_hash_get(member_hash, insn->lhs.name);
      if (de && se &&
          members[de->member].layout == members[se->member].layout) {
        const char *dst_m = members[de->member].name;
        const char *src_m = members[se->member].name;
        const IRSroaSlot *slots = layouts[members[de->member].layout].slots;
        size_t slot_count = layouts[members[de->member].layout].slot_count;
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
      if (de && !se) {
        const IRSroaFlatMember *owner = &members[de->member];
        IRInstruction take = {0};
        take.op = IR_OP_ADDRESS_OF;
        take.location = insn->location;
        take.dest = ir_sroa_fresh_temp();
        take.lhs = ir_operand_symbol(insn->lhs.name);
        IROperand whole = ir_operand_copy(&take.dest);
        if (!take.dest.name || !take.lhs.name ||
            !ir_instruction_vector_append_move(&vec, &take) ||
            !ir_sroa_load_pieces(&vec, owner, &layouts[owner->layout], &whole,
                                 insn->location)) {
          ok = 0;
        }
        ir_operand_destroy(&whole);
        continue;
      }
    }

    if (insn->op == IR_OP_LOAD && insn->lhs.kind == IR_OPERAND_TEMP) {
      const IRSroaHashEnt *ae = ir_sroa_hash_get(addr_hash, insn->lhs.name);
      if (ae) {
        long long off = ae->offset;
        const IRSroaFlatMember *owner = &members[ae->member];
        const IRSroaSlot *slot =
            ir_sroa_find_const_slot(layouts[owner->layout].slots,
                                    layouts[owner->layout].slot_count, off);
        if (!slot) {
          ok = 0;
          continue;
        }
        IRInstruction assign = {0};
        assign.op = IR_OP_ASSIGN;
        assign.location = insn->location;
        assign.is_float = slot->is_float;
        assign.float_bits = slot->float_bits;
        if (!slot->is_float && slot->size < 8 &&
            (insn->is_unsigned ? 1 : 0) !=
                ir_sroa_piece_is_unsigned(function, owner, slot)) {
          const char *view =
              ir_sroa_int_view_name(slot->size, insn->is_unsigned);
          if (view) {
            assign.op = IR_OP_CAST;
            assign.text = mettle_strdup(view);
            assign.is_unsigned = insn->is_unsigned;
            assign.value_type = insn->value_type;
            if (!assign.text) {
              ok = 0;
              continue;
            }
          }
        }
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

    if (insn->op == IR_OP_STORE && insn->dest.kind == IR_OPERAND_TEMP) {
      const IRSroaHashEnt *ae = ir_sroa_hash_get(addr_hash, insn->dest.name);
      if (ae) {
        long long off = ae->offset;
        const IRSroaFlatMember *owner = &members[ae->member];
        if (insn->rhs.kind == IR_OPERAND_INT && insn->rhs.int_value > 8) {
          if (!ir_sroa_load_pieces(&vec, owner, &layouts[owner->layout], &insn->lhs,
                                   insn->location)) {
            ok = 0;
          }
          continue;
        }
        const IRSroaSlot *slot =
            ir_sroa_find_const_slot(layouts[owner->layout].slots,
                                    layouts[owner->layout].slot_count, off);
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

    if (!ir_instruction_vector_append_move(&vec, insn)) {
      ok = 0;
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

static int ir_sroa_is_scalar_type_name(const char *t) {
  return t && (strcmp(t, "int8") == 0 || strcmp(t, "int16") == 0 ||
               strcmp(t, "int32") == 0 || strcmp(t, "int64") == 0 ||
               strcmp(t, "uint8") == 0 || strcmp(t, "uint16") == 0 ||
               strcmp(t, "uint32") == 0 || strcmp(t, "uint64") == 0 ||
               strcmp(t, "float32") == 0 || strcmp(t, "float64") == 0 ||
               strcmp(t, "float16") == 0 || strcmp(t, "bfloat16") == 0 ||
               strcmp(t, "bool") == 0);
}

typedef struct {
  const char *name;
  size_t decl_index;
  int eligible;
  int comp_fail;
  IRSroaSlot slots[IR_SROA_MAX_SLOTS];
  size_t slot_count;
  size_t partners[IR_SROA_MAX_GROUP];
  size_t partner_count;
  size_t addr_count;
  size_t visited;
} IRSroaRec;

static void ir_sroa_rec_add_partner(IRSroaRec *recs, size_t owner,
                                    size_t other) {
  IRSroaRec *rec = &recs[owner];
  for (size_t p = 0; p < rec->partner_count; p++) {
    if (rec->partners[p] == other) {
      return;
    }
  }
  if (rec->partner_count >= IR_SROA_MAX_GROUP) {
    rec->comp_fail = 1;
    rec->eligible = 0;
    recs[other].comp_fail = 1;
    recs[other].eligible = 0;
    return;
  }
  rec->partners[rec->partner_count++] = other;
}

static void ir_sroa_rec_add_addr(IRSroaRec *recs, size_t owner,
                                 IRSroaHash *addr_hash, const char *temp,
                                 long long offset) {
  IRSroaRec *rec = &recs[owner];
  if (rec->addr_count >= IR_SROA_MAX_SLOTS * 2) {
    rec->eligible = 0;
    return;
  }
  const IRSroaHashEnt *dup = ir_sroa_hash_get(addr_hash, temp);
  if (dup) {
    recs[dup->member].eligible = 0;
    rec->eligible = 0;
    return;
  }
  ir_sroa_hash_put(addr_hash, temp, owner, offset);
  rec->addr_count++;
}

static int ir_sroa_text_is_descriptor(const char *text) {
  size_t length = text ? strlen(text) : 0;
  return length > 0 && (text[length - 1] == ']' || strcmp(text, "string") == 0);
}

static int ir_sroa_is_whole_size(const IRFunction *function, const IRSroaRec *rec,
                                 long long bytes) {
  const IRInstruction *decl = &function->instructions[rec->decl_index];
  const MtlcType *type = ir_sroa_declared_aggregate(decl);
  return type && type->kind == MTLC_TYPE_STRUCT &&
         !ir_sroa_text_is_descriptor(decl->text) && (long long)type->size == bytes;
}

static int ir_sroa_rec_is_struct(const IRFunction *function, const IRSroaRec *rec) {
  const IRInstruction *decl = &function->instructions[rec->decl_index];
  const MtlcType *type = ir_sroa_declared_aggregate(decl);
  return type && type->kind == MTLC_TYPE_STRUCT &&
         !ir_sroa_text_is_descriptor(decl->text);
}

static int ir_sroa_scan_uses(IRFunction *function, size_t i, IRSroaRec *recs,
                             IRSroaHash *addr_hash) {
  const IRInstruction *insn = &function->instructions[i];
  if ((insn->op == IR_OP_ADDRESS_OF || insn->op == IR_OP_BINARY) &&
      insn->dest.kind == IR_OPERAND_TEMP &&
      ir_sroa_hash_get(addr_hash, insn->dest.name)) {
    return 1;
  }
  if (insn->op == IR_OP_LOAD) {
    const IRSroaHashEnt *ae = ir_sroa_hash_get(addr_hash, insn->lhs.name);
    if (ae) {
      IRSroaRec *rec = &recs[ae->member];
      if (ir_operand_is_temp_named(&insn->dest, insn->lhs.name) ||
          ir_operand_is_temp_named(&insn->rhs, insn->lhs.name)) {
        rec->eligible = 0;
        return 1;
      }
      if (ir_operand_is_temp(&insn->dest)) {
        const IRSroaHashEnt *o =
            ir_sroa_hash_get(addr_hash, insn->dest.name);
        if (o) {
          recs[o->member].eligible = 0;
        }
      }
      if (ir_operand_is_temp(&insn->rhs)) {
        const IRSroaHashEnt *o =
            ir_sroa_hash_get(addr_hash, insn->rhs.name);
        if (o) {
          recs[o->member].eligible = 0;
        }
      }
      int size = (insn->rhs.kind == IR_OPERAND_INT)
                     ? (int)insn->rhs.int_value
                     : 8;
      if (!ir_sroa_scalar_access_width(size)) {
        rec->eligible = 0;
        return 1;
      }
      if (!ir_sroa_note_slot(rec->slots, &rec->slot_count, ae->offset,
                             size, insn->is_float, insn->float_bits,
                             insn->is_unsigned, insn->alias_class)) {
        rec->eligible = 0;
      }
      return 1;
    }
  }
  if (insn->op == IR_OP_STORE) {
    const IRSroaHashEnt *ae =
        ir_sroa_hash_get(addr_hash, insn->dest.name);
    if (ae) {
      IRSroaRec *rec = &recs[ae->member];
      if (ir_operand_is_temp_named(&insn->lhs, insn->dest.name)) {
        rec->eligible = 0;
        return 1;
      }
      if (ae->offset == 0 && !insn->is_volatile && ir_operand_is_temp(&insn->lhs) &&
          !ir_sroa_hash_get(addr_hash, insn->lhs.name) &&
          insn->rhs.kind == IR_OPERAND_INT && insn->rhs.int_value > 8 &&
          ir_sroa_is_whole_size(function, rec, insn->rhs.int_value)) {
        return 1;
      }
      if (ir_operand_is_temp(&insn->lhs)) {
        const IRSroaHashEnt *o =
            ir_sroa_hash_get(addr_hash, insn->lhs.name);
        if (o) {
          recs[o->member].eligible = 0;
        }
      }
      if (ir_operand_is_temp(&insn->rhs)) {
        const IRSroaHashEnt *o =
            ir_sroa_hash_get(addr_hash, insn->rhs.name);
        if (o) {
          recs[o->member].eligible = 0;
        }
      }
      int size = (insn->rhs.kind == IR_OPERAND_INT)
                     ? (int)insn->rhs.int_value
                     : 8;
      if (!ir_sroa_scalar_access_width(size)) {
        rec->eligible = 0;
        return 1;
      }
      if (!ir_sroa_note_slot(rec->slots, &rec->slot_count, ae->offset,
                             size, insn->is_float, insn->float_bits,
                             insn->is_unsigned, insn->alias_class)) {
        rec->eligible = 0;
      }
      return 1;
    }
  }
  if (ir_operand_is_temp(&insn->dest)) {
    const IRSroaHashEnt *ae =
        ir_sroa_hash_get(addr_hash, insn->dest.name);
    if (ae) {
      recs[ae->member].eligible = 0;
    }
  }
  if (ir_operand_is_temp(&insn->lhs)) {
    const IRSroaHashEnt *ae = ir_sroa_hash_get(addr_hash, insn->lhs.name);
    if (ae) {
      recs[ae->member].eligible = 0;
    }
  }
  if (ir_operand_is_temp(&insn->rhs)) {
    const IRSroaHashEnt *ae = ir_sroa_hash_get(addr_hash, insn->rhs.name);
    if (ae) {
      recs[ae->member].eligible = 0;
    }
  }
  for (size_t a = 0; a < insn->argument_count; a++) {
    const IROperand *arg = &insn->arguments[a];
    if (arg->kind == IR_OPERAND_TEMP && arg->name) {
      const IRSroaHashEnt *ae = ir_sroa_hash_get(addr_hash, arg->name);
      if (ae) {
        recs[ae->member].eligible = 0;
      }
    }
  }
  return 1;
}

static int ir_sroa_is_parameter(const IRFunction *function, const char *name) {
  for (size_t p = 0; p < function->parameter_count; p++) {
    if (function->parameter_names[p] &&
        strcmp(function->parameter_names[p], name) == 0) {
      return 1;
    }
  }
  return 0;
}

static int ir_sroa_collect_records(IRFunction *function, size_t i,
                                   IRSroaRec *recs, IRSroaHash *rec_hash,
                                   IRSroaHash *addr_hash) {
  const IRInstruction *insn = &function->instructions[i];
  if (insn->op == IR_OP_ADDRESS_OF &&
      ir_operand_is_symbol(&insn->lhs) &&
      ir_operand_is_temp(&insn->dest)) {
    const IRSroaHashEnt *m = ir_sroa_hash_get(rec_hash, insn->lhs.name);
    if (m) {
      ir_sroa_rec_add_addr(recs, m->member, addr_hash, insn->dest.name,
                           0);
      return 1;
    }
  }
  if (insn->op == IR_OP_DECLARE_LOCAL &&
      ir_operand_is_symbol(&insn->dest)) {
    const IRSroaHashEnt *m = ir_sroa_hash_get(rec_hash, insn->dest.name);
    if (m && recs[m->member].decl_index == i) {
      return 1;
    }
  }
  if (insn->op == IR_OP_ASSIGN && insn->dest.kind == IR_OPERAND_SYMBOL &&
      insn->dest.name && insn->lhs.kind == IR_OPERAND_SYMBOL &&
      insn->lhs.name) {
    const IRSroaHashEnt *de = ir_sroa_hash_get(rec_hash, insn->dest.name);
    const IRSroaHashEnt *se = ir_sroa_hash_get(rec_hash, insn->lhs.name);
    if (de || se) {
      if (ir_operand_names_match(&insn->dest, &insn->lhs)) {
        recs[(de ? de : se)->member].eligible = 0;
        return 1;
      }
      if (de && se) {
        ir_sroa_rec_add_partner(recs, de->member, se->member);
        ir_sroa_rec_add_partner(recs, se->member, de->member);
      } else if (!de || !ir_sroa_is_parameter(function, insn->lhs.name) ||
                 !ir_sroa_rec_is_struct(function, &recs[de->member])) {
        recs[(de ? de : se)->member].comp_fail = 1;
      }
      return 1;
    }
  }
  if (ir_operand_is_symbol(&insn->dest)) {
    const IRSroaHashEnt *m = ir_sroa_hash_get(rec_hash, insn->dest.name);
    if (m) {
      recs[m->member].eligible = 0;
    }
  }
  if (ir_operand_is_symbol(&insn->lhs)) {
    const IRSroaHashEnt *m = ir_sroa_hash_get(rec_hash, insn->lhs.name);
    if (m) {
      recs[m->member].eligible = 0;
    }
  }
  if (ir_operand_is_symbol(&insn->rhs)) {
    const IRSroaHashEnt *m = ir_sroa_hash_get(rec_hash, insn->rhs.name);
    if (m) {
      recs[m->member].eligible = 0;
    }
  }
  for (size_t a = 0; a < insn->argument_count; a++) {
    const IROperand *arg = &insn->arguments[a];
    if (arg->kind == IR_OPERAND_SYMBOL && arg->name) {
      const IRSroaHashEnt *m = ir_sroa_hash_get(rec_hash, arg->name);
      if (m) {
        recs[m->member].eligible = 0;
      }
    }
  }
  return 1;
}

static int ir_sroa_round(IRFunction *function, int *changed,
                         int iter) {
  IRSroaRec *recs = NULL;
  size_t rec_count = 0, rec_cap = 0;
  IRSroaHash rec_hash = {0};
  IRSroaHash addr_hash = {0};

  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *insn = &function->instructions[i];
    if (insn->op != IR_OP_DECLARE_LOCAL ||
        insn->dest.kind != IR_OPERAND_SYMBOL || !insn->dest.name ||
        !insn->text || ir_sroa_is_scalar_type_name(insn->text)) {
      continue;
    }
    if (rec_count >= rec_cap) {
      size_t nc = rec_cap ? rec_cap * 2 : 16;
      IRSroaRec *grown = (IRSroaRec *)realloc(recs, nc * sizeof(IRSroaRec));
      if (!grown) {
        free(recs);
        return 0;
      }
      recs = grown;
      rec_cap = nc;
    }
    IRSroaRec *rec = &recs[rec_count];
    memset(rec, 0, sizeof(*rec));
    rec->name = insn->dest.name;
    rec->decl_index = i;
    rec->eligible = 1;
    rec_count++;
  }
  if (rec_count == 0) {
    free(recs);
    return 0;
  }
  if (!ir_sroa_hash_init(&rec_hash, rec_count) ||
      !ir_sroa_hash_init(&addr_hash, rec_count * IR_SROA_MAX_SLOTS * 2)) {
    free(rec_hash.ents);
    free(addr_hash.ents);
    free(recs);
    return 0;
  }
  for (size_t r = 0; r < rec_count; r++) {
    const IRSroaHashEnt *dup = ir_sroa_hash_get(&rec_hash, recs[r].name);
    if (dup) {
      recs[dup->member].eligible = 0;
      recs[r].eligible = 0;
      continue;
    }
    ir_sroa_hash_put(&rec_hash, recs[r].name, r, 0);
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    if (!ir_sroa_collect_records(function, i, recs, &rec_hash, &addr_hash)) {
      return 1;
    }
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *insn = &function->instructions[i];
    if (insn->op == IR_OP_BINARY && insn->text &&
        strcmp(insn->text, "+") == 0 && !insn->is_float &&
        ir_operand_is_temp(&insn->dest) &&
        ir_operand_is_temp(&insn->lhs) &&
        insn->rhs.kind == IR_OPERAND_INT) {
      const IRSroaHashEnt *base =
          ir_sroa_hash_get(&addr_hash, insn->lhs.name);
      if (base) {
        ir_sroa_rec_add_addr(recs, base->member, &addr_hash,
                             insn->dest.name,
                             base->offset + insn->rhs.int_value);
      }
    }
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    if (!ir_sroa_scan_uses(function, i, recs, &addr_hash)) {
      return 1;
    }
  }

  for (size_t r = 0; r < rec_count; r++) {
    if ((recs[r].partner_count == 0 || !ir_sroa_rec_is_struct(function, &recs[r])) &&
        (recs[r].addr_count == 0 || recs[r].slot_count == 0)) {
      recs[r].eligible = 0;
    }
    ir_sroa_sort_slots(recs[r].slots, recs[r].slot_count);
  }
  static int sroa_debug = -1;
  if (sroa_debug < 0) {
    sroa_debug = getenv("METTLE_SROA_DEBUG") ? 1 : 0;
  }
  if (sroa_debug) {
    for (size_t r = 0; r < rec_count; r++) {
      fprintf(stderr,
              "SROA rec %zu name=%s decl=%zu elig=%d comp_fail=%d "
              "partners=%zu addrs=%zu slots=%zu\n",
              r, recs[r].name, recs[r].decl_index, recs[r].eligible,
              recs[r].comp_fail, recs[r].partner_count, recs[r].addr_count,
              recs[r].slot_count);
    }
  }
  free(addr_hash.ents);
  addr_hash.ents = NULL;
  addr_hash.bucket_count = 0;

  IRSroaFlatMember *members = NULL;
  IRSroaLayout *layouts = NULL;
  size_t member_count = 0, member_cap = 0;
  size_t layout_count = 0, layout_cap = 0;
  size_t comp[IR_SROA_MAX_GROUP];
  size_t stack[IR_SROA_MAX_GROUP];
  int oom = 0;

  for (size_t r = 0; r < rec_count && !oom; r++) {
    if (recs[r].visited) {
      continue;
    }
    size_t comp_count = 0, stack_count = 0;
    int comp_ok = 1;
    recs[r].visited = r + 1;
    stack[stack_count++] = r;
    while (stack_count > 0) {
      size_t cur = stack[--stack_count];
      if (comp_count >= IR_SROA_MAX_GROUP) {
        comp_ok = 0;
        break;
      }
      comp[comp_count++] = cur;
      for (size_t p = 0; p < recs[cur].partner_count; p++) {
        size_t nxt = recs[cur].partners[p];
        if (recs[nxt].visited && recs[nxt].visited != r + 1) {
          comp_ok = 0;
        }
        if (!recs[nxt].visited) {
          recs[nxt].visited = r + 1;
          if (stack_count >= IR_SROA_MAX_GROUP) {
            comp_ok = 0;
            break;
          }
          stack[stack_count++] = nxt;
        }
      }
      if (!comp_ok) {
        break;
      }
    }
    IRSroaLayout merged = {0};
    if (comp_ok) {
      for (size_t c = 0; c < comp_count && comp_ok; c++) {
        const IRSroaRec *m = &recs[comp[c]];
        if (!m->eligible || m->comp_fail ||
            !ir_sroa_merge_slots(&merged, m->slots, m->slot_count)) {
          comp_ok = 0;
        }
      }
      if (comp_ok && (merged.slot_count == 0 || !ir_sroa_slots_apart(&merged))) {
        comp_ok = 0;
      }
      for (size_t c = 0; c < comp_count && comp_ok; c++) {
        const IRSroaRec *m = &recs[comp[c]];
        if (!ir_sroa_rec_is_struct(function, m) && m->slot_count != merged.slot_count) {
          comp_ok = 0;
        }
      }
    }
    if (!comp_ok) {
      continue;
    }

    if (layout_count >= layout_cap) {
      size_t nc = layout_cap ? layout_cap * 2 : 8;
      IRSroaLayout *grown =
          (IRSroaLayout *)realloc(layouts, nc * sizeof(IRSroaLayout));
      if (!grown) {
        oom = 1;
        break;
      }
      layouts = grown;
      layout_cap = nc;
    }
    layouts[layout_count] = merged;

    if (member_count + comp_count > member_cap) {
      size_t nc = member_cap ? member_cap * 2 : 16;
      while (nc < member_count + comp_count) {
        nc *= 2;
      }
      IRSroaFlatMember *grown = (IRSroaFlatMember *)realloc(
          members, nc * sizeof(IRSroaFlatMember));
      if (!grown) {
        oom = 1;
        break;
      }
      members = grown;
      member_cap = nc;
    }
    for (size_t c = 0; c < comp_count; c++) {
      IRSroaFlatMember *fm = &members[member_count++];
      fm->name = recs[comp[c]].name;
      fm->decl_index = recs[comp[c]].decl_index;
      fm->layout = layout_count;
      fm->addr_count = 0;
    }
    layout_count++;
  }

  free(rec_hash.ents);
  free(recs);
  if (oom) {
    free(members);
    free(layouts);
    return 0;
  }
  if (sroa_debug) {
    fprintf(stderr, "SROA fn=%s iter=%d members=%zu layouts=%zu\n",
            function->name ? function->name : "?", iter, member_count,
            layout_count);
  }
  if (member_count == 0) {
    free(members);
    free(layouts);
    return 0;
  }

  IRSroaHash member_hash = {0};
  IRSroaHash xform_addr_hash = {0};
  int rc = ir_sroa_hash_init(&member_hash, member_count) &&
           ir_sroa_hash_init(&xform_addr_hash,
                             member_count * IR_SROA_MAX_SLOTS * 2);
  if (rc) {
    for (size_t m = 0; m < member_count; m++) {
      ir_sroa_hash_put(&member_hash, members[m].name, m, 0);
    }
    ir_sroa_collect_all_addrs(function, members, &member_hash,
                              &xform_addr_hash);
    rc = ir_sroa_transform_all(function, members, member_count, layouts,
                               &member_hash, &xform_addr_hash);
  }
  free(member_hash.ents);
  free(xform_addr_hash.ents);
  free(members);
  free(layouts);
  if (!rc) {
    return 0;
  }
  if (changed) {
    *changed = 1;
  }
  return 1;
}

static void ir_sroa_note_piece_read(IRSroaHash *reads, const IROperand *operand) {
  if (ir_operand_is_symbol(operand) && strchr(operand->name, '$')) {
    ir_sroa_hash_put(reads, operand->name, 0, 0);
  }
}

static void ir_sroa_drop_unread_pieces(IRFunction *function) {
  for (int removed = 1; removed;) {
    IRSroaHash reads = {0};
    removed = 0;
    if (!ir_sroa_hash_init(&reads, function->instruction_count)) {
      return;
    }
    for (size_t i = 0; i < function->instruction_count; i++) {
      const IRInstruction *in = &function->instructions[i];
      if (in->op == IR_OP_ADDRESS_OF || in->op == IR_OP_STORE) {
        ir_sroa_note_piece_read(&reads, &in->dest);
      }
      ir_sroa_note_piece_read(&reads, &in->lhs);
      ir_sroa_note_piece_read(&reads, &in->rhs);
      for (size_t a = 0; a < in->argument_count; a++) {
        ir_sroa_note_piece_read(&reads, &in->arguments[a]);
      }
    }
    unsigned char *unread = calloc(function->instruction_count, 1);
    if (!unread) {
      free(reads.ents);
      return;
    }
    for (size_t i = 0; i < function->instruction_count; i++) {
      const IRInstruction *in = &function->instructions[i];
      if (!ir_operand_is_symbol(&in->dest) || !strchr(in->dest.name, '$') ||
          ir_sroa_hash_get(&reads, in->dest.name)) {
        continue;
      }
      if (in->op == IR_OP_DECLARE_LOCAL || in->op == IR_OP_ASSIGN ||
          (in->op == IR_OP_LOAD && !in->is_volatile)) {
        unread[i] = 1;
      }
    }
    free(reads.ents);
    for (size_t i = 0; i < function->instruction_count; i++) {
      if (unread[i]) {
        ir_instruction_make_nop(&function->instructions[i]);
        removed = 1;
      }
    }
    free(unread);
  }
}

static int ir_sroa_operand_is_piece(const IROperand *operand) {
  return ir_operand_is_symbol(operand) && strchr(operand->name, '$') != NULL;
}

static void ir_sroa_count_up(IRSroaHash *counts, const char *name, long long by) {
  ir_sroa_hash_put(counts, name, 0, 0);
  const IRSroaHashEnt *known = ir_sroa_hash_get(counts, name);
  if (known) {
    ((IRSroaHashEnt *)known)->offset += by;
  }
}

static int ir_sroa_type_is_word(const char *type) {
  size_t length = type ? strlen(type) : 0;
  return length > 0 && (strcmp(type, "int64") == 0 || strcmp(type, "uint64") == 0 ||
                        type[length - 1] == '*');
}

static int ir_sroa_forward_read(IRFunction *function, const IRSroaHash *sources,
                                size_t at, IROperand *operand, int *changed) {
  if (!ir_sroa_operand_is_piece(operand)) {
    return 1;
  }
  const IRSroaHashEnt *source = ir_sroa_hash_get(sources, operand->name);
  if (!source || !ir_function_instruction_dominates(function, source->member, at)) {
    return 1;
  }
  IROperand value = ir_operand_none();
  if (!ir_operand_clone(&function->instructions[source->member].lhs, &value)) {
    return 0;
  }
  ir_operand_destroy(operand);
  *operand = value;
  if (changed) {
    *changed = 1;
  }
  return 1;
}

int ir_forward_piece_copies_pass(IRFunction *function, int *changed) {
  size_t candidates = 0;
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (in->op == IR_OP_ASSIGN && ir_sroa_operand_is_piece(&in->dest) && !in->is_float &&
        (ir_operand_is_temp(&in->lhs) || in->lhs.kind == IR_OPERAND_INT)) {
      candidates++;
    }
  }
  if (!candidates) {
    return 1;
  }
  IRSroaHash piece_writes = {0};
  IRSroaHash temp_writes = {0};
  IRSroaHash sources = {0};
  if (!ir_sroa_hash_init(&piece_writes, function->instruction_count) ||
      !ir_sroa_hash_init(&temp_writes, function->instruction_count) ||
      !ir_sroa_hash_init(&sources, candidates)) {
    free(piece_writes.ents);
    free(temp_writes.ents);
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (in->op == IR_OP_DECLARE_LOCAL) {
      if (ir_sroa_operand_is_piece(&in->dest) && !ir_sroa_type_is_word(in->text)) {
        ir_sroa_count_up(&piece_writes, in->dest.name, 2);
      }
      continue;
    }
    if (in->op == IR_OP_ADDRESS_OF && ir_sroa_operand_is_piece(&in->lhs)) {
      ir_sroa_count_up(&piece_writes, in->lhs.name, 2);
    }
    if (ir_instruction_writes_destination(in)) {
      if (ir_sroa_operand_is_piece(&in->dest)) {
        ir_sroa_count_up(&piece_writes, in->dest.name, 1);
      } else if (ir_operand_is_temp(&in->dest)) {
        ir_sroa_count_up(&temp_writes, in->dest.name, 1);
      }
    }
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (in->op != IR_OP_ASSIGN || !ir_sroa_operand_is_piece(&in->dest) || in->is_float) {
      continue;
    }
    const IRSroaHashEnt *writes = ir_sroa_hash_get(&piece_writes, in->dest.name);
    if (!writes || writes->offset != 1) {
      continue;
    }
    if (in->lhs.kind == IR_OPERAND_INT) {
      ir_sroa_hash_put(&sources, in->dest.name, i, 0);
    } else if (ir_operand_is_temp(&in->lhs)) {
      const IRSroaHashEnt *temp = ir_sroa_hash_get(&temp_writes, in->lhs.name);
      if (temp && temp->offset == 1) {
        ir_sroa_hash_put(&sources, in->dest.name, i, 0);
      }
    }
  }
  int forwarded = 0;
  int ok = 1;
  for (size_t i = 0; i < function->instruction_count && ok; i++) {
    IRInstruction *in = &function->instructions[i];
    if (in->op == IR_OP_DECLARE_LOCAL || in->op == IR_OP_NOP) {
      continue;
    }
    ok = ir_sroa_forward_read(function, &sources, i, &in->lhs, &forwarded) &&
         ir_sroa_forward_read(function, &sources, i, &in->rhs, &forwarded);
    if (ok && in->op == IR_OP_STORE) {
      ok = ir_sroa_forward_read(function, &sources, i, &in->dest, &forwarded);
    }
    for (size_t a = 0; a < in->argument_count && ok; a++) {
      ok = ir_sroa_forward_read(function, &sources, i, &in->arguments[a], &forwarded);
    }
  }
  free(piece_writes.ents);
  free(temp_writes.ents);
  free(sources.ents);
  if (forwarded) {
    ir_sroa_drop_unread_pieces(function);
    if (changed) {
      *changed = 1;
    }
  }
  return ok;
}

static size_t ir_sroa_struct_bytes(const IRFunction *function, const char *name) {
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (in->op == IR_OP_DECLARE_LOCAL && ir_operand_is_symbol(&in->dest) &&
        strcmp(in->dest.name, name) == 0) {
      const MtlcType *type = ir_sroa_declared_aggregate(in);
      return type && type->kind == MTLC_TYPE_STRUCT &&
                     !ir_sroa_text_is_descriptor(in->text)
                 ? type->size
                 : 0;
    }
  }
  if (function->parameter_types && ir_optimize_get_program()) {
    for (size_t p = 0; p < function->parameter_count; p++) {
      if (function->parameter_names[p] &&
          strcmp(function->parameter_names[p], name) == 0) {
        const MtlcType *type = ir_program_lookup_type(ir_optimize_get_program(),
                                                      function->parameter_types[p]);
        return type && type->kind == MTLC_TYPE_STRUCT &&
                       !ir_sroa_text_is_descriptor(function->parameter_types[p])
                   ? type->size
                   : 0;
      }
    }
  }
  return 0;
}

static int ir_sroa_name_whole_copies(IRFunction *function, int *changed) {
  IRSroaHash addresses = {0};
  if (!ir_sroa_hash_init(&addresses, function->instruction_count)) {
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (in->op == IR_OP_ADDRESS_OF && ir_operand_is_symbol(&in->lhs) &&
        ir_operand_is_temp(&in->dest)) {
      ir_sroa_hash_put(&addresses, in->dest.name, i, 0);
    }
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (!ir_operand_is_temp(&in->dest) || in->op == IR_OP_STORE) {
      continue;
    }
    const IRSroaHashEnt *known = ir_sroa_hash_get(&addresses, in->dest.name);
    if (known && known->member != i) {
      ((IRSroaHashEnt *)known)->offset = -1;
    }
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *in = &function->instructions[i];
    if (in->op != IR_OP_STORE || in->is_volatile ||
        !ir_operand_is_temp(&in->dest) || !ir_operand_is_temp(&in->lhs) ||
        in->rhs.kind != IR_OPERAND_INT || in->rhs.int_value <= 8) {
      continue;
    }
    const IRSroaHashEnt *to = ir_sroa_hash_get(&addresses, in->dest.name);
    const IRSroaHashEnt *from = ir_sroa_hash_get(&addresses, in->lhs.name);
    if (!to || !from || to->offset != 0 || from->offset != 0) {
      continue;
    }
    const char *target = function->instructions[to->member].lhs.name;
    const char *source = function->instructions[from->member].lhs.name;
    if (strcmp(target, source) == 0 ||
        (long long)ir_sroa_struct_bytes(function, target) != in->rhs.int_value ||
        (long long)ir_sroa_struct_bytes(function, source) != in->rhs.int_value) {
      continue;
    }
    IRInstruction named = {0};
    named.op = IR_OP_ASSIGN;
    named.location = in->location;
    named.dest = ir_operand_symbol(target);
    named.lhs = ir_operand_symbol(source);
    if (!named.dest.name || !named.lhs.name) {
      ir_instruction_destroy_storage(&named);
      free(addresses.ents);
      return 0;
    }
    ir_instruction_destroy_storage(in);
    *in = named;
    if (changed) {
      *changed = 1;
    }
  }
  free(addresses.ents);
  return 1;
}

int ir_sroa_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }

  if (!ir_sroa_name_whole_copies(function, changed)) {
    return 0;
  }
  int split = 0;
  for (int iter = 0; iter < 32; iter++) {
    if (!ir_sroa_round(function, &split, iter)) {
      break;
    }
  }
  if (split) {
    if (changed) {
      *changed = 1;
    }
    ir_sroa_drop_unread_pieces(function);
  }
  return 1;
}

typedef struct {
  char *local;
  int bytes;
  int is_unsigned;
  size_t decl_index;
  size_t address_index;
  char *address_temp;
  char *alias_symbol;
  size_t alias_index;
  int eligible;
} IRDemoteCandidate;

static int ir_demote_name_matches(const IROperand *operand, IROperandKind kind,
                                  const char *name) {
  return operand && name && operand->kind == kind && operand->name &&
         strcmp(operand->name, name) == 0;
}

static int ir_demote_operand_mentions(const IRInstruction *in,
                                      IROperandKind kind, const char *name) {
  if (ir_demote_name_matches(&in->dest, kind, name) ||
      ir_demote_name_matches(&in->lhs, kind, name) ||
      ir_demote_name_matches(&in->rhs, kind, name)) {
    return 1;
  }
  for (size_t a = 0; a < in->argument_count; a++) {
    if (ir_demote_name_matches(&in->arguments[a], kind, name)) {
      return 1;
    }
  }
  return 0;
}

static int ir_demote_access_ok(const IRInstruction *in, IROperandKind kind,
                               const char *pointer, int bytes) {
  if (in->is_volatile || in->rhs.kind != IR_OPERAND_INT ||
      in->rhs.int_value != bytes || in->is_float) {
    return 0;
  }
  if (in->op == IR_OP_LOAD) {
    return ir_demote_name_matches(&in->lhs, kind, pointer) &&
           (in->dest.kind == IR_OPERAND_TEMP ||
            in->dest.kind == IR_OPERAND_SYMBOL);
  }
  if (in->op == IR_OP_STORE) {
    return ir_demote_name_matches(&in->dest, kind, pointer) &&
           (in->lhs.kind == IR_OPERAND_TEMP ||
            in->lhs.kind == IR_OPERAND_SYMBOL ||
            in->lhs.kind == IR_OPERAND_INT) &&
           !ir_demote_name_matches(&in->lhs, kind, pointer);
  }
  return 0;
}

static int ir_demote_uses_are_direct(const IRFunction *function,
                                     IROperandKind kind, const char *pointer,
                                     size_t def_index, int bytes,
                                     size_t *alias_out) {
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (i == def_index || in->op == IR_OP_NOP ||
        in->op == IR_OP_DECLARE_LOCAL ||
        !ir_demote_operand_mentions(in, kind, pointer)) {
      continue;
    }
    if (ir_demote_access_ok(in, kind, pointer, bytes)) {
      continue;
    }
    if (kind == IR_OPERAND_TEMP && alias_out && *alias_out == SIZE_MAX &&
        in->op == IR_OP_ASSIGN && in->dest.kind == IR_OPERAND_SYMBOL &&
        in->dest.name && ir_demote_name_matches(&in->lhs, kind, pointer)) {
      *alias_out = i;
      continue;
    }
    return 0;
  }
  return 1;
}

static int ir_demote_symbol_written_once(const IRFunction *function,
                                         const char *symbol, size_t at) {
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (i == at || in->op == IR_OP_NOP) {
      continue;
    }
    if (in->op == IR_OP_DECLARE_LOCAL) {
      continue;
    }
    if (ir_instruction_writes_destination(in) &&
        ir_demote_name_matches(&in->dest, IR_OPERAND_SYMBOL, symbol)) {
      return 0;
    }
    if (in->op == IR_OP_ADDRESS_OF &&
        ir_demote_name_matches(&in->lhs, IR_OPERAND_SYMBOL, symbol)) {
      return 0;
    }
  }
  return 1;
}

static int ir_demote_rewrite_access(IRInstruction *in, const char *local,
                                    int is_unsigned, int *changed) {
  if (in->op == IR_OP_LOAD) {
    IROperand source = ir_operand_symbol(local);
    int keep_unsigned = in->is_unsigned;
    MtlcType *keep_type = in->value_type;
    if (!source.name || !ir_rewrite_to_assign_operand(in, &source, changed)) {
      ir_operand_destroy(&source);
      return 0;
    }
    ir_operand_destroy(&source);
    in->is_unsigned = keep_unsigned;
    in->value_type = keep_type;
    return 1;
  }
  IROperand value = ir_operand_copy(&in->lhs);
  IROperand target = ir_operand_symbol(local);
  if (!target.name) {
    ir_operand_destroy(&value);
    return 0;
  }
  ir_instruction_destroy_storage(in);
  memset(in, 0, sizeof(*in));
  in->op = IR_OP_ASSIGN;
  in->dest = target;
  in->lhs = value;
  in->is_unsigned = is_unsigned;
  if (changed) {
    *changed = 1;
  }
  return 1;
}

int ir_demote_scalar_addresses_pass(IRFunction *function, int *changed) {
  if (!function || function->instruction_count == 0) {
    return 1;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *addr = &function->instructions[i];
    if (addr->op != IR_OP_ADDRESS_OF || addr->lhs.kind != IR_OPERAND_SYMBOL ||
        !addr->lhs.name || addr->dest.kind != IR_OPERAND_TEMP ||
        !addr->dest.name) {
      continue;
    }
    const char *type_name =
        ir_function_local_declared_type(function, addr->lhs.name);
    int bits = 0;
    int is_unsigned = 0;
    if (!type_name || !ir_int_type_name_info(type_name, &bits, &is_unsigned) ||
        bits < 8 || bits > 64) {
      continue;
    }
    int bytes = bits / 8;
    size_t other_address = SIZE_MAX;
    for (size_t k = 0; k < function->instruction_count; k++) {
      const IRInstruction *in = &function->instructions[k];
      if (k != i && in->op == IR_OP_ADDRESS_OF &&
          ir_demote_name_matches(&in->lhs, IR_OPERAND_SYMBOL, addr->lhs.name)) {
        other_address = k;
        break;
      }
    }
    if (other_address != SIZE_MAX) {
      continue;
    }
    size_t alias_at = SIZE_MAX;
    if (!ir_demote_uses_are_direct(function, IR_OPERAND_TEMP, addr->dest.name,
                                   i, bytes, &alias_at)) {
      continue;
    }
    const char *alias_symbol = NULL;
    if (alias_at != SIZE_MAX) {
      alias_symbol = function->instructions[alias_at].dest.name;
      if (!ir_demote_symbol_written_once(function, alias_symbol, alias_at) ||
          !ir_demote_uses_are_direct(function, IR_OPERAND_SYMBOL, alias_symbol,
                                     alias_at, bytes, NULL)) {
        continue;
      }
    }
    for (size_t k = 0; k < function->instruction_count; k++) {
      IRInstruction *in = &function->instructions[k];
      if (in->op != IR_OP_LOAD && in->op != IR_OP_STORE) {
        continue;
      }
      if (ir_demote_access_ok(in, IR_OPERAND_TEMP, addr->dest.name, bytes) ||
          (alias_symbol &&
           ir_demote_access_ok(in, IR_OPERAND_SYMBOL, alias_symbol, bytes))) {
        if (!ir_demote_rewrite_access(in, addr->lhs.name, is_unsigned,
                                      changed)) {
          return 0;
        }
      }
    }
    if (alias_at != SIZE_MAX) {
      ir_instruction_make_nop(&function->instructions[alias_at]);
    }
    ir_instruction_make_nop(&function->instructions[i]);
    if (changed) {
      *changed = 1;
    }
  }
  return 1;
}
