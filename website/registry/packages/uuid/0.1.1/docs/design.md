# Design

## Layout

A `Uuid` is 16 bytes in network order, exactly as RFC 9562 section 4 draws
it. Every other view (two 64-bit halves, GUID fields, Microsoft byte order)
is a conversion, so there is one truth and no hidden byte swaps.

## Parsing

The form is known from the length: 32 simple, 36 hyphenated, 38 braced, 45
URN. Anything else is refused at once. The hyphenated parser checks the four
hyphens at fixed places, then decodes all 32 hex digits through a 256-entry
table at fixed offsets. Bad digits map to 255; the values are ORed together
and checked once at the end, so a valid input runs straight through with no
per-digit branch. Only a failed parse goes back over the input to report the
offset of the first bad byte. This is the plan of Rust's `uuid` crate; it is
also what a SIMD version does, and the loop has the shape a vectorising
compiler wants.

The parser is strict: no surrounding spaces, no braces around the simple
form, no missing hyphens. Python and Go accept a few more shapes; being
strict means every string that parses has exactly one meaning.

## Formatting

The same fixed offsets in reverse, with a 16-character digit table.
`uuid_encode` writes into a buffer you give it and allocates nothing.

## Generators

- **v4** takes random bytes from a 256-byte pool filled by `std/random`
  (the OS), 16 at a time, behind a lock. That is one system call per 16
  UUIDs instead of one each, the idea of Go's `EnableRandPool`. Bytes are
  zeroed once handed out.
- **v7** follows RFC 9562 section 6.2, method 1: a 42-bit counter fills
  rand_a and the top 30 bits of rand_b, and 32 random bits fill the rest.
  Each new millisecond seeds the counter with 41 random bits (the top bit
  clear leaves room to count), so values are unguessable yet strictly
  increasing. If the counter runs out, or the clock steps back, the
  generator keeps the last time and counts on, moving to the next
  millisecond when needed. This is how Rust's `uuid` crate does it.
- **v1 and v6** share one clock. Two calls in the same 100 ns tick, or a
  clock that steps back, get the last tick plus one, so values never repeat
  and v6 never goes down. The node is random with the multicast bit set
  (section 6.10) unless `uuid_set_node` gives a real one; the clock sequence
  is random.
- **v3 and v5** hash the namespace's 16 bytes then the name, take the first
  16 bytes of the digest and set the version and variant. The hashes come
  from cryptex (`cr_md5_*`, `cr_sha1_*`), the ecosystem's crypto package.
- **v8** is your bytes with version and variant set.

## Ordering and hashing

`uuid_cmp` compares as unsigned 128-bit numbers, which equals comparing the
bytes in order and the lower-case hyphenated text in order (RFC 9562
section 6.11). Java compares the two halves as signed numbers, which puts
some v7 values out of time order; this library does not copy that.

`uuid_hash` mixes both halves through MurmurHash3's 64-bit finalizer. It is
for hash tables. It is not keyed, so do not use it where an attacker picks
the UUIDs.

## ULID

Same 128 bits, 48-bit milliseconds then 80 random bits, written in Crockford
base32. The monotonic generator adds one to the whole value inside a
millisecond. Where the spec says to fail when the random part overflows,
this one carries into the time instead (the next millisecond), which keeps
order and never fails; it can only happen after 2^79 ids in one
millisecond on average.

## Speed

`mbuild bench`, release build, on the machine this was written on:

| Operation | Time |
|---|---|
| format hyphenated | ~25 ns |
| parse hyphenated | ~26 ns |
| parse simple | ~24 ns |
| hash | ~15 ns |
| v4 | ~90 ns |
| v6 | ~220 ns |
| v7 | ~300 ns |
| v5, short name | ~380 ns |

v6 and v7 time is mostly reading the wall clock and taking the lock.
