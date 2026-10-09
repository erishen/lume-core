#!/usr/bin/env sh
# Lume-core — one-command installer.
#
#   curl -sSfL https://raw.githubusercontent.com/erishen/lume-core/main/install.sh | sh
#
# Downloads the prebuilt release tarball for your platform from GitHub
# Releases and installs it: the binary lands in ~/.local/bin/lume-core, and the
# examples + docs in ~/.local/share/lume-core so `lume-core examples/hello.lume`
# has its example scripts on hand. Needs curl or wget.
#
# Overrides (set before piping, e.g. `... | LUME_CORE_VERSION=v0.5.0 sh`):
#   LUME_CORE_VERSION  version tag to install (default: latest)
#   LUME_CORE_PREFIX   install root (default: $HOME/.local; binary goes to $PREFIX/bin)
#   LUME_CORE_SHA256   expected sha256 of the tarball, verified when set
#   LUME_CORE_REPO     GitHub repo to fetch from (default: erishen/lume-core)
set -eu

REPO="${LUME_CORE_REPO:-erishen/lume-core}"
VERSION="${LUME_CORE_VERSION:-latest}"
PREFIX="${LUME_CORE_PREFIX:-$HOME/.local}"
BINDIR="$PREFIX/bin"

# --- platform detection -----------------------------------------------------
case "$(uname -s)" in
  Linux)  os=linux ;;
  Darwin) os=darwin ;;
  *)
    echo "lume-core-install: unsupported OS: $(uname -s)" >&2
    exit 1
    ;;
esac

case "$(uname -m)" in
  x86_64|amd64)  arch=x64 ;;
  arm64|aarch64) arch=arm64 ;;
  *)
    echo "lume-core-install: unsupported architecture: $(uname -m)" >&2
    exit 1
    ;;
esac

asset="lume-core-$os-$arch.tar.gz"
if [ "$VERSION" = "latest" ]; then
  url="https://github.com/$REPO/releases/latest/download/$asset"
else
  url="https://github.com/$REPO/releases/download/$VERSION/$asset"
fi

# --- download ---------------------------------------------------------------
mkdir -p "$BINDIR"
tmp="$BINDIR/.lume-core.tmp.$$"
trap 'rm -f "$tmp"; rm -rf "$BINDIR/.lume-core.stage.$$"' EXIT INT TERM

echo "==> lume-core-install: downloading $asset"
if command -v curl >/dev/null 2>&1; then
  curl -sSfL "$url" -o "$tmp"
elif command -v wget >/dev/null 2>&1; then
  wget -qO "$tmp" "$url"
else
  echo "lume-core-install: need curl or wget" >&2
  exit 1
fi

# --- optional checksum ------------------------------------------------------
if [ -n "${LUME_CORE_SHA256:-}" ]; then
  if command -v sha256sum >/dev/null 2>&1; then
    actual=$(sha256sum "$tmp" | awk '{print $1}')
  elif command -v shasum >/dev/null 2>&1; then
    actual=$(shasum -a 256 "$tmp" | awk '{print $1}')
  else
    echo "lume-core-install: LUME_CORE_SHA256 set but no sha256sum/shasum found" >&2
    exit 1
  fi
  if [ "$actual" != "$LUME_CORE_SHA256" ]; then
    echo "lume-core-install: checksum mismatch" >&2
    echo "  expected: $LUME_CORE_SHA256" >&2
    echo "  actual:   $actual" >&2
    exit 1
  fi
  echo "==> lume-core-install: checksum ok"
fi

# --- install ----------------------------------------------------------------
if ! command -v tar >/dev/null 2>&1; then
  echo "lume-core-install: need tar to unpack the release" >&2
  exit 1
fi
stage="$BINDIR/.lume-core.stage.$$"
mkdir -p "$stage"
tar -xzf "$tmp" -C "$stage" || { echo "lume-core-install: corrupt download" >&2; exit 1; }

# The release tarball wraps everything in a lume-core-<os>-<arch>/ top dir;
# `make pack` produces a flat layout (bin/lume-core at the root). Accept both.
binpath=$(find "$stage" -type f -name lume-core -path '*/bin/*' | head -1)
[ -n "$binpath" ] || binpath=$(find "$stage" -type f -name lume-core | head -1)
[ -n "$binpath" ] || { echo "lume-core-install: tarball layout unexpected" >&2; exit 1; }

chmod +x "$binpath"
mv "$binpath" "$BINDIR/lume-core"

# Carry the examples + docs along so an installed binary can run the bundled
# scripts out of the box (lume-core has no server, so this is just files).
toplevel=$(dirname "$binpath")
if [ -d "$toplevel/examples" ] || [ -d "$toplevel/docs" ] || \
   [ -f "$toplevel/README.md" ] || [ -f "$toplevel/CHANGELOG.md" ]; then
  sharedir="$PREFIX/share/lume-core"
  mkdir -p "$sharedir"
  cp -R "$toplevel/examples" "$toplevel/docs" \
        "$toplevel/README.md" "$toplevel/README.zh.md" "$toplevel/CHANGELOG.md" \
        "$sharedir/" 2>/dev/null || true
  echo "==> lume-core-install: examples + docs in $sharedir"
fi
rm -rf "$stage"
echo "==> lume-core-install: installed $BINDIR/lume-core"
echo "    run: $BINDIR/lume-core examples/hello.lume   (or cd $sharedir)"

case ":$PATH:" in
  *":$BINDIR:"*) ;;
  *) echo "    add to PATH: export PATH=\"$BINDIR:\$PATH\"" ;;
esac
