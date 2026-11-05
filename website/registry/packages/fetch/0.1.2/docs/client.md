# The client

```mettle
import "fetch/fetch";
```

## FetchClient

```mettle
var c: FetchClient;
fetch_client_init(&c);
// set fields
fetch_client_free(&c);
```

| Field | Default | Meaning |
|---|---|---|
| `timeout_ms` | 30000 | The most one request may take, redirects included. 0 means no limit. |
| `max_body` | 64 MiB | The most body bytes taken. It applies to the bytes on the wire and again to the decoded bytes. |
| `max_redirects` | 10 | How many redirects to follow. 0 follows none. |
| `allow_downgrade` | 0 | 1 lets a redirect go from https to http. |
| `decompress` | 1 | 1 asks for gzip and deflate and decodes them. |
| `keep_alive` | 1 | 1 keeps connections for reuse. |
| `jar` | none | An `FetchCookieJar*`. When set, cookies are stored and sent. |
| `pool` | | Limits of the idle pool, below. |
| `tls` | | https settings, below. |

Counters you can read: `dials` (connections opened), `reuses` (requests sent on a kept connection) and `retries`.

## Requests

```mettle
fetch_client_get(c, arena, url) -> Result<FetchReply*, string>
fetch_client_request(c, arena, verb, url, headers, nheaders, body) -> Result<FetchReply*, string>
```

The URL is `http://` or `https://`. `headers` are extra request headers (`HttpHeader` from `std/http`). The client adds `Host`, `Content-Length` when there is a body or the method is POST, PUT or PATCH, `Accept-Encoding` and `Cookie`. A header of yours with the same name replaces the client's. `Content-Length`, `Transfer-Encoding` and `Connection` belong to the client, and passing them is an error. Header names and values are checked, so a value cannot carry a second header.

The error is a short message. For a failed connection or handshake it ends with the reason.

```mettle
export struct FetchReply {
  status: int32;
  nheaders: int32;
  reason: string;
  headers: HttpHeader*;
  body: string;       // a zero byte follows it, not counted in the length
  url: string;        // the URL that answered, after redirects
  redirects: int32;   // how many were followed
  reused: int32;      // 1 when a kept connection carried the final request
}
```

Look headers up with `http_header` and `http_header_find` from `std/http`.

## Connection reuse

After a response the connection goes back to the pool when the response allows it: HTTP/1.1 without `Connection: close`, a body with a known length, and nothing extra on the wire. The pool is keyed by scheme, host and port.

| `c.pool` field | Default | Meaning |
|---|---|---|
| `max_idle_per_host` | 4 | Idle connections per scheme, host and port. The oldest goes first. |
| `max_idle_total` | 32 | Idle connections in all. The oldest goes first. |
| `idle_timeout_ms` | 30000 | An idle connection older than this is closed. |

Idle connections are checked when the client is used. A connection that the server closed in the meantime is thrown away before it is used. Call `fetch_client_prune(&c)` to close the old ones without sending a request. `fetch_client_free` closes them all.

A kept connection can still be closed by the server at the moment a request goes out. When that happens before any byte of the answer came back, GET, HEAD, PUT, DELETE, OPTIONS and TRACE are sent once more on a new connection. Other methods are not, and the error is returned.

## Redirects

A response with status 301, 302, 303, 307 or 308 and a `Location` is followed, up to `max_redirects`. More than that is the error `too many redirects`. A redirect status with no `Location` is returned as it is.

| Status | The next request |
|---|---|
| 303 | GET with no body (HEAD stays HEAD) |
| 301, 302 | GET with no body if it was a POST; otherwise the same method and body |
| 307, 308 | the same method and body |

When the body is dropped, `Content-Type`, `Content-Encoding` and `Content-Language` are dropped with it.

`Authorization`, `Proxy-Authorization` and a `Cookie` header you passed are not sent when the host or the port changes. Cookies from the jar are chosen again for each URL.

A redirect from https to http is the error `redirect from https to http refused`, unless `allow_downgrade` is 1.

`Location` may be absolute, start with `//`, start with `/`, be a query, or be relative to the path. `fetch_url_resolve` does this and is in `fetch/redirect`. The fragment is dropped. A `Location` with another scheme is the error `bad redirect location`.

## gzip and deflate

With `decompress` on, the request carries `Accept-Encoding: gzip, deflate` (unless you passed your own). A response with `Content-Encoding` gzip (or x-gzip) or deflate is decoded. Deflate is zlib data, and raw deflate is accepted too. In the reply, the `Content-Encoding` and `Content-Length` headers are left out, because they described the encoded body.

The decoded body may not pass `max_body`. A small body that expands past it is the error `body too large`. Data that does not decode is `bad gzip or deflate data`. Other codings, such as br, are left alone.

With `decompress` at 0, no `Accept-Encoding` is sent and the body is returned as it came. If you pass your own `Accept-Encoding`, the body is also returned as it came.

## https

`https://` URLs use mtls. The server certificate is checked against the roots, and the host name of the URL is the name checked and the SNI sent. ALPN offers `http/1.1`. TLS 1.3 and 1.2 are both allowed.

| `c.tls` field | Default | Meaning |
|---|---|---|
| `roots` | system | An `MtlsRoots*` to trust instead of the system's store. |
| `insecure` | 0 | 1 turns every certificate check off. For tests only. |

Set both before the first https request. The error for a refused certificate is `certificate refused: ` and the reason, such as `unknown issuer` or `name mismatch`. A failed handshake is `TLS handshake failed: ` and the reason.

A TLS connection that ends without a close_notify is treated as a plain end of stream.

## Error codes

`fetch/error` adds these to the `HTTP_ERR_*` codes of `std/http`. `fetch_client_error_text(code)` gives the words.

| Code | Text |
|---|---|
| `FETCH_CLIENT_ERR_REDIRECTS` | too many redirects |
| `FETCH_CLIENT_ERR_DOWNGRADE` | redirect from https to http refused |
| `FETCH_CLIENT_ERR_LOCATION` | bad redirect location |
| `FETCH_CLIENT_ERR_ENCODING` | bad gzip or deflate data |
| `FETCH_CLIENT_ERR_SCHEME` | not an http URL |
| `FETCH_CLIENT_ERR_HANDSHAKE` | TLS handshake failed |
| `FETCH_CLIENT_ERR_CERT` | certificate refused |

## FetchStream

The client reads and writes through an `FetchStream`: a context pointer, read, write and close functions, and a socket to poll (`fd`, or -1 when the stream waits for itself). `read` and `write` return a count, 0 at the end of the stream, or a negative error. `fetch_stream_plain(fd)` wraps a socket. The TLS connection is another `FetchStream`.
