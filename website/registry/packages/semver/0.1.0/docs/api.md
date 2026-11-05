# API

`import "semver/semver";` brings in everything. Every name starts with `semver_`, `Semver` or `SEMVER_`.

Strings in Mettle are views. A parsed version keeps views into the text it was parsed from,
so keep that text alive while you use the version. Ranges from `semver_npm_parse` keep their own
copy; ranges from `semver_cargo_parse` point into your text. Free every range with
`semver_range_free`. Strings returned by `semver_format` and friends are yours.

## Types

```mettle
struct SemverVersion {
  major: uint64; minor: uint64; patch: uint64;
  pre: string;    // pre-release without the '-', or ""
  build: string;  // build metadata without the '+', or ""
}

struct SemverError { at: int64; why: string; }  // byte offset in the input, short reason

struct SemverComparator { op: int32; parts: int32; major: uint64; minor: uint64; patch: uint64; pre: string; }
struct SemverRange { cmps: List<SemverComparator>; ends: List<int64>; style: int32; include_pre: bool; owned: uint8*; }
```

Comparator operators: `SEMVER_EQ`, `SEMVER_GT`, `SEMVER_GE`, `SEMVER_LT`, `SEMVER_LE`,
`SEMVER_TILDE`, `SEMVER_CARET`, `SEMVER_WILD` (Cargo `1.*`), `SEMVER_ANY` (`*`).
`parts` says how many of major, minor and patch were written (Cargo keeps partial versions).
Range styles: `SEMVER_STYLE_NPM`, `SEMVER_STYLE_CARGO`.

A range is sets joined by OR, each set a list of comparators joined by AND. Set `k` holds
comparators `semver_range_set_start(&r, k)` up to `semver_range_set_end(&r, k)`;
`semver_range_sets(&r)` counts sets and `semver_range_cmp(&r, i)` reads one comparator.

## Versions

| Function | What it does |
|----------|--------------|
| `semver_parse(s) -> Result<SemverVersion, SemverError>` | Strict SemVer 2.0.0. No `v`, no spaces, no leading zeros. Numbers up to 2^64 - 1. |
| `semver_valid(s) -> bool` | `semver_parse` succeeded. |
| `semver_parse_loose(s) -> Result<...>` | node-semver `{ loose: true }`: spaces, `v` and `=` prefixes, leading zeros, `1.2.3beta`. |
| `semver_coerce(s) -> Option<SemverVersion>` | node-semver `coerce`: the first version-like run in any text (`"v2"` gives 2.0.0). |
| `semver_new(major, minor, patch) -> SemverVersion` | A release version. |
| `semver_format(v) -> string` | `MAJOR.MINOR.PATCH[-PRE][+BUILD]`. |
| `semver_format_nobuild(v) -> string` | Without build metadata. |

## Comparing

| Function | What it does |
|----------|--------------|
| `semver_cmp(a, b) -> int32` | Precedence: -1, 0 or 1. Build metadata is ignored. |
| `semver_cmp_build(a, b) -> int32` | Precedence, then build metadata (node-semver `compareBuild`), for a total order. |
| `semver_eq`, `semver_lt`, `semver_gt`, `semver_le`, `semver_ge` | Precedence tests. |
| `semver_is_pre(v) -> bool` | Has a pre-release. |
| `semver_cmp_pre(a, b)`, `semver_cmp_idents(a, b)`, `semver_cmp_ident(a, b)` | The pieces of precedence, on raw text. |
| `semver_sort(list, n)`, `semver_sort_desc(list, n)` | Stable sort by `semver_cmp_build`. |
| `semver_max(list, n, include_pre) -> int64` | Index of the highest version, releases first unless `include_pre`; -1 if empty. |

## Bumps

`semver_bump_major`, `semver_bump_minor`, `semver_bump_patch` follow node-semver's `inc`:
`1.2.3` bumps to `2.0.0`, `1.3.0`, `1.2.4`, and a pre-release of the target becomes that
release (`2.0.0-rc.1` major bumps to `2.0.0`). Pre-release and build are dropped.

## Ranges

| Function | What it does |
|----------|--------------|
| `semver_npm_parse(text, include_pre) -> Result<SemverRange, SemverError>` | npm range syntax, read as node-semver reads it. `include_pre` is node's `includePrerelease`. |
| `semver_cargo_parse(text) -> Result<SemverRange, SemverError>` | Cargo requirement syntax, as the semver crate reads it. At most 32 comparators. |
| `semver_satisfies(&r, v) -> bool` | `v` is in the range. |
| `semver_max_satisfying(&r, list, n) -> int64` | Index of the highest matching version, or -1. |
| `semver_min_satisfying(&r, list, n) -> int64` | Index of the lowest matching version, or -1. |
| `semver_range_format(&r) -> string` | npm: node-semver's `range.range` (`>=1.2.3 <2.0.0-0`). Cargo: the crate's Display (`^1.2, <1.5`). |
| `semver_cmp_test(&c, &v) -> bool` | One comparator, without the pre-release rule. |
| `semver_npm_satisfies(version, range) -> bool` | Both as text; false if either does not parse. |
| `semver_cargo_matches(version, req) -> bool` | Both as text; false if either does not parse. |
| `semver_range_free(&r)` | Release a range. |

The pre-release rule (both styles): a version with a pre-release only matches a set that has
a comparator naming a pre-release of the same `MAJOR.MINOR.PATCH`. So `^1.2.3` does not take
`1.3.0-beta`, but `>=1.3.0-alpha <2` takes `1.3.0-beta`. npm's `include_pre` turns the rule off.
