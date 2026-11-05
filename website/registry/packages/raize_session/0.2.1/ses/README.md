# ses: the session core of RaizeSession

Framework-agnostic session handling for Mettle. Any program, framework or
library can use it: the API is plain functions over strings, bytes and
structs, with no HTTP request or response types and no dependency on a web
framework.

- **No cryptography of its own.** Every random byte, SHA-256, HMAC-SHA256,
  base64url conversion and constant-time comparison comes from the
  [crypto](../cryptex) library (pure Mettle since cryptex 0.2.0).
- **No storage of its own.** The application supplies a `SesStore`
  (a vtable of five functions) over its database. Built-in storage is on
  hold until the alph actor runtime is complete (reports/002).

## Requirements

- The crypto library next to this one (`../cryptex`, pure Mettle: no C
  library to install).
- The dev Mettle compiler and mbuild 0.2.0.

## Quick start: a plain program

```mettle
// mbuild.mettle
import "mbuild/api";

fn project() {
  mb_package("app", "0.1.0");
}

fn deps() {
  mb_dep_path("raize_session", "../raize_web/libs/session");
}
```

No link flags are needed (`tests/dependent/` proves it).

```mettle
// main.mettle
import "std/io";
import "raize_session/ses/session";   // also brings crypto's cr_* functions

fn main() -> int32 {
  var st: SesStore;
  my_store_bind(&st);                       // your SesStore, see below

  var cfg: SesConfig;
  ses_config_defaults(&cfg);                // 30 min idle, 8 h absolute
  load_secret(&cfg.key.b[0], 32);           // 32 random bytes, kept secret
  var m: SesManager;
  if (ses_init(&m, &cfg, &st) != SES_OK) { return 1; }
  defer ses_close(&m);

  var now: int64 = unix_now();              // you supply the clock
  var tok: SesToken;
  var s: SesSession;
  if (ses_create(&m, "user-42", now, &tok, &s) != SES_OK) { return 1; }
  println(ses_token_str(&tok));             // give this to the client, once

  var again: SesSession;
  if (ses_lookup(&m, ses_token_str(&tok), now + 5, &again) == SES_OK) {
    println("signed in as " + ses_subject(&again));
  }
  ses_token_wipe(&tok);
  return 0;
}
```

## Using it from a web framework

The framework keeps its own request/response types and passes strings:

**On login** (after the password check):

```mettle
var tok: SesToken;
var s: SesSession;
rc = ses_create(&m, user_id, now, &tok, &s);      // a fresh token: no fixation
var o: SesCookieOpts;
ses_cookie_defaults(&o);                           // Path=/, HttpOnly, Secure, SameSite=Lax
o.max_age = ses_max_age(&m, &s, now);
var b: uint8[512];
var n: int64 = 0;
ses_cookie_format(&b[0], 512, &n, "sid", ses_token_str(&tok), &o);
add_header(resp, "Set-Cookie", str_from_bytes((cstring)&b[0], n));
ses_token_wipe(&tok);
```

If the visitor already had a pre-login session with data you want to keep,
`ses_rotate` it instead of creating a new one; never keep the old token
across a login or privilege change.

**On every request:**

```mettle
var tokv: string;
var s: SesSession;
if (ses_cookie_find(header(req, "Cookie"), "sid", &tokv) != SES_OK
    || ses_lookup(&m, tokv, now, &s) != SES_OK) {
  return redirect_to_login();                      // only SES_OK means signed in
}
if (is_unsafe_method(req) && !ses_csrf_verify(&s, form_value(req, "csrf"))) {
  return http_403();
}
// ... handler work: ses_subject(&s), ses_data_get/set, ses_flash_count/at ...
if (ses_needs_rotation(&m, &s, now)) {
  var nt: SesToken;
  if (ses_rotate(&m, &s, now, &nt) == SES_OK) { /* Set-Cookie with nt */ }
}
ses_touch(&m, &s, now);                            // extends idle expiry, saves changes
```

Pages with forms embed `ses_csrf_token(&s, &c)`; flash messages queued with
`ses_flash_add` show up in the next request's `ses_lookup`, once.

**On logout:**

```mettle
ses_revoke(&m, &s);                                // or ses_revoke_token(&m, tokv)
ses_cookie_clear(&b[0], 512, &n, "sid", &o);       // same Path/Domain as when set
add_header(resp, "Set-Cookie", str_from_bytes((cstring)&b[0], n));
```

`ses_revoke_subject(&m, user_id)` ends every session of a user (password
change, "log out everywhere"). Run `ses_prune(&m, now)` from a periodic job.

## Implementing SesStore

```mettle
fn db_put(ctx: rawptr, r: SesRecord*) -> int32 {
  var db: Db* = (Db*)ctx;
  // INSERT OR REPLACE INTO sessions(hash, subject, created, last_seen, expires,
  //   abs_expires, rotated, revoked, csrf, data, flash) VALUES (...)
  // with r->hash (32 bytes), ses_record_subject(r), the int64 fields,
  // r->csrf (32 bytes), ses_record_data(r), ses_record_flash(r).
  return SES_OK;                     // or SES_E_STORE / SES_E_LIMIT
}
fn db_get(ctx: rawptr, hash: uint8*, out: SesRecord*) -> int32 {
  // SELECT ... WHERE hash = ?; SES_E_NOT_FOUND if no row.
  // Copy columns back: memcpy(&out->hash[0], ...), out->created = ...,
  // ses_record_set_subject(out, ...), ses_record_set_data(out, ...),
  // ses_record_set_flash(out, ...).
  return SES_OK;
}
fn db_del(ctx: rawptr, hash: uint8*) -> int32 { /* DELETE WHERE hash = ? */ return SES_OK; }
fn db_del_subject(ctx: rawptr, subject: string) -> int32 { /* DELETE WHERE subject = ? */ return count; }
fn db_prune(ctx: rawptr, before: int64) -> int32 { /* DELETE WHERE revoked != 0 OR expires <= ? */ return count; }

fn my_store_bind(st: SesStore*) {
  st->ctx = (rawptr)g_db;
  st->put = &db_put;
  st->get = &db_get;
  st->del = &db_del;
  st->del_subject = &db_del_subject;
  st->prune = &db_prune;
}
```

The contract (top of `src/store.mettle`):

- **Keys** are SHA-256 of the token; the token never reaches the store.
- **Ownership:** inputs are borrowed for the call (copy what you keep);
  `get` copies a whole `SesRecord` into `*out`. `SesRecord` holds no
  pointers, so a byte store may persist the raw struct with
  `SES_RECORD_VERSION`.
- **Errors:** only `SES_OK`, `SES_E_NOT_FOUND` (get/del of a missing hash),
  `SES_E_LIMIT` (full), `SES_E_STORE` (backend failure), or a count >= 0 from
  `del_subject` / `prune`. No printing, no exit, no calls back into `ses_*`.
- **Threads:** one call at a time per session; a store shared with
  other threads must be thread-safe (lock inside each function).
- **Rotation** is `put(new)` then `del(old)`, with `del(new)` if that fails.

`tests/teststore.mettle` is a complete (test-only) example.

## Conventions

- **Prefixes:** functions `ses_`, types `Ses`, constants `SES_`.
- **Status:** `int32`: `SES_OK` (0) or a negative `SES_E_*` (`ARG`, `SPACE`,
  `NOT_FOUND`, `EXPIRED`, `REVOKED`, `INVALID`, `LIMIT`, `STORE`, `CRYPTO`,
  `PARSE`, `DUPLICATE`, `CONFIG`); `ses_status_name` names them. Results go
  through out-params; predicates return `bool` and fail closed.
- **Clock:** every time-dependent call takes `now: int64` (unix seconds);
  the library never reads a clock (reports/004), so tests inject time.
- **Ownership:** no allocation. Values read from a session (`ses_subject`,
  `ses_data_get`, `ses_flash_at`, `ses_cookie_find`, `ses_verify`) are views
  into your structs or strings; copy them to keep them.
- Each export has a `///` contract: summary, `in:`, `returns:`, `owns:`,
  `thread:`, `example:`.

## API

| Area | Functions |
|---|---|
| Config | `ses_config_defaults`, `ses_config_check`, `ses_init`, `ses_close`; `SesConfig`, `SesManager` |
| Sessions | `ses_create`, `ses_lookup`, `ses_touch`, `ses_save`, `ses_subject`, `ses_is_live`, `ses_max_age`, `ses_record_expired`, `ses_record_deadline` |
| Rotation / revocation | `ses_rotate`, `ses_needs_rotation`, `ses_revoke`, `ses_revoke_token`, `ses_revoke_subject`, `ses_prune` |
| Data | `ses_data_get`, `ses_data_set`, `ses_data_del`, `ses_data_count` |
| Flash | `ses_flash_add`, `ses_flash_count`, `ses_flash_at` |
| CSRF | `ses_csrf_token`, `ses_csrf_verify` |
| Signed values | `ses_sign`, `ses_verify` (current key, then `prev_key`; reports which matched) |
| Cookies | `ses_cookie_defaults`, `ses_cookie_format`, `ses_cookie_clear`, `ses_cookie_find`, `ses_cookie_name_ok`, `ses_cookie_value_ok`; `SesCookieOpts`, `SES_SAMESITE_*` |
| Tokens | `ses_token_str`, `ses_token_wipe`, `ses_token_new`, `ses_token_hash`; `SesToken` |
| Store | `SesStore`, `SesRecord`, `ses_store_complete`, `ses_store_put/_get/_del/_del_subject/_prune`, `ses_record_subject/_data/_flash`, `ses_record_set_subject/_data/_flash` |

Limits: subject 256 bytes; data 32 keys, key 1..64, value 0..1024, 4096
encoded bytes; flash 8 messages, kind 1..32, text 0..512, 1024 bytes.

## Defaults

| Setting | Default | Rule |
|---|---|---|
| `idle_timeout` | 1800 s | > 0 |
| `absolute_lifetime` | 28800 s | >= idle_timeout; never extended by activity |
| `touch_interval` | 60 s | 0 <= x < idle_timeout; at most one write per interval |
| `rotate_interval` | 3600 s | 0 = never; `ses_needs_rotation` says when |
| `token_bytes` | 32 | 32..64 (43..86 base64url characters) |
| `key`, `prev_key` | zero | a zero key is refused by `ses_init` |

## Security notes

- **Tokens** are 32+ bytes from the crypto library's CSPRNG, shown once, never stored:
  the store sees only SHA-256 of the token (the tests assert it). A leaked
  database does not yield usable tokens.
- **Lookups** hash the presented token, fetch by hash, then compare the
  hash in constant time; malformed tokens are rejected before any store
  call, with the same answer as unknown ones.
- **Fixation:** create a session at login, or `ses_rotate` on login and on
  every privilege change. Rotation invalidates the old token and the old
  CSRF token at once.
- **Expiry** is enforced on every lookup (idle and absolute); expired
  sessions are deleted when seen. With `touch_interval`, idle expiry can
  come up to that much earlier than configured, never later.
- **CSRF:** verify on every state-changing request. The token is
  per-session (not masked per response; reports/005).
- **Signed values** are authenticated, not encrypted, and do not expire on
  their own. Use a distinct `purpose` per use. Rotate keys by moving the old
  key to `prev_key` (`has_prev_key = true`), re-sign values for which
  `ses_verify` reports `used_prev`, and drop `prev_key` later.
- **Cookies:** keep `HttpOnly` and `Secure` (turn `Secure` off only for
  plain-HTTP local development), prefer host-only cookies (no `Domain`),
  and consider the `__Host-` name prefix. `ses_cookie_find` refuses a
  header that carries the name twice (`SES_E_DUPLICATE`), so a cookie
  planted from a sibling domain cannot shadow yours.
- **Secrets in memory:** wipe tokens with `ses_token_wipe`; `ses_close`
  wipes the manager's keys; wipe your own key copies with `cr_wipe`.
- **Concurrent requests** on one session: last write wins (reports/005).

## Threads: any thread

Every `ses_*` function works on any thread: the crypto library is pure
Mettle (cryptex 0.2.0), so there is no foreign-C restriction (this
library's reports/001 is historical). The one requirement is on the store:
the library makes one store call at a time per session but never locks, so a
`SesStore` shared between threads must be thread-safe. A `SesSession` is a
plain value for one request; do not share one between threads.

## Read-only views

`ses_peek` is `ses_lookup` without side effects: it leaves pending flash in
the record, never deletes an expired record, never updates `last_seen` and
never writes. Use it where something only needs to read the session (for
example a WebSocket page next to the user's own page request). The returned
session is read-only: `ses_save`, `ses_touch`, `ses_set_subject` and
`ses_rotate` refuse it with `SES_E_ARG`.

## Changing the subject

A session can start anonymous and gain a user at login:
`ses_set_subject(&m, &s, uid)` then `ses_rotate(...)` (rotate on login
anyway, against fixation). After that `ses_revoke_subject(&m, uid)` ends all
of that user's sessions.

## Tests

```bash
export METTLE=/path/to/Mettle/bin/mettle
mbuild test-programs            # debug and release; --debug / --release for one
```

builds `tests/` (package `session_tests`, depending on session by path) and
`tests/dependent/`, then runs each suite:

```text
OK session (119 checks)    config checks, create/lookup, idle + absolute expiry, touch rate limit,
                          rotate (old token dies; rollback on store failure), revoke, revoke-all, prune, peek (read-only), set_subject,
                          token never reaches the store in clear
OK data (55 checks)       data map get/set/delete, every limit, persistence; flash one-shot
OK csrf (16 checks)       own / other session / tampered / malformed / rotated
OK sign (30 checks)       known answer (checked with Python hmac), tampering, purpose, wrong key, rotation
OK cookie (56 checks)     exact Set-Cookie strings, clear values, strict Cookie parsing edge cases
OK dependent (...)        a dependent with no flags of its own builds and runs
```

The suites are built programs because `mbuild test` runs `@test` functions
in the interpreter, which cannot call C (reports/003).

## Known limitations

See [reports/README.md](reports/README.md).
