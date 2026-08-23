#!/bin/sh
# libmtlc fetcher: get ONLY the backend (headers + static library), no driver.
#
#   curl -fsSL https://raw.githubusercontent.com/suidvandiewereld/Mettle/main/get-libmtlc.sh | sh
#
# Downloads the prebuilt libmtlc release for this platform and unpacks it into
# ./libmtlc (include/ + lib/). That folder is everything a frontend links
# against. No root, no toolchain, no compiler driver.
#
# Environment overrides:
#   LIBMTLC_VERSION   a specific tag (e.g. v0.13.0) instead of latest
#   LIBMTLC_DIR       where to unpack (default: ./libmtlc)
#   LIBMTLC_BASE_URL  a mirror or local test server standing in for GitHub
#
# Flags: --version <tag>, --dir <path>, --help

set -eu

REPO="suidvandiewereld/Mettle"
VERSION="${LIBMTLC_VERSION:-}"
DIR="${LIBMTLC_DIR:-./libmtlc}"

if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
  BOLD="$(printf '\033[1m')"; GREEN="$(printf '\033[32m')"
  YELLOW="$(printf '\033[33m')"; BLUE="$(printf '\033[34m')"; RESET="$(printf '\033[0m')"
else
  BOLD=""; GREEN=""; YELLOW=""; BLUE=""; RESET=""
fi
say()  { printf '%s%s%s\n' "$BLUE" "$1" "$RESET"; }
ok()   { printf '%sok%s %s\n' "$GREEN" "$RESET" "$1"; }
die()  { printf '%serror:%s %s\n' "$YELLOW" "$RESET" "$1" >&2; exit 1; }

usage() {
  cat <<EOF
${BOLD}get-libmtlc${RESET}: download the libmtlc backend (headers + static library)

Usage: get-libmtlc.sh [--version <tag>] [--dir <path>]
Environment: LIBMTLC_VERSION, LIBMTLC_DIR, LIBMTLC_BASE_URL
EOF
}

while [ $# -gt 0 ]; do
  case "$1" in
    --version) VERSION="${2:-}"; shift 2 ;;
    --dir)     DIR="${2:-}"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown option: $1 (try --help)" ;;
  esac
done

need() { command -v "$1" >/dev/null 2>&1; }

os="$(uname -s)"; arch="$(uname -m)"
case "$os" in
  Linux) platform="linux" ;;
  Darwin) die "macOS is not supported yet: libmtlc emits ELF and COFF, not Mach-O." ;;
  *) die "unsupported OS '$os'. libmtlc ships Linux and Windows builds (use get-libmtlc.ps1 on Windows)." ;;
esac
case "$arch" in
  x86_64|amd64) arch="x64" ;;
  *) die "unsupported architecture '$arch'. libmtlc currently targets x86-64." ;;
esac
target="${platform}-${arch}"

if need curl; then
  dl() { curl -fsSL "$1" -o "$2"; }; dl_stdout() { curl -fsSL "$1"; }
elif need wget; then
  dl() { wget -qO "$2" "$1"; }; dl_stdout() { wget -qO - "$1"; }
else
  die "need curl or wget."
fi
need tar || die "need tar to unpack the archive."

if [ -z "$VERSION" ]; then
  say "Finding the latest release..."
  VERSION="$(dl_stdout "https://api.github.com/repos/$REPO/releases/latest" 2>/dev/null \
    | grep -m1 '"tag_name"' \
    | sed 's/.*"tag_name"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/')" || true
  [ -n "$VERSION" ] || die "could not determine the latest release. Set LIBMTLC_VERSION=vX.Y.Z."
fi

bundle="libmtlc-${VERSION}-${target}"
base_url="${LIBMTLC_BASE_URL:-https://github.com/$REPO/releases/download/${VERSION}}"
url="${base_url}/${bundle}.tar.gz"

printf '%sFetching libmtlc %s%s%s for %s%s%s\n' \
  "$BOLD" "$GREEN" "$VERSION" "$RESET$BOLD" "$GREEN" "$target" "$RESET"

tmp="$(mktemp -d "${TMPDIR:-/tmp}/libmtlc.XXXXXX")"
trap 'rm -rf "$tmp"' EXIT INT TERM

say "Downloading $url"
dl "$url" "$tmp/libmtlc.tar.gz" || die "download failed. Does $VERSION ship a $target libmtlc asset? See https://github.com/$REPO/releases."
[ -s "$tmp/libmtlc.tar.gz" ] || die "downloaded archive is empty."

say "Unpacking to $DIR"
tar -xzf "$tmp/libmtlc.tar.gz" -C "$tmp" || die "failed to unpack the archive."
src="$tmp/libmtlc"; [ -d "$src" ] || src="$tmp"
[ -f "$src/lib/libmtlc.a" ] || die "archive did not contain lib/libmtlc.a (unexpected layout)."

rm -rf "$DIR"
mkdir -p "$(dirname "$DIR")"
cp -R "$src" "$DIR"
ok "Installed libmtlc to $DIR"

cat <<EOF

Link a frontend with:
  ${BOLD}cc -I$DIR/include app.c $DIR/lib/libmtlc.a -o app${RESET}
Or, if a pkg-config file shipped:
  ${BOLD}cc \$(PKG_CONFIG_PATH=$DIR/lib/pkgconfig pkg-config --cflags --libs libmtlc) app.c${RESET}

API reference: ${BLUE}https://github.com/$REPO/blob/main/docs/libmtlc/api.md${RESET}
EOF
