#ifndef META_STAGE_H
#define META_STAGE_H

#include "../error/error_reporter.h"
#include "../parser/ast.h"
#include <stdio.h>

typedef struct {
  size_t layouts;
  size_t generators;
  size_t generated_bytes;
  size_t stage_builds;
  long long steps;
  const char *pending_error;
  size_t passes_run;
  size_t passes_checked;
  size_t passes_unchecked;
} MetaStageReport;

int meta_stage_run(ASTNode *program, ErrorReporter *reporter,
                   const char *root_file, const char *source,
                   const char *target, long long fuel,
                   MetaStageReport *report);

void meta_stage_print_report(const MetaStageReport *report, FILE *out);
void meta_stage_print_pass_report(const MetaStageReport *report, FILE *out);

int meta_stage_has_passes(void);
void meta_stage_capture_passes(void *checker);
void meta_stage_print_captured(FILE *out);

#endif
