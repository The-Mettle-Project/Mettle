#!/bin/sh
#   curl -fsSL https://raw.githubusercontent.com/suidvandiewereld/Mettle/main/install.sh | sh
#
# Or from a checkout:
#   ./install.sh
#
#   METTLE_VERSION=v0.13.0 METTLE_INSTALL_DIR=$HOME/.mettle ./install.sh
#   ./install.sh --version v0.13.0 --dir ~/.mettle --no-modify-path

set -eu
REPO="suidvandiewereld/Mettle"
INSTALL_DIR="${METTLE_INSTALL_DIR:-$HOME/.mettle}"
VERSION="${METTLE_VERSION:-}"
NO_MODIFY_PATH="${METTLE_NO_MODIFY_PATH:-}"
BUILD_TYPE="${METTLE_BUILD_TYPE:-Release}"

say() { printf '%s\n' "$*"; }

die() {
	printf 'error: %s\n' "$*" >&2
	exit 1
}

need() { command -v "$1" >/dev/null 2>&1; }

usage() {
	cat <<EOF
Usage: install.sh [options]
  --version <tag>   git tag/branch (default: current checkout, or origin/main)
  --dir <path>      install prefix (default: ~/.mettle)
  --debug           build CMAKE_BUILD_TYPE=Debug
  --no-modify-path  do not edit shell rc
  -h, --help
EOF
}

while [ $# -gt 0 ]; do
	case "$1" in
	--version)
		VERSION="${2:-}"
		shift 2
		;;
	--dir)
		INSTALL_DIR="${2:-}"
		shift 2
		;;
	--debug)
		BUILD_TYPE=Debug
		shift
		;;
	--no-modify-path)
		NO_MODIFY_PATH=1
		shift
		;;
	-h | --help)
		usage
		exit 0
		;;
	*) die "unknown option: $1" ;;
	esac
done

os="$(uname -s)"
case "$os" in
Linux) ;;
Darwin) die "macOS is not supported (ELF/COFF only)" ;;
MINGW* | MSYS* | CYGWIN*) ;;
*) die "unsupported OS '$os'" ;;
esac

need cmake || die "need cmake"
need git || die "need git"
need cc || need gcc || die "need a C compiler (cc/gcc)"
need ld || die "need ld"
need ar || die "need ar"
need ninja && GEN="-G Ninja" || GEN=""

src=""
cleanup=""

if [ -f "$(dirname "$0")/CMakeLists.txt" ]; then
	src="$(CDPATH= cd -- "$(dirname "$0")" && pwd)"
elif [ -f ./CMakeLists.txt ]; then
	src="$(pwd)"
else
	need git || die "need git to clone $REPO"
	tmp="$(mktemp -d "${TMPDIR:-/tmp}/mettle-src.XXXXXX")"
	cleanup="$tmp"
	trap 'rm -rf "$cleanup"' EXIT INT TERM
	say "Cloning https://github.com/$REPO"
	if [ -n "$VERSION" ]; then
		git clone --depth 1 --branch "$VERSION" "https://github.com/$REPO.git" "$tmp/Mettle"
	else
		git clone --depth 1 "https://github.com/$REPO.git" "$tmp/Mettle"
	fi
	src="$tmp/Mettle"
fi

if [ -n "$VERSION" ] && [ -d "$src/.git" ]; then
	git -C "$src" fetch --tags --depth 1 origin "$VERSION" 2>/dev/null || true
	git -C "$src" checkout "$VERSION"
fi

jobs="$(command -v nproc >/dev/null && nproc || echo 4)"
build="$src/build"

say "Configuring $BUILD_TYPE -> $INSTALL_DIR"

cmake -S "$src" -B "$build" $GEN \
	-DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
	-DCMAKE_INSTALL_PREFIX="$INSTALL_DIR"

say "Building"

cmake --build "$build" --parallel "$jobs"

say "Installing"
cmake --install "$build" --prefix "$INSTALL_DIR"
chmod +x "$INSTALL_DIR/bin/mettle" 2>/dev/null || true
say "Installed $INSTALL_DIR/bin/mettle"

bindir="$INSTALL_DIR/bin"
line="export PATH=\"$bindir:\$PATH\""
on_path() { case ":$PATH:" in *":$bindir:"*) return 0 ;; *) return 1 ;; esac }

if [ -n "$NO_MODIFY_PATH" ]; then
	say "PATH not modified. Add:"
	say "  $line"
elif on_path; then
	say "$bindir already on PATH"
else
	shell_name="$(basename "${SHELL:-/bin/sh}")"
	case "$shell_name" in
	zsh) rc="${ZDOTDIR:-$HOME}/.zshrc" ;;
	bash) [ -f "$HOME/.bashrc" ] && rc="$HOME/.bashrc" || rc="$HOME/.bash_profile" ;;
	*) rc="$HOME/.profile" ;;
	esac
	touch "$rc"
	if ! grep -qF "$bindir" "$rc" 2>/dev/null; then
		printf '\n# Added by the Mettle installer\n%s\n' "$line" >>"$rc"
		say "Added $bindir to PATH in $rc (reload the shell)"
	fi
fi

say "Check: $bindir/mettle --version"
