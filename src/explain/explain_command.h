#ifndef METTLE_EXPLAIN_COMMAND_H
#define METTLE_EXPLAIN_COMMAND_H

#include "../main.h"

int mettle_explain_is_view_target(const char *arg);
int mettle_explain_parse_flag(CompilerOptions *options, const char *arg);
int mettle_explain_report_file(int argc, char **argv);
int mettle_explain_view(CompilerOptions *options);

#endif
