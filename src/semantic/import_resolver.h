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

/* The module spelling a file was imported as, e.g. "std/net" for the file
 * that `import "std/net"` resolved to. Reflection reports type names qualified
 * by this, so `.name` distinguishes two modules that both declare `Point`.
 *
 * Recorded as imports resolve, keyed by the resolved absolute path, so it
 * names the module a declaration was DEFINED in rather than whichever import
 * edge happened to pull it in. A file reached by two different spellings keeps
 * the first; a file never imported (the root program) is not in the registry,
 * and the caller falls back to its stem. Returns interned storage, or NULL. */
const char *import_resolver_module_for_file(const char *resolved_path);

#endif // IMPORT_RESOLVER_H
