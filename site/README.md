# Mettle landing page

The static site served at GitHub Pages. Plain HTML/CSS with a little vanilla JS —
no build step, no dependencies. Fonts load from Google Fonts; everything else
(the emblem, the favicon, all styling) is inline or in this folder.

```
site/
  index.html                 the landing page
  internals.html             the pipeline tour
  reading-diagnostics.html   anatomy of a diagnostic
  reading-the-report.html    anatomy of the --explain report
  explain/                   GENERATED: one page per diagnostic and decision code
  assets/doc.css             shared styling for everything but index.html
  favicon.svg                tab icon (the emblem on a dark tile)
  README.md                  this file
```

## The generated reference

`site/explain/` is written by
[`tools/gen-site-reference.py`](../tools/gen-site-reference.py) from the help
tables in `src/error/error_explain.c` -- the same text `mettle explain <CODE>`
prints in the terminal. Never edit those pages by hand; edit the table and
regenerate:

```sh
python3 tools/gen-site-reference.py --compiler ./bin/mettle
```

CI runs the same script with `--check` and fails if the committed pages have
drifted from the compiler.

## Deploying

A workflow at [`.github/workflows/pages.yml`](../.github/workflows/pages.yml)
publishes this folder on every push to `main` that touches `site/`.

One-time setup: **Settings → Pages → Source → "GitHub Actions"**. The next push
(or a manual *Run workflow*) deploys it. The site then lives at
`https://suidvandiewereld.github.io/Mettle/`, which is the URL
`--explain` prints next to a decision code.

## Local preview

Serve the folder, so the relative links between pages resolve:

```sh
python3 -m http.server -d site 8080   # http://localhost:8080
```

## Notes for editors

- **Benchmark numbers** are from `docs/benchmarks/` measured against **gcc 13.3 -O3
  under WSL/Linux** (not MinGW, which runs the C binaries slower and overstates the
  gap). Regenerate with `tools/benchmark/run-benchmarks.sh` and keep the figures and
  the "loses on serial/call-heavy code" caveat honest.
- The signature palette is the **steel tempering oxide sequence** (straw → amber →
  ember → plum → steel-blue) — the colours steel turns as it's heated and given its
  mettle. It's the `--temper` gradient; reuse it rather than inventing new accents.
- The install command points at `install.sh` on `main`. Update the URL if the org or
  default branch changes.
