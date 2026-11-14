#ifndef IR_OPTIMIZE_EXPLAIN_INTERNAL_H
#define IR_OPTIMIZE_EXPLAIN_INTERNAL_H

#include "../ir.h"

#include <stddef.h>

typedef enum {
  IR_EXPLAIN_STYLE_RESET = 0,
  IR_EXPLAIN_STYLE_BOLD,
  IR_EXPLAIN_STYLE_DIM,
  IR_EXPLAIN_STYLE_GREEN,
  IR_EXPLAIN_STYLE_RED
} IRExplainStyle;

const char *ir_explain_style(IRExplainStyle style);
void ir_explain_emit(const char *fmt, ...);
void ir_explain_json_raw(const char *fmt, ...);
void ir_explain_json_str(const char *s);
void ir_explain_print_header(const char *what);
void ir_explain_echo_source_range(size_t line, size_t end_line);
char *ir_explain_text_dup(const char *s);
void ir_explain_device_render(void);
int ir_explain_device_has_report(void);
void ir_tensor_explain_declines(const IRFunction *function);
void ir_gpu_staging_explain_declines(const IRFunction *function);

#endif
