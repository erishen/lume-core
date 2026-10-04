#!/usr/bin/env bash
#
# Guard the published tarball against build-machine paths.
#
#   make pack  calls this on the tarball it just built; `make packcheck`
#   runs it against whatever is already in dist/.
#
# Two things bake the build machine into bin/lume-core, and they need
# different treatment:
#
#   - CFLAGS keeps -g, so DWARF records the compile-time working directory.
#     `strip -u -r` removes it.
#   - Makefile:17 passes -DLUME_RT_SRC="$(CURDIR)/src/rt.c", so the absolute
#     path also lands in .rodata as a *string literal*. Strip does not touch
#     literals, which is why `pack` relinks with a package-relative value
#     first (PACK_RT_DEFS in the Makefile).
#
# So this check is the only thing that actually proves the two halves are
# both handled: a stripped binary can still carry the literal, and a
# relocated binary can still carry DWARF.
#
# Usage: scripts/check-pack-privacy.sh dist/lume-core-<os>-<arch>.tar.gz

set -u

fail() { printf 'FAIL  %s\n' "$*" >&2; exit 1; }

[ "$#" -ge 1 ] || fail "usage: check-pack-privacy.sh <package.tar.gz>"
pkg=$1
[ -f "$pkg" ] || fail "$pkg not found (run 'make pack' first)"

# Any absolute path under a home directory (macOS /Users/<name>, Linux
# /home/<name>). Deliberately not anchored to a trailing slash: a leaked
# prefix is already the leak, and requiring the slash would let a bare
# "/Users/<user>" through.
PATTERN='/(Users|home)/[A-Za-z0-9._-]+'

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

mkdir -p "$tmp/x" || fail "cannot create temp dir"
tar -xzf "$pkg" -C "$tmp/x" 2>/dev/null || fail "cannot extract $pkg"

printf 'ok    extracted %s\n' "$pkg"

# -a: treat every file as text, otherwise grep reports "Binary file ...
# matches" and the -l listing still works but the -o below would not.
hits=$(grep -ralE "$PATTERN" "$tmp/x" 2>/dev/null || true)

if [ -n "$hits" ]; then
    # Re-scan with context so the report says *where* in the file, not just
    # that the file matched.
    while IFS= read -r f; do
        [ -n "$f" ] || continue
        rel=${f#"$tmp/x"/}
        printf 'FAIL  build-machine path in %s:\n' "$rel" >&2
        # DWARF records one path per translation unit, so an unstripped binary
        # hits dozens of times. Show a handful with context, then the total,
        # otherwise the report buries the point it is making.
        shown=$(grep -aoE ".{0,40}$PATTERN.{0,40}" "$f" 2>/dev/null | head -5 | sed 's/^/        /')
        printf '%s\n' "$shown" >&2
        total=$(grep -acE "$PATTERN" "$f" 2>/dev/null || echo 0)
        if [ "$total" -gt 5 ]; then
            printf '        ... and %s more matches in this file\n' "$((total - 5))" >&2
        fi
    done <<< "$hits"
    fail "tarball leaks build-machine paths; see 'make pack' (strip + PACK_RT_DEFS)"
fi

printf 'ok    no build-machine paths in %s\n' "$pkg"
exit 0
