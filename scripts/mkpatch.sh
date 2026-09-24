#!/usr/bin/env bash
#
# Generate a new patch from edits made in build-deps/<dep>.
#
#   scripts/mkpatch.sh vim 0004-xdiff-config-include-path
#
# Why this exists instead of "git init in the tree": scripts/prepare-deps.sh
# re-extracts build-deps/<dep> from the archive every run, which deletes any
# throwaway .git you created there -- and because build-deps/ sits inside THIS
# repository's worktree, a git command in a tree without its own .git silently
# operates on the outer repo instead. That produces an empty patch and, with
# `git stash`, can touch your real work. So: no git in the tree at all.
#
# Instead we extract a pristine reference copy, apply the EXISTING patch series
# to it, and diff that against your edited tree. The difference is exactly your
# new change.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
THIRD_PARTY="$REPO_ROOT/third_party"
MANIFEST="$THIRD_PARTY/manifest.txt"

die() { printf 'mkpatch: %s\n' "$*" >&2; exit 1; }

DEP="${1:-}"
NAME="${2:-}"
[ -n "$DEP" ]  || die "usage: mkpatch.sh <dep> <patch-name>"
[ -n "$NAME" ] || die "usage: mkpatch.sh <dep> <patch-name>"
NAME="${NAME%.patch}"

TREE="$REPO_ROOT/build-deps/$DEP"
[ -d "$TREE" ] || die "no $TREE -- run: pixi run deps"

field() {
    awk -v want="$DEP" -v key="$1" '
        /^[[:space:]]*#/ { next }
        /^[[:space:]]/   { next }
        $0 !~ /^[a-z0-9_]+:/ { next }
        { k=$0; sub(/:.*$/,"",k); v=$0; sub(/^[a-z0-9_]+:[[:space:]]*/,"",v) }
        k == "name" { cur=v; next }
        cur == want && k == key { print v; exit }
    ' "$MANIFEST"
}

FILE="$(field file)"; PREFIX="$(field prefix)"; PATCHDIR="$(field patches)"
[ -n "$FILE" ] || die "'$DEP' not in $MANIFEST"

OUT="$REPO_ROOT/patches/$PATCHDIR/$NAME.patch"
[ -e "$OUT" ] && die "already exists: $OUT"

REF="$(mktemp -d)"
trap 'rm -rf "$REF"' EXIT

tar xzf "$THIRD_PARTY/$FILE" -C "$REF"
[ -d "$REF/$PREFIX" ] || die "archive has no '$PREFIX'"
mv "$REF/$PREFIX" "$REF/a"

# Apply the existing series, so the diff isolates only the NEW edit.
if [ -d "$REPO_ROOT/patches/$PATCHDIR" ]; then
    while IFS= read -r p; do
        patch -d "$REF/a" -p1 --forward --silent --no-backup-if-mismatch < "$p" \
            || die "existing patch $(basename "$p") does not apply to a pristine tree"
    done < <(find "$REPO_ROOT/patches/$PATCHDIR" -maxdepth 1 -name '*.patch' | sort)
fi

# diff -N so new files appear; -p1 strips the a/ and b/ components.
# Named a/ and b/ so the output reads like a git patch and -p1 works.
ln -s "$TREE" "$REF/b"
( cd "$REF" && diff -ruN \
    --exclude=.git --exclude='*.o' --exclude='*.rej' --exclude='*.orig' \
    a b ) > "$OUT" || true

if [ ! -s "$OUT" ]; then
    rm -f "$OUT"
    die "no differences found -- did you edit build-deps/$DEP?"
fi

printf 'wrote %s\n' "${OUT#"$REPO_ROOT"/}"
printf '  %s\n' "$(grep -c '^+++' "$OUT") file(s), $(grep -c '^@@' "$OUT") hunk(s)"
printf 'Now verify it applies from scratch:  scripts/prepare-deps.sh %s\n' "$DEP"
