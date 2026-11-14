static IRInstruction ptx_tile_view(const IRInstruction *in) {
  IRInstruction view = *in;
  if (in->argument_count) {
    view.dest = in->arguments[0];
    view.arguments = in->arguments + 1;
    view.argument_types = in->argument_types ? in->argument_types + 1 : NULL;
    view.argument_count = in->argument_count - 1;
  }
  return view;
}

static int ptx_tile_is_type(const MtlcType *type) {
  return ir_tile_operand_is_tile(type);
}

static PtxTile *ptx_tile_find(PtxFn *fn, const char *name) {
  for (size_t i = 0; name && i < fn->tile_count; i++) {
    if (strcmp(fn->tiles[i].name, name) == 0) {
      return &fn->tiles[i];
    }
  }
  return NULL;
}

static PtxTile *ptx_tile_operand(PtxFn *fn, const IROperand *op) {
  if (!op || !op->name ||
      (op->kind != IR_OPERAND_TEMP && op->kind != IR_OPERAND_SYMBOL)) {
    return NULL;
  }
  return ptx_tile_find(fn, op->name);
}

static PtxClass ptx_tile_class(const MtlcType *type) {
  if (type->view_layout == MTLC_VIEW_LAYOUT_FRAGMENT_A ||
      type->view_layout == MTLC_VIEW_LAYOUT_FRAGMENT_B) {
    return PC_B32;
  }
  switch (type->base_type ? type->base_type->kind : MTLC_TYPE_VOID) {
  case MTLC_TYPE_FLOAT32: return PC_F32;
  case MTLC_TYPE_BOOL: return PC_PRED;
  case MTLC_TYPE_INT32: return PC_B32;
  default: return PC_NONE;
  }
}

static int ptx_tile_register_count(const MtlcType *type) {
  int rows = (int)type->view_extents[0];
  int cols = (int)type->view_extents[1];
  if (!cols) {
    return rows / 8;
  }
  switch (type->view_layout) {
  case MTLC_VIEW_LAYOUT_FRAGMENT_A: return (rows / 16) * (cols / 16) * 4;
  case MTLC_VIEW_LAYOUT_FRAGMENT_B: return (rows / 16) * (cols / 8) * 2;
  default: return (rows / 16) * (cols / 8) * 4;
  }
}

static void ptx_tile_register(PtxFn *fn, const IRInstruction *in) {
  PtxTile *tile;
  const MtlcType *type = in->value_type;
  if (fn->tile_count == fn->tile_capacity) {
    size_t next = fn->tile_capacity ? fn->tile_capacity * 2 : 8;
    PtxTile *grown = realloc(fn->tiles, next * sizeof(*grown));
    if (!grown) {
      fn_error(fn, "PTX: out of memory registering tiles");
      return;
    }
    fn->tiles = grown;
    fn->tile_capacity = next;
  }
  tile = &fn->tiles[fn->tile_count];
  memset(tile, 0, sizeof(*tile));
  tile->name = strdup(in->dest.name);
  tile->type = type;
  tile->cls = ptx_tile_class(type);
  tile->rows = (int)type->view_extents[0];
  tile->cols = (int)type->view_extents[1];
  tile->layout = type->view_layout;
  tile->count = ptx_tile_register_count(type);
  tile->first = SIZE_MAX;
  tile->last = 0;
  tile->line = in->location.line;
  tile->temporary = in->dest.kind == IR_OPERAND_TEMP;
  if (tile->cls == PC_NONE || tile->count <= 0) {
    fn_error(fn, "PTX: tile '%s' has no register form", in->dest.name);
    free(tile->name);
    return;
  }
  tile->base = fn->count[tile->cls];
  fn->count[tile->cls] += tile->count;
  fn->tile_count++;
}

static void ptx_tile_lanes(PtxFn *fn) {
  char lane[24], group[24], thread[24], pair[24];
  fn->tile_lane = new_reg(fn, PC_B32);
  fn->tile_group = new_reg(fn, PC_B32);
  fn->tile_thread = new_reg(fn, PC_B32);
  fn->tile_pair = new_reg(fn, PC_B32);
  reg_name(PC_B32, fn->tile_lane, lane);
  reg_name(PC_B32, fn->tile_group, group);
  reg_name(PC_B32, fn->tile_thread, thread);
  reg_name(PC_B32, fn->tile_pair, pair);
  sb_printf(&fn->body, "\tmov.u32 %s, %%laneid;\n", lane);
  sb_printf(&fn->body, "\tshr.u32 %s, %s, 2;\n", group, lane);
  sb_printf(&fn->body, "\tand.b32 %s, %s, 3;\n", thread, lane);
  sb_printf(&fn->body, "\tshl.b32 %s, %s, 1;\n", pair, thread);
}

static void ptx_tile_reg(const PtxTile *tile, int index, char *out) {
  reg_name(tile->cls, tile->base + index, out);
}

static int ptx_tile_c_index(const PtxTile *tile, int mt, int nt, int e) {
  return (mt * (tile->cols / 8) + nt) * 4 + e;
}

static int ptx_tile_vector_index(int mt, int half) { return mt * 2 + half; }

typedef struct {
  PtxTile *tile;
  char scalar[24];
} PtxTileArg;

static int ptx_tile_arg(PtxFn *fn, const IRInstruction *in, size_t index,
                        PtxClass scalar_class, PtxTileArg *out) {
  memset(out, 0, sizeof(*out));
  if (index >= in->argument_count) {
    return 0;
  }
  out->tile = ptx_tile_operand(fn, &in->arguments[index]);
  if (out->tile) {
    return 1;
  }
  if (in->argument_types && ptx_tile_is_type(in->argument_types[index])) {
    fn_error(fn, "PTX: tile operand '%s' has no registers",
             in->arguments[index].name ? in->arguments[index].name : "?");
    return 0;
  }
  use_as(fn, &in->arguments[index], scalar_class, out->scalar);
  return 1;
}

static void ptx_tile_arg_element(const PtxTileArg *arg, int mt, int nt, int e,
                                 int vector_dest, char *out) {
  if (!arg->tile) {
    snprintf(out, 24, "%s", arg->scalar);
    return;
  }
  if (arg->tile->cols == 0) {
    ptx_tile_reg(arg->tile,
                 ptx_tile_vector_index(mt, vector_dest ? e : (e >> 1)), out);
    return;
  }
  ptx_tile_reg(arg->tile, ptx_tile_c_index(arg->tile, mt, nt, e), out);
}

static const char *ptx_tile_type_suffix(PtxClass cls) {
  return cls == PC_F32 ? "f32" : cls == PC_PRED ? "pred" : "s32";
}

static const char *ptx_tile_compare(const char *op, PtxClass cls) {
  int real = cls == PC_F32;
  if (!strcmp(op, "lt")) return "lt";
  if (!strcmp(op, "le")) return "le";
  if (!strcmp(op, "gt")) return "gt";
  if (!strcmp(op, "ge")) return "ge";
  if (!strcmp(op, "eq")) return "eq";
  if (!strcmp(op, "ne")) return real ? "neu" : "ne";
  return NULL;
}

static void ptx_tile_math(PtxFn *fn, const char *op, const char *dst,
                          const char *src) {
  if (!strcmp(op, "exp")) {
    char t[24];
    reg_name(PC_F32, new_reg(fn, PC_F32), t);
    sb_printf(&fn->body, "\tmul.rn.f32 %s, %s, 0f3FB8AA3B;\n", t, src);
    sb_printf(&fn->body, "\tex2.approx.f32 %s, %s;\n", dst, t);
  } else if (!strcmp(op, "log")) {
    char t[24];
    reg_name(PC_F32, new_reg(fn, PC_F32), t);
    sb_printf(&fn->body, "\tlg2.approx.f32 %s, %s;\n", t, src);
    sb_printf(&fn->body, "\tmul.rn.f32 %s, %s, 0f3F317218;\n", dst, t);
  } else if (!strcmp(op, "sqrt")) {
    sb_printf(&fn->body, "\tsqrt.rn.f32 %s, %s;\n", dst, src);
  } else if (!strcmp(op, "rsqrt")) {
    sb_printf(&fn->body, "\trsqrt.approx.f32 %s, %s;\n", dst, src);
  } else if (!strcmp(op, "abs")) {
    sb_printf(&fn->body, "\tabs.f32 %s, %s;\n", dst, src);
  } else if (!strcmp(op, "sin")) {
    sb_printf(&fn->body, "\tsin.approx.f32 %s, %s;\n", dst, src);
  } else if (!strcmp(op, "cos")) {
    sb_printf(&fn->body, "\tcos.approx.f32 %s, %s;\n", dst, src);
  } else {
    fn_error(fn, "PTX: unknown tile function '%s'", op);
  }
}

static void ptx_tile_element(PtxFn *fn, const char *op, PtxClass dest_cls,
                             PtxClass operand_cls, const char *dst,
                             const char *a, const char *b, const char *c) {
  const char *ty = ptx_tile_type_suffix(operand_cls);
  const char *cmp = ptx_tile_compare(op, operand_cls);
  if (!strcmp(op, "fill") || !strcmp(op, "copy")) {
    sb_printf(&fn->body, "\tmov%s %s, %s;\n", cls_regtype(dest_cls), dst, a);
  } else if (cmp) {
    sb_printf(&fn->body, "\tsetp.%s.%s %s, %s, %s;\n", cmp, ty, dst, a, b);
  } else if (!strcmp(op, "select")) {
    if (dest_cls == PC_PRED) {
      char t[24], u[24];
      reg_name(PC_PRED, new_reg(fn, PC_PRED), t);
      reg_name(PC_PRED, new_reg(fn, PC_PRED), u);
      sb_printf(&fn->body, "\tand.pred %s, %s, %s;\n", t, a, b);
      sb_printf(&fn->body, "\tnot.pred %s, %s;\n", u, a);
      sb_printf(&fn->body, "\tand.pred %s, %s, %s;\n", u, u, c);
      sb_printf(&fn->body, "\tor.pred %s, %s, %s;\n", dst, t, u);
    } else {
      sb_printf(&fn->body, "\tselp.%s %s, %s, %s, %s;\n",
                dest_cls == PC_F32 ? "f32" : "b32", dst, b, c, a);
    }
  } else if (!strcmp(op, "and") || !strcmp(op, "or")) {
    sb_printf(&fn->body, "\t%s.pred %s, %s, %s;\n", op, dst, a, b);
  } else if (!strcmp(op, "not")) {
    sb_printf(&fn->body, "\tnot.pred %s, %s;\n", dst, a);
  } else if (!strcmp(op, "neg")) {
    sb_printf(&fn->body, "\tneg.%s %s, %s;\n", ty, dst, a);
  } else if (!strcmp(op, "max") || !strcmp(op, "min")) {
    sb_printf(&fn->body, "\t%s.%s %s, %s, %s;\n", op, ty, dst, a, b);
  } else if (!strcmp(op, "add") || !strcmp(op, "sub") ||
             !strcmp(op, "mul") || !strcmp(op, "div")) {
    if (operand_cls == PC_F32) {
      sb_printf(&fn->body, "\t%s.rn.f32 %s, %s, %s;\n", op, dst, a, b);
    } else if (!strcmp(op, "mul")) {
      sb_printf(&fn->body, "\tmul.lo.s32 %s, %s, %s;\n", dst, a, b);
    } else {
      sb_printf(&fn->body, "\t%s.s32 %s, %s, %s;\n", op, dst, a, b);
    }
  } else {
    ptx_tile_math(fn, op, dst, a);
  }
}

static PtxClass ptx_tile_operand_class(const IRInstruction *in, PtxFn *fn,
                                       PtxTile *dest) {
  const char *op = in->text;
  if (!strcmp(op, "select")) {
    return dest->cls;
  }
  for (size_t i = 0; i < in->argument_count; i++) {
    PtxTile *tile = ptx_tile_operand(fn, &in->arguments[i]);
    if (tile) {
      return tile->cls;
    }
  }
  return dest->cls;
}

static void ptx_tile_index(PtxFn *fn, PtxTile *dest, int column) {
  char group[24], pair[24];
  reg_name(PC_B32, fn->tile_group, group);
  reg_name(PC_B32, fn->tile_pair, pair);
  if (dest->cols == 0) {
    for (int mt = 0; mt < dest->rows / 16; mt++) {
      for (int h = 0; h < 2; h++) {
        char d[24];
        ptx_tile_reg(dest, ptx_tile_vector_index(mt, h), d);
        sb_printf(&fn->body, "\tadd.s32 %s, %s, %d;\n", d, group,
                  mt * 16 + h * 8);
      }
    }
    return;
  }
  for (int mt = 0; mt < dest->rows / 16; mt++) {
    for (int nt = 0; nt < dest->cols / 8; nt++) {
      for (int e = 0; e < 4; e++) {
        char d[24];
        ptx_tile_reg(dest, ptx_tile_c_index(dest, mt, nt, e), d);
        if (column) {
          sb_printf(&fn->body, "\tadd.s32 %s, %s, %d;\n", d, pair,
                    nt * 8 + (e & 1));
        } else {
          sb_printf(&fn->body, "\tadd.s32 %s, %s, %d;\n", d, group,
                    mt * 16 + (e >> 1) * 8);
        }
      }
    }
  }
}

static void ptx_tile_reduce(PtxFn *fn, PtxTile *dest, PtxTile *source,
                            int sum) {
  const char *ty = ptx_tile_type_suffix(source->cls);
  for (int mt = 0; mt < source->rows / 16; mt++) {
    for (int h = 0; h < 2; h++) {
      char acc[24], other[24];
      ptx_tile_reg(dest, ptx_tile_vector_index(mt, h), acc);
      for (int nt = 0; nt < source->cols / 8; nt++) {
        for (int e = 0; e < 2; e++) {
          char v[24];
          ptx_tile_reg(source, ptx_tile_c_index(source, mt, nt, h * 2 + e), v);
          if (nt == 0 && e == 0) {
            sb_printf(&fn->body, "\tmov.%s %s, %s;\n",
                      source->cls == PC_F32 ? "f32" : "b32", acc, v);
          } else if (sum) {
            if (source->cls == PC_F32)
              sb_printf(&fn->body, "\tadd.rn.f32 %s, %s, %s;\n", acc, acc, v);
            else
              sb_printf(&fn->body, "\tadd.s32 %s, %s, %s;\n", acc, acc, v);
          } else {
            sb_printf(&fn->body, "\tmax.%s %s, %s, %s;\n", ty, acc, acc, v);
          }
        }
      }
      reg_name(source->cls, new_reg(fn, source->cls), other);
      for (int step = 1; step <= 2; step <<= 1) {
        sb_printf(&fn->body,
                  "\tshfl.sync.bfly.b32 %s, %s, %d, 31, 0xffffffff;\n", other,
                  acc, step);
        if (sum) {
          if (source->cls == PC_F32)
            sb_printf(&fn->body, "\tadd.rn.f32 %s, %s, %s;\n", acc, acc,
                      other);
          else
            sb_printf(&fn->body, "\tadd.s32 %s, %s, %s;\n", acc, acc, other);
        } else {
          sb_printf(&fn->body, "\tmax.%s %s, %s, %s;\n", ty, acc, acc, other);
        }
      }
    }
  }
}

static void ptx_tile_pack(PtxFn *fn, MtlcTypeKind element, const char *dst,
                          const char *lo, const char *hi) {
  sb_printf(&fn->body, "\tcvt.rn.%s.f32 %s, %s, %s;\n",
            element == MTLC_TYPE_BFLOAT16 ? "bf16x2" : "f16x2", dst, hi, lo);
}

static void ptx_tile_cast(PtxFn *fn, PtxTile *dest, PtxTile *source) {
  if (dest->layout == MTLC_VIEW_LAYOUT_FRAGMENT_A) {
    MtlcTypeKind element = dest->type->base_type->kind;
    for (int mt = 0; mt < dest->rows / 16; mt++) {
      for (int ks = 0; ks < dest->cols / 16; ks++) {
        for (int i = 0; i < 4; i++) {
          int nt = ks * 2 + (i >> 1);
          int base = (i & 1) * 2;
          char d[24], lo[24], hi[24];
          ptx_tile_reg(dest, (mt * (dest->cols / 16) + ks) * 4 + i, d);
          ptx_tile_reg(source, ptx_tile_c_index(source, mt, nt, base), lo);
          ptx_tile_reg(source, ptx_tile_c_index(source, mt, nt, base + 1), hi);
          ptx_tile_pack(fn, element, d, lo, hi);
        }
      }
    }
    return;
  }
  for (int i = 0; i < dest->count; i++) {
    char d[24], s[24];
    ptx_tile_reg(dest, i, d);
    ptx_tile_reg(source, i, s);
    if (dest->cls == source->cls) {
      sb_printf(&fn->body, "\tmov%s %s, %s;\n", cls_regtype(dest->cls), d, s);
    } else if (dest->cls == PC_F32 && source->cls == PC_B32) {
      sb_printf(&fn->body, "\tcvt.rn.f32.s32 %s, %s;\n", d, s);
    } else if (dest->cls == PC_B32 && source->cls == PC_F32) {
      sb_printf(&fn->body, "\tcvt.rzi.s32.f32 %s, %s;\n", d, s);
    } else if (dest->cls == PC_B32 && source->cls == PC_PRED) {
      sb_printf(&fn->body, "\tselp.b32 %s, 1, 0, %s;\n", d, s);
    } else if (dest->cls == PC_F32 && source->cls == PC_PRED) {
      sb_printf(&fn->body, "\tselp.f32 %s, 0f3F800000, 0f00000000, %s;\n", d,
                s);
    } else if (dest->cls == PC_PRED && source->cls == PC_B32) {
      sb_printf(&fn->body, "\tsetp.ne.s32 %s, %s, 0;\n", d, s);
    } else if (dest->cls == PC_PRED && source->cls == PC_F32) {
      sb_printf(&fn->body, "\tsetp.neu.f32 %s, %s, 0f00000000;\n", d, s);
    } else {
      fn_error(fn, "PTX: unsupported tile conversion");
      return;
    }
  }
}

static void ptx_tile_elementwise(PtxFn *fn, const IRInstruction *in,
                                 PtxTile *dest) {
  const char *op = in->text;
  PtxClass operand_cls = ptx_tile_operand_class(in, fn, dest);
  PtxTileArg args[3];
  size_t count = in->argument_count < 3 ? in->argument_count : 3;
  int vector = dest->cols == 0;
  for (size_t i = 0; i < count; i++) {
    PtxClass want = operand_cls;
    if (!strcmp(op, "select") && i == 0) want = PC_PRED;
    if (!strcmp(op, "fill")) want = dest->cls;
    if (!ptx_tile_arg(fn, in, i, want, &args[i])) {
      return;
    }
  }
  int mts = dest->rows / 16;
  int nts = vector ? 1 : dest->cols / 8;
  int es = vector ? 2 : 4;
  for (int mt = 0; mt < mts; mt++) {
    for (int nt = 0; nt < nts; nt++) {
      for (int e = 0; e < es; e++) {
        char d[24], a[24] = {0}, b[24] = {0}, c[24] = {0};
        int index = vector ? ptx_tile_vector_index(mt, e)
                           : ptx_tile_c_index(dest, mt, nt, e);
        ptx_tile_reg(dest, index, d);
        if (count > 0) ptx_tile_arg_element(&args[0], mt, nt, e, vector, a);
        if (count > 1) ptx_tile_arg_element(&args[1], mt, nt, e, vector, b);
        if (count > 2) ptx_tile_arg_element(&args[2], mt, nt, e, vector, c);
        ptx_tile_element(fn, op, dest->cls, operand_cls, d, a, b, c);
      }
    }
  }
}

static const char *ptx_tile_space(PtxFn *fn, const IROperand *pointer,
                                  int load) {
  PtxVal desc = operand_desc(fn, pointer);
  const char *space = load ? ptx_load_space(desc.address_space)
                           : ptx_memory_space(desc.address_space);
  return space ? space : "";
}

static int ptx_tile_is_view(const MtlcType *type) {
  return type && type->view_extents[0] && type->view_extents[1] &&
         !ptx_tile_is_type(type);
}

static void ptx_tile_offset(PtxFn *fn, const MtlcType *memory,
                            const char *ld, const char *row, const char *col,
                            char *out) {
  reg_name(PC_B32, new_reg(fn, PC_B32), out);
  if (!ptx_tile_is_view(memory) ||
      memory->view_layout == MTLC_VIEW_LAYOUT_ROW ||
      memory->view_layout == MTLC_VIEW_LAYOUT_NONE) {
    sb_printf(&fn->body, "\tmad.lo.s32 %s, %s, %s, %s;\n", out, row, ld, col);
    return;
  }
  {
    long long rows = (long long)memory->view_extents[0];
    long long cols = (long long)memory->view_extents[1];
    long long esize = memory->base_type && memory->base_type->size
                          ? (long long)memory->base_type->size
                          : 2;
    if (memory->view_layout == MTLC_VIEW_LAYOUT_COL) {
      sb_printf(&fn->body, "\tmad.lo.s32 %s, %s, %lld, %s;\n", out, col, rows,
                row);
      return;
    }
    if (memory->view_layout == MTLC_VIEW_LAYOUT_SWIZZLE32 ||
        memory->view_layout == MTLC_VIEW_LAYOUT_SWIZZLE64 ||
        memory->view_layout == MTLC_VIEW_LAYOUT_SWIZZLE128) {
      long long chunk_bytes =
          memory->view_layout == MTLC_VIEW_LAYOUT_SWIZZLE32   ? 4
          : memory->view_layout == MTLC_VIEW_LAYOUT_SWIZZLE64 ? 8
                                                              : 16;
      long long chunk = chunk_bytes / esize;
      long long per_row = chunk > 0 ? cols / chunk : 0;
      int shift = 0;
      while ((1LL << shift) < chunk) shift++;
      if (chunk > 0 && (1LL << shift) == chunk && per_row > 0 &&
          (per_row & (per_row - 1)) == 0) {
        char key[24], which[24], within[24], base[24];
        reg_name(PC_B32, new_reg(fn, PC_B32), key);
        reg_name(PC_B32, new_reg(fn, PC_B32), which);
        reg_name(PC_B32, new_reg(fn, PC_B32), within);
        reg_name(PC_B32, new_reg(fn, PC_B32), base);
        sb_printf(&fn->body, "\tand.b32 %s, %s, %lld;\n", key, row,
                  per_row - 1);
        sb_printf(&fn->body, "\tshr.u32 %s, %s, %d;\n", which, col, shift);
        sb_printf(&fn->body, "\txor.b32 %s, %s, %s;\n", which, which, key);
        sb_printf(&fn->body, "\tand.b32 %s, %s, %lld;\n", within, col,
                  chunk - 1);
        sb_printf(&fn->body, "\tmul.lo.s32 %s, %s, %lld;\n", base, row, cols);
        sb_printf(&fn->body, "\tshl.b32 %s, %s, %d;\n", which, which, shift);
        sb_printf(&fn->body, "\tadd.s32 %s, %s, %s;\n", base, base, which);
        sb_printf(&fn->body, "\tadd.s32 %s, %s, %s;\n", out, base, within);
        return;
      }
    }
    fn_error(fn, "PTX: tile access through layout %d is not supported",
             (int)memory->view_layout);
  }
}

static void ptx_tile_address(PtxFn *fn, const char *base, const char *offset,
                             int esize, char *out) {
  char wide[24];
  reg_name(PC_B64, new_reg(fn, PC_B64), wide);
  reg_name(PC_B64, new_reg(fn, PC_B64), out);
  sb_printf(&fn->body, "\tmul.wide.s32 %s, %s, %d;\n", wide, offset, esize);
  sb_printf(&fn->body, "\tadd.s64 %s, %s, %s;\n", out, base, wide);
}

static void ptx_tile_row_guard(PtxFn *fn, const char *rows, const char *row,
                               char *pred) {
  reg_name(PC_PRED, new_reg(fn, PC_PRED), pred);
  sb_printf(&fn->body, "\tsetp.lt.s32 %s, %s, %s;\n", pred, row, rows);
}

static void ptx_tile_lane_rc(PtxFn *fn, int row_const, int col_const,
                             int use_pair, char *row, char *col) {
  char group[24], pair[24];
  reg_name(PC_B32, fn->tile_group, group);
  reg_name(PC_B32, fn->tile_pair, pair);
  reg_name(PC_B32, new_reg(fn, PC_B32), row);
  reg_name(PC_B32, new_reg(fn, PC_B32), col);
  sb_printf(&fn->body, "\tadd.s32 %s, %s, %d;\n", row, group, row_const);
  if (use_pair) {
    sb_printf(&fn->body, "\tadd.s32 %s, %s, %d;\n", col, pair, col_const);
  } else {
    sb_printf(&fn->body, "\tmov.u32 %s, %d;\n", col, col_const);
  }
}

static void ptx_tile_load_element(PtxFn *fn, const char *space,
                                  MtlcTypeKind memory_kind, const char *addr,
                                  PtxClass want, const char *dst,
                                  const char *guard) {
  char raw[24];
  if (memory_kind == MTLC_TYPE_FLOAT16 || memory_kind == MTLC_TYPE_BFLOAT16 ||
      memory_kind == MTLC_TYPE_UINT16) {
    reg_name(PC_B16, new_reg(fn, PC_B16), raw);
    sb_printf(&fn->body, "\tmov.b16 %s, 0;\n", raw);
    sb_printf(&fn->body, "\t%s%s%sld%s.b16 %s, [%s];\n", guard ? "@" : "",
              guard ? guard : "", guard ? " " : "", space, raw, addr);
    if (want == PC_F32) {
      sb_printf(&fn->body, "\tcvt.f32.%s %s, %s;\n",
                memory_kind == MTLC_TYPE_BFLOAT16 ? "bf16" : "f16", dst, raw);
    } else {
      sb_printf(&fn->body, "\tmov.b16 %s, %s;\n", dst, raw);
    }
    return;
  }
  sb_printf(&fn->body, "\tmov%s %s, %s;\n", cls_regtype(want), dst,
            want == PC_F32 ? "0f00000000" : "0");
  sb_printf(&fn->body, "\t%s%s%sld%s.%s %s, [%s];\n", guard ? "@" : "",
            guard ? guard : "", guard ? " " : "", space,
            mem_type_suffix(memory_kind), dst, addr);
}

static void ptx_tile_load(PtxFn *fn, const IRInstruction *in) {
  PtxTile *tile = ptx_tile_operand(fn, &in->dest);
  const MtlcType *memory = in->argument_types ? in->argument_types[0] : NULL;
  const char *space = ptx_tile_space(fn, &in->arguments[0], 1);
  char base[24], ld[24], rows[24];
  MtlcTypeKind memory_kind;
  int esize;
  int guarded = in->arguments[2].kind != IR_OPERAND_NONE;
  if (!tile || !memory || !memory->base_type) {
    fn_error(fn, "PTX: malformed tile load");
    return;
  }
  memory_kind = memory->base_type->kind;
  esize = (int)(memory->base_type->size ? memory->base_type->size : 4);
  use_as(fn, &in->arguments[0], PC_B64, base);
  if (in->arguments[1].kind != IR_OPERAND_NONE) {
    use_as(fn, &in->arguments[1], PC_B32, ld);
  } else {
    snprintf(ld, sizeof(ld), "%llu",
             (unsigned long long)memory->view_extents[1]);
  }
  if (guarded) use_as(fn, &in->arguments[2], PC_B32, rows);
  if (tile->cols == 0) {
    for (int mt = 0; mt < tile->rows / 16; mt++) {
      for (int h = 0; h < 2; h++) {
        char row[24], col[24], off[24], addr[24], guard[24], d[24];
        ptx_tile_lane_rc(fn, mt * 16 + h * 8, 0, 0, row, col);
        reg_name(PC_B32, new_reg(fn, PC_B32), off);
        sb_printf(&fn->body, "\tmul.lo.s32 %s, %s, %s;\n", off, row, ld);
        ptx_tile_address(fn, base, off, esize, addr);
        if (guarded) ptx_tile_row_guard(fn, rows, row, guard);
        ptx_tile_reg(tile, ptx_tile_vector_index(mt, h), d);
        ptx_tile_load_element(fn, space, memory_kind, addr, tile->cls, d,
                              guarded ? guard : NULL);
      }
    }
    return;
  }
  if (tile->layout == MTLC_VIEW_LAYOUT_FRAGMENT_A) {
    MtlcTypeKind element = tile->type->base_type->kind;
    for (int mt = 0; mt < tile->rows / 16; mt++) {
      for (int ks = 0; ks < tile->cols / 16; ks++) {
        for (int i = 0; i < 4; i++) {
          char row[24], col[24], guard[24], d[24];
          char halves[2][24];
          ptx_tile_lane_rc(fn, mt * 16 + (i & 1) * 8, ks * 16 + (i >> 1) * 8,
                           1, row, col);
          if (guarded) ptx_tile_row_guard(fn, rows, row, guard);
          ptx_tile_reg(tile, (mt * (tile->cols / 16) + ks) * 4 + i, d);
          for (int half = 0; half < 2; half++) {
            char c2[24], off[24], addr[24];
            reg_name(PC_B32, new_reg(fn, PC_B32), c2);
            sb_printf(&fn->body, "\tadd.s32 %s, %s, %d;\n", c2, col, half);
            ptx_tile_offset(fn, memory, ld, row, c2, off);
            ptx_tile_address(fn, base, off, esize, addr);
            if (memory_kind == MTLC_TYPE_FLOAT32) {
              reg_name(PC_F32, new_reg(fn, PC_F32), halves[half]);
              ptx_tile_load_element(fn, space, memory_kind, addr, PC_F32,
                                    halves[half], guarded ? guard : NULL);
            } else {
              reg_name(PC_B16, new_reg(fn, PC_B16), halves[half]);
              ptx_tile_load_element(fn, space, memory_kind, addr, PC_B16,
                                    halves[half], guarded ? guard : NULL);
            }
          }
          if (memory_kind == MTLC_TYPE_FLOAT32) {
            ptx_tile_pack(fn, element, d, halves[0], halves[1]);
          } else {
            sb_printf(&fn->body, "\tmov.b32 %s, {%s, %s};\n", d, halves[0],
                      halves[1]);
          }
        }
      }
    }
    return;
  }
  for (int mt = 0; mt < tile->rows / 16; mt++) {
    for (int nt = 0; nt < tile->cols / 8; nt++) {
      for (int e = 0; e < 4; e++) {
        char row[24], col[24], off[24], addr[24], guard[24], d[24];
        ptx_tile_lane_rc(fn, mt * 16 + (e >> 1) * 8, nt * 8 + (e & 1), 1, row,
                         col);
        ptx_tile_offset(fn, memory, ld, row, col, off);
        ptx_tile_address(fn, base, off, esize, addr);
        if (guarded) ptx_tile_row_guard(fn, rows, row, guard);
        ptx_tile_reg(tile, ptx_tile_c_index(tile, mt, nt, e), d);
        ptx_tile_load_element(fn, space, memory_kind, addr, tile->cls, d,
                              guarded ? guard : NULL);
      }
    }
  }
}

static void ptx_tile_store_element(PtxFn *fn, const char *space,
                                   MtlcTypeKind memory_kind, const char *addr,
                                   PtxClass cls, const char *src,
                                   const char *guard) {
  char g[32] = "";
  if (guard) snprintf(g, sizeof(g), "@%s ", guard);
  if ((memory_kind == MTLC_TYPE_FLOAT16 || memory_kind == MTLC_TYPE_BFLOAT16) &&
      cls == PC_F32) {
    char half[24];
    reg_name(PC_B16, new_reg(fn, PC_B16), half);
    sb_printf(&fn->body, "\tcvt.rn.%s.f32 %s, %s;\n",
              memory_kind == MTLC_TYPE_BFLOAT16 ? "bf16" : "f16", half, src);
    sb_printf(&fn->body, "\t%sst%s.b16 [%s], %s;\n", g, space, addr, half);
    return;
  }
  if (cls == PC_PRED) {
    char value[24];
    reg_name(PC_B32, new_reg(fn, PC_B32), value);
    sb_printf(&fn->body, "\tselp.b32 %s, 1, 0, %s;\n", value, src);
    sb_printf(&fn->body, "\t%sst%s.%s [%s], %s;\n", g, space,
              mem_type_suffix(memory_kind), addr, value);
    return;
  }
  sb_printf(&fn->body, "\t%sst%s.%s [%s], %s;\n", g, space,
            mem_type_suffix(memory_kind), addr, src);
}

static void ptx_tile_store(PtxFn *fn, const IRInstruction *in) {
  PtxTile *tile = ptx_tile_operand(fn, &in->arguments[0]);
  const MtlcType *memory = in->value_type;
  const char *space = ptx_tile_space(fn, &in->dest, 0);
  char base[24], ld[24], rows[24];
  MtlcTypeKind memory_kind;
  int esize;
  int guarded = in->arguments[2].kind != IR_OPERAND_NONE;
  if (!tile || !memory || !memory->base_type) {
    fn_error(fn, "PTX: malformed tile store");
    return;
  }
  memory_kind = memory->base_type->kind;
  esize = (int)(memory->base_type->size ? memory->base_type->size : 4);
  use_as(fn, &in->dest, PC_B64, base);
  if (in->arguments[1].kind != IR_OPERAND_NONE) {
    use_as(fn, &in->arguments[1], PC_B32, ld);
  } else {
    snprintf(ld, sizeof(ld), "%llu",
             (unsigned long long)memory->view_extents[1]);
  }
  if (guarded) use_as(fn, &in->arguments[2], PC_B32, rows);
  if (tile->cols == 0) {
    char thread[24], first[24];
    reg_name(PC_B32, fn->tile_thread, thread);
    reg_name(PC_PRED, new_reg(fn, PC_PRED), first);
    sb_printf(&fn->body, "\tsetp.eq.u32 %s, %s, 0;\n", first, thread);
    for (int mt = 0; mt < tile->rows / 16; mt++) {
      for (int h = 0; h < 2; h++) {
        char row[24], col[24], off[24], addr[24], guard[24], s[24];
        ptx_tile_lane_rc(fn, mt * 16 + h * 8, 0, 0, row, col);
        reg_name(PC_B32, new_reg(fn, PC_B32), off);
        sb_printf(&fn->body, "\tmul.lo.s32 %s, %s, %s;\n", off, row, ld);
        ptx_tile_address(fn, base, off, esize, addr);
        if (guarded) {
          ptx_tile_row_guard(fn, rows, row, guard);
          sb_printf(&fn->body, "\tand.pred %s, %s, %s;\n", guard, guard,
                    first);
        } else {
          snprintf(guard, sizeof(guard), "%s", first);
        }
        ptx_tile_reg(tile, ptx_tile_vector_index(mt, h), s);
        ptx_tile_store_element(fn, space, memory_kind, addr, tile->cls, s,
                               guard);
      }
    }
    return;
  }
  for (int mt = 0; mt < tile->rows / 16; mt++) {
    for (int nt = 0; nt < tile->cols / 8; nt++) {
      for (int e = 0; e < 4; e++) {
        char row[24], col[24], off[24], addr[24], guard[24], s[24];
        ptx_tile_lane_rc(fn, mt * 16 + (e >> 1) * 8, nt * 8 + (e & 1), 1, row,
                         col);
        ptx_tile_offset(fn, memory, ld, row, col, off);
        ptx_tile_address(fn, base, off, esize, addr);
        if (guarded) ptx_tile_row_guard(fn, rows, row, guard);
        ptx_tile_reg(tile, ptx_tile_c_index(tile, mt, nt, e), s);
        ptx_tile_store_element(fn, space, memory_kind, addr, tile->cls, s,
                               guarded ? guard : NULL);
      }
    }
  }
}

static int ptx_tile_b_ldmatrix_ok(PtxFn *fn, const MtlcType *memory,
                                  const char *space, unsigned ldb) {
  if (fn->target_arch < 75 || !ptx_version_at_least(fn, 6, 5) ||
      strcmp(space, ".shared") != 0) {
    return 0;
  }
  if (ptx_tile_is_view(memory)) {
    long long cols = (long long)memory->view_extents[1];
    return memory->view_layout == MTLC_VIEW_LAYOUT_ROW ||
           memory->view_layout == MTLC_VIEW_LAYOUT_NONE ||
           memory->view_layout == MTLC_VIEW_LAYOUT_SWIZZLE64 ||
           memory->view_layout == MTLC_VIEW_LAYOUT_SWIZZLE128
               ? (cols * 2) % 16 == 0
               : 0;
  }
  return ldb && (ldb * 2u) % 16u == 0;
}

static void ptx_tile_mma(PtxFn *fn, const IRInstruction *in) {
  const MtlcTensorMmaDesc *desc = &IR_TENSOR_MMA(in);
  PtxTile *d = ptx_tile_operand(fn, &in->dest);
  PtxTile *a = ptx_tile_operand(fn, &in->arguments[0]);
  PtxTile *c = ptx_tile_operand(fn, &in->arguments[2]);
  PtxTile *scale = in->argument_count > 4
                       ? ptx_tile_operand(fn, &in->arguments[4])
                       : NULL;
  const MtlcType *b_type = in->argument_types ? in->argument_types[1] : NULL;
  const char *b_space = ptx_tile_space(fn, &in->arguments[1], 1);
  int col_b = desc->b_layout == MTLC_TENSOR_LAYOUT_COLUMN_MAJOR;
  int m_tiles = desc->m / 16, n_tiles = desc->n / 8, k_steps = desc->k / 16;
  unsigned ldb_static = ptx_tile_is_view(b_type)
                            ? (unsigned)b_type->view_extents[1]
                            : desc->b_leading_dimension;
  char b_base[24], ldb[24];
  const char *element = desc->a_element == MTLC_TENSOR_ELEMENT_BFLOAT16
                            ? "bf16"
                            : "f16";
  if (!d || (in->arguments[2].kind != IR_OPERAND_FLOAT && !c) || !b_type) {
    fn_error(fn, "PTX: malformed tile MMA");
    return;
  }
  if (!a) {
    fn_error(fn, "PTX: a tile MMA reads A from a fragment_a tile here");
    return;
  }
  for (int i = 0; i < d->count; i++) {
    char r[24];
    ptx_tile_reg(d, i, r);
    if (!c) {
      sb_printf(&fn->body, "\tmov.f32 %s, 0f00000000;\n", r);
    } else if (c != d) {
      char s[24];
      ptx_tile_reg(c, i, s);
      sb_printf(&fn->body, "\tmov.f32 %s, %s;\n", r, s);
    }
  }
  if (scale) {
    for (int mt = 0; mt < m_tiles; mt++) {
      for (int nt = 0; nt < n_tiles; nt++) {
        for (int e = 0; e < 4; e++) {
          char r[24], s[24];
          ptx_tile_reg(d, ptx_tile_c_index(d, mt, nt, e), r);
          ptx_tile_reg(scale, ptx_tile_vector_index(mt, e >> 1), s);
          sb_printf(&fn->body, "\tmul.rn.f32 %s, %s, %s;\n", r, r, s);
        }
      }
    }
  }
  use_as(fn, &in->arguments[1], PC_B64, b_base);
  if (in->arguments[3].kind != IR_OPERAND_NONE) {
    use_as(fn, &in->arguments[3], PC_B32, ldb);
    ldb_static = 0;
  } else {
    snprintf(ldb, sizeof(ldb), "%u", ldb_static);
  }
  int ldsm = ptx_tile_b_ldmatrix_ok(fn, b_type, b_space, ldb_static);
  char lane[24], narrow[24], lane_major[24], lane_minor[24];
  reg_name(PC_B32, fn->tile_lane, lane);
  if (ldsm) {
    reg_name(PC_B32, new_reg(fn, PC_B32), narrow);
    reg_name(PC_B32, new_reg(fn, PC_B32), lane_major);
    reg_name(PC_B32, new_reg(fn, PC_B32), lane_minor);
    sb_printf(&fn->body, "\tcvt.u32.u64 %s, %s;\n", narrow, b_base);
    if (col_b) {
      char low[24], hi[24];
      reg_name(PC_B32, new_reg(fn, PC_B32), low);
      reg_name(PC_B32, new_reg(fn, PC_B32), hi);
      sb_printf(&fn->body, "\tand.b32 %s, %s, 7;\n", low, lane);
      sb_printf(&fn->body, "\tshr.u32 %s, %s, 4;\n", hi, lane);
      sb_printf(&fn->body, "\tmad.lo.s32 %s, %s, 8, %s;\n", lane_major, hi,
                low);
      sb_printf(&fn->body, "\tbfe.u32 %s, %s, 3, 1;\n", lane_minor, lane);
      sb_printf(&fn->body, "\tshl.b32 %s, %s, 3;\n", lane_minor, lane_minor);
    } else {
      char low[24], mid[24];
      reg_name(PC_B32, new_reg(fn, PC_B32), low);
      reg_name(PC_B32, new_reg(fn, PC_B32), mid);
      sb_printf(&fn->body, "\tand.b32 %s, %s, 7;\n", low, lane);
      sb_printf(&fn->body, "\tbfe.u32 %s, %s, 3, 1;\n", mid, lane);
      sb_printf(&fn->body, "\tmad.lo.s32 %s, %s, 8, %s;\n", lane_major, mid,
                low);
      sb_printf(&fn->body, "\tshr.u32 %s, %s, 4;\n", lane_minor, lane);
      sb_printf(&fn->body, "\tshl.b32 %s, %s, 3;\n", lane_minor, lane_minor);
    }
  }
  for (int ks = 0; ks < k_steps; ks++) {
    int b_base_reg = fn->count[PC_B32];
    for (int i = 0; i < n_tiles * 2; i++) new_reg(fn, PC_B32);
    for (int nt = 0; nt < n_tiles; nt += 2) {
      int pair = nt + 1 < n_tiles;
      if (ldsm) {
        char row[24], col[24], off[24], addr[24], regs[4][24];
        reg_name(PC_B32, new_reg(fn, PC_B32), row);
        reg_name(PC_B32, new_reg(fn, PC_B32), col);
        if (col_b) {
          sb_printf(&fn->body, "\tadd.s32 %s, %s, %d;\n", row, lane_major,
                    nt * 8);
          sb_printf(&fn->body, "\tadd.s32 %s, %s, %d;\n", col, lane_minor,
                    ks * 16);
        } else {
          sb_printf(&fn->body, "\tadd.s32 %s, %s, %d;\n", row, lane_major,
                    ks * 16);
          sb_printf(&fn->body, "\tadd.s32 %s, %s, %d;\n", col, lane_minor,
                    nt * 8);
        }
        ptx_tile_offset(fn, b_type, ldb, row, col, off);
        reg_name(PC_B32, new_reg(fn, PC_B32), addr);
        sb_printf(&fn->body, "\tshl.b32 %s, %s, 1;\n", off, off);
        sb_printf(&fn->body, "\tadd.s32 %s, %s, %s;\n", addr, narrow, off);
        for (int i = 0; i < 4; i++) {
          reg_name(PC_B32, b_base_reg + nt * 2 + i, regs[i]);
        }
        if (pair) {
          sb_printf(&fn->body,
                    "\tldmatrix.sync.aligned.m8n8.x4%s.shared.b16 {%s, %s, %s, %s}, [%s];\n",
                    col_b ? "" : ".trans", regs[0], regs[1], regs[2], regs[3],
                    addr);
        } else {
          sb_printf(&fn->body,
                    "\tldmatrix.sync.aligned.m8n8.x2%s.shared.b16 {%s, %s}, [%s];\n",
                    col_b ? "" : ".trans", regs[0], regs[1], addr);
        }
        continue;
      }
      for (int t = nt; t < nt + (pair ? 2 : 1); t++) {
        for (int r = 0; r < 2; r++) {
          char halves[2][24], dst[24];
          reg_name(PC_B32, b_base_reg + t * 2 + r, dst);
          for (int half = 0; half < 2; half++) {
            char kr[24], nr[24], row[24], col[24], off[24], addr[24], thread[24],
                group[24];
            reg_name(PC_B32, fn->tile_pair, thread);
            reg_name(PC_B32, fn->tile_group, group);
            reg_name(PC_B32, new_reg(fn, PC_B32), kr);
            reg_name(PC_B32, new_reg(fn, PC_B32), nr);
            sb_printf(&fn->body, "\tadd.s32 %s, %s, %d;\n", kr, thread,
                      ks * 16 + r * 8 + half);
            sb_printf(&fn->body, "\tadd.s32 %s, %s, %d;\n", nr, group, t * 8);
            snprintf(row, sizeof(row), "%s", col_b ? nr : kr);
            snprintf(col, sizeof(col), "%s", col_b ? kr : nr);
            ptx_tile_offset(fn, b_type, ldb, row, col, off);
            ptx_tile_address(fn, b_base, off, 2, addr);
            reg_name(PC_B16, new_reg(fn, PC_B16), halves[half]);
            sb_printf(&fn->body, "\tld%s.b16 %s, [%s];\n", b_space,
                      halves[half], addr);
          }
          sb_printf(&fn->body, "\tmov.b32 %s, {%s, %s};\n", dst, halves[0],
                    halves[1]);
        }
      }
    }
    for (int mt = 0; mt < m_tiles; mt++) {
      char av[4][24];
      for (int i = 0; i < 4; i++) {
        ptx_tile_reg(a, (mt * (a->cols / 16) + ks) * 4 + i, av[i]);
      }
      for (int nt = 0; nt < n_tiles; nt++) {
        char b0[24], b1[24], acc[4][24];
        reg_name(PC_B32, b_base_reg + nt * 2, b0);
        reg_name(PC_B32, b_base_reg + nt * 2 + 1, b1);
        for (int e = 0; e < 4; e++) {
          ptx_tile_reg(d, ptx_tile_c_index(d, mt, nt, e), acc[e]);
        }
        sb_printf(&fn->body,
                  "\tmma.sync.aligned.m16n8k16.row.col.f32.%s.%s.f32 {%s, %s, %s, %s}, {%s, %s, %s, %s}, {%s, %s}, {%s, %s, %s, %s};\n",
                  element, element, acc[0], acc[1], acc[2], acc[3], av[0],
                  av[1], av[2], av[3], b0, b1, acc[0], acc[1], acc[2], acc[3]);
      }
    }
  }
}

static int ptx_tile_ew_op(const char *op) {
  static const char *const ops[] = {
      "fill", "copy", "add", "sub", "mul", "div", "lt",  "le",
      "gt",   "ge",   "eq",  "ne",  "and", "or",  "neg", "not",
      "max",  "min",  "select", "exp", "sqrt", "rsqrt", "abs", "log",
      "sin",  "cos",  "row", "col"};
  for (size_t i = 0; op && i < sizeof(ops) / sizeof(ops[0]); i++) {
    if (strcmp(op, ops[i]) == 0) return 1;
  }
  return 0;
}

static int ptx_tile_reduce_op(const char *op) {
  return op && (strcmp(op, "row_max") == 0 || strcmp(op, "row_sum") == 0);
}

static int ptx_tile_same_shape(const MtlcType *a, const MtlcType *b) {
  return a && b && a->view_extents[0] == b->view_extents[0] &&
         a->view_extents[1] == b->view_extents[1];
}

static int ptx_tile_window_member(const IRInstruction *in,
                                  const MtlcType *shape) {
  return in->op == IR_OP_TILE && ptx_tile_ew_op(in->text) &&
         ptx_tile_is_type(in->value_type) &&
         in->value_type->view_layout == MTLC_VIEW_LAYOUT_FRAGMENT_C &&
         (!shape || ptx_tile_same_shape(shape, in->value_type));
}

static int ptx_tile_scalar_passthrough(const IRInstruction *in) {
  switch (in->op) {
  case IR_OP_NOP:
    return 1;
  case IR_OP_ADDRESS_SPACE_ALLOC:
    return ptx_tile_is_type(in->value_type);
  case IR_OP_BINARY:
  case IR_OP_UNARY:
  case IR_OP_CAST:
  case IR_OP_ASSIGN:
  case IR_OP_SELECT:
    return in->dest.kind == IR_OPERAND_TEMP;
  default:
    return 0;
  }
}

static int ptx_tile_references(const IRInstruction *in, const char *name) {
  if (in->dest.name && strcmp(in->dest.name, name) == 0) return 1;
  for (size_t a = 0; a < in->argument_count; a++) {
    if (in->arguments[a].name && strcmp(in->arguments[a].name, name) == 0)
      return 1;
  }
  return 0;
}

static void ptx_tile_plan(PtxFn *fn, const IRFunction *func) {
  size_t n = func->instruction_count;
  if (!fn->tile_count || !n) return;
  fn->tile_role = calloc(n, 1);
  fn->tile_window_start = calloc(n, sizeof(size_t));
  fn->tile_role_count = n;
  if (!fn->tile_role || !fn->tile_window_start) {
    fn_error(fn, "PTX: out of memory planning tiles");
    return;
  }
  for (size_t i = 0; i < n;) {
    const IRInstruction *in = &func->instructions[i];
    const MtlcType *shape;
    size_t last = i;
    if (!ptx_tile_window_member(in, NULL)) {
      i++;
      continue;
    }
    shape = in->value_type;
    for (size_t j = i + 1; j < n; j++) {
      const IRInstruction *next = &func->instructions[j];
      if (ptx_tile_window_member(next, shape)) {
        last = j;
        continue;
      }
      if (next->op == IR_OP_TILE && ptx_tile_reduce_op(next->text) &&
          shape->view_extents[1] && next->argument_types &&
          next->argument_count > 1 &&
          ptx_tile_same_shape(shape, next->argument_types[1])) {
        last = j;
        break;
      }
      if (ptx_tile_scalar_passthrough(next)) continue;
      break;
    }
    for (size_t k = i; k <= last; k++) {
      if (func->instructions[k].op == IR_OP_TILE) {
        fn->tile_role[k] = 1;
      }
    }
    fn->tile_role[last] = 2;
    fn->tile_window_start[last] = i;
    for (size_t t = 0; t < fn->tile_count; t++) {
      PtxTile *tile = &fn->tiles[t];
      int inside = 0, outside = 0, written = 0;
      if (!tile->temporary || tile->layout != MTLC_VIEW_LAYOUT_FRAGMENT_C)
        continue;
      for (size_t k = 0; k < n && !outside; k++) {
        const IRInstruction *x = &func->instructions[k];
        if (x->op == IR_OP_ADDRESS_SPACE_ALLOC) continue;
        if (!ptx_tile_references(x, tile->name)) continue;
        if (k >= i && k <= last && x->op == IR_OP_TILE) {
          if (x->argument_count && x->arguments[0].name &&
              strcmp(x->arguments[0].name, tile->name) == 0) {
            if (written) outside = 1;
            written = 1;
          } else if (!written) {
            outside = 1;
          }
          inside = 1;
        } else {
          outside = 1;
        }
      }
      if (inside && !outside && written) {
        tile->scalarized = 1;
      }
    }
    i = last + 1;
  }
}

typedef struct {
  const IRInstruction *in;
  PtxTile *dest;
  PtxTileArg args[3];
  size_t count;
  PtxClass operand_cls;
  int reduce;
  int sum;
} PtxTileStep;

static void ptx_tile_value_at(const PtxTileArg *arg, int mt, int nt, int e,
                              int vector_dest, char *out) {
  if (arg->tile && arg->tile->scalarized) {
    snprintf(out, 24, "%s", arg->tile->current);
    return;
  }
  ptx_tile_arg_element(arg, mt, nt, e, vector_dest, out);
}

static void ptx_tile_emit_window(PtxFn *fn, const IRFunction *func,
                                 size_t start, size_t last) {
  PtxTileStep steps[64];
  IRInstruction views[64];
  size_t count = 0;
  const MtlcType *shape = NULL;
  for (size_t k = start; k <= last; k++) {
    const IRInstruction *in;
    PtxTileStep *step;
    if (func->instructions[k].op != IR_OP_TILE) continue;
    if (count < sizeof(views) / sizeof(views[0])) {
      views[count] = ptx_tile_view(&func->instructions[k]);
    }
    in = &views[count < sizeof(views) / sizeof(views[0]) ? count : 0];
    if (count == sizeof(steps) / sizeof(steps[0])) {
      fn_error(fn, "PTX: a run of tile operations is longer than %zu",
               sizeof(steps) / sizeof(steps[0]));
      return;
    }
    step = &steps[count++];
    memset(step, 0, sizeof(*step));
    step->in = in;
    step->dest = ptx_tile_operand(fn, &in->dest);
    step->reduce = ptx_tile_reduce_op(in->text);
    step->sum = step->reduce && strcmp(in->text, "row_sum") == 0;
    if (!step->dest) {
      fn_error(fn, "PTX: tile operation '%s' has no destination tile",
               in->text ? in->text : "?");
      return;
    }
    if (step->reduce) {
      step->args[0].tile = ptx_tile_operand(fn, &in->arguments[0]);
      step->count = 1;
      step->operand_cls = step->args[0].tile ? step->args[0].tile->cls : PC_F32;
      continue;
    }
    if (!shape) shape = in->value_type;
    step->operand_cls = ptx_tile_operand_class(in, fn, step->dest);
    step->count = in->argument_count < 3 ? in->argument_count : 3;
    for (size_t a = 0; a < step->count; a++) {
      PtxClass want = step->operand_cls;
      if (!strcmp(in->text, "select") && a == 0) want = PC_PRED;
      if (!strcmp(in->text, "fill")) want = step->dest->cls;
      if (!ptx_tile_arg(fn, in, a, want, &step->args[a])) return;
    }
  }
  if (!shape) return;
  int vector = shape->view_extents[1] == 0;
  int mts = (int)shape->view_extents[0] / 16;
  int nts = vector ? 1 : (int)shape->view_extents[1] / 8;
  int es = vector ? 2 : 4;
  char group[24], pair[24];
  reg_name(PC_B32, fn->tile_group, group);
  reg_name(PC_B32, fn->tile_pair, pair);
  for (int mt = 0; mt < mts; mt++) {
    for (int nt = 0; nt < nts; nt++) {
      for (int e = 0; e < es; e++) {
        for (size_t s = 0; s < count; s++) {
          PtxTileStep *step = &steps[s];
          char d[24], v[3][24];
          if (step->reduce) {
            char acc[24], value[24];
            const char *ty = ptx_tile_type_suffix(step->operand_cls);
            ptx_tile_reg(step->dest, ptx_tile_vector_index(mt, e >> 1), acc);
            ptx_tile_value_at(&step->args[0], mt, nt, e, 0, value);
            if (nt == 0 && (e & 1) == 0) {
              sb_printf(&fn->body, "\tmov.%s %s, %s;\n",
                        step->operand_cls == PC_F32 ? "f32" : "b32", acc,
                        value);
            } else if (step->sum) {
              if (step->operand_cls == PC_F32)
                sb_printf(&fn->body, "\tadd.rn.f32 %s, %s, %s;\n", acc, acc,
                          value);
              else
                sb_printf(&fn->body, "\tadd.s32 %s, %s, %s;\n", acc, acc,
                          value);
            } else {
              sb_printf(&fn->body, "\tmax.%s %s, %s, %s;\n", ty, acc, acc,
                        value);
            }
            continue;
          }
          for (size_t a = 0; a < step->count; a++) {
            ptx_tile_value_at(&step->args[a], mt, nt, e, vector, v[a]);
          }
          if (step->dest->scalarized) {
            reg_name(step->dest->cls, new_reg(fn, step->dest->cls), d);
            snprintf(step->dest->current, sizeof(step->dest->current), "%s",
                     d);
          } else {
            ptx_tile_reg(step->dest,
                         vector ? ptx_tile_vector_index(mt, e)
                                : ptx_tile_c_index(step->dest, mt, nt, e),
                         d);
          }
          if (!strcmp(step->in->text, "row")) {
            sb_printf(&fn->body, "\tadd.s32 %s, %s, %d;\n", d, group,
                      mt * 16 + (vector ? e : (e >> 1)) * 8);
          } else if (!strcmp(step->in->text, "col")) {
            sb_printf(&fn->body, "\tadd.s32 %s, %s, %d;\n", d, pair,
                      nt * 8 + (e & 1));
          } else {
            ptx_tile_element(fn, step->in->text, step->dest->cls,
                             step->operand_cls, d, v[0],
                             step->count > 1 ? v[1] : "",
                             step->count > 2 ? v[2] : "");
          }
        }
      }
    }
  }
  for (size_t s = 0; s < count; s++) {
    PtxTileStep *step = &steps[s];
    const char *ty = ptx_tile_type_suffix(step->operand_cls);
    if (!step->reduce) continue;
    for (int mt = 0; mt < mts; mt++) {
      for (int h = 0; h < 2; h++) {
        char acc[24], other[24];
        ptx_tile_reg(step->dest, ptx_tile_vector_index(mt, h), acc);
        reg_name(step->operand_cls, new_reg(fn, step->operand_cls), other);
        for (int stride = 1; stride <= 2; stride <<= 1) {
          sb_printf(&fn->body,
                    "\tshfl.sync.bfly.b32 %s, %s, %d, 31, 0xffffffff;\n",
                    other, acc, stride);
          if (step->sum) {
            if (step->operand_cls == PC_F32)
              sb_printf(&fn->body, "\tadd.rn.f32 %s, %s, %s;\n", acc, acc,
                        other);
            else
              sb_printf(&fn->body, "\tadd.s32 %s, %s, %s;\n", acc, acc,
                        other);
          } else {
            sb_printf(&fn->body, "\tmax.%s %s, %s, %s;\n", ty, acc, acc,
                      other);
          }
        }
      }
    }
  }
}

static void ptx_emit_tile_view(PtxFn *fn, const IRInstruction *in);

static void ptx_emit_tile(PtxFn *fn, const IRInstruction *original) {
  IRInstruction view;
  size_t at = fn->function && fn->function->instructions
                  ? (size_t)(original - fn->function->instructions)
                  : SIZE_MAX;
  if (fn->tile_role && at < fn->tile_role_count && fn->tile_role[at]) {
    if (fn->tile_role[at] == 2) {
      ptx_tile_emit_window(fn, fn->function, fn->tile_window_start[at], at);
    }
    return;
  }
  view = ptx_tile_view(original);
  ptx_emit_tile_view(fn, &view);
}

static void ptx_emit_tile_view(PtxFn *fn, const IRInstruction *in) {
  const char *op = in->text ? in->text : "";
  PtxTile *dest;
  if (!strcmp(op, "load")) {
    ptx_tile_load(fn, in);
    return;
  }
  if (!strcmp(op, "store")) {
    ptx_tile_store(fn, in);
    return;
  }
  if (!strcmp(op, "mma")) {
    ptx_tile_mma(fn, in);
    return;
  }
  dest = ptx_tile_operand(fn, &in->dest);
  if (!dest) {
    fn_error(fn, "PTX: tile operation '%s' has no destination tile", op);
    return;
  }
  if (!strcmp(op, "row") || !strcmp(op, "col")) {
    ptx_tile_index(fn, dest, !strcmp(op, "col"));
    return;
  }
  if (!strcmp(op, "row_max") || !strcmp(op, "row_sum")) {
    PtxTile *source = ptx_tile_operand(fn, &in->arguments[0]);
    if (!source) {
      fn_error(fn, "PTX: row reduction of a non-tile");
      return;
    }
    ptx_tile_reduce(fn, dest, source, !strcmp(op, "row_sum"));
    return;
  }
  if (dest->layout != MTLC_VIEW_LAYOUT_FRAGMENT_C) {
    if (!strcmp(op, "fill")) {
      char scalar[24], packed[24];
      const IROperand *value = &in->arguments[0];
      if ((value->kind == IR_OPERAND_FLOAT && value->float_value == 0.0 &&
           !signbit(value->float_value)) ||
          (value->kind == IR_OPERAND_INT && value->int_value == 0)) {
        snprintf(packed, sizeof(packed), "0");
      } else {
        use_as(fn, value, PC_F32, scalar);
        reg_name(PC_B32, new_reg(fn, PC_B32), packed);
        ptx_tile_pack(fn, dest->type->base_type->kind, packed, scalar, scalar);
      }
      for (int i = 0; i < dest->count; i++) {
        char d[24];
        ptx_tile_reg(dest, i, d);
        sb_printf(&fn->body, "\tmov.b32 %s, %s;\n", d, packed);
      }
      return;
    }
    if (!strcmp(op, "copy")) {
      PtxTile *source = ptx_tile_operand(fn, &in->arguments[0]);
      for (int i = 0; source && i < dest->count; i++) {
        char d[24], s[24];
        ptx_tile_reg(dest, i, d);
        ptx_tile_reg(source, i, s);
        sb_printf(&fn->body, "\tmov.b32 %s, %s;\n", d, s);
      }
      if (!source) fn_error(fn, "PTX: tile copy from a non-tile");
      return;
    }
    if (strcmp(op, "cast") != 0) {
      fn_error(fn, "PTX: '%s' on a %s tile", op,
               dest->layout == MTLC_VIEW_LAYOUT_FRAGMENT_A ? "fragment_a"
                                                           : "fragment_b");
      return;
    }
  }
  if (!strcmp(op, "cast")) {
    PtxTile *source = ptx_tile_operand(fn, &in->arguments[0]);
    if (!source) {
      fn_error(fn, "PTX: tile cast of a non-tile");
      return;
    }
    ptx_tile_cast(fn, dest, source);
    return;
  }
  ptx_tile_elementwise(fn, in, dest);
}

static Sb g_ptx_tile_report;

const char *ptx_tile_report_text(void) {
  return g_ptx_tile_report.data ? g_ptx_tile_report.data : "";
}

static void ptx_tile_report(PtxFn *fn, const IRFunction *func) {
  size_t temporaries = 0;
  sb_printf(&g_ptx_tile_report,
            "  %s: peak %d of %d registers a work item, at line %llu\n",
            func->name ? func->name : "?", fn->tile_peak, fn->tile_budget,
            (unsigned long long)fn->tile_peak_line);
  if (fn->reported) {
    char headline[120];
    snprintf(headline, sizeof(headline),
             "peak %d of %d registers a work item, at line %llu",
             fn->tile_peak, fn->tile_budget,
             (unsigned long long)fn->tile_peak_line);
    ir_explain_device_note(func->name, func->location.line, "tiles",
                           "register tiles", IR_DEVICE_FACT, headline, NULL,
                           NULL, "register-tiles");
  }
  for (size_t t = 0; t < fn->tile_count; t++) {
    const PtxTile *tile = &fn->tiles[t];
    unsigned long long first = 0;
    unsigned long long last = 0;
    for (size_t i = tile->first;
         tile->first != SIZE_MAX && i <= tile->last && i < func->instruction_count;
         i++) {
      unsigned long long line =
          (unsigned long long)func->instructions[i].location.line;
      if (!line) continue;
      if (!first || line < first) first = line;
      if (line > last) last = line;
    }
    if (tile->temporary) {
      temporaries++;
      if (tile->scalarized) continue;
    }
    sb_printf(&g_ptx_tile_report,
              "    %-8s %-38s %4d registers, lines %llu..%llu\n",
              tile->temporary ? "(value)" : tile->name,
              tile->type && tile->type->name ? tile->type->name : "?",
              tile->count, first, last);
    if (fn->reported) {
      char subject[160], headline[240];
      if (tile->temporary) {
        snprintf(subject, sizeof(subject), "tile value");
      } else {
        snprintf(subject, sizeof(subject), "tile `%s`", tile->name);
      }
      snprintf(headline, sizeof(headline),
               "%d registers from line %llu to line %llu, %s", tile->count,
               first, last,
               tile->type && tile->type->name ? tile->type->name : "?");
      ir_explain_device_note(func->name, (size_t)first, "tiles", subject,
                             IR_DEVICE_FACT, headline, NULL, NULL,
                             "register-tiles");
    }
  }
  {
    size_t scalar = 0;
    for (size_t t = 0; t < fn->tile_count; t++) {
      if (fn->tiles[t].scalarized) scalar++;
    }
    if (scalar) {
      sb_printf(&g_ptx_tile_report,
                "    %zu of %zu intermediate tiles computed one element at a "
                "time, no registers held\n",
                scalar, temporaries);
      if (fn->reported) {
        char headline[160];
        snprintf(headline, sizeof(headline),
                 "%zu of %zu computed one element at a time, no registers "
                 "held",
                 scalar, temporaries);
        ir_explain_device_note(func->name, func->location.line, "tiles",
                               "intermediate tiles", IR_DEVICE_GOOD, headline,
                               NULL, NULL, "register-tiles");
      }
    }
  }
}

typedef struct {
  size_t start;
  size_t end;
} PtxTileLoop;

static void ptx_tile_note_use(PtxFn *fn, const IROperand *op, size_t at) {
  PtxTile *tile = ptx_tile_operand(fn, op);
  if (!tile) return;
  if (tile->first == SIZE_MAX || at < tile->first) tile->first = at;
  if (at > tile->last) tile->last = at;
}

static int ptx_tile_budget(PtxFn *fn, const IRFunction *func, int threads) {
  int budget = 255;
  PtxTileLoop *loops = NULL;
  size_t loop_count = 0;
  if (!fn->tile_count) return 1;
  if (threads > 0) {
    int per = (65536 / threads) / 8 * 8;
    if (per < budget) budget = per;
  }
  for (size_t i = 0; i < func->instruction_count; i++) {
    const IRInstruction *in = &func->instructions[i];
    if (in->op == IR_OP_TILE || in->op == IR_OP_ADDRESS_SPACE_ALLOC) {
      ptx_tile_note_use(fn, &in->dest, i);
      for (size_t a = 0; a < in->argument_count; a++) {
        ptx_tile_note_use(fn, &in->arguments[a], i);
      }
    }
    if ((in->op == IR_OP_JUMP || in->op == IR_OP_BRANCH_ZERO ||
         in->op == IR_OP_BRANCH_EQ) &&
        in->text) {
      for (size_t j = 0; j < i; j++) {
        const IRInstruction *label = &func->instructions[j];
        if (label->op == IR_OP_LABEL && label->text &&
            strcmp(label->text, in->text) == 0) {
          PtxTileLoop *grown =
              realloc(loops, (loop_count + 1) * sizeof(*loops));
          if (!grown) {
            free(loops);
            return 1;
          }
          loops = grown;
          loops[loop_count].start = j;
          loops[loop_count].end = i;
          loop_count++;
          break;
        }
      }
    }
  }
  int changed = 1;
  while (changed) {
    changed = 0;
    for (size_t t = 0; t < fn->tile_count; t++) {
      PtxTile *tile = &fn->tiles[t];
      for (size_t l = 0; l < loop_count; l++) {
        if (tile->first < loops[l].start && tile->last >= loops[l].start &&
            tile->last < loops[l].end) {
          tile->last = loops[l].end;
          changed = 1;
        }
      }
    }
  }
  free(loops);
  {
    int peak = 0;
    size_t peak_at = 0;
    for (size_t i = 0; i < func->instruction_count; i++) {
      int live = 0;
      for (size_t t = 0; t < fn->tile_count; t++) {
        const PtxTile *tile = &fn->tiles[t];
        if (!tile->scalarized && tile->first != SIZE_MAX &&
            tile->first <= i && i <= tile->last)
          live += tile->count;
      }
      if (live > peak) {
        peak = live;
        peak_at = i;
      }
    }
    fn->tile_peak = peak;
    fn->tile_budget = budget;
    fn->tile_peak_line = func->instructions[peak_at].location.line;
    ptx_tile_report(fn, func);
    if (peak > budget) {
      char list[384] = "";
      size_t used = 0;
      for (size_t t = 0; t < fn->tile_count && used < sizeof(list); t++) {
        const PtxTile *tile = &fn->tiles[t];
        if (!tile->scalarized && tile->first != SIZE_MAX &&
            tile->first <= peak_at && peak_at <= tile->last) {
          int more = snprintf(list + used, sizeof(list) - used, "%s%s %d",
                              used ? ", " : "", tile->name, tile->count);
          if (more > 0) used += (size_t)more;
        }
      }
      fn_error(fn,
               "G0001: kernel '%s' holds %d tile registers at line %llu, over "
               "its budget of %d registers a work item; tiles never spill. "
               "Live there: %s",
               func->name ? func->name : "?", peak,
               (unsigned long long)fn->tile_peak_line, budget, list);
      return 0;
    }
  }
  return 1;
}
