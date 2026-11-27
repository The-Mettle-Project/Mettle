#include "translate/ctr_emit.h"

#include <stdlib.h>
#include <string.h>

typedef enum { TERM_NONE, TERM_JUMP, TERM_BRANCH, TERM_SWITCH, TERM_RETURN, TERM_INDIRECT } TermKind;

typedef struct FlowCase {
  int64_t lo;
  int64_t hi;
  int target;
} FlowCase;

typedef struct FlowBlock {
  int id;
  CtrVec stmts;
  TermKind term;
  CtrNode *expr;
  CtrNode *ret;
  int targets[2];
  FlowCase *cases;
  int case_count;
  int case_cap;
  int default_target;
  const char *label_name;
  int reachable;
  int rpo;
  int idom;
  int forward_preds;
  int back_preds;
  int *succ;
  int succ_count;
} FlowBlock;

typedef struct FlowLoop {
  struct FlowLoop *next;
  CtrNode *node;
  int break_to;
  int continue_to;
} FlowLoop;

typedef struct FlowSwitch {
  struct FlowSwitch *next;
  CtrNode *node;
  int block;
  int exit;
  int has_default;
} FlowSwitch;

typedef struct {
  FlowBlock **blocks;
  int count;
  int cap;
  int cur;
  FlowLoop *loops;
  FlowSwitch *switches;
  CtrMap labels;
  CtrVec label_values;
  int *order;
  int order_count;
} Flow;

static Flow *F;

static int new_block(void) {
  if (F->count == F->cap) {
    F->cap = F->cap ? F->cap * 2 : 32;
    FlowBlock **b = ctr_alloc(sizeof(FlowBlock *) * (size_t)F->cap);
    if (F->blocks) {
      memcpy(b, F->blocks, sizeof(FlowBlock *) * (size_t)F->count);
    }
    F->blocks = b;
  }
  FlowBlock *b = ctr_alloc(sizeof(FlowBlock));
  b->id = F->count;
  b->default_target = -1;
  b->idom = -1;
  F->blocks[F->count] = b;
  return F->count++;
}

static FlowBlock *blk(int id) { return F->blocks[id]; }

static int current(void) {
  if (F->cur < 0) {
    F->cur = new_block();
  }
  return F->cur;
}

static void append(CtrNode *stmt) { ctr_vec_push(&blk(current())->stmts, stmt); }

static void jump_to(int target) {
  if (F->cur >= 0) {
    FlowBlock *b = blk(F->cur);
    b->term = TERM_JUMP;
    b->targets[0] = target;
  }
  F->cur = -1;
}

static void start(int block) {
  jump_to(block);
  F->cur = block;
}

static void branch(CtrNode *cond, int t, int f) {
  FlowBlock *b = blk(current());
  b->term = TERM_BRANCH;
  b->expr = cond;
  b->targets[0] = t;
  b->targets[1] = f;
  F->cur = -1;
}

static int label_block(const char *name) {
  void *found = ctr_map_get(&F->labels, name);
  if (found) {
    return (int)(intptr_t)found - 1;
  }
  int id = new_block();
  blk(id)->label_name = name;
  ctr_map_put(&F->labels, name, (void *)(intptr_t)(id + 1));
  return id;
}

static void lower(CtrNode *n);

static void lower_list(CtrNode *n) {
  for (; n; n = n->next) {
    lower(n);
  }
}

static void push_loop(CtrNode *node, int brk, int cont) {
  FlowLoop *l = ctr_alloc(sizeof(FlowLoop));
  l->node = node;
  l->break_to = brk;
  l->continue_to = cont;
  l->next = F->loops;
  F->loops = l;
}

static void lower_if(CtrNode *n) {
  int t = new_block(), e = new_block(), j = new_block();
  branch(n->cond, t, n->els ? e : j);
  F->cur = t;
  lower(n->then);
  jump_to(j);
  if (n->els) {
    F->cur = e;
    lower(n->els);
    jump_to(j);
  }
  F->cur = j;
}

static void lower_while(CtrNode *n) {
  int h = new_block(), b = new_block(), x = new_block();
  start(h);
  branch(n->cond, b, x);
  push_loop(n, x, h);
  F->cur = b;
  lower(n->then);
  jump_to(h);
  F->loops = F->loops->next;
  F->cur = x;
}

static void lower_do(CtrNode *n) {
  int b = new_block(), c = new_block(), x = new_block();
  start(b);
  push_loop(n, x, c);
  lower(n->then);
  F->loops = F->loops->next;
  start(c);
  branch(n->cond, b, x);
  F->cur = x;
}

static void lower_for(CtrNode *n) {
  if (n->init && n->init->kind == ND_EXPR_STMT) {
    append(n->init);
  } else if (n->init) {
    lower(n->init);
  }
  int h = new_block(), b = new_block(), i = new_block(), x = new_block();
  start(h);
  if (n->cond) {
    branch(n->cond, b, x);
  } else {
    jump_to(b);
  }
  push_loop(n, x, i);
  F->cur = b;
  lower(n->then);
  F->loops = F->loops->next;
  start(i);
  if (n->inc) {
    CtrNode *s = ctr_new_node(ND_EXPR_STMT, n->tok);
    s->lhs = n->inc;
    append(s);
  }
  jump_to(h);
  F->cur = x;
}

static void add_case(FlowBlock *sw, int64_t lo, int64_t hi, int target) {
  if (sw->case_count == sw->case_cap) {
    sw->case_cap = sw->case_cap ? sw->case_cap * 2 : 8;
    FlowCase *c = ctr_alloc(sizeof(FlowCase) * (size_t)sw->case_cap);
    if (sw->cases) {
      memcpy(c, sw->cases, sizeof(FlowCase) * (size_t)sw->case_count);
    }
    sw->cases = c;
  }
  FlowCase *c = &sw->cases[sw->case_count++];
  c->lo = lo;
  c->hi = hi;
  c->target = target;
}

static void lower_switch(CtrNode *n) {
  int s = current();
  FlowBlock *sb = blk(s);
  sb->term = TERM_SWITCH;
  sb->expr = n->cond;
  int x = new_block();
  FlowSwitch *fs = ctr_alloc(sizeof(FlowSwitch));
  fs->node = n;
  fs->block = s;
  fs->exit = x;
  fs->next = F->switches;
  F->switches = fs;
  push_loop(n, x, F->loops ? F->loops->continue_to : -1);
  F->loops->node = n;
  F->cur = -1;
  lower(n->then);
  jump_to(x);
  F->loops = F->loops->next;
  F->switches = fs->next;
  if (sb->default_target < 0) {
    sb->default_target = x;
  }
  F->cur = x;
}

static void lower_case(CtrNode *n) {
  int l = new_block();
  start(l);
  FlowSwitch *fs = F->switches;
  while (fs && fs->node != n->target) {
    fs = fs->next;
  }
  if (fs) {
    FlowBlock *sb = blk(fs->block);
    if (n->is_default) {
      sb->default_target = l;
    } else {
      add_case(sb, n->case_begin, n->case_end, l);
    }
  }
  lower(n->lhs);
}

static FlowLoop *loop_for(CtrNode *target) {
  for (FlowLoop *l = F->loops; l; l = l->next) {
    if (l->node == target) {
      return l;
    }
  }
  return NULL;
}

static void lower_jump(CtrNode *n) {
  FlowLoop *l = loop_for(n->target);
  if (!l) {
    ctr_error_tok(n->tok, "jump target not found");
    F->cur = -1;
    return;
  }
  if (n->kind == ND_BREAK) {
    jump_to(l->break_to);
    return;
  }
  for (FlowLoop *k = F->loops; k; k = k->next) {
    if (k->node == n->target && k->continue_to >= 0) {
      jump_to(k->continue_to);
      return;
    }
  }
  jump_to(l->continue_to);
}

static void lower_return(CtrNode *n) {
  FlowBlock *b = blk(current());
  b->term = TERM_RETURN;
  b->ret = n;
  F->cur = -1;
}

static void lower_indirect(CtrNode *n) {
  FlowBlock *b = blk(current());
  b->term = TERM_INDIRECT;
  b->expr = n->lhs;
  F->cur = -1;
}

static int holds_label(CtrNode *n) {
  if (!n) {
    return 0;
  }
  if (n->kind == ND_LABEL) {
    return 1;
  }
  if (holds_label(n->lhs) || holds_label(n->then) || holds_label(n->els)) {
    return 1;
  }
  for (CtrNode *b = n->body; b; b = b->next) {
    if (holds_label(b)) {
      return 1;
    }
  }
  return 0;
}

static CtrNode *through_casts(CtrNode *n) {
  while (n->kind == ND_CAST && n->implicit) {
    n = n->lhs;
  }
  return n;
}

static CtrNode *with_value(CtrNode *rhs, CtrNode *value) {
  if (rhs->kind != ND_CAST || !rhs->implicit) {
    return value;
  }
  CtrNode *copy = ctr_alloc(sizeof(CtrNode));
  *copy = *rhs;
  copy->lhs = with_value(rhs->lhs, value);
  return copy;
}

static int lower_statement_expression(CtrNode *n) {
  CtrNode *e = n->lhs;
  CtrNode *assign = e->kind == ND_ASSIGN ? e : NULL;
  CtrNode *se = through_casts(assign ? assign->rhs : e);
  if (se->kind != ND_STMT_EXPR || !holds_label(se)) {
    return 0;
  }
  CtrNode *last = se->body;
  while (last && last->next) {
    last = last->next;
  }
  for (CtrNode *s = se->body; s && s != last; s = s->next) {
    lower(s);
  }
  if (!last) {
    return 1;
  }
  if (!assign || last->kind != ND_EXPR_STMT) {
    lower(last);
    return 1;
  }
  CtrNode *copy = ctr_alloc(sizeof(CtrNode));
  *copy = *assign;
  copy->rhs = with_value(assign->rhs, last->lhs);
  CtrNode *stmt = ctr_new_node(ND_EXPR_STMT, n->tok);
  stmt->lhs = copy;
  append(stmt);
  return 1;
}

static void lower_more(CtrNode *n) {
  switch (n->kind) {
  case ND_LABEL:
    start(label_block(n->label));
    n->flow_index = F->cur;
    lower(n->lhs);
    return;
  case ND_CASE:
    lower_case(n);
    return;
  case ND_GOTO:
    jump_to(label_block(n->label));
    return;
  case ND_GOTO_EXPR:
    lower_indirect(n);
    return;
  case ND_BREAK:
  case ND_CONTINUE:
    lower_jump(n);
    return;
  case ND_RETURN:
    lower_return(n);
    return;
  case ND_EMPTY:
  case ND_ASM:
    return;
  case ND_EXPR_STMT:
    if (lower_statement_expression(n)) {
      return;
    }
    append(n);
    return;
  default:
    append(n);
    return;
  }
}

static void lower(CtrNode *n) {
  if (!n) {
    return;
  }
  switch (n->kind) {
  case ND_BLOCK:
    lower_list(n->body);
    return;
  case ND_IF:
    lower_if(n);
    return;
  case ND_WHILE:
    lower_while(n);
    return;
  case ND_DO:
    lower_do(n);
    return;
  case ND_FOR:
    lower_for(n);
    return;
  case ND_SWITCH:
    lower_switch(n);
    return;
  default:
    lower_more(n);
    return;
  }
}

static void collect_label_values(CtrNode *n);

static void label_values_list(CtrNode *n) {
  for (; n; n = n->next) {
    collect_label_values(n);
  }
}

static void collect_label_values(CtrNode *n) {
  if (!n) {
    return;
  }
  if (n->kind == ND_LABEL_VAL && n->label) {
    ctr_vec_push(&F->label_values, (void *)n->label);
  }
  collect_label_values(n->lhs);
  collect_label_values(n->rhs);
  collect_label_values(n->cond);
  collect_label_values(n->then);
  collect_label_values(n->els);
  collect_label_values(n->init);
  collect_label_values(n->inc);
  label_values_list(n->body);
  label_values_list(n->args);
}

static void add_succ(FlowBlock *b, int s) {
  for (int i = 0; i < b->succ_count; i++) {
    if (b->succ[i] == s) {
      return;
    }
  }
  b->succ[b->succ_count++] = s;
}

static void compute_succ(FlowBlock *b) {
  int max = 2 + b->case_count + 1 + F->label_values.len;
  b->succ = ctr_alloc(sizeof(int) * (size_t)max);
  b->succ_count = 0;
  switch (b->term) {
  case TERM_JUMP:
    add_succ(b, b->targets[0]);
    return;
  case TERM_BRANCH:
    add_succ(b, b->targets[0]);
    add_succ(b, b->targets[1]);
    return;
  case TERM_SWITCH:
    for (int i = 0; i < b->case_count; i++) {
      add_succ(b, b->cases[i].target);
    }
    add_succ(b, b->default_target);
    return;
  case TERM_INDIRECT:
    for (int i = 0; i < F->label_values.len; i++) {
      add_succ(b, label_block(F->label_values.items[i]));
    }
    return;
  default:
    return;
  }
}

static void dfs(int id, int *visited, int *post, int *n) {
  visited[id] = 1;
  FlowBlock *b = blk(id);
  b->reachable = 1;
  for (int i = 0; i < b->succ_count; i++) {
    if (!visited[b->succ[i]]) {
      dfs(b->succ[i], visited, post, n);
    }
  }
  post[(*n)++] = id;
}

static void number_blocks(void) {
  int *visited = ctr_alloc(sizeof(int) * (size_t)F->count);
  int *post = ctr_alloc(sizeof(int) * (size_t)F->count);
  int n = 0;
  dfs(0, visited, post, &n);
  F->order = ctr_alloc(sizeof(int) * (size_t)n);
  F->order_count = n;
  for (int i = 0; i < n; i++) {
    F->order[i] = post[n - 1 - i];
    blk(F->order[i])->rpo = i;
  }
}

static int intersect_dom(int a, int b) {
  while (a != b) {
    while (blk(a)->rpo > blk(b)->rpo) {
      a = blk(a)->idom;
    }
    while (blk(b)->rpo > blk(a)->rpo) {
      b = blk(b)->idom;
    }
  }
  return a;
}

static int new_idom_for(int id) {
  int nid = -1;
  for (int k = 0; k < F->order_count; k++) {
    FlowBlock *p = blk(F->order[k]);
    for (int s = 0; s < p->succ_count; s++) {
      if (p->succ[s] != id || p->idom < 0) {
        continue;
      }
      nid = nid < 0 ? p->id : intersect_dom(p->id, nid);
    }
  }
  return nid;
}

static void dominators(void) {
  blk(0)->idom = 0;
  int changed = 1;
  while (changed) {
    changed = 0;
    for (int i = 1; i < F->order_count; i++) {
      int id = F->order[i];
      int nid = new_idom_for(id);
      if (nid >= 0 && blk(id)->idom != nid) {
        blk(id)->idom = nid;
        changed = 1;
      }
    }
  }
}

static int dominates(int a, int b) {
  while (b != a) {
    if (b == 0) {
      return a == 0;
    }
    b = blk(b)->idom;
  }
  return 1;
}

static int classify_edges(void) {
  int reducible = 1;
  for (int k = 0; k < F->order_count; k++) {
    FlowBlock *b = blk(F->order[k]);
    for (int s = 0; s < b->succ_count; s++) {
      FlowBlock *t = blk(b->succ[s]);
      if (t->rpo <= b->rpo) {
        t->back_preds++;
        if (!dominates(t->id, b->id)) {
          reducible = 0;
        }
      } else {
        t->forward_preds++;
      }
    }
  }
  return reducible;
}

typedef enum { CTX_IF, CTX_LOOP, CTX_BLOCK, CTX_SWITCH } CtxKind;

typedef struct Ctx {
  struct Ctx *next;
  CtxKind kind;
  int block;
  const char *label;
  int label_used;
} Ctx;

static int is_merge(int id) { return blk(id)->forward_preds >= 2; }

static int is_loop_header(int id) { return blk(id)->back_preds > 0; }

static const char *block_label(int id, const char *prefix) {
  FlowBlock *b = blk(id);
  if (b->label_name) {
    return ctr_safe_ident(b->label_name);
  }
  return ctr_format("%s%d", prefix, id);
}

static void do_tree(int x, Ctx *ctx);

static Ctx *push_ctx(Ctx *ctx, CtxKind kind, int block, const char *label) {
  Ctx *c = ctr_alloc(sizeof(Ctx));
  c->next = ctx;
  c->kind = kind;
  c->block = block;
  c->label = label;
  return c;
}

static const char *jump_text(Ctx *ctx, CtxKind want, int target) {
  int intervening_break = 0;
  int intervening_loop = 0;
  for (Ctx *c = ctx; c; c = c->next) {
    if (c->kind == want && c->block == target) {
      int bare = want == CTX_LOOP ? !intervening_loop : !intervening_break;
      c->label_used |= !bare;
      const char *verb = want == CTX_LOOP ? "continue" : "break";
      return bare ? ctr_format("%s;", verb) : ctr_format("%s %s;", verb, c->label);
    }
    if (c->kind == CTX_LOOP || c->kind == CTX_BLOCK) {
      intervening_break = 1;
      intervening_loop = 1;
    } else if (c->kind == CTX_SWITCH) {
      intervening_break = 1;
    }
  }
  ctr_error_tok(NULL, "structuring lost a jump target");
  return "break;";
}

static void do_branch(int from, int to, Ctx *ctx) {
  if (blk(to)->rpo <= blk(from)->rpo) {
    ctr_line("%s", jump_text(ctx, CTX_LOOP, to));
    return;
  }
  if (is_merge(to)) {
    ctr_line("%s", jump_text(ctx, CTX_BLOCK, to));
    return;
  }
  do_tree(to, ctx);
}

static void emit_block_stmts(FlowBlock *b) {
  for (int i = 0; i < b->stmts.len; i++) {
    CtrNode *s = b->stmts.items[i];
    if (s->kind == ND_DECL) {
      ctr_emit_local_decl(s, 1);
    } else if (s->kind == ND_BLOCK && s->op == 1) {
      for (CtrNode *d = s->body; d; d = d->next) {
        if (d->kind == ND_DECL) {
          ctr_emit_local_decl(d, 1);
        } else {
          ctr_emit_stmt(d);
        }
      }
    } else {
      ctr_emit_stmt(s);
    }
  }
}

static void emit_return(FlowBlock *b) { ctr_emit_stmt(b->ret); }

static void emit_switch_term(FlowBlock *b, Ctx *ctx) {
  ctr_open("switch (%s) {", ctr_expr_text(b->expr, PREC_LOWEST));
  Ctx *sw = push_ctx(ctx, CTX_SWITCH, -1, NULL);
  for (int i = 0; i < b->case_count; i++) {
    FlowCase *c = &b->cases[i];
    if (c->lo == c->hi) {
      ctr_open("case %lld: {", (long long)c->lo);
    } else {
      ctr_open("case %lld..%lld: {", (long long)c->lo, (long long)c->hi);
    }
    do_branch(b->id, c->target, sw);
    ctr_close();
  }
  ctr_open("default: {");
  do_branch(b->id, b->default_target, sw);
  ctr_close();
  ctr_close();
}

static void emit_indirect_term(FlowBlock *b, Ctx *ctx) {
  ctr_open("switch ((int64)%s) {", ctr_expr_text(b->expr, PREC_UNARY));
  Ctx *sw = push_ctx(ctx, CTX_SWITCH, -1, NULL);
  for (int i = 0; i < b->succ_count; i++) {
    ctr_open("case %d: {", b->succ[i]);
    do_branch(b->id, b->succ[i], sw);
    ctr_close();
  }
  ctr_open("default: {");
  ctr_em->mod->needs_c = 1;
  ctr_line("libc.trap();");
  ctr_close();
  ctr_close();
}

static void emit_terminator(FlowBlock *b, Ctx *ctx) {
  switch (b->term) {
  case TERM_JUMP:
    do_branch(b->id, b->targets[0], ctx);
    return;
  case TERM_BRANCH: {
    Ctx *c = push_ctx(ctx, CTX_IF, -1, NULL);
    ctr_open("if (%s) {", ctr_cond_text(b->expr));
    do_branch(b->id, b->targets[0], c);
    ctr_close_with("} else {");
    ctr_em->indent++;
    do_branch(b->id, b->targets[1], c);
    ctr_close();
    return;
  }
  case TERM_SWITCH:
    emit_switch_term(b, ctx);
    return;
  case TERM_INDIRECT:
    emit_indirect_term(b, ctx);
    return;
  case TERM_RETURN:
    emit_return(b);
    return;
  default:
    if (ctr_em->fn && ctr_em->fn->ty->return_ty->kind == TY_VOID) {
      ctr_line("return;");
    } else if (ctr_em->fn && strcmp(ctr_em->fn->name, "main") == 0) {
      ctr_line("return 0;");
    } else if (ctr_em->fn && ctr_is_scalar(ctr_em->fn->ty->return_ty)) {
      ctr_line("return %s;", ctr_is_flonum(ctr_em->fn->ty->return_ty) ? "0.0" : "0");
    } else {
      ctr_line("var undefined_result: %s;", ctr_type_text(ctr_em->fn->ty->return_ty));
      ctr_line("return undefined_result;");
    }
    return;
  }
}

typedef struct {
  int x;
  int *ys;
  int n;
  Ctx *ctx;
} WithinArgs;

static char *capture_text(void (*fn)(void *), void *arg, int indent) {
  CtrBuf *saved = ctr_em->out;
  int saved_indent = ctr_em->indent;
  CtrBuf buf = {0};
  ctr_em->out = &buf;
  ctr_em->indent = indent;
  fn(arg);
  ctr_em->out = saved;
  ctr_em->indent = saved_indent;
  return ctr_buf_take(&buf);
}

static int ends_in_jump(const char *text) {
  size_t n = strlen(text);
  while (n > 0 && (text[n - 1] == '\n' || text[n - 1] == ' ')) {
    n--;
  }
  const char *line = text + n;
  while (line > text && line[-1] != '\n') {
    line--;
  }
  while (*line == ' ') {
    line++;
  }
  return strncmp(line, "return", 6) == 0 || strncmp(line, "break", 5) == 0 ||
         strncmp(line, "continue", 8) == 0 || strncmp(line, "libc.trap", 9) == 0;
}

static void node_within(int x, int *ys, int n, Ctx *ctx);

static void within_body(void *arg) {
  WithinArgs *a = arg;
  node_within(a->x, a->ys, a->n, a->ctx);
}

static void node_within(int x, int *ys, int n, Ctx *ctx) {
  if (n == 0) {
    FlowBlock *b = blk(x);
    emit_block_stmts(b);
    emit_terminator(b, ctx);
    return;
  }
  int y = ys[0];
  Ctx *c = push_ctx(ctx, CTX_BLOCK, y, block_label(y, "b"));
  WithinArgs args = {x, ys + 1, n - 1, c};
  char *body = capture_text(within_body, &args, ctr_em->indent + 1);
  if (c->label_used) {
    ctr_line("%s: while (true) {", c->label);
  } else {
    ctr_line("while (true) {");
  }
  ctr_buf_put(ctr_em->out, body);
  if (!ends_in_jump(body)) {
    ctr_em->indent++;
    ctr_line("break;");
    ctr_em->indent--;
  }
  ctr_line("}");
  do_tree(y, ctx);
}

static int cmp_rpo_desc(const void *a, const void *b) {
  int x = *(const int *)a, y = *(const int *)b;
  return blk(y)->rpo - blk(x)->rpo;
}

static int merge_children(int x, int *out) {
  int n = 0;
  for (int k = 0; k < F->order_count; k++) {
    int id = F->order[k];
    if (id != x && blk(id)->idom == x && is_merge(id)) {
      out[n++] = id;
    }
  }
  qsort(out, (size_t)n, sizeof(int), cmp_rpo_desc);
  return n;
}

static void do_tree(int x, Ctx *ctx) {
  int *ys = ctr_alloc(sizeof(int) * (size_t)(F->order_count + 1));
  int n = merge_children(x, ys);
  if (!is_loop_header(x)) {
    node_within(x, ys, n, ctx);
    return;
  }
  const char *label = block_label(x, "loop");
  if (is_merge(x) && blk(x)->label_name) {
    label = ctr_format("%s_again", label);
  }
  Ctx *c = push_ctx(ctx, CTX_LOOP, x, label);
  WithinArgs args = {x, ys, n, c};
  char *body = capture_text(within_body, &args, ctr_em->indent + 1);
  if (c->label_used) {
    ctr_line("%s: while (true) {", c->label);
  } else {
    ctr_line("while (true) {");
  }
  ctr_buf_put(ctr_em->out, body);
  ctr_line("}");
}

static void emit_dispatch_case(FlowBlock *b, const char *state, const char *loop) {
  ctr_open("case %d: {", b->id);
  emit_block_stmts(b);
  switch (b->term) {
  case TERM_JUMP:
    ctr_line("%s = %d;", state, b->targets[0]);
    break;
  case TERM_BRANCH:
    ctr_line("%s = if (%s) %d else %d;", state, ctr_cond_text(b->expr), b->targets[0], b->targets[1]);
    break;
  case TERM_SWITCH:
    ctr_open("%s = switch (%s) {", state, ctr_expr_text(b->expr, PREC_LOWEST));
    for (int i = 0; i < b->case_count; i++) {
      FlowCase *c = &b->cases[i];
      if (c->lo == c->hi) {
        ctr_line("case %lld: %d,", (long long)c->lo, c->target);
      } else {
        ctr_line("case %lld..%lld: %d,", (long long)c->lo, (long long)c->hi, c->target);
      }
    }
    ctr_line("default: %d,", b->default_target);
    ctr_close_with("};");
    break;
  case TERM_INDIRECT:
    ctr_line("%s = (int32)(int64)%s;", state, ctr_expr_text(b->expr, PREC_UNARY));
    break;
  default:
    emit_terminator(b, NULL);
    ctr_close();
    return;
  }
  ctr_line("continue %s;", loop);
  ctr_close();
}

static void emit_dispatch(void) {
  const char *state = ctr_new_temp("state");
  const char *loop = "flow";
  ctr_line("var %s: int32 = %d;", state, F->order[0]);
  ctr_open("%s: while (true) {", loop);
  ctr_open("switch (%s) {", state);
  for (int k = 0; k < F->order_count; k++) {
    emit_dispatch_case(blk(F->order[k]), state, loop);
  }
  ctr_open("default: {");
  ctr_line("libc.trap();");
  ctr_em->mod->needs_c = 1;
  ctr_close();
  ctr_close();
  ctr_close();
}

static void declare_locals(CtrObj *fn) {
  for (CtrObj *v = fn->locals; v; v = v->next) {
    if (v->ty->kind == TY_VLA) {
      ctr_em->mod->needs_c = 1;
      ctr_line("var %s: %s = 0;", ctr_mettle_name_of(v), ctr_type_text(v->ty));
      continue;
    }
    ctr_line("var %s: %s%s;", ctr_mettle_name_of(v), ctr_type_text(v->ty), ctr_zero_suffix(v));
  }
}

void ctr_emit_cfg_body(CtrObj *fn) {
  Flow flow = {0};
  F = &flow;
  ctr_em->cfg_mode = 1;
  new_block();
  F->cur = 0;
  collect_label_values(fn->body);
  lower(fn->body);
  if (F->cur >= 0) {
    blk(F->cur)->term = TERM_NONE;
  }
  for (int i = 0; i < F->count; i++) {
    compute_succ(blk(i));
  }
  number_blocks();
  dominators();
  int reducible = classify_edges();
  declare_locals(fn);
  if (!reducible) {
    emit_dispatch();
  } else {
    do_tree(0, NULL);
  }
  F = NULL;
  ctr_em->cfg_mode = 0;
}
