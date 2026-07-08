/* --ml-opt: apply the native model's dispositions (NOP / COPY <src> / CONST <int>
 * / REWRITE <postfix>) to the IR after the classical optimizer, gating every
 * applied disposition through the reference-interpreter differential
 * (ir_verify_check_rewrite). The model proposes; the validator disposes. */
#include "ml_opt.h"
#include "ir_verify.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#define MLO_ISATTY _isatty
#define MLO_FILENO _fileno
#else
#include <unistd.h>
#define MLO_ISATTY isatty
#define MLO_FILENO fileno
#endif

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

/* ---------------- the dispositions ---------------- */

enum { MLK_NOP, MLK_COPY, MLK_CONST, MLK_REWRITE };
enum { MLV_PENDING, MLV_VALIDATED, MLV_PROVEN, MLV_REJECTED, MLV_SKIPPED };

typedef struct {
  char fn[256];
  long long gidx;
  int kind;
  char *arg;     /* COPY/CONST operand or REWRITE postfix (immutable here) */
  int applied;   /* changed the IR and currently stands */
  int verdict;   /* MLV_* */
} MLDisp;

static const char *mlv_name(int verdict) {
  switch (verdict) {
  case MLV_VALIDATED: return "validated";
  case MLV_PROVEN:    return "proven";
  case MLV_REJECTED:  return "rejected";
  default:            return "skipped";
  }
}

static const char *mlk_name(int kind) {
  switch (kind) {
  case MLK_NOP:     return "NOP";
  case MLK_COPY:    return "COPY";
  case MLK_CONST:   return "CONST";
  default:          return "REWRITE";
  }
}

/* NOP dispositions are the model's speculative dead-code deletes: they carry
 * no construction-time proof and stand only when the validator can check the
 * function. COPY/CONST/REWRITE come from sound transforms (GVN dataflow,
 * collapse probing, truth-table/GF(2) superoptimization). */
static int disp_speculative(const MLDisp *d) { return d->kind == MLK_NOP; }

static MLDisp *parse_disps(char *text, int *out_n) {
  int cap = 64, n = 0;
  MLDisp *d = calloc((size_t)cap, sizeof(MLDisp));
  if (!d) { *out_n = 0; return NULL; }
  for (char *p = text; *p;) {
    char *nl = strchr(p, '\n');
    if (nl) *nl = 0;
    char fname[256], kind[32];
    long long gi = 0;
    int consumed = 0;
    if (*p &&
        sscanf(p, "%255s %lld %31s %n", fname, &gi, kind, &consumed) >= 3) {
      if (n == cap) {
        MLDisp *grown = realloc(d, (size_t)cap * 2 * sizeof(MLDisp));
        if (!grown) break;
        memset(grown + cap, 0, (size_t)cap * sizeof(MLDisp));
        d = grown; cap *= 2;
      }
      MLDisp *m = &d[n];
      snprintf(m->fn, sizeof(m->fn), "%s", fname);
      m->gidx = gi;
      m->verdict = MLV_PENDING;
      m->arg = strdup(p + consumed);
      if (strcmp(kind, "NOP") == 0) m->kind = MLK_NOP;
      else if (strcmp(kind, "COPY") == 0) m->kind = MLK_COPY;
      else if (strcmp(kind, "CONST") == 0) m->kind = MLK_CONST;
      else if (strcmp(kind, "REWRITE") == 0) m->kind = MLK_REWRITE;
      else { free(m->arg); m->arg = NULL; }
      if (m->arg) {
        char *end = m->arg + strlen(m->arg);
        while (end > m->arg && (end[-1] == '\r' || end[-1] == ' ')) *--end = 0;
        n++;
      }
    }
    if (!nl) break;
    p = nl + 1;
  }
  *out_n = n;
  return d;
}

/* METTLE_ML_SABOTAGE: corrupt the first COPY/CONST disposition into a wrong
 * constant, proving end to end that the validator catches and discards a bad
 * model proposal - the ml-opt twin of METTLE_VERIFY_BREAK. */
static void maybe_sabotage(MLDisp *d, int n) {
  const char *spec = getenv("METTLE_ML_SABOTAGE");
  if (!spec || !spec[0] || strcmp(spec, "0") == 0) {
    return;
  }
  for (int i = 0; i < n; i++) {
    if (d[i].kind == MLK_COPY || d[i].kind == MLK_CONST) {
      free(d[i].arg);
      d[i].kind = MLK_CONST;
      d[i].arg = strdup("271828");
      fprintf(stderr,
              "ml-opt: SABOTAGE armed: disposition for '%s' ir#%lld forced to "
              "CONST 271828\n",
              d[i].fn, d[i].gidx);
      return;
    }
  }
}

/* Apply one disposition. Returns 1 when the IR changed (the disposition now
 * stands until validation says otherwise), 0 when the applier declined. */
static int apply_one(IRFunction *fn, const MLDisp *d) {
  if (!fn->cfg_valid && !ir_function_rebuild_cfg(fn)) {
    return 0;
  }
  if (d->kind == MLK_REWRITE) {
    char *scratch = strdup(d->arg); /* apply_rewrite tokenizes destructively */
    if (!scratch) return 0;
    int changed = apply_rewrite(fn, (size_t)d->gidx, scratch);
    free(scratch);
    return changed;
  }
  IRInstruction *ins = find_instr(fn, (size_t)d->gidx);
  if (!ins) {
    return 0;
  }
  if (d->kind == MLK_NOP) {
    /* Deleting control flow would leave the CFG lying about the stream;
     * the model has no business proposing it and the applier refuses. */
    if (ins->op == IR_OP_NOP || ins->op == IR_OP_LABEL ||
        ins->op == IR_OP_JUMP || ins->op == IR_OP_BRANCH_ZERO ||
        ins->op == IR_OP_BRANCH_EQ || ins->op == IR_OP_RETURN) {
      return 0;
    }
    ins->op = IR_OP_NOP;
    return 1;
  }
  if (ins->dest.kind != IR_OPERAND_TEMP || !ins->dest.name ||
      !defs_once(fn, ins->dest.name)) {
    return 0;
  }
  char dest[256];
  snprintf(dest, sizeof(dest), "%s", ins->dest.name);
  if (d->kind == MLK_CONST) {
    redirect_uses(fn, dest, 1, atoll(d->arg), NULL, 0);
  } else {
    int sym = (d->arg[0] == '@');
    redirect_uses(fn, dest, 0, 0, d->arg + 1, sym);
  }
  ins->op = IR_OP_NOP;
  return 1;
}

static int mlo_color(void) {
  static int cached = -1;
  if (cached < 0) {
    const char *no_color = getenv("NO_COLOR");
    cached = (!no_color || !no_color[0]) && MLO_ISATTY(MLO_FILENO(stderr));
  }
  return cached;
}

static void report_rejection(const MLDisp *d, const char *cex,
                             const char *why) {
  const char *red = mlo_color() ? "\x1b[31m\x1b[1m" : "";
  const char *cyan = mlo_color() ? "\x1b[36m" : "";
  const char *reset = mlo_color() ? "\x1b[0m" : "";
  fprintf(stderr,
          "\n%sml-opt: PROPOSAL REJECTED%s: model rewrite '%s ir#%lld' changed "
          "the observable behavior of function '%s'\n",
          red, reset, mlk_name(d->kind), d->gidx, d->fn);
  fprintf(stderr, "  %scounterexample%s %s\n", cyan, reset, cex);
  fprintf(stderr, "  %sdivergence%s: %s\n", cyan, reset, why);
  fprintf(stderr,
          "  %saction%s: proposal discarded; '%s' keeps its validated IR\n",
          cyan, reset, d->fn);
}

/* Restore the pre-disposition IR and leave the function in an appliable
 * state (blocks rebuilt: apply_one and redirect_uses walk fn->blocks). */
static void restore_fn(IRFunction *fn, const IRVerifySnapshot *snap) {
  if (ir_verify_snapshot_restore(fn, snap)) {
    ir_function_rebuild_cfg(fn);
  }
}

/* Validate the disposition group of one function. `idx`/`count` index into
 * `d` in apply order (non-REWRITE first in model order, then REWRITEs by
 * descending gidx so insertions never shift a pending target). */
static void run_function_group(IRProgram *program, IRFunction *fn, MLDisp *d,
                               const int *idx, int count, MLOptStats *stats) {
  IRVerifySnapshot *snap = ir_verify_snapshot_capture(fn);
  char why[192], cex[320], skip[160];

  if (!snap) {
    /* Function too large (or empty) to snapshot: no gate possible. Proven
     * dispositions stand on their proofs; speculative ones never stand. */
    for (int i = 0; i < count; i++) {
      MLDisp *m = &d[idx[i]];
      if (disp_speculative(m)) {
        m->verdict = MLV_SKIPPED;
        stats->skipped++;
      } else if (apply_one(fn, m)) {
        m->applied = 1;
        m->verdict = MLV_PROVEN;
        stats->proven++;
      } else {
        m->verdict = MLV_SKIPPED;
        stats->skipped++;
      }
    }
    return;
  }

  int applied_any = 0;
  for (int i = 0; i < count; i++) {
    MLDisp *m = &d[idx[i]];
    m->applied = apply_one(fn, m);
    if (!m->applied) {
      m->verdict = MLV_SKIPPED;
      stats->skipped++;
    }
    applied_any |= m->applied;
  }
  if (!applied_any) {
    ir_verify_snapshot_free(snap);
    return;
  }

  IRVerifyRewriteVerdict verdict = ir_verify_check_rewrite(
      program, fn, snap, why, sizeof(why), cex, sizeof(cex), skip,
      sizeof(skip));

  if (verdict == IR_VERIFY_REWRITE_VALIDATED) {
    for (int i = 0; i < count; i++) {
      if (d[idx[i]].applied) {
        d[idx[i]].verdict = MLV_VALIDATED;
        stats->validated++;
      }
    }
    ir_verify_snapshot_free(snap);
    return;
  }

  if (verdict == IR_VERIFY_REWRITE_UNVERIFIABLE) {
    /* The gate cannot run this function. Proven dispositions stand; any
     * applied speculative one must come back out, which means restoring and
     * re-applying only the proven ones. */
    int has_spec = 0;
    for (int i = 0; i < count; i++) {
      if (d[idx[i]].applied && disp_speculative(&d[idx[i]])) has_spec = 1;
    }
    if (has_spec) {
      restore_fn(fn, snap);
      for (int i = 0; i < count; i++) {
        MLDisp *m = &d[idx[i]];
        if (!m->applied) continue;
        if (disp_speculative(m)) {
          m->applied = 0;
          m->verdict = MLV_SKIPPED;
          stats->skipped++;
        } else {
          m->applied = apply_one(fn, m);
          m->verdict = m->applied ? MLV_PROVEN : MLV_SKIPPED;
          if (m->applied) stats->proven++; else stats->skipped++;
        }
      }
    } else {
      for (int i = 0; i < count; i++) {
        if (d[idx[i]].applied) {
          d[idx[i]].verdict = MLV_PROVEN;
          stats->proven++;
        }
      }
    }
    ir_verify_snapshot_free(snap);
    return;
  }

  /* Divergence. Roll everything back and bisect: re-apply one disposition at
   * a time, each against a fresh snapshot, so exactly the offending
   * proposal(s) are named and discarded while the innocent ones stand. */
  restore_fn(fn, snap);
  ir_verify_snapshot_free(snap);
  for (int i = 0; i < count; i++) {
    MLDisp *m = &d[idx[i]];
    if (!m->applied) continue; /* already counted as skipped */
    m->applied = 0;
    IRVerifySnapshot *one = ir_verify_snapshot_capture(fn);
    if (!one) {
      m->verdict = MLV_SKIPPED;
      stats->skipped++;
      continue;
    }
    if (!apply_one(fn, m)) {
      m->verdict = MLV_SKIPPED;
      stats->skipped++;
      ir_verify_snapshot_free(one);
      continue;
    }
    switch (ir_verify_check_rewrite(program, fn, one, why, sizeof(why), cex,
                                    sizeof(cex), skip, sizeof(skip))) {
    case IR_VERIFY_REWRITE_VALIDATED:
      m->applied = 1;
      m->verdict = MLV_VALIDATED;
      stats->validated++;
      break;
    case IR_VERIFY_REWRITE_DIVERGED:
      report_rejection(m, cex, why);
      restore_fn(fn, one);
      m->verdict = MLV_REJECTED;
      stats->rejected++;
      break;
    default: /* function became unverifiable mid-bisect */
      if (disp_speculative(m)) {
        restore_fn(fn, one);
        m->verdict = MLV_SKIPPED;
        stats->skipped++;
      } else {
        m->applied = 1;
        m->verdict = MLV_PROVEN;
        stats->proven++;
      }
      break;
    }
    ir_verify_snapshot_free(one);
  }
}

/* ---------------- explain-record annotation ---------------- */

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

/* After validation, append each record's verdict (matched by fn+gidx) so the
 * --explain report can say which rewrites stood and which were rejected. */
static void append_verdicts(const MLDisp *d, int n) {
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
      char rec[1500];
      if (len >= sizeof(rec)) len = sizeof(rec) - 1;
      memcpy(rec, p, len);
      rec[len] = 0;
      char cpy[1500];
      snprintf(cpy, sizeof(cpy), "%s", rec);
      char *fn = strtok(cpy, "\t");
      char *gi = fn ? strtok(NULL, "\t") : NULL;
      const char *verdict = "skipped";
      if (fn && gi) {
        long long g = atoll(gi);
        for (int i = 0; i < n; i++) {
          if (d[i].gidx == g && strcmp(d[i].fn, fn) == 0) {
            verdict = mlv_name(d[i].verdict);
            break;
          }
        }
      }
      fprintf(out, "%s\t%s\n", rec, verdict);
    }
    if (!nl) break;
    p = nl + 1;
  }
  fclose(out);
  free(buf);
}

/* ---------------- entry ---------------- */

int ir_apply_ml_opt(IRProgram *program, MLOptStats *stats) {
  MLOptStats local;
  if (!stats) stats = &local;
  memset(stats, 0, sizeof(*stats));
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

  int n = 0;
  MLDisp *d = parse_disps(disp, &n);
  free(disp);
  if (!d) {
    return 0;
  }
  maybe_sabotage(d, n);
  stats->proposals = n;

  /* Group by function, preserving first-seen order. Apply order inside a
   * group: non-REWRITEs in model order (in-place, index-stable), then
   * REWRITEs by descending gidx (insertions never shift a pending target). */
  int *idx = malloc(n ? (size_t)n * sizeof(int) : sizeof(int));
  char *done = calloc(n ? (size_t)n : 1, 1);
  if (idx && done) {
    for (int i = 0; i < n; i++) {
      if (done[i]) continue;
      int count = 0;
      for (int j = i; j < n; j++) {
        if (!done[j] && strcmp(d[j].fn, d[i].fn) == 0 &&
            d[j].kind != MLK_REWRITE) {
          idx[count++] = j;
          done[j] = 1;
        }
      }
      int rw_start = count;
      for (int j = i; j < n; j++) {
        if (!done[j] && strcmp(d[j].fn, d[i].fn) == 0) {
          idx[count++] = j;
          done[j] = 1;
        }
      }
      for (int a = rw_start; a < count; a++) {
        for (int b = a + 1; b < count; b++) {
          if (d[idx[b]].gidx > d[idx[a]].gidx) {
            int t = idx[a]; idx[a] = idx[b]; idx[b] = t;
          }
        }
      }
      IRFunction *fn = find_func(program, d[i].fn);
      if (!fn) {
        for (int a = 0; a < count; a++) {
          d[idx[a]].verdict = MLV_SKIPPED;
          stats->skipped++;
        }
        continue;
      }
      run_function_group(program, fn, d, idx, count, stats);
    }
  }
  free(idx);
  free(done);

  append_verdicts(d, n);
  for (int i = 0; i < n; i++) {
    free(d[i].arg);
  }
  free(d);
  return stats->validated + stats->proven;
}
