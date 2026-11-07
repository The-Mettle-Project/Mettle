#ifndef METTLE_EXPLAIN_MODEL_H
#define METTLE_EXPLAIN_MODEL_H

#include "explain_json.h"

#include <stddef.h>

enum {
  EX_TAB_OVERVIEW,
  EX_TAB_OPTIMIZER,
  EX_TAB_OWNERSHIP,
  EX_TAB_MEMORY,
  EX_TAB_FUNCTIONS,
  EX_TAB_PROOFS,
  EX_TAB_PASSES,
  EX_TAB_COUNT
};

enum { EX_TONE_PLAIN, EX_TONE_DONE, EX_TONE_MISSED, EX_TONE_FIXABLE };

enum { EXF_MISSED = 1, EXF_FIXABLE = 2 };

#define EX_CELLS 4

typedef struct {
  char *label;
  char *text;
  int tone;
} ExField;

typedef struct {
  char *group;
  char *cells[EX_CELLS];
  char *title;
  ExField *fields;
  size_t field_count;
  int line;
  int end_line;
  int tone;
  unsigned flags;
  char *code;
  int link_tab;
  int link_entry;
  char *search;
  int order;
  int repeat;
} ExEntry;

typedef struct {
  const char *name;
  const char *columns[EX_CELLS];
  int column_count;
  char **summary;
  size_t summary_count;
  ExEntry *entries;
  size_t count;
  size_t capacity;
  const char *empty;
} ExTab;

typedef struct {
  char *source_name;
  char *source_path;
  char **lines;
  size_t line_count;
  ExTab tabs[EX_TAB_COUNT];
} ExReport;

ExReport *ex_report_build(const ExJson *root, const char *source_path);
void ex_report_free(ExReport *report);
const char *ex_report_line(const ExReport *report, int line);

#endif
