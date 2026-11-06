#ifndef METTLE_EXPLAIN_TUI_H
#define METTLE_EXPLAIN_TUI_H

#include "explain_model.h"
#include "explain_term.h"

#include <stdio.h>

int ex_tui_run(ExReport *report, ExTerm *term);
void ex_report_print(const ExReport *report, FILE *out);

#endif
