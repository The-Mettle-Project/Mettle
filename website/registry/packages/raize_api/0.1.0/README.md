# RaizeAPI

JSON APIs on [RaizeWeb](https://github.com/Lex-Dellimere/RaizeWeb), written in pure Mettle.
Package `raize_api`, prefix `rapi_`.

You describe your types and operations once. RaizeAPI parses and checks every request
before your handler runs, answers mistakes with RFC 9457 problem details, writes the
OpenAPI 3.1 document and serves a docs page.

```mettle
import "raize_api/raize_api";
import "raize_api/types/model";

@rapi_model struct NewTask { title: string; prio: int32; }
@rapi_model struct Task { id: int64; title: string; prio: int32; }

fn create_task(r: RapiReq*) -> int32 {
  var in: NewTask* = (NewTask*)rapi_body_of(r);   // already decoded and checked
  var t: Task;
  t.id = 7;
  t.title = in->title;
  t.prio = in->prio;
  return rapi_created(r, "/api/v1/tasks/7", Task_rapi(), (rawptr)&t);
}

fn tasks_app(app: RzApp*) {
  var api: RapiApi* = rapi_new(app, "/api", "Tasks", "1.0.0");
  rapi_len(rapi_field_type(NewTask_rapi(), "title"), 1, 120);
  rapi_range(rapi_field_type(NewTask_rapi(), "prio"), 1, 5);
  rapi_read_only(Task_rapi(), "id");

  rapi_group(api, "/v1");
  var op: RapiOp* = rapi_post(api, "/tasks", &create_task);
  rapi_body(op, NewTask_rapi());
  rapi_returns(op, 201, Task_rapi(), "The new task");
  rapi_group_end(api);
}

fn main() -> int32 { return rz_run("tasks", &tasks_app); }
```

A bad request gets every problem at once:

```json
{"type":"about:blank","title":"Unprocessable Content","status":422,
 "detail":"the body has invalid values",
 "errors":[{"detail":"must be at least 1 characters","pointer":"#/title"},
           {"detail":"must be at most 5","pointer":"#/prio"}]}
```

## Features

| Area | What you get |
|---|---|
| Types | `@rapi_model` structs, constraints (lengths, ranges, enums, formats), nested objects and arrays |
| Requests | typed path, query and header parameters; JSON bodies decoded straight into your struct |
| Errors | RFC 9457 problem details with a JSON Pointer per field; 400, 404, 405, 413, 415, 422 |
| Routing | operations under a base path, groups for versions, deprecation and sunset headers |
| OpenAPI | 3.1 document with JSON Schema 2020-12 at `/openapi.json`, docs page at `/docs` |
| REST | operations derived from a raize resource (`rapi_resource`) |
| Lists | limit/offset and cursor pages, `filter[field][op]`, `sort=-a,b`, `fields=a,b`, Link headers |
| HTTP | CORS, Accept and 406, gzip/deflate/zstd both ways, ETags and 304/412/428 |
| Safety | rate limits (GCRA, RateLimit headers), Idempotency-Key replay, Bearer and API key auth, scopes, policies |

Every part is optional: an API uses only the `rapi_use_*` hooks it turns on.

## Getting it

```mettle
fn deps() {
  mb_dep_path("raize_api", "../raize_api");
}
```

RaizeAPI depends on raize_core, raize_resource and raize_app (RaizeWeb), Codec, Press and
the registry `json` reader.

## Example

`examples/todos` is a full API: a resource, a hand-written operation, auth, CORS,
rate limits, idempotency, ETags and compression.

```bash
mbuild build example_todos && ./build/linux/debug/example_todos --port 8090
```

Then open `http://localhost:8090/api/docs`.

## Docs

- [docs/guide.md](docs/guide.md): how to use each part
- [docs/standards.md](docs/standards.md): the standards followed, section by section
- [docs/references.md](docs/references.md): which ideas came from which projects
- [docs/design.md](docs/design.md): module layout, how it fits RaizeWeb, roadmap

## Testing

```bash
mbuild test
mbuild test -r
mbuild fuzz 1000000
```

The tests cover each module. The fuzz run sends random and mutated bodies, paths,
queries and headers through every hook, in the release build.

## License

Apache-2.0. See [LICENSE](LICENSE).
