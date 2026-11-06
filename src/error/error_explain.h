#ifndef ERROR_EXPLAIN_H
#define ERROR_EXPLAIN_H

int mettle_explain_error_code(const char *code);
int mettle_explain_lookup(const char *code, const char **title,
                          const char **body);

#endif
