# References

## Standards

- **Semantic Versioning 2.0.0**, https://semver.org/spec/v2.0.0.html. The version grammar
  (its Backus-Naur form) and precedence rules (section 11) are implemented exactly. The valid
  and invalid examples and the suggested regular expression linked from the semver.org FAQ are
  the first conformance test (`tests/data/semver_org.txt`), and the regular expression is the
  oracle for generated strict-version cases.
- **npm range syntax**, https://github.com/npm/node-semver#ranges and
  https://docs.npmjs.com/cli/v10/configuring-npm/package-json#dependencies.
- **Cargo requirement syntax**,
  https://doc.rust-lang.org/cargo/reference/specifying-dependencies.html.

## Implementations studied

**node-semver** (https://github.com/npm/node-semver, ISC license), the reference for npm.
Taken from it:
- How each range form becomes plain comparators: caret, tilde, x-range, hyphen and star
  rules, including the `-0` upper bounds that keep pre-releases of the next version out, and
  the `includePrerelease` variants (`classes/range.js`).
- The pre-release rule of `testSet`, set clean-up (null sets, duplicate and `*` removal),
  build metadata stripping, whitespace handling (JavaScript `\s`, including Unicode spaces),
  and the 2^53 - 1 number limit in ranges.
- Loose parsing, `coerce`, `compareBuild` and the `inc` rules for major, minor and patch.
- Its test fixtures, used as data, and node-semver itself as the oracle for about 20,000
  generated cases (`tools/gen_npm.js`).

node-semver works by regular-expression replacement over the range text. This library uses a
single left-to-right scan instead, with no regular expressions and no allocation per
comparator, and was checked to give node-semver's answers on all of the above.

**semver crate** (https://github.com/dtolnay/semver, MIT or Apache-2.0), the reference for
Cargo. Taken from it:
- The matching rules for each operator on partial versions (`src/eval.rs`), ported nearly line
  for line, and the requirement grammar (`src/parse.rs`): default caret, optional spaces after
  operators, wildcards only in place of whole parts, the 32-comparator limit, and 64-bit
  numbers with overflow errors.
- Its Display format for requirements.
- Its tests (`tests/test_version_req.rs`, `tests/test_version.rs`), turned into data by
  `tools/gen_cargo.py`.

**mbuild's semver module** (`mbuild/src/model/semver.mettle`), the earlier Mettle version
this package replaces. Its lenient `[v]MAJOR[.MINOR[.PATCH]]` reading is covered here by
`semver_parse_loose` and `semver_coerce`.

No code was copied from any of these; the ideas and test data are credited above.
