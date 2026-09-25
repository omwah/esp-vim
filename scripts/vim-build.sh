#!/usr/bin/env bash
#
# Build the Vim firmware for one target.
#
#   scripts/vim-build.sh            # esp32p4, with Ethernet (the emulator's P4)
#   scripts/vim-build.sh tab5       # esp32p4, WiFi through the ESP32-C6 (Tab5)
#   scripts/vim-build.sh esp32s3    # the S3 build variant
#   scripts/vim-build.sh es3c28p    # esp32s3, console on USB (Hosyond ES3C28P CYD)
#
# Each variant builds in its own directory, esp-vim/build-<variant>/, with its
# own sdkconfig inside it, so switching never reconfigures another one. A board
# variant adds sdkconfig.defaults.<variant> on top of its chip's defaults.
# Expects scripts/env.sh to have been sourced (pixi tasks do that).

set -euo pipefail

VARIANT="${1:-${ESPVIM_TARGET:-esp32p4}}"
case "$VARIANT" in
    esp32p4|esp32s3) TARGET="$VARIANT" ;;
    tab5)            TARGET=esp32p4 ;;
    es3c28p)         TARGET=esp32s3 ;;
    *) echo "vim-build: unsupported variant '$VARIANT' (esp32p4, tab5, esp32s3, es3c28p)" >&2; exit 1 ;;
esac

PROJECT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../esp-vim" && pwd)"
BUILD="build-$VARIANT"
# ESP-IDF also loads <file>.<target> for each listed file, so the chip's own
# defaults come in with sdkconfig.defaults.
DEFAULTS="sdkconfig.defaults"
[ "$VARIANT" = "$TARGET" ] || DEFAULTS="$DEFAULTS;sdkconfig.defaults.$VARIANT"

cd "$PROJECT"
# IDF_TARGET only takes effect on the first configure of a build directory; the
# sdkconfig it creates there then pins the target for that directory.
python3 "$PROJECT/../scripts/check-builtins.py"

# $BUILD/sdkconfig is generated from sdkconfig.defaults*, but once it exists
# ESP-IDF prefers it, so an edit to the defaults would silently never apply.
# Nothing hand-made lives in it (menuconfig changes belong in the defaults), so
# regenerate it whenever a defaults file is newer.
for d in sdkconfig.defaults sdkconfig.defaults."$TARGET" sdkconfig.defaults."$VARIANT"; do
    if [ -f "$BUILD/sdkconfig" ] && [ "$d" -nt "$BUILD/sdkconfig" ]; then
        echo "vim-build: $d changed; regenerating $BUILD/sdkconfig"
        rm -f "$BUILD/sdkconfig"
        break
    fi
done

# managed_components/ is shared by every variant -- the component manager has
# no per-build location -- and configuring one variant prunes the components
# only others use (the display driver, esp-hosted). Reconfigure whenever the
# last variant to fill it was a different one, so this one's are fetched back.
STAMP=../build-deps/managed-components-variant
RECONF=()
if [ "$(cat "$STAMP" 2>/dev/null)" != "$VARIANT" ]; then
    RECONF=(reconfigure)
fi
mkdir -p ../build-deps
echo "$VARIANT" > "$STAMP"

exec idf.py -B "$BUILD" -D "SDKCONFIG=$BUILD/sdkconfig" -D "IDF_TARGET=$TARGET" \
    -D "SDKCONFIG_DEFAULTS=$DEFAULTS" -D "ESPVIM_VARIANT=$VARIANT" "${RECONF[@]}" build
