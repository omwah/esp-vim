#!/usr/bin/env bash
#
# Extract vendored third-party archives into build-deps/ and apply our patches.
#
# This script does NOT use the network.  The archives live in third_party/ as
# Git LFS blobs; `git lfs pull` is the only fetch step, and it happens at clone
# time.  See docs/PLAN.md, Phase 0.
#
# Idempotent by construction: each dependency's tree is removed and re-extracted
# from the archive before patches are applied, so running this twice in a row
# yields the same tree and patches are never applied to an already-patched tree.
#
# Usage:
#   scripts/prepare-deps.sh            # all dependencies
#   scripts/prepare-deps.sh vim        # just one

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
THIRD_PARTY="$REPO_ROOT/third_party"
MANIFEST="$THIRD_PARTY/manifest.txt"
PATCH_ROOT="$REPO_ROOT/patches"
BUILD_DEPS="$REPO_ROOT/build-deps"

die() { printf 'prepare-deps: %s\n' "$*" >&2; exit 1; }
info() { printf '  %s\n' "$*"; }

[ -f "$MANIFEST" ] || die "manifest not found: $MANIFEST"

# build-deps/ holds ~100 MB of extracted upstream source and must never be
# committed.  If it is not ignored, a later `git add -A` would swallow all of it,
# so refuse to populate it rather than set that trap.
if git -C "$REPO_ROOT" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    if ! git -C "$REPO_ROOT" check-ignore -q build-deps 2>/dev/null; then
        die "build-deps/ is not git-ignored -- refusing to extract into a tracked path.
       Add '/build-deps/' to .gitignore first."
    fi
fi

# Read one field of one record from the manifest.
#
# Records are blank-line separated; fields are "key: value" anchored at column 1.
# Indented lines are continuations of the previous field's value and are ignored
# here -- only the first line of a value is returned.  Anchoring matters: a value
# that happens to contain "name:" must not be mistaken for a new record.
manifest_field() {
    local want_name="$1" key="$2"
    awk -v want="$want_name" -v key="$key" '
        /^[[:space:]]*#/  { next }          # comment
        /^[[:space:]]/    { next }          # continuation line
        $0 !~ /^[a-z0-9_]+:/ { next }       # not a field at all

        {
            k = $0; sub(/:.*$/, "", k)                  # key
            v = $0; sub(/^[a-z0-9_]+:[[:space:]]*/, "", v)  # value
        }
        k == "name" { cur = v; next }
        cur == want && k == key { print v; exit }
    ' "$MANIFEST"
}

manifest_names() {
    awk '/^name:[[:space:]]/ { sub(/^name:[[:space:]]*/, ""); print }' "$MANIFEST"
}

# An LFS pointer file is ~130 bytes of text beginning with a version line.  If
# we see one, the blob was never fetched and extraction would fail confusingly.
check_not_lfs_pointer() {
    local f="$1"
    if head -c 64 "$f" 2>/dev/null | grep -q '^version https://git-lfs'; then
        die "$(basename "$f") is an unfetched Git LFS pointer. Run: git lfs pull"
    fi
}

prepare_one() {
    local name="$1"
    local file prefix want_sha got_sha patchdir dest

    file="$(manifest_field "$name" file)"
    prefix="$(manifest_field "$name" prefix)"
    want_sha="$(manifest_field "$name" sha256)"
    patchdir="$(manifest_field "$name" patches)"

    [ -n "$file" ]   || die "no 'file' for '$name' in manifest"
    [ -n "$prefix" ] || die "no 'prefix' for '$name' in manifest"

    local archive="$THIRD_PARTY/$file"
    [ -f "$archive" ] || die "missing archive: $archive"
    check_not_lfs_pointer "$archive"

    printf '%s %s\n' "$name" "$(manifest_field "$name" version)"

    got_sha="$(sha256sum "$archive" | cut -d' ' -f1)"
    if [ "$got_sha" != "$want_sha" ]; then
        die "sha256 mismatch for $file
       expected $want_sha
       got      $got_sha"
    fi
    info "sha256 ok"

    # Always start from a clean tree -- this is what makes the script idempotent.
    dest="$BUILD_DEPS/$name"
    rm -rf "$dest"
    mkdir -p "$BUILD_DEPS"

    local tmp
    tmp="$(mktemp -d "$BUILD_DEPS/.extract.XXXXXX")"
    tar xzf "$archive" -C "$tmp" || { rm -rf "$tmp"; die "extract failed: $file"; }
    if [ ! -d "$tmp/$prefix" ]; then
        rm -rf "$tmp"
        die "archive $file has no top-level dir '$prefix'"
    fi
    mv "$tmp/$prefix" "$dest"
    rm -rf "$tmp"
    info "extracted -> build-deps/$name"

    if [ -n "$patchdir" ] && [ "$patchdir" != "-" ]; then
        apply_patches "$dest" "$PATCH_ROOT/$patchdir"
    fi
}

# Apply patches/<dir>/*.patch in lexical order.
#
# Uses `patch`, deliberately NOT `git apply`. build-deps/ lives inside this
# repository's worktree, and git apply resolves patch paths against the REPO
# ROOT -- so "a/src/feature.h" is read as <repo>/src/feature.h, falls outside
# build-deps/vim, and is silently ignored with EXIT CODE 0. It reports success
# having changed nothing.
#
# Because a tool lying about success is the worst failure mode here, every patch
# is also verified: we hash the files it claims to touch before and after, and
# fail if nothing actually changed.
apply_patches() {
    local tree="$1" dir="$2"
    [ -d "$dir" ] || { info "no patch dir ($dir) -- skipping"; return 0; }

    local -a patches=()
    while IFS= read -r p; do patches+=("$p"); done \
        < <(find "$dir" -maxdepth 1 -name '*.patch' | sort)

    if [ "${#patches[@]}" -eq 0 ]; then
        info "no patches"
        return 0
    fi

    local p
    for p in "${patches[@]}"; do
        local before after
        before="$(patch_target_hash "$tree" "$p")"

        if ! patch -d "$tree" -p1 --forward --silent --no-backup-if-mismatch < "$p"; then
            die "FAILED to apply $(basename "$p") to $tree
       The tree may be half-patched; re-run this script to reset it.
       To refresh a patch, see 'Patch authoring' in docs/DECISIONS.md."
        fi

        after="$(patch_target_hash "$tree" "$p")"
        if [ "$before" = "$after" ]; then
            die "$(basename "$p") reported success but changed NOTHING in $tree.
       The patch is a no-op against this tree -- wrong paths, or already
       applied. Refusing to continue with an unpatched tree."
        fi
        info "applied $(basename "$p")"
    done
}

# Hash the files a patch claims to modify, so a no-op apply cannot pass silently.
# Missing files hash as absent, which still differs once the patch creates them.
patch_target_hash() {
    local tree="$1" patchfile="$2" f
    {
        # Strip the leading path component, matching `patch -p1`. Do NOT assume
        # git's "b/": scripts/mkpatch.sh emits its own prefix names.
        awk '/^\+\+\+ /{ p=$2; if (p == "/dev/null") next; sub(/^[^\/]*\//, "", p); print p }' \
            "$patchfile" | sort -u | while IFS= read -r f; do
            if [ -f "$tree/$f" ]; then
                sha256sum "$tree/$f"
            else
                printf 'absent  %s\n' "$f"
            fi
        done
    } | sha256sum
}

main() {
    local -a targets=()
    if [ "$#" -gt 0 ]; then
        targets=("$@")
    else
        while IFS= read -r n; do targets+=("$n"); done < <(manifest_names)
    fi

    printf 'Preparing dependencies into build-deps/\n'
    local t
    for t in "${targets[@]}"; do
        manifest_names | grep -qx "$t" || die "unknown dependency '$t'"
        prepare_one "$t"
    done
    printf 'Done.\n'
}

main "$@"
