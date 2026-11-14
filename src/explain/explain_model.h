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
  EX_TAB_KERNELS,
  EX_TAB_COUNT
};

enum { EX_TONE_PLAIN, EX_TONE_DONE, EX_TONE_MISSED, EX_TONE_FIXABLE };

enum { EXF_MISSED = 1, EXF_FIXABLE = 2 };

#define EX_CELLS 5
#define EX_DEVICES 2

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
  char *function;
  int device;
} ExEntry;

typedef struct {
  int line;
  char *text;
  char *ports;
  char *origin;
  int cost;
  int latency;
  int kernel;
  int depth;
  int label;
  int device;
  long long offset;
} ExInsn;

typedef struct {
  char *name;
  ExInsn *insns;
  size_t count;
  int device;
} ExMachineFn;

typedef struct {
  int line;
  char *text;
  int note;
  int inserted;
} ExExpandLine;

typedef struct {
  char *name;
  ExExpandLine *lines;
  size_t count;
  size_t capacity;
} ExExpandFn;

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
  ExMachineFn *machine;
  size_t machine_count;
  long long machine_total;
  ExExpandFn *expanded;
  size_t expanded_count;
  char *device_labels[EX_DEVICES];
  const ExJson *device_reports[EX_DEVICES];
  int device_count;
} ExReport;

ExReport *ex_report_build(const ExJson *root, const char *source_path);
void ex_report_free(ExReport *report);
const char *ex_report_line(const ExReport *report, int line);
const ExMachineFn *ex_report_machine(const ExReport *report,
                                     const char *function);
const ExMachineFn *ex_report_device_code(const ExReport *report,
                                         const char *function, int device);
const ExExpandFn *ex_report_expanded(const ExReport *report,
                                     const char *function);

#endif
