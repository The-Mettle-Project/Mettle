#!/bin/sh
# Fetch the libmtlc backend that Mettle compiles against (Linux, macOS).
#
#   ./get-libmtlc.sh
#
# Mettle is a frontend: it lexes, parses, type-checks and lowers .mettle source
# into libmtlc's IR. Everything after that -- optimization, code generation,
# linking -- is libmtlc (https://github.com/The-Mettle-Project/libmtlc). This
# script downloads the libmtlc source at the revision pinned in libmtlc.version,
# unpacks it into ./libmtlc, and builds the static archive the driver links.
#
# The download is a source checkout, not a release bundle: the driver uses the
# backend's own headers, so headers and archive must come from one revision. A
# prebuilt release ships only the public mtlc/ API, which is the surface for a
# foreign frontend rather than for Mettle's own driver.
#
# Overrides:
#   LIBMTLC_VERSION   a tag, branch or commit SHA instead of libmtlc.version
#   LIBMTLC_DIR       where to unpack (default ./libmtlc)
#   LIBMTLC_SKIP_BUILD=1   download only; do not build the archive
#   LIBMTLC_FORCE=1        re-download even if the pinned revision is present
set -eu

REPO="The-Mettle-Project/libmtlc"
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DIR=${LIBMTLC_DIR:-"$ROOT/libmtlc"}
STAMP="$DIR/.libmtlc-revision"

say()  { printf '\033[34m%s\033[0m\n' "$1"; }
ok()   { printf '\033[32mok %s\033[0m\n' "$1"; }
warn() { printf '\033[33mwarning: %s\033[0m\n' "$1" >&2; }
die()  { printf '\033[31merror: %s\033[0m\n' "$1" >&2; exit 1; }

# --- the pinned revision -----------------------------------------------------
VERSION=${LIBMTLC_VERSION:-}
if [ -z "$VERSION" ] && [ -f "$ROOT/libmtlc.version" ]; then
  VERSION=$(sed -e 's/#.*//' -e 's/[[:space:]]//g' "$ROOT/libmtlc.version" \
            | grep -v '^$' | head -n 1 || true)
fi
[ -n "$VERSION" ] || VERSION=main

printf 'libmtlc %s\n' "$VERSION"

# --- download ----------------------------------------------------------------
# A local git checkout is left completely alone: that is the point of
# LIBMTLC_DIR when you are working on both halves at once.
if [ -d "$DIR/.git" ] && [ ! -f "$STAMP" ]; then
  say "$DIR is a git checkout; leaving it untouched"
elif [ -f "$STAMP" ] && [ "${LIBMTLC_FORCE:-0}" != "1" ] &&
     [ "$(tr -d ' \t\r\n' < "$STAMP")" = "$VERSION" ]; then
  say "$DIR is already at $VERSION (set LIBMTLC_FORCE=1 to re-download)"
else
  URL="https://codeload.github.com/$REPO/tar.gz/$VERSION"
  say "Downloading $URL"
  TMP=$(mktemp -d)
  trap 'rm -rf "$TMP"' EXIT INT TERM

  if command -v curl >/dev/null 2>&1; then
    curl -fsSL "$URL" -o "$TMP/libmtlc.tar.gz" ||
      die "download failed. Does $REPO have a revision '$VERSION'? See https://github.com/$REPO"
  elif command -v wget >/dev/null 2>&1; then
    wget -qO "$TMP/libmtlc.tar.gz" "$URL" ||
      die "download failed. Does $REPO have a revision '$VERSION'? See https://github.com/$REPO"
  else
    die "need curl or wget to download libmtlc."
  fi

  say "Unpacking to $DIR"
  mkdir -p "$TMP/x"
  tar -xzf "$TMP/libmtlc.tar.gz" -C "$TMP/x"
  UNPACKED=$(find "$TMP/x" -mindepth 1 -maxdepth 1 -type d | head -n 1)
  [ -n "$UNPACKED" ] || die "the archive was empty."
  for probe in include/mtlc/mtlc.h src/ir/ir.h Makefile; do
    [ -f "$UNPACKED/$probe" ] || die "the archive is missing $probe (unexpected layout)."
  done

  rm -rf "$DIR"
  mkdir -p "$(dirname "$DIR")"
  mv "$UNPACKED" "$DIR"
  printf '%s\n' "$VERSION" > "$STAMP"
  ok "unpacked libmtlc $VERSION into $DIR"
fi

# --- build the archive -------------------------------------------------------
# libmtlc's own Makefile owns the object list, so the backend is described in
# exactly one place. Its `libmtlc` target builds the archive alone, without
# libmtlc's copy of the driver.
if [ "${LIBMTLC_SKIP_BUILD:-0}" = "1" ]; then
  say "skipping the build (LIBMTLC_SKIP_BUILD=1)"
  exit 0
fi

for candidate in lib/libmtlc.a lib/mtlc.lib bin/libmtlc.a bin/mtlc.lib; do
  if [ -f "$DIR/$candidate" ] && [ "${LIBMTLC_FORCE:-0}" != "1" ]; then
    ok "using the archive already present at $DIR/$candidate"
    exit 0
  fi
done

[ -f "$DIR/Makefile" ] || die "$DIR/Makefile not found; cannot build the backend."
command -v make >/dev/null 2>&1 || die "make not found; install it to build libmtlc."

# Build the archive with the same compiler and flags the driver will use.
# libmtlc's Makefile hard-assigns CC, so this has to come in on the command
# line to take effect. LIBMTLC_EXTRA_CFLAGS is how a sanitizer build gets the
# backend instrumented too -- without it, most of the compiler is uncovered.
say "Building the backend archive (this takes a few minutes)"
if [ -n "${LIBMTLC_EXTRA_CFLAGS:-}" ]; then
  set -- libmtlc ${CC:+CC="$CC"} EXTRA_CFLAGS="$LIBMTLC_EXTRA_CFLAGS"
else
  set -- libmtlc ${CC:+CC="$CC"}
fi
if ! make -C "$DIR" "$@"; then
  warn "the 'libmtlc' target failed; trying a full libmtlc build"
  shift
  make -C "$DIR" "$@" || die "building libmtlc failed."
fi

[ -f "$DIR/bin/libmtlc.a" ] || die "the build finished but $DIR/bin/libmtlc.a is missing."
ok "built $DIR/bin/libmtlc.a"
echo
echo "Now build Mettle:"
echo "  make"
