#ifndef IMPORT_RESOLVER_H
#define IMPORT_RESOLVER_H

#include "../error/error_reporter.h"
#include "../parser/ast.h"
#include <stddef.h>

typedef struct {
  const char **import_directories;
  size_t import_directory_count;
  const char *stdlib_directory;
  int target_is_elf;
  int target_is_macos;
  int gpu_provider_metal;
} ImportResolverOptions;

int resolve_imports(ASTNode *program, const char *base_path,
                    ErrorReporter *reporter);
int resolve_imports_with_options(ASTNode *program, const char *base_path,
                                 ErrorReporter *reporter,
                                 const ImportResolverOptions *options);

const char *import_resolver_module_for_file(const char *resolved_path);
size_t import_resolver_file_count(void);
const char *import_resolver_file_at(size_t index);

#endif
