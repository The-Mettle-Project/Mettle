#ifndef MIR_ANNOTATE_H
#define MIR_ANNOTATE_H

#include <stddef.h>

#include "codegen/binary/mir.h"
#include "ir/ir.h"

typedef enum {
  MIR_ANNOT_SYNTAX_INTEL = 0,
  MIR_ANNOT_SYNTAX_ATT = 1,
  MIR_ANNOT_SYNTAX_BOTH = 2
} MirAnnotSyntax;

void mir_annotate_set_enabled(int enabled);
int mir_annotate_enabled(void);
void mir_annotate_set_cost_only(int cost_only);
void mir_annotate_set_output_path(const char *output_path);
void mir_annotate_set_syntax(MirAnnotSyntax syntax);
void mir_annotate_set_source_file(const char *source_file);

void mir_annotate_set_line_query(int lo, int hi, const char *fn);
void mir_annotate_set_hot_query(int n);

void mir_annotate_begin_function(const char *name, const IRFunction *ir_fn,
                                 const char *filename, size_t decl_line);
void mir_annotate_end_function(void);

void mir_annotate_record(const MirFunction *fn, const MirInst *in,
                         int mir_index, size_t byte_off, size_t byte_len,
                         const unsigned char *bytes);

void mir_annotate_record_synthetic(const char *label, const char *decision,
                                   size_t byte_off, size_t byte_len,
                                   const unsigned char *bytes);

void mir_annotate_relocate(size_t (*map)(void *, size_t), void *map_arg,
                           size_t base, const unsigned char *code);

void mir_annotate_note_backend(const char *backend, const char *reason);

void mir_annotate_record_ir(const IRFunction *ir_fn, int ir_index,
                            size_t byte_off, size_t byte_len,
                            const unsigned char *bytes);

void mir_annotate_flush(void);

#endif
