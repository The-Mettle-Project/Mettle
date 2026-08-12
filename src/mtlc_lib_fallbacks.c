/* mtlc_lib_fallbacks.c - fallback definitions that keep bin/mtlc.lib
 * self-contained for an arbitrary frontend.
 *
 * A couple of backend TUs reference symbols the reference Mettle DRIVER
 * normally provides directly: the lexer's string-interning query (used by
 * common.c's free helper) and the Windows crash reporter's exception namer
 * (used by the compiler ICE handler). So that a frontend can link bin/mtlc.lib
 * ALONE -- the whole point of libmtlc -- this dedicated archive member supplies
 * default definitions.
 *
 * These are intentionally NOT weak: on the PE/COFF toolchain a weak archive
 * member is never pulled to satisfy an undefined reference. Being a member of
 * its own, it is pulled ONLY when one of these symbols is otherwise undefined.
 * The Mettle driver links its own strong lexer/crash_handler objects ahead of
 * the archive, so these symbols are already satisfied and this member is never
 * pulled there -- no duplicate-definition conflict. A frontend that provides
 * neither gets these defaults instead. */
#include "string_intern.h"

/* A frontend that does not intern strings owns every string it passes to
 * mettle_free_string, so none are interned and all are freed normally. */
int string_is_interned(const char *value) {
  (void)value;
  return 0;
}
