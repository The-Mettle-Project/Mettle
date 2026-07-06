#!/bin/sh
# Mettle installer.
#
#   curl -fsSL https://raw.githubusercontent.com/The-Mettle-Project/Mettle/main/install.sh | sh
#
# Downloads the latest Mettle release for this platform, installs it under
# ~/.mettle, and adds the compiler to your PATH. No root required.
#
# Environment overrides:
#   METTLE_VERSION      install a specific tag (e.g. v0.13.0) instead of latest
#   METTLE_INSTALL_DIR  install location (default: ~/.mettle)
#   METTLE_NO_MODIFY_PATH=1  install but don't touch shell rc files
#
# Flags: --version <tag>, --dir <path>, --no-modify-path, --help

set -eu

REPO="The-Mettle-Project/Mettle"
INSTALL_DIR="${METTLE_INSTALL_DIR:-$HOME/.mettle}"
VERSION="${METTLE_VERSION:-}"
NO_MODIFY_PATH="${METTLE_NO_MODIFY_PATH:-}"

# --- pretty output (only colorize an interactive terminal) ------------------
if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
  BOLD="$(printf '\033[1m')"; DIM="$(printf '\033[2m')"
  RED="$(printf '\033[31m')"; GREEN="$(printf '\033[32m')"
  YELLOW="$(printf '\033[33m')"; BLUE="$(printf '\033[34m')"
  RESET="$(printf '\033[0m')"
else
  BOLD=""; DIM=""; RED=""; GREEN=""; YELLOW=""; BLUE=""; RESET=""
fi

say()  { printf '%s%s%s\n' "$BLUE" "$1" "$RESET"; }
ok()   { printf '%s✓%s %s\n' "$GREEN" "$RESET" "$1"; }
warn() { printf '%s!%s %s\n' "$YELLOW" "$RESET" "$1" >&2; }
err()  { printf '%serror:%s %s\n' "$RED" "$RESET" "$1" >&2; }
die()  { err "$1"; exit 1; }

usage() {
  cat <<EOF
${BOLD}Mettle installer${RESET}

Usage: install.sh [options]

Options:
  --version <tag>     Install a specific release (default: latest)
  --dir <path>        Install location (default: ~/.mettle)
  --no-modify-path    Don't add Mettle to your shell PATH
  -h, --help          Show this help

Environment: METTLE_VERSION, METTLE_INSTALL_DIR, METTLE_NO_MODIFY_PATH
EOF
}

while [ $# -gt 0 ]; do
  case "$1" in
    --version) VERSION="${2:-}"; shift 2 ;;
    --dir)     INSTALL_DIR="${2:-}"; shift 2 ;;
    --no-modify-path) NO_MODIFY_PATH=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown option: $1 (try --help)" ;;
  esac
done

need() { command -v "$1" >/dev/null 2>&1; }

# --- detect platform --------------------------------------------------------
os="$(uname -s)"
arch="$(uname -m)"

case "$os" in
  Linux) platform="linux" ;;
  Darwin)
    die "macOS is not supported yet: Mettle's native backend emits ELF (Linux) and COFF (Windows), not Mach-O. Follow https://github.com/$REPO for macOS progress."
    ;;
  *) die "unsupported OS '$os'. Mettle ships Linux and Windows builds." ;;
esac

case "$arch" in
  x86_64|amd64) arch="x64" ;;
  *) die "unsupported architecture '$arch'. Mettle currently targets x86-64." ;;
esac

target="${platform}-${arch}"

# --- pick a downloader ------------------------------------------------------
if need curl; then
  dl() { curl -fsSL "$1" -o "$2"; }
  dl_stdout() { curl -fsSL "$1"; }
elif need wget; then
  dl() { wget -qO "$2" "$1"; }
  dl_stdout() { wget -qO - "$1"; }
else
  die "need curl or wget to download Mettle."
fi

need tar || die "need tar to unpack the Mettle release."

# --- resolve version --------------------------------------------------------
if [ -z "$VERSION" ]; then
  say "Finding the latest Mettle release..."
  api="https://api.github.com/repos/$REPO/releases/latest"
  # Pull "tag_name": "vX.Y.Z" out of the JSON without needing jq.
  VERSION="$(dl_stdout "$api" 2>/dev/null \
    | grep -m1 '"tag_name"' \
    | sed 's/.*"tag_name"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/')" || true
  [ -n "$VERSION" ] || die "could not determine the latest release. Set METTLE_VERSION=vX.Y.Z and retry, or check https://github.com/$REPO/releases."
fi

bundle="mettle-${VERSION}-${target}"
# METTLE_BASE_URL lets a mirror or a local test server stand in for GitHub.
base_url="${METTLE_BASE_URL:-https://github.com/$REPO/releases/download/${VERSION}}"
url="${base_url}/${bundle}.tar.gz"

printf '%sInstalling Mettle %s%s%s for %s%s%s\n' \
  "$BOLD" "$GREEN" "$VERSION" "$RESET$BOLD" "$GREEN" "$target" "$RESET"

# --- download + extract -----------------------------------------------------
tmp="$(mktemp -d "${TMPDIR:-/tmp}/mettle-install.XXXXXX")"
trap 'rm -rf "$tmp"' EXIT INT TERM

say "Downloading $url"
if ! dl "$url" "$tmp/mettle.tar.gz"; then
  die "download failed. Does $VERSION ship a $target build? See https://github.com/$REPO/releases."
fi
[ -s "$tmp/mettle.tar.gz" ] || die "downloaded archive is empty."

say "Unpacking..."
tar -xzf "$tmp/mettle.tar.gz" -C "$tmp" || die "failed to unpack the archive."

# The tarball contains a top-level <bundle>/ directory.
src="$tmp/$bundle"
[ -d "$src" ] || src="$tmp"
[ -f "$src/bin/mettle" ] || die "archive did not contain bin/mettle (unexpected layout)."

# --- install (replace any prior install atomically-ish) ---------------------
say "Installing to $INSTALL_DIR"
rm -rf "$INSTALL_DIR.old"
[ -d "$INSTALL_DIR" ] && mv "$INSTALL_DIR" "$INSTALL_DIR.old"
mkdir -p "$INSTALL_DIR"
# Copy bin/, stdlib/, runtime/, and any docs/license that shipped.
cp -R "$src/." "$INSTALL_DIR/"
chmod +x "$INSTALL_DIR/bin/mettle"
rm -rf "$INSTALL_DIR.old"
ok "Installed mettle to $INSTALL_DIR/bin/mettle"

# --- PATH -------------------------------------------------------------------
bindir="$INSTALL_DIR/bin"
add_path_line="export PATH=\"$bindir:\$PATH\""

already_on_path() {
  case ":$PATH:" in *":$bindir:"*) return 0 ;; *) return 1 ;; esac
}

rc_for_shell() {
  # Choose the rc file for the user's login shell.
  shell_name="$(basename "${SHELL:-/bin/sh}")"
  case "$shell_name" in
    zsh)  printf '%s\n' "${ZDOTDIR:-$HOME}/.zshrc" ;;
    bash) [ -f "$HOME/.bashrc" ] && printf '%s\n' "$HOME/.bashrc" || printf '%s\n' "$HOME/.bash_profile" ;;
    *)    printf '%s\n' "$HOME/.profile" ;;
  esac
}

if [ -n "$NO_MODIFY_PATH" ]; then
  warn "Skipping PATH update (--no-modify-path). Add this to your shell yourself:"
  printf '    %s\n' "$add_path_line"
elif already_on_path; then
  ok "$bindir is already on your PATH"
else
  rc="$(rc_for_shell)"
  touch "$rc"
  if ! grep -qF "$bindir" "$rc" 2>/dev/null; then
    {
      printf '\n# Added by the Mettle installer\n'
      printf '%s\n' "$add_path_line"
    } >> "$rc"
    ok "Added $bindir to PATH in $rc"
    PATH_NEEDS_RELOAD=1
  else
    ok "PATH entry already present in $rc"
  fi
fi

# --- toolchain check (detect + guide; do not auto-install) ------------------
printf '\n%sChecking the C toolchain Mettle links with...%s\n' "$DIM" "$RESET"
missing=""
need ld || missing="ld"
need cc || need gcc || missing="${missing:+$missing }cc/gcc"

if [ -n "$missing" ]; then
  warn "Missing: $missing"
  printf '  Mettle compiles to native code but links the final executable with the\n'
  printf '  system linker. Install a C toolchain:\n'
  if need apt-get; then
    printf '    %ssudo apt-get install build-essential%s\n' "$BOLD" "$RESET"
  elif need dnf; then
    printf '    %ssudo dnf groupinstall "Development Tools"%s\n' "$BOLD" "$RESET"
  elif need pacman; then
    printf '    %ssudo pacman -S base-devel%s\n' "$BOLD" "$RESET"
  elif need apk; then
    printf '    %ssudo apk add build-base%s\n' "$BOLD" "$RESET"
  else
    printf '    install gcc/binutils via your package manager\n'
  fi
else
  ok "Found ld and a C compiler"
fi

# --- done -------------------------------------------------------------------
printf '\n%sMettle %s is installed.%s\n' "$GREEN$BOLD" "$VERSION" "$RESET"
if [ "${PATH_NEEDS_RELOAD:-}" = "1" ]; then
  printf 'Open a new terminal (or run %ssource %s%s), then:\n' "$BOLD" "${rc:-your shell rc}" "$RESET"
else
  printf 'Get started:\n'
fi
cat <<EOF
    ${BOLD}mettle --version${RESET}
    ${BOLD}echo 'fn main() -> int32 { return 0; }' > hello.mettle${RESET}
    ${BOLD}mettle --build hello.mettle -o hello && ./hello${RESET}

Docs: ${BLUE}https://github.com/$REPO${RESET}
EOF
