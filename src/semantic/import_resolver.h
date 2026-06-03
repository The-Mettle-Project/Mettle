#ifndef IMPORT_RESOLVER_H
#define IMPORT_RESOLVER_H

#include "../error/error_reporter.h"
#include "../parser/ast.h"
#include <stddef.h>

typedef struct {
  const char **import_directories;
  size_t import_directory_count;
  const char *stdlib_directory;
  /* When nonzero, the compilation targets the native ELF (Linux) backend. The
   * resolver then prefers an OS-specific `<name>.linux.mettle` sibling over the
   * plain `<name>.mettle` for std imports, so Linux gets syscall-based stdlib
   * variants without any source-level platform branching. */
  int target_is_elf;
} ImportResolverOptions;

// Resolves imports by finding AST_IMPORT nodes, lexing/parsing the imported
// files, and merging their declarations into the main program's AST. Returns 1
// on success, 0 on failure.
int resolve_imports(ASTNode *program, const char *base_path,
                    ErrorReporter *reporter);
int resolve_imports_with_options(ASTNode *program, const char *base_path,
                                 ErrorReporter *reporter,
                                 const ImportResolverOptions *options);

#endif // IMPORT_RESOLVER_H
