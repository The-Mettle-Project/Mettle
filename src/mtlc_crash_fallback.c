/* Windows compiler diagnostics can name an exception without pulling the
 * optional generated program crash reporter into libmtlc. */
#ifdef _WIN32
#include "runtime/crash_handler.h"

const char *mettle_crash_exception_name(DWORD code) {
  (void)code;
  return "exception";
}
#endif
