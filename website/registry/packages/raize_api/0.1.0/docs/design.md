# Design

## How it fits RaizeWeb

| Package | What RaizeAPI uses from it |
|---|---|
| `raize_core` | `RzContext`, request and response, headers, query decoding, arena, dates |
| `raize_app` | `RzApp`, `rz_route` for mounting, `rz_run` |
| `raize_resource` | resource definitions, the data-layer vtable and actions behind `rapi_resource` |
| `json` (registry) | the strict pull reader for request bodies |
| `codec` | the JSON writer for responses and the OpenAPI document |
| `press` | gzip, deflate and zstd |
| `raize_auth` | optional: its tokens plug into `rapi_use_bearer`'s `verify` |
| `flux_orm` | reached through raize_resource's data layer |

The public prefix is `rapi_`/`Rapi`/`RAPI_`; internals are `rapi__`.

## Modules

```
src/
  raize_api.mettle   facade
  problem/           RFC 9457 problem details, collected errors
  types/             RapiType, @rapi_model, formats, JSON decode and encode
  route/             RapiApi and operations, parameters, responses, dispatch
  openapi/           the OpenAPI 3.1 document and docs page
  query/             pages, cursors, filters, sorting, sparse fields
  rest/              operations derived from a raize resource
  cors/              CORS
  negotiate/         Accept, Accept-Encoding, compression both ways
  cache/             ETags and conditional requests
  ratelimit/         GCRA and RateLimit headers
  idempotency/       Idempotency-Key
  auth/              Bearer and API keys, scopes, policies
tests/               one program per module, plus fuzz_test
examples/todos/      a complete API
```

## Limits

Fixed at compile time, chosen to keep matching and parsing allocation-free:
256 operations per API, 8 APIs, 16 parameters and 8 responses per operation, 16 path
segments, 32 fields per object type, 32 errors per problem, 4 sort fields and 8 filters
per list. The request arena holds the decoded body and per-request strings.

## Roadmap

- JSON:API 1.1 documents and errors for resources (`application/vnd.api+json`)
- a gRPC gateway (Codec protobuf, std/net HTTP/2)
- webhooks: signed outbound requests with retries
- a raize_auth adapter once its API tokens land
