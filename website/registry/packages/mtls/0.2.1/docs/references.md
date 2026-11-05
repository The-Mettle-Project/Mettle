# Standards and references

## Standards implemented

| Standard | What mtls takes from it |
|---|---|
| RFC 8446 | TLS 1.3: handshake, key schedule, record protection, HelloRetryRequest, PSK resumption with `psk_dhe_ke`, KeyUpdate, exporters, downgrade protection (4.1.3), middlebox compatibility (D.4) |
| RFC 8448 | TLS 1.3 example handshakes: known-answer tests for the key schedule |
| RFC 5246 | TLS 1.2: handshake, PRF, Finished, record layer |
| RFC 5288, RFC 7905 | AES-GCM and ChaCha20-Poly1305 suites for TLS 1.2 (nonce layout) |
| RFC 8422 | ECDHE and ECDSA/EdDSA in TLS 1.2 |
| RFC 7627 | Extended master secret (required by default) |
| RFC 5746 | Secure renegotiation signalling (renegotiation itself is refused) |
| RFC 5077 | TLS 1.2 session tickets |
| RFC 7507 | TLS_FALLBACK_SCSV |
| RFC 6066 | Server Name Indication |
| RFC 7301 | ALPN |
| RFC 5280 | X.509 certificates and path validation |
| RFC 6125, RFC 9525 | Host name checks (subjectAltName only, one leftmost wildcard) |
| RFC 5869, RFC 2104 | HKDF and HMAC |
| RFC 5208, RFC 5915, RFC 8017, RFC 8410, RFC 7468 | Private key formats (PKCS#8, SEC 1, PKCS#1, Ed25519 keys) and PEM |
| X.690 | DER, strictly (minimal lengths, no indefinite forms) |

## Implementations studied, and what came from each

No code was copied; these are designs and limits, re-done in Mettle.

| Source (license) | Idea used in mtls |
|---|---|
| rustls (Apache-2.0 / MIT / ISC) | The I/O-free connection: bytes in, bytes out, the caller owns the socket (`mtls_conn_in_space` / `_commit`, `_out_peek` / `_done`). One expected message type per state, anything else is `unexpected_message`. No renegotiation, no CBC or RSA key exchange. Sealed stateless tickets. |
| webpki (ISC) | Path building that tries every candidate issuer with a budget of signature checks (mtls stops at 64), subjectAltName-only name checks, DNS name constraints, refusing unknown critical extensions. |
| Go crypto/tls and crypto/x509 (BSD-3-Clause) | Config shape (`min_version`, `max_version`, ALPN list, client auth levels, roots, pinning instead of a skip-verify callback). The `cryptobyte` pattern: a reader and writer with a sticky error and length prefixes that are opened and closed (core/bytes.mettle). Requiring a server name unless checks are off. |
| BoringSSL (ISC / OpenSSL) | Hardening limits: at most 32 empty records in a row, one ChangeCipherSpec in a TLS 1.3 handshake, keys must change on a record boundary, a bound on PSK ticket age skew (mtls allows 10 seconds), enforcing the downgrade sentinel. |
| s2n-tls (Apache-2.0) | Strict extension rules: a ServerHello or EncryptedExtensions may not carry an extension the client did not offer, duplicates are fatal, `pre_shared_key` must be last. |
| BearSSL (MIT) | Constant-time primitives (cryptex ports `aes_ct64`, `ghash_ctmul64`, `ec_p256_m31`, `ec_prime_i31`, `rsa_i31`, `ecdsa_i31`); bounded buffers sized for the largest record. |
| OpenSSL 3 (Apache-2.0) | The interop reference: `mbuild interop` runs 43 cases against `openssl s_server` and `s_client`. |

## Choices worth knowing

- **TLS 1.2 is ECDHE + AEAD only.** No static RSA key exchange (no forward
  secrecy), no CBC (padding oracles), no compression.
- **Extended master secret is required** in TLS 1.2 unless
  `cfg.allow_no_ems` is set. Without it a TLS 1.2 session is open to the
  triple handshake attack.
- **TLS 1.3 resumption always runs a fresh key exchange** (`psk_dhe_ke`
  only), so a stolen ticket key does not reveal past sessions.
- **No 0-RTT.** Early data can be replayed; leaving it out removes that risk.
- **Host names come from subjectAltName only**, never the common name.
- **Client alerts during a TLS 1.3 handshake** are sent under the handshake
  keys once those exist, because the server reads only protected records
  by then.

## Conformance

- Known-answer tests: RFC 4231 (HMAC), RFC 5869 (HKDF), RFC 8448 (TLS 1.3
  key schedule).
- Interop: OpenSSL 3.5 in both directions, both versions, every suite,
  Ed25519, P-256, P-384 and RSA certificates, HelloRetryRequest, ALPN,
  resumption, mutual TLS, refused certificates.
- Real servers: Google, GitHub, Amazon, Apple, Microsoft, Cloudflare,
  Wikipedia, Let's Encrypt, Mozilla, Reddit and others, over TLS 1.3 and
  TLS 1.2, with full validation against the system trust store.
- Fuzzing: damaged handshakes, random records, and damaged certificates
  and keys (`mbuild fuzz`), checked in a bounds-checked debug build.
- Not yet run: BoringSSL's BoGo suite and tlsfuzzer. Both need a test shim
  that drives mtls from their harness; that is the next conformance step.
