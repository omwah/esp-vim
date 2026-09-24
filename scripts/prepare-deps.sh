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

# Read one field of one record from the manifest.
manifest_field() {
    local want_name="$1" key="$2"
    awk -v want="$want_name" -v key="$key" '
        /^[[:space:]]*#/ { next }
        /^name:/ { split($0, a, ":"); cur = $2 }
        cur == want && $1 == key ":" {
            sub(/^[a-z0-9]+:[[:space:]]*/, "")
            print
            exit
        }
    ' "$MANIFEST"
}

manifest_names() {
    awk '/^name:/ { print $2 }' "$MANIFEST"
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

# Apply patches/<dir>/*.patch in lexical order.  Stop on the first failure
# rather than leaving a half-patched tree behind.
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
        if git -C "$tree" apply --whitespace=nowarn "$p" 2>/dev/null; then
            info "applied $(basename "$p")"
        elif patch -d "$tree" -p1 --forward --silent < "$p"; then
            info "applied $(basename "$p") (via patch -p1)"
        else
            die "FAILED to apply $(basename "$p") to $tree
       The tree is now half-patched; re-run this script to reset it.
       To refresh a patch, see 'Patch authoring' in docs/DECISIONS.md."
        fi
    done
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
