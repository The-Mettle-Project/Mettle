/*
 * C backtracking regex benchmark - counterpart to regex_match.mettle
 *
 * Compiles five patterns (literals, wildcards, bracket classes, greedy
 * quantifiers, anchors) into instruction programs, then matches each one
 * against 4000 generated log lines at every start offset.
 *
 * Build: build.bat (or: gcc -O3 -o regex_match_c.exe regex_match.c -lkernel32)
 * Run: regex_match_c.exe
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <inttypes.h>

#include "../bench_time.h"

#define LINES 4000
#define TEXT_CAP 655360
#define MAX_INST 64
#define MAX_CLASS 16
#define PATTERNS 5
#define PASSES 3

#define K_CHAR 0
#define K_ANY 1
#define K_CLASS 2
#define K_START 3
#define K_END 4

#define Q_ONE 0
#define Q_STAR 1
#define Q_PLUS 2
#define Q_OPT 3

typedef struct {
    int32_t kind;
    int32_t ch;
    int32_t cls;
    int32_t quant;
} Inst;

typedef struct {
    Inst *inst;
    int32_t count;
    uint8_t *class_bits;
    int32_t class_count;
    int32_t anchored;
} Regex;

static int32_t emit_cstr(uint8_t *dst, int32_t pos, const char *s) {
    int32_t i = 0;
    while (s[i] != 0) {
        dst[pos + i] = (uint8_t)s[i];
        i += 1;
    }
    return pos + i;
}

static int32_t emit_pad_int(uint8_t *dst, int32_t pos, int32_t v, int32_t width) {
    uint8_t tmp[16];
    int32_t n = 0;
    if (v == 0) {
        tmp[0] = 48;
        n = 1;
    }
    while (v > 0) {
        tmp[n] = (uint8_t)(48 + v % 10);
        v = v / 10;
        n += 1;
    }
    while (n < width) {
        tmp[n] = 48;
        n += 1;
    }
    while (n > 0) {
        n -= 1;
        dst[pos] = tmp[n];
        pos += 1;
    }
    return pos;
}

static int32_t emit_hex(uint8_t *dst, int32_t pos, uint32_t v, int32_t digits) {
    int32_t i = digits;
    while (i > 0) {
        i -= 1;
        int32_t nibble = (int32_t)((v >> (uint32_t)(i * 4)) & 15);
        if (nibble < 10) {
            dst[pos] = (uint8_t)(48 + nibble);
        } else {
            dst[pos] = (uint8_t)(87 + nibble);
        }
        pos += 1;
    }
    return pos;
}

static int32_t gen_corpus(uint8_t *dst, int32_t *line_off, int32_t *line_len) {
    uint32_t state = 3735928559u;
    int32_t pos = 0;
    int32_t line = 0;
    while (line < LINES) {
        line_off[line] = pos;
        state = state ^ (state << 13);
        state = state ^ (state >> 17);
        state = state ^ (state << 5);

        pos = emit_pad_int(dst, pos, 2020 + (int32_t)(state % 7), 4);
        dst[pos] = 45;
        pos += 1;
        pos = emit_pad_int(dst, pos, 1 + (int32_t)(state % 12), 2);
        dst[pos] = 45;
        pos += 1;
        pos = emit_pad_int(dst, pos, 1 + (int32_t)(state % 28), 2);
        dst[pos] = 84;
        pos += 1;
        pos = emit_pad_int(dst, pos, (int32_t)(state % 24), 2);
        dst[pos] = 58;
        pos += 1;
        pos = emit_pad_int(dst, pos, (int32_t)(state % 60), 2);
        dst[pos] = 58;
        pos += 1;
        pos = emit_pad_int(dst, pos, (int32_t)((state >> 8) % 60), 2);

        int32_t level = (int32_t)(state % 5);
        if (level == 0) {
            pos = emit_cstr(dst, pos, " [ERROR] ");
        } else if (level == 1) {
            pos = emit_cstr(dst, pos, " [WARN ] ");
        } else if (level == 2) {
            pos = emit_cstr(dst, pos, " [DEBUG] ");
        } else {
            pos = emit_cstr(dst, pos, " [INFO ] ");
        }

        pos = emit_cstr(dst, pos, "mod-");
        pos = emit_pad_int(dst, pos, (int32_t)(state % 64), 1);
        pos = emit_cstr(dst, pos, ": request id=");
        pos = emit_hex(dst, pos, state, 8);
        pos = emit_cstr(dst, pos, " status=");
        int32_t status = 200;
        int32_t roll = (int32_t)(state % 100);
        if (roll < 8) {
            status = 500 + (int32_t)(state % 4);
        } else if (roll < 20) {
            status = 400 + (int32_t)(state % 5);
        } else if (roll < 30) {
            status = 300 + (int32_t)(state % 3);
        }
        pos = emit_pad_int(dst, pos, status, 3);
        pos = emit_cstr(dst, pos, " took=");
        pos = emit_pad_int(dst, pos, 1 + (int32_t)((state >> 4) % 9000), 1);
        pos = emit_cstr(dst, pos, "ms");

        line_len[line] = pos - line_off[line];
        dst[pos] = 10;
        pos += 1;
        line += 1;
    }
    dst[pos] = 0;
    return pos;
}

static void class_set(Regex *re, int32_t cls, int32_t ch) {
    int32_t base = cls * 32;
    re->class_bits[base + ch / 8] = (uint8_t)((int32_t)re->class_bits[base + ch / 8] | (1 << (ch % 8)));
}

static int32_t class_has(Regex *re, int32_t cls, int32_t ch) {
    int32_t base = cls * 32;
    return ((int32_t)re->class_bits[base + ch / 8] >> (ch % 8)) & 1;
}

static void compile(Regex *re, const char *pattern) {
    re->count = 0;
    re->class_count = 0;
    re->anchored = 0;
    int32_t i = 0;
    while (i < MAX_CLASS * 32) {
        re->class_bits[i] = 0;
        i += 1;
    }
    i = 0;
    while (pattern[i] != 0 && re->count < MAX_INST) {
        int32_t c = (int32_t)pattern[i];
        int32_t kind = K_CHAR;
        int32_t ch = c;
        int32_t cls = -1;
        if (c == 94 && i == 0) {
            kind = K_START;
            re->anchored = 1;
            i += 1;
        } else if (c == 36 && pattern[i + 1] == 0) {
            kind = K_END;
            i += 1;
        } else if (c == 46) {
            kind = K_ANY;
            i += 1;
        } else if (c == 91) {
            kind = K_CLASS;
            cls = re->class_count;
            re->class_count += 1;
            i += 1;
            int32_t negate = 0;
            if ((int32_t)pattern[i] == 94) {
                negate = 1;
                i += 1;
            }
            while (pattern[i] != 0 && (int32_t)pattern[i] != 93) {
                int32_t lo = (int32_t)pattern[i];
                if ((int32_t)pattern[i + 1] == 45 && pattern[i + 2] != 0 && (int32_t)pattern[i + 2] != 93) {
                    int32_t hi = (int32_t)pattern[i + 2];
                    int32_t v = lo;
                    while (v <= hi) {
                        class_set(re, cls, v);
                        v += 1;
                    }
                    i += 3;
                } else {
                    class_set(re, cls, lo);
                    i += 1;
                }
            }
            if ((int32_t)pattern[i] == 93) {
                i += 1;
            }
            if (negate != 0) {
                int32_t b = 0;
                while (b < 32) {
                    re->class_bits[cls * 32 + b] = (uint8_t)(~(int32_t)re->class_bits[cls * 32 + b] & 255);
                    b += 1;
                }
            }
        } else {
            i += 1;
        }

        int32_t quant = Q_ONE;
        int32_t q = (int32_t)pattern[i];
        if (q == 42) {
            quant = Q_STAR;
            i += 1;
        } else if (q == 43) {
            quant = Q_PLUS;
            i += 1;
        } else if (q == 63) {
            quant = Q_OPT;
            i += 1;
        }

        re->inst[re->count].kind = kind;
        re->inst[re->count].ch = ch;
        re->inst[re->count].cls = cls;
        re->inst[re->count].quant = quant;
        re->count += 1;
    }
}

static int32_t inst_matches(Regex *re, int32_t pc, int32_t ch) {
    int32_t kind = re->inst[pc].kind;
    if (kind == K_ANY) {
        return 1;
    }
    if (kind == K_CHAR) {
        if (re->inst[pc].ch == ch) {
            return 1;
        }
        return 0;
    }
    if (kind == K_CLASS) {
        return class_has(re, re->inst[pc].cls, ch);
    }
    return 0;
}

static int32_t match_here(Regex *re, int32_t pc, const uint8_t *text, int32_t pos, int32_t end) {
    if (pc >= re->count) {
        return pos;
    }
    int32_t kind = re->inst[pc].kind;
    if (kind == K_START) {
        return match_here(re, pc + 1, text, pos, end);
    }
    if (kind == K_END) {
        if (pos == end) {
            return match_here(re, pc + 1, text, pos, end);
        }
        return -1;
    }
    int32_t quant = re->inst[pc].quant;
    if (quant == Q_STAR || quant == Q_PLUS) {
        int32_t run = 0;
        while (pos + run < end && inst_matches(re, pc, (int32_t)text[pos + run]) != 0) {
            run += 1;
        }
        int32_t floor_count = 0;
        if (quant == Q_PLUS) {
            floor_count = 1;
        }
        while (run >= floor_count) {
            int32_t r = match_here(re, pc + 1, text, pos + run, end);
            if (r >= 0) {
                return r;
            }
            if (run == 0) {
                break;
            }
            run -= 1;
        }
        return -1;
    }
    if (quant == Q_OPT) {
        if (pos < end && inst_matches(re, pc, (int32_t)text[pos]) != 0) {
            int32_t r = match_here(re, pc + 1, text, pos + 1, end);
            if (r >= 0) {
                return r;
            }
        }
        return match_here(re, pc + 1, text, pos, end);
    }
    if (pos < end && inst_matches(re, pc, (int32_t)text[pos]) != 0) {
        return match_here(re, pc + 1, text, pos + 1, end);
    }
    return -1;
}

static int32_t scan_lines(Regex *re, const uint8_t *text, const int32_t *line_off,
                          const int32_t *line_len, int64_t *out_span) {
    int32_t hits = 0;
    int64_t span = 0;
    int32_t line = 0;
    while (line < LINES) {
        int32_t start = line_off[line];
        int32_t end = start + line_len[line];
        int32_t pos = start;
        for (;;) {
            int32_t r = match_here(re, 0, text, pos, end);
            if (r >= 0) {
                hits += 1;
                span = span + (int64_t)(r - pos);
                break;
            }
            if (re->anchored != 0 || pos >= end) {
                break;
            }
            pos += 1;
        }
        line += 1;
    }
    *out_span = span;
    return hits;
}

static const char *pattern_text(int32_t index) {
    switch (index) {
        case 0: return "^[0-9]+-[0-9]+-[0-9]+T";
        case 1: return "status=5[0-9][0-9]";
        case 2: return "id=[a-f0-9]+ ";
        case 3: return "took=[0-9]+ms$";
        default: return "mod-[0-9]+: r.quest";
    }
}

static uint64_t run_patterns(Regex *re, const uint8_t *text, const int32_t *line_off,
                             const int32_t *line_len) {
    uint64_t h = 14695981039346656037ULL;
    int32_t p = 0;
    while (p < PATTERNS) {
        compile(re, pattern_text(p));
        int64_t span = 0;
        int32_t hits = scan_lines(re, text, line_off, line_len, &span);
        h = h * 1000003 + (uint64_t)hits;
        h = h * 31 + (uint64_t)span;
        h = h ^ (uint64_t)(re->count * 7 + re->class_count);
        p += 1;
    }
    return h;
}

int main(void) {
    uint8_t *text = (uint8_t *)malloc(TEXT_CAP);
    int32_t *line_off = (int32_t *)malloc((size_t)LINES * 4);
    int32_t *line_len = (int32_t *)malloc((size_t)LINES * 4);
    Inst *inst = (Inst *)malloc((size_t)MAX_INST * sizeof(Inst));
    uint8_t *class_bits = (uint8_t *)malloc((size_t)MAX_CLASS * 32);
    if (text == NULL || line_off == NULL || line_len == NULL || inst == NULL || class_bits == NULL) {
        printf("malloc failed\n");
        return 1;
    }

    int32_t text_len = gen_corpus(text, line_off, line_len);

    Regex re;
    re.inst = inst;
    re.count = 0;
    re.class_bits = class_bits;
    re.class_count = 0;
    re.anchored = 0;

    printf("Regex match: %d log lines, %d bytes, %d patterns\n", LINES, text_len, PATTERNS);

    int32_t p = 0;
    while (p < PATTERNS) {
        compile(&re, pattern_text(p));
        int64_t span = 0;
        int32_t hits = scan_lines(&re, text, line_off, line_len, &span);
        printf("  pattern %d: %d instructions, %d lines matched, span %" PRId64 "\n",
               p, re.count, hits, span);
        p += 1;
    }

    uint64_t check = run_patterns(&re, text, line_off, line_len);
    printf("Checksum = %" PRIu64 "\n", check);

    printf("Benchmark: %d passes (%d patterns over %d lines each)\n", PASSES, PATTERNS, LINES);

    uint64_t t0 = bench_time_us();
    uint64_t bench_hash = 0;
    int32_t pass = 0;
    while (pass < PASSES) {
        bench_hash = bench_hash * 1000003 + run_patterns(&re, text, line_off, line_len);
        pass += 1;
    }
    uint64_t elapsed_us = bench_time_us() - t0;

    printf("Bench hash = %" PRIu64 "\n", bench_hash);
    printf("Time: %" PRIu64 " us\n", elapsed_us);

    uint64_t per_pass_us = elapsed_us / (uint64_t)PASSES;
    printf("Per pass: ~%" PRIu64 " us\n", per_pass_us);

    free(text);
    free(line_off);
    free(line_len);
    free(inst);
    free(class_bits);
    return 0;
}
