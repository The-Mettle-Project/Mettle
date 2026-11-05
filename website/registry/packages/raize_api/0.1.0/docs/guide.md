# Guide

Everything is declared once, at startup, inside raize's declaration function. Handlers
run on worker threads and only read what was declared.

## An API

```mettle
var api: RapiApi* = rapi_new(app, "/api", "Tasks", "1.0.0");
```

`rapi_new` mounts one raize route per verb, `<base>/*rapi_rest`, so RaizeAPI matches
every path under the base itself. Unknown paths and verbs get problem details (404,
405 with `Allow`), and `OPTIONS` answers with `Allow`. Pass `0` for `app` to drive the
API yourself with `rapi_handle(api, ctx)`, as the tests do.

Declaration mistakes (a bad path template, the same route twice) do not stop the app at
once; `rapi_error(api)` returns the first one. Hand it to `rz__app_error` so `rz_run`
reports it with raize's other mistakes.

## Types

A `RapiType` describes a JSON value and where it lives in a struct. The easiest way to
get one is `@rapi_model`:

```mettle
@rapi_model struct Task {
  id: int64;
  title: string;
  done: bool;
  tags: CodecList<string>;
}
```

This writes `Task_rapi() -> RapiType*`. Field types: `string`, `int32`, `int64`,
`float64`, `bool`, another `@rapi_model` struct, and `CodecList<T>` of these (a JSON
array). Every field starts out required. Refine the type after the first call:

| Call | Effect |
|---|---|
| `rapi_len(t, min, max)` | string length in characters |
| `rapi_range(t, min, max)` | integer range |
| `rapi_items(t, min, max)` | array length |
| `rapi_enum(t, "a")` | allowed string values (call once per value) |
| `rapi_format(t, "email")` | `date-time`, `date`, `email`, `uuid` and `uri` are checked |
| `rapi_nullable(t)` | `null` is accepted |
| `rapi_desc(t, text)`, `rapi_example(t, json)` | for the docs |
| `rapi_require(t, "field", false)` | the member may be left out |
| `rapi_read_only(t, "id")` | written in responses, refused in requests |
| `rapi_closed(t)` | unknown members are an error |
| `rapi_field_type(t, "field")` | the field's type, to constrain it in place |

Use `RAPI_UNSET` for an open end: `rapi_range(rapi_int64_t(), 1, RAPI_UNSET)`.

Without the decorator, build types by hand with `rapi_object_t`, `rapi_field` and the
`rapi_*_t` constructors.

## Operations

```mettle
rapi_group(api, "/v1");
rapi_group_tag(api, "tasks");
var op: RapiOp* = rapi_get(api, "/tasks/:id", &get_task);
rapi_summary(op, "Get a task");
rapi_path_param(op, "id", rapi_range(rapi_int64_t(), 1, RAPI_UNSET), "The task id");
rapi_query(op, "expand", rapi_bool_t(), false, "Include the comments");
rapi_header_param(op, "X-Request-Id", rapi_format(rapi_string_t(), "uuid"), false, "");
rapi_returns(op, 200, Task_rapi(), "The task");
rapi_group_end(api);
```

Paths use raize's form: a segment is text or one whole `:name`. The OpenAPI document
shows `{name}`. Literal segments win over parameters (`/tasks/new` over `/tasks/:id`).

A query parameter with an array type takes repeated keys (`?tag=a&tag=b`). A header
array takes a comma list.

Other calls: `rapi_body`, `rapi_body_list`, `rapi_body_optional`, `rapi_returns_list`,
`rapi_param_default`, `rapi_describe`, `rapi_tag`, `rapi_op_id`, `rapi_cache`
(Cache-Control on success), `rapi_op_max_body`, `rapi_deprecate`, `rapi_op_set_data`.

## Handlers

```mettle
fn get_task(r: RapiReq*) -> int32 {
  var id: int64 = rapi_int(r, "id");
  var t: Task* = find_task(id);
  if (t == 0) { return rapi_fail(r, 404, "no such task"); }
  return rapi_json(r, 200, Task_rapi(), (rawptr)t);
}
```

Reading: `rapi_int`, `rapi_str`, `rapi_float`, `rapi_bool`, `rapi_has`, `rapi_count`,
`rapi_str_at`, `rapi_int_at`, `rapi_body_of`, `rapi_body_has` (for PATCH), `rapi_header`.

Answering: `rapi_json`, `rapi_json_list`, `rapi_json_text`, `rapi_created`,
`rapi_no_content`, `rapi_fail`, `rapi_set_header`, or `rapi_writer_begin` and
`rapi_writer_send` to write the body with Codec's JSON writer yourself. For a problem with
your own type URI use `rapi_problem_typed(r->ctx, ...)`.

What happens before the handler, in order:

1. edge hooks (CORS)
2. matching: 404, 405, OPTIONS
3. before hooks (auth, rate limits, idempotency, 406, request decompression)
4. parameters: 400 with every bad parameter
5. body: 413 too large, 415 not JSON, 400 not valid JSON, 422 values that fail checks
6. the handler
7. after hooks (ETag and 304, compression, storing idempotent results)

## Versions

Put each version in a group. Mark an old one deprecated, with the date it was
deprecated and when it goes away (unix seconds):

```mettle
rapi_group(api, "/v1");
rapi_group_deprecate(api, 1767225600, 1798761600);
...
rapi_group_end(api);
```

Its responses carry `Deprecation: @1767225600` (RFC 9745) and `Sunset: <date>` (RFC 8594),
and the OpenAPI document marks its operations deprecated.

## REST resources

```mettle
rapi_resource(api, "/todos", todo_resource(), dl);
```

This declares list, get, create, update, delete and one `POST .../:id/<name>` per custom
action, for the actions the resource declares. Bodies decode straight into the record,
raize runs the resource's own checks, and its messages come back as 422 problems with
pointers. Schemas are `<Type>`, `<Type>Create` (accepted fields only, closed) and
`<Type>Update`.

## Lists

```mettle
var l: RapiListing* = rapi_listing(op, Task_rapi(), 20, 100);
rapi_listing_sort(op, l, "title prio");
rapi_listing_filter(op, l, "done prio");
rapi_listing_fields(op, l);
// rapi_listing_cursor(op, l);   // cursor pages on id instead of offsets

fn list_tasks(r: RapiReq*) -> int32 {
  var q: RapiQuery;
  var st: int32 = rapi_query_parse(r, &q);
  if (st != RAPI_NEXT) { return st; }
  var page: rawptr = (rawptr)0;
  var n: int64 = 0;
  rapi_query_apply(&q, Task_rapi(), all_tasks, count, r->ctx->arena, &page, &n);
  return rapi_json_page(r, &q, Task_rapi(), page, n);
}
```

Callers write `?limit=20&offset=40&sort=-prio,title&filter[done]=false&filter[prio][gte]=2&fields=id,title`.
Filter operators: `eq` (the default), `ne`, `lt`, `lte`, `gt`, `gte`, `contains`
(text, ignoring case) and `in` (a comma list). The response is
`{"items":[...],"total":57,"limit":20,"offset":40}` with a `Link` header.

With a database behind the handler, read `q` (limit, offset, sorts, filters) and build
the query there instead of calling `rapi_query_apply`.

## Turning things on

```mettle
rapi_use_cors(api, "https://app.example.com", true, 600);
rapi_use_bearer(api, "tasks", &verify_token);    // or rapi_use_api_key
rapi_use_rate_limit(api, 120, 60);
rapi_use_idempotency(api, 86400, 10000);
rapi_use_negotiation(api);
rapi_use_etags(api);
rapi_use_compression(api, 1024);
```

Hooks run in the order they are turned on, so keep this order: auth before rate limits
(so callers are counted by who they are) and before idempotency (so keys are kept per
caller). Turn on ETags before compression (so the ETag is taken from the plain body).

- **Auth**: `rapi_secure(op, "scope ...")` or `rapi_group_secure(api, ...)`;
  `rapi_public(op)`; `rapi_policy(op, &check, "why")`; `rapi_has_scope(r, scope)` in
  handlers. `verify` gets the token and sets `r->subject`, `r->scopes` and
  `r->principal`. raize_auth's tokens plug in there.
- **Rate limits**: `rapi_op_rate(op, limit, secs)` for one operation, `rapi_rate_key`
  to choose who is counted.
- **Idempotency**: `rapi_idempotent(op, required)` on POST and PATCH operations.
- **ETags**: GET responses get an ETag and answer `If-None-Match` with 304. For writes,
  `rapi_require_if_match(op)` and `rapi_if_match(r, current_etag)`.

## Your own hooks

`rapi_before(api, &fn)` runs `fn(RapiReq*) -> int32` before the handler: return
`RAPI_NEXT` to go on, or send a response and return its status. `rapi_after(api, &fn)`
gets the status and returns the one to keep. `rapi_edge(api, &fn)` runs first for every
request under the base, matched or not.
