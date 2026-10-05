#!/usr/bin/env bash
#
# Guard the *repository* against build-machine paths.
#
#   scripts/check-pack-privacy.sh covers make pack's tarball. That binary is
#   stripped (removes DWARF) and relinked with a package-relative
#   -DLUME_RT_SRC (removes the literal), so it structurally cannot carry a
#   /Users/<name> path.
#
#   A development binary makes neither promise: CFLAGS keeps -g, so DWARF
#   records the compile-time working directory. That is exactly how
#   backups/lume-core.bak-2026* (65 hits each, every one pointing into the
#   tree's own compile dir under /Users/<user>/...) got committed in
#   93150d2/de190f8 — packcheck was green the whole time, because packcheck
#   only ever looks at dist/. (Note the shape of that sentence: it cannot
#   name the directory it points at. This file trips its own guard if it
#   does.)
#
# So there are two exits for a build-machine path, and this is the one the
# other guard cannot see.
#
# Default scope: blobs reachable from HEAD. -H also walks the whole object
# database, which is what catches "the tip is clean but some abandoned branch
# still has it" — cheap here because the repo is a few hundred objects.
#
# Writing about the pattern is allowed, writing the literal is not: a comment
# that spells /Users/<user>/... out in full is flagged too (it failed this
# script on its first run, via the Makefile line 21 comment that describes the
# very thing being guarded). Placeholder brackets are not part of the
# [A-Za-z0-9._-]+ class the pattern matches, so `<user>` stays green.
#
# History matters more than it looks: a blob that leaves HEAD only via a
# filter-repo run stays readable for the entire window before GitHub's GC.
# While this repo is still unpushed that window is the only risk there is,
# and it is why the cleanup below (`git rm --cached`) is enough *today* —
# but -H is one flag away if it ever gets pushed.
#
# --remote <sha> [<sha>...] asks GitHub whether those blobs are *still in the
# remote object database* — the question that decides "may I flip this repo back
# to public?". A force-push only makes an object unreachable; GitHub keeps it
# readable until its own GC runs (no API to trigger that, no SLA on it).
#
# 🔴 The trap this exists for: while the repo is private, an *unauthenticated*
# 404 proves nothing (a private repo 404s for everyone anyway). Only an
# authenticated probe can tell "gone" from "still there but hidden", so this
# checks has("content") on the blobs that used to leak:
#
#     ./scripts/check-tree-privacy.sh --remote \
#        61b9c23a732d6b86ece3eed0417ac00a98056464 \
#        7fac20dba69d72925433561a906dd20aa1ff84b1
#
# Every blob gone  => exit 0, safe to flip back to public.
# Any blob present => exit 1, stay private, poll again later.
#
# Usage: scripts/check-tree-privacy.sh [--history]
#        scripts/check-tree-privacy.sh --remote <blob-sha> [<blob-sha>...]
#
# --history is on-demand, not wired into any target: those two backup blobs
# stay reachable until this repo is pushed somewhere and rewritten, so the
# flag is red by design until someone actually scrubs. Default scope is the
# honest day-to-day question ("is what's in the repo now clean?").
#
set -u

fail() { printf 'FAIL  %s\n' "$*" >&2; exit 1; }

# Same pattern as check-pack-privacy.sh, on purpose: the two guards must
# agree on what counts as a leak. Not anchored to a trailing slash — a leaked
# prefix is already the leak.
PATTERN='/(Users|home)/[A-Za-z0-9._-]+'

history=0
remote=0
remote_shas=()
argc=$#          # before any shift: "$#" counts *all* argv, not the remainder
case "${1:-}" in
    --remote)
        remote=1
        shift
        remote_shas=("$@")
        ;;
    --history)
        history=1
        shift
        ;;
esac
# 🔴 Careful: after `shift`, "$#" is "argv minus the flag", *not* "unexpected
# arguments". The first cut at this script compared "$#" against 0 after
# shifting --remote and the two perfectly good blob shas read as "extra args",
# so --remote failed its own usage check.
if [ "$history" -eq 1 ]; then
    [ "$#" -eq 0 ] || fail "usage: check-tree-privacy.sh [--history]"
elif [ "$remote" -eq 1 ]; then
    [ "${#remote_shas[@]}" -gt 0 ] \
        || fail "usage: check-tree-privacy.sh --remote <blob-sha> [<blob-sha>...]"
    [ "$#" -eq $((argc - 1)) ] \
        || fail "usage: check-tree-privacy.sh --remote <blob-sha> [<blob-sha>...] ($# stray arg(s))"
else
    [ "$#" -eq 0 ] || fail "usage: check-tree-privacy.sh [--history]|--remote <sha> [<sha>...]"
fi

# Deliberately not `set -e`: a blob is fed through a pipe and the count comes
# back from the other end, so the interesting failures are exit statuses of
# things further down the pipeline, not of git itself.

leaks=0
scanned=0

# $1 = blob sha, $2 = label to report. Writes nothing on a clean blob.
scan() {
    sha=$1
    label=$2
    scanned=$((scanned + 1))
    # -a: without it grep says "Binary file (standard input) matches" and -o
    # below would never see the matches at all.
    total=$(git cat-file blob "$sha" 2>/dev/null | grep -aoE "$PATTERN" | wc -l | tr -d ' ')
    [ "${total:-0}" -eq 0 ] && return 0
    printf 'FAIL  build-machine path in %s (blob %s):\n' "$label" "$sha" >&2
    # DWARF records one path per translation unit, so a single unstripped
    # binary hits dozens of times. Show a few with context, then the total.
    git cat-file blob "$sha" 2>/dev/null | grep -aoE ".{0,40}$PATTERN.{0,40}" 2>/dev/null \
        | head -3 | sed 's/^/        /' >&2
    printf '        ... and %s more match(es) in this blob\n' "$((total - 3))" >&2
    leaks=$((leaks + 1))
}

# --remote: ask GitHub whether each blob is still in its object database.
# `has("content")` is the whole question — a blob that is unreachable but not
# yet GC-ed still answers true, and that is exactly the window where flipping
# the repo back to public would expose it.
remote_check() {
    local origin_repo blob_state
    origin_repo=$(git remote get-url origin 2>/dev/null) \
        || fail "no origin remote; --remote needs one"
    remote_repo=${origin_repo#https://github.com/}
    remote_repo=${remote_repo%.git}
    [ "$remote_repo" != "$origin_repo" ] \
        || fail "cannot parse owner/repo out of origin '$origin_repo'"

    # Pre-flight: if the repo itself cannot be read, every blob probe below
    # would come back empty, and "empty" must never be allowed to read as
    # "gone" (that would green-light a public flip on a broken probe).
    gh api "repos/$remote_repo" --jq '.name' >/dev/null 2>&1 \
        || fail "cannot read repos/$remote_repo (gh not authenticated?): refusing to guess"

    present=0
    unknown=0
    for blob in "$@"; do
        blob_state=$(gh api "repos/$remote_repo/git/blobs/$blob" --jq 'has("content")' 2>/dev/null)
        case "$blob_state" in
            true)
                printf 'FAIL  %s is still in the remote object database (unreachable, not GC-ed)\n' "$blob" >&2
                present=$((present + 1)) ;;
            false)
                printf 'ok    %s gone from remote\n' "$blob" ;;
            *)
                # %.60s: on a 404 gh prints the whole error JSON; only the
                # message matters, and a 90-char dump buries the verdict.
                printf 'warn  %s unreadable, probe inconclusive (answer: %.60s)\n' \
                    "$blob" "${blob_state:-<empty>}" >&2
                unknown=$((unknown + 1)) ;;
        esac
    done
    [ "$present" -eq 0 ] || fail "$present blob(s) still readable on remote; stay private until GitHub GCs them"
    [ "$unknown" -eq 0 ] || fail "$unknown blob(s) could not be probed; decide by hand"
    printf 'ok    all %s blob(s) gone from remote: safe to flip back to public\n' "$#"
    exit 0
}

if [ "$remote" -eq 1 ]; then
    remote_check "${remote_shas[@]}"
fi

if [ "$history" -eq 1 ]; then
    git rev-parse --verify --quiet HEAD >/dev/null 2>&1 \
        || fail "no HEAD; --history needs at least one commit"
    while read -r type sha; do
        [ "$type" = blob ] || continue
        scan "$sha" "blob $sha"
    done < <(git cat-file --batch-all-objects --batch-check='%(objecttype) %(objectname)' 2>/dev/null)
else
    git rev-parse --verify --quiet HEAD >/dev/null 2>&1 \
        || fail "no HEAD yet; nothing tracked"
    # ls-tree 一行 = "<mode> <type> <sha>\t<path>". IFS 不能设成只有 \t:那样
    # 前三个**空格**分隔的字段会粘成 "$mode" 一个值, type 比对永远为假
    # (第一版就是这么静默扫到 0 个 blob 的 —— 绿的, 但什么都没查)。
    # 纯字符串切分, 避免每行四个 cut 的 fork。
    while IFS= read -r line; do
        [ -n "$line" ] || continue
        mode=${line%% *}
        rest=${line#* }
        type=${rest%% *}
        sha=${rest#* }
        sha=${sha%%$'\t'*}
        path=${line#*$'\t'}
        [ "$type" = blob ] || continue
        scan "$sha" "$path"
    done < <(git ls-tree -r HEAD 2>/dev/null)
fi

if [ "$history" -eq 1 ]; then
    scope='the whole object database'
else
    scope='HEAD'
fi

if [ "$leaks" -ne 0 ]; then
    fail "$leaks of $scanned blob(s) reachable from $scope leak build-machine paths"
fi

printf 'ok    %s blob(s) from %s: no build-machine path\n' "$scanned" "$scope"
exit 0
