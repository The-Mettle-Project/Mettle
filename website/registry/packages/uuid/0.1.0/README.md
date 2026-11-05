# uuid

UUIDs for Mettle, in pure Mettle, following
[RFC 9562](https://www.rfc-editor.org/rfc/rfc9562). It needs the standard
library and [cryptex](https://github.com/Lex-Dellimere/Cryptex) (package
`crypto`), whose MD5 and SHA-1 make versions 3 and 5.

- Versions 1, 3, 4, 5, 6, 7 and 8, plus the nil and max UUIDs.
- Version 7 that only ever goes up, even many times in one millisecond.
- Parse and format the hyphenated, simple, URN and braced forms, any case.
- Fast fixed-offset parse and format (about 25 ns each), byte order helpers
  (network order, Microsoft GUID order, Java's high and low 64 bits),
  comparison and hashing.
- ULIDs, with a monotonic generator.

```mettle
import "std/io";
import "std/mem";
import "uuid/uuid";

fn main() -> int32 {
  var id: Uuid = uuid_v7();
  var s: string = uuid_to_string(id);   // "01a1a904-d6df-71b5-920b-0a61d1b9358c"
  println(s);
  free(s.chars);

  match (uuid_parse("urn:uuid:6ba7b810-9dad-11d1-80b4-00c04fd430c8")) {
    case Ok(u): { println("version {uuid_version(u)}"); }
    case Err(e): { println("bad UUID at {e.at}: {e.why}"); }
  }

  var named: Uuid = uuid_v5(uuid_ns_dns(), "www.example.com");
  return 0;
}
```

## Use it

```
mbuild get uuid
```

then `import "uuid/uuid";`. Every exported name starts with `uuid_`, `Uuid` or
`UUID_`.

## Which version?

| Want | Use |
|---|---|
| A database key that sorts by creation time | `uuid_v7()` |
| A random id with nothing in it | `uuid_v4()` |
| The same id every time for the same name | `uuid_v5(ns, name)` (or `uuid_v3` for old systems) |
| Time-ordered with a node id, for old v1 users | `uuid_v6()` |
| Compatibility with existing v1 data | `uuid_v1()` |
| Your own layout | `uuid_v8(bytes)` |
| 26-character, URL-safe, time-ordered | `uuid_ulid()` |

## Build and test

Until the package registry is live, uuid finds cryptex by path, so clone
[Cryptex](https://github.com/Lex-Dellimere/Cryptex) next to this repo (as
`../cryptex`).

```
mbuild test        # debug
mbuild test -r     # release
mbuild bench       # timings
mbuild run ids     # one of each
```

The tests use RFC 9562's own examples (Appendices A and B), 800 v3/v5 values
and 500 parsed values checked against Python's `uuid` module, 300 ULIDs
checked against a reference encoder, ordering checks on hundreds of
thousands of generated values, and a fuzz run of 200,000 random and broken
inputs.

## Docs

- [docs/api.md](docs/api.md): every function.
- [docs/design.md](docs/design.md): how it works and why.
- [docs/references.md](docs/references.md): standards and the libraries studied.

## License

Apache License 2.0. See [LICENSE](LICENSE).
