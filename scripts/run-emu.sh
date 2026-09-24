#!/usr/bin/env bash
#
# Run a built ESP-IDF project under esp-emu.
#
# Handles the two things that are easy to get wrong by hand (docs/PLAN.md Ph.5):
#   - esp-emu wants a MERGED flash image, not idf.py's separate artefacts.
#   - --save-state writes back OVER the firmware file, so we always run a copy
#     and leave the pristine merged image alone.
#
# Usage:
#   scripts/run-emu.sh <project-dir> [--chip esp32p4] [--psram 32M]
#                      [--save-state] [--reuse] [--exit-on STR] [--timeout 30s]
#                      [-- <extra esp-emu args>]

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EMU="$REPO_ROOT/build-deps/esp-emu/esp-emu"

die() { printf 'run-emu: %s\n' "$*" >&2; exit 1; }

PROJECT=""; CHIP="esp32p4"; PSRAM="32M"; SAVE=0; REUSE=0
EXIT_ON=""; TIMEOUT=""; EXTRA=()

while [ "$#" -gt 0 ]; do
    case "$1" in
        --chip)       CHIP="$2";    shift 2 ;;
        --psram)      PSRAM="$2";   shift 2 ;;
        --save-state) SAVE=1;       shift ;;
        --reuse)      REUSE=1;      shift ;;
        --exit-on)    EXIT_ON="$2"; shift 2 ;;
        --timeout)    TIMEOUT="$2"; shift 2 ;;
        --)           shift; EXTRA=("$@"); break ;;
        -*)           die "unknown option: $1" ;;
        *)            [ -z "$PROJECT" ] || die "one project dir only"
                      PROJECT="$1"; shift ;;
    esac
done

[ -n "$PROJECT" ] || die "no project directory given"
[ -x "$EMU" ] || die "esp-emu not found at $EMU -- run scripts/prepare-deps.sh"

PROJECT="$(cd "$PROJECT" && pwd)"
BUILD="$PROJECT/build"
[ -d "$BUILD" ] || die "no build/ in $PROJECT -- run 'idf.py build' first"

# idf.py can produce the merged image for us, and it knows every partition's
# offset -- including the generated FAT images. Far safer than hand-listing
# offsets to esptool.
#
# Always re-merge. An earlier version skipped this when flasher_args.json was
# no newer than merged.bin -- but a rebuild changes the app binary without
# touching flasher_args.json, so the emulator silently ran a stale image and
# the results looked like a code bug. Merging is cheap; being wrong is not.
MERGED="$BUILD/merged.bin"
command -v idf.py >/dev/null 2>&1 \
    || die "idf.py not on PATH -- source \$IDF_PATH/export.sh"
printf 'Merging flash image...\n'
( cd "$PROJECT" && idf.py merge-bin -o merged.bin >/dev/null )
[ -f "$MERGED" ] || die "merge produced no $MERGED"

# Always run a copy: --save-state overwrites the image it was given, and the
# pristine merged image must survive. --reuse keeps the copy from a previous
# --save-state run instead of starting fresh -- that is what makes a two-run
# "write it, reboot, read it back" test possible.
RUN_IMG="$BUILD/run.bin"
if [ "$REUSE" -eq 1 ]; then
    [ -f "$RUN_IMG" ] || die "--reuse given but $RUN_IMG does not exist"
    printf 'Reusing saved state in %s\n' "${RUN_IMG#"$PROJECT"/}"
else
    cp "$MERGED" "$RUN_IMG"
fi

ARGS=(--chip "$CHIP" --firmware "$RUN_IMG" --psram-size "$PSRAM")
[ "$SAVE" -eq 1 ]      && ARGS+=(--save-state)
[ -n "$EXIT_ON" ]      && ARGS+=(--exit-on "$EXIT_ON")
[ -n "$TIMEOUT" ]      && ARGS+=(--timeout "$TIMEOUT")
[ "${#EXTRA[@]}" -gt 0 ] && ARGS+=("${EXTRA[@]}")

printf 'esp-emu %s\n\n' "${ARGS[*]}"
exec "$EMU" "${ARGS[@]}"
