#ifndef IR_EXPLAIN_LEDGER_H
#define IR_EXPLAIN_LEDGER_H

#include <stddef.h>

void ir_explain_ledger_set_collect(int enabled, const char *focus_file);
void ir_explain_belief(const char *what, const char *why);
void ir_explain_proof_held(const char *type_name, const char *expression,
                           size_t line, const char *route,
                           const char *consumer);
void ir_explain_effect_held(const char *file, const char *function,
                            const char *performs, const char *needs);
void ir_explain_rule_ran(const char *rule, const char *verdict,
                         long long steps);
void ir_explain_type_payoff(const char *file, size_t line,
                            const char *function_name, const char *type_name,
                            const char *what, const char *detail);
size_t ir_explain_payoff_total(void);
void ir_explain_payoff_rows(void);
void ir_explain_ledger_flush(void);
void ir_explain_ledger_standalone(const char *focus_file);
void ir_explain_ledger_release(void);

#endif
