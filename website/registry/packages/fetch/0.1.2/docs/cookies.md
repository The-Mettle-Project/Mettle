# The cookie jar

```mettle
import "fetch/cookie";
```

```mettle
var jar: FetchCookieJar;
fetch_cookie_jar_init(&jar);
client.jar = &jar;
// ...
fetch_cookie_jar_free(&jar);
```

Attach the jar to an `FetchClient` and it does the rest: it stores the `Set-Cookie` lines of every response, redirects included, and sends a `Cookie` header with each request. You can also use it by hand.

## Calls

| Call | Does |
|---|---|
| `fetch_cookie_jar_init(j)`, `_free(j)`, `_clear(j)` | Set up, release, empty. |
| `fetch_cookie_jar_set(j, line, host, path, secure)` | Parse one `Set-Cookie` line from a response to a request for `host` and `path` (`secure`: it came over TLS) and store it. Returns `FETCH_COOKIE_OK`, `FETCH_COOKIE_DELETED` or an `HTTP_COOKIE_ERR_*` code. |
| `fetch_cookie_jar_header(j, arena, host, path, secure)` | The `Cookie` header value for a request, or `""`. |
| `fetch_cookie_parse(arena, line, host, path, secure, now, &cookie)` | Parse a line without storing it. |
| `fetch_cookie_jar_add(j, &cookie)` | Store a copy of a cookie. |
| `fetch_cookie_jar_find(j, name, domain, path)` | The stored cookie, or 0. |
| `fetch_cookie_jar_count(j)`, `_get(j, i)` | Walk the stored cookies. |
| `fetch_cookie_jar_expire(j)` | Drop expired cookies. |
| `fetch_cookie_matches(c, host, path, secure)` | Whether a cookie goes to a request. |
| `fetch_cookie_parse_date(s, &unix)` | The cookie date format of RFC 6265. |

Fields of the jar: `now` (unix seconds; 0 reads the clock, a test can set it), `max_total` (3000) and `max_per_domain` (50). When a limit is reached, expired cookies go first, then the oldest.

## Rules

- **Attributes.** `Expires`, `Max-Age`, `Domain`, `Path`, `Secure`, `HttpOnly` and `SameSite` are read. `Max-Age` wins over `Expires`. A cookie with neither lasts until the jar is freed.
- **Host-only.** A cookie with no `Domain` is sent only to the host that set it. With a `Domain`, it also goes to the subdomains.
- **Domain checks.** The request host must match the `Domain`. A `Domain` with no dot, such as `com`, is refused unless it is the host itself (`localhost`). For an IP address, the `Domain` must be that same address. There is no public suffix list.
- **Path.** Without a valid `Path`, the default is the request path up to its last slash. A cookie goes to a request when its path is a prefix on a `/` boundary.
- **Secure.** A `Secure` cookie from a response that was not over TLS is refused. It is sent only over TLS. The prefixes `__Secure-` and `__Host-` are enforced.
- **Order.** The header lists longer paths first, then older cookies.
- **Deleting.** A cookie that is already expired (or has `Max-Age` of 0 or less) removes the stored cookie with the same name, domain and path.
- **Size.** A line over 8192 bytes, or a name and value over 4096, is refused.
- **SameSite.** It is stored and has no effect. The client has no page that a request could come from.

A cookie header you pass to the client yourself replaces the jar's for that request.
