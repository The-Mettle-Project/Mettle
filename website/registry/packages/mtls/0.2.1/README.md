# mtls

TLS 1.3 and TLS 1.2 for Mettle, written in pure Mettle.

Client and server, X.509 certificate validation, mutual TLS, ALPN, SNI,
session resumption, key update, and both blocking and non-blocking I/O.
Crypto comes from [cryptex](https://github.com/Lex-Dellimere/Cryptex).

- **API reference:** [docs/API.md](docs/API.md)
- **Event loops (alph, epoll):** [docs/nonblocking.md](docs/nonblocking.md)
- **Standards and design sources:** [docs/references.md](docs/references.md)
- **Speed:** [docs/performance.md](docs/performance.md)

> **Not audited yet.** mtls passes 43 interop cases against OpenSSL in both
> directions, connects to major sites with full validation, and survives
> long fuzz runs, but no outside party has reviewed it. Keep that in mind
> before you protect high-value data with it.

## What it supports

| Area | Supported |
|---|---|
| Versions | TLS 1.3 (RFC 8446), TLS 1.2 (RFC 5246) |
| TLS 1.3 suites | TLS_AES_128_GCM_SHA256, TLS_AES_256_GCM_SHA384, TLS_CHACHA20_POLY1305_SHA256 |
| TLS 1.2 suites | ECDHE with AES-GCM or ChaCha20-Poly1305, ECDSA/Ed25519 or RSA certificates |
| Key exchange | X25519, P-256, P-384 (HelloRetryRequest when the peer prefers another group) |
| Certificates | Ed25519, ECDSA P-256 and P-384, RSA 2048-4096 (PKCS#1 v1.5 and PSS), for our own keys and the peer's |
| Extensions | SNI (RFC 6066), ALPN (RFC 7301), extended master secret (RFC 7627), secure renegotiation (RFC 5746, renegotiation itself is refused), session tickets (RFC 5077, RFC 8446) |
| Checks | full chain validation (RFC 5280), host names (RFC 6125), pinning, system trust store on Linux and Windows |
| Not included | 0-RTT early data, renegotiation, TLS 1.1 and older, RSA key exchange, CBC suites, compression |

## Requirements

- Linux x86-64 (Windows, macOS and ARM64 Linux cross-compile).
- The Mettle compiler and mbuild.
- [Cryptex](https://github.com/Lex-Dellimere/Cryptex) checked out next to
  mtls (`../cryptex`), at a version with P-256, P-384, RSA and AES-GCM.
- OpenSSL 3, only for `mbuild interop`.

## Install

In your `mbuild.mettle`:

```mettle
fn deps() {
  mb_dep_path("mtls", "../mtls");
}
```

Then `import "mtls/mtls";`.

## Client

```mettle
import "mtls/mtls";

fn fetch() -> int32 {
  var roots: MtlsRoots;
  mtls_roots_init(&roots);
  mtls_roots_load_system(&roots);          // trust what the OS trusts
  var cfg: MtlsConfig;
  mtls_config_init(&cfg);                  // TLS 1.2 + 1.3, full checks
  cfg.roots = &roots;
  mtls_config_add_alpn(&cfg, "http/1.1");

  var s: MtlsStream;
  if (mtls_dial(&s, "www.example.org", 443, &cfg, 10000) != MTLS_OK) { return 1; }
  mtls_stream_write_str(&s, "GET / HTTP/1.1\r\nHost: www.example.org\r\nConnection: close\r\n\r\n");
  var buf: uint8[4096];
  var n: int64 = mtls_stream_read(&s, &buf[0], 4096);
  mtls_stream_close(&s);
  mtls_roots_free(&roots);
  return 0;
}
```

## Server

```mettle
var cfg: MtlsConfig;
mtls_config_init(&cfg);
mtls_config_load_cert_files(&cfg, "chain.pem", "key.pem");
mtls_config_add_alpn(&cfg, "h2");
mtls_config_add_alpn(&cfg, "http/1.1");
// mutual TLS:
//   cfg.roots = &client_cas; cfg.client_auth = MTLS_CLIENT_AUTH_REQUIRE;

var s: MtlsStream;
if (mtls_stream_server(&s, accepted_fd, &cfg) == MTLS_OK) {
  // mtls_stream_read / mtls_stream_write, then mtls_stream_close
}
```

For an event loop (alph actors or tasks, epoll), use `MtlsNb`: the
handshake and all I/O move one readiness event at a time and never block,
and `mtls_nb_close` is safe to call from an exit hook. `MtlsConn` underneath
never touches a socket at all. See [docs/nonblocking.md](docs/nonblocking.md).

An `MtlsStream` can be shared by threads after the handshake: one reader
and any number of writers, with no lock of your own.

## Tools

`mbuild build -r` builds three programs into `build/linux/release`:

- `mtls-client HOST [PORT]`: connect, print what was negotiated, `--get /`
  fetches a page. `--ca`, `--pin`, `--cert/--key`, `--alpn`, `--tls12`,
  `--resume N` and more; run it without arguments for the list.
- `mtls-server --cert FILE --key FILE`: echo or `--www` test server, with
  `--require-client` for mutual TLS.
- `mtls-bench`: handshakes per second and throughput.

## Tests

```bash
mbuild test          # unit, loopback and fuzz tests, debug build
mbuild test -r       # the same, release build
mbuild interop       # 43 cases against openssl, plus the alph example
mbuild fuzz 100000   # long fuzz run of the handshake and X.509 parsers
```

## Examples

- [examples/https_get](examples/https_get): fetch a page, checked against the system roots.
- [examples/echo_server](examples/echo_server): TLS echo server, optional client certificates.
- [examples/alph_echo](examples/alph_echo): TLS server and client as alph actors, non-blocking, with exit-hook cleanup (needs alph next to mtls).

## Layout

| Path | What |
|---|---|
| src/mtls.mettle | the facade: import this |
| src/core | status codes, bounds-checked byte reader and writer |
| src/crypto | hash, HMAC, HKDF, AEAD, key exchange and signatures over cryptex |
| src/x509 | DER, PEM, certificates, private keys, chain validation, trust stores |
| src/tls | records, handshakes (client, server, 1.3, 1.2), tickets, streams |
| cmd | the three tools |
| tests | unit, loopback, fuzz and interop tests; data/ holds test certificates |

## License

Apache License 2.0. See [LICENSE](LICENSE).
