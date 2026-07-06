#ifndef ERROR_EXPLAIN_H
#define ERROR_EXPLAIN_H

/* Print extended documentation for a diagnostic code (E0001..E0007,
   M0101..M0112). Returns process exit code (0 known, 1 unknown). Passing
   NULL, "list", or "all" prints the code index. */
int mettle_explain_error_code(const char *code);

#endif /* ERROR_EXPLAIN_H */
