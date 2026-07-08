#include "ir_optimize_internal.h"
#include "../ir_optimize.h"

/* Fold reads of never-written global integer vars to their initializer
 * constant. main.c supplies the candidates (globals with literal integer
 * initializers, from the AST); this pass proves each one is never stored to
 * and never has its address taken anywhere in the program, then rewrites
 * every read operand to the constant. Turns `idx * NODE_BYTES` into a
 * strength-reducible constant multiply and removes the per-call global-cache
 * reload the MIR backend would otherwise emit.
 *
 * All candidates are checked and folded together in a fixed number of
 * whole-program sweeps: the per-candidate program scan was
 * O(candidates x instructions) and dominated the phase on real applications
 * with hundreds of constant globals. */

typedef struct {
  const char *name;
  long long value;
  int disqualified;
  size_t shadow_gen; /* == current function generation when shadowed there */
} IRRgCandidate;

typedef struct {
  IRRgCandidate *items;
  size_t count;
  size_t *buckets; /* slot+1; 0 = empty */
  size_t bucket_count;
} IRRgTable;

static IRRgCandidate *ir_rg_table_find(IRRgTable *table, const char *name) {
  if (!name) {
    return NULL;
  }
  size_t b = mettle_fnv1a_hash(name) & (table->bucket_count - 1);
  while (table->buckets[b]) {
    IRRgCandidate *cand = &table->items[table->buckets[b] - 1];
    if (strcmp(cand->name, name) == 0) {
      return cand;
    }
    b = (b + 1) & (table->bucket_count - 1);
  }
  return NULL;
}

/* Mark every candidate shadowed in `fn` (declared as a local or bound as a
 * parameter there) with the function's generation `gen`. A shadowing name
 * hides the global throughout the function: its writes don't disqualify and
 * its reads are never folded. */
static void ir_rg_mark_shadows(IRRgTable *table, const IRFunction *fn,
                               size_t gen) {
  for (size_t p = 0; p < fn->parameter_count; p++) {
    if (fn->parameter_names && fn->parameter_names[p]) {
      IRRgCandidate *cand = ir_rg_table_find(table, fn->parameter_names[p]);
      if (cand) {
        cand->shadow_gen = gen;
      }
    }
  }
  for (size_t i = 0; i < fn->instruction_count; i++) {
    const IRInstruction *ins = &fn->instructions[i];
    if (ins->op == IR_OP_DECLARE_LOCAL &&
        ins->dest.kind == IR_OPERAND_SYMBOL && ins->dest.name && ins->text) {
      IRRgCandidate *cand = ir_rg_table_find(table, ins->dest.name);
      if (cand) {
        cand->shadow_gen = gen;
      }
    }
  }
}

static void ir_rg_fold_operand(IRRgTable *table, IROperand *operand,
                               size_t gen, int *changed) {
  if (operand->kind != IR_OPERAND_SYMBOL || !operand->name) {
    return;
  }
  IRRgCandidate *cand = ir_rg_table_find(table, operand->name);
  if (!cand || cand->disqualified || cand->shadow_gen == gen) {
    return;
  }
  long long value = cand->value;
  ir_operand_destroy(operand);
  *operand = ir_operand_int(value);
  if (changed) {
    *changed = 1;
  }
}

int ir_fold_readonly_globals_pass(IRProgram *program,
                                  const IRGlobalIntConst *consts,
                                  size_t count, int *changed) {
  if (!program || !consts || count == 0) {
    return 1;
  }

  IRRgTable table = {0};
  table.items = calloc(count, sizeof(IRRgCandidate));
  size_t nb = 64;
  while (nb < count * 2) {
    nb *= 2;
  }
  table.buckets = calloc(nb, sizeof(size_t));
  if (!table.items || !table.buckets) {
    free(table.items);
    free(table.buckets);
    return 0;
  }
  table.bucket_count = nb;
  for (size_t c = 0; c < count; c++) {
    if (!consts[c].name || ir_rg_table_find(&table, consts[c].name)) {
      continue;
    }
    IRRgCandidate *cand = &table.items[table.count];
    cand->name = consts[c].name;
    cand->value = consts[c].value;
    cand->shadow_gen = 0; /* generations start at 1 */
    size_t b = mettle_fnv1a_hash(cand->name) & (nb - 1);
    while (table.buckets[b]) {
      b = (b + 1) & (nb - 1);
    }
    table.buckets[b] = ++table.count; /* slot index + 1 */
  }

  /* Disqualify: any address-of or write of a candidate (outside functions
   * that shadow it) anywhere in the program. */
  size_t gen = 0;
  for (size_t f = 0; f < program->function_count; f++) {
    const IRFunction *fn = program->functions[f];
    if (!fn) {
      continue;
    }
    gen++;
    ir_rg_mark_shadows(&table, fn, gen);
    for (size_t i = 0; i < fn->instruction_count; i++) {
      const IRInstruction *ins = &fn->instructions[i];
      if (ins->op == IR_OP_ADDRESS_OF && ins->lhs.kind == IR_OPERAND_SYMBOL &&
          ins->lhs.name) {
        IRRgCandidate *cand = ir_rg_table_find(&table, ins->lhs.name);
        if (cand && cand->shadow_gen != gen) {
          cand->disqualified = 1;
        }
      }
      if (ir_instruction_writes_destination(ins) &&
          ins->dest.kind == IR_OPERAND_SYMBOL && ins->dest.name) {
        IRRgCandidate *cand = ir_rg_table_find(&table, ins->dest.name);
        if (cand && cand->shadow_gen != gen) {
          cand->disqualified = 1;
        }
      }
    }
  }

  /* Fold every surviving candidate's reads. */
  for (size_t f = 0; f < program->function_count; f++) {
    IRFunction *fn = program->functions[f];
    if (!fn) {
      continue;
    }
    gen++;
    ir_rg_mark_shadows(&table, fn, gen);
    for (size_t i = 0; i < fn->instruction_count; i++) {
      IRInstruction *ins = &fn->instructions[i];
      if (ins->op == IR_OP_ADDRESS_OF) {
        continue;
      }
      ir_rg_fold_operand(&table, &ins->lhs, gen, changed);
      ir_rg_fold_operand(&table, &ins->rhs, gen, changed);
      for (size_t a = 0; a < ins->argument_count; a++) {
        ir_rg_fold_operand(&table, &ins->arguments[a], gen, changed);
      }
    }
  }

  free(table.items);
  free(table.buckets);
  return 1;
}
