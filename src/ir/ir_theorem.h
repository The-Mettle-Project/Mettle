#ifndef IR_THEOREM_H
#define IR_THEOREM_H

#include "ir.h"
#include <stdio.h>

typedef struct {
  int report;
  int show_goals;
  const char *interactive;
  FILE *input;
  int stop_after;
} IRTheoremOptions;

int ir_theorem_stop_after(void);

void ir_theorem_set_options(const IRTheoremOptions *options);

int ir_program_has_theorems(const IRProgram *program);

int ir_theorems_check(IRProgram *program);

#endif
