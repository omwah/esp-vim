#!/usr/bin/env bash
#
# Build the Vim firmware for one target.
#
#   scripts/vim-build.sh            # esp32p4 (the Tab5)
#   scripts/vim-build.sh esp32s3    # the S3 build variant
#
# Each target builds in its own directory, esp-vim/build-<target>/, with its own
# sdkconfig inside it, so switching chips never reconfigures the other one.
# Expects scripts/env.sh to have been sourced (pixi tasks do that).

set -euo pipefail

TARGET="${1:-${ESPVIM_TARGET:-esp32p4}}"
case "$TARGET" in
    esp32p4|esp32s3) ;;
    *) echo "vim-build: unsupported target '$TARGET' (esp32p4, esp32s3)" >&2; exit 1 ;;
esac

PROJECT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../esp-vim" && pwd)"
BUILD="build-$TARGET"

cd "$PROJECT"
# IDF_TARGET only takes effect on the first configure of a build directory; the
# sdkconfig it creates there then pins the target for that directory.
python3 "$PROJECT/../scripts/check-builtins.py"

# $BUILD/sdkconfig is generated from sdkconfig.defaults*, but once it exists
# ESP-IDF prefers it, so an edit to the defaults would silently never apply.
# Nothing hand-made lives in it (menuconfig changes belong in the defaults), so
# regenerate it whenever a defaults file is newer.
for d in sdkconfig.defaults sdkconfig.defaults."$TARGET"; do
    if [ -f "$BUILD/sdkconfig" ] && [ "$d" -nt "$BUILD/sdkconfig" ]; then
        echo "vim-build: $d changed; regenerating $BUILD/sdkconfig"
        rm -f "$BUILD/sdkconfig"
        break
    fi
done

exec idf.py -B "$BUILD" -D "SDKCONFIG=$BUILD/sdkconfig" -D "IDF_TARGET=$TARGET" build
