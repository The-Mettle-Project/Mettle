# References

## Standards

- [RFC 9562](https://www.rfc-editor.org/rfc/rfc9562), Universally Unique
  IDentifiers (2024): every layout, the nil and max UUIDs, the v7 counter
  methods (section 6.2), the multicast node rule (section 6.10), sort order
  (section 6.11), and the test vectors in Appendices A and B used in
  `tests/rfc_test.mettle`.
- [RFC 1321](https://www.rfc-editor.org/rfc/rfc1321), MD5, for version 3, and
  [FIPS 180-4](https://csrc.nist.gov/pubs/fips/180-4/upd1/final), SHA-1, for
  version 5. Both come from [cryptex](https://github.com/Lex-Dellimere/Cryptex).
- [ULID spec](https://github.com/ulid/spec) and
  [Crockford's base32](https://www.crockford.com/base32.html).

## Libraries studied

| Library | License | What came from it |
|---|---|---|
| Rust [`uuid`](https://github.com/uuid-rs/uuid) | MIT / Apache-2.0 | Length-dispatched, fixed-offset, table-driven parse and format; the 42-bit v7 counter seeded with 41 random bits and its overflow rule; `from_bytes_le`; the set of text forms and their names |
| Go [`google/uuid`](https://github.com/google/uuid) | BSD-3-Clause | The random byte pool for v4; one shared clock for v1 and v6 that never repeats a tick; the error offsets in parse messages |
| Java [`java.util.UUID`](https://docs.oracle.com/en/java/javase/21/docs/api/java.base/java/util/UUID.html) | GPL-2.0 with classpath exception (API only; no code read or copied) | The two 64-bit halves (`uuid_hi` and `uuid_lo`); its signed compare is noted as a pitfall and not copied |
| Python [`uuid`](https://docs.python.org/3/library/uuid.html) | PSF | Reference answers for the test corpora (`tools/gen_vectors.py`) |
| [oklog/ulid](https://github.com/oklog/ulid) | Apache-2.0 | Strict ULID decoding (no I, L, O, U) and the monotonic increment |

No code was copied from any of them; the algorithms are re-written from the
standards.
