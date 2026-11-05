# Performance

`mtls-bench` runs a client and a server in one process with no sockets,
so it measures mtls and cryptex alone. Release build, AMD Ryzen 7 8845HS,
one core:

| Test | mtls |
|---|---|
| TLS 1.3 handshake, X25519 + Ed25519 certificate | 173 per second (client and server together) |
| TLS 1.3 handshake, X25519 + ECDSA P-256 | 223 per second |
| TLS 1.3 handshake, X25519 + RSA-2048 (PSS) | 160 per second |
| TLS 1.3 handshake, X25519 + ECDSA P-384 | 142 per second |
| TLS 1.3 handshake, P-256 + ECDSA P-256 | 305 per second |
| TLS 1.2 handshake, X25519 + Ed25519 | 171 per second |
| Bulk, TLS_AES_128_GCM_SHA256 | 45 MB/s (encrypt and decrypt) |
| Bulk, TLS_AES_256_GCM_SHA384 | 37 MB/s |
| Bulk, TLS_CHACHA20_POLY1305_SHA256 | 99 MB/s |

For scale, OpenSSL 3.5 (`openssl speed`, hand-written assembly, AES-NI) on
the same machine: X25519 30,868 ops/s, P-256 ECDSA sign 48,879/s and
verify 16,239/s, Ed25519 verify 9,126/s, AES-128-GCM about 10 GB/s,
ChaCha20-Poly1305 about 3.5 GB/s.

Nearly all of the time is in the primitives, which are portable,
constant-time Mettle in cryptex. The protocol code adds little: records
are encrypted and decrypted in place in the network buffers. Faster primitives in cryptex (AES-NI, CLMUL and
vectorized field arithmetic are planned there) speed mtls up directly.
