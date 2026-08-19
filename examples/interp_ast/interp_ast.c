/*
 * C AST interpreter benchmark - counterpart to interp_ast.mettle
 *
 * Generates an ~160 KB program text, lexes it into a token array, parses the
 * tokens into an AST arena with precedence climbing, then walks the tree to
 * run it. Every pass redoes all three phases.
 *
 * Build: build.bat (or: gcc -O3 -o interp_ast_c.exe interp_ast.c -lkernel32)
 * Run: interp_ast_c.exe
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <inttypes.h>

#include "../bench_time.h"

#define BLOCKS 800
#define SRC_CAP 262144
#define TOKEN_CAP 131072
#define NODE_CAP 131072
#define NAME_CAP 64
#define PASSES 20
#define MODULUS 1000003

#define T_EOF 0
#define T_NUM 1
#define T_IDENT 2
#define T_OP 3
#define T_PUNCT 4
#define T_IF 5
#define T_ELSE 6
#define T_WHILE 7

#define OP_ADD 1
#define OP_SUB 2
#define OP_MUL 3
#define OP_DIV 4
#define OP_MOD 5
#define OP_LT 6
#define OP_LE 7
#define OP_GT 8
#define OP_GE 9
#define OP_EQ 10
#define OP_NE 11
#define OP_AND 12
#define OP_OR 13
#define OP_XOR 14
#define OP_SHL 15
#define OP_SHR 16
#define OP_LAND 17
#define OP_LOR 18
#define OP_ASSIGN 19
#define OP_NOT 20
#define OP_NEG 21

#define N_NUM 0
#define N_VAR 1
#define N_ASSIGN 2
#define N_BIN 3
#define N_UN 4
#define N_IF 5
#define N_WHILE 6
#define N_BLOCK 7

typedef struct {
    int64_t ival;
    int32_t kind;
    int32_t op;
} Token;

typedef struct {
    int64_t ival;
    int32_t kind;
    int32_t op;
    int32_t a;
    int32_t b;
    int32_t c;
    int32_t next;
} AstNode;

typedef struct {
    uint8_t *src;
    int32_t src_len;
    Token *toks;
    int32_t tok_count;
    AstNode *nodes;
    int32_t node_count;
    int32_t *name_off;
    int32_t *name_len;
    int32_t name_count;
    int32_t pos;
    int32_t failed;
} Ctx;

static int32_t parse_expr(Ctx *ctx, int32_t min_prec);
static int32_t parse_stmt(Ctx *ctx);
static int32_t parse_block(Ctx *ctx);

static int32_t emit_cstr(uint8_t *dst, int32_t pos, const char *s) {
    int32_t i = 0;
    while (s[i] != 0) {
        dst[pos + i] = (uint8_t)s[i];
        i += 1;
    }
    return pos + i;
}

static int32_t emit_int(uint8_t *dst, int32_t pos, int32_t v) {
    uint8_t tmp[24];
    if (v < 0) {
        dst[pos] = 45;
        pos += 1;
        v = -v;
    }
    if (v == 0) {
        dst[pos] = 48;
        return pos + 1;
    }
    int32_t n = 0;
    while (v > 0) {
        tmp[n] = (uint8_t)(48 + v % 10);
        v = v / 10;
        n += 1;
    }
    while (n > 0) {
        n -= 1;
        dst[pos] = tmp[n];
        pos += 1;
    }
    return pos;
}

static int32_t emit_var(uint8_t *dst, int32_t pos, int32_t which) {
    dst[pos] = 118;
    dst[pos + 1] = (uint8_t)(48 + which % 7);
    return pos + 2;
}

static int32_t gen_source(uint8_t *dst) {
    int32_t pos = 0;
    int32_t v = 0;
    while (v < 7) {
        pos = emit_var(dst, pos, v);
        pos = emit_cstr(dst, pos, " = ");
        pos = emit_int(dst, pos, v + 1);
        pos = emit_cstr(dst, pos, ";\n");
        v += 1;
    }
    pos = emit_cstr(dst, pos, "k = 0;\n");

    int32_t b = 0;
    while (b < BLOCKS) {
        int32_t a_var = b % 7;
        int32_t b_var = (b * 3 + 1) % 7;
        int32_t c_var = (b * 5 + 2) % 7;
        int32_t k1 = 1 + b % 9;
        int32_t k2 = 2 + b % 5;
        int32_t k3 = 3 + b % 11;
        int32_t k4 = 3 + b % 5;
        int32_t k5 = 7 + b % 13;

        pos = emit_var(dst, pos, c_var);
        pos = emit_cstr(dst, pos, " = (");
        pos = emit_var(dst, pos, a_var);
        pos = emit_cstr(dst, pos, " + ");
        pos = emit_var(dst, pos, b_var);
        pos = emit_cstr(dst, pos, " * ");
        pos = emit_int(dst, pos, k1);
        pos = emit_cstr(dst, pos, ") % ");
        pos = emit_int(dst, pos, (int32_t)MODULUS);
        pos = emit_cstr(dst, pos, ";\n");

        pos = emit_cstr(dst, pos, "if (");
        pos = emit_var(dst, pos, a_var);
        pos = emit_cstr(dst, pos, " % ");
        pos = emit_int(dst, pos, k2);
        pos = emit_cstr(dst, pos, " == 0) {\n  ");
        pos = emit_var(dst, pos, b_var);
        pos = emit_cstr(dst, pos, " = (");
        pos = emit_var(dst, pos, b_var);
        pos = emit_cstr(dst, pos, " + ");
        pos = emit_var(dst, pos, c_var);
        pos = emit_cstr(dst, pos, ") % ");
        pos = emit_int(dst, pos, (int32_t)MODULUS);
        pos = emit_cstr(dst, pos, ";\n} else {\n  ");
        pos = emit_var(dst, pos, b_var);
        pos = emit_cstr(dst, pos, " = (");
        pos = emit_var(dst, pos, b_var);
        pos = emit_cstr(dst, pos, " + ");
        pos = emit_int(dst, pos, (int32_t)MODULUS);
        pos = emit_cstr(dst, pos, " - ");
        pos = emit_var(dst, pos, c_var);
        pos = emit_cstr(dst, pos, " % ");
        pos = emit_int(dst, pos, k3);
        pos = emit_cstr(dst, pos, ") % ");
        pos = emit_int(dst, pos, (int32_t)MODULUS);
        pos = emit_cstr(dst, pos, ";\n}\n");

        pos = emit_cstr(dst, pos, "k = 0;\nwhile (k < ");
        pos = emit_int(dst, pos, k4);
        pos = emit_cstr(dst, pos, ") {\n  ");
        pos = emit_var(dst, pos, a_var);
        pos = emit_cstr(dst, pos, " = (");
        pos = emit_var(dst, pos, a_var);
        pos = emit_cstr(dst, pos, " + ((");
        pos = emit_var(dst, pos, c_var);
        pos = emit_cstr(dst, pos, " ^ k) % ");
        pos = emit_int(dst, pos, k5);
        pos = emit_cstr(dst, pos, ")) % ");
        pos = emit_int(dst, pos, (int32_t)MODULUS);
        pos = emit_cstr(dst, pos, ";\n  k = k + 1;\n}\n");
        b += 1;
    }

    dst[pos] = 0;
    return pos;
}

static int32_t intern(Ctx *ctx, int32_t off, int32_t len) {
    int32_t i = 0;
    while (i < ctx->name_count) {
        if (ctx->name_len[i] == len) {
            int32_t k = 0;
            int32_t same = 1;
            while (k < len) {
                if (ctx->src[ctx->name_off[i] + k] != ctx->src[off + k]) {
                    same = 0;
                    break;
                }
                k += 1;
            }
            if (same != 0) {
                return i;
            }
        }
        i += 1;
    }
    if (ctx->name_count >= NAME_CAP) {
        ctx->failed = 1;
        return 0;
    }
    int32_t idx = ctx->name_count;
    ctx->name_off[idx] = off;
    ctx->name_len[idx] = len;
    ctx->name_count += 1;
    return idx;
}

static int32_t span_equals(Ctx *ctx, int32_t off, int32_t len, const char *word) {
    int32_t i = 0;
    while (i < len) {
        if (word[i] == 0 || (int32_t)ctx->src[off + i] != (int32_t)word[i]) {
            return 0;
        }
        i += 1;
    }
    if (word[len] != 0) {
        return 0;
    }
    return 1;
}

static void push_token(Ctx *ctx, int32_t kind, int32_t op, int64_t ival) {
    if (ctx->tok_count >= TOKEN_CAP) {
        ctx->failed = 1;
        return;
    }
    ctx->toks[ctx->tok_count].kind = kind;
    ctx->toks[ctx->tok_count].op = op;
    ctx->toks[ctx->tok_count].ival = ival;
    ctx->tok_count += 1;
}

static void lex(Ctx *ctx) {
    int32_t i = 0;
    ctx->tok_count = 0;
    ctx->name_count = 0;
    while (i < ctx->src_len) {
        int32_t c = (int32_t)ctx->src[i];
        if (c == 32 || c == 9 || c == 10 || c == 13) {
            i += 1;
            continue;
        }
        if (c >= 48 && c <= 57) {
            int64_t value = 0;
            while (i < ctx->src_len) {
                int32_t d = (int32_t)ctx->src[i];
                if (d < 48 || d > 57) {
                    break;
                }
                value = value * 10 + (int64_t)(d - 48);
                i += 1;
            }
            push_token(ctx, T_NUM, 0, value);
            continue;
        }
        if ((c >= 97 && c <= 122) || (c >= 65 && c <= 90) || c == 95) {
            int32_t start = i;
            while (i < ctx->src_len) {
                int32_t d = (int32_t)ctx->src[i];
                if ((d >= 97 && d <= 122) || (d >= 65 && d <= 90) || (d >= 48 && d <= 57) || d == 95) {
                    i += 1;
                } else {
                    break;
                }
            }
            int32_t len = i - start;
            if (span_equals(ctx, start, len, "if") != 0) {
                push_token(ctx, T_IF, 0, 0);
            } else if (span_equals(ctx, start, len, "else") != 0) {
                push_token(ctx, T_ELSE, 0, 0);
            } else if (span_equals(ctx, start, len, "while") != 0) {
                push_token(ctx, T_WHILE, 0, 0);
            } else {
                push_token(ctx, T_IDENT, intern(ctx, start, len), 0);
            }
            continue;
        }
        int32_t next = 0;
        if (i + 1 < ctx->src_len) {
            next = (int32_t)ctx->src[i + 1];
        }
        if (c == 61 && next == 61) {
            push_token(ctx, T_OP, OP_EQ, 0);
            i += 2;
            continue;
        }
        if (c == 33 && next == 61) {
            push_token(ctx, T_OP, OP_NE, 0);
            i += 2;
            continue;
        }
        if (c == 60 && next == 61) {
            push_token(ctx, T_OP, OP_LE, 0);
            i += 2;
            continue;
        }
        if (c == 62 && next == 61) {
            push_token(ctx, T_OP, OP_GE, 0);
            i += 2;
            continue;
        }
        if (c == 60 && next == 60) {
            push_token(ctx, T_OP, OP_SHL, 0);
            i += 2;
            continue;
        }
        if (c == 62 && next == 62) {
            push_token(ctx, T_OP, OP_SHR, 0);
            i += 2;
            continue;
        }
        if (c == 38 && next == 38) {
            push_token(ctx, T_OP, OP_LAND, 0);
            i += 2;
            continue;
        }
        if (c == 124 && next == 124) {
            push_token(ctx, T_OP, OP_LOR, 0);
            i += 2;
            continue;
        }
        i += 1;
        switch (c) {
            case 43: push_token(ctx, T_OP, OP_ADD, 0); continue;
            case 45: push_token(ctx, T_OP, OP_SUB, 0); continue;
            case 42: push_token(ctx, T_OP, OP_MUL, 0); continue;
            case 47: push_token(ctx, T_OP, OP_DIV, 0); continue;
            case 37: push_token(ctx, T_OP, OP_MOD, 0); continue;
            case 60: push_token(ctx, T_OP, OP_LT, 0); continue;
            case 62: push_token(ctx, T_OP, OP_GT, 0); continue;
            case 38: push_token(ctx, T_OP, OP_AND, 0); continue;
            case 124: push_token(ctx, T_OP, OP_OR, 0); continue;
            case 94: push_token(ctx, T_OP, OP_XOR, 0); continue;
            case 33: push_token(ctx, T_OP, OP_NOT, 0); continue;
            case 61: push_token(ctx, T_OP, OP_ASSIGN, 0); continue;
            default: push_token(ctx, T_PUNCT, c, 0); continue;
        }
    }
    push_token(ctx, T_EOF, 0, 0);
}

static int32_t precedence(int32_t op) {
    switch (op) {
        case 18: return 1;
        case 17: return 2;
        case 13: return 3;
        case 14: return 4;
        case 12: return 5;
        case 10: return 6;
        case 11: return 6;
        case 6: return 7;
        case 7: return 7;
        case 8: return 7;
        case 9: return 7;
        case 15: return 8;
        case 16: return 8;
        case 1: return 9;
        case 2: return 9;
        case 3: return 10;
        case 4: return 10;
        case 5: return 10;
        default: return 0;
    }
}

static int32_t new_node(Ctx *ctx, int32_t kind) {
    if (ctx->node_count >= NODE_CAP) {
        ctx->failed = 1;
        return 0;
    }
    int32_t idx = ctx->node_count;
    ctx->node_count += 1;
    ctx->nodes[idx].ival = 0;
    ctx->nodes[idx].kind = kind;
    ctx->nodes[idx].op = 0;
    ctx->nodes[idx].a = -1;
    ctx->nodes[idx].b = -1;
    ctx->nodes[idx].c = -1;
    ctx->nodes[idx].next = -1;
    return idx;
}

static int32_t peek_kind(Ctx *ctx) {
    return ctx->toks[ctx->pos].kind;
}

static int32_t peek_op(Ctx *ctx) {
    return ctx->toks[ctx->pos].op;
}

static void expect_punct(Ctx *ctx, int32_t ch) {
    if (ctx->toks[ctx->pos].kind != T_PUNCT || ctx->toks[ctx->pos].op != ch) {
        ctx->failed = 1;
        return;
    }
    ctx->pos += 1;
}

static int32_t parse_primary(Ctx *ctx) {
    int32_t kind = peek_kind(ctx);
    if (kind == T_NUM) {
        int32_t idx = new_node(ctx, N_NUM);
        ctx->nodes[idx].ival = ctx->toks[ctx->pos].ival;
        ctx->pos += 1;
        return idx;
    }
    if (kind == T_IDENT) {
        int32_t idx = new_node(ctx, N_VAR);
        ctx->nodes[idx].op = ctx->toks[ctx->pos].op;
        ctx->pos += 1;
        return idx;
    }
    if (kind == T_PUNCT && peek_op(ctx) == 40) {
        ctx->pos += 1;
        int32_t inner = parse_expr(ctx, 1);
        expect_punct(ctx, 41);
        return inner;
    }
    if (kind == T_OP && (peek_op(ctx) == OP_SUB || peek_op(ctx) == OP_NOT)) {
        int32_t unop = OP_NEG;
        if (peek_op(ctx) == OP_NOT) {
            unop = OP_NOT;
        }
        ctx->pos += 1;
        int32_t idx = new_node(ctx, N_UN);
        ctx->nodes[idx].op = unop;
        ctx->nodes[idx].a = parse_primary(ctx);
        return idx;
    }
    ctx->failed = 1;
    ctx->pos += 1;
    return new_node(ctx, N_NUM);
}

static int32_t parse_expr(Ctx *ctx, int32_t min_prec) {
    int32_t left = parse_primary(ctx);
    for (;;) {
        if (peek_kind(ctx) != T_OP) {
            return left;
        }
        int32_t op = peek_op(ctx);
        int32_t prec = precedence(op);
        if (prec < min_prec || prec == 0) {
            return left;
        }
        ctx->pos += 1;
        int32_t right = parse_expr(ctx, prec + 1);
        int32_t idx = new_node(ctx, N_BIN);
        ctx->nodes[idx].op = op;
        ctx->nodes[idx].a = left;
        ctx->nodes[idx].b = right;
        left = idx;
    }
}

static int32_t parse_block(Ctx *ctx) {
    expect_punct(ctx, 123);
    int32_t head = -1;
    int32_t tail = -1;
    while (peek_kind(ctx) != T_EOF && ctx->failed == 0) {
        if (peek_kind(ctx) == T_PUNCT && peek_op(ctx) == 125) {
            ctx->pos += 1;
            break;
        }
        int32_t stmt = parse_stmt(ctx);
        if (head < 0) {
            head = stmt;
        } else {
            ctx->nodes[tail].next = stmt;
        }
        tail = stmt;
    }
    int32_t idx = new_node(ctx, N_BLOCK);
    ctx->nodes[idx].a = head;
    return idx;
}

static int32_t parse_stmt(Ctx *ctx) {
    int32_t kind = peek_kind(ctx);
    if (kind == T_IF) {
        ctx->pos += 1;
        expect_punct(ctx, 40);
        int32_t cond = parse_expr(ctx, 1);
        expect_punct(ctx, 41);
        int32_t then_block = parse_block(ctx);
        int32_t else_block = -1;
        if (peek_kind(ctx) == T_ELSE) {
            ctx->pos += 1;
            else_block = parse_block(ctx);
        }
        int32_t idx = new_node(ctx, N_IF);
        ctx->nodes[idx].a = cond;
        ctx->nodes[idx].b = then_block;
        ctx->nodes[idx].c = else_block;
        return idx;
    }
    if (kind == T_WHILE) {
        ctx->pos += 1;
        expect_punct(ctx, 40);
        int32_t cond = parse_expr(ctx, 1);
        expect_punct(ctx, 41);
        int32_t body = parse_block(ctx);
        int32_t idx = new_node(ctx, N_WHILE);
        ctx->nodes[idx].a = cond;
        ctx->nodes[idx].b = body;
        return idx;
    }
    if (kind == T_PUNCT && peek_op(ctx) == 123) {
        return parse_block(ctx);
    }
    if (kind != T_IDENT) {
        ctx->failed = 1;
        ctx->pos += 1;
        return new_node(ctx, N_BLOCK);
    }
    int32_t slot = ctx->toks[ctx->pos].op;
    ctx->pos += 1;
    if (peek_kind(ctx) != T_OP || peek_op(ctx) != OP_ASSIGN) {
        ctx->failed = 1;
        return new_node(ctx, N_BLOCK);
    }
    ctx->pos += 1;
    int32_t value = parse_expr(ctx, 1);
    expect_punct(ctx, 59);
    int32_t idx = new_node(ctx, N_ASSIGN);
    ctx->nodes[idx].op = slot;
    ctx->nodes[idx].a = value;
    return idx;
}

static int32_t parse_program(Ctx *ctx) {
    ctx->pos = 0;
    ctx->node_count = 0;
    int32_t head = -1;
    int32_t tail = -1;
    while (peek_kind(ctx) != T_EOF && ctx->failed == 0) {
        int32_t stmt = parse_stmt(ctx);
        if (head < 0) {
            head = stmt;
        } else {
            ctx->nodes[tail].next = stmt;
        }
        tail = stmt;
    }
    int32_t idx = new_node(ctx, N_BLOCK);
    ctx->nodes[idx].a = head;
    return idx;
}

static int64_t eval(Ctx *ctx, int32_t idx, int64_t *env) {
    int32_t kind = ctx->nodes[idx].kind;
    if (kind == N_NUM) {
        return ctx->nodes[idx].ival;
    }
    if (kind == N_VAR) {
        return env[ctx->nodes[idx].op];
    }
    if (kind == N_UN) {
        int64_t v = eval(ctx, ctx->nodes[idx].a, env);
        if (ctx->nodes[idx].op == OP_NEG) {
            return -v;
        }
        if (v == 0) {
            return 1;
        }
        return 0;
    }
    int32_t op = ctx->nodes[idx].op;
    if (op == OP_LAND) {
        if (eval(ctx, ctx->nodes[idx].a, env) == 0) {
            return 0;
        }
        if (eval(ctx, ctx->nodes[idx].b, env) == 0) {
            return 0;
        }
        return 1;
    }
    if (op == OP_LOR) {
        if (eval(ctx, ctx->nodes[idx].a, env) != 0) {
            return 1;
        }
        if (eval(ctx, ctx->nodes[idx].b, env) != 0) {
            return 1;
        }
        return 0;
    }
    int64_t l = eval(ctx, ctx->nodes[idx].a, env);
    int64_t r = eval(ctx, ctx->nodes[idx].b, env);
    switch (op) {
        case 1: return l + r;
        case 2: return l - r;
        case 3: return l * r;
        case 4: if (r == 0) { return 0; } return l / r;
        case 5: if (r == 0) { return 0; } return l % r;
        case 6: if (l < r) { return 1; } return 0;
        case 7: if (l <= r) { return 1; } return 0;
        case 8: if (l > r) { return 1; } return 0;
        case 9: if (l >= r) { return 1; } return 0;
        case 10: if (l == r) { return 1; } return 0;
        case 11: if (l != r) { return 1; } return 0;
        case 12: return l & r;
        case 13: return l | r;
        case 14: return l ^ r;
        case 15: return l << r;
        case 16: return l >> r;
        default: return 0;
    }
}

static void exec(Ctx *ctx, int32_t idx, int64_t *env, int64_t *steps) {
    int32_t node = idx;
    while (node >= 0) {
        int32_t kind = ctx->nodes[node].kind;
        *steps = *steps + 1;
        if (kind == N_ASSIGN) {
            env[ctx->nodes[node].op] = eval(ctx, ctx->nodes[node].a, env);
        } else if (kind == N_BLOCK) {
            if (ctx->nodes[node].a >= 0) {
                exec(ctx, ctx->nodes[node].a, env, steps);
            }
        } else if (kind == N_IF) {
            if (eval(ctx, ctx->nodes[node].a, env) != 0) {
                exec(ctx, ctx->nodes[node].b, env, steps);
            } else if (ctx->nodes[node].c >= 0) {
                exec(ctx, ctx->nodes[node].c, env, steps);
            }
        } else if (kind == N_WHILE) {
            while (eval(ctx, ctx->nodes[node].a, env) != 0) {
                exec(ctx, ctx->nodes[node].b, env, steps);
                *steps = *steps + 1;
            }
        }
        node = ctx->nodes[node].next;
    }
}

static int64_t run_once(Ctx *ctx, int64_t *env, int64_t *steps) {
    lex(ctx);
    int32_t root = parse_program(ctx);
    int32_t i = 0;
    while (i < NAME_CAP) {
        env[i] = 0;
        i += 1;
    }
    exec(ctx, root, env, steps);
    int64_t h = 0;
    i = 0;
    while (i < ctx->name_count) {
        h = (h * 131 + env[i] + (int64_t)(i + 1)) % 2147483647;
        i += 1;
    }
    return h;
}

int main(void) {
    uint8_t *src = (uint8_t *)malloc(SRC_CAP);
    Token *toks = (Token *)malloc((size_t)TOKEN_CAP * sizeof(Token));
    AstNode *nodes = (AstNode *)malloc((size_t)NODE_CAP * sizeof(AstNode));
    int32_t *name_off = (int32_t *)malloc((size_t)NAME_CAP * 4);
    int32_t *name_len = (int32_t *)malloc((size_t)NAME_CAP * 4);
    int64_t *env = (int64_t *)malloc((size_t)NAME_CAP * 8);
    if (src == NULL || toks == NULL || nodes == NULL || name_off == NULL || name_len == NULL || env == NULL) {
        printf("malloc failed\n");
        return 1;
    }

    Ctx ctx;
    ctx.src = src;
    ctx.src_len = gen_source(src);
    ctx.toks = toks;
    ctx.tok_count = 0;
    ctx.nodes = nodes;
    ctx.node_count = 0;
    ctx.name_off = name_off;
    ctx.name_len = name_len;
    ctx.name_count = 0;
    ctx.pos = 0;
    ctx.failed = 0;

    printf("AST interpreter: %d byte program, %d blocks\n", ctx.src_len, BLOCKS);

    int64_t steps = 0;
    int64_t check = run_once(&ctx, env, &steps);
    printf("Tokens = %d nodes = %d names = %d failed = %d\n",
           ctx.tok_count, ctx.node_count, ctx.name_count, ctx.failed);
    printf("Result = %" PRId64 " steps = %" PRId64 "\n", check, steps);

    printf("Benchmark: %d passes (lex + parse + eval)\n", PASSES);

    uint64_t t0 = bench_time_us();
    int64_t bench_sum = 0;
    int64_t total_steps = 0;
    int32_t pass = 0;
    while (pass < PASSES) {
        bench_sum = bench_sum + run_once(&ctx, env, &total_steps);
        pass += 1;
    }
    uint64_t elapsed_us = bench_time_us() - t0;

    printf("Bench sum = %" PRId64 "\n", bench_sum);
    printf("Steps = %" PRId64 "\n", total_steps);
    printf("Time: %" PRIu64 " us\n", elapsed_us);

    uint64_t per_pass_us = elapsed_us / (uint64_t)PASSES;
    printf("Per pass: ~%" PRIu64 " us\n", per_pass_us);

    free(src);
    free(toks);
    free(nodes);
    free(name_off);
    free(name_len);
    free(env);
    return 0;
}
