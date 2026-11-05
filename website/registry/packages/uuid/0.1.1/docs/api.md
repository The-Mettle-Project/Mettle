# API

`import "uuid/uuid";` brings in everything below.

Strings returned by `uuid_format`, `uuid_to_string` and `uuid_ulid_to_string`
are new heap blocks that belong to the caller: `free(s.chars)` when done. The
`_encode` forms write into your buffer and allocate nothing.

## The type

```mettle
struct Uuid { b: uint8[16]; }   // network order (big-endian)
```

| Function | What it does |
|---|---|
| `uuid_nil() -> Uuid` | all zeros |
| `uuid_max() -> Uuid` | all ones |
| `uuid_is_nil(u) -> bool`, `uuid_is_max(u) -> bool` | |
| `uuid_version(u) -> int32` | high 4 bits of byte 6 |
| `uuid_variant(u) -> int32` | `UUID_VARIANT_NCS`, `_RFC`, `_MICROSOFT` or `_FUTURE` |
| `uuid_set_version(u*, v)` | sets the version and the RFC variant |
| `uuid_cmp(a, b) -> int32` | -1, 0, 1, as unsigned 128-bit numbers |
| `uuid_eq(a, b)`, `uuid_lt(a, b)` | |
| `uuid_hash(u) -> uint64` | well mixed, not keyed |
| `uuid_hash_ptr(p)`, `uuid_equal_ptr(a, b)` | for `std/collections` maps |

## Bytes and fields

| Function | What it does |
|---|---|
| `uuid_from_bytes(p) -> Uuid`, `uuid_to_bytes(u, out)` | 16 bytes, network order |
| `uuid_from_slice(p, n, out*) -> bool` | false unless n is 16 |
| `uuid_from_bytes_le(p)`, `uuid_to_bytes_le(u, out)` | Microsoft GUID memory order |
| `uuid_from_u64(hi, lo)`, `uuid_hi(u)`, `uuid_lo(u)` | Java's most and least significant bits |
| `uuid_from_fields(d1, d2, d3, d4)`, `uuid_to_fields(u, &d1, &d2, &d3, d4)` | 32, 16, 16 bits and 8 bytes |

## Text

Formats: `UUID_FMT_HYPHENATED` (36), `UUID_FMT_SIMPLE` (32), `UUID_FMT_URN` (45),
`UUID_FMT_BRACED` (38).

| Function | What it does |
|---|---|
| `uuid_parse(s) -> Result<Uuid, UuidError>` | any form, any case |
| `uuid_parse_into(s, out*, err*) -> bool` | same, without the Result |
| `uuid_try_parse(s, out*) -> bool` | |
| `uuid_valid(s) -> bool` | |
| `uuid_parse_or_nil(s) -> Uuid` | |
| `uuid_encode(u, format, upper, buf) -> int64` | writes into buf, returns length |
| `uuid_encoded_len(format) -> int64` | |
| `uuid_format(u, format, upper) -> string` | new string |
| `uuid_to_string(u) -> string` | lower-case hyphenated |

`UuidError` has `at` (byte offset) and `why` (a short message).

## Making UUIDs

| Function | Version |
|---|---|
| `uuid_v1()`, `uuid_v1_from(ticks, clock_seq, node)` | 1 |
| `uuid_v3(ns, name)`, `uuid_v3_bytes(ns, p, n)` | 3 |
| `uuid_v4()`, `uuid_v4_from(bytes)`, `uuid_v4_fill(out, n) -> bool` | 4 |
| `uuid_v5(ns, name)`, `uuid_v5_bytes(ns, p, n)` | 5 |
| `uuid_v6()`, `uuid_v6_from(ticks, clock_seq, node)` | 6 |
| `uuid_v7()`, `uuid_v7_at(unix_ms)`, `uuid_v7_from(unix_ms, rand_a, rand_b)` | 7 |
| `uuid_v7_next(gen*, unix_ms)` with `UuidV7Gen` | 7, your own generator |
| `uuid_v8(bytes)` | 8 |

Namespaces: `uuid_ns_dns()`, `uuid_ns_url()`, `uuid_ns_oid()`, `uuid_ns_x500()`.

`uuid_set_node(node)` sets the 6-byte node used by `uuid_v1` and `uuid_v6`
(by default a random one with the multicast bit set). `uuid_now_ticks()` is
the current time in v1 ticks (100 ns since 1582-10-15).

`uuid_v1`, `uuid_v4`, `uuid_v6`, `uuid_v7` and `uuid_ulid` are safe to call
from many threads. `uuid_v7_next` and `uuid_ulid_next` work on the generator
you pass and take no lock.

## Reading fields back

| Function | What it does |
|---|---|
| `uuid_unix_ms(u, out*) -> bool` | for v1, v6 and v7 |
| `uuid_ticks(u, out*) -> bool` | the 60-bit time of v1 and v6 |
| `uuid_clock_seq(u) -> uint16`, `uuid_node(u, out)` | v1 and v6 |
| `uuid_v1_to_v6(u)`, `uuid_v6_to_v1(u)` | same fields, other layout |

## ULID

| Function | What it does |
|---|---|
| `uuid_ulid() -> UuidUlid` | now, monotonic, locked |
| `uuid_ulid_next(gen*, unix_ms)` with `UuidUlidGen` | your own generator |
| `uuid_ulid_from(unix_ms, rand10)` | from parts |
| `uuid_ulid_ms(u)` | its milliseconds |
| `uuid_ulid_encode(u, buf)`, `uuid_ulid_to_string(u)` | 26 characters |
| `uuid_ulid_parse(s)`, `uuid_ulid_parse_into`, `uuid_ulid_try_parse` | any case |
| `uuid_ulid_to_uuid(u)`, `uuid_ulid_from_uuid(x)` | same 16 bytes |
| `uuid_ulid_cmp(a, b)`, `uuid_ulid_eq(a, b)` | |

## Hashes

Versions 3 and 5 use cryptex's `cr_md5_*` and `cr_sha1_*`. Neither hash is
for security.
