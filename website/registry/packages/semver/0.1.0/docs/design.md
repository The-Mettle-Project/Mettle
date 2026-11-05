# Design

## Layout

| File | What it holds |
|------|---------------|
| `src/semver.mettle` | Re-exports every module. |
| `src/semver/version.mettle` | `SemverVersion`, `SemverError`, strict and loose parsing, coerce, shared scanners. |
| `src/semver/compare.mettle` | Precedence, `compareBuild` order, formatting, bumps. |
| `src/semver/range.mettle` | Comparators, ranges, matching (the semver crate's rules), best match, sorting, range formatting. |
| `src/semver/npm.mettle` | npm range parser that produces node-semver's comparators. |
| `src/semver/cargo.mettle` | Cargo requirement parser. |

## One matcher for both styles

Cargo keeps comparators as written, with partial versions (`1.2` has two parts), and matches
them with the semver crate's per-operator rules. npm ranges are turned into comparators that
always have three parts. With three parts, the crate's rules reduce to plain precedence, so the
same matcher serves npm too, and the pre-release rule is the same test in both: a pre-release
version needs a comparator in the set with a pre-release on the same `MAJOR.MINOR.PATCH`.

## Memory

A version is a plain struct of three numbers and two string views; parsing never allocates.
A range holds two lists (comparators and the end of each OR set). The npm parser first makes a
tidied copy of the text (spaces collapsed, build metadata dropped), which the range owns, so
its pre-release views stay valid after the caller's text is gone.

## Limits and differences

- Version numbers are 64-bit. SemVer sets no limit, so a version like
  `99999999999999999999.0.0` is valid by the spec but refused here with "number too large".
  node-semver stops at 2^53 - 1 and the semver crate at 2^64 - 1, so this follows the crate.
- npm ranges refuse numbers above 2^53 - 1, as node-semver does, so the same range text is
  accepted by both.
- npm ranges are read in node-semver's default (strict) mode. Its `loose` option for ranges is
  not supported; `semver_parse_loose` covers loose versions.
- Range intersection and node-semver's `gtr`/`ltr`/`outside` are not provided yet.
- Bumps cover major, minor and patch; pre-release bumps are not provided yet.
