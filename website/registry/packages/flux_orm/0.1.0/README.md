# flux_orm

An ORM for the [MeoDB](../meodb) database, written in Mettle.

Status: models, queries, inserts, loads, updates of changed fields, deletes,
update_all/delete_all/count, changeset validations, relations without N+1,
migrations with up and down, sessions with transactions and stale-write
detection and automatic timestamps and Multi all work and are tested in the
interpreter, debug and release. The MeoDB adapter talks to MeoDB's server
over its wire protocol, and has been run end to end against the real server.
The other tests run against an in-memory adapter that records statements.

## Layout

```
src/flux_orm.mettle  the import root
src/core/            values, status codes, model metadata, entities, sessions,
                     clock, single-row insert/load/delete
src/adapter/         the adapter interface (SQL text, or statements as data),
                     the in-memory test adapter, the MeoDB wire codec and
                     network adapter
src/query/           SELECT/INSERT/UPDATE builder and SQL dialect
src/changeset/       casting user input, dirty fields, validations, save
src/relation/        has-many / belongs-to / through, loaded in batches
src/migration/       versioned up/down migrations, CREATE TABLE from metadata,
                     drop, add column, indexes
src/repo/            Repo calls, Multi and preload through a session; the pool
tests/               models.mettle (the model template), one t_*.mettle per area
```

## Models

Mettle reflects at compile time only, so a model's metadata comes from
`comptime for` over `typeof(T).fields`: field names, offsets and types come
from the compiler, with no hand-written offset table. An override table adds
what a struct cannot say (column names, keys, NOT NULL, unique, a version
column, `RC_F_CREATED`/`RC_F_UPDATED` timestamps). See `tests/models.mettle`.

```mettle
export struct Note {
  id: int64;
  text: string;
  version: int64;
}

const NOTE_COLS: RcColumn[3] = [
  { index: 0, column: "id", ty: RC_T_I64, flags: RC_F_PK | RC_F_AUTO | RC_F_NOT_NULL },
  { index: 1, column: "text", ty: RC_T_TEXT, flags: RC_F_NOT_NULL },
  { index: 2, column: "version", ty: RC_T_I64, flags: RC_F_VERSION | RC_F_NOT_NULL },
];

var NOTE: RcSchema;

fn note_fields(s: RcSchema*) {
  comptime for f in typeof(Note).fields {
    rc_schema_field(s, f.name, f.type.kind, (int64)f.offset, (int64)f.type.size);
  }
}

export fn note_meta() -> RcMeta {
  return rc_schema_meta(&NOTE, &note_fields, "notes", "Note", (int64)sizeof(Note),
                        &NOTE_COLS[0], 3);
}
```

`rc_schema_status(&NOTE)` says whether the model passed validation.

## Use

Add flux_orm as a path dependency in your app's `mbuild.mettle`, then import
the root:

```
mb_dep_path("flux_orm", "../flux_orm");
```

```mettle
import "flux_orm/flux_orm";
import "flux_orm/core/clock_linux";   // the real clock, if you use timestamps
import "flux_orm/adapter/meodb";     // the MeoDB adapter, if you use MeoDB
```

Connect to MeoDB's server (IPv4 address; the token is "" when it has none):

```mettle
var adapter: RcAdapter* = rc_meodb_adapter("token");
rc_adapter_open(adapter, "127.0.0.1:5490");
```

```mettle
rc_clock_set(&rc_clock_realtime_us);  // once, if any model has timestamps
var s: RcSession = rc_session_new(adapter);
defer rc_session_free(&s);

// Insert through a changeset. Casts, NOT NULL checks and validations run
// first; every error is kept per field, and an invalid changeset is not sent.
var n: Note;
memset((rawptr)&n, 0, (int64)sizeof(Note));
var e: RcEntity = rc_entity_new(note_meta(), (rawptr)&n);
var cs: RcChangeset = rc_changeset_new(note_meta());
rc_changeset_cast(&cs, "text", "draft");
rc_validate_required(&cs, "text");
rc_validate_length(&cs, "text", 1, 200);
rc_changeset_unique(&cs, "text");  // a duplicate key is reported on "text"
rc_repo_insert(&s, &cs, &e);
// On failure, rc_changeset_error_at(&cs, i) for i < rc_changeset_error_count(&cs)
// gives each error's field name, code and reason ("is too long", ...).
rc_changeset_free(&cs);

// Load by key, change one field, save. Only "text" is sent, matched on the
// version the row was read with.
var got: Note;
var ge: RcEntity;
rc_repo_get(&s, note_meta(), RcInt(n.id), (rawptr)&got, &ge);
var edit: RcChangeset = rc_changeset_new(note_meta());
rc_changeset_bind(&edit, (rawptr)&got, ge.nulls);
rc_changeset_cast(&edit, "text", "final");
var rc: int32 = rc_repo_update(&s, &edit, &ge);
// RC_ERR_CONFLICT: someone else changed the row first; nothing was written.
rc_changeset_free(&edit);

// Query, and read each row into a model.
var q: RcQuery = rc_query_select(note_meta());
rc_query_where(&q, "version", RC_CMP_GT, RcInt(1));
rc_query_order_by(&q, "id", RC_ASC);
var rows: RcRows;
rc_repo_all(&s, &q, &rows);
var i: int32 = 0;
while (i < rows.n) {
  var row: Note;
  var re: RcEntity;
  rc_rows_get(&rows, i, (rawptr)&row, &re);
  i = i + 1;
}
rc_query_free(&q);

// Loaded models point into rows the session keeps. Clear them when done.
rc_session_clear(&s);
```

Several writes that must all happen or none, after Ecto.Multi:

```mettle
var m: RcMulti = rc_multi_new();
rc_multi_insert(&m, "post", &post_cs, &post);
rc_multi_update(&m, "author", &author_cs, &author);
if (rc_repo_multi(&s, &m, RC_TX_DEFAULT) != RC_OK) {
  // rc_multi_failed_name(&m) says which step failed; nothing was written,
  // and the entities are back as they were, so the Multi can run again.
}
rc_multi_free(&m);
```

Migrations are versioned and run once each, in order, inside their own
transaction. Each step is SQL text or a callback; callbacks also work with
stores that take no SQL, like MeoDB:

```mettle
fn up_notes(a: RcAdapter*) -> int32 { return rc_migration_create(a, note_meta()); }
fn down_notes(a: RcAdapter*) -> int32 { return rc_migration_drop(a, note_meta()); }

const MIGRATIONS: RcMigration[1] = [
  { version: 1, name: "create notes", up: &up_notes, down: &down_notes },
];

var set: RcMigrationSet = rc_migration_set(&MIGRATIONS[0], 1);
var rep: RcMigrationReport;
rc_migration_run(adapter, &set, &rep);      // apply what is missing
rc_migration_down(adapter, &set, 0, &rep);  // roll back to version 0
```

`rc_migration_add_column`, `rc_migration_create_index` and
`rc_migration_drop_index` cover the other schema changes.

Load a relation for many owners in one statement, after Ecto's preload:

```mettle
var posts_rel: RcRelation = rc_relation_has_many("posts", user_meta(), "id",
                                                 post_meta(), "user_id");
var pre: RcPreload;
rc_repo_preload(&s, &users, &posts_rel, &pre);   // users: from rc_repo_all
var n: int32 = rc_preload_count(&pre, &user);    // user: one of those rows
rc_preload_get(&pre, &user, 0, (rawptr)&post, &post_e);
```

For many rows at once:

```mettle
var up: RcQuery = rc_query_update_all(user_meta());
rc_query_set(&up, "active", RcBool(false));
rc_query_where(&up, "created_at", RC_CMP_LT, RcTime(cutoff));
var changed: int32 = 0;
rc_repo_update_all(&s, &up, &changed);
rc_query_free(&up);
```

`rc_query_delete` and `rc_query_count` work the same way with
`rc_repo_delete_all` and `rc_repo_count`. An UPDATE or DELETE without a WHERE
is refused.

Every call returns an `RC_*` code (`src/core/status.mettle`). `rc_repo_one`
expects exactly one row. Wrap related writes in
`rc_session_begin`/`rc_session_commit`, or `rc_session_transaction`.

A connection is not thread-safe. A server with worker threads keeps a pool and
checks out one session per request. Checkin rolls back an open transaction and
clears the session:

```
import "flux_orm/repo/pool";

var pool: RcPool;
rc_meodb_pool(&pool, 8, "token", "127.0.0.1:5490");
var s: RcSession* = 0;
if (rc_pool_checkout(&pool, 1000, &s) == RC_OK) {   // RC_ERR_BUSY after 1 s
  rc_repo_get(s, user_meta(), RcInt(7), (rawptr)&u, &e);
  rc_pool_checkin(&pool, s);
}
rc_pool_free(&pool);
```

### Live changes and scans

MeoDB pushes row changes to a subscriber, like Postgres logical replication.
A subscription has its own connection (it cannot run queries) and never sits
in a pool. To follow a table from a known state, scan it first and subscribe
from the scan's commit: every later commit then arrives once.

```mettle
fn on_points(ctx: rawptr, pts: RcPoint*, n: int32) -> bool {
  // pts[i].x, pts[i].y, pts[i].group; return false to stop early
  return true;
}

var snap: uint64 = 0;
rc_meodb_scan(adapter, "posts", "id", "score", "", 0.0, 1000.0, 4096,
              &on_points, (rawptr)0, &snap);    // or rc_session_adapter(s)

var sub: RcSub;
var tables: string[1] = ["posts"];
rc_meodb_subscribe(&sub, "token", "127.0.0.1:5490", &tables[0], 1, 0, snap, 256);
var ev: RcEvent;
while (running) {
  var rc: int32 = rc_sub_next(&sub, &ev);
  if (rc == RC_OK) {
    // ev.kind: RC_EV_INSERT, RC_EV_UPDATE (ev.old is the row before),
    // RC_EV_DELETE, RC_EV_SCHEMA, or RC_EV_GAP (rescan, then resubscribe).
    // ev.vals[i] is column rc_sub_column(&sub, ev.table_id, i).
    rc_sub_ack(&sub, ev.seq);
  } else if (rc == RC_ERR_CLOSED) {
    Sleep(1000);
    rc_sub_reopen(&sub);   // resumes after the last event returned
  }
}
rc_sub_close(&sub);
```

A silent connection (twice the server's 15 s heartbeat) counts as closed.

raize_auth apps can keep API tokens, lockout counters and password hashes in
MeoDB through `ext/raize_auth` (package `flux_raize_auth`), a separate package
so flux_orm itself does not depend on raize_auth. See its README.

## Reference

Every exported function has a comment above it in `src/` with its inputs,
what it returns and who owns what. This is the map of the calls an app uses.

Conventions:
- Every call returns an `RC_*` status (`src/core/status.mettle`): `RC_OK` or
  an `RC_ERR_*` code. Results come back through out-parameters.
- Columns and fields are named by their field name (or the column name from
  the override table). Each such call has a `_field` twin that takes a field
  index instead, for generated code.
- Values (`RcValue`) passed in are borrowed: keep text alive until the call
  that uses them has run. Query constructors report failure in the query's
  `err`, and `rc_query_exec` returns it.

| Area | Calls |
|---|---|
| Models | `rc_schema_field`, `rc_schema_meta`, `rc_schema_status` |
| Session | `rc_session_new`, `rc_session_begin`, `rc_session_commit`, `rc_session_rollback`, `rc_session_transaction`, `rc_session_clear`, `rc_session_free` |
| Repo | `rc_repo_insert`, `rc_repo_insert_all`, `rc_repo_get`, `rc_repo_get_by`, `rc_repo_one`, `rc_repo_all` + `rc_rows_get`, `rc_repo_update`, `rc_repo_delete`, `rc_repo_update_all`, `rc_repo_delete_all`, `rc_repo_count`, `rc_repo_exists`, `rc_repo_multi`, `rc_repo_preload`, `rc_repo_preload_one`, `rc_preload_count`, `rc_preload_get` |
| Queries | `rc_query_select`, `rc_query_where`, `rc_query_where_in`, `rc_query_where_null`, `rc_query_where_not_null`, `rc_query_order_by`, `rc_query_limit`, `rc_query_offset`, `rc_query_join`, `rc_query_update_all` + `rc_query_set`, `rc_query_delete`, `rc_query_count`, `rc_query_free` |
| Changesets | `rc_changeset_new`, `rc_changeset_bind`, `rc_changeset_cast`, `rc_changeset_put`, `rc_changeset_put_null`, `rc_changeset_changed`, `rc_changeset_unique`, `rc_changeset_valid`, `rc_changeset_error_count`, `rc_changeset_error_at`, `rc_changeset_free` |
| Validations | `rc_validate_required`, `rc_validate_length`, `rc_validate_int`, `rc_validate_float`, `rc_validate_inclusion`, `rc_validate_email`, `rc_validate_charset`, `rc_validate_digits`, `rc_validate_change` |
| Entities | `rc_entity_new`, `rc_entity_get`, `rc_entity_set`, `rc_entity_pk` |
| Multi | `rc_multi_new`, `rc_multi_insert`, `rc_multi_update`, `rc_multi_delete`, `rc_multi_update_all`, `rc_multi_delete_all`, `rc_multi_run`, `rc_multi_failed_name`, `rc_multi_free` |
| Relations | `rc_relation_has_many`, `rc_relation_belongs_to`, `rc_relation_has_many_through`, `rc_relation_load`, `rc_relation_group`, `rc_relation_count`, `rc_relation_hydrate`, `rc_relation_free` |
| Migrations | `rc_migration_set`, `rc_migration_run`, `rc_migration_down`, `rc_migration_create`, `rc_migration_drop`, `rc_migration_add_column`, `rc_migration_create_index`, `rc_migration_drop_index` |
| Pool | `rc_pool_new`, `rc_meodb_pool`, `rc_pool_checkout`, `rc_pool_checkin`, `rc_pool_idle`, `rc_pool_free` |
| Adapters | `rc_meodb_adapter`, `rc_meodb_timeout`, `rc_meodb_caps`, `rc_adapter_open`, `rc_adapter_close`, `rc_mem_adapter` (tests) |
| Scans | `rc_meodb_scan` |
| Subscriptions | `rc_meodb_subscribe`, `rc_sub_next`, `rc_sub_ack`, `rc_sub_reopen`, `rc_sub_table`, `rc_sub_column`, `rc_sub_n_columns`, `rc_sub_start_seq`, `rc_sub_commit_seq`, `rc_sub_close` |
| Clock | `rc_clock_set`, `rc_clock_realtime_us` |

## Build and test

flux_orm is a library: there are no programs to link. It needs the dev Mettle
compiler; `mbuild.mettle` has no toolchain pin, so select it with `METTLE`.
`mbuild` writes a `mettle.deps` (not committed) that maps `flux_orm` to `src/`.

```
MB=/home/nox/Projects/mettle/mbuild/build/linux/release/mbuild
export METTLE=/home/nox/Projects/mettle/Mettle/bin/mettle
$MB check             # type-check src/flux_orm.mettle and what it imports
$MB test              # every @test, in the interpreter
```

Each `tests/t_*.mettle` is also a program. Build and run them in debug and
release (compiler bugs have shown up in release only):

```
for t in tests/t_*.mettle; do
  for mode in "" --release; do
    $MB mettle -- $mode --build $t -o /tmp/flux_test && /tmp/flux_test || echo "FAIL $t $mode"
  done
done
```

`tests/live_meodb.mettle` runs flux_orm against a real MeoDB server. Start one
(`../meodb/build/linux/release/meodb --db /tmp/flux_live.meo`), then build and
run that file the same way.

Imports are file-relative, so the compiler can be run from anywhere.

## License

Apache License 2.0. See [LICENSE](LICENSE).
