# Cryptex

Safe crypto for Mettle, written in **pure Mettle**.

No C, no shared libraries, no global state. Every function is safe on any thread. Randomness comes straight from the Linux `getrandom` system call.

- **API reference:** [docs/API.md](docs/API.md)
- **Standards and sources:** [docs/references.md](docs/references.md)

> **Not audited yet.** The code follows the RFCs, matches libsodium byte for byte on thousands of test cases, and avoids secret-dependent branches by design. No outside party has reviewed it. Keep that in mind before you protect high-value data with it.

## Requirements

- Linux x86-64. Linux AArch64 builds too (not yet run on real hardware).
- The Mettle compiler and a current mbuild (with `mbuild test`).

## Install

In your `mbuild.mettle`:

```mettle
fn deps() {
  mb_dep_path("crypto", "../cryptex");
}
```

Then `import "crypto/crypto";`. Nothing to link: a program that uses Cryptex stays a static binary.

## Example

```mettle
import "crypto/crypto";

fn main() -> int32 {
  // Encrypt a value. The nonce is inside the output.
  var k: CrKey32;
  if (cr_aead_keygen(&k) != CR_OK) { return 1; }
  defer cr_wipe(&k.b[0], 32);
  var msg: string = "hello";
  var box: uint8[64];
  var blen: int64 = 0;
  cr_seal(&box[0], 64, &blen, msg.chars, (int64)msg.length, 0, 0, &k);

  var out: uint8[64];
  var olen: int64 = 0;
  if (cr_open(&out[0], 64, &olen, &box[0], blen, 0, 0, &k) != CR_OK) { return 1; }
  return 0;
}
```

## What is in it

**Recipes** (start here):

| Need | Use |
|---|---|
| Sign a cookie or link | `cr_signed_sign`, `cr_signed_verify` |
| Sign a link that expires | `cr_signed_sign_until`, `cr_signed_verify_until` |
| API token or session id | `cr_token_new`, `cr_token_hash` |
| Recovery codes | `cr_code_new`, `cr_code_hash` |
| Two-factor codes (authenticator apps) | `cr_totp`, `cr_totp_verify`, `cr_totp_uri` |
| Encrypt a value | `cr_seal`, `cr_open` |

**Primitives:**

| Area | Functions | Standard |
|---|---|---|
| Random, compare, wipe | `cr_random_bytes`, `cr_equal`, `cr_is_zero`, `cr_wipe` | Linux `getrandom` |
| Hashes | `cr_sha256`, `cr_sha384`, `cr_sha512`, `cr_blake2b`, and `_init`/`_update`/`_final` to stream | FIPS 180-4, RFC 7693 |
| Legacy hashes, not for security | `cr_md5`, `cr_sha1` (and streaming forms), only where a standard needs them, such as RFC 9562 UUIDs | RFC 1321, FIPS 180-4 |
| MAC | `cr_hmac_sha256*`, `cr_hmac_sha384*`, `cr_hmac_sha512*` | RFC 2104 |
| Key derivation | `cr_hkdf_sha256*`, `cr_hkdf_sha384*` | RFC 5869 |
| Passwords | `cr_pwhash_str`, `cr_pwhash_verify`, `cr_pwhash_needs_rehash` | Argon2id, RFC 9106 |
| Encryption | `cr_aead_encrypt`, `cr_aead_decrypt` (random nonces) | XChaCha20-Poly1305 (RFC 8439 + draft-irtf-cfrg-xchacha) |
| Encryption, counter nonces | `cr_chacha20poly1305_encrypt`, `_decrypt` | ChaCha20-Poly1305, RFC 8439 |
| Encryption for TLS | `cr_aes_gcm_init`, `_encrypt`, `_decrypt` | AES-128/256-GCM, FIPS 197 + NIST SP 800-38D |
| Key exchange | `cr_x25519_*`, `cr_kx_*` | X25519, RFC 7748 |
| Signatures | `cr_ed25519_*` | Ed25519, RFC 8032 |
| NIST P-256, P-384 | `cr_p256_*`, `cr_p384_*`: key pairs, ECDH, ECDSA sign and verify | FIPS 186-5, SEC 1, RFC 6979 |
| RSA signatures | `cr_rsa_verify_pkcs1`, `cr_rsa_verify_pss`, `cr_rsa_sign_pss`, `cr_rsa_sign_pkcs1` | RFC 8017 |
| Encoding | `cr_hex_*`, `cr_base64url_*` | RFC 4648 |


## Layout

```
src/crypto.mettle   the facade: import "crypto/crypto" gives you everything
src/core/           status codes, types, random, compare, wipe, hex/base64url
src/hash/           SHA-256, SHA-512, BLAKE2b (SHA-1 is internal, for TOTP)
src/mac/            HMAC, HKDF
src/cipher/         XChaCha20-Poly1305, ChaCha20-Poly1305
src/ecc/            X25519, Ed25519, P-256 and P-384 (ECDH, ECDSA), big integers
src/rsa/            RSA signatures (PKCS#1 v1.5, PSS)
src/password/       Argon2id password hashing
src/recipes/        signed values, tokens and codes, seal/open, TOTP
tests/              test programs; test data in tests/vectors/
examples/basic/     a small project that uses the library
examples/bench/     speed of every primitive (`mbuild run -r`)
docs/API.md         every public function
```

## Rules

- **Names:** functions `cr_`, types `Cr`, constants `CR_`. Names starting `crx_` are internal.
- **Results:** functions return `CR_OK` (0) or a negative `CR_E_*` code. Output goes through pointers. Checks like `cr_equal` return `bool` and say `false` on any error.
- **Buffers:** you own every buffer. Cryptex never allocates on your heap and never keeps a pointer. If a buffer is too small you get `CR_E_SPACE` and nothing is written. (Argon2id maps its own work memory and unmaps it before it returns.)
- **Any thread.** No function shares state. `cr_init` does nothing and is only kept so old code still compiles.
- **Wipe your secrets** with `cr_wipe` when done: `defer cr_wipe(&k.b[0], 32);`
- **Compare secrets** with `cr_equal`, never `==`.
- **One key per job.** Do not use a signing key for anything else.

## Password presets

| Preset | Cost | Use |
|---|---|---|
| `CR_PW_INTERACTIVE` | t=2, 64 MiB | logins (default, about 0.2 s) |
| `CR_PW_MODERATE` | t=3, 256 MiB | when you can wait about 1 s |
| `CR_PW_SENSITIVE` | t=4, 1 GiB | rare, high-value unlocks (several seconds) |

Store the text from `cr_pwhash_text`. After a good login, `cr_pwhash_needs_rehash` tells you when to store a new hash.

## Speed

Release build on one x86-64 machine (2026-11-05), next to OpenSSL 3.5 on the
same machine. Run `cd examples/bench && mbuild run -r` to measure yours.

| Operation | Cryptex | OpenSSL 3.5 |
|---|---|---|
| AES-128-GCM, 16 KiB (AES-NI) | 3.6 GB/s | 11.0 GB/s |
| AES-256-GCM, 16 KiB (AES-NI) | 3.2 GB/s | 9.8 GB/s |
| AES-128-GCM, portable constant-time code | 93 MB/s | - |
| ChaCha20-Poly1305, 16 KiB | 1.6 GB/s | 3.9 GB/s |
| SHA-256, 16 KiB (SHA-NI) | 1.6 GB/s | 1.95 GB/s |
| SHA-512, 16 KiB | 210 MB/s | 0.91 GB/s |
| X25519 | 103 us | 35 us |
| Ed25519 sign / verify | 51 / 134 us | 41 / 116 us |
| P-256 ECDH | 500 us | 62 us |
| P-256 ECDSA sign / verify | 447 / 923 us | 25 / 75 us |
| P-384 ECDSA sign / verify | 1.6 / 3.0 ms | 0.29 / 0.70 ms |
| RSA-2048 sign / verify | 3.5 ms / 192 us | 0.33 ms / 21 us |
| Argon2id, INTERACTIVE preset | 239 ms | - |

OpenSSL uses hand-written assembly with SIMD and the CPU's SHA and AES
instructions. Cryptex uses AES-NI and PCLMULQDQ for AES-GCM, SSSE3 or AVX2
for ChaCha20 and SHA-NI for SHA-256, picked at run time, and 64-bit
multiplies in assembly for Poly1305; everything else is portable Mettle
code, and the gaps above are the next speed work.

## Tests

```bash
export METTLE=/path/to/Mettle/bin/mettle
mbuild test        # debug
mbuild test -r     # release
```

Each `tests/<name>_test.mettle` is a test program. Two kinds of checks:

- **Standard vectors** from the RFCs and FIPS documents.
- **Wycheproof:** 1,954 known-hard edge cases from Google's Wycheproof project (X25519, Ed25519, both ChaCha20-Poly1305 forms, HMAC-SHA1/256/512, HKDF) in `tests/vectors/vec_wp_*.mettle`.
- **libsodium vectors:** `tests/vectors/vec_*.mettle`, thousands of random cases computed by libsodium (and Python for SHA-1 and HKDF). Cryptex must match them byte for byte. The library and its tests do not need libsodium.

`examples/basic` is a small project that uses Cryptex the way your project would. Run it with `cd examples/basic && mbuild run`.

## Docs

`docs/API.md` lists every public function, built from the `///` comments in `src/`.

## License

Apache License 2.0. See [LICENSE](LICENSE). Some code is ported from BearSSL (MIT) and Monocypher (BSD-2-Clause); see [NOTICE](NOTICE).
