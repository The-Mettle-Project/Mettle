#include "../src/error/error_reporter.h"

#include <stdio.h>

static int check(int condition, const char *what) {
  if (!condition) {
    fprintf(stderr, "FAIL: %s\n", what);
    return 1;
  }
  return 0;
}

int main(void) {
  int failures = 0;
  ErrorReporter *reporter = error_reporter_create("cap.mettle", "fn main() {}\n");
  SourceLocation location = {0};
  location.line = 1;
  location.column = 1;
  for (int i = 0; i < 300; i++) {
    location.line = (size_t)(i + 1);
    error_reporter_add_warning(reporter, ERROR_SEMANTIC, location,
                               "a warning that fills the reporter");
  }
  failures += check(!error_reporter_has_errors(reporter),
                    "warnings alone are not errors");
  location.line = 1;
  error_reporter_add_error(reporter, ERROR_SEMANTIC, location,
                           "an error after the cap");
  failures += check(error_reporter_has_errors(reporter),
                    "an error is kept when warnings fill the reporter");
  failures += check(error_reporter_get_error_count(reporter) == 1,
                    "exactly one error");
  for (int i = 0; i < 300; i++) {
    location.line = (size_t)(i + 1);
    error_reporter_add_error(reporter, ERROR_SEMANTIC, location, "flood");
  }
  failures += check(error_reporter_get_error_count(reporter) <= 100,
                    "errors still stop at the cap");
  error_reporter_destroy(reporter);
  printf("error reporter cap: %d failures\n", failures);
  return failures != 0;
}
