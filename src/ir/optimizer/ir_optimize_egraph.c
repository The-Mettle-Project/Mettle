#include "ir_optimize_internal.h"

/* ============================================================================
 * E-class pilot over straight-line integer arithmetic.
 *
 * A destructive rewrite commits to one form and loses the others; an e-graph
 * keeps every equivalent form in one class and lets extraction pick per use.
 * This pass is the reversible pilot of that idea: it builds e-classes for the
 * temp dataflow of straight-line integer regions, saturates them with the
 * same algebraic identities the rewrite table applies destructively
 * (commutativity, association with constants, folding, the x+0/x*1/x*2^k
 * family), and rewrites a definition only when extraction finds a strictly
 * cheaper form. Everything else is left exactly as written.
 *
 * Gated on METTLE_EGRAPH=1 and registered as an ordinary fixpoint pass, so
 * every change it makes runs under the translation-validation snapshot
 * harness (--verify), and METTLE_SKIP_PASS=egraph_simplify turns it off.
 *
 * What it measured, which is the point of building it as a pilot: on IR the
 * ordinary pipeline has already optimized, saturation finds almost nothing.
 * The destructive passes reach the easy equivalences first, and what is left
 * is rarely a cheaper form of a single instruction. Disabling reassociation
 * makes it visible -- the non-adjacent constant chains the const-chain table
 * cannot see collapse here -- which is the honest shape of the result: the
 * value of an e-graph in this compiler is subsuming the destructive rules and
 * extracting per target, not finding extra folds on x86. Keep that in mind
 * before spending a quarter on the full version.
 * ==========================================================================*/

#define EG_MAX_NODES 512
#define EG_MAX_LEAVES 128
#define EG_SATURATE_ROUNDS 4

typedef enum {
  EG_CONST,
  EG_LEAF, /* a symbol or a temp defined outside the region */
  EG_BIN
} EgKind;

typedef struct {
  EgKind kind;
  long long value;   /* EG_CONST */
  int leaf;          /* EG_LEAF: index into the leaf name table */
  char op;           /* EG_BIN: '+','-','*','&','|','^','<','>' for shifts see op2 */
  char op2;          /* second char of "<<"/">>" or 0 */
  int a, b;          /* EG_BIN: child e-class ids */
} EgNode;

typedef struct {
  EgNode nodes[EG_MAX_NODES];
  int node_class[EG_MAX_NODES]; /* e-class of each node */
  int node_count;
  int parent[EG_MAX_NODES]; /* union-find over class ids */
  const char *leaves[EG_MAX_LEAVES];
  int leaf_count;
} EGraph;

static int eg_find(EGraph *g, int c) {
  while (g->parent[c] != c) {
    g->parent[c] = g->parent[g->parent[c]];
    c = g->parent[c];
  }
  return c;
}

static void eg_union(EGraph *g, int a, int b) {
  a = eg_find(g, a);
  b = eg_find(g, b);
  if (a != b) {
    g->parent[b] = a;
  }
}

static int eg_leaf_id(EGraph *g, const char *name) {
  for (int i = 0; i < g->leaf_count; i++) {
    if (strcmp(g->leaves[i], name) == 0) {
      return i;
    }
  }
  if (g->leaf_count >= EG_MAX_LEAVES) {
    return -1;
  }
  g->leaves[g->leaf_count] = name;
  return g->leaf_count++;
}

static int eg_node_equal(EGraph *g, const EgNode *n, const EgNode *m) {
  if (n->kind != m->kind) {
    return 0;
  }
  switch (n->kind) {
  case EG_CONST:
    return n->value == m->value;
  case EG_LEAF:
    return n->leaf == m->leaf;
  case EG_BIN:
    return n->op == m->op && n->op2 == m->op2 &&
           eg_find(g, n->a) == eg_find(g, m->a) &&
           eg_find(g, n->b) == eg_find(g, m->b);
  }
  return 0;
}

/* Hashcons: add the node, returning the e-class of an existing equal node
 * when there is one (congruence), a fresh class otherwise. -1 on overflow. */
static int eg_add(EGraph *g, const EgNode *n) {
  for (int i = 0; i < g->node_count; i++) {
    if (eg_node_equal(g, &g->nodes[i], n)) {
      return eg_find(g, g->node_class[i]);
    }
  }
  if (g->node_count >= EG_MAX_NODES) {
    return -1;
  }
  int id = g->node_count++;
  g->nodes[id] = *n;
  g->node_class[id] = id;
  g->parent[id] = id;
  return id;
}

static int eg_add_const(EGraph *g, long long v) {
  EgNode n;
  memset(&n, 0, sizeof(n));
  n.kind = EG_CONST;
  n.value = v;
  return eg_add(g, &n);
}

static int eg_add_bin(EGraph *g, char op, char op2, int a, int b) {
  EgNode n;
  memset(&n, 0, sizeof(n));
  n.kind = EG_BIN;
  n.op = op;
  n.op2 = op2;
  n.a = a;
  n.b = b;
  return eg_add(g, &n);
}

/* The class's constant value, when any node in it is a constant. */
static int eg_class_const(EGraph *g, int c, long long *out) {
  c = eg_find(g, c);
  for (int i = 0; i < g->node_count; i++) {
    if (eg_find(g, g->node_class[i]) == c && g->nodes[i].kind == EG_CONST) {
      *out = g->nodes[i].value;
      return 1;
    }
  }
  return 0;
}

static int eg_fold(char op, char op2, long long x, long long y, long long *out) {
  switch (op) {
  case '+': *out = x + y; return 1;
  case '-': *out = x - y; return 1;
  case '*': *out = x * y; return 1;
  case '&': *out = x & y; return 1;
  case '|': *out = x | y; return 1;
  case '^': *out = x ^ y; return 1;
  case '<':
    if (op2 == '<' && y >= 0 && y < 64) { *out = x << y; return 1; }
    return 0;
  default:
    return 0;
  }
}

static int eg_is_commutative(char op, char op2) {
  return op2 == 0 && (op == '+' || op == '*' || op == '&' || op == '|' ||
                      op == '^');
}

/* One saturation round: for every BIN node, offer its equivalent forms and
 * union them into its class. The identities mirror the destructive table in
 * ir_optimize_rewrite.c; here both forms stay alive. Returns 1 when any
 * union or new node changed the graph. */
static int eg_saturate_round(EGraph *g) {
  int changed = 0;
  int count = g->node_count; /* nodes added this round wait for the next */
  for (int i = 0; i < count; i++) {
    EgNode n = g->nodes[i];
    if (n.kind != EG_BIN) {
      continue;
    }
    int cls = eg_find(g, g->node_class[i]);
    long long ca, cb;
    int has_a = eg_class_const(g, n.a, &ca);
    int has_b = eg_class_const(g, n.b, &cb);

    /* Constant folding. */
    if (has_a && has_b) {
      long long v;
      if (eg_fold(n.op, n.op2, ca, cb, &v)) {
        int c = eg_add_const(g, v);
        if (c >= 0 && eg_find(g, c) != cls) {
          eg_union(g, cls, c);
          changed = 1;
        }
      }
    }

    /* Commutativity. */
    if (eg_is_commutative(n.op, n.op2)) {
      int c = eg_add_bin(g, n.op, 0, n.b, n.a);
      if (c >= 0 && eg_find(g, c) != eg_find(g, cls)) {
        eg_union(g, cls, c);
        changed = 1;
      }
    }

    /* Identity elements: x+0, x-0, x*1, x&-1, x|0, x^0, x<<0, x>>0. */
    if (has_b &&
        ((cb == 0 && (n.op == '+' || n.op == '-' || n.op == '|' ||
                      n.op == '^' || n.op2 != 0)) ||
         (cb == 1 && n.op == '*' && n.op2 == 0) ||
         (cb == -1 && n.op == '&'))) {
      if (eg_find(g, n.a) != eg_find(g, cls)) {
        eg_union(g, cls, n.a);
        changed = 1;
      }
    }
    /* Annihilators: x*0, x&0. */
    if (has_b && cb == 0 && (n.op == '*' || n.op == '&') && n.op2 == 0) {
      int c = eg_add_const(g, 0);
      if (c >= 0 && eg_find(g, c) != eg_find(g, cls)) {
        eg_union(g, cls, c);
        changed = 1;
      }
    }
    /* x - x and x ^ x. */
    if ((n.op == '-' || n.op == '^') && n.op2 == 0 &&
        eg_find(g, n.a) == eg_find(g, n.b)) {
      int c = eg_add_const(g, 0);
      if (c >= 0 && eg_find(g, c) != eg_find(g, cls)) {
        eg_union(g, cls, c);
        changed = 1;
      }
    }

    /* Association with a constant: (x + c1) + c2 = x + (c1 + c2), and the
     * multiplicative twin. This is the reassociation the const-chain table
     * does destructively, without needing the two instructions adjacent. */
    if ((n.op == '+' || n.op == '*') && n.op2 == 0 && has_b) {
      int ac = eg_find(g, n.a);
      for (int j = 0; j < count; j++) {
        if (eg_find(g, g->node_class[j]) != ac ||
            g->nodes[j].kind != EG_BIN || g->nodes[j].op != n.op ||
            g->nodes[j].op2 != 0) {
          continue;
        }
        long long inner;
        if (eg_class_const(g, g->nodes[j].b, &inner)) {
          long long merged;
          if (eg_fold(n.op, 0, inner, cb, &merged)) {
            int mc = eg_add_const(g, merged);
            int rebuilt = mc >= 0
                              ? eg_add_bin(g, n.op, 0, g->nodes[j].a, mc)
                              : -1;
            if (rebuilt >= 0 && eg_find(g, rebuilt) != eg_find(g, cls)) {
              eg_union(g, cls, rebuilt);
              changed = 1;
            }
          }
        }
        break;
      }
    }
  }
  return changed;
}

static int eg_class_leaf_node(EGraph *g, int c);

/* The cheapest node of the class whose children are leaves or constants (so
 * it can replace one instruction in place). Returns node index or -1. */
static int eg_class_best_shallow(EGraph *g, int c) {
  c = eg_find(g, c);
  int best = -1;
  int best_cost = 100000;
  for (int i = 0; i < g->node_count; i++) {
    if (eg_find(g, g->node_class[i]) != c) {
      continue;
    }
    const EgNode *n = &g->nodes[i];
    int cost = 0;
    if (n->kind == EG_BIN) {
      /* Both children have to extract as a leaf or a constant for this node
       * to replace one instruction in place. Asked through the same helper
       * the rewrite below uses to fetch them, so "there is one" and "here it
       * is" can never disagree. */
      if (eg_class_leaf_node(g, n->a) < 0 || eg_class_leaf_node(g, n->b) < 0) {
        continue;
      }
      cost = (n->op == '*' && n->op2 == 0) ? 4 : 1;
    }
    if (cost < best_cost) {
      best_cost = cost;
      best = i;
    }
  }
  return best;
}

/* A leaf/const representative node of the class, or -1. */
static int eg_class_leaf_node(EGraph *g, int c) {
  c = eg_find(g, c);
  for (int i = 0; i < g->node_count; i++) {
    if (eg_find(g, g->node_class[i]) == c && g->nodes[i].kind != EG_BIN) {
      return i;
    }
  }
  return -1;
}

static int eg_enabled(void) {
  static int cached = -1;
  if (cached < 0) {
    const char *spec = getenv("METTLE_EGRAPH");
    cached = (spec && spec[0] != '\0' && strcmp(spec, "0") != 0) ? 1 : 0;
  }
  return cached;
}

static char eg_op_char(const char *text, char *op2) {
  *op2 = 0;
  if (!text || !text[0]) {
    return 0;
  }
  if (text[1] == 0 &&
      (text[0] == '+' || text[0] == '-' || text[0] == '*' || text[0] == '&' ||
       text[0] == '|' || text[0] == '^')) {
    return text[0];
  }
  if (text[2] == 0 && text[0] == '<' && text[1] == '<') {
    *op2 = '<';
    return '<';
  }
  /* `>>` is excluded: the node key does not carry the instruction's
   * signedness, and merging an arithmetic shift with a logical one over the
   * same operands would be wrong. */
  return 0;
}

static IROperand eg_node_operand(EGraph *g, const EgNode *n) {
  if (n->kind == EG_CONST) {
    return ir_operand_int(n->value);
  }
  return ir_operand_symbol(g->leaves[n->leaf]);
}

static int eg_sym_in(const char **names, int count, const char *name) {
  for (int i = 0; i < count; i++) {
    if (strcmp(names[i], name) == 0) {
      return 1;
    }
  }
  return 0;
}

int ir_egraph_simplify_pass(IRFunction *function, int *changed) {
  if (!function || !changed) {
    return 0;
  }
  if (!eg_enabled()) {
    return 1;
  }

  EGraph *g = (EGraph *)calloc(1, sizeof(EGraph));
  if (!g) {
    return 0;
  }

  /* Name -> e-class bindings for the current straight-line region. Temps
   * bind at their def; symbols bind at each ASSIGN, SSA-style, with the
   * newest binding shadowing (lookups scan backward). A region breaks at
   * control flow, calls, loads, stores, and any write this pass cannot
   * model. */
  enum { EG_MAX_DEFS = 256 };
  struct {
    const char *name;
    int is_symbol;
    int cls;
    size_t at;
    int rewritable; /* integer BINARY temp defs get extraction */
  } defs[EG_MAX_DEFS];
  int def_count = 0;
  /* Symbols written inside the region: their leaf nodes denote the value at
   * region entry, so a form that references such a leaf may only replace a
   * def sitting before the first write. Cheaper to forbid than to date. */
  const char *written_syms[EG_MAX_DEFS];
  int written_count = 0;

  int any_changed = 0;

  for (size_t i = 0; i <= function->instruction_count; i++) {
    const IRInstruction *ins =
        i < function->instruction_count ? &function->instructions[i] : NULL;
    if (ins && ins->op == IR_OP_NOP) {
      continue; /* a nop neither feeds nor invalidates the region */
    }
    char op2 = 0;
    char opch =
        ins && ins->op == IR_OP_BINARY && !ins->is_float
            ? eg_op_char(ins->text, &op2)
            : 0;
    int dest_temp = ins && ins->dest.kind == IR_OPERAND_TEMP && ins->dest.name
                        ? 1
                        : 0;
    int dest_sym =
        ins && ins->dest.kind == IR_OPERAND_SYMBOL && ins->dest.name ? 1 : 0;
    int is_copy = ins && ins->op == IR_OP_ASSIGN && !ins->is_float &&
                  (dest_temp || dest_sym);
    int is_bin = opch != 0 && (dest_temp || dest_sym);
    int breaks = !ins || (!is_bin && !is_copy);

    if (breaks) {
      /* Region ends: extract. Rewrite any def whose class has a strictly
       * cheaper single-instruction form, unless the form reads a leaf whose
       * symbol was rebound inside the region. */
      for (int d = 0; d < def_count; d++) {
        int cls = defs[d].cls;
        if (!defs[d].rewritable) {
          continue;
        }
        IRInstruction *target = &function->instructions[defs[d].at];
        long long cv;
        if (eg_class_const(g, cls, &cv)) {
          if (target->op != IR_OP_ASSIGN ||
              target->lhs.kind != IR_OPERAND_INT ||
              target->lhs.int_value != cv) {
            ir_rewrite_to_assign_int(target, cv, &any_changed);
          }
          continue;
        }
        int leaf = eg_class_leaf_node(g, cls);
        if (leaf >= 0) {
          if (g->nodes[leaf].kind == EG_LEAF &&
              eg_sym_in(written_syms, written_count,
                        g->leaves[g->nodes[leaf].leaf])) {
            continue;
          }
          if (target->op != IR_OP_ASSIGN) {
            IROperand rep = eg_node_operand(g, &g->nodes[leaf]);
            ir_rewrite_to_assign_operand(target, &rep, &any_changed);
          }
          continue;
        }
        int cur_cost =
            target->op == IR_OP_BINARY &&
                    eg_op_char(target->text, &op2) == '*' && op2 == 0
                ? 4
                : 1;
        /* Does the current form read a value computed inside the region? A
         * temp always is; a symbol is when a binding shadows its leaf. The
         * extracted form reads only leaves and constants, so at equal op
         * cost it still wins: it cuts the dependency chain and lets the
         * feeding instruction go dead. */
        int cur_reads_computed = 0;
        const IROperand *cur_ops[2] = {&target->lhs, &target->rhs};
        for (int k = 0; k < 2; k++) {
          if (cur_ops[k]->kind == IR_OPERAND_TEMP) {
            cur_reads_computed = 1;
          } else if (cur_ops[k]->kind == IR_OPERAND_SYMBOL &&
                     cur_ops[k]->name) {
            for (int d2 = def_count - 1; d2 >= 0; d2--) {
              if (defs[d2].is_symbol &&
                  strcmp(defs[d2].name, cur_ops[k]->name) == 0) {
                cur_reads_computed = 1;
                break;
              }
            }
          }
        }
        int best = eg_class_best_shallow(g, cls);
        if (best < 0 || g->nodes[best].kind != EG_BIN) {
          continue;
        }
        int best_cost = (g->nodes[best].op == '*' && g->nodes[best].op2 == 0)
                            ? 4
                            : 1;
        if (best_cost > cur_cost ||
            (best_cost == cur_cost && !cur_reads_computed)) {
          continue;
        }
        int an = eg_class_leaf_node(g, g->nodes[best].a);
        int bn = eg_class_leaf_node(g, g->nodes[best].b);
        if (an < 0 || bn < 0) {
          continue;
        }
        if ((g->nodes[an].kind == EG_LEAF &&
             eg_sym_in(written_syms, written_count,
                       g->leaves[g->nodes[an].leaf])) ||
            (g->nodes[bn].kind == EG_LEAF &&
             eg_sym_in(written_syms, written_count,
                       g->leaves[g->nodes[bn].leaf]))) {
          continue;
        }
        char text[3] = {g->nodes[best].op, g->nodes[best].op2, 0};
        char *owned = mettle_strdup(text);
        if (!owned) {
          continue;
        }
        IROperand la = eg_node_operand(g, &g->nodes[an]);
        IROperand lb = eg_node_operand(g, &g->nodes[bn]);
        ir_operand_destroy(&target->lhs);
        ir_operand_destroy(&target->rhs);
        ir_instruction_clear_arguments(target);
        mettle_free_string(target->text);
        target->op = IR_OP_BINARY;
        target->text = owned;
        target->lhs = la;
        target->rhs = lb;
        target->is_float = 0;
        target->ast_ref = NULL;
        any_changed = 1;
      }
      def_count = 0;
      written_count = 0;
      memset(g, 0, sizeof(*g));
      continue;
    }

    /* Bind operands to classes (a copy reads its source twice, harmlessly). */
    int cls[2] = {-1, -1};
    const IROperand *ops[2] = {&ins->lhs,
                               is_copy ? &ins->lhs : &ins->rhs};
    for (int k = 0; k < 2; k++) {
      const IROperand *o = ops[k];
      if (o->kind == IR_OPERAND_INT) {
        cls[k] = eg_add_const(g, o->int_value);
      } else if ((o->kind == IR_OPERAND_SYMBOL ||
                  o->kind == IR_OPERAND_TEMP) &&
                 o->name) {
        /* Newest in-region binding shadows; scan backward. */
        for (int d = def_count - 1; d >= 0; d--) {
          if (defs[d].is_symbol == (o->kind == IR_OPERAND_SYMBOL ? 1 : 0) &&
              strcmp(defs[d].name, o->name) == 0) {
            cls[k] = defs[d].cls;
            break;
          }
        }
        if (cls[k] < 0 && o->kind == IR_OPERAND_SYMBOL) {
          int leaf = eg_leaf_id(g, o->name);
          if (leaf >= 0) {
            EgNode n;
            memset(&n, 0, sizeof(n));
            n.kind = EG_LEAF;
            n.leaf = leaf;
            cls[k] = eg_add(g, &n);
          }
        }
      }
    }
    if (cls[0] < 0 || cls[1] < 0 || def_count >= EG_MAX_DEFS ||
        written_count >= EG_MAX_DEFS) {
      /* Untracked operand (a load result, a call, an out-of-region temp):
       * end the region conservatively at this instruction. */
      def_count = 0;
      written_count = 0;
      memset(g, 0, sizeof(*g));
      continue;
    }

    int c;
    if (is_copy) {
      c = cls[0]; /* the copy's dest joins its source's class */
    } else {
      c = eg_add_bin(g, opch, op2, cls[0], cls[1]);
      if (c >= 0) {
        for (int r = 0; r < EG_SATURATE_ROUNDS; r++) {
          if (!eg_saturate_round(g)) {
            break;
          }
        }
      }
    }
    if (c < 0) {
      def_count = 0;
      written_count = 0;
      memset(g, 0, sizeof(*g));
      continue;
    }
    defs[def_count].name = ins->dest.name;
    defs[def_count].is_symbol = dest_sym;
    defs[def_count].cls = c;
    defs[def_count].at = i;
    /* Only a temp-dest BINARY is extraction material: a symbol def is an
     * observable store other code may read at this exact position. Its
     * BINDING still folds forward into everything downstream. */
    defs[def_count].rewritable = (!is_copy && dest_temp) ? 1 : 0;
    def_count++;
    if (dest_sym) {
      written_syms[written_count++] = ins->dest.name;
    }
  }

  free(g);
  *changed = any_changed;
  return 1;
}
