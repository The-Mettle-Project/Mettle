# Standards

| Standard | Where | What RaizeAPI follows |
|---|---|---|
| RFC 9110 HTTP Semantics | everywhere | 5.6.1 lists, 8.4 Content-Encoding, 8.8.3 ETag, 10.2.3 Retry-After, 11 authentication, 12.4.2 quality values, 12.5.1 Accept, 12.5.3 Accept-Encoding, 12.5.5 Vary, 13.1.1 If-Match, 13.1.2 If-None-Match, 13.2.2 precedence, 15 status codes (304, 400, 401, 403, 404, 405 with Allow, 406, 409, 412, 413, 415, 422) |
| RFC 9111 HTTP Caching | cache, route | Cache-Control on responses |
| RFC 9457 Problem Details | problem | every error; `type` defaults to `about:blank` with the status phrase as `title` (4.2.1); the `errors` extension from section 3 with JSON Pointers |
| RFC 6901 JSON Pointer | types/decode | `pointer` of each body error, as a URI fragment (`#/items/0/name`) |
| RFC 8259 JSON | types | strict reading (registry `json`), writing through Codec |
| RFC 3629 UTF-8 | types/format | strings in bodies are checked; overlongs and surrogates are refused |
| RFC 3339 | types/format | `date-time` and `date` |
| RFC 9562 | types/format | `uuid` |
| RFC 3986 | types/format | `uri` |
| OpenAPI 3.1.0 | openapi | the document, parameters (form and deepObject styles), request bodies, responses, components, security schemes |
| JSON Schema 2020-12 | openapi | schemas; nullable as `type: [T, "null"]`; `examples`; `readOnly` |
| RFC 8288 Web Linking | query | `Link` with first, prev and next |
| RFC 4648 section 5 | query | base64url cursors |
| JSON:API 1.1 | query | the `sort`, `filter[...]` and sparse fields conventions |
| RFC 9745 Deprecation | route | `Deprecation: @<seconds>` |
| RFC 8594 Sunset | route | `Sunset: <HTTP-date>` |
| RFC 6839 | route | `+json` media types count as JSON |
| WHATWG Fetch, CORS protocol | cors | Access-Control-* headers, preflight, `Vary: Origin` |
| RFC 1950, 1951, 1952, 8878 | negotiate | deflate, gzip and zstd (through Press) |
| RFC 6750 Bearer tokens | auth | 2.1 header syntax, 3 `WWW-Authenticate` with `invalid_token` and `insufficient_scope` |
| RFC 6585 | cache, ratelimit | 428 Precondition Required, 429 Too Many Requests |
| draft-ietf-httpapi-ratelimit-headers-11 | ratelimit | `RateLimit-Policy` and `RateLimit` |
| draft-ietf-httpapi-idempotency-key-header-08 | idempotency | `Idempotency-Key` as a Structured Fields string (RFC 9651) |
