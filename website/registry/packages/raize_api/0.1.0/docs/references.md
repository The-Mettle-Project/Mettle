# References

Ideas taken from existing projects, and why. No code was copied; RaizeAPI is written
from the standards and these designs.

| Project | License | What RaizeAPI took |
|---|---|---|
| FastAPI (Python) | MIT | the request is checked against types before the handler runs; the OpenAPI document and docs page come from the same declarations; 422 for a body that parses but fails checks |
| Huma (Go) | MIT | RFC 9457 problem details for every error with a list of field errors; operations registered with explicit metadata; a schema registry where named types become components |
| utoipa (Rust) | MIT / Apache-2.0 | a derive (here `@rapi_model`) writes the schema from the struct, so the type and its schema cannot drift |
| axum (Rust) | MIT | typed access to path, query and header values; rejections become responses before the handler |
| Django REST framework | BSD-3-Clause | LimitOffset pagination with a count; filters and ordering declared per view; per-caller throttling |
| AshJsonApi (Elixir) | MIT | REST routes derived from a resource's declared actions; input schemas from accepted attributes |
| JSON:API 1.1 | CC0 | `sort=-a,b`, `filter[...]` and sparse fields |
| redis-cell, throttled (Go) | MIT, BSD-3-Clause | GCRA rate limiting: one number per caller, exact bursts, no timers |
| xxHash | BSD-2-Clause | xxHash64 for ETags: fast on large bodies with good mixing |
| Stripe API | (public docs) | idempotent requests: the key is scoped to the caller, replays answer with the first response, a different body with the same key is an error |
| Swagger UI | Apache-2.0 | the docs page loads swagger-ui-dist from a CDN (it can be pointed elsewhere with `rapi_docs_cdn`) |

## Choices

- **Problem details, not a custom error shape.** RFC 9457 is the published standard, and
  Huma and FastAPI both show that one shape for every error is what clients want.
- **Bodies decoded into structs by offset.** One pass of the `json` pull reader writes each
  member straight into place and collects every error. Nothing is built in between.
- **Paths with `:name`.** Mettle reads `{name}` in a string as interpolation, and raize's
  router already uses `:name`. The OpenAPI document shows `{name}`.
- **One raize route per verb per API.** raize allows 64 routes per app; an API can have 256
  operations, and its own matcher gives problem details for 404 and 405.
- **GCRA over token buckets with timers.** One timestamp per key, checked and moved under a
  mutex.
- **Cursor pages on id.** Keyset pages stay correct while rows are added; offsets are kept
  for lists that need a total and random access.
