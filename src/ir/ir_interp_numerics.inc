static NumTerm ii_sym_const_of(IRInterpMachine *machine,
                               const IRInterpValue *value, unsigned width,
                               int as_float) {
  if (as_float) {
    double number = value->is_float ? value->f : (double)value->i;
    if (width == 32) {
      float single = (float)number;
      uint32_t bits;
      memcpy(&bits, &single, 4);
      return num_const(machine->num, 32, bits);
    }
    {
      uint64_t bits;
      memcpy(&bits, &number, 8);
      return num_const(machine->num, 64, bits);
    }
  }
  if (value->is_float) {
    ii_misuse();
    return num_const(machine->num, width ? width : 64, 0);
  }
  return num_const(machine->num, width ? width : 64, (uint64_t)value->i);
}

static NumTerm ii_sym_term(IRInterpMachine *machine, const IRInterpValue *value,
                           unsigned width, int as_float) {
  NumTerm term;
  unsigned have;
  if (!value->term) {
    return ii_sym_const_of(machine, value, width, as_float);
  }
  term = value->term;
  have = num_width(machine->num, term);
  if (as_float && !value->is_float) {
    return num_op(machine->num, NUM_CVT, width == 32 ? 32 : 64,
                  width == 32 ? NUM_CVT_S64_TO_F32 : NUM_CVT_S64_TO_F64,
                  &term, 1);
  }
  if (!as_float && value->is_float) {
    ii_misuse();
    return term;
  }
  if (!width || have == width) {
    return term;
  }
  if (as_float) {
    if (have == 32 && width == 64) {
      return num_op(machine->num, NUM_CVT, 64, NUM_CVT_F32_TO_F64, &term, 1);
    }
    if (have == 64 && width == 32) {
      return num_op(machine->num, NUM_CVT, 32, NUM_CVT_F64_TO_F32, &term, 1);
    }
    return term;
  }
  return num_resize(machine->num, term, width, 1);
}

static IRInterpValue ii_sym_value(IRInterpMachine *machine, NumTerm term,
                                  int is_float) {
  IRInterpValue value = is_float ? ii_float_value(0.0) : ii_int_value(0);
  uint64_t bits;
  if (!term) {
    ii_misuse();
    return value;
  }
  if (num_constant(machine->num, term, &bits)) {
    unsigned width = num_width(machine->num, term);
    if (is_float) {
      if (width == 32) {
        uint32_t word = (uint32_t)bits;
        float single;
        memcpy(&single, &word, 4);
        return ii_float_value((double)single);
      }
      if (width == 64) {
        double number;
        memcpy(&number, &bits, 8);
        return ii_float_value(number);
      }
    } else {
      if (width < 64 && (bits & (1ULL << (width - 1)))) {
        bits |= ~((1ULL << width) - 1ULL);
      }
      return ii_int_value((long long)bits);
    }
  }
  value.term = term;
  return value;
}

static int ii_sym_range(IIBuffer *buf, long long offset, long long length) {
  if (!buf || !buf->shadow) {
    return 0;
  }
  for (long long i = 0; i < length; i++) {
    if (buf->shadow[offset + i]) {
      return 1;
    }
  }
  return 0;
}

static void ii_sym_clear(IIBuffer *buf, long long offset, long long length) {
  if (buf && buf->shadow && length > 0) {
    memset(buf->shadow + offset, 0, (size_t)length * sizeof(uint32_t));
  }
}

static int ii_sym_shadow(IIBuffer *buf) {
  if (!buf->shadow) {
    buf->shadow = (uint32_t *)calloc((size_t)(buf->size > 0 ? buf->size : 1),
                                     sizeof(uint32_t));
  }
  return buf->shadow != NULL;
}

static NumTerm ii_sym_read_bytes(IRInterpMachine *machine, IIBuffer *buf,
                                 long long offset, int size) {
  NumTerm bytes[8];
  for (int i = 0; i < size; i++) {
    uint32_t shadow = buf->shadow ? buf->shadow[offset + i] : 0;
    bytes[i] = shadow ? shadow
                      : num_const(machine->num, 8, buf->data[offset + i]);
  }
  return num_concat(machine->num, bytes, (unsigned)size);
}

static int ii_sym_write_bytes(IRInterpMachine *machine, IIBuffer *buf,
                              long long offset, int size, NumTerm term) {
  if (!ii_sym_shadow(buf)) {
    ii_fail(machine, IR_INTERP_TRAP, "out of memory tracking symbolic bytes");
    return 0;
  }
  for (int i = 0; i < size; i++) {
    NumTerm byte = num_byte(machine->num, term, (unsigned)i);
    uint64_t bits;
    if (!byte) {
      ii_misuse();
      return 1;
    }
    if (num_constant(machine->num, byte, &bits)) {
      buf->data[offset + i] = (unsigned char)bits;
      buf->shadow[offset + i] = 0;
    } else {
      buf->data[offset + i] = II_POISON_BYTE;
      buf->shadow[offset + i] = byte;
    }
  }
  ii_mark_bytes(buf, offset, size, 1);
  return 1;
}

static int ii_sym_load(IRInterpMachine *machine, unsigned long long addr,
                       int size, int is_float, int is_unsigned,
                       unsigned char alias, IRInterpValue *out,
                       int *handled) {
  long long offset = 0;
  IIBuffer *buf;
  NumTerm term;
  *handled = 0;
  if (!machine->num) {
    return 1;
  }
  buf = ii_addr_to_buffer(machine, addr, size, &offset);
  if (!buf || !ii_sym_range(buf, offset, size)) {
    return 1;
  }
  if (buf->pending && !ii_pending_check(machine, buf, offset, size)) {
    return 0;
  }
  term = ii_sym_read_bytes(machine, buf, offset, size);
  if (is_float) {
    if (size == 2) {
      term = num_op(machine->num, NUM_CVT, 32,
                    alias == IR_ALIAS_CLASS_BF16 ? NUM_CVT_BF16_TO_F32
                                                 : NUM_CVT_F16_TO_F32,
                    &term, 1);
    } else if (size != 4 && size != 8) {
      ii_misuse();
    }
    *out = ii_sym_value(machine, term, 1);
  } else {
    term = num_resize(machine->num, term, 64, !is_unsigned);
    *out = ii_sym_value(machine, term, 0);
  }
  *handled = 1;
  return 1;
}

static int ii_sym_store(IRInterpMachine *machine, unsigned long long addr,
                        int size, int is_float, unsigned char alias,
                        const IRInterpValue *value, int *handled) {
  long long offset = 0;
  IIBuffer *buf;
  NumTerm term;
  *handled = 0;
  if (!machine->num) {
    return 1;
  }
  buf = ii_addr_to_buffer(machine, addr, size, &offset);
  if (!buf) {
    return 1;
  }
  if (!value->term) {
    ii_sym_clear(buf, offset, size);
    return 1;
  }
  if (is_float || value->is_float) {
    unsigned width = size == 8 ? 64 : 32;
    term = ii_sym_term(machine, value, width, 1);
    if (size == 2) {
      term = num_op(machine->num, NUM_CVT, 16,
                    alias == IR_ALIAS_CLASS_BF16 ? NUM_CVT_F32_TO_BF16
                                                 : NUM_CVT_F32_TO_F16,
                    &term, 1);
    } else if (size != 4 && size != 8) {
      ii_misuse();
      return 1;
    }
  } else {
    term = num_resize(machine->num, value->term, (unsigned)size * 8u, 0);
  }
  *handled = 1;
  return ii_sym_write_bytes(machine, buf, offset, size, term);
}

static int ii_sym_copy(IRInterpMachine *machine, unsigned long long to,
                       unsigned long long from, long long length) {
  long long to_offset = 0, from_offset = 0;
  IIBuffer *source = ii_addr_to_buffer(machine, from, length, &from_offset);
  IIBuffer *dest = ii_addr_to_buffer(machine, to, length, &to_offset);
  if (!source || !dest) {
    return 1;
  }
  if (!ii_sym_range(source, from_offset, length)) {
    ii_sym_clear(dest, to_offset, length);
    return 1;
  }
  if (!ii_sym_shadow(dest)) {
    ii_fail(machine, IR_INTERP_TRAP, "out of memory tracking symbolic bytes");
    return 0;
  }
  memmove(dest->shadow + to_offset, source->shadow + from_offset,
          (size_t)length * sizeof(uint32_t));
  return 1;
}

static NumCompare ii_sym_compare(const char *op, int is_unsigned) {
  if (strcmp(op, "==") == 0) return NUM_CMP_EQ;
  if (strcmp(op, "!=") == 0) return NUM_CMP_NE;
  if (strcmp(op, "<") == 0) return is_unsigned ? NUM_CMP_ULT : NUM_CMP_LT;
  if (strcmp(op, "<=") == 0) return is_unsigned ? NUM_CMP_ULE : NUM_CMP_LE;
  if (strcmp(op, ">") == 0) return is_unsigned ? NUM_CMP_UGT : NUM_CMP_GT;
  if (strcmp(op, ">=") == 0) return is_unsigned ? NUM_CMP_UGE : NUM_CMP_GE;
  return (NumCompare)0;
}

static NumTerm ii_sym_truth(IRInterpMachine *machine, NumTerm term) {
  NumTerm args[2];
  args[0] = term;
  args[1] = num_const(machine->num, num_width(machine->num, term), 0);
  return num_op(machine->num, NUM_ICMP, 64, NUM_CMP_NE, args, 2);
}

static int ii_sym_binary(IRInterpMachine *machine, const IRInstruction *insn,
                         const IRInterpValue *a, const IRInterpValue *b,
                         IRInterpValue *out) {
  const char *op = insn->text ? insn->text : "?";
  NumTerm args[2];
  NumTerm term = 0;
  if (insn->is_float) {
    unsigned width = insn->float_bits == 32 ? 32 : 64;
    NumOp code = (NumOp)0;
    NumCompare compare;
    args[0] = ii_sym_term(machine, a, width, 1);
    args[1] = ii_sym_term(machine, b, width, 1);
    if (strcmp(op, "+") == 0) code = NUM_FADD;
    else if (strcmp(op, "-") == 0) code = NUM_FSUB;
    else if (strcmp(op, "*") == 0) code = NUM_FMUL;
    else if (strcmp(op, "/") == 0) code = NUM_FDIV;
    if (code) {
      term = num_op(machine->num, code, width, 0, args, 2);
      *out = ii_sym_value(machine, term, 1);
      return 1;
    }
    compare = ii_sym_compare(op, 0);
    if (compare) {
      term = num_op(machine->num, NUM_FCMP, 64, compare, args, 2);
      *out = ii_sym_value(machine, term, 0);
      return 1;
    }
    ii_misuse();
    *out = ii_int_value(0);
    return 1;
  }
  args[0] = ii_sym_term(machine, a, 64, 0);
  args[1] = ii_sym_term(machine, b, 64, 0);
  {
    NumOp code = (NumOp)0;
    NumCompare compare = ii_sym_compare(op, insn->is_unsigned);
    if (strcmp(op, "+") == 0) code = NUM_ADD;
    else if (strcmp(op, "-") == 0) code = NUM_SUB;
    else if (strcmp(op, "*") == 0) code = NUM_MUL;
    else if (strcmp(op, "&") == 0) code = NUM_AND;
    else if (strcmp(op, "|") == 0) code = NUM_OR;
    else if (strcmp(op, "^") == 0) code = NUM_XOR;
    else if (strcmp(op, "<<") == 0) code = NUM_SHL;
    else if (strcmp(op, ">>") == 0) code = insn->is_unsigned ? NUM_LSHR : NUM_ASHR;
    else if (strcmp(op, "/") == 0) code = insn->is_unsigned ? NUM_UDIV : NUM_SDIV;
    else if (strcmp(op, "%") == 0) code = insn->is_unsigned ? NUM_UREM : NUM_SREM;
    if (code) {
      term = num_op(machine->num, code, 64, 0, args, 2);
    } else if (compare) {
      term = num_op(machine->num, NUM_ICMP, 64, compare, args, 2);
    } else if (strcmp(op, "&&") == 0 || strcmp(op, "||") == 0) {
      NumTerm truths[2];
      truths[0] = ii_sym_truth(machine, args[0]);
      truths[1] = ii_sym_truth(machine, args[1]);
      term = num_op(machine->num, op[0] == '&' ? NUM_AND : NUM_OR, 64, 0,
                    truths, 2);
    } else {
      ii_misuse();
      *out = ii_int_value(0);
      return 1;
    }
  }
  *out = ii_sym_value(machine, term, 0);
  return 1;
}

static int ii_sym_unary(IRInterpMachine *machine, const IRInstruction *insn,
                        const IRInterpValue *a, IRInterpValue *out) {
  const char *op = insn->text ? insn->text : "?";
  NumTerm term;
  if (strcmp(op, "+") == 0) {
    *out = *a;
    return 1;
  }
  if (strcmp(op, "-") == 0) {
    if (insn->is_float || a->is_float) {
      unsigned width = insn->float_bits == 32 ? 32
                       : num_width(machine->num, a->term) == 32 ? 32
                                                                : 64;
      NumTerm arg = ii_sym_term(machine, a, width, 1);
      term = num_op(machine->num, NUM_FNEG, width, 0, &arg, 1);
      *out = ii_sym_value(machine, term, 1);
      return 1;
    }
    {
      NumTerm args[2];
      args[0] = num_const(machine->num, 64, 0);
      args[1] = ii_sym_term(machine, a, 64, 0);
      *out = ii_sym_value(machine, num_op(machine->num, NUM_SUB, 64, 0, args, 2), 0);
      return 1;
    }
  }
  if (strcmp(op, "!") == 0) {
    NumTerm args[2];
    if (a->is_float) {
      unsigned width = num_width(machine->num, a->term) == 32 ? 32 : 64;
      args[0] = a->term;
      args[1] = num_const(machine->num, width, 0);
      term = num_op(machine->num, NUM_FCMP, 64, NUM_CMP_EQ, args, 2);
    } else {
      args[0] = a->term;
      args[1] = num_const(machine->num, 64, 0);
      term = num_op(machine->num, NUM_ICMP, 64, NUM_CMP_EQ, args, 2);
    }
    *out = ii_sym_value(machine, term, 0);
    return 1;
  }
  if (strcmp(op, "~") == 0 && !a->is_float) {
    NumTerm args[2];
    args[0] = a->term;
    args[1] = num_const(machine->num, 64, ~0ULL);
    *out = ii_sym_value(machine, num_op(machine->num, NUM_XOR, 64, 0, args, 2), 0);
    return 1;
  }
  ii_misuse();
  *out = ii_int_value(0);
  return 1;
}

static int ii_sym_cast(IRInterpMachine *machine, const IRInstruction *insn,
                       const IRInterpValue *in, IRInterpValue *out) {
  const char *type = insn->text ? insn->text : "";
  int size = 0, target_unsigned = 0;
  NumTerm term;
  if (strcmp(type, "float32") == 0 || strcmp(type, "float64") == 0) {
    unsigned width = type[5] == '3' ? 32 : 64;
    if (!in->is_float) {
      NumTerm source = in->term;
      term = num_op(machine->num, NUM_CVT, width,
                    insn->is_unsigned
                        ? (width == 32 ? NUM_CVT_U64_TO_F32 : NUM_CVT_U64_TO_F64)
                        : (width == 32 ? NUM_CVT_S64_TO_F32 : NUM_CVT_S64_TO_F64),
                    &source, 1);
    } else {
      term = ii_sym_term(machine, in, width, 1);
    }
    *out = ii_sym_value(machine, term, 1);
    return 1;
  }
  if (strcmp(type, "float16") == 0 || strcmp(type, "bfloat16") == 0) {
    int brain = type[0] == 'b';
    NumTerm single = ii_sym_term(machine, in, 32, 1);
    NumTerm half = num_op(machine->num, NUM_CVT, 16,
                          brain ? NUM_CVT_F32_TO_BF16 : NUM_CVT_F32_TO_F16,
                          &single, 1);
    term = num_op(machine->num, NUM_CVT, 32,
                  brain ? NUM_CVT_BF16_TO_F32 : NUM_CVT_F16_TO_F32, &half, 1);
    *out = ii_sym_value(machine, term, 1);
    return 1;
  }
  if (strcmp(type, "bool") == 0) {
    *out = ii_sym_value(machine, ii_sym_truth(machine, in->term), 0);
    return 1;
  }
  if (!ii_integer_cast_width(type, &size, &target_unsigned) || in->is_float) {
    ii_misuse();
    *out = ii_int_value(0);
    return 1;
  }
  term = num_resize(machine->num, in->term, (unsigned)size * 8u, 0);
  term = num_resize(machine->num, term, 64, !target_unsigned);
  *out = ii_sym_value(machine, term, 0);
  return 1;
}

static int ii_sym_select_values(IRInterpMachine *machine, NumTerm cond,
                                const IRInterpValue *yes,
                                const IRInterpValue *no, unsigned float_width,
                                IRInterpValue *out) {
  NumTerm args[3];
  int is_float = yes->is_float || no->is_float;
  unsigned width;
  if (!yes->term && !no->term && yes->is_float == no->is_float &&
      ((yes->is_float && yes->f == no->f &&
        signbit(yes->f) == signbit(no->f)) ||
       (!yes->is_float && yes->i == no->i))) {
    *out = *yes;
    return 1;
  }
  if (yes->term && no->term && yes->term == no->term) {
    *out = *yes;
    return 1;
  }
  if (is_float) {
    width = yes->term ? num_width(machine->num, yes->term)
            : no->term ? num_width(machine->num, no->term)
                       : float_width;
    if (width != 32 && width != 64) width = 64;
  } else {
    width = 64;
  }
  args[0] = cond;
  args[1] = ii_sym_term(machine, yes, width, is_float);
  args[2] = ii_sym_term(machine, no, width, is_float);
  *out = ii_sym_value(machine,
                      num_op(machine->num, NUM_SELECT, width, 0, args, 3),
                      is_float);
  return 1;
}

static unsigned ii_sym_var_float_width(const IIVar *var) {
  if (var->slotted) {
    return var->slot_size == 4 ? 32 : 64;
  }
  return var->value_size == -4 ? 32 : 64;
}

static int ii_sym_pure_op(const IRInstruction *insn) {
  switch (insn->op) {
  case IR_OP_ASSIGN:
  case IR_OP_BINARY:
  case IR_OP_UNARY:
  case IR_OP_CAST:
  case IR_OP_SELECT:
  case IR_OP_NOP:
    return insn->op == IR_OP_NOP ||
           insn->dest.kind == IR_OPERAND_TEMP ||
           insn->dest.kind == IR_OPERAND_SYMBOL;
  default:
    return 0;
  }
}

static int ii_exec_pure(IRInterpMachine *machine, IIFrame *frame,
                        const IRInstruction *insn);

#define II_SYM_MERGE_MAX 32

static int ii_sym_merge_branch(IRInterpMachine *machine, IIFrame *frame,
                               IRFunction *fn, size_t pc,
                               const IRInstruction *insn, NumTerm cond,
                               int taken_when_zero, size_t *next_pc) {
  const IILabel *label = insn->text ? ii_find_label(frame, insn->text) : NULL;
  size_t skip, then_begin = pc + 1, then_end, else_begin = 0, else_end = 0,
               end;
  IIVar *vars[II_SYM_MERGE_MAX];
  IRInterpValue before[II_SYM_MERGE_MAX], yes[II_SYM_MERGE_MAX],
      no[II_SYM_MERGE_MAX];
  size_t count = 0;
  if (!label || label->index <= pc) {
    return 0;
  }
  skip = label->index;
  then_end = skip;
  end = skip;
  for (size_t i = pc + 1; i < skip; i++) {
    const IRInstruction *step = &fn->instructions[i];
    const IILabel *join;
    if (step->op != IR_OP_JUMP) {
      continue;
    }
    join = step->text ? ii_find_label(frame, step->text) : NULL;
    for (size_t k = i + 1; k < skip; k++) {
      if (fn->instructions[k].op != IR_OP_LABEL) {
        return 0;
      }
    }
    if (!join || join->index < skip) {
      return 0;
    }
    then_end = i;
    if (join->index > skip) {
      else_begin = skip + 1;
      else_end = join->index;
    }
    end = join->index;
    break;
  }
  for (size_t i = then_begin; i < then_end; i++) {
    if (!ii_sym_pure_op(&fn->instructions[i])) return 0;
  }
  for (size_t i = else_begin; i < else_end; i++) {
    if (!ii_sym_pure_op(&fn->instructions[i])) return 0;
  }
  for (int arm = 0; arm < 2; arm++) {
    size_t begin = arm ? else_begin : then_begin;
    size_t stop = arm ? else_end : then_end;
    for (size_t i = begin; i < stop; i++) {
      const IRInstruction *step = &fn->instructions[i];
      IIVar *var;
      size_t k;
      if (step->op == IR_OP_NOP) continue;
      var = ii_resolve(machine, frame, &step->dest);
      if (!var) {
        ii_fail(machine, IR_INTERP_TRAP, "out of memory");
        return -1;
      }
      for (k = 0; k < count && vars[k] != var; k++) {
      }
      if (k == count) {
        if (count == II_SYM_MERGE_MAX) return 0;
        vars[count++] = var;
      }
    }
  }
  for (size_t k = 0; k < count; k++) {
    if (!ii_var_read(machine, vars[k], &before[k])) return -1;
  }
  for (int arm = 0; arm < 2; arm++) {
    size_t begin = arm ? else_begin : then_begin;
    size_t stop = arm ? else_end : then_end;
    for (size_t i = begin; i < stop; i++) {
      if (!ii_exec_pure(machine, frame, &fn->instructions[i])) return -1;
    }
    for (size_t k = 0; k < count; k++) {
      if (!ii_var_read(machine, vars[k], arm ? &no[k] : &yes[k])) return -1;
      if (!ii_var_write(machine, vars[k], &before[k])) return -1;
    }
  }
  for (size_t k = 0; k < count; k++) {
    IRInterpValue merged;
    const IRInterpValue *if_true = taken_when_zero ? &yes[k] : &no[k];
    const IRInterpValue *if_false = taken_when_zero ? &no[k] : &yes[k];
    if (!ii_sym_select_values(machine, cond, if_true, if_false,
                              ii_sym_var_float_width(vars[k]), &merged) ||
        !ii_var_write(machine, vars[k], &merged)) {
      return -1;
    }
  }
  *next_pc = end;
  return 1;
}

static uint32_t ii_sym_next_input(IRInterpMachine *machine) {
  return machine->num_inputs++;
}

static int ii_sym_extern(IRInterpMachine *machine, const char *name,
                         const IRInterpValue *args, size_t count,
                         IRInterpValue *out, int *handled) {
  NumTerm x, k[2], term;
  *handled = 0;
  if (!machine->num || count < 1) {
    return 1;
  }
  {
    int symbolic = 0;
    for (size_t i = 0; i < count; i++) symbolic |= args[i].term != 0;
    if (!symbolic) return 1;
  }
  *handled = 1;
  x = ii_sym_term(machine, &args[0], 32, 1);
  if (strcmp(name, "expf") == 0) {
    k[0] = x;
    k[1] = num_const(machine->num, 32, 0x3FB8AA3Bu);
    term = num_op(machine->num, NUM_FMUL, 32, 0, k, 2);
    term = num_op(machine->num, NUM_APPROX, 32, NUM_APPROX_EX2, &term, 1);
  } else if (strcmp(name, "logf") == 0) {
    term = num_op(machine->num, NUM_APPROX, 32, NUM_APPROX_LG2, &x, 1);
    k[0] = term;
    k[1] = num_const(machine->num, 32, 0x3F317218u);
    term = num_op(machine->num, NUM_FMUL, 32, 0, k, 2);
  } else if (strcmp(name, "sqrtf") == 0) {
    term = num_op(machine->num, NUM_FSQRT, 32, 0, &x, 1);
  } else if (strcmp(name, "rsqrtf") == 0) {
    term = num_op(machine->num, NUM_APPROX, 32, NUM_APPROX_RSQRT, &x, 1);
  } else if (strcmp(name, "fabsf") == 0) {
    term = num_op(machine->num, NUM_FABS, 32, 0, &x, 1);
  } else if (strcmp(name, "sinf") == 0) {
    term = num_op(machine->num, NUM_APPROX, 32, NUM_APPROX_SIN, &x, 1);
  } else if (strcmp(name, "cosf") == 0) {
    term = num_op(machine->num, NUM_APPROX, 32, NUM_APPROX_COS, &x, 1);
  } else if ((strcmp(name, "fmaxf") == 0 || strcmp(name, "fminf") == 0) &&
             count == 2) {
    k[0] = x;
    k[1] = ii_sym_term(machine, &args[1], 32, 1);
    term = num_op(machine->num, name[1] == 'm' && name[2] == 'a' ? NUM_FMAX
                                                                 : NUM_FMIN,
                  32, 0, k, 2);
  } else {
    *handled = 0;
    return 1;
  }
  *out = ii_sym_value(machine, term, 1);
  return 1;
}

static int ii_sym_device_call(IRInterpMachine *machine,
                              MtlcIntrinsic intrinsic,
                              const IRInterpValue *args, size_t count,
                              IRInterpValue *out, int *handled) {
  NumTerm term, parts[2];
  int symbolic = 0;
  *handled = 0;
  if (!machine->num) return 1;
  for (size_t i = 0; i < count; i++) symbolic |= args[i].term != 0;
  if (!symbolic) return 1;
  *handled = 1;
  switch (intrinsic) {
  case MTLC_INTRINSIC_GPU_F16_BITS_TO_F32:
  case MTLC_INTRINSIC_GPU_H2F_LO:
  case MTLC_INTRINSIC_GPU_H2F_HI: {
    NumTerm bits = ii_sym_term(machine, &args[0], 64, 0);
    NumTerm bytes[2];
    unsigned shift = intrinsic == MTLC_INTRINSIC_GPU_H2F_HI ? 2 : 0;
    bytes[0] = num_byte(machine->num, bits, shift);
    bytes[1] = num_byte(machine->num, bits, shift + 1);
    term = num_concat(machine->num, bytes, 2);
    term = num_op(machine->num, NUM_CVT, 32, NUM_CVT_F16_TO_F32, &term, 1);
    *out = ii_sym_value(machine, term, 1);
    return 1;
  }
  case MTLC_INTRINSIC_GPU_F32_TO_F16_BITS:
    term = ii_sym_term(machine, &args[0], 32, 1);
    term = num_op(machine->num, NUM_CVT, 16, NUM_CVT_F32_TO_F16, &term, 1);
    *out = ii_sym_value(machine, num_resize(machine->num, term, 64, 0), 0);
    return 1;
  case MTLC_INTRINSIC_GPU_F2H2:
    parts[0] = ii_sym_term(machine, &args[0], 32, 1);
    parts[1] = ii_sym_term(machine, &args[1], 32, 1);
    for (int i = 0; i < 2; i++) {
      parts[i] = num_op(machine->num, NUM_CVT, 16, NUM_CVT_F32_TO_F16,
                        &parts[i], 1);
    }
    {
      NumTerm bytes[8];
      bytes[0] = num_byte(machine->num, parts[0], 0);
      bytes[1] = num_byte(machine->num, parts[0], 1);
      bytes[2] = num_byte(machine->num, parts[1], 0);
      bytes[3] = num_byte(machine->num, parts[1], 1);
      for (int i = 4; i < 8; i++) bytes[i] = num_const(machine->num, 8, 0);
      *out = ii_sym_value(machine, num_concat(machine->num, bytes, 8), 0);
    }
    return 1;
  case MTLC_INTRINSIC_GPU_F32_FROM_BITS:
    term = num_resize(machine->num, ii_sym_term(machine, &args[0], 64, 0), 32, 0);
    *out = ii_sym_value(machine, term, 1);
    return 1;
  case MTLC_INTRINSIC_GPU_WORKGROUP_UNIFORM_U32:
    *out = args[0];
    return 1;
  case MTLC_INTRINSIC_GPU_F32_TO_BITS:
    term = ii_sym_term(machine, &args[0], 32, 1);
    *out = ii_sym_value(machine, num_resize(machine->num, term, 64, 0), 0);
    return 1;
  default:
    *handled = 0;
    ii_misuse();
    return 1;
  }
}

static int ii_extern_numerics_harness(IRInterpMachine *machine,
                                      const char *name,
                                      const IRInterpValue *args,
                                      size_t arg_count,
                                      IRInterpValue *result) {
  if (strcmp(name, "numerics_input") == 0 && arg_count >= 1) {
    long long bytes = ii_as_int(&args[0]);
    unsigned long long addr;
    long long offset = 0;
    IIBuffer *buf;
    if (bytes <= 0 || bytes > II_MAX_BUFFER_SIZE) {
      ii_fail(machine, IR_INTERP_TRAP,
              "numerics_input: a size from 1 byte to 16 MB");
      return -1;
    }
    addr = ir_interp_add_buffer(machine, NULL, bytes);
    buf = addr ? ii_addr_to_buffer(machine, addr, bytes, &offset) : NULL;
    if (!buf) {
      ii_fail(machine, IR_INTERP_TRAP, "numerics_input: out of memory");
      return -1;
    }
    buf->alloc_line = machine->current_call_loc.line;
    if (machine->num) {
      uint32_t input = ii_sym_next_input(machine);
      if (!ii_sym_shadow(buf)) {
        ii_fail(machine, IR_INTERP_TRAP, "numerics_input: out of memory");
        return -1;
      }
      for (long long i = 0; i < bytes; i++) {
        buf->data[i] = II_POISON_BYTE;
        buf->shadow[i] = num_input(machine->num, input, (uint64_t)i);
      }
    } else {
      uint32_t state = 0x9E3779B9u ^ (uint32_t)machine->num_inputs++;
      for (long long i = 0; i < bytes; i++) {
        state = state * 1664525u + 1013904223u;
        buf->data[i] = (unsigned char)(state >> 24);
      }
    }
    *result = ii_int_value((long long)addr);
    return 1;
  }
  if (strcmp(name, "numerics_same") == 0 && arg_count >= 5) {
    unsigned long long left = (unsigned long long)ii_as_int(&args[0]);
    unsigned long long right = (unsigned long long)ii_as_int(&args[1]);
    long long rows = ii_as_int(&args[2]);
    long long cols = ii_as_int(&args[3]);
    long long ld = ii_as_int(&args[4]);
    int same = 1;
    IRInterpClaim claim;
    memset(&claim, 0, sizeof(claim));
    claim.line = machine->current_call_loc.line;
    claim.rows = rows;
    claim.cols = cols;
    for (long long r = 0; r < rows && same; r++) {
      for (long long c = 0; c < cols && same; c++) {
        unsigned long long at = (unsigned long long)((r * ld + c) * 4);
        long long lo = 0, ro = 0;
        IIBuffer *lb = ii_addr_to_buffer(machine, left + at, 4, &lo);
        IIBuffer *rb = ii_addr_to_buffer(machine, right + at, 4, &ro);
        if (!lb || !rb) {
          ii_fail(machine, IR_INTERP_TRAP,
                  "numerics_same reads outside its buffers");
          return -1;
        }
        if (machine->num) {
          NumTerm a = ii_sym_read_bytes(machine, lb, lo, 4);
          NumTerm b = ii_sym_read_bytes(machine, rb, ro, 4);
          IRInterpClaim element = claim;
          element.row = r;
          element.column = c;
          element.left = a;
          element.right = b;
          element.equal = a == b;
          if (machine->claim_hook) {
            machine->claim_hook(machine->claim_ctx, &element);
          }
          if (a != b) {
            same = 0;
            claim.row = r;
            claim.column = c;
            claim.left = a;
            claim.right = b;
          }
        } else {
          float x, y;
          memcpy(&x, lb->data + lo, 4);
          memcpy(&y, rb->data + ro, 4);
          if (memcmp(lb->data + lo, rb->data + ro, 4) != 0 &&
              !(x != x && y != y)) {
            same = 0;
            claim.row = r;
            claim.column = c;
          }
        }
      }
    }
    claim.equal = same;
    claim.summary = 1;
    if (machine->claim_hook) {
      machine->claim_hook(machine->claim_ctx, &claim);
    }
    *result = ii_int_value(same);
    return 1;
  }
  return II_EXTERN_UNHANDLED;
}

static void ii_sym_refuse(IRInterpMachine *machine, const IRInstruction *insn,
                          const IRFunction *fn, const char *what) {
  machine->num_site = insn;
  machine->num_site_function = fn ? fn->name : NULL;
  if (machine->status == IR_INTERP_OK) {
    machine->status = IR_INTERP_UNSUPPORTED;
    snprintf(machine->detail, sizeof(machine->detail), "line %llu of %s: %s",
             (unsigned long long)insn->location.line,
             fn && fn->name ? fn->name : "?", what);
  }
}

static int ii_sym_simd_touches(IRInterpMachine *machine, IIFrame *frame,
                               const IRInstruction *insn) {
  for (size_t i = 0; i < insn->argument_count; i++) {
    IRInterpValue value;
    long long offset = 0;
    IIBuffer *buf;
    if (insn->arguments[i].kind != IR_OPERAND_TEMP &&
        insn->arguments[i].kind != IR_OPERAND_SYMBOL) {
      continue;
    }
    if (!ii_fetch(machine, frame, &insn->arguments[i], &value)) {
      return 1;
    }
    if (value.term) {
      return 1;
    }
    if (value.is_float || value.i < (long long)II_ADDR_BASE) {
      continue;
    }
    buf = ii_addr_to_buffer(machine, (unsigned long long)value.i, 0, &offset);
    if (buf && buf->shadow && ii_sym_range(buf, 0, buf->size)) {
      return 1;
    }
  }
  return 0;
}

static int ii_sym_select_insn(IRInterpMachine *machine, IIFrame *frame,
                              const IRInstruction *insn, int *handled) {
  IRInterpValue a, b, yes, no, out;
  NumTerm cond;
  *handled = 0;
  if (insn->text && insn->argument_count > 1) {
    NumTerm args[2];
    NumCompare compare;
    if (!ii_fetch(machine, frame, &insn->lhs, &a) ||
        !ii_fetch(machine, frame, &insn->arguments[1], &b)) {
      return 0;
    }
    if (!a.term && !b.term) {
      return 1;
    }
    compare = ii_sym_compare(insn->text, insn->is_unsigned);
    if (!compare) {
      ii_misuse();
      return 1;
    }
    args[0] = ii_sym_term(machine, &a, 64, 0);
    args[1] = ii_sym_term(machine, &b, 64, 0);
    cond = num_op(machine->num, NUM_ICMP, 64, compare, args, 2);
  } else {
    if (!ii_fetch(machine, frame, &insn->lhs, &a)) {
      return 0;
    }
    if (!a.term) {
      return 1;
    }
    cond = ii_sym_truth(machine, a.term);
  }
  if (!ii_fetch(machine, frame, &insn->rhs, &yes) ||
      insn->argument_count < 1 ||
      !ii_fetch(machine, frame, &insn->arguments[0], &no)) {
    return 0;
  }
  if (!ii_sym_select_values(
          machine, cond, &yes, &no,
          insn->value_type && insn->value_type->kind == MTLC_TYPE_FLOAT32 ? 32
                                                                          : 64,
          &out)) {
    return 0;
  }
  *handled = 1;
  return ii_store_dest(machine, frame, &insn->dest, &out);
}

static int ii_op_assign(IRInterpMachine *machine, IIFrame *frame,
                        const IRInstruction *insn);
static int ii_op_unary(IRInterpMachine *machine, IIFrame *frame,
                       const IRInstruction *insn);

static int ii_exec_pure(IRInterpMachine *machine, IIFrame *frame,
                        const IRInstruction *insn) {
  IRInterpValue a, b, out;
  switch (insn->op) {
  case IR_OP_NOP:
    return 1;
  case IR_OP_ASSIGN:
    return ii_op_assign(machine, frame, insn);
  case IR_OP_BINARY:
    if (!ii_fetch(machine, frame, &insn->lhs, &a) ||
        !ii_fetch(machine, frame, &insn->rhs, &b) ||
        !ii_binary(machine, insn, &a, &b, &out)) {
      return 0;
    }
    return ii_store_dest(machine, frame, &insn->dest, &out);
  case IR_OP_UNARY:
    return ii_op_unary(machine, frame, insn);
  case IR_OP_CAST:
    if (!ii_fetch(machine, frame, &insn->lhs, &a) ||
        !ii_cast(machine, insn, &a, &out)) {
      return 0;
    }
    return ii_store_dest(machine, frame, &insn->dest, &out);
  case IR_OP_SELECT:
    return ii_op_select(machine, frame, insn);
  default:
    ii_fail(machine, IR_INTERP_UNSUPPORTED, "an impure instruction in a merged branch");
    return 0;
  }
}

void ir_interp_set_launch_hook(IRInterpMachine *machine,
                               IRInterpLaunchHook hook, void *ctx) {
  if (machine) {
    machine->launch_hook = hook;
    machine->launch_ctx = ctx;
  }
}

void ir_interp_enable_numerics(IRInterpMachine *machine, NumStore *store,
                               IRInterpClaimHook hook, void *ctx) {
  if (!machine) {
    return;
  }
  machine->num = store;
  machine->claim_hook = hook;
  machine->claim_ctx = ctx;
  g_ii_symbolic = store != NULL;
  g_ii_misuse = 0;
  g_ii_misuse_insn = NULL;
  g_ii_misuse_fn = NULL;
}

const IRInstruction *ir_interp_symbolic_site(const IRInterpMachine *machine,
                                             const char **function) {
  if (!machine) {
    return NULL;
  }
  if (function) {
    *function = machine->num_site_function;
  }
  return machine->num_site;
}
