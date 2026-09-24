#!/usr/bin/env bash
#
# Onboard a NEW third-party dependency as a vendored LFS archive.
#
# This is the ONLY script in the repo that touches the network.  Everyday builds
# never download anything: third_party/ archives are Git LFS blobs, retrieved by
# `git lfs pull` at clone time and extracted by scripts/prepare-deps.sh.
#
# Usage:
#   scripts/add-dep.sh --name micropython --version 1.29.0 \
#                      --url https://github.com/micropython/micropython/releases/... \
#                      [--prefix micropython-1.29.0] [--patches micropython]
#
# Downloads the archive, records its sha256, stages it into LFS and appends a
# manifest record for you to review.  Nothing is committed -- inspect and commit
# yourself.
#
# NOTE on hashes: the recorded sha256 attests the blob WE vendor, not an upstream
# digest.  If upstream publishes one, verify it by hand before committing and note
# that in the record.  See the header of third_party/manifest.txt.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
THIRD_PARTY="$REPO_ROOT/third_party"
MANIFEST="$THIRD_PARTY/manifest.txt"

die() { printf 'add-dep: %s\n' "$*" >&2; exit 1; }

NAME=""; VERSION=""; URL=""; PREFIX=""; PATCHES="-"; NOTES=""
while [ "$#" -gt 0 ]; do
    case "$1" in
        --name)    NAME="$2";    shift 2 ;;
        --version) VERSION="$2"; shift 2 ;;
        --url)     URL="$2";     shift 2 ;;
        --prefix)  PREFIX="$2";  shift 2 ;;
        --patches) PATCHES="$2"; shift 2 ;;
        --notes)   NOTES="$2";   shift 2 ;;
        -h|--help) sed -n '2,25p' "$0"; exit 0 ;;
        *) die "unknown argument: $1" ;;
    esac
done

[ -n "$NAME" ]    || die "--name is required"
[ -n "$VERSION" ] || die "--version is required"
[ -n "$URL" ]     || die "--url is required"

grep -q "^name:[[:space:]]*$NAME\$" "$MANIFEST" 2>/dev/null \
    && die "'$NAME' is already in the manifest; edit it by hand to bump a version"

FILE="$(basename "${URL%%\?*}")"
case "$FILE" in
    *.tar.gz|*.tgz|*.tar.xz|*.tar.bz2|*.zip) ;;
    *) die "URL does not end in a recognised archive extension: $FILE" ;;
esac

DEST="$THIRD_PARTY/$FILE"
[ -e "$DEST" ] && die "already present: $DEST"

printf 'Downloading %s\n' "$URL"
curl -fsSL --proto '=https' --tlsv1.2 -o "$DEST" "$URL" \
    || die "download failed"

SHA="$(sha256sum "$DEST" | cut -d' ' -f1)"
printf '  sha256 %s\n' "$SHA"

# Derive the archive's top-level directory rather than trusting a guess.
if [ -z "$PREFIX" ]; then
    case "$FILE" in
        *.zip) PREFIX="$(unzip -Z1 "$DEST" | head -1 | cut -d/ -f1)" ;;
        *)     PREFIX="$(tar tf "$DEST" | head -1 | cut -d/ -f1)" ;;
    esac
    printf '  detected prefix: %s\n' "$PREFIX"
fi

git -C "$REPO_ROOT" add "$DEST"

{
    printf '\n'
    printf 'name:     %s\n' "$NAME"
    printf 'version:  %s\n' "$VERSION"
    printf 'file:     %s\n' "$FILE"
    printf 'sha256:   %s\n' "$SHA"
    printf 'prefix:   %s\n' "$PREFIX"
    printf 'url:      %s\n' "$URL"
    printf 'origin:   upstream download via scripts/add-dep.sh\n'
    printf 'patches:  %s\n' "$PATCHES"
    [ -n "$NOTES" ] && printf 'notes:    %s\n' "$NOTES"
} >> "$MANIFEST"

cat <<EOF

Added '$NAME' to the manifest and staged the archive into LFS.

Next:
  1. Review the new record in third_party/manifest.txt (add 'notes:').
  2. Confirm it is tracked by LFS:  git check-attr filter -- "$DEST"
  3. scripts/prepare-deps.sh $NAME
  4. Commit.
EOF
