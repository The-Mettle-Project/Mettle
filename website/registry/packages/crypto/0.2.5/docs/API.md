# Cryptex API

Generated from the `///` contracts in `src/`. Do not edit by hand.

Pure Mettle: no C, no shared state. Every function is safe on any thread.

Primitives: [core/status](#corestatus), [core/types](#coretypes), [core/util](#coreutil), [core/encode](#coreencode), [hash/hash](#hashhash), [hash/legacy](#hashlegacy), [mac/mac](#macmac), [mac/kdf](#mackdf), [password/pwhash](#passwordpwhash), [cipher/aead](#cipheraead), [cipher/gcm](#ciphergcm), [ecc/kx](#ecckx), [ecc/sign](#eccsign), [ecc/p256](#eccp256), [ecc/p384](#eccp384), [ecc/ecdsa](#eccecdsa), [rsa/rsa](#rsarsa).
Recipes: [recipes/signed](#recipessigned), [recipes/token](#recipestoken), [recipes/seal](#recipesseal), [recipes/totp](#recipestotp).

## core/status

Status codes shared by every crypto module.

### CR_OK

```mettle
export const CR_OK: int32 = 0;
```

the operation succeeded.

- **returns:** 0
- **owns:** nothing
- **thread:** constant

```mettle
if (cr_sha256(&d, p, n) != CR_OK) { ... }
```

### CR_E_INIT

```mettle
export const CR_E_INIT: int32 = -1;
```

never returned; kept so existing code that names it still compiles.

- **returns:** -1
- **owns:** nothing
- **thread:** constant

```mettle
if (cr_init() == CR_E_INIT) { abort(); }  // never true
```

### CR_E_ARG

```mettle
export const CR_E_ARG: int32 = -2;
```

an argument is invalid (null pointer with a non-zero length, negative length, length outside the primitive's allowed range).

- **returns:** -2
- **owns:** nothing
- **thread:** constant

```mettle
cr_random_bytes(0, 4) == CR_E_ARG
```

### CR_E_SPACE

```mettle
export const CR_E_SPACE: int32 = -3;
```

the caller's output buffer is too small. Nothing is written.

- **returns:** -3
- **owns:** nothing
- **thread:** constant

```mettle
cr_hex_encode(&buf[0], 2, &n, p, 8) == CR_E_SPACE
```

### CR_E_VERIFY

```mettle
export const CR_E_VERIFY: int32 = -4;
```

authentication failed (AEAD tag mismatch). The output is wiped.

- **returns:** -4
- **owns:** nothing
- **thread:** constant

```mettle
if (cr_aead_decrypt(...) == CR_E_VERIFY) { reject(); }
```

### CR_E_DECODE

```mettle
export const CR_E_DECODE: int32 = -5;
```

hex or base64url input is malformed or non-canonical.

- **returns:** -5
- **owns:** nothing
- **thread:** constant

```mettle
cr_hex_decode(&b[0], 4, &n, "zz", 2) == CR_E_DECODE
```

### CR_E_INTERNAL

```mettle
export const CR_E_INTERNAL: int32 = -6;
```

an internal failure that is not the caller's fault (the kernel random source failed, or memory for Argon2id could not be mapped).

- **returns:** -6
- **owns:** nothing
- **thread:** constant

```mettle
if (rc == CR_E_INTERNAL) { retry_later(); }
```

### CR_E_KEY

```mettle
export const CR_E_KEY: int32 = -7;
```

a public key is unusable (X25519 result is all zero, i.e. the peer sent a low-order point). Outputs are wiped.

- **returns:** -7
- **owns:** nothing
- **thread:** constant

```mettle
if (cr_x25519_shared(&s, &sk, &peer) == CR_E_KEY) { reject(); }
```

### CR_E_EXPIRED

```mettle
export const CR_E_EXPIRED: int32 = -8;
```

a signed value is authentic but its expiry time has passed (cr_signed_verify_until). The value is not released.

- **returns:** -8
- **owns:** nothing
- **thread:** constant

```mettle
if (rc == CR_E_EXPIRED) { show("link expired"); }
```

### cr_status_name

```mettle
export fn cr_status_name(code: int32) -> string
```

Names a status code for logs.

- **in:** code, a CR_* status
- **returns:** a static string such as "CR_E_ARG" ("CR_E_UNKNOWN" otherwise)
- **owns:** nothing; the result points at a string literal, never free it
- **thread:** any thread (pure)

```mettle
println(cr_status_name(rc));
```

## core/types

Fixed-size value types. Sizes are part of the type, so a nonce cannot be passed where a key is expected and no length can be mixed up. Each type is one field, `b`, the raw bytes; address them as &v.b[0].

### CrKey32

```mettle
export struct CrKey32 {
  b: uint8[32];
}
```

A 32-byte symmetric secret key: XChaCha20-Poly1305 keys and the session keys cr_kx_* derive.

- **in:** - (zeroed when declared: `var x: CrKey32;`)
- **owns:** secret: wipe with cr_wipe(&k.b[0], 32) when done
- **thread:** any thread (no shared state)

```mettle
var k: CrKey32; cr_aead_keygen(&k);
```

### CrNonce24

```mettle
export struct CrNonce24 {
  b: uint8[24];
}
```

A 24-byte XChaCha20-Poly1305 nonce. Random nonces of this size are safe to generate per message.

- **in:** - (zeroed when declared: `var x: CrNonce24;`)
- **owns:** public: sent alongside the ciphertext
- **thread:** any thread (no shared state)

```mettle
var n: CrNonce24; cr_aead_nonce(&n);
```

### CrNonce12

```mettle
export struct CrNonce12 {
  b: uint8[12];
}
```

A 12-byte IETF ChaCha20-Poly1305 nonce (RFC 8439). Never reuse a key/nonce pair. Meant for protocols with counter nonces such as Noise; for random nonces use CrNonce24 and cr_aead_*.

- **in:** - (zeroed when declared: `var x: CrNonce12;`)
- **owns:** public: sent alongside the ciphertext, or implied by a message counter
- **thread:** any thread (no shared state)

```mettle
var n: CrNonce12; n.b[4] = 1; // Noise: 4 zero bytes || 64-bit LE counter
```

### CrSha256

```mettle
export struct CrSha256 {
  b: uint8[32];
}
```

A SHA-256 digest.

- **in:** - (zeroed when declared: `var x: CrSha256;`)
- **owns:** public
- **thread:** any thread (no shared state)

```mettle
var d: CrSha256; cr_sha256(&d, msg, len);
```

### CrSha384

```mettle
export struct CrSha384 {
  b: uint8[48];
}
```

A SHA-384 digest.

- **in:** - (zeroed when declared: `var x: CrSha384;`)
- **owns:** public
- **thread:** any thread (no shared state)

```mettle
var d: CrSha384; cr_sha384(&d, msg, len);
```

### CrSha512

```mettle
export struct CrSha512 {
  b: uint8[64];
}
```

A SHA-512 digest.

- **in:** - (zeroed when declared: `var x: CrSha512;`)
- **owns:** public
- **thread:** any thread (no shared state)

```mettle
var d: CrSha512; cr_sha512(&d, msg, len);
```

### CrHmacSha256

```mettle
export struct CrHmacSha256 {
  b: uint8[32];
}
```

An HMAC-SHA256 tag.

- **in:** - (zeroed when declared: `var x: CrHmacSha256;`)
- **owns:** public, but compare it only with cr_hmac_sha256_verify or cr_equal
- **thread:** any thread (no shared state)

```mettle
var t: CrHmacSha256; cr_hmac_sha256(&t, key, klen, msg, mlen);
```

### CrHmacSha384

```mettle
export struct CrHmacSha384 {
  b: uint8[48];
}
```

An HMAC-SHA384 tag.

- **in:** - (zeroed when declared: `var x: CrHmacSha384;`)
- **owns:** public, but compare it only with cr_hmac_sha384_verify or cr_equal
- **thread:** any thread (no shared state)

```mettle
var t: CrHmacSha384; cr_hmac_sha384(&t, key, klen, msg, mlen);
```

### CrHmacSha512

```mettle
export struct CrHmacSha512 {
  b: uint8[64];
}
```

An HMAC-SHA512 tag.

- **in:** - (zeroed when declared: `var x: CrHmacSha512;`)
- **owns:** public, but compare it only with cr_hmac_sha512_verify or cr_equal
- **thread:** any thread (no shared state)

```mettle
var t: CrHmacSha512; cr_hmac_sha512(&t, key, klen, msg, mlen);
```

### CrX25519Public

```mettle
export struct CrX25519Public {
  b: uint8[32];
}
```

An X25519 (Curve25519 Diffie-Hellman) public key; also the key type of cr_kx_*.

- **in:** - (zeroed when declared: `var x: CrX25519Public;`)
- **owns:** public
- **thread:** any thread (no shared state)

```mettle
var pk: CrX25519Public; var sk: CrX25519Secret; cr_x25519_keypair(&pk, &sk);
```

### CrX25519Secret

```mettle
export struct CrX25519Secret {
  b: uint8[32];
}
```

An X25519 secret scalar; also the key type of cr_kx_*.

- **in:** - (zeroed when declared: `var x: CrX25519Secret;`)
- **owns:** secret: wipe with cr_wipe(&sk.b[0], 32) when done
- **thread:** any thread (no shared state)

```mettle
var sk: CrX25519Secret; cr_x25519_keypair(&pk, &sk);
```

### CrX25519Shared

```mettle
export struct CrX25519Shared {
  b: uint8[32];
}
```

A raw X25519 shared point. Not a key: hash it (with both public keys) or use cr_kx_* session keys instead of using it directly.

- **in:** - (zeroed when declared: `var x: CrX25519Shared;`)
- **owns:** secret: wipe with cr_wipe(&s.b[0], 32) when done
- **thread:** any thread (no shared state)

```mettle
var s: CrX25519Shared; cr_x25519_shared(&s, &sk, &peer);
```

### CrEd25519Public

```mettle
export struct CrEd25519Public {
  b: uint8[32];
}
```

An Ed25519 public (verification) key.

- **in:** - (zeroed when declared: `var x: CrEd25519Public;`)
- **owns:** public
- **thread:** any thread (no shared state)

```mettle
var pk: CrEd25519Public; var sk: CrEd25519Secret; cr_ed25519_keypair(&pk, &sk);
```

### CrEd25519Secret

```mettle
export struct CrEd25519Secret {
  b: uint8[64];
}
```

An Ed25519 secret key in the 64-byte layout: the 32-byte seed followed by the 32-byte public key (RFC 8032).

- **in:** - (zeroed when declared: `var x: CrEd25519Secret;`)
- **owns:** secret: wipe with cr_wipe(&sk.b[0], 64) when done
- **thread:** any thread (no shared state)

```mettle
var sk: CrEd25519Secret; cr_ed25519_keypair(&pk, &sk);
```

### CrSeed32

```mettle
export struct CrSeed32 {
  b: uint8[32];
}
```

A 32-byte seed for deterministic key generation (cr_ed25519_seed_keypair). Equal to the RFC 8032 private key.

- **in:** - (zeroed when declared: `var x: CrSeed32;`)
- **owns:** secret: wipe with cr_wipe(&s.b[0], 32) when done
- **thread:** any thread (no shared state)

```mettle
var s: CrSeed32; cr_random_bytes(&s.b[0], 32);
```

### CrEd25519Sig

```mettle
export struct CrEd25519Sig {
  b: uint8[64];
}
```

A detached Ed25519 signature.

- **in:** - (zeroed when declared: `var x: CrEd25519Sig;`)
- **owns:** public
- **thread:** any thread (no shared state)

```mettle
var sig: CrEd25519Sig; cr_ed25519_sign(&sig, msg, len, &sk);
```

### CrPwHash

```mettle
export struct CrPwHash {
  b: uint8[128];
}
```

An Argon2id password hash in its self-describing, nul-terminated PHC string form ($argon2id$v=19$m=...,t=...,p=1$salt$hash). Store cr_pwhash_text(&h); the salt and costs travel inside it.

- **in:** - (zeroed when declared: `var x: CrPwHash;`)
- **owns:** not secret, but treat as sensitive (offline guessing)
- **thread:** any thread (no shared state)

```mettle
var h: CrPwHash; cr_pwhash_str(&h, pw.chars, (int64)pw.length, CR_PW_INTERACTIVE);
```

## core/util

Initialization (a no-op), randomness, constant-time comparison and wiping. Pure Mettle: randomness comes from the Linux getrandom(2) system call through the syscall built-in (Linux x86-64 and AArch64); there is no foreign code and no shared state, so every function in this library may be called from any thread.

### cr_init

```mettle
export fn cr_init() -> int32
```

Kept for compatibility: there is nothing to initialize. Safe to call any number of times, from any thread.

- **returns:** CR_OK, always
- **owns:** nothing
- **thread:** any thread (no shared state)

```mettle
if (cr_init() != CR_OK) { return 1; }
```

### cr_random_bytes

```mettle
export fn cr_random_bytes(buf: uint8*, n: int64) -> int32
```

Fills buf with n cryptographically secure random bytes (the kernel's getrandom(2)).

- **in:** buf, writable for n bytes (may be 0 only when n == 0); n >= 0
- **returns:** CR_OK; CR_E_ARG for a null buf with n > 0 or n < 0; CR_E_INTERNAL if the kernel random source fails
- **owns:** nothing; the caller owns buf
- **thread:** any thread (no shared state)

```mettle
var salt: uint8[16]; cr_random_bytes(&salt[0], 16);
```

### cr_random_u32_below

```mettle
export fn cr_random_u32_below(n: uint32, out: uint32*) -> int32
```

Picks a uniformly distributed random integer in [0, n) without modulo bias (rejection sampling).

- **in:** n >= 1, the exclusive upper bound; out, where the value goes
- **returns:** CR_OK; CR_E_ARG if n == 0 or out is null (out untouched); CR_E_INTERNAL if the kernel random source fails
- **owns:** nothing
- **thread:** any thread (no shared state)

```mettle
var die: uint32; cr_random_u32_below(6, &die);  // 0..5
```

### cr_equal

```mettle
export fn cr_equal(a: uint8*, b: uint8*, n: int64) -> bool
```

Compares two byte ranges in constant time: the time depends on n only, never on the contents. Use it for every comparison of secrets, tags and tokens; never use == or mem_equal for those.

- **in:** a, b readable for n bytes; n >= 0
- **returns:** true only if all n bytes are equal; false otherwise, and false for invalid arguments (null pointer with n > 0, n < 0)
- **owns:** nothing
- **thread:** any thread (no shared state)

```mettle
if (cr_equal(&got.b[0], &want.b[0], 32)) { accept(); }
```

### cr_wipe

```mettle
export fn cr_wipe(buf: uint8*, n: int64) -> int32
```

Overwrites n bytes with zeros in a way the optimizer cannot remove (volatile stores). Call it on every secret (keys, seeds, passwords, plaintexts, HMAC states) as soon as you are done with it, including on error paths: `defer cr_wipe(&k.b[0], 32);`.

- **in:** buf writable for n bytes; n >= 0
- **returns:** CR_OK; CR_E_ARG for a null buf with n > 0 or n < 0
- **owns:** nothing; the caller still owns (and frees) buf
- **thread:** any thread (no shared state)

```mettle
var k: CrKey32; cr_aead_keygen(&k); defer cr_wipe(&k.b[0], 32);
```

### cr_is_zero

```mettle
export fn cr_is_zero(buf: uint8*, n: int64) -> bool
```

Checks in constant time whether all n bytes are zero: the time depends on n only, never on the contents. Use it on secrets (for example to reject an all-zero shared secret) instead of a loop with an early exit.

- **in:** buf readable for n bytes; n >= 0
- **returns:** true only if all n bytes are zero (also for n == 0); false otherwise, and false for invalid arguments (null buf with n > 0, n < 0)
- **owns:** nothing
- **thread:** any thread (no shared state)

```mettle
if (cr_is_zero(&shared.b[0], 32)) { reject(); }
```

## core/encode

Encodings for tokens and keys: lowercase hex and base64url without padding (RFC 4648 section 5). Pure Mettle and constant time in the data: characters and values are converted with arithmetic masks only, with no table lookup and no branch on a byte's value, so encoding or decoding a secret does not leak it through timing. Branches depend on lengths only.

### cr_hex_encoded_cap

```mettle
export fn cr_hex_encoded_cap(n: int64) -> int64
```

Capacity cr_hex_encode needs for n input bytes: 2n + 1 (the nul).

- **in:** n >= 0
- **returns:** 2n + 1, or -1 for a negative or absurdly large n
- **owns:** nothing
- **thread:** any thread (pure)

```mettle
var cap: int64 = cr_hex_encoded_cap(32);  // 65
```

### cr_base64url_encoded_cap

```mettle
export fn cr_base64url_encoded_cap(n: int64) -> int64
```

Capacity cr_base64url_encode needs for n input bytes, nul included: (4n + 2) / 3 + 1.

- **in:** n >= 0
- **returns:** the capacity, or -1 for a negative or absurdly large n
- **owns:** nothing
- **thread:** any thread (pure)

```mettle
var cap: int64 = cr_base64url_encoded_cap(32);  // 44
```

### cr_hex_encode

```mettle
export fn cr_hex_encode(out: uint8*, out_cap: int64, out_len: int64*, bin: uint8*, n: int64) -> int32
```

Encodes n bytes as lowercase hex.

- **in:** out writable for out_cap >= cr_hex_encoded_cap(n) bytes; out_len; bin/n
- **returns:** CR_OK and *out_len = 2n (text followed by a nul); CR_E_ARG; CR_E_SPACE if out_cap is too small (nothing written). On error *out_len = 0
- **owns:** nothing; the caller owns out
- **thread:** any thread (no shared state)

```mettle
var hx: uint8[65]; var hl: int64; cr_hex_encode(&hx[0], 65, &hl, &d.b[0], 32);
```

### cr_hex_decode

```mettle
export fn cr_hex_decode(out: uint8*, out_cap: int64, out_len: int64*, text: uint8*, n: int64) -> int32
```

Decodes hex (either case) strictly.

- **in:** out writable for out_cap bytes; out_len; text/n, the hex digits (no nul needed)
- **returns:** CR_OK and *out_len = n / 2; CR_E_DECODE for an odd length or any non-hex character (including spaces and a trailing nul); CR_E_SPACE if out_cap < n / 2; CR_E_ARG. On error *out_len = 0 and out's first out_cap bytes are wiped
- **owns:** nothing; the caller owns out
- **thread:** any thread (no shared state)

```mettle
var k: uint8[32]; var kl: int64; cr_hex_decode(&k[0], 32, &kl, s.chars, (int64)s.length);
```

### cr_base64url_encode

```mettle
export fn cr_base64url_encode(out: uint8*, out_cap: int64, out_len: int64*, bin: uint8*, n: int64) -> int32
```

Encodes n bytes as base64url without padding (alphabet A-Z a-z 0-9 - _), the form used in URLs, cookies and tokens.

- **in:** out writable for out_cap >= cr_base64url_encoded_cap(n) bytes; out_len; bin/n
- **returns:** CR_OK and *out_len = the text length (a nul follows it); CR_E_ARG; CR_E_SPACE if out_cap is too small (nothing written). On error *out_len = 0
- **owns:** nothing; the caller owns out
- **thread:** any thread (no shared state)

```mettle
var t: uint8[44]; var tl: int64; cr_base64url_encode(&t[0], 44, &tl, &raw[0], 32);
```

### cr_base64url_decode

```mettle
export fn cr_base64url_decode(out: uint8*, out_cap: int64, out_len: int64*, text: uint8*, n: int64) -> int32
```

Decodes unpadded base64url strictly.

- **in:** out writable for out_cap bytes; out_len; text/n (no nul needed)
- **returns:** CR_OK and *out_len = the decoded length; CR_E_DECODE for any character outside the base64url alphabet (including '=', '+', '/', spaces), a length of the form 4k+1, or non-zero trailing bits; CR_E_SPACE if out_cap < floor(3n / 4); CR_E_ARG. On error *out_len = 0 and out's first out_cap bytes are wiped
- **owns:** nothing; the caller owns out
- **thread:** any thread (no shared state)

```mettle
var raw: uint8[32]; var rl: int64; cr_base64url_decode(&raw[0], 32, &rl, t.chars, (int64)t.length);
```

## hash/hash

Hashes: SHA-256, SHA-384, SHA-512 (FIPS 180-4, sha2) and BLAKE2b (RFC 7693, blake2b), all pure Mettle.

### CR_BLAKE2B_MIN

```mettle
export const CR_BLAKE2B_MIN: int64 = 16;
```

Smallest BLAKE2b output length cr_blake2b accepts (a hash shorter than this is too weak to be useful).

- **returns:** 16
- **owns:** nothing
- **thread:** constant

```mettle
cr_blake2b(&o[0], CR_BLAKE2B_MIN, m, n, 0, 0);
```

### CR_BLAKE2B_MAX

```mettle
export const CR_BLAKE2B_MAX: int64 = 64;
```

Largest BLAKE2b output and key length (the RFC 7693 maximum).

- **returns:** 64
- **owns:** nothing
- **thread:** constant

```mettle
var o: uint8[64]; cr_blake2b(&o[0], CR_BLAKE2B_MAX, m, n, 0, 0);
```

### CR_BLAKE2B_KEY_MIN

```mettle
export const CR_BLAKE2B_KEY_MIN: int64 = 16;
```

Smallest non-empty BLAKE2b key (shorter keys are too weak to be useful).

- **returns:** 16
- **owns:** nothing
- **thread:** constant

```mettle
cr_blake2b(&o[0], 32, m, n, &k[0], CR_BLAKE2B_KEY_MIN);
```

### cr_sha256

```mettle
export fn cr_sha256(out: CrSha256*, msg: uint8*, n: int64) -> int32
```

Computes SHA-256 of n bytes (FIPS 180-4).

- **in:** out; msg readable for n bytes (may be 0 when n == 0); n >= 0
- **returns:** CR_OK; CR_E_ARG (null out, null msg with n > 0, n < 0). On error out is not written
- **owns:** nothing; the caller owns out and msg
- **thread:** any thread (no shared state)

```mettle
var d: CrSha256; cr_sha256(&d, s.chars, (int64)s.length);
```

### cr_sha384

```mettle
export fn cr_sha384(out: CrSha384*, msg: uint8*, n: int64) -> int32
```

Computes SHA-384 of n bytes (FIPS 180-4).

- **in:** out; msg readable for n bytes (may be 0 when n == 0); n >= 0
- **returns:** CR_OK; CR_E_ARG (null out, null msg with n > 0, n < 0). On error out is not written
- **owns:** nothing; the caller owns out and msg
- **thread:** any thread (no shared state)

```mettle
var d: CrSha384; cr_sha384(&d, s.chars, (int64)s.length);
```

### cr_sha512

```mettle
export fn cr_sha512(out: CrSha512*, msg: uint8*, n: int64) -> int32
```

Computes SHA-512 of n bytes (FIPS 180-4).

- **in:** out; msg readable for n bytes (may be 0 when n == 0); n >= 0
- **returns:** CR_OK; CR_E_ARG (null out, null msg with n > 0, n < 0). On error out is not written
- **owns:** nothing; the caller owns out and msg
- **thread:** any thread (no shared state)

```mettle
var d: CrSha512; cr_sha512(&d, s.chars, (int64)s.length);
```

### cr_blake2b

```mettle
export fn cr_blake2b(out: uint8*, outlen: int64, msg: uint8*, n: int64, key: uint8*, keylen: int64) -> int32
```

Computes BLAKE2b (RFC 7693), optionally keyed (then it is a MAC/PRF), with a chosen output length.

- **in:** out writable for outlen bytes, outlen in [16, 64]; msg readable for n bytes (0 allowed when n == 0), n >= 0; key readable for keylen bytes, keylen 0 (unkeyed; key may be 0) or in [16, 64]
- **returns:** CR_OK; CR_E_ARG for any length outside those ranges or a null pointer with a non-zero length. On error out is not written
- **owns:** nothing; the caller owns out, msg and key (wipe the key yourself)
- **thread:** any thread (no shared state)

```mettle
var d: uint8[32]; cr_blake2b(&d[0], 32, s.chars, (int64)s.length, 0, 0);
```

### CrSha256State

```mettle
export struct CrSha256State {
  s: CrxSha256;
}
```

Streaming SHA-256 state. Treat as opaque: only cr_sha256_init, _update and _final touch it. The caller provides the memory.

- **owns:** the caller owns the state; _final wipes it; to abandon a hash early wipe it with cr_wipe((uint8*)st, (int64)sizeof(CrSha256State))
- **thread:** any thread (no shared state); one thread per state

```mettle
var st: CrSha256State; cr_sha256_init(&st);
```

### CrSha384State

```mettle
export struct CrSha384State {
  s: CrxSha384;
}
```

Streaming SHA-384 state. Treat as opaque: only cr_sha384_init, _update and _final touch it. The caller provides the memory.

- **owns:** the caller owns the state; _final wipes it; to abandon a hash early wipe it with cr_wipe((uint8*)st, (int64)sizeof(CrSha384State))
- **thread:** any thread (no shared state); one thread per state

```mettle
var st: CrSha384State; cr_sha384_init(&st);
```

### CrSha512State

```mettle
export struct CrSha512State {
  s: CrxSha512;
}
```

Streaming SHA-512 state. Treat as opaque: only cr_sha512_init, _update and _final touch it. The caller provides the memory.

- **owns:** the caller owns the state; _final wipes it; to abandon a hash early wipe it with cr_wipe((uint8*)st, (int64)sizeof(CrSha512State))
- **thread:** any thread (no shared state); one thread per state

```mettle
var st: CrSha512State; cr_sha512_init(&st);
```

### CrBlake2bState

```mettle
export struct CrBlake2bState {
  s: CrxBlake2b;
}
```

Streaming BLAKE2b state. Treat as opaque: only cr_blake2b_init, _update and _final touch it. The caller provides the memory.

- **owns:** the caller owns the state; _final wipes it; to abandon a hash early wipe it with cr_wipe((uint8*)st, (int64)sizeof(CrBlake2bState)). A keyed state holds the key: wipe or finish it
- **thread:** any thread (no shared state); one thread per state

```mettle
var st: CrBlake2bState; cr_blake2b_init(&st, 32, 0, 0);
```

### cr_sha256_init

```mettle
export fn cr_sha256_init(st: CrSha256State*) -> int32
```

Starts a streaming SHA-256 hash (FIPS 180-4). Feed data with cr_sha256_update, finish with cr_sha256_final.

- **in:** st writable state
- **returns:** CR_OK; CR_E_ARG (null st)
- **owns:** the caller owns st; _final wipes it, or abandon it with cr_wipe((uint8*)st, (int64)sizeof(CrSha256State))
- **thread:** any thread (no shared state)

```mettle
var st: CrSha256State; cr_sha256_init(&st);
```

### cr_sha256_update

```mettle
export fn cr_sha256_update(st: CrSha256State*, msg: uint8*, n: int64) -> int32
```

Feeds n bytes into a streaming SHA-256 hash. Any split of the input gives the same digest as cr_sha256 over the whole.

- **in:** st initialised by cr_sha256_init; msg readable for n bytes (may be 0 when n == 0); n >= 0
- **returns:** CR_OK; CR_E_ARG (null st, null msg with n > 0, n < 0). On error the state is unchanged
- **owns:** the caller owns st and msg
- **thread:** any thread (no shared state)

```mettle
cr_sha256_update(&st, buf, (int64)got);
```

### cr_sha256_final

```mettle
export fn cr_sha256_final(st: CrSha256State*, out: CrSha256*) -> int32
```

Finishes a streaming SHA-256 hash and writes the digest. Always wipes st, also on error.

- **in:** st initialised by cr_sha256_init; out writable
- **returns:** CR_OK; CR_E_ARG (null st or out). With a null out and a non-null st the state is wiped and nothing is written
- **owns:** the caller owns st and out; st is wiped
- **thread:** any thread (no shared state)

```mettle
var d: CrSha256; cr_sha256_final(&st, &d);
```

### cr_sha384_init

```mettle
export fn cr_sha384_init(st: CrSha384State*) -> int32
```

Starts a streaming SHA-384 hash (FIPS 180-4). Feed data with cr_sha384_update, finish with cr_sha384_final.

- **in:** st writable state
- **returns:** CR_OK; CR_E_ARG (null st)
- **owns:** the caller owns st; _final wipes it, or abandon it with cr_wipe((uint8*)st, (int64)sizeof(CrSha384State))
- **thread:** any thread (no shared state)

```mettle
var st: CrSha384State; cr_sha384_init(&st);
```

### cr_sha384_update

```mettle
export fn cr_sha384_update(st: CrSha384State*, msg: uint8*, n: int64) -> int32
```

Feeds n bytes into a streaming SHA-384 hash. Any split of the input gives the same digest as cr_sha384 over the whole.

- **in:** st initialised by cr_sha384_init; msg readable for n bytes (may be 0 when n == 0); n >= 0
- **returns:** CR_OK; CR_E_ARG (null st, null msg with n > 0, n < 0). On error the state is unchanged
- **owns:** the caller owns st and msg
- **thread:** any thread (no shared state)

```mettle
cr_sha384_update(&st, buf, (int64)got);
```

### cr_sha384_final

```mettle
export fn cr_sha384_final(st: CrSha384State*, out: CrSha384*) -> int32
```

Finishes a streaming SHA-384 hash and writes the digest. Always wipes st, also on error.

- **in:** st initialised by cr_sha384_init; out writable
- **returns:** CR_OK; CR_E_ARG (null st or out). With a null out and a non-null st the state is wiped and nothing is written
- **owns:** the caller owns st and out; st is wiped
- **thread:** any thread (no shared state)

```mettle
var d: CrSha384; cr_sha384_final(&st, &d);
```

### cr_sha512_init

```mettle
export fn cr_sha512_init(st: CrSha512State*) -> int32
```

Starts a streaming SHA-512 hash (FIPS 180-4). Feed data with cr_sha512_update, finish with cr_sha512_final.

- **in:** st writable state
- **returns:** CR_OK; CR_E_ARG (null st)
- **owns:** the caller owns st; _final wipes it, or abandon it with cr_wipe((uint8*)st, (int64)sizeof(CrSha512State))
- **thread:** any thread (no shared state)

```mettle
var st: CrSha512State; cr_sha512_init(&st);
```

### cr_sha512_update

```mettle
export fn cr_sha512_update(st: CrSha512State*, msg: uint8*, n: int64) -> int32
```

Feeds n bytes into a streaming SHA-512 hash. Any split of the input gives the same digest as cr_sha512 over the whole.

- **in:** st initialised by cr_sha512_init; msg readable for n bytes (may be 0 when n == 0); n >= 0
- **returns:** CR_OK; CR_E_ARG (null st, null msg with n > 0, n < 0). On error the state is unchanged
- **owns:** the caller owns st and msg
- **thread:** any thread (no shared state)

```mettle
cr_sha512_update(&st, buf, (int64)got);
```

### cr_sha512_final

```mettle
export fn cr_sha512_final(st: CrSha512State*, out: CrSha512*) -> int32
```

Finishes a streaming SHA-512 hash and writes the digest. Always wipes st, also on error.

- **in:** st initialised by cr_sha512_init; out writable
- **returns:** CR_OK; CR_E_ARG (null st or out). With a null out and a non-null st the state is wiped and nothing is written
- **owns:** the caller owns st and out; st is wiped
- **thread:** any thread (no shared state)

```mettle
var d: CrSha512; cr_sha512_final(&st, &d);
```

### cr_blake2b_init

```mettle
export fn cr_blake2b_init(st: CrBlake2bState*, outlen: int64, key: uint8*, keylen: int64) -> int32
```

Starts a streaming BLAKE2b hash (RFC 7693), optionally keyed, with a chosen output length. Same length rules as cr_blake2b.

- **in:** st writable state; outlen in [16, 64]; key readable for keylen bytes, keylen 0 (unkeyed; key may be 0) or in [16, 64]
- **returns:** CR_OK; CR_E_ARG (null st, outlen or keylen out of range, null key with keylen > 0). On error st is wiped so a later _final fails too
- **owns:** the caller owns st and key (wipe the key yourself); st holds a copy of the key until _final wipes it, or abandon it with cr_wipe((uint8*)st, (int64)sizeof(CrBlake2bState))
- **thread:** any thread (no shared state)

```mettle
var st: CrBlake2bState; cr_blake2b_init(&st, 32, 0, 0);
```

### cr_blake2b_update

```mettle
export fn cr_blake2b_update(st: CrBlake2bState*, msg: uint8*, n: int64) -> int32
```

Feeds n bytes into a streaming BLAKE2b hash. Any split of the input gives the same digest as cr_blake2b over the whole.

- **in:** st initialised by cr_blake2b_init; msg readable for n bytes (may be 0 when n == 0); n >= 0
- **returns:** CR_OK; CR_E_ARG (null st, null msg with n > 0, n < 0). On error the state is unchanged
- **owns:** the caller owns st and msg
- **thread:** any thread (no shared state)

```mettle
cr_blake2b_update(&st, buf, (int64)got);
```

### cr_blake2b_final

```mettle
export fn cr_blake2b_final(st: CrBlake2bState*, out: uint8*, outlen: int64) -> int32
```

Finishes a streaming BLAKE2b hash and writes the digest. Always wipes st, also on error.

- **in:** st initialised by cr_blake2b_init; out writable for outlen bytes; outlen equal to the outlen given to cr_blake2b_init
- **returns:** CR_OK; CR_E_ARG (null st or out, outlen different from the init outlen). On error st is wiped and out is not written
- **owns:** the caller owns st and out; st is wiped
- **thread:** any thread (no shared state)

```mettle
var d: uint8[32]; cr_blake2b_final(&st, &d[0], 32);
```

### CR_HASH_SHA256

```mettle
export const CR_HASH_SHA256: int32 = 1;
```

names SHA-256 for functions that take a hash choice (ECDSA, RSA).

- **returns:** 1
- **owns:** nothing
- **thread:** constant

```mettle
cr_p256_ecdsa_verify(&sig[0], sl, m, n, CR_HASH_SHA256, &pk[0], 65)
```

### CR_HASH_SHA384

```mettle
export const CR_HASH_SHA384: int32 = 2;
```

names SHA-384 for functions that take a hash choice (ECDSA, RSA).

- **returns:** 2
- **owns:** nothing
- **thread:** constant

```mettle
cr_rsa_verify_pss(&sig[0], sl, m, n, CR_HASH_SHA384, mod, ml, e, el)
```

### CR_HASH_SHA512

```mettle
export const CR_HASH_SHA512: int32 = 3;
```

names SHA-512 for functions that take a hash choice (ECDSA, RSA).

- **returns:** 3
- **owns:** nothing
- **thread:** constant

```mettle
cr_rsa_verify_pkcs1(&sig[0], sl, m, n, CR_HASH_SHA512, mod, ml, e, el)
```

## hash/legacy

Legacy hashes: MD5 (RFC 1321) and SHA-1 (FIPS 180-4), pure Mettle.

### CrMd5

```mettle
export struct CrMd5 {
  b: uint8[16];
}
```

An MD5 digest (16 bytes). Legacy: not for security. Use only where a standard requires it, such as RFC 9562 name-based UUIDs. For anything security-related use SHA-256 or BLAKE2b.

- **owns:** the caller owns it
- **thread:** any thread (no shared state)

```mettle
var d: CrMd5; cr_md5(&d, s.chars, (int64)s.length);
```

### CrMd5State

```mettle
export struct CrMd5State {
  s: CrxMd5;
}
```

Streaming MD5 state. Treat as opaque: only cr_md5_init, _update and _final touch it. The caller provides the memory. Legacy: not for security. Use only where a standard requires it, such as RFC 9562 name-based UUIDs. For anything security-related use SHA-256 or BLAKE2b.

- **owns:** the caller owns the state; _final wipes it; to abandon a hash early wipe it with cr_wipe((uint8*)st, (int64)sizeof(CrMd5State))
- **thread:** any thread (no shared state); one thread per state

```mettle
var st: CrMd5State; cr_md5_init(&st);
```

### cr_md5

```mettle
export fn cr_md5(out: CrMd5*, msg: uint8*, n: int64) -> int32
```

Computes MD5 of n bytes (RFC 1321). Legacy: not for security. Use only where a standard requires it, such as RFC 9562 name-based UUIDs. For anything security-related use SHA-256 or BLAKE2b.

- **in:** out; msg readable for n bytes (may be 0 when n == 0); n >= 0
- **returns:** CR_OK; CR_E_ARG (null out, null msg with n > 0, n < 0). On error out is not written
- **owns:** nothing; the caller owns out and msg
- **thread:** any thread (no shared state)

```mettle
var d: CrMd5; cr_md5(&d, s.chars, (int64)s.length);
```

### cr_md5_init

```mettle
export fn cr_md5_init(st: CrMd5State*) -> int32
```

Starts a streaming MD5 hash (RFC 1321). Feed data with cr_md5_update, finish with cr_md5_final. Legacy: not for security. Use only where a standard requires it, such as RFC 9562 name-based UUIDs. For anything security-related use SHA-256 or BLAKE2b.

- **in:** st writable state
- **returns:** CR_OK; CR_E_ARG (null st)
- **owns:** the caller owns st; _final wipes it, or abandon it with cr_wipe((uint8*)st, (int64)sizeof(CrMd5State))
- **thread:** any thread (no shared state)

```mettle
var st: CrMd5State; cr_md5_init(&st);
```

### cr_md5_update

```mettle
export fn cr_md5_update(st: CrMd5State*, msg: uint8*, n: int64) -> int32
```

Feeds n bytes into a streaming MD5 hash. Any split of the input gives the same digest as cr_md5 over the whole. Legacy: not for security. Use only where a standard requires it, such as RFC 9562 name-based UUIDs. For anything security-related use SHA-256 or BLAKE2b.

- **in:** st initialised by cr_md5_init; msg readable for n bytes (may be 0 when n == 0); n >= 0
- **returns:** CR_OK; CR_E_ARG (null st, null msg with n > 0, n < 0). On error the state is unchanged
- **owns:** the caller owns st and msg
- **thread:** any thread (no shared state)

```mettle
cr_md5_update(&st, buf, (int64)got);
```

### cr_md5_final

```mettle
export fn cr_md5_final(st: CrMd5State*, out: CrMd5*) -> int32
```

Finishes a streaming MD5 hash and writes the digest. Always wipes st, also on error. Legacy: not for security. Use only where a standard requires it, such as RFC 9562 name-based UUIDs. For anything security-related use SHA-256 or BLAKE2b.

- **in:** st initialised by cr_md5_init; out writable
- **returns:** CR_OK; CR_E_ARG (null st or out). With a null out and a non-null st the state is wiped and nothing is written
- **owns:** the caller owns st and out; st is wiped
- **thread:** any thread (no shared state)

```mettle
var d: CrMd5; cr_md5_final(&st, &d);
```

### CrSha1

```mettle
export struct CrSha1 {
  b: uint8[20];
}
```

An SHA-1 digest (20 bytes). Legacy: not for security. Use only where a standard requires it, such as RFC 9562 name-based UUIDs. For anything security-related use SHA-256 or BLAKE2b.

- **owns:** the caller owns it
- **thread:** any thread (no shared state)

```mettle
var d: CrSha1; cr_sha1(&d, s.chars, (int64)s.length);
```

### CrSha1State

```mettle
export struct CrSha1State {
  s: CrxSha1;
}
```

Streaming SHA-1 state. Treat as opaque: only cr_sha1_init, _update and _final touch it. The caller provides the memory. Legacy: not for security. Use only where a standard requires it, such as RFC 9562 name-based UUIDs. For anything security-related use SHA-256 or BLAKE2b.

- **owns:** the caller owns the state; _final wipes it; to abandon a hash early wipe it with cr_wipe((uint8*)st, (int64)sizeof(CrSha1State))
- **thread:** any thread (no shared state); one thread per state

```mettle
var st: CrSha1State; cr_sha1_init(&st);
```

### cr_sha1

```mettle
export fn cr_sha1(out: CrSha1*, msg: uint8*, n: int64) -> int32
```

Computes SHA-1 of n bytes (FIPS 180-4). Legacy: not for security. Use only where a standard requires it, such as RFC 9562 name-based UUIDs. For anything security-related use SHA-256 or BLAKE2b.

- **in:** out; msg readable for n bytes (may be 0 when n == 0); n >= 0
- **returns:** CR_OK; CR_E_ARG (null out, null msg with n > 0, n < 0). On error out is not written
- **owns:** nothing; the caller owns out and msg
- **thread:** any thread (no shared state)

```mettle
var d: CrSha1; cr_sha1(&d, s.chars, (int64)s.length);
```

### cr_sha1_init

```mettle
export fn cr_sha1_init(st: CrSha1State*) -> int32
```

Starts a streaming SHA-1 hash (FIPS 180-4). Feed data with cr_sha1_update, finish with cr_sha1_final. Legacy: not for security. Use only where a standard requires it, such as RFC 9562 name-based UUIDs. For anything security-related use SHA-256 or BLAKE2b.

- **in:** st writable state
- **returns:** CR_OK; CR_E_ARG (null st)
- **owns:** the caller owns st; _final wipes it, or abandon it with cr_wipe((uint8*)st, (int64)sizeof(CrSha1State))
- **thread:** any thread (no shared state)

```mettle
var st: CrSha1State; cr_sha1_init(&st);
```

### cr_sha1_update

```mettle
export fn cr_sha1_update(st: CrSha1State*, msg: uint8*, n: int64) -> int32
```

Feeds n bytes into a streaming SHA-1 hash. Any split of the input gives the same digest as cr_sha1 over the whole. Legacy: not for security. Use only where a standard requires it, such as RFC 9562 name-based UUIDs. For anything security-related use SHA-256 or BLAKE2b.

- **in:** st initialised by cr_sha1_init; msg readable for n bytes (may be 0 when n == 0); n >= 0
- **returns:** CR_OK; CR_E_ARG (null st, null msg with n > 0, n < 0). On error the state is unchanged
- **owns:** the caller owns st and msg
- **thread:** any thread (no shared state)

```mettle
cr_sha1_update(&st, buf, (int64)got);
```

### cr_sha1_final

```mettle
export fn cr_sha1_final(st: CrSha1State*, out: CrSha1*) -> int32
```

Finishes a streaming SHA-1 hash and writes the digest. Always wipes st, also on error. Legacy: not for security. Use only where a standard requires it, such as RFC 9562 name-based UUIDs. For anything security-related use SHA-256 or BLAKE2b.

- **in:** st initialised by cr_sha1_init; out writable
- **returns:** CR_OK; CR_E_ARG (null st or out). With a null out and a non-null st the state is wiped and nothing is written
- **owns:** the caller owns st and out; st is wiped
- **thread:** any thread (no shared state)

```mettle
var d: CrSha1; cr_sha1_final(&st, &d);
```

## mac/mac

HMAC-SHA256, HMAC-SHA384 and HMAC-SHA512 (RFC 2104, FIPS 198-1) with keys of any length, in pure Mettle on top of sha2. The state keeps two running hashes (inner, outer) that were primed with the key pads, so no key copy stays around. Verification is constant time (cr_equal).

### CrHmacSha256State

```mettle
export struct CrHmacSha256State {
  st: uint64[32];
}
```

an incremental HMAC-SHA256 computation. Use one state per computation (held in caller memory; 256 bytes reserved, the inner and outer hash contexts use 224).

- **in:** - (initialize with cr_hmac_sha256_init before use)
- **owns:** secret (it holds key-derived hash contexts): cr_hmac_sha256_final wipes it; if you abandon a computation, call cr_wipe(cr_hmac_sha256_state_bytes(&st), 256)
- **thread:** any thread (no shared state)

```mettle
var st: CrHmacSha256State; cr_hmac_sha256_init(&st, k, klen);
```

### cr_hmac_sha256_state_bytes

```mettle
export fn cr_hmac_sha256_state_bytes(st: CrHmacSha256State*) -> uint8*
```

Gives the address of a state's storage, for cr_wipe.

- **in:** st
- **returns:** a pointer to its 256 bytes (0 if st is 0)
- **owns:** the pointer borrows st; it is caller memory
- **thread:** any thread (no shared state)

```mettle
cr_wipe(cr_hmac_sha256_state_bytes(&st), 256);
```

### cr_hmac_sha256_init

```mettle
export fn cr_hmac_sha256_init(st: CrHmacSha256State*, key: uint8*, keylen: int64) -> int32
```

Starts an HMAC-SHA256 computation (RFC 2104 / FIPS 198-1) with a key of any length (longer than the block size it is hashed, as RFC 2104 says).

- **in:** st; key readable for keylen bytes, keylen >= 0 (32+ random bytes recommended; key may be 0 only when keylen == 0)
- **returns:** CR_OK; CR_E_ARG (null st, null key with keylen > 0, keylen < 0)
- **owns:** st stays the caller's; the key is absorbed into st's hash contexts, so the caller may wipe its key right away
- **thread:** any thread (no shared state)

```mettle
var st: CrHmacSha256State; cr_hmac_sha256_init(&st, &k[0], 32);
```

### cr_hmac_sha256_update

```mettle
export fn cr_hmac_sha256_update(st: CrHmacSha256State*, msg: uint8*, n: int64) -> int32
```

Feeds n more message bytes into an HMAC-SHA256 computation.

- **in:** st from cr_hmac_sha256_init; msg readable for n bytes; n >= 0
- **returns:** CR_OK; CR_E_ARG (null st, null msg with n > 0, n < 0)
- **owns:** nothing new
- **thread:** any thread (no shared state)

```mettle
cr_hmac_sha256_update(&st, part.chars, (int64)part.length);
```

### cr_hmac_sha256_final

```mettle
export fn cr_hmac_sha256_final(st: CrHmacSha256State*, out: CrHmacSha256*) -> int32
```

Finishes an HMAC-SHA256 computation, writes the tag and wipes the state.

- **in:** st from cr_hmac_sha256_init; out
- **returns:** CR_OK; CR_E_ARG (null st or out)
- **owns:** st is wiped (start again with cr_hmac_sha256_init to reuse it)
- **thread:** any thread (no shared state)

```mettle
var t: CrHmacSha256; cr_hmac_sha256_final(&st, &t);
```

### cr_hmac_sha256

```mettle
export fn cr_hmac_sha256(out: CrHmacSha256*, key: uint8*, keylen: int64, msg: uint8*, n: int64) -> int32
```

Computes HMAC-SHA256(key, msg) in one call (init + update + final), for a key of any length.

- **in:** out; key/keylen as cr_hmac_sha256_init; msg/n as cr_hmac_sha256_update
- **returns:** CR_OK; CR_E_ARG. On error out is left zeroed
- **owns:** nothing; the temporary state is wiped
- **thread:** any thread (no shared state)

```mettle
var t: CrHmacSha256; cr_hmac_sha256(&t, &k[0], 32, m.chars, (int64)m.length);
```

### cr_hmac_sha256_verify

```mettle
export fn cr_hmac_sha256_verify(tag: CrHmacSha256*, key: uint8*, keylen: int64, msg: uint8*, n: int64) -> bool
```

Checks an HMAC-SHA256 tag in constant time: recomputes the tag and compares with cr_equal.

- **in:** tag, the received tag; key/keylen and msg/n as cr_hmac_sha256
- **returns:** true only if the tag is valid; false for a wrong tag and for every error (fails closed)
- **owns:** nothing; the recomputed tag is wiped
- **thread:** any thread (no shared state)

```mettle
if (!cr_hmac_sha256_verify(&t, &k[0], 32, m.chars, (int64)m.length)) { reject(); }
```

### CrHmacSha384State

```mettle
export struct CrHmacSha384State {
  st: uint64[64];
}
```

an incremental HMAC-SHA384 computation. Use one state per computation (held in caller memory; 512 bytes reserved, the inner and outer hash contexts use 416).

- **in:** - (initialize with cr_hmac_sha384_init before use)
- **owns:** secret (it holds key-derived hash contexts): cr_hmac_sha384_final wipes it; if you abandon a computation, call cr_wipe(cr_hmac_sha384_state_bytes(&st), 512)
- **thread:** any thread (no shared state)

```mettle
var st: CrHmacSha384State; cr_hmac_sha384_init(&st, k, klen);
```

### cr_hmac_sha384_state_bytes

```mettle
export fn cr_hmac_sha384_state_bytes(st: CrHmacSha384State*) -> uint8*
```

Gives the address of a state's storage, for cr_wipe.

- **in:** st
- **returns:** a pointer to its 512 bytes (0 if st is 0)
- **owns:** the pointer borrows st; it is caller memory
- **thread:** any thread (no shared state)

```mettle
cr_wipe(cr_hmac_sha384_state_bytes(&st), 512);
```

### cr_hmac_sha384_init

```mettle
export fn cr_hmac_sha384_init(st: CrHmacSha384State*, key: uint8*, keylen: int64) -> int32
```

Starts an HMAC-SHA384 computation (RFC 2104 / FIPS 198-1) with a key of any length (longer than the block size it is hashed, as RFC 2104 says).

- **in:** st; key readable for keylen bytes, keylen >= 0 (32+ random bytes recommended; key may be 0 only when keylen == 0)
- **returns:** CR_OK; CR_E_ARG (null st, null key with keylen > 0, keylen < 0)
- **owns:** st stays the caller's; the key is absorbed into st's hash contexts, so the caller may wipe its key right away
- **thread:** any thread (no shared state)

```mettle
var st: CrHmacSha384State; cr_hmac_sha384_init(&st, &k[0], 32);
```

### cr_hmac_sha384_update

```mettle
export fn cr_hmac_sha384_update(st: CrHmacSha384State*, msg: uint8*, n: int64) -> int32
```

Feeds n more message bytes into an HMAC-SHA384 computation.

- **in:** st from cr_hmac_sha384_init; msg readable for n bytes; n >= 0
- **returns:** CR_OK; CR_E_ARG (null st, null msg with n > 0, n < 0)
- **owns:** nothing new
- **thread:** any thread (no shared state)

```mettle
cr_hmac_sha384_update(&st, part.chars, (int64)part.length);
```

### cr_hmac_sha384_final

```mettle
export fn cr_hmac_sha384_final(st: CrHmacSha384State*, out: CrHmacSha384*) -> int32
```

Finishes an HMAC-SHA384 computation, writes the tag and wipes the state.

- **in:** st from cr_hmac_sha384_init; out
- **returns:** CR_OK; CR_E_ARG (null st or out)
- **owns:** st is wiped (start again with cr_hmac_sha384_init to reuse it)
- **thread:** any thread (no shared state)

```mettle
var t: CrHmacSha384; cr_hmac_sha384_final(&st, &t);
```

### cr_hmac_sha384

```mettle
export fn cr_hmac_sha384(out: CrHmacSha384*, key: uint8*, keylen: int64, msg: uint8*, n: int64) -> int32
```

Computes HMAC-SHA384(key, msg) in one call (init + update + final), for a key of any length.

- **in:** out; key/keylen as cr_hmac_sha384_init; msg/n as cr_hmac_sha384_update
- **returns:** CR_OK; CR_E_ARG. On error out is left zeroed
- **owns:** nothing; the temporary state is wiped
- **thread:** any thread (no shared state)

```mettle
var t: CrHmacSha384; cr_hmac_sha384(&t, &k[0], 32, m.chars, (int64)m.length);
```

### cr_hmac_sha384_verify

```mettle
export fn cr_hmac_sha384_verify(tag: CrHmacSha384*, key: uint8*, keylen: int64, msg: uint8*, n: int64) -> bool
```

Checks an HMAC-SHA384 tag in constant time: recomputes the tag and compares with cr_equal.

- **in:** tag, the received tag; key/keylen and msg/n as cr_hmac_sha384
- **returns:** true only if the tag is valid; false for a wrong tag and for every error (fails closed)
- **owns:** nothing; the recomputed tag is wiped
- **thread:** any thread (no shared state)

```mettle
if (!cr_hmac_sha384_verify(&t, &k[0], 32, m.chars, (int64)m.length)) { reject(); }
```

### CrHmacSha512State

```mettle
export struct CrHmacSha512State {
  st: uint64[64];
}
```

an incremental HMAC-SHA512 computation. Use one state per computation (held in caller memory; 512 bytes reserved, the inner and outer hash contexts use 416).

- **in:** - (initialize with cr_hmac_sha512_init before use)
- **owns:** secret (it holds key-derived hash contexts): cr_hmac_sha512_final wipes it; if you abandon a computation, call cr_wipe(cr_hmac_sha512_state_bytes(&st), 512)
- **thread:** any thread (no shared state)

```mettle
var st: CrHmacSha512State; cr_hmac_sha512_init(&st, k, klen);
```

### cr_hmac_sha512_state_bytes

```mettle
export fn cr_hmac_sha512_state_bytes(st: CrHmacSha512State*) -> uint8*
```

Gives the address of a state's storage, for cr_wipe.

- **in:** st
- **returns:** a pointer to its 512 bytes (0 if st is 0)
- **owns:** the pointer borrows st; it is caller memory
- **thread:** any thread (no shared state)

```mettle
cr_wipe(cr_hmac_sha512_state_bytes(&st), 512);
```

### cr_hmac_sha512_init

```mettle
export fn cr_hmac_sha512_init(st: CrHmacSha512State*, key: uint8*, keylen: int64) -> int32
```

Starts an HMAC-SHA512 computation (RFC 2104 / FIPS 198-1) with a key of any length (longer than the block size it is hashed, as RFC 2104 says).

- **in:** st; key readable for keylen bytes, keylen >= 0 (32+ random bytes recommended; key may be 0 only when keylen == 0)
- **returns:** CR_OK; CR_E_ARG (null st, null key with keylen > 0, keylen < 0)
- **owns:** st stays the caller's; the key is absorbed into st's hash contexts, so the caller may wipe its key right away
- **thread:** any thread (no shared state)

```mettle
var st: CrHmacSha512State; cr_hmac_sha512_init(&st, &k[0], 32);
```

### cr_hmac_sha512_update

```mettle
export fn cr_hmac_sha512_update(st: CrHmacSha512State*, msg: uint8*, n: int64) -> int32
```

Feeds n more message bytes into an HMAC-SHA512 computation.

- **in:** st from cr_hmac_sha512_init; msg readable for n bytes; n >= 0
- **returns:** CR_OK; CR_E_ARG (null st, null msg with n > 0, n < 0)
- **owns:** nothing new
- **thread:** any thread (no shared state)

```mettle
cr_hmac_sha512_update(&st, part.chars, (int64)part.length);
```

### cr_hmac_sha512_final

```mettle
export fn cr_hmac_sha512_final(st: CrHmacSha512State*, out: CrHmacSha512*) -> int32
```

Finishes an HMAC-SHA512 computation, writes the tag and wipes the state.

- **in:** st from cr_hmac_sha512_init; out
- **returns:** CR_OK; CR_E_ARG (null st or out)
- **owns:** st is wiped (start again with cr_hmac_sha512_init to reuse it)
- **thread:** any thread (no shared state)

```mettle
var t: CrHmacSha512; cr_hmac_sha512_final(&st, &t);
```

### cr_hmac_sha512

```mettle
export fn cr_hmac_sha512(out: CrHmacSha512*, key: uint8*, keylen: int64, msg: uint8*, n: int64) -> int32
```

Computes HMAC-SHA512(key, msg) in one call (init + update + final), for a key of any length.

- **in:** out; key/keylen as cr_hmac_sha512_init; msg/n as cr_hmac_sha512_update
- **returns:** CR_OK; CR_E_ARG. On error out is left zeroed
- **owns:** nothing; the temporary state is wiped
- **thread:** any thread (no shared state)

```mettle
var t: CrHmacSha512; cr_hmac_sha512(&t, &k[0], 32, m.chars, (int64)m.length);
```

### cr_hmac_sha512_verify

```mettle
export fn cr_hmac_sha512_verify(tag: CrHmacSha512*, key: uint8*, keylen: int64, msg: uint8*, n: int64) -> bool
```

Checks an HMAC-SHA512 tag in constant time: recomputes the tag and compares with cr_equal.

- **in:** tag, the received tag; key/keylen and msg/n as cr_hmac_sha512
- **returns:** true only if the tag is valid; false for a wrong tag and for every error (fails closed)
- **owns:** nothing; the recomputed tag is wiped
- **thread:** any thread (no shared state)

```mettle
if (!cr_hmac_sha512_verify(&t, &k[0], 32, m.chars, (int64)m.length)) { reject(); }
```

## mac/kdf

HKDF with HMAC-SHA256 and HMAC-SHA384 (RFC 5869): extract a pseudorandom key from input keying material, expand it into output keying material. Pure Mettle on top of the streaming HMAC in mac. Loop bounds depend only on public lengths.

### CR_HKDF_SHA256_PRK_BYTES

```mettle
export const CR_HKDF_SHA256_PRK_BYTES: int64 = 32;
```

Size of a pseudorandom key made by cr_hkdf_sha256_extract (the HMAC-SHA256 output size).

- **returns:** 32
- **thread:** constant

```mettle
var prk: uint8[CR_HKDF_SHA256_PRK_BYTES];
```

### CR_HKDF_SHA256_MAX

```mettle
export const CR_HKDF_SHA256_MAX: int64 = 8160;
```

Largest output cr_hkdf_sha256_expand and cr_hkdf_sha256 can produce (255 * 32 bytes, RFC 5869 section 2.3).

- **returns:** 8160
- **thread:** constant

```mettle
if (n > CR_HKDF_SHA256_MAX) { return CR_E_ARG; }
```

### cr_hkdf_sha256_extract

```mettle
export fn cr_hkdf_sha256_extract(prk: uint8*, salt: uint8*, saltlen: int64, ikm: uint8*, ikmlen: int64) -> int32
```

HKDF-Extract (RFC 5869 section 2.2): prk = HMAC-SHA256(salt, ikm). An empty salt (0 with saltlen 0) acts as 32 zero bytes, as the RFC says.

- **in:** prk, writable for 32 bytes; salt readable for saltlen bytes, saltlen >= 0 (random, but not secret; may be 0 only when saltlen == 0); ikm readable for ikmlen bytes, ikmlen >= 0 (may be 0 only when ikmlen == 0)
- **returns:** CR_OK; CR_E_ARG (null prk, null salt with saltlen > 0, null ikm with ikmlen > 0, negative length); CR_E_INIT. On error prk is wiped
- **owns:** prk stays the caller's and is secret: wipe it when done; nothing else is kept
- **thread:** any thread (no shared state)

```mettle
var prk: uint8[32]; cr_hkdf_sha256_extract(&prk[0], &salt[0], 16, &ikm[0], 32);
```

### cr_hkdf_sha256_expand

```mettle
export fn cr_hkdf_sha256_expand(out: uint8*, outlen: int64, prk: uint8*, prklen: int64, info: uint8*, infolen: int64) -> int32
```

HKDF-Expand (RFC 5869 section 2.3): stretches a pseudorandom key into outlen bytes of output keying material, T(1) | T(2) | ... with T(i) = HMAC-SHA256(prk, T(i-1) | info | i).

- **in:** out, writable for outlen bytes; outlen 1..8160 (CR_HKDF_SHA256_MAX); prk readable for prklen bytes, prklen >= 32 (normally the 32 bytes from cr_hkdf_sha256_extract); info readable for infolen bytes, infolen >= 0 (context, not secret; may be 0 only when infolen == 0)
- **returns:** CR_OK; CR_E_ARG (null out or prk, outlen outside 1..8160, prklen < 32, null info with infolen > 0, negative infolen); CR_E_INIT. On error out is wiped (when out is non-null and outlen is in range)
- **owns:** out stays the caller's and is secret; the temporary blocks and HMAC states are wiped
- **thread:** any thread (no shared state)

```mettle
var k: uint8[32]; cr_hkdf_sha256_expand(&k[0], 32, &prk[0], 32, info.chars, (int64)info.length);
```

### cr_hkdf_sha256

```mettle
export fn cr_hkdf_sha256(out: uint8*, outlen: int64, salt: uint8*, saltlen: int64, ikm: uint8*, ikmlen: int64, info: uint8*, infolen: int64) -> int32
```

HKDF-SHA256 in one call: extract with salt and ikm, then expand with info (RFC 5869). Use it to derive one or more keys from a shared secret.

- **in:** out, writable for outlen bytes; outlen 1..8160; salt/saltlen and ikm/ikmlen as cr_hkdf_sha256_extract; info/infolen as cr_hkdf_sha256_expand
- **returns:** CR_OK; CR_E_ARG (as cr_hkdf_sha256_extract and cr_hkdf_sha256_expand); CR_E_INIT. On error out is wiped (when out is non-null and outlen is in range)
- **owns:** out stays the caller's and is secret; the intermediate PRK is wiped
- **thread:** any thread (no shared state)

```mettle
var k: uint8[32]; cr_hkdf_sha256(&k[0], 32, &salt[0], 16, &ikm[0], 32, info.chars, (int64)info.length);
```

### CR_HKDF_SHA384_PRK_BYTES

```mettle
export const CR_HKDF_SHA384_PRK_BYTES: int64 = 48;
```

Size of a pseudorandom key made by cr_hkdf_sha384_extract (the HMAC-SHA384 output size).

- **returns:** 48
- **thread:** constant

```mettle
var prk: uint8[CR_HKDF_SHA384_PRK_BYTES];
```

### CR_HKDF_SHA384_MAX

```mettle
export const CR_HKDF_SHA384_MAX: int64 = 12240;
```

Largest output cr_hkdf_sha384_expand and cr_hkdf_sha384 can produce (255 * 48 bytes, RFC 5869 section 2.3).

- **returns:** 12240
- **thread:** constant

```mettle
if (n > CR_HKDF_SHA384_MAX) { return CR_E_ARG; }
```

### cr_hkdf_sha384_extract

```mettle
export fn cr_hkdf_sha384_extract(prk: uint8*, salt: uint8*, saltlen: int64, ikm: uint8*, ikmlen: int64) -> int32
```

HKDF-Extract (RFC 5869 section 2.2): prk = HMAC-SHA384(salt, ikm). An empty salt (0 with saltlen 0) acts as 48 zero bytes, as the RFC says.

- **in:** prk, writable for 48 bytes; salt readable for saltlen bytes, saltlen >= 0 (random, but not secret; may be 0 only when saltlen == 0); ikm readable for ikmlen bytes, ikmlen >= 0 (may be 0 only when ikmlen == 0)
- **returns:** CR_OK; CR_E_ARG (null prk, null salt with saltlen > 0, null ikm with ikmlen > 0, negative length); CR_E_INIT. On error prk is wiped
- **owns:** prk stays the caller's and is secret: wipe it when done; nothing else is kept
- **thread:** any thread (no shared state)

```mettle
var prk: uint8[48]; cr_hkdf_sha384_extract(&prk[0], &salt[0], 16, &ikm[0], 48);
```

### cr_hkdf_sha384_expand

```mettle
export fn cr_hkdf_sha384_expand(out: uint8*, outlen: int64, prk: uint8*, prklen: int64, info: uint8*, infolen: int64) -> int32
```

HKDF-Expand (RFC 5869 section 2.3): stretches a pseudorandom key into outlen bytes of output keying material, T(1) | T(2) | ... with T(i) = HMAC-SHA384(prk, T(i-1) | info | i).

- **in:** out, writable for outlen bytes; outlen 1..12240 (CR_HKDF_SHA384_MAX); prk readable for prklen bytes, prklen >= 48 (normally the 48 bytes from cr_hkdf_sha384_extract); info readable for infolen bytes, infolen >= 0 (context, not secret; may be 0 only when infolen == 0)
- **returns:** CR_OK; CR_E_ARG (null out or prk, outlen outside 1..12240, prklen < 48, null info with infolen > 0, negative infolen); CR_E_INIT. On error out is wiped (when out is non-null and outlen is in range)
- **owns:** out stays the caller's and is secret; the temporary blocks and HMAC states are wiped
- **thread:** any thread (no shared state)

```mettle
var k: uint8[48]; cr_hkdf_sha384_expand(&k[0], 48, &prk[0], 48, info.chars, (int64)info.length);
```

### cr_hkdf_sha384

```mettle
export fn cr_hkdf_sha384(out: uint8*, outlen: int64, salt: uint8*, saltlen: int64, ikm: uint8*, ikmlen: int64, info: uint8*, infolen: int64) -> int32
```

HKDF-SHA384 in one call: extract with salt and ikm, then expand with info (RFC 5869). Use it to derive one or more keys from a shared secret.

- **in:** out, writable for outlen bytes; outlen 1..12240; salt/saltlen and ikm/ikmlen as cr_hkdf_sha384_extract; info/infolen as cr_hkdf_sha384_expand
- **returns:** CR_OK; CR_E_ARG (as cr_hkdf_sha384_extract and cr_hkdf_sha384_expand); CR_E_INIT. On error out is wiped (when out is non-null and outlen is in range)
- **owns:** out stays the caller's and is secret; the intermediate PRK is wiped
- **thread:** any thread (no shared state)

```mettle
var k: uint8[48]; cr_hkdf_sha384(&k[0], 48, &salt[0], 16, &ikm[0], 48, info.chars, (int64)info.length);
```

## password/pwhash

Password hashing: Argon2id (RFC 9106), pure Mettle (argon2.mettle).

### CR_PW_INTERACTIVE

```mettle
export const CR_PW_INTERACTIVE: int32 = 1;
```

Preset for online logins: Argon2id t=2 passes, m=64 MiB. About 0.1-0.5 s and 64 MiB of RAM per hash on a desktop core; the right default for web logins.

- **returns:** 1
- **owns:** nothing
- **thread:** constant

```mettle
cr_pwhash_str(&h, pw.chars, (int64)pw.length, CR_PW_INTERACTIVE);
```

### CR_PW_MODERATE

```mettle
export const CR_PW_MODERATE: int32 = 2;
```

Preset with a higher margin: Argon2id t=3, m=256 MiB. Roughly 1-2 s per hash.

- **returns:** 2
- **owns:** nothing
- **thread:** constant

```mettle
cr_pwhash_str(&h, pw.chars, (int64)pw.length, CR_PW_MODERATE);
```

### CR_PW_SENSITIVE

```mettle
export const CR_PW_SENSITIVE: int32 = 3;
```

Preset for rarely used, high-value secrets (key files, vault unlock): Argon2id t=4, m=1 GiB. Several seconds and 1 GiB of RAM per hash; never use it on a server that hashes on request.

- **returns:** 3
- **owns:** nothing
- **thread:** constant

```mettle
cr_pwhash_str(&h, pw.chars, (int64)pw.length, CR_PW_SENSITIVE);
```

### cr_pwhash_preset_limits

```mettle
export fn cr_pwhash_preset_limits(preset: int32, ops: uint64*, mem: uint64*) -> int32
```

Reads the Argon2id costs of a preset.

- **in:** preset, one of CR_PW_*; ops and mem, where the costs go (passes, and memory in bytes)
- **returns:** CR_OK; CR_E_ARG for an unknown preset or a null out-param
- **owns:** nothing
- **thread:** any thread (no shared state)

```mettle
var t: uint64; var m: uint64; cr_pwhash_preset_limits(CR_PW_MODERATE, &t, &m);
```

### cr_pwhash_str

```mettle
export fn cr_pwhash_str(out: CrPwHash*, pw: uint8*, n: int64, preset: int32) -> int32
```

Hashes a password with Argon2id and a fresh random salt.

- **in:** out; pw readable for n bytes (any bytes; 0 allowed when n == 0), n >= 0; preset, one of CR_PW_INTERACTIVE / MODERATE / SENSITIVE
- **returns:** CR_OK; CR_E_ARG (null out, null pw with n > 0, n < 0, n above 4294967295, unknown preset); CR_E_INTERNAL if the random source fails or Argon2id could not allocate its memory. On error out is all zeros
- **owns:** nothing; the working memory is mapped, wiped and unmapped inside the call (never the Mettle heap). The caller should wipe the plaintext password after the call
- **thread:** any thread (no shared state). Blocks for the preset's time

```mettle
var h: CrPwHash; if (cr_pwhash_str(&h, pw.chars, (int64)pw.length, CR_PW_INTERACTIVE) == CR_OK) { store(cr_pwhash_text(&h)); }
```

### cr_pwhash_verify

```mettle
export fn cr_pwhash_verify(h: CrPwHash*, pw: uint8*, n: int64) -> bool
```

Checks a password against a stored hash: recomputes Argon2id with the costs, salt and hash length stored in the string and compares in constant time. Accepts only the form cr_pwhash_str writes ($argon2id, v=19, p=1, standard base64 without padding, salt 8..64 bytes, hash 16..64 bytes) and refuses strings that ask for more than m = 4 GiB or t = 64 passes, so an attacker-supplied string cannot make a server do unbounded work. Use cr_pwhash_needs_rehash afterwards to upgrade.

- **in:** h, a hash from cr_pwhash_str or cr_pwhash_from_text; pw/n the candidate password
- **returns:** true only if the password matches; false for a wrong password, a malformed, out-of-range or unterminated hash, any argument error, or an allocation failure inside Argon2id (fails closed)
- **owns:** nothing
- **thread:** any thread (no shared state). Takes as long as the hash's own costs

```mettle
if (cr_pwhash_verify(&h, pw.chars, (int64)pw.length)) { login(); }
```

### cr_pwhash_needs_rehash

```mettle
export fn cr_pwhash_needs_rehash(h: CrPwHash*, preset: int32) -> bool
```

Tells whether a stored hash should be replaced: it is not Argon2id as cr_pwhash_str writes it, or its costs, salt length or hash length differ from the preset's. Call it after a successful cr_pwhash_verify and, when true, store cr_pwhash_str of the same password.

- **in:** h; preset, one of CR_PW_*
- **returns:** false only if h is a well-formed Argon2id string with exactly the preset's costs (v=19, p=1, 16-byte salt, 32-byte hash); true otherwise (including malformed hashes and argument errors, so a caller that rehashes is never left on old costs)
- **owns:** nothing
- **thread:** any thread (no shared state)

```mettle
if (cr_pwhash_needs_rehash(&h, CR_PW_INTERACTIVE)) { rehash(); }
```

### cr_pwhash_text

```mettle
export fn cr_pwhash_text(h: CrPwHash*) -> string
```

Views a hash as text for storage (without the terminating nul).

- **in:** h
- **returns:** a string view of h's bytes up to the nul; empty if h is 0 or unterminated
- **owns:** the view borrows h (caller memory); copy it before h goes away
- **thread:** any thread (no shared state)

```mettle
db_put(user, cr_pwhash_text(&h));
```

### cr_pwhash_from_text

```mettle
export fn cr_pwhash_from_text(out: CrPwHash*, text: string) -> int32
```

Loads a stored hash string into a CrPwHash. Only the shape is checked here (length 8..127, no nul bytes, "$argon2" prefix); cr_pwhash_verify parses the rest.

- **in:** out; text, the string cr_pwhash_text gave you
- **returns:** CR_OK; CR_E_ARG (null out); CR_E_DECODE if text cannot be an Argon2 string. On error out is all zeros
- **owns:** text is copied; the caller keeps ownership of it
- **thread:** any thread (no shared state)

```mettle
var h: CrPwHash; if (cr_pwhash_from_text(&h, row.hash) == CR_OK) { ... }
```

## cipher/aead

Authenticated encryption, in pure Mettle: XChaCha20-Poly1305-IETF (draft-irtf-cfrg-xchacha-03 on top of RFC 8439) and the plain IETF ChaCha20-Poly1305 of RFC 8439 section 2.8 with a 12-byte nonce.

### CR_AEAD_TAG_BYTES

```mettle
export const CR_AEAD_TAG_BYTES: int64 = 16;
```

Bytes the authentication tag adds to every ciphertext.

- **returns:** 16
- **owns:** nothing
- **thread:** constant

```mettle
var cap: int64 = mlen + CR_AEAD_TAG_BYTES;
```

### cr_aead_keygen

```mettle
export fn cr_aead_keygen(k: CrKey32*) -> int32
```

Generates a random XChaCha20-Poly1305 key.

- **in:** k
- **returns:** CR_OK; CR_E_ARG (null k); CR_E_INTERNAL if the kernel random source fails
- **owns:** k is secret caller memory: cr_wipe(&k.b[0], 32) when done
- **thread:** any thread (no shared state)

```mettle
var k: CrKey32; cr_aead_keygen(&k); defer cr_wipe(&k.b[0], 32);
```

### cr_aead_nonce

```mettle
export fn cr_aead_nonce(n: CrNonce24*) -> int32
```

Generates a random 24-byte nonce. Call it once per message.

- **in:** n
- **returns:** CR_OK; CR_E_ARG (null n); CR_E_INTERNAL if the kernel random source fails
- **owns:** nothing; the nonce is public and travels with the ciphertext
- **thread:** any thread (no shared state)

```mettle
var nonce: CrNonce24; cr_aead_nonce(&nonce);
```

### cr_aead_encrypt

```mettle
export fn cr_aead_encrypt(ct: uint8*, ct_cap: int64, ct_len: int64*, msg: uint8*, mlen: int64, ad: uint8*, adlen: int64, nonce: CrNonce24*, key: CrKey32*) -> int32
```

Encrypts and authenticates msg, and authenticates (does not encrypt) ad. Output is ciphertext || 16-byte tag.

- **in:** ct writable for ct_cap bytes, ct_cap >= mlen + CR_AEAD_TAG_BYTES (ct may equal msg for in-place encryption; no other overlap); ct_len, where the ciphertext length goes; msg/mlen the plaintext; ad/adlen associated data (0, 0 for none); nonce, unique per key (cr_aead_nonce); key
- **returns:** CR_OK and *ct_len = mlen + 16; CR_E_ARG (null pointer with a non-zero length, negative length, null nonce/key/ct_len); CR_E_SPACE if ct_cap is too small (nothing written, *ct_len = 0)
- **owns:** nothing; the caller owns every buffer and must wipe key and plaintext when done
- **thread:** any thread (no shared state)

```mettle
cr_aead_encrypt(&ct[0], 64, &clen, m.chars, (int64)m.length, 0, 0, &nonce, &k);
```

### cr_aead_decrypt

```mettle
export fn cr_aead_decrypt(pt: uint8*, pt_cap: int64, pt_len: int64*, ct: uint8*, clen: int64, ad: uint8*, adlen: int64, nonce: CrNonce24*, key: CrKey32*) -> int32
```

Verifies and decrypts ct (ciphertext || tag) produced by cr_aead_encrypt with the same key, nonce and ad. Fails closed.

- **in:** pt writable for pt_cap bytes, pt_cap >= clen - CR_AEAD_TAG_BYTES (pt may equal ct for in-place decryption); pt_len; ct/clen; ad/adlen; nonce; key
- **returns:** CR_OK and *pt_len = clen - 16; CR_E_VERIFY if the tag does not match (wrong key, nonce or ad, tampered or truncated ciphertext, clen < 16); CR_E_ARG; CR_E_SPACE. On every failure *pt_len = 0 and the first pt_cap bytes of pt are wiped (cr_wipe), so no unauthenticated plaintext ever reaches the caller (when decrypting in place, pt == ct, that also wipes the ciphertext)
- **owns:** nothing; the caller owns every buffer and must wipe the plaintext when done
- **thread:** any thread (no shared state)

```mettle
if (cr_aead_decrypt(&pt[0], 64, &plen, &ct[0], clen, 0, 0, &nonce, &k) != CR_OK) { reject(); }
```

### cr_chacha20poly1305_encrypt

```mettle
export fn cr_chacha20poly1305_encrypt(ct: uint8*, ct_cap: int64, ct_len: int64*, msg: uint8*, mlen: int64, ad: uint8*, adlen: int64, nonce: CrNonce12*, key: CrKey32*) -> int32
```

Encrypts and authenticates msg, and authenticates (does not encrypt) ad, with the IETF ChaCha20-Poly1305 of RFC 8439 section 2.8 (12-byte nonce). Output is ciphertext || 16-byte tag. For protocols with counter nonces such as Noise. The caller must never use the same key with the same nonce twice; with random nonces use cr_aead_encrypt (24-byte nonces) instead.

- **in:** ct writable for ct_cap bytes, ct_cap >= mlen + CR_AEAD_TAG_BYTES (ct may equal msg for in-place encryption; no other overlap); ct_len, where the ciphertext length goes; msg/mlen the plaintext (mlen <= 274877906816, RFC 8439: 2^38 - 64); ad/adlen associated data (0, 0 for none); nonce, unique per key; key
- **returns:** CR_OK and *ct_len = mlen + 16; CR_E_ARG (null pointer with a non-zero length, negative length, mlen above 274877906816, null nonce/key/ct_len); CR_E_SPACE if ct_cap is too small (nothing written, *ct_len = 0)
- **owns:** nothing; the caller owns every buffer and must wipe key and plaintext when done
- **thread:** any thread (no shared state)

```mettle
cr_chacha20poly1305_encrypt(&ct[0], 64, &clen, m.chars, (int64)m.length, 0, 0, &nonce, &k);
```

### cr_chacha20poly1305_decrypt

```mettle
export fn cr_chacha20poly1305_decrypt(pt: uint8*, pt_cap: int64, pt_len: int64*, ct: uint8*, clen: int64, ad: uint8*, adlen: int64, nonce: CrNonce12*, key: CrKey32*) -> int32
```

Verifies and decrypts ct (ciphertext || tag) produced by cr_chacha20poly1305_encrypt with the same key, nonce and ad (RFC 8439 section 2.8). Fails closed.

- **in:** pt writable for pt_cap bytes, pt_cap >= clen - CR_AEAD_TAG_BYTES (pt may equal ct for in-place decryption); pt_len; ct/clen (clen - 16 <= 274877906816); ad/adlen; nonce; key
- **returns:** CR_OK and *pt_len = clen - 16; CR_E_VERIFY if the tag does not match (wrong key, nonce or ad, tampered or truncated ciphertext, clen < 16); CR_E_ARG (including a message above 274877906816 bytes); CR_E_SPACE. On every failure *pt_len = 0 and the first pt_cap bytes of pt are wiped (cr_wipe), so no unauthenticated plaintext ever reaches the caller (when decrypting in place, pt == ct, that also wipes the ciphertext)
- **owns:** nothing; the caller owns every buffer and must wipe the plaintext when done
- **thread:** any thread (no shared state)

```mettle
if (cr_chacha20poly1305_decrypt(&pt[0], 64, &plen, &ct[0], clen, 0, 0, &nonce, &k) != CR_OK) { reject(); }
```

## cipher/gcm

AES-128-GCM and AES-256-GCM (NIST SP 800-38D) with a 96-bit IV, in pure Mettle and constant time: bitsliced AES with no lookup tables (aes.mettle) and GHASH from masked 64-bit multiplications (ghash.mettle), both ported from BearSSL. On an x86-64 CPU with AES-NI, PCLMULQDQ and SSSE3 (checked once, in cr_aes_gcm_init) the same GCM runs on the hardware instructions instead (aes_hw_x86_64.mettle, also ported from BearSSL); the output is identical and neither path has a secret-dependent branch or address.

### CrAesGcm

```mettle
export struct CrAesGcm {
  skey: uint64[120];
  h: uint8[16];
  rounds: uint32;
  ready: uint32;
  hw: uint32;
  rk: uint8[240];
}
```

An AES-128-GCM or AES-256-GCM key, ready for cr_aes_gcm_*: the expanded AES round keys (bitsliced, and for AES-NI when the CPU has it) and the GHASH key. Initialize it once with cr_aes_gcm_init and use it for any number of messages.

- **in:** - (zeroed when declared: `var x: CrAesGcm;`; a zeroed context is not initialized and every use returns CR_E_ARG)
- **owns:** secret: wipe with cr_aes_gcm_wipe(&ctx) when done
- **thread:** any thread (no shared state)

```mettle
var ctx: CrAesGcm; cr_aes_gcm_init(&ctx, &key[0], 32); defer cr_aes_gcm_wipe(&ctx);
```

### cr_aes_gcm_init

```mettle
export fn cr_aes_gcm_init(ctx: CrAesGcm*, key: uint8*, keylen: int64) -> int32
```

Prepares an AES-128-GCM or AES-256-GCM key: expands the AES round keys and derives the GHASH key H = AES_K(0^128). The key is not kept in its original form. Any previous contents of ctx are replaced.

- **in:** ctx; key readable for keylen bytes; keylen, 16 (AES-128) or 32 (AES-256)
- **returns:** CR_OK; CR_E_ARG (null ctx or key, keylen other than 16 or 32; ctx is then left zeroed and not initialized)
- **owns:** ctx is secret caller memory: cr_aes_gcm_wipe(&ctx) when done; the caller also wipes its copy of the key
- **thread:** any thread (no shared state)

```mettle
var ctx: CrAesGcm; if (cr_aes_gcm_init(&ctx, &key[0], 16) != CR_OK) { return 1; }
```

### cr_aes_gcm_encrypt

```mettle
export fn cr_aes_gcm_encrypt(ctx: CrAesGcm*, ct: uint8*, ct_cap: int64, ct_len: int64*, msg: uint8*, mlen: int64, ad: uint8*, adlen: int64, nonce: CrNonce12*) -> int32
```

Encrypts and authenticates msg, and authenticates (does not encrypt) ad, with AES-GCM (NIST SP 800-38D, 96-bit IV). Output is ciphertext || 16-byte tag. Never use the same key with the same nonce twice.

- **in:** ctx initialized with cr_aes_gcm_init; ct writable for ct_cap bytes, ct_cap >= mlen + 16 (ct may equal msg for in-place encryption; no other overlap); ct_len, where the ciphertext length goes; msg/mlen the plaintext (mlen <= 68719476704, 2^36 - 32); ad/adlen associated data (0, 0 for none); nonce, 12 bytes, unique per key
- **returns:** CR_OK and *ct_len = mlen + 16; CR_E_ARG (null pointer with a non-zero length, negative length, mlen above 68719476704, ctx not initialized, null ctx/nonce/ct_len); CR_E_SPACE if ct_cap is too small (nothing written, *ct_len = 0)
- **owns:** nothing; the caller owns every buffer and must wipe the plaintext when done
- **thread:** any thread (no shared state)

```mettle
cr_aes_gcm_encrypt(&ctx, &ct[0], 64, &clen, m.chars, (int64)m.length, 0, 0, &nonce);
```

### cr_aes_gcm_decrypt

```mettle
export fn cr_aes_gcm_decrypt(ctx: CrAesGcm*, pt: uint8*, pt_cap: int64, pt_len: int64*, ct: uint8*, clen: int64, ad: uint8*, adlen: int64, nonce: CrNonce12*) -> int32
```

Verifies and decrypts ct (ciphertext || tag) produced by cr_aes_gcm_encrypt with the same key, nonce and ad. The tag is checked (in constant time) before anything is decrypted. Fails closed.

- **in:** ctx initialized with cr_aes_gcm_init; pt writable for pt_cap bytes, pt_cap >= clen - 16 (pt may equal ct for in-place decryption); pt_len; ct/clen (clen - 16 <= 68719476704); ad/adlen; nonce
- **returns:** CR_OK and *pt_len = clen - 16; CR_E_VERIFY if the tag does not match (wrong key, nonce or ad, tampered or truncated ciphertext, clen < 16); CR_E_ARG (including ctx not initialized and a message above 68719476704 bytes); CR_E_SPACE. On every failure *pt_len = 0 and the first pt_cap bytes of pt are wiped (cr_wipe), so no unauthenticated plaintext ever reaches the caller (when decrypting in place, pt == ct, that also wipes the ciphertext)
- **owns:** nothing; the caller owns every buffer and must wipe the plaintext when done
- **thread:** any thread (no shared state)

```mettle
if (cr_aes_gcm_decrypt(&ctx, &pt[0], 64, &plen, &ct[0], clen, 0, 0, &nonce) != CR_OK) { reject(); }
```

### cr_aes_gcm_wipe

```mettle
export fn cr_aes_gcm_wipe(ctx: CrAesGcm*)
```

Erases a CrAesGcm (both forms of the round keys and the GHASH key) and marks it uninitialized. Call it when the key is no longer needed.

- **in:** ctx, may be null (nothing happens)
- **owns:** nothing; the caller still owns ctx
- **thread:** any thread (no shared state)

```mettle
defer cr_aes_gcm_wipe(&ctx);
```

## ecc/kx

Key exchange: X25519 (RFC 7748), in pure Mettle, and a session-key derivation (BLAKE2b-512 of the X25519 point and both public keys, split into a receive key and a transmit key).

### cr_x25519_keypair

```mettle
export fn cr_x25519_keypair(pk: CrX25519Public*, sk: CrX25519Secret*) -> int32
```

Generates a random X25519 key pair: 32 random secret bytes and their public key. The pair works for cr_x25519_shared and for cr_kx_*.

- **in:** pk, sk
- **returns:** CR_OK; CR_E_ARG (null pointer); CR_E_INTERNAL if the kernel random source fails
- **owns:** sk is secret caller memory: cr_wipe(&sk.b[0], 32) when done
- **thread:** any thread (no shared state)

```mettle
var pk: CrX25519Public; var sk: CrX25519Secret; cr_x25519_keypair(&pk, &sk);
```

### cr_x25519_public_from_secret

```mettle
export fn cr_x25519_public_from_secret(pk: CrX25519Public*, sk: CrX25519Secret*) -> int32
```

Computes the public key of a secret scalar: X25519(sk, 9) (the scalar is clamped as RFC 7748 says).

- **in:** pk; sk
- **returns:** CR_OK; CR_E_ARG; CR_E_KEY if the result is zero
- **owns:** nothing
- **thread:** any thread (no shared state)

```mettle
cr_x25519_public_from_secret(&pk, &sk);
```

### cr_x25519_shared

```mettle
export fn cr_x25519_shared(out: CrX25519Shared*, sk: CrX25519Secret*, peer: CrX25519Public*) -> int32
```

Computes the raw X25519 shared point X25519(sk, peer). This is not a key: feed it, with both public keys, into a KDF (as Noise does) or use cr_kx_* instead.

- **in:** out; sk, our secret; peer, the other side's public key
- **returns:** CR_OK; CR_E_ARG; CR_E_KEY if peer is a low-order point (the result would be all zero; out is wiped)
- **owns:** out is secret caller memory: cr_wipe(&out.b[0], 32) when done
- **thread:** any thread (no shared state)

```mettle
var s: CrX25519Shared; if (cr_x25519_shared(&s, &sk, &peer) != CR_OK) { reject(); }
```

### cr_kx_client_session_keys

```mettle
export fn cr_kx_client_session_keys(rx: CrKey32*, tx: CrKey32*, client_pk: CrX25519Public*, client_sk: CrX25519Secret*, server_pk: CrX25519Public*) -> int32
```

Derives the client's session keys: rx decrypts what the server sends, tx encrypts what the client sends. The server's cr_kx_server_session_keys gives the mirror image (server rx == client tx, server tx == client rx).

- **in:** rx, tx; client_pk, client_sk (cr_x25519_keypair); server_pk
- **returns:** CR_OK; CR_E_ARG; CR_E_KEY if server_pk is unusable (rx and tx are wiped)
- **owns:** rx and tx are secret caller memory: wipe them when done
- **thread:** any thread (no shared state)

```mettle
var rx: CrKey32; var tx: CrKey32; cr_kx_client_session_keys(&rx, &tx, &cpk, &csk, &spk);
```

### cr_kx_server_session_keys

```mettle
export fn cr_kx_server_session_keys(rx: CrKey32*, tx: CrKey32*, server_pk: CrX25519Public*, server_sk: CrX25519Secret*, client_pk: CrX25519Public*) -> int32
```

Derives the server's session keys: rx decrypts what the client sends, tx encrypts what the server sends.

- **in:** rx, tx; server_pk, server_sk; client_pk
- **returns:** CR_OK; CR_E_ARG; CR_E_KEY if client_pk is unusable (rx and tx are wiped)
- **owns:** rx and tx are secret caller memory: wipe them when done
- **thread:** any thread (no shared state)

```mettle
cr_kx_server_session_keys(&rx, &tx, &spk, &ssk, &cpk);
```

## ecc/sign

Signatures: Ed25519 (RFC 8032 section 5.1), in pure Mettle, detached form. The secret key is 64 bytes: the RFC 8032 32-byte private key (the seed) followed by the public key.

### cr_ed25519_keypair

```mettle
export fn cr_ed25519_keypair(pk: CrEd25519Public*, sk: CrEd25519Secret*) -> int32
```

Generates a random Ed25519 key pair (a random seed).

- **in:** pk, sk
- **returns:** CR_OK; CR_E_ARG (null pointer); CR_E_INTERNAL if the kernel random source fails
- **owns:** sk is secret caller memory: cr_wipe(&sk.b[0], 64) when done
- **thread:** any thread (no shared state)

```mettle
var pk: CrEd25519Public; var sk: CrEd25519Secret; cr_ed25519_keypair(&pk, &sk);
```

### cr_ed25519_seed_keypair

```mettle
export fn cr_ed25519_seed_keypair(pk: CrEd25519Public*, sk: CrEd25519Secret*, seed: CrSeed32*) -> int32
```

Derives the Ed25519 key pair of a 32-byte seed (the RFC 8032 private key) deterministically.

- **in:** pk, sk; seed
- **returns:** CR_OK; CR_E_ARG (null pointer)
- **owns:** seed and sk are secret caller memory: wipe both when done
- **thread:** any thread (no shared state)

```mettle
cr_ed25519_seed_keypair(&pk, &sk, &seed);
```

### cr_ed25519_sign

```mettle
export fn cr_ed25519_sign(sig: CrEd25519Sig*, msg: uint8*, n: int64, sk: CrEd25519Secret*) -> int32
```

Signs msg, producing a detached 64-byte signature. Ed25519 is deterministic: the same key and message give the same signature.

- **in:** sig; msg/n (0 allowed when n == 0); sk
- **returns:** CR_OK; CR_E_ARG (null sig/sk, null msg with n > 0, n < 0). On error sig is zeroed
- **owns:** nothing; the caller owns sk and must wipe it when done
- **thread:** any thread (no shared state)

```mettle
var sig: CrEd25519Sig; cr_ed25519_sign(&sig, m.chars, (int64)m.length, &sk);
```

### cr_ed25519_verify

```mettle
export fn cr_ed25519_verify(sig: CrEd25519Sig*, msg: uint8*, n: int64, pk: CrEd25519Public*) -> bool
```

Verifies a detached Ed25519 signature (rejecting a non-canonical S, and non-canonical or small-order keys and R).

- **in:** sig; msg/n; pk, the signer's public key
- **returns:** true only if sig is a valid signature of exactly these n bytes under pk; false otherwise and for every argument error (fails closed)
- **owns:** nothing
- **thread:** any thread (no shared state)

```mettle
if (!cr_ed25519_verify(&sig, m.chars, (int64)m.length, &pk)) { reject(); }
```

## ecc/p256

NIST P-256 (secp256r1) key pairs and ECDH, in pure Mettle, constant time in the secret scalar: the "m31" implementation of BearSSL, with field elements as nine 30-bit limbs, Jacobian points, a 2-bit window for arbitrary points and a 4-bit window over a precomputed table for the generator. Standards: FIPS 186-5 (the curve and key pairs), SEC 1 v2 (point encoding).

### CR_P256_PUBLIC_BYTES

```mettle
export const CR_P256_PUBLIC_BYTES: int64 = 65;
```

Size in bytes of a P-256 public key: the uncompressed point 0x04 || X || Y.

- **returns:** 65
- **owns:** nothing
- **thread:** constant

```mettle
var pk: uint8[65]; cr_p256_keypair(&pk[0], CR_P256_PUBLIC_BYTES, &sk);
```

### CrP256Secret

```mettle
export struct CrP256Secret {
  b: uint8[32];
}
```

A P-256 secret key, the scalar d as 32 big-endian bytes with 1 <= d <= n-1 (n is the group order).

- **in:** - (zeroed when declared: `var x: CrP256Secret;`)
- **owns:** secret: wipe with cr_wipe(&sk.b[0], 32) when done
- **thread:** any thread (no shared state)

```mettle
var sk: CrP256Secret; cr_p256_keypair(&pk[0], 65, &sk);
```

### cr_p256_keypair

```mettle
export fn cr_p256_keypair(pk: uint8*, pk_cap: int64, sk: CrP256Secret*) -> int32
```

Generates a P-256 key pair: a uniformly random secret scalar in [1, n-1] (rejection sampling on 32 random bytes) and its public key, the uncompressed point 0x04 || X || Y.

- **in:** pk, writable for pk_cap bytes, pk_cap >= 65 (CR_P256_PUBLIC_BYTES); sk
- **returns:** CR_OK; CR_E_ARG (null pointer or negative pk_cap); CR_E_SPACE if pk_cap < 65 (nothing is written); CR_E_INTERNAL if the kernel random source fails (sk is wiped)
- **owns:** sk is secret caller memory: cr_wipe(&sk.b[0], 32) when done; pk is public
- **thread:** any thread (no shared state)

```mettle
var pk: uint8[65]; var sk: CrP256Secret; cr_p256_keypair(&pk[0], 65, &sk);
```

### cr_p256_public_from_secret

```mettle
export fn cr_p256_public_from_secret(pk: uint8*, pk_cap: int64, sk: CrP256Secret*) -> int32
```

Computes the public key of a P-256 secret scalar: the uncompressed point 0x04 || X || Y of d * G.

- **in:** pk, writable for pk_cap bytes, pk_cap >= 65 (CR_P256_PUBLIC_BYTES); sk
- **returns:** CR_OK; CR_E_ARG (null pointer or negative pk_cap); CR_E_SPACE if pk_cap < 65 (nothing is written); CR_E_KEY if the scalar is 0 or not lower than the group order n (the first 65 bytes of pk are wiped)
- **owns:** nothing
- **thread:** any thread (no shared state)

```mettle
if (cr_p256_public_from_secret(&pk[0], 65, &sk) != CR_OK) { reject(); }
```

### cr_p256_ecdh

```mettle
export fn cr_p256_ecdh(shared: CrKey32*, sk: CrP256Secret*, peer: uint8*, peer_len: int64) -> int32
```

Computes the raw ECDH shared secret of P-256: the X coordinate of the point sk * peer, as 32 big-endian bytes. This is not a key: feed it into a KDF (cr_hkdf_sha256 or cr_hkdf_sha384) with both public keys and a context string. The peer key is validated: it must be exactly 65 bytes, 0x04 || X || Y, with X and Y lower than the field prime and the point on the curve (the curve has prime order, so there are no small subgroups).

- **in:** shared; sk, our secret key; peer, readable for peer_len bytes
- **returns:** CR_OK; CR_E_ARG (null pointer, negative peer_len); CR_E_KEY if peer is not a valid public key (wrong length or format, coordinates out of range, not on the curve) or sk is 0 or not lower than n (shared is wiped)
- **owns:** shared is secret caller memory: cr_wipe(&shared.b[0], 32) when done
- **thread:** any thread (no shared state)

```mettle
var z: CrKey32; if (cr_p256_ecdh(&z, &sk, &peer[0], 65) != CR_OK) { reject(); }
```

## ecc/p384

NIST P-384 (secp384r1) key pairs and ECDH, in pure Mettle, constant time in the secret scalar: the generic prime-curve code of BearSSL on 31-bit big integers, with Jacobian points in Montgomery form, the doubling and addition formulas written as straight sequences of modular operations, and a 2-bit window for point multiplication. Standards: FIPS 186-5 (the curve and key pairs), SEC 1 v2 (point encoding).

### CR_P384_PUBLIC_BYTES

```mettle
export const CR_P384_PUBLIC_BYTES: int64 = 97;
```

Size in bytes of a P-384 public key: the uncompressed point 0x04 || X || Y.

- **returns:** 97
- **owns:** nothing
- **thread:** constant

```mettle
var pk: uint8[97]; cr_p384_keypair(&pk[0], CR_P384_PUBLIC_BYTES, &sk);
```

### CrP384Secret

```mettle
export struct CrP384Secret {
  b: uint8[48];
}
```

A P-384 secret key, the scalar d as 48 big-endian bytes with 1 <= d <= n-1 (n is the group order).

- **in:** - (zeroed when declared: `var x: CrP384Secret;`)
- **owns:** secret: wipe with cr_wipe(&sk.b[0], 48) when done
- **thread:** any thread (no shared state)

```mettle
var sk: CrP384Secret; cr_p384_keypair(&pk[0], 97, &sk);
```

### cr_p384_keypair

```mettle
export fn cr_p384_keypair(pk: uint8*, pk_cap: int64, sk: CrP384Secret*) -> int32
```

Generates a P-384 key pair: a uniformly random secret scalar in [1, n-1] (rejection sampling on 48 random bytes) and its public key, the uncompressed point 0x04 || X || Y.

- **in:** pk, writable for pk_cap bytes, pk_cap >= 97 (CR_P384_PUBLIC_BYTES); sk
- **returns:** CR_OK; CR_E_ARG (null pointer or negative pk_cap); CR_E_SPACE if pk_cap < 97 (nothing is written); CR_E_INTERNAL if the kernel random source fails (sk is wiped)
- **owns:** sk is secret caller memory: cr_wipe(&sk.b[0], 48) when done; pk is public
- **thread:** any thread (no shared state)

```mettle
var pk: uint8[97]; var sk: CrP384Secret; cr_p384_keypair(&pk[0], 97, &sk);
```

### cr_p384_public_from_secret

```mettle
export fn cr_p384_public_from_secret(pk: uint8*, pk_cap: int64, sk: CrP384Secret*) -> int32
```

Computes the public key of a P-384 secret scalar: the uncompressed point 0x04 || X || Y of d * G.

- **in:** pk, writable for pk_cap bytes, pk_cap >= 97 (CR_P384_PUBLIC_BYTES); sk
- **returns:** CR_OK; CR_E_ARG (null pointer or negative pk_cap); CR_E_SPACE if pk_cap < 97 (nothing is written); CR_E_KEY if the scalar is 0 or not lower than the group order n (the first 97 bytes of pk are wiped)
- **owns:** nothing
- **thread:** any thread (no shared state)

```mettle
if (cr_p384_public_from_secret(&pk[0], 97, &sk) != CR_OK) { reject(); }
```

### cr_p384_ecdh

```mettle
export fn cr_p384_ecdh(shared: uint8*, shared_cap: int64, sk: CrP384Secret*, peer: uint8*, peer_len: int64) -> int32
```

Computes the raw ECDH shared secret of P-384: the X coordinate of the point sk * peer, as 48 big-endian bytes. This is not a key: feed it into a KDF (cr_hkdf_sha384) with both public keys and a context string. The peer key is validated: it must be exactly 97 bytes, 0x04 || X || Y, with X and Y lower than the field prime and the point on the curve (the curve has prime order, so there are no small subgroups).

- **in:** shared, writable for shared_cap bytes, shared_cap >= 48; sk, our secret key; peer, readable for peer_len bytes
- **returns:** CR_OK; CR_E_ARG (null pointer, negative shared_cap or peer_len); CR_E_SPACE if shared_cap < 48 (nothing is written); CR_E_KEY if peer is not a valid public key (wrong length or format, coordinates out of range, not on the curve) or sk is 0 or not lower than n (the first 48 bytes of shared are wiped)
- **owns:** shared is secret caller memory: cr_wipe(shared, 48) when done
- **thread:** any thread (no shared state)

```mettle
var z: uint8[48]; if (cr_p384_ecdh(&z[0], 48, &sk, &peer[0], 97) != CR_OK) { reject(); }
```

## ecc/ecdsa

ECDSA on NIST P-256 (secp256r1) and P-384 (secp384r1), in pure Mettle: deterministic signatures (RFC 6979) with constant-time signing, and strict verification. Standards: FIPS 186-5 (ECDSA), SEC 1 v2, RFC 6979 (the nonce), RFC 3279 (the DER signature, SEQUENCE { INTEGER r, INTEGER s }).

### CR_P256_ECDSA_SIG_MAX

```mettle
export const CR_P256_ECDSA_SIG_MAX: int64 = 72;
```

Largest DER signature cr_p256_ecdsa_sign can write: 72 bytes (a SEQUENCE of two INTEGERs of up to 33 bytes each).

- **returns:** 72
- **owns:** nothing
- **thread:** constant

```mettle
var sig: uint8[72]; var sl: int64; cr_p256_ecdsa_sign(&sig[0], CR_P256_ECDSA_SIG_MAX, &sl, m, n, CR_HASH_SHA256, &sk);
```

### CR_P384_ECDSA_SIG_MAX

```mettle
export const CR_P384_ECDSA_SIG_MAX: int64 = 104;
```

Largest DER signature cr_p384_ecdsa_sign can write: 104 bytes (a SEQUENCE of two INTEGERs of up to 49 bytes each).

- **returns:** 104
- **owns:** nothing
- **thread:** constant

```mettle
var sig: uint8[104]; var sl: int64; cr_p384_ecdsa_sign(&sig[0], CR_P384_ECDSA_SIG_MAX, &sl, m, n, CR_HASH_SHA384, &sk);
```

### cr_p256_ecdsa_sign

```mettle
export fn cr_p256_ecdsa_sign(sig: uint8*, sig_cap: int64, sig_len: int64*, msg: uint8*, n: int64, hash: int32, sk: CrP256Secret*) -> int32
```

Signs a message with ECDSA on P-256 and returns the signature in DER (SEQUENCE { INTEGER r, INTEGER s }, 8 to 72 bytes). The message is hashed with the chosen hash (CR_HASH_SHA256, CR_HASH_SHA384 or CR_HASH_SHA512); the nonce is deterministic (RFC 6979, HMAC_DRBG over the same hash), so a bad random source cannot leak the key, and the same inputs always give the same signature. The signing time does not depend on the secret scalar or the nonce.

- **in:** sig, writable for sig_cap bytes (CR_P256_ECDSA_SIG_MAX = 72 always suffices); sig_len, receives the length; msg readable for n bytes (msg may be 0 only when n == 0); hash a CR_HASH_* id; sk the secret scalar
- **returns:** CR_OK; CR_E_ARG (null pointer, negative n or sig_cap, unknown hash id); CR_E_KEY if the scalar is 0 or not lower than the group order; CR_E_SPACE if sig_cap is smaller than the DER length (nothing is written). On every error *sig_len is 0
- **owns:** sk is secret caller memory; every temporary (nonce, DRBG state, scalars) is wiped before return
- **thread:** any thread (no shared state)

```mettle
var sl: int64; if (cr_p256_ecdsa_sign(&sig[0], 72, &sl, m.chars, (int64)m.length, CR_HASH_SHA256, &sk) != CR_OK) { fail(); }
```

### cr_p256_ecdsa_verify

```mettle
export fn cr_p256_ecdsa_verify(sig: uint8*, sig_len: int64, msg: uint8*, n: int64, hash: int32, pk: uint8*, pk_len: int64) -> bool
```

Verifies an ECDSA P-256 signature over a message. The message is hashed with the chosen hash (CR_HASH_SHA256, CR_HASH_SHA384 or CR_HASH_SHA512). The public key is the 65-byte point 0x04 || X || Y and is fully validated (coordinates below the field prime, on the curve). The signature must be strict DER: it is converted to r || s and encoded again, and the two encodings must be byte-identical (no leading zeros, no BER forms); r and s must be in [1, n-1]. Only public data is used.

- **in:** sig readable for sig_len bytes; msg readable for n bytes (msg may be 0 only when n == 0); hash a CR_HASH_* id; pk readable for pk_len bytes
- **returns:** true only for a valid signature; false for an invalid one and for every error (null pointer, negative length, unknown hash, bad key or signature encoding): it fails closed
- **owns:** nothing
- **thread:** any thread (no shared state)

```mettle
if (!cr_p256_ecdsa_verify(&sig[0], sl, m.chars, (int64)m.length, CR_HASH_SHA256, &pk[0], 65)) { reject(); }
```

### cr_p384_ecdsa_sign

```mettle
export fn cr_p384_ecdsa_sign(sig: uint8*, sig_cap: int64, sig_len: int64*, msg: uint8*, n: int64, hash: int32, sk: CrP384Secret*) -> int32
```

Signs a message with ECDSA on P-384 and returns the signature in DER (SEQUENCE { INTEGER r, INTEGER s }, 8 to 104 bytes). The message is hashed with the chosen hash (CR_HASH_SHA256, CR_HASH_SHA384 or CR_HASH_SHA512); the nonce is deterministic (RFC 6979, HMAC_DRBG over the same hash), so a bad random source cannot leak the key, and the same inputs always give the same signature. The signing time does not depend on the secret scalar or the nonce.

- **in:** sig, writable for sig_cap bytes (CR_P384_ECDSA_SIG_MAX = 104 always suffices); sig_len, receives the length; msg readable for n bytes (msg may be 0 only when n == 0); hash a CR_HASH_* id; sk the secret scalar
- **returns:** CR_OK; CR_E_ARG (null pointer, negative n or sig_cap, unknown hash id); CR_E_KEY if the scalar is 0 or not lower than the group order; CR_E_SPACE if sig_cap is smaller than the DER length (nothing is written). On every error *sig_len is 0
- **owns:** sk is secret caller memory; every temporary (nonce, DRBG state, scalars) is wiped before return
- **thread:** any thread (no shared state)

```mettle
var sl: int64; if (cr_p384_ecdsa_sign(&sig[0], 104, &sl, m.chars, (int64)m.length, CR_HASH_SHA384, &sk) != CR_OK) { fail(); }
```

### cr_p384_ecdsa_verify

```mettle
export fn cr_p384_ecdsa_verify(sig: uint8*, sig_len: int64, msg: uint8*, n: int64, hash: int32, pk: uint8*, pk_len: int64) -> bool
```

Verifies an ECDSA P-384 signature over a message. The message is hashed with the chosen hash (CR_HASH_SHA256, CR_HASH_SHA384 or CR_HASH_SHA512). The public key is the 97-byte point 0x04 || X || Y and is fully validated (coordinates below the field prime, on the curve). The signature must be strict DER: it is converted to r || s and encoded again, and the two encodings must be byte-identical (no leading zeros, no BER forms); r and s must be in [1, n-1]. Only public data is used.

- **in:** sig readable for sig_len bytes; msg readable for n bytes (msg may be 0 only when n == 0); hash a CR_HASH_* id; pk readable for pk_len bytes
- **returns:** true only for a valid signature; false for an invalid one and for every error (null pointer, negative length, unknown hash, bad key or signature encoding): it fails closed
- **owns:** nothing
- **thread:** any thread (no shared state)

```mettle
if (!cr_p384_ecdsa_verify(&sig[0], sl, m.chars, (int64)m.length, CR_HASH_SHA384, &pk[0], 97)) { reject(); }
```

## rsa/rsa

RSA signatures in pure Mettle: RSASSA-PKCS1-v1_5 (RFC 8017 sections 8.2 and 9.2) and RSASSA-PSS (sections 8.1 and 9.1, with MGF1 from appendix B.2.1), signing and verifying, over the 31-bit big integers of crypto/ecc/i31.

### cr_rsa_verify_pkcs1

```mettle
export fn cr_rsa_verify_pkcs1(sig: uint8*, sig_len: int64, msg: uint8*, n: int64, hash: int32, mod: uint8*, mod_len: int64, e: uint8*, e_len: int64) -> bool
```

Verifies an RSASSA-PKCS1-v1_5 signature (RFC 8017 8.2.2) over a message. The message is hashed with the CR_HASH_* choice and the signature must equal the encoding 00 01 FF..FF 00 DigestInfo(hash, NULL parameters) H exactly. The modulus must have 2048 to 4096 bits and be odd; the public exponent must be odd, at least 3 and at most 8 bytes; both are big-endian and may carry leading zero bytes. The signature must be exactly as long as the modulus and lower than it.

- **in:** sig readable for sig_len bytes; msg readable for n bytes (may be 0 when n == 0); hash is CR_HASH_SHA256, CR_HASH_SHA384 or CR_HASH_SHA512; mod readable for mod_len bytes; e readable for e_len bytes
- **returns:** true only for a valid signature; false for an invalid one and for any bad argument (null pointer, negative length, unknown hash, bad key, wrong signature length)
- **owns:** nothing; all inputs stay with the caller
- **thread:** any thread (no shared state)

```mettle
if (cr_rsa_verify_pkcs1(sig, sl, m, n, CR_HASH_SHA256, mod, ml, e, el)) { accept(); }
```

### cr_rsa_verify_pss

```mettle
export fn cr_rsa_verify_pss(sig: uint8*, sig_len: int64, msg: uint8*, n: int64, hash: int32, mod: uint8*, mod_len: int64, e: uint8*, e_len: int64) -> bool
```

Verifies an RSASSA-PSS signature (RFC 8017 8.1.2) over a message. The message is hashed with the CR_HASH_* choice; MGF1 uses the same hash, the salt is as long as the hash and the trailer is 0xbc (RFC 8017 9.1.2). The modulus must have 2048 to 4096 bits and be odd; the public exponent must be odd, at least 3 and at most 8 bytes; both are big-endian and may carry leading zero bytes. The signature must be exactly as long as the modulus and lower than it.

- **in:** sig readable for sig_len bytes; msg readable for n bytes (may be 0 when n == 0); hash is CR_HASH_SHA256, CR_HASH_SHA384 or CR_HASH_SHA512; mod readable for mod_len bytes; e readable for e_len bytes
- **returns:** true only for a valid signature; false for an invalid one and for any bad argument (null pointer, negative length, unknown hash, bad key, wrong signature length)
- **owns:** nothing; all inputs stay with the caller
- **thread:** any thread (no shared state)

```mettle
if (cr_rsa_verify_pss(sig, sl, m, n, CR_HASH_SHA256, mod, ml, e, el)) { accept(); }
```

### CrRsaPrivate

```mettle
export struct CrRsaPrivate {
  n: uint8*;
  n_len: int64;
  e: uint8*;
  e_len: int64;
  p: uint8*;
  p_len: int64;
  q: uint8*;
  q_len: int64;
  dp: uint8*;
  dp_len: int64;
  dq: uint8*;
  dq_len: int64;
  qinv: uint8*;
  qinv_len: int64;
}
```

An RSA private key for signing, as the big-endian bytes of the fields of a PKCS#1 RSAPrivateKey (RFC 8017 A.1.2): modulus n, public exponent e, primes p and q, dp = d mod (p - 1), dq = d mod (q - 1) and qinv = q^-1 mod p. Leading zero bytes are allowed on every field. The modulus has 2048 to 4096 bits; p and q have at most 2080 bits each and are odd.

- **in:** pointers to the field bytes and their lengths; each pointer is readable for its length (zero the struct when declared: `var k: CrRsaPrivate;`)
- **owns:** the struct holds pointers only; the caller owns the bytes they point to and wipes p, q, dp, dq and qinv with cr_wipe when done
- **thread:** any thread (no shared state)

```mettle
var k: CrRsaPrivate; k.n = n; k.n_len = nl; k.e = e; k.e_len = el; k.p = p; k.p_len = pl; k.q = q; k.q_len = ql; k.dp = dp; k.dp_len = dpl; k.dq = dq; k.dq_len = dql; k.qinv = qi; k.qinv_len = qil;
```

### cr_rsa_sign_pss

```mettle
export fn cr_rsa_sign_pss(sig: uint8*, sig_cap: int64, sig_len: int64*, msg: uint8*, n: int64, hash: int32, key: CrRsaPrivate*) -> int32
```

Signs a message with RSASSA-PSS (RFC 8017 8.1.1 and 9.1.1). The message is hashed with the CR_HASH_* choice; MGF1 uses the same hash, the salt is as long as the hash and fresh from cr_random_bytes, and the trailer is 0xbc. The signature is as long as the modulus (2048 to 4096 bits); cr_rsa_verify_pss accepts it. The private operation uses the Chinese remainder theorem in constant time in p, q, dp, dq, qinv and the message. Before the signature is returned the public operation is run on it and compared with the padded message; if that differs (a fault, or a damaged key) the output is wiped and CR_E_INTERNAL is returned. Signatures differ on every call (random salt).

- **in:** sig, writable for sig_cap bytes (the modulus length always suffices; 512 is the most); sig_len, receives the length; msg readable for n bytes (msg may be 0 only when n == 0); hash is CR_HASH_SHA256, CR_HASH_SHA384 or CR_HASH_SHA512; key as described at CrRsaPrivate
- **returns:** CR_OK; CR_E_ARG (null pointer, negative n or sig_cap, unknown hash, bad key part: modulus outside 2048..4096 bits or even, bad exponent, p or q even, empty or oversized, p and q not matching the modulus length); CR_E_SPACE if sig_cap is smaller than the modulus length (nothing is written, *sig_len is the length needed); CR_E_INTERNAL if the random source fails, the private operation fails (message not lower than p*q) or the fault check fails (sig is zeroed). On every other error *sig_len is 0
- **owns:** the key bytes stay with the caller, who wipes them; every secret temporary (big integers, salt) is wiped before return
- **thread:** any thread (no shared state)

```mettle
var sl: int64; if (cr_rsa_sign_pss(&sig[0], 512, &sl, m.chars, (int64)m.length, CR_HASH_SHA256, &key) != CR_OK) { fail(); }
```

### cr_rsa_sign_pkcs1

```mettle
export fn cr_rsa_sign_pkcs1(sig: uint8*, sig_cap: int64, sig_len: int64*, msg: uint8*, n: int64, hash: int32, key: CrRsaPrivate*) -> int32
```

Signs a message with RSASSA-PKCS1-v1_5 (RFC 8017 8.2.1 and 9.2). The message is hashed with the CR_HASH_* choice and encoded as 00 01 FF..FF 00 DigestInfo(hash, NULL parameters) H, as long as the modulus. The signature is as long as the modulus (2048 to 4096 bits) and is deterministic; cr_rsa_verify_pkcs1 accepts it. The private operation uses the Chinese remainder theorem in constant time in p, q, dp, dq, qinv and the message. Before the signature is returned the public operation is run on it and compared with the padded message; if that differs (a fault, or a damaged key) the output is wiped and CR_E_INTERNAL is returned.

- **in:** sig, writable for sig_cap bytes (the modulus length always suffices; 512 is the most); sig_len, receives the length; msg readable for n bytes (msg may be 0 only when n == 0); hash is CR_HASH_SHA256, CR_HASH_SHA384 or CR_HASH_SHA512; key as described at CrRsaPrivate
- **returns:** CR_OK; CR_E_ARG (null pointer, negative n or sig_cap, unknown hash, bad key part: modulus outside 2048..4096 bits or even, bad exponent, p or q even, empty or oversized, p and q not matching the modulus length); CR_E_SPACE if sig_cap is smaller than the modulus length (nothing is written, *sig_len is the length needed); CR_E_INTERNAL if the private operation fails (message not lower than p*q) or the fault check fails (sig is zeroed). On every other error *sig_len is 0
- **owns:** the key bytes stay with the caller, who wipes them; every secret temporary (big integers) is wiped before return
- **thread:** any thread (no shared state)

```mettle
var sl: int64; if (cr_rsa_sign_pkcs1(&sig[0], 512, &sl, m.chars, (int64)m.length, CR_HASH_SHA256, &key) != CR_OK) { fail(); }
```

## recipes/signed

Signed values, pure Mettle: "value.SIG", a readable value with an HMAC-SHA256 tag (RFC 2104). SIG = base64url, no padding (always 43 chars), of HMAC-SHA256(key, le64(len(purpose)) || purpose || value) where le64 is the purpose length as 8 bytes, little endian. The purpose ("remember", "unsubscribe") separates uses: a value signed for one purpose does not verify for another. The value stays readable by anyone; only key holders can produce a valid tag. The format is byte-compatible with the session library's signed values (same formula).

### CR_SIGNED_EXTRA

```mettle
export const CR_SIGNED_EXTRA: int64 = 44;
```

the bytes cr_signed_sign adds to a value: '.' plus the 43-character tag.

- **returns:** 44
- **owns:** nothing
- **thread:** constant

```mettle
var cap: int64 = (int64)v.length + CR_SIGNED_EXTRA;
```

### CR_SIGNED_MAX_KEYS

```mettle
export const CR_SIGNED_MAX_KEYS: int32 = 8;
```

the most keys cr_signed_verify accepts.

- **returns:** 8
- **owns:** nothing
- **thread:** constant

```mettle
if (nkeys > CR_SIGNED_MAX_KEYS) { ... }
```

### cr_signed_sign

```mettle
export fn cr_signed_sign(out: uint8*, out_cap: int64, out_len: int64*, key: CrKey32*, purpose: string, value: string) -> int32
```

Signs a value for a purpose: writes "value.SIG" (the value, a '.', and the 43-character base64url tag of HMAC-SHA256(key, le64(len(purpose)) || purpose || value)). There is no nul terminator. The value is not encrypted: anyone can read it.

- **in:** out writable for out_cap >= value.length + CR_SIGNED_EXTRA bytes, and not overlapping value; out_len; key (a CrKey32, 32+ random bytes); purpose, 1..255 bytes; value, any bytes (may be empty)
- **returns:** CR_OK and *out_len = value.length + 44; CR_E_ARG (null out_len, out or key; purpose length outside 1..255); CR_E_SPACE if out_cap is too small (nothing written). On error *out_len = 0
- **owns:** nothing; the caller owns out, the tag and encode buffers are wiped
- **thread:** any thread (no shared state)

```mettle
var buf: uint8[128]; var n: int64; cr_signed_sign(&buf[0], 128, &n, &k, "remember", "user:42");
```

### cr_signed_verify

```mettle
export fn cr_signed_verify(out_value: string*, key_index: int32*, keys: CrKey32*, nkeys: int32, purpose: string, signed_value: string) -> int32
```

Verifies a "value.SIG" string against up to 8 keys for a purpose and returns the value. keys[0] is the current key; keys[1..] are older keys still accepted during rotation. The tag is recomputed with every key (no early exit) and compared in constant time; the lowest matching index is reported. Everything malformed fails closed with CR_E_VERIFY: too short, no '.' before the 43-character tag, a tag that is not canonical base64url of 32 bytes, or no key matching.

- **in:** out_value; key_index (may be 0); keys readable for nkeys CrKey32, 1..CR_SIGNED_MAX_KEYS of them; purpose, 1..255 bytes, the same as when signing; signed_value
- **returns:** CR_OK with *out_value the value and *key_index the matching key (key_index > 0 means the value was signed with an older key: re-sign it with keys[0]); CR_E_ARG (null out_value or keys, nkeys outside 1..8, purpose length outside 1..255); CR_E_VERIFY. On error *out_value = "" and *key_index = -1
- **owns:** *out_value borrows signed_value (a view into it, not a copy; do not free it, and keep signed_value alive while it is used)
- **thread:** any thread (no shared state)

```mettle
var v: string; var ki: int32; if (cr_signed_verify(&v, &ki, &keys[0], 2, "remember", cookie) == CR_OK) { ... }
```

### CR_SIGNED_UNTIL_EXTRA

```mettle
export const CR_SIGNED_UNTIL_EXTRA: int64 = 61;
```

the bytes cr_signed_sign_until adds to a value: '.', 16 hex digits of the expiry, '.', and the 43-character tag.

- **returns:** 61
- **owns:** nothing
- **thread:** constant

```mettle
var cap: int64 = (int64)v.length + CR_SIGNED_UNTIL_EXTRA;
```

### cr_signed_sign_until

```mettle
export fn cr_signed_sign_until(out: uint8*, out_cap: int64, out_len: int64*, key: CrKey32*, purpose: string, value: string, expires: int64) -> int32
```

Signs a value with an expiry time: writes "value.EXP.SIG" (the value, a '.', EXP, a '.', and the 43-character base64url tag SIG). EXP is exactly 16 lowercase hex digits of the expiry as an unsigned 64-bit big-endian number (Unix seconds; the caller picks the clock). SIG is the tag of HMAC-SHA256(key, "cryptex/signed-until/v1" || le64(len(purpose)) || purpose || le64(expires) || value), which is domain-separated from the plain cr_signed_sign form: a value from one form never verifies as the other. There is no nul terminator. The value is not encrypted: anyone can read it and the expiry.

- **in:** out writable for out_cap >= value.length + CR_SIGNED_UNTIL_EXTRA bytes, and not overlapping value; out_len; key (a CrKey32, 32+ random bytes); purpose, 1..255 bytes; value, any bytes (may be empty); expires, the absolute expiry time, >= 0
- **returns:** CR_OK and *out_len = value.length + 61; CR_E_ARG (null out_len, out or key; purpose length outside 1..255; expires < 0); CR_E_SPACE if out_cap is too small (nothing written). On error *out_len = 0
- **owns:** nothing; the caller owns out, the tag and encode buffers are wiped
- **thread:** any thread (no shared state)

```mettle
var buf: uint8[128]; var n: int64; cr_signed_sign_until(&buf[0], 128, &n, &k, "confirm-email", "user:42", now + 3600);
```

### cr_signed_verify_until

```mettle
export fn cr_signed_verify_until(out_value: string*, key_index: int32*, keys: CrKey32*, nkeys: int32, purpose: string, signed_value: string, now: int64) -> int32
```

Verifies a "value.EXP.SIG" string against up to 8 keys for a purpose, then checks the expiry, and returns the value. keys[0] is the current key; keys[1..] are older keys still accepted during rotation. The tag is recomputed with every key (no early exit) and compared in constant time; the lowest matching index is reported. Everything malformed fails closed with CR_E_VERIFY: too short, a '.' missing before EXP or before the 43-character tag, an EXP that is not exactly 16 lowercase hex digits (uppercase is rejected: the encoding is canonical) or has its top bit set, a tag that is not canonical base64url of 32 bytes, or no key matching. The expiry is checked only after a key matched, so an unauthenticated value never reports CR_E_EXPIRED. expires is an absolute time bound by the MAC: a verifier cannot extend it, and changing EXP invalidates the tag. Pass the current time (the same clock used when signing) as now.

- **in:** out_value; key_index (may be 0); keys readable for nkeys CrKey32, 1..CR_SIGNED_MAX_KEYS of them; purpose, 1..255 bytes, the same as when signing; signed_value; now, the current time in the signer's units (Unix seconds)
- **returns:** CR_OK with *out_value the value and *key_index the matching key (key_index > 0 means the value was signed with an older key: re-sign it with keys[0]), when now < expires; CR_E_EXPIRED when authentic but now >= expires; CR_E_ARG (null out_value or keys, nkeys outside 1..8, purpose length outside 1..255); CR_E_VERIFY. On any error *out_value = "" and *key_index = -1
- **owns:** *out_value borrows signed_value (a view into it, not a copy; do not free it, and keep signed_value alive while it is used)
- **thread:** any thread (no shared state)

```mettle
var v: string; var ki: int32; if (cr_signed_verify_until(&v, &ki, &keys[0], 2, "confirm-email", token, now) == CR_OK) { ... }
```

## recipes/token

Machine tokens and human codes, pure Mettle (SHA-256 per FIPS 180-4 and HMAC-SHA256 per RFC 2104). They look alike (random characters from an alphabet) but they are different things and are stored differently.

### CR_TOKEN_BASE62

```mettle
export const CR_TOKEN_BASE62: int32 = 1;
```

Alphabet selector: base62, 0-9 A-Z a-z (5.954 bits per character).

- **returns:** 1
- **owns:** nothing
- **thread:** constant

```mettle
cr_token_new(&t[0], 64, &n, "mtlr_", CR_TOKEN_BASE62, 40);
```

### CR_TOKEN_BASE32

```mettle
export const CR_TOKEN_BASE32: int32 = 2;
```

Alphabet selector: Crockford base32, lowercase, without i l o u (5 bits per character).

- **returns:** 2
- **owns:** nothing
- **thread:** constant

```mettle
cr_token_new(&t[0], 32, &n, "", CR_TOKEN_BASE32, 26);
```

### CR_TOKEN_MIN_BITS

```mettle
export const CR_TOKEN_MIN_BITS: int64 = 128;
```

Fewest bits of entropy a token may have (base62 needs 22 characters, base32 26).

- **returns:** 128
- **owns:** nothing
- **thread:** constant

```mettle
if (cr_token_bits(CR_TOKEN_BASE62, 22) >= CR_TOKEN_MIN_BITS) { ok(); }
```

### CR_TOKEN_MAX_CHARS

```mettle
export const CR_TOKEN_MAX_CHARS: int64 = 256;
```

Most random characters a token may have.

- **returns:** 256
- **owns:** nothing
- **thread:** constant

```mettle
var t: uint8[300]; cr_token_new(&t[0], 300, &n, "", CR_TOKEN_BASE62, CR_TOKEN_MAX_CHARS);
```

### CR_TOKEN_MAX_PREFIX

```mettle
export const CR_TOKEN_MAX_PREFIX: int64 = 32;
```

Longest prefix, in bytes.

- **returns:** 32
- **owns:** nothing
- **thread:** constant

```mettle
var cap: int64 = CR_TOKEN_MAX_PREFIX + CR_TOKEN_MAX_CHARS + 1;
```

### CR_CODE_MIN_CHARS

```mettle
export const CR_CODE_MIN_CHARS: int64 = 8;
```

Fewest random characters a code may have (8 characters is 40 bits; 10 or more is recommended).

- **returns:** 8
- **owns:** nothing
- **thread:** constant

```mettle
cr_code_new(&c[0], 16, &n, CR_CODE_MIN_CHARS, 0);
```

### CR_CODE_MAX_CHARS

```mettle
export const CR_CODE_MAX_CHARS: int64 = 32;
```

Most random characters a code may have.

- **returns:** 32
- **owns:** nothing
- **thread:** constant

```mettle
var c: uint8[40]; cr_code_new(&c[0], 40, &n, CR_CODE_MAX_CHARS, 0);
```

### cr_token_new

```mettle
export fn cr_token_new(out: uint8*, out_cap: int64, out_len: int64*, prefix: string, alphabet: int32, nchars: int64) -> int32
```

Generates a random token: prefix, then nchars characters each picked uniformly from the alphabet (no modulo bias), then a nul. A bearer token needs at least 128 bits of entropy (CR_TOKEN_MIN_BITS): base62 >= 22 characters, base32 >= 26 (see cr_token_bits).

- **in:** out writable for out_cap >= prefix.length + nchars + 1 bytes; out_len; prefix, at most 32 bytes of [A-Za-z0-9_-] (may be empty); alphabet, one of CR_TOKEN_BASE62 / CR_TOKEN_BASE32; nchars <= 256 with cr_token_bits(alphabet, nchars) >= 128
- **returns:** CR_OK and *out_len = prefix.length + nchars (the nul is not counted); CR_E_ARG (null out or out_len, bad prefix, unknown alphabet, nchars too small for 128 bits or above 256); CR_E_SPACE if out_cap is too small (nothing written); CR_E_INTERNAL if the kernel random source fails (out, out_cap bytes, is wiped). On error *out_len = 0
- **owns:** nothing; the caller owns out
- **thread:** any thread (no shared state)

```mettle
var t: uint8[64]; var n: int64; cr_token_new(&t[0], 64, &n, "mtlr_", CR_TOKEN_BASE62, 40);  // registry API token
```

### cr_token_check

```mettle
export fn cr_token_check(tok: string, prefix: string, alphabet: int32, nchars: int64) -> bool
```

Checks the format of a token: its length is prefix.length + nchars, it starts with prefix, and every following byte is in the alphabet. Use it to reject malformed input before any store lookup; it is not a secret comparison.

- **in:** tok, prefix; alphabet, one of the CR_TOKEN_* selectors; nchars <= 256 with cr_token_bits(alphabet, nchars) >= 128
- **returns:** true if the token has the format; false otherwise, including for an unknown alphabet or an nchars that cr_token_new would refuse
- **owns:** nothing
- **thread:** any thread (pure)

```mettle
if (!cr_token_check(header, "mtlr_", CR_TOKEN_BASE62, 40)) { reject(); }
```

### cr_token_bits

```mettle
export fn cr_token_bits(alphabet: int32, nchars: int64) -> int64
```

Entropy of a random part in whole bits, rounded down: base62 nchars * 5954 / 1000 (log2 62 = 5.954), base32 5 * nchars. Any size from 1 to 256 characters can be asked, so callers can compute what a length gives before choosing it.

- **in:** alphabet, one of the CR_TOKEN_* selectors; nchars in 1..256
- **returns:** the bits, or -1 for an unknown alphabet or nchars outside 1..256
- **owns:** nothing
- **thread:** any thread (pure)

```mettle
cr_token_bits(CR_TOKEN_BASE62, 22) == 130
```

### cr_token_hash

```mettle
export fn cr_token_hash(out: CrSha256*, tok: string) -> int32
```

SHA-256 of the whole token text (prefix included), for storing tokens at rest. ONLY for high-entropy tokens (>= 128 bits, see cr_token_bits): a leaked SHA-256 of such a token cannot be brute-forced. Low-entropy codes, such as 10-character recovery codes (~50 bits), must NOT be stored this way: store them with cr_code_hash instead, because a leaked SHA-256 of a ~50-bit code is brute-forceable.

- **in:** out; tok, the token text
- **returns:** CR_OK; CR_E_ARG for a null out. On error out is not written
- **owns:** nothing; the caller owns out
- **thread:** any thread (no shared state)

```mettle
var d: CrSha256; cr_token_hash(&d, tok);  // store d, not tok
```

### cr_code_new

```mettle
export fn cr_code_new(out: uint8*, out_cap: int64, out_len: int64*, nchars: int64, group: int64) -> int32
```

Generates a random human code (a recovery code): nchars characters from the Crockford base32 lowercase alphabet "0123456789abcdefghjkmnpqrstvwxyz" (uniform, no modulo bias), with a '-' between every `group` characters when group > 0, then a nul. For example nchars 10 and group 5 give "xxxxx-yyyyy". 10 or more characters are recommended (50 bits). The code is a secret until used: show it once, store only cr_code_hash of it.

- **in:** out writable for out_cap >= total + 1 bytes, where total = nchars + (nchars - 1) / group dashes (no dashes when group == 0); out_len; nchars in 8..32; group 0 (no dashes) or in 2..16
- **returns:** CR_OK and *out_len = total (the nul is not counted); CR_E_ARG (null out or out_len, nchars outside 8..32, group < 0, group == 1, group > 16); CR_E_SPACE if out_cap < total + 1 (nothing written); CR_E_INTERNAL if the kernel random source fails (out, out_cap bytes, is wiped). On error *out_len = 0
- **owns:** nothing; the caller owns out and must cr_wipe it when done
- **thread:** any thread (no shared state)

```mettle
var c: uint8[16]; var n: int64; cr_code_new(&c[0], 16, &n, 10, 5);  // "k3f9a-7mx2q"
```

### cr_code_normalize

```mettle
export fn cr_code_normalize(out: uint8*, out_cap: int64, out_len: int64*, input: string) -> int32
```

Turns what a person typed into the canonical code: skips '-' and ' ', turns ASCII A-Z into lowercase, then reads i and l as 1 and o as 0 (the Crockford rules); every other byte, u included, must be in the alphabet "0123456789abcdefghjkmnpqrstvwxyz". The result is followed by a nul.

- **in:** out writable for out_cap >= normalized length + 1 bytes; out_len; input, the typed text
- **returns:** CR_OK and *out_len = the normalized length (8..32, the nul is not counted); CR_E_ARG (null out or out_len); CR_E_DECODE for a byte outside the rules or a normalized length outside 8..32; CR_E_SPACE if the normalized length + 1 > out_cap. On CR_E_DECODE and CR_E_SPACE out (out_cap bytes) is wiped; on every error *out_len = 0
- **owns:** nothing; the caller owns out (a code is a secret, wipe it when done)
- **thread:** any thread (no shared state)

```mettle
var c: uint8[40]; var n: int64; cr_code_normalize(&c[0], 40, &n, "ABCDE-I2345");  // "abcde12345"
```

### cr_code_hash

```mettle
export fn cr_code_hash(out: CrHmacSha256*, pepper: CrKey32*, code: string) -> int32
```

Hashes a human code for storage: out = HMAC-SHA256(pepper, "cryptex/code/v1" || normalized code), where the code is first normalized with cr_code_normalize (case, dashes, spaces and look-alike letters do not matter) and the 15-byte context string has no nul. This, not a plain SHA-256, is how recovery codes are stored: a code has only ~50 bits (10 characters), so SHA-256 of it is brute-forceable from a leaked database. The pepper is a 32-byte server secret kept outside the database (config or environment) and never stored next to the tags. Store the tag; look a submitted code up by its tag (an equality lookup is fine, because the tag is useless without the pepper); delete the row once the code is used. 10 or more characters are recommended (50 bits).

- **in:** out; pepper, the 32-byte server secret; code, the code as typed
- **returns:** CR_OK; CR_E_ARG (null out or pepper); CR_E_DECODE (cr_code_normalize refuses the code). On error out is wiped
- **owns:** nothing; the caller owns out and pepper. The local buffer holding the normalized code and the HMAC state are wiped
- **thread:** any thread (no shared state)

```mettle
var tag: CrHmacSha256; cr_code_hash(&tag, &pepper, typed);  // WHERE tag = ?
```

## recipes/seal

Sealed messages, pure Mettle: XChaCha20-Poly1305 (draft-irtf-cfrg-xchacha on RFC 8439) with the nonce carried inside the output, so callers never handle nonces.

### CR_SEAL_OVERHEAD

```mettle
export const CR_SEAL_OVERHEAD: int64 = 40;
```

Bytes a sealed message adds to the plaintext: 24-byte nonce + 16-byte tag.

- **returns:** 40
- **owns:** nothing
- **thread:** constant

```mettle
var cap: int64 = mlen + CR_SEAL_OVERHEAD;
```

### CR_SEAL_VERSION

```mettle
export const CR_SEAL_VERSION: int32 = 1;
```

The format version cr_seal writes into the first byte of the nonce, and the only one cr_open accepts.

- **returns:** 1
- **owns:** nothing
- **thread:** constant

```mettle
if (box[0] != (uint8)CR_SEAL_VERSION) { not_a_sealed_message(); }
```

### cr_seal

```mettle
export fn cr_seal(out: uint8*, out_cap: int64, out_len: int64*, msg: uint8*, mlen: int64, ad: uint8*, adlen: int64, key: CrKey32*) -> int32
```

Encrypts and authenticates msg under key with a fresh nonce (version byte + 23 random bytes), and authenticates (does not encrypt) ad. Output is nonce || ciphertext || tag.

- **in:** out writable for out_cap bytes, out_cap >= mlen + CR_SEAL_OVERHEAD (out must not overlap msg); out_len, where the sealed length goes; msg/mlen the plaintext; ad/adlen associated data, authenticated but not encrypted (0, 0 for none); key
- **returns:** CR_OK and *out_len = mlen + 40; CR_E_ARG (null out/key/out_len, negative length or capacity, null msg with mlen > 0, null ad with adlen > 0); CR_E_SPACE if out_cap is too small (nothing written, *out_len = 0); CR_E_INTERNAL if the kernel random source fails. On every failure *out_len = 0, and once writing has started the first out_cap bytes of out are wiped
- **owns:** nothing; the caller owns every buffer and must wipe key and plaintext when done
- **thread:** any thread (no shared state)

```mettle
cr_seal(&box[0], 128, &blen, m.chars, (int64)m.length, 0, 0, &k);
```

### cr_open

```mettle
export fn cr_open(out: uint8*, out_cap: int64, out_len: int64*, sealed: uint8*, slen: int64, ad: uint8*, adlen: int64, key: CrKey32*) -> int32
```

Verifies and decrypts a message produced by cr_seal with the same key and ad. Fails closed: on every failure *out_len = 0 and out's first out_cap bytes are wiped.

- **in:** out writable for out_cap bytes, out_cap >= slen - CR_SEAL_OVERHEAD (out must not overlap sealed); out_len; sealed/slen the sealed message (nonce || ciphertext || tag); ad/adlen the same associated data given to cr_seal (0, 0 for none); key
- **returns:** CR_OK and *out_len = slen - 40; CR_E_VERIFY if authentication fails (wrong key or ad, tampered nonce, ciphertext or tag, slen < 40, a version other than CR_SEAL_VERSION); CR_E_SPACE if out_cap < slen - 40; CR_E_ARG (null out/key/sealed/ out_len, negative length or capacity, null ad with adlen > 0)
- **owns:** nothing; the caller owns every buffer and must wipe key and the plaintext when done
- **thread:** any thread (no shared state)

```mettle
if (cr_open(&pt[0], 128, &plen, &box[0], blen, 0, 0, &k) != CR_OK) { reject(); }
```

## recipes/totp

Time-based and counter-based one-time passwords for two-factor sign-in: HOTP (RFC 4226), TOTP (RFC 6238), the base32 text people type or scan (RFC 4648) and the otpauth:// provisioning URI that authenticator apps import (as a QR code). Pure Mettle on top of an internal HMAC-SHA1, which is what every authenticator app speaks; plain SHA-1 is not exposed.

### CR_TOTP_SECRET_BYTES

```mettle
export const CR_TOTP_SECRET_BYTES: int64 = 20;
```

the recommended size of an authenticator secret, in bytes (160 bits, the HMAC-SHA1 output size; RFC 4226 requires at least 16). Generate one with cr_random_bytes.

- **returns:** 20
- **owns:** nothing
- **thread:** constant

```mettle
var sec: uint8[20]; cr_random_bytes(&sec[0], CR_TOTP_SECRET_BYTES);
```

### cr_hotp

```mettle
export fn cr_hotp(out: uint32*, secret: uint8*, n: int64, counter: uint64, digits: int32) -> int32
```

Computes an HMAC-based one-time password (HOTP, RFC 4226 section 5): HMAC-SHA1 of the 8-byte big-endian counter under the secret, dynamic truncation, then the result modulo 10^digits.

- **in:** out; secret readable for n bytes, 1 <= n <= 64 (CR_TOTP_SECRET_BYTES recommended; RFC 4226 requires at least 16); counter; digits 6, 7 or 8
- **returns:** CR_OK and *out = the code as a number (format it with leading zeros to `digits` digits); CR_E_ARG for a null pointer, n outside 1..64 or digits outside 6..8 (*out = 0 when out is not null)
- **owns:** nothing; the secret stays the caller's, internal temporaries are wiped
- **thread:** any thread (no shared state)

```mettle
var code: uint32; cr_hotp(&code, &sec[0], 20, counter, 6);
```

### cr_totp

```mettle
export fn cr_totp(out: uint32*, secret: uint8*, n: int64, unix_time: int64, period: int32, digits: int32) -> int32
```

Computes a time-based one-time password (TOTP, RFC 6238 with HMAC-SHA1): the HOTP of the counter floor(unix_time / period).

- **in:** out; secret readable for n bytes, 1 <= n <= 64; unix_time >= 0 (seconds since 1970-01-01 UTC); period 1..300 seconds (30 is what authenticator apps use); digits 6, 7 or 8 (6 is what authenticator apps use)
- **returns:** CR_OK and *out = the code as a number (format it with leading zeros to `digits` digits); CR_E_ARG for any argument outside these ranges or a null pointer (*out = 0 when out is not null)
- **owns:** nothing; internal temporaries are wiped
- **thread:** any thread (no shared state)

```mettle
var code: uint32; cr_totp(&code, &sec[0], 20, now, 30, 6);
```

### cr_totp_verify

```mettle
export fn cr_totp_verify(code: string, secret: uint8*, n: int64, unix_time: int64, period: int32, digits: int32, window: int32, matched_step: int64*) -> bool
```

Checks a code a user typed against the codes for the time steps from window steps before to window steps after the current one (to tolerate clock drift and typing delay). The code for every step in the range is computed and compared with cr_equal, with no early exit, so timing does not reveal which step (if any) matched. REPLAY: a code stays valid for its whole step, so the caller must keep the last accepted step per user and reject a matched step <= that one (store the new step when a code is accepted), or one code can be used twice. Throttle failed attempts too; six digits are guessable.

- **in:** code, exactly `digits` ASCII digits '0'..'9'; secret readable for n bytes, 1 <= n <= 64; unix_time >= 0; period 1..300; digits 6..8; window 0..10 steps on each side; matched_step, where the matching step number (unix_time / period + offset) goes (may be 0)
- **returns:** true if the code matches one of the steps; false otherwise and for any invalid argument or a code that is not `digits` ASCII digits (it fails closed). *matched_step = the step that matched, else -1 (when matched_step is not null)
- **owns:** nothing; internal temporaries are wiped
- **thread:** any thread (no shared state)

```mettle
var step: int64; if (cr_totp_verify(typed, &sec[0], 20, now, 30, 6, 1, &step) && step > last_step) { accept(step); }
```

### cr_totp_secret_text

```mettle
export fn cr_totp_secret_text(out: uint8*, out_cap: int64, out_len: int64*, secret: uint8*, n: int64) -> int32
```

Writes a secret as RFC 4648 base32 (alphabet A-Z 2-7, uppercase, no padding), the text authenticator apps accept and show. Capacity needed: (8n + 4) / 5 + 1 bytes (the text plus a nul).

- **in:** out writable for out_cap bytes; out_len; secret readable for n bytes, n >= 0
- **returns:** CR_OK and *out_len = the text length (a nul follows it); CR_E_ARG (null out or out_len, null secret with n > 0, n < 0 or absurdly large); CR_E_SPACE if out_cap < (8n + 4) / 5 + 1 (nothing written). On error *out_len = 0
- **owns:** nothing; the caller owns out (it holds the secret: wipe it with cr_wipe when done)
- **thread:** any thread (no shared state)

```mettle
var t: uint8[33]; var tl: int64; cr_totp_secret_text(&t[0], 33, &tl, &sec[0], 20);
```

### cr_totp_secret_from_text

```mettle
export fn cr_totp_secret_from_text(out: uint8*, out_cap: int64, out_len: int64*, text: string) -> int32
```

Reads the base32 text people paste or type into a secret: accepts lowercase, skips spaces and '-' (so "jbsw y3dp-ehpk 3pxp" works) and rejects everything else, including '=' padding. The text must be canonical: a last group of 1, 3 or 6 characters, or non-zero trailing bits, is refused. The spaces and '-' are the only characters whose position is treated as public.

- **in:** out writable for out_cap bytes; out_len; text (not necessarily nul terminated)
- **returns:** CR_OK and *out_len = the decoded length (5 bytes per 8 characters); CR_E_DECODE for a bad character, bad group length or non-zero trailing bits; CR_E_SPACE if out_cap is smaller than the decoded length; CR_E_ARG (null out or out_len, null text with length > 0, negative out_cap). On error *out_len = 0 and out's first out_cap bytes are wiped
- **owns:** nothing; the caller owns out (a secret: wipe it with cr_wipe when done)
- **thread:** any thread (no shared state)

```mettle
var sec: uint8[64]; var sl: int64; cr_totp_secret_from_text(&sec[0], 64, &sl, typed);
```

### cr_totp_uri

```mettle
export fn cr_totp_uri(out: uint8*, out_cap: int64, out_len: int64*, issuer: string, account: string, secret_text: string, digits: int32, period: int32) -> int32
```

Builds the otpauth:// URI that authenticator apps import (usually shown as a QR code): otpauth://totp/ISSUER:ACCOUNT?secret=...&issuer=ISSUER& algorithm=SHA1&digits=D&period=P, with issuer and account percent-encoded (every byte except A-Z a-z 0-9 - . _ ~ becomes %XX, uppercase hex). It ends with a nul after the text.

- **in:** out writable for out_cap bytes; out_len; issuer and account, each 1..255 bytes without ':' (the service name, and the user's name or email); secret_text, the secret as base32 A-Z 2-7 (cr_totp_secret_text) and not empty; digits 6..8 (apps only show 6 reliably); period 1..300 (30)
- **returns:** CR_OK and *out_len = the URI length (a nul follows it); CR_E_ARG for a bad label, secret text, digits or period or a null pointer; CR_E_SPACE if out_cap is too small (nothing written). On error *out_len = 0
- **owns:** nothing; the caller owns out (it contains the secret: wipe it with cr_wipe when done)
- **thread:** any thread (no shared state)

```mettle
var u: uint8[512]; var ul: int64; cr_totp_uri(&u[0], 512, &ul, "Mettle", "nox@example.com", st, 6, 30);
```
