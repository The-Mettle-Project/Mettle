/* --ml-opt: apply the native model's dispositions (NOP / COPY <src> / CONST <int>
 * / REWRITE <postfix>) to the IR after the classical optimizer.  */
#include "ir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int ir_rewrite_to_assign_int(IRInstruction *instruction, long long value,
                             int *changed);
int ir_function_insert_instruction(IRFunction *function, size_t index,
                                   const IRInstruction *instruction);
int ir_function_rebuild_cfg(IRFunction *function);

/* Hoist a >imm32 constant used 3+ times into a temp at function entry so the
 * allocator keeps it in a register instead of re-emitting a movabs at every use. */
static int fits_imm32(long long v) { return v >= -2147483648LL && v <= 2147483647LL; }

static void hoist_replace(IROperand *op, long long v, const char *tname) {
  if (op->kind == IR_OPERAND_INT && op->int_value == v) {
    ir_operand_destroy(op);
    *op = ir_operand_temp(tname);
  }
}

static int hoist_constants_fn(IRFunction *f) {
  if (!f) return 0;
  int hoisted = 0, tag = 0;
  for (int pass = 0; pass < 24; pass++) {
    /* find the most-used large constant not yet hoisted */
    long long best_v = 0; size_t best_n = 0;
    for (size_t i = 0; i < f->instruction_count; i++) {
      IRInstruction *in = &f->instructions[i];
      IROperand *ops[2] = {&in->lhs, &in->rhs};
      for (int k = 0; k < 2; k++) {
        if (ops[k]->kind != IR_OPERAND_INT || fits_imm32(ops[k]->int_value))
          continue;
        long long v = ops[k]->int_value;
        size_t n = 0;
        for (size_t j = 0; j < f->instruction_count; j++) {
          if (f->instructions[j].lhs.kind == IR_OPERAND_INT &&
              f->instructions[j].lhs.int_value == v) n++;
          if (f->instructions[j].rhs.kind == IR_OPERAND_INT &&
              f->instructions[j].rhs.int_value == v) n++;
        }
        if (n > best_n) { best_n = n; best_v = v; }
      }
    }
    if (best_n < 3) break;             /* nothing worth hoisting */
    char tname[32];
    snprintf(tname, sizeof(tname), "__kh%d", tag++);
    IRInstruction def = {0};
    def.op = IR_OP_ASSIGN;
    def.dest = ir_operand_temp(tname);
    def.lhs = ir_operand_int(best_v);
    def.rhs = ir_operand_none();
    if (!ir_function_insert_instruction(f, 0, &def)) {
      ir_operand_destroy(&def.dest); ir_operand_destroy(&def.lhs); break;
    }
    ir_operand_destroy(&def.dest); ir_operand_destroy(&def.lhs);
    for (size_t i = 1; i < f->instruction_count; i++) {
      hoist_replace(&f->instructions[i].lhs, best_v, tname);
      hoist_replace(&f->instructions[i].rhs, best_v, tname);
    }
    hoisted++;
  }
  if (hoisted) ir_function_rebuild_cfg(f);
  return hoisted;
}

int ir_hoist_constants(IRProgram *program) {
  if (!program) return 0;
  int n = 0;
  for (size_t i = 0; i < program->function_count; i++) {
    n += hoist_constants_fn(program->functions[i]);
  }
  return n;
}

static void redirect_operand(IROperand *op, const char *name,
                             int is_int, long long ival, const char *sname,
                             int is_symbol) {
  if (op->kind == IR_OPERAND_TEMP && op->name && strcmp(op->name, name) == 0) {
    ir_operand_destroy(op);
    if (is_int) {
      *op = ir_operand_int(ival);
    } else {
      *op = is_symbol ? ir_operand_symbol(sname) : ir_operand_temp(sname);
    }
  }
}

static void redirect_uses(IRFunction *f, const char *name, int is_int,
                          long long ival, const char *sname, int is_symbol) {
  for (size_t b = 0; b < f->block_count; b++) {
    IRBasicBlock *blk = &f->blocks[b];
    for (size_t j = 0; j < blk->instruction_count; j++) {
      IRInstruction *in = &blk->instructions[j];
      redirect_operand(&in->lhs, name, is_int, ival, sname, is_symbol);
      redirect_operand(&in->rhs, name, is_int, ival, sname, is_symbol);
      for (size_t a = 0; a < in->argument_count; a++) {
        redirect_operand(&in->arguments[a], name, is_int, ival, sname, is_symbol);
      }
    }
  }
}

static int defs_once(IRFunction *f, const char *name) {
  int n = 0;
  for (size_t b = 0; b < f->block_count; b++) {
    IRBasicBlock *blk = &f->blocks[b];
    for (size_t j = 0; j < blk->instruction_count; j++) {
      IROperand *d = &blk->instructions[j].dest;
      if (d->kind == IR_OPERAND_TEMP && d->name && strcmp(d->name, name) == 0) {
        n++;
      }
    }
  }
  return n == 1;
}

static IRFunction *find_func(IRProgram *p, const char *name) {
  for (size_t i = 0; i < p->function_count; i++) {
    if (p->functions[i] && p->functions[i]->name &&
        strcmp(p->functions[i]->name, name) == 0) {
      return p->functions[i];
    }
  }
  return NULL;
}

static IRInstruction *find_instr(IRFunction *f, size_t gidx) {
  for (size_t b = 0; b < f->block_count; b++) {
    IRBasicBlock *blk = &f->blocks[b];
    if (gidx >= blk->first_instruction &&
        gidx < blk->first_instruction + blk->instruction_count) {
      return &blk->instructions[gidx - blk->first_instruction];
    }
  }
  return NULL;
}

/* Apply one NOP/COPY/CONST disposition line in place. Returns 1 if it changed. */
static int apply_disp_line(IRProgram *program, const char *line) {
  char fname[256], kind[32], arg[256];
  long long gi = 0;
  int n = sscanf(line, "%255s %lld %31s %255s", fname, &gi, kind, arg);
  if (n < 3) {
    return 0;
  }
  IRFunction *fn = find_func(program, fname);
  if (!fn) {
    return 0;
  }
  IRInstruction *ins = find_instr(fn, (size_t)gi);
  if (!ins) {
    return 0;
  }
  if (strcmp(kind, "NOP") == 0) {
    ins->op = IR_OP_NOP;
    return 1;
  }
  if ((strcmp(kind, "COPY") == 0 || strcmp(kind, "CONST") == 0) &&
      n >= 4 && ins->dest.kind == IR_OPERAND_TEMP && ins->dest.name &&
      defs_once(fn, ins->dest.name)) {
    char dest[256];
    snprintf(dest, sizeof(dest), "%s", ins->dest.name);
    if (strcmp(kind, "CONST") == 0) {
      redirect_uses(fn, dest, 1, atoll(arg), NULL, 0);
    } else {
      int sym = (arg[0] == '@');
      redirect_uses(fn, dest, 0, 0, arg + 1, sym);
    }
    ins->op = IR_OP_NOP;
    return 1;
  }
  return 0;
}

int ml_gnn_run(const char *ir_dump_path, char **out_disp);

static int g_rw_tmp = 0;

static IROperand rw_operand(const char *tok) {
  if (tok[0] == '%') return ir_operand_temp(tok + 1);
  if (tok[0] == '@') return ir_operand_symbol(tok + 1);
  return ir_operand_int(atoll(tok));
}

/* Materialize a REWRITE postfix (RPN over ~ & | ^ << >> and operands) as new
 * instructions before gidx, the last writing the root's dest, then NOP the root. */
static int apply_rewrite(IRFunction *fn, size_t gidx, char *postfix) {
  if (gidx >= fn->instruction_count) return 0;
  IROperand root_dest = ir_operand_copy(&fn->instructions[gidx].dest);
  if (root_dest.kind != IR_OPERAND_TEMP) { ir_operand_destroy(&root_dest); return 0; }

  char *toks[64]; int nt = 0;
  for (char *t = strtok(postfix, " "); t && nt < 64; t = strtok(NULL, " ")) toks[nt++] = t;

  IROperand stack[64]; int sp = 0;
  IRInstruction ops[64]; int nops = 0; int ok = 1;
  for (int i = 0; i < nt && ok; i++) {
    char *t = toks[i];
    int is_shift = (t[0] == '<' && t[1] == '<') || (t[0] == '>' && t[1] == '>');
    if (strcmp(t, "~") == 0 || strcmp(t, "&") == 0 || strcmp(t, "|") == 0 ||
        strcmp(t, "^") == 0 || is_shift) {
      int unary = (t[0] == '~');
      if (sp < (unary ? 1 : 2 - is_shift) || nops >= 64) { ok = 0; break; }
      /* shift: a << count, count is the digits after the operator token */
      IROperand b = unary ? ir_operand_none()
                  : is_shift ? ir_operand_int(atoll(t + 2)) : stack[--sp];
      IROperand a = stack[--sp];
      char nm[24]; snprintf(nm, sizeof nm, "__rw%d", g_rw_tmp++);
      IRInstruction in; memset(&in, 0, sizeof in);
      in.op = unary ? IR_OP_UNARY : IR_OP_BINARY;
      in.text = is_shift ? strdup(t[0] == '<' ? "<<" : ">>") : strdup(t);
      in.dest = ir_operand_temp(nm);
      in.lhs = a; in.rhs = b;
      ops[nops++] = in;
      stack[sp++] = ir_operand_temp(nm);
    } else if (sp < 64) {
      stack[sp++] = rw_operand(t);
    } else { ok = 0; }
  }
  if (!ok || nops == 0 || sp != 1) {
    for (int i = 0; i < nops; i++) { ir_operand_destroy(&ops[i].dest); ir_operand_destroy(&ops[i].lhs); ir_operand_destroy(&ops[i].rhs); free(ops[i].text); }
    for (int i = 0; i < sp; i++) ir_operand_destroy(&stack[i]);
    ir_operand_destroy(&root_dest);
    return 0;
  }
  for (int i = 0; i < sp; i++) ir_operand_destroy(&stack[i]);
  ir_operand_destroy(&ops[nops - 1].dest);
  ops[nops - 1].dest = root_dest;                 /* final op writes the root dest */

  int good = 1;
  for (int i = 0; i < nops && good; i++)
    if (!ir_function_insert_instruction(fn, gidx + (size_t)i, &ops[i])) good = 0;
  if (good && gidx + (size_t)nops < fn->instruction_count)
    fn->instructions[gidx + (size_t)nops].op = IR_OP_NOP;
  for (int i = 0; i < nops; i++) { ir_operand_destroy(&ops[i].dest); ir_operand_destroy(&ops[i].lhs); ir_operand_destroy(&ops[i].rhs); free(ops[i].text); }
  ir_function_rebuild_cfg(fn);
  return good;
}

static const char *base_name(const char *p) {
  if (!p) return "";
  const char *b = p;
  for (const char *q = p; *q; q++)
    if (*q == '/' || *q == '\\') b = q + 1;
  return b;
}

/* Append source file:line to each explain record, resolved from the pristine
 * program (must run before any disposition shifts indices). */
static void annotate_explain(IRProgram *program) {
  FILE *in = fopen("_mlopt.explain", "rb");
  if (!in) {
    return;
  }
  fseek(in, 0, SEEK_END);
  long sz = ftell(in);
  fseek(in, 0, SEEK_SET);
  char *buf = sz > 0 ? malloc((size_t)sz + 1) : NULL;
  size_t got = buf ? fread(buf, 1, (size_t)sz, in) : 0;
  fclose(in);
  if (!buf) {
    return;
  }
  buf[got] = 0;
  FILE *out = fopen("_mlopt.explain", "wb");
  if (!out) {
    free(buf);
    return;
  }
  for (char *p = buf; *p;) {
    char *nl = strchr(p, '\n');
    size_t len = nl ? (size_t)(nl - p) : strlen(p);
    if (len) {
      char rec[1400];
      if (len >= sizeof(rec)) len = sizeof(rec) - 1;
      memcpy(rec, p, len);
      rec[len] = 0;
      char cpy[1400];
      snprintf(cpy, sizeof(cpy), "%s", rec);
      char *fn = strtok(cpy, "\t");
      char *gi = fn ? strtok(NULL, "\t") : NULL;
      size_t line = 0;
      const char *file = "";
      if (fn && gi) {
        IRFunction *f = find_func(program, fn);
        IRInstruction *ins = f ? find_instr(f, (size_t)atoll(gi)) : NULL;
        if (ins) {
          line = ins->location.line;
          file = base_name(ins->location.filename);
        }
      }
      fprintf(out, "%s\t%zu\t%s\n", rec, line, file);
    }
    if (!nl) break;
    p = nl + 1;
  }
  fclose(out);
  free(buf);
}

int ir_apply_ml_opt(IRProgram *program) {
  if (!program) {
    return 0;
  }
  const char *ir_path = "_mlopt.ir";
  FILE *f = fopen(ir_path, "w");
  if (!f) {
    return 0;
  }
  ir_program_dump(program, f);
  fclose(f);

  char *disp = NULL;
  const char *disp_override = getenv("METTLE_ML_DISP");
  if (disp_override) {
    FILE *od = fopen(disp_override, "rb");
    if (od) {
      fseek(od, 0, SEEK_END);
      long n = ftell(od);
      fseek(od, 0, SEEK_SET);
      disp = malloc(n + 1);
      size_t got = disp ? fread(disp, 1, n, od) : 0;
      if (disp) {
        disp[got] = 0;
      }
      fclose(od);
    }
    if (!disp) {
      return 0;
    }
  } else if (!ml_gnn_run(ir_path, &disp) || !disp) {
    return 0;
  }
  FILE *dd = fopen("_mlopt.disp", "w");
  if (dd) {
    fputs(disp, dd);
    fclose(dd);
  }
  annotate_explain(program);
  char *rw_fn[2048]; long long rw_gi[2048]; char *rw_body[2048]; int rw_n = 0;
  int applied = 0;
  char *p = disp;
  while (*p) {
    char *nl = strchr(p, '\n');
    if (nl) {
      *nl = 0;
    }
    if (*p) {
      char fname[256], kind[32]; long long gi = 0;
      if (sscanf(p, "%255s %lld %31s", fname, &gi, kind) == 3 &&
          strcmp(kind, "REWRITE") == 0 && rw_n < 2048) {
        const char *body = strstr(p, "REWRITE ");
        if (body) { rw_fn[rw_n] = strdup(fname); rw_gi[rw_n] = gi;
          rw_body[rw_n] = strdup(body + 8); rw_n++; }
      } else {
        applied += apply_disp_line(program, p);
      }
    }
    if (!nl) {
      break;
    }
    p = nl + 1;
  }
  for (int i = 0; i < rw_n; i++)
    for (int j = i + 1; j < rw_n; j++)
      if (rw_gi[j] > rw_gi[i]) {
        long long tg = rw_gi[i]; rw_gi[i] = rw_gi[j]; rw_gi[j] = tg;
        char *tf = rw_fn[i]; rw_fn[i] = rw_fn[j]; rw_fn[j] = tf;
        char *tb = rw_body[i]; rw_body[i] = rw_body[j]; rw_body[j] = tb;
      }
  for (int i = 0; i < rw_n; i++) {
    IRFunction *fn = find_func(program, rw_fn[i]);
    if (fn) applied += apply_rewrite(fn, (size_t)rw_gi[i], rw_body[i]);
    free(rw_fn[i]); free(rw_body[i]);
  }
  free(disp);
  return applied;
}
