// Type checker: definite-assignment (initialization) tracking.
#include "type_checker_internal.h"

void type_checker_init_tracker_reset(TypeChecker *checker) {
  if (!checker) {
    return;
  }
  for (size_t i = 0; i < checker->tracked_var_count; i++) {
    free(checker->tracked_var_names[i]);
  }
  checker->tracked_var_count = 0;
  checker->tracked_scope_count = 0;
  checker->tracked_scope_depth = 0;
  type_checker_buffer_extent_clear(checker);
}

int type_checker_init_tracker_ensure_var_capacity(TypeChecker *checker) {
  if (!checker) {
    return 0;
  }
  if (checker->tracked_var_count < checker->tracked_var_capacity) {
    return 1;
  }

  size_t new_capacity = checker->tracked_var_capacity == 0
                            ? 16
                            : checker->tracked_var_capacity * 2;
  char **new_names =
      realloc(checker->tracked_var_names, new_capacity * sizeof(char *));
  unsigned char *new_initialized = realloc(
      checker->tracked_var_initialized, new_capacity * sizeof(unsigned char));
  int *new_depths =
      realloc(checker->tracked_var_scope_depth, new_capacity * sizeof(int));
  if (!new_names || !new_initialized || !new_depths) {
    free(new_names);
    free(new_initialized);
    free(new_depths);
    return 0;
  }

  checker->tracked_var_names = new_names;
  checker->tracked_var_initialized = new_initialized;
  checker->tracked_var_scope_depth = new_depths;
  checker->tracked_var_capacity = new_capacity;
  return 1;
}

int
type_checker_init_tracker_ensure_scope_capacity(TypeChecker *checker) {
  if (!checker) {
    return 0;
  }
  if (checker->tracked_scope_count < checker->tracked_scope_capacity) {
    return 1;
  }

  size_t new_capacity = checker->tracked_scope_capacity == 0
                            ? 8
                            : checker->tracked_scope_capacity * 2;
  size_t *new_markers =
      realloc(checker->tracked_scope_markers, new_capacity * sizeof(size_t));
  if (!new_markers) {
    return 0;
  }
  checker->tracked_scope_markers = new_markers;
  checker->tracked_scope_capacity = new_capacity;
  return 1;
}

int type_checker_init_tracker_enter_scope(TypeChecker *checker) {
  if (!checker || !type_checker_init_tracker_ensure_scope_capacity(checker)) {
    return 0;
  }
  checker->tracked_scope_markers[checker->tracked_scope_count++] =
      checker->tracked_var_count;
  checker->tracked_scope_depth++;
  return 1;
}

void type_checker_init_tracker_exit_scope(TypeChecker *checker) {
  if (!checker || checker->tracked_scope_count == 0) {
    return;
  }
  int exiting_depth = checker->tracked_scope_depth;
  size_t marker =
      checker->tracked_scope_markers[checker->tracked_scope_count - 1];
  checker->tracked_scope_count--;
  while (checker->tracked_var_count > marker) {
    size_t idx = checker->tracked_var_count - 1;
    free(checker->tracked_var_names[idx]);
    checker->tracked_var_names[idx] = NULL;
    checker->tracked_var_initialized[idx] = 0;
    checker->tracked_var_scope_depth[idx] = 0;
    checker->tracked_var_count--;
  }
  type_checker_buffer_extent_exit_scope(checker, exiting_depth);
  if (checker->tracked_scope_depth > 0) {
    checker->tracked_scope_depth--;
  }
}

int type_checker_init_tracker_declare(TypeChecker *checker,
                                             const char *name,
                                             int initialized) {
  if (!checker || !name) {
    return 0;
  }
  if (!type_checker_init_tracker_ensure_var_capacity(checker)) {
    return 0;
  }

  char *name_copy = strdup(name);
  if (!name_copy) {
    return 0;
  }

  size_t idx = checker->tracked_var_count++;
  checker->tracked_var_names[idx] = name_copy;
  checker->tracked_var_initialized[idx] = initialized ? 1 : 0;
  checker->tracked_var_scope_depth[idx] = checker->tracked_scope_depth;
  return 1;
}

long long type_checker_init_tracker_find(TypeChecker *checker,
                                                const char *name) {
  if (!checker || !name) {
    return -1;
  }
  for (size_t i = checker->tracked_var_count; i > 0; i--) {
    size_t idx = i - 1;
    if (checker->tracked_var_names[idx] &&
        strcmp(checker->tracked_var_names[idx], name) == 0) {
      return (long long)idx;
    }
  }
  return -1;
}

int type_checker_init_tracker_is_initialized(TypeChecker *checker,
                                                    const char *name,
                                                    int *known) {
  if (known) {
    *known = 0;
  }
  long long idx = type_checker_init_tracker_find(checker, name);
  if (idx < 0) {
    return 0;
  }
  if (known) {
    *known = 1;
  }
  return checker->tracked_var_initialized[idx] ? 1 : 0;
}

void type_checker_init_tracker_set_initialized(TypeChecker *checker,
                                                      const char *name) {
  long long idx = type_checker_init_tracker_find(checker, name);
  if (idx >= 0) {
    checker->tracked_var_initialized[idx] = 1;
  }
}

unsigned char *type_checker_init_tracker_capture(TypeChecker *checker,
                                                        size_t *count) {
  if (count) {
    *count = 0;
  }
  if (!checker) {
    return NULL;
  }
  if (count) {
    *count = checker->tracked_var_count;
  }
  if (checker->tracked_var_count == 0) {
    return NULL;
  }

  unsigned char *snapshot =
      malloc(checker->tracked_var_count * sizeof(unsigned char));
  if (!snapshot) {
    return NULL;
  }
  memcpy(snapshot, checker->tracked_var_initialized,
         checker->tracked_var_count * sizeof(unsigned char));
  return snapshot;
}

void type_checker_init_tracker_restore(TypeChecker *checker,
                                              const unsigned char *snapshot,
                                              size_t count) {
  if (!checker || !snapshot) {
    return;
  }
  size_t limit =
      count < checker->tracked_var_count ? count : checker->tracked_var_count;
  memcpy(checker->tracked_var_initialized, snapshot,
         limit * sizeof(unsigned char));
}
