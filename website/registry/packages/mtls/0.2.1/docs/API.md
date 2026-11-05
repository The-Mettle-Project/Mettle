# mtls API

Everything is exported from `import "mtls/mtls";`. Every name starts with
`mtls_`, `Mtls` or `MTLS_`. Functions that can fail return `MTLS_OK` (0)
or a negative `MTLS_E_*` code; `mtls_err_text(code)` gives the words.

## Config: `MtlsConfig`

One config can serve many connections. Do not change it while they run.

| Call / field | Meaning |
|---|---|
| `mtls_config_init(&cfg)` | Defaults: TLS 1.2 and 1.3, every supported suite, X25519 then P-256 then P-384, full certificate checks, tickets on |
| `cfg.min_version`, `cfg.max_version` | `MTLS_TLS12`, `MTLS_TLS13` |
| `mtls_config_set_suites(&cfg, ids, n)` | Restrict suites, in order of preference (IANA numbers, e.g. `0x1301`) |
| `cfg.groups[]`, `cfg.ngroups` | Key exchange groups in order: `MTLS_GROUP_X25519`, `MTLS_GROUP_SECP256R1`, `MTLS_GROUP_SECP384R1` |
| `mtls_config_add_alpn(&cfg, "h2")` | Offer (client) or accept (server) a protocol; order is preference |
| `cfg.roots` | `MtlsRoots*` to check the peer against (servers: client certificates) |
| `cfg.verify_mode` | `MTLS_VERIFY_FULL` (default), `MTLS_VERIFY_PIN` (only `cfg.pin`, the SHA-256 of the leaf's SubjectPublicKeyInfo), `MTLS_VERIFY_NONE` (tests only) |
| `mtls_config_load_cert_files(&cfg, chain, key)` | Our certificate chain (PEM, leaf first) and private key (PKCS#8, SEC 1 or PKCS#1 PEM) |
| `mtls_config_set_cert_pem(&cfg, p, n, k, kn)` | The same from memory |
| `mtls_config_set_cert(&cfg, ders, n, &key)` | DER chain you keep alive, and a loaded key (moved into the config) |
| `cfg.client_auth` | Server: `MTLS_CLIENT_AUTH_NONE`, `_REQUEST` (ask), `_REQUIRE` (ask and refuse without) |
| `cfg.tickets`, `cfg.ticket_key`, `cfg.ticket_lifetime` | Session tickets. The key is random per config; set it to share tickets across processes |
| `cfg.cache` | Client: an `MtlsSessionCache*` (`mtls_cache_new`, `mtls_cache_free`) to resume sessions |
| `cfg.allow_no_ems` | Allow TLS 1.2 peers without the extended master secret (off: refused) |
| `cfg.now` | Fixed clock in unix seconds, for tests; 0 is the real clock |
| `mtls_config_free(&cfg)` | Frees what the loaders allocated and wipes the key |

## Blocking streams: `MtlsStream`

| Call | Meaning |
|---|---|
| `mtls_dial(&s, host, port, &cfg, timeout_ms)` | TCP connect and client handshake; `host` is also the name checked |
| `mtls_dial_name(&s, addr, port, name, &cfg, timeout_ms)` | Connect to `addr`, check the certificate for `name` |
| `mtls_stream_client(&s, fd, &cfg, host)` | Client handshake on a connected socket |
| `mtls_stream_server(&s, fd, &cfg)` | Server handshake on an accepted socket |
| `mtls_stream_read(&s, buf, cap)` | Count, 0 at a clean end, `MTLS_E_EOF` if the peer closed without close_notify, or an error |
| `mtls_stream_write(&s, p, n)`, `mtls_stream_write_str(&s, text)` | Sends everything; returns n or an error |
| `mtls_stream_shutdown(&s)` | close_notify and a socket shutdown, from any thread; wakes a blocked reader |
| `mtls_stream_close(&s)` | close_notify, frees the connection, closes the socket if the stream owns it (`s.owns_fd`). No other thread may still be using it |
| `s.c` | The `MtlsConn*`, for the info calls below |
| `s.cert_error`, `s.alert_sent`, `s.alert_received` | Why `mtls_dial` failed |

## Non-blocking sockets: `MtlsNb`

For event loops (alph actors and tasks, epoll). See [nonblocking.md](nonblocking.md).

| Call | Meaning |
|---|---|
| `mtls_nb_client(&t, fd, &cfg, host)`, `mtls_nb_server(&t, fd, &cfg)` | Start on a connected or accepted socket (made non-blocking) |
| `mtls_nb_pump(&t)` | Call on each readiness event; advances the handshake |
| `mtls_nb_wants(&t)` | `MTLS_NB_READ` (1) / `MTLS_NB_WRITE` (4), the alph and epoll values; 0 when finished |
| `mtls_nb_handshaking(&t)` | Still shaking hands |
| `mtls_nb_read(&t, buf, cap)` | Count, 0 at a clean end, `MTLS_E_WANT_IO`, `MTLS_E_EOF`, or an error |
| `mtls_nb_write(&t, p, n)`, `mtls_nb_pending(&t)` | Queue and send; bytes still queued |
| `mtls_nb_close(&t)` | Idempotent cleanup, fit for an exit hook; closes the socket only if `t.owns_fd` |
| `t.c` | The `MtlsConn*`, for the info calls below |

## Connections without I/O: `MtlsConn`

| Call | Meaning |
|---|---|
| `mtls_client_new(&cfg)`, `mtls_server_new(&cfg)` | New connection (0 if out of memory) |
| `mtls_client_start(c, host)` | Queues the ClientHello. `host` is used for SNI and the name check |
| `mtls_conn_in_space(c, &p)`, `mtls_conn_in_commit(c, n)` | Receive into mtls's buffer, then process |
| `mtls_conn_feed(c, p, n)` | Copying version; returns how many bytes were taken |
| `mtls_conn_out_peek(c, &p)`, `mtls_conn_out_done(c, n)` | Bytes to send, then mark them sent |
| `mtls_conn_take_out(c, buf, cap)` | Copying version |
| `mtls_conn_wants(c)` | `MTLS_WANT_READ` / `MTLS_WANT_WRITE` bits |
| `mtls_conn_read(c, buf, cap)` | Count, 0 at a clean end, `MTLS_E_WANT_IO` (need input), or an error |
| `mtls_conn_write(c, p, n)` | Encrypts into the output queue |
| `mtls_conn_close(c)` | Queues close_notify |
| `mtls_conn_key_update(c, ask_peer)` | TLS 1.3 key update |
| `mtls_conn_export(c, label, ctx, clen, out, olen)` | TLS 1.3 exporter (RFC 8446 7.5) |
| `mtls_conn_free(c)` | Wipes and frees |

Information: `mtls_conn_handshaking`, `mtls_conn_error`, `mtls_conn_version`,
`mtls_conn_suite` (with `mtls_suite_name`), `mtls_conn_alpn`,
`mtls_conn_server_name` (the SNI a client asked for), `mtls_conn_resumed`,
`mtls_conn_peer_verified`, `mtls_conn_peer_cert_count`,
`mtls_conn_peer_cert(c, i)` (DER, 0 = leaf), `mtls_conn_cert_error`
(`MTLS_CV_*`), `mtls_conn_alert_sent`, `mtls_conn_alert_received`
(`mtls_alert_text`), `mtls_conn_peer_closed`.

## Trust stores: `MtlsRoots`

| Call | Meaning |
|---|---|
| `mtls_roots_init(&r)`, `mtls_roots_free(&r)` | Empty store / release |
| `mtls_roots_load_system(&r)` | The OS store: `$SSL_CERT_FILE`, `$SSL_CERT_DIR`, the usual Linux bundle paths, or the Windows ROOT store. Returns the count added |
| `mtls_roots_add_file(&r, path)` | A PEM bundle file |
| `mtls_roots_add_pem(&r, p, n)`, `mtls_roots_add_der(&r, p, n)` | From memory |

## Certificates and keys

| Call | Meaning |
|---|---|
| `mtls_cert_parse(&cert, der, n)` | Parse (points into `der`); `_copy` keeps its own copy, `mtls_cert_free` |
| `mtls_cert_cn(&cert)` | Subject common name, for display only |
| `mtls_cert_match_host(&cert, host)` | RFC 6125 host check against the subjectAltName |
| `mtls_cert_spki_sha256(&cert, out32)` | The value used for pins |
| `mtls_chain_verify(&roots, leaf, n, inter, ni, &opts, &res)` | Path validation on its own (`MtlsVerifyOpts`: `now`, `hostname`, `purpose`, `pin_mode`, `pin`) |
| `mtls_key_load(p, n, &key)` | Private key from PEM or DER; `mtls_privkey_free` |
| `mtls_certlist_from_pem(&l, p, n)` | Every certificate in a PEM buffer; `mtls_certlist_free` |
| `mtls_pem_iter`, `mtls_pem_next`, `mtls_pem_decode` | PEM blocks by hand |

`MTLS_CV_*` codes say why a chain was refused: `EXPIRED`, `NOT_YET_VALID`,
`UNKNOWN_ISSUER`, `BAD_SIGNATURE`, `NAME_MISMATCH`, `NOT_CA`, `PATH_LEN`,
`KEY_USAGE`, `EKU`, `UNKNOWN_CRITICAL`, `NAME_CONSTRAINT`,
`UNSUPPORTED_SIG`, `WEAK_KEY`, `TOO_LONG`, `PIN_MISMATCH`, `BAD_VERSION`,
`MALFORMED`. `mtls_cv_text` gives the words.

## Status codes

| Code | Meaning |
|---|---|
| `MTLS_E_ARG` | bad argument |
| `MTLS_E_SPACE`, `MTLS_E_NOMEM` | buffer too small, out of memory |
| `MTLS_E_DECODE`, `MTLS_E_PROTOCOL` | the peer sent something malformed or out of order |
| `MTLS_E_UNSUPPORTED` | valid but not supported here |
| `MTLS_E_CRYPTO`, `MTLS_E_VERIFY` | a primitive failed, a signature or tag did not verify |
| `MTLS_E_CERT` | the peer's certificate was refused (see `mtls_conn_cert_error`) |
| `MTLS_E_ALERT` | the peer sent a fatal alert (see `mtls_conn_alert_received`) |
| `MTLS_E_CLOSED`, `MTLS_E_EOF` | closed; closed without close_notify |
| `MTLS_E_IO`, `MTLS_E_TIMEOUT` | socket error or timeout |
| `MTLS_E_STATE`, `MTLS_E_WANT_IO` | call not valid now; non-blocking "wait for I/O" |

## Threads

`MtlsConn` and `MtlsNb` belong to one thread at a time. An `MtlsStream`
may be shared after the handshake: one thread reads while any number of
threads write (each `mtls_stream_write` goes out whole). Stop with
`mtls_stream_shutdown`, join the threads, then `mtls_stream_close`.

A config, a trust store and a session cache may be shared by connections
on many threads only if nobody changes them; the session cache is not
locked, so give each thread its own or guard it.
