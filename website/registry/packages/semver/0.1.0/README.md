# semver

Semantic Versioning 2.0.0 for Mettle, written in pure Mettle.

- Parse, compare and format versions exactly as [semver.org](https://semver.org/spec/v2.0.0.html)
  defines them, including pre-release and build metadata.
- npm ranges (`^1.2.3`, `~1.2`, `1.x`, `1.2.3 - 2.3.4`, `>=1 <2 || 3`) with the same meaning
  node-semver gives them, down to its `-0` bounds and its pre-release rule.
- Cargo requirements (`1.2`, `~1.2.3`, `=1.2`, `1.*`, `>=1.2, <1.5`) with the meaning the
  semver crate gives them.
- Pick the best (or lowest) version from a list, sort version lists, bump versions.
- Loose parsing and coercion for messy input, as in node-semver.

No allocation for versions: a parsed version is five fields that point into your text.

## Use

```bash
mbuild get semver
```

```mettle
import "std/io";
import "semver/semver";

fn main() -> int32 {
  match (semver_parse("1.4.2-rc.1+build.7")) {
    case Ok(v): { println("{v.major} {v.minor} {v.patch} pre={v.pre} build={v.build}"); }
    case Err(e): { println("bad version at {e.at}: {e.why}"); }
  }

  println("{semver_npm_satisfies(\"1.5.0\", \"^1.2 || 2.x\")}");   // true
  println("{semver_cargo_matches(\"1.3.0\", \"~1.2\")}");          // false

  match (semver_npm_parse(">=1.2.7 <1.3.0", false)) {
    case Ok(r): {
      var range: SemverRange = r;
      var list: SemverVersion[3];
      list[0] = semver_new(1, 2, 6);
      list[1] = semver_new(1, 2, 8);
      list[2] = semver_new(1, 3, 0);
      var best: int64 = semver_max_satisfying(&range, &list[0], 3); // 1, which is 1.2.8
      semver_range_free(&range);
    }
    case Err(e): { }
  }
  return 0;
}
```

`examples/pick.mettle` is a small command-line tool: `mbuild run pick -- npm "^1.2" 1.1.0 1.4.2`.

The whole API is in [docs/api.md](docs/api.md). How it works and where it differs from the
references is in [docs/design.md](docs/design.md), and the sources it learned from are in
[docs/references.md](docs/references.md).

## Tested against

- Every valid and invalid example linked from semver.org, and its precedence examples.
- node-semver's own fixtures, plus about 20,000 generated cases answered by node-semver
  (range text, matching, max and min satisfying, compare, loose, coerce, bumps).
- Every case in the semver crate's `test_version_req.rs` and `test_version.rs`.
- A fuzz test: 23,000 mutated and random inputs through every parser; nothing may crash,
  errors must point into the input, and formatted output must parse back the same.

```bash
mbuild test
mbuild test -r
```

## Speed

`mbuild bench` against `node tools/bench_node.js` (node-semver 7.8.6), one core, nanoseconds per call:

| Operation | semver (Mettle) | node-semver |
|-----------|-----:|-----:|
| parse a version | 44 | 398 |
| compare two versions | 10 | 10 |
| parse an npm range | 388 | 5842 |
| range satisfies version | 20 | 157 |
| parse a Cargo requirement | 157 | - |

## License

Apache-2.0. See [LICENSE](LICENSE).
