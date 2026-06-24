# Mettle landing page

The static site served at GitHub Pages. Plain HTML/CSS with a little vanilla JS —
no build step, no dependencies. Fonts load from Google Fonts; everything else
(the emblem, the favicon, all styling) is inline or in this folder.

```
site/
  index.html     the page
  favicon.svg    tab icon (the emblem on a dark tile)
  README.md      this file
```

## Deploying

A workflow at [`.github/workflows/pages.yml`](../.github/workflows/pages.yml)
publishes this folder on every push to `main` that touches `site/`.

One-time setup: **Settings → Pages → Source → "GitHub Actions"**. The next push
(or a manual *Run workflow*) deploys it. The site then lives at
`https://the-mettle-project.github.io/Mettle/`.

## Local preview

It's a single file — open `index.html` in a browser, or:

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
