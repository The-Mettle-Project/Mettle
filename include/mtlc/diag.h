/* mtlc/diag.h - how libmtlc reports what went wrong.
 *
 * The backend historically wrote every failure to stderr and returned 0, which
 * is fine for a command-line driver and useless for anything else: an editor, a
 * JIT server, or a test harness embedding libmtlc could neither capture the
 * message nor suppress it. A frontend now installs a handler and receives every
 * diagnostic as a string it can format, log, or attach to its own source
 * locations.
 *
 * Install a handler on an MtlcContext (mtlc/context.h) to receive pipeline
 * diagnostics -- optimize, codegen, link -- and on an MtlcBuilder (mtlc/build.h)
 * to receive IR-construction diagnostics. Both default to writing errors to
 * stderr exactly as before, so an existing consumer that installs nothing keeps
 * its current behavior.
 *
 *   static void on_diag(void *user, MtlcDiagSeverity sev, const char *msg) {
 *     fprintf((FILE *)user, "[%s] %s\n", mtlc_diag_severity_name(sev), msg);
 *   }
 *   mtlc_context_set_diagnostic_handler(ctx, on_diag, stderr);
 */
#ifndef MTLC_DIAG_H
#define MTLC_DIAG_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  MTLC_DIAG_ERROR,   /* the operation failed; the caller's entry point returns 0/NULL */
  MTLC_DIAG_WARNING, /* the operation continued, but something is suspect */
  MTLC_DIAG_NOTE     /* extra context attached to the preceding diagnostic */
} MtlcDiagSeverity;

/* Called once per diagnostic. `message` is a complete, human-readable sentence
 * with no trailing newline; it is owned by libmtlc and is only valid for the
 * duration of the call, so copy it if you keep it. `user_data` is whatever was
 * passed when the handler was installed. A handler must not call back into the
 * context or builder that produced the diagnostic. */
typedef void (*MtlcDiagHandler)(void *user_data, MtlcDiagSeverity severity,
                                const char *message);

/* "error", "warning", or "note". */
const char *mtlc_diag_severity_name(MtlcDiagSeverity severity);

#ifdef __cplusplus
}
#endif

#endif /* MTLC_DIAG_H */
