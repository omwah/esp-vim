#!/usr/bin/env bash
#
# Build the Vim firmware for one target.
#
#   scripts/vim-build.sh            # esp32p4, with Ethernet (the emulator's P4)
#   scripts/vim-build.sh tab5       # esp32p4, WiFi through the ESP32-C6 (Tab5)
#   scripts/vim-build.sh tab5uart   # the same with its console on UART0 (M5-Bus
#                                   #   G37/38): for esp-emu, which shows no USB
#                                   #   Serial/JTAG, and if USB-C isn't the console
#   scripts/vim-build.sh esp32s3    # the S3 build variant
#   scripts/vim-build.sh es3c28p    # esp32s3, console on USB (Hosyond ES3C28P CYD)
#   scripts/vim-build.sh fnk0115    # esp32s3, 5" RGB panel (Freenove FNK0115)
#   scripts/vim-build.sh rlcd42     # esp32s3, 4.2" mono RLCD (Waveshare ESP32-S3-RLCD-4.2)
#   scripts/vim-build.sh tdeck      # esp32s3, keyboard and trackball (LilyGo T-Deck,
#                                   #   T-Deck Plus)
#   scripts/vim-build.sh cardputer  # esp32s3, no PSRAM, 8 MB: an app for a launcher
#                                   #   (M5Stack Cardputer ADV); its runtime on SD
#
# Each variant builds in its own directory, esp-vim/build-<variant>/, with its
# own sdkconfig inside it, so switching never reconfigures another one. A board
# variant adds sdkconfig.defaults.<variant> on top of its chip's defaults.
# Expects scripts/env.sh to have been sourced (pixi tasks do that).

set -euo pipefail

VARIANT="${1:-${ESPVIM_TARGET:-esp32p4}}"
case "$VARIANT" in
    esp32p4|esp32s3) TARGET="$VARIANT" ;;
    tab5|tab5uart)   TARGET=esp32p4 ;;
    es3c28p|fnk0115|rlcd42|cardputer|tdeck) TARGET=esp32s3 ;;
    *) echo "vim-build: unsupported variant '$VARIANT' (esp32p4, tab5, tab5uart, esp32s3, es3c28p, fnk0115, rlcd42, cardputer, tdeck)" >&2; exit 1 ;;
esac

PROJECT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../esp-vim" && pwd)"
BUILD="build-$VARIANT"
# ESP-IDF also loads <file>.<target> for each listed file, so the chip's own
# defaults come in with sdkconfig.defaults.
DEFAULTS="sdkconfig.defaults"
# A variant of a variant (tab5uart) layers its file on its parent's.
PARENT=""
[ "$VARIANT" = tab5uart ] && PARENT=tab5
[ -z "$PARENT" ] || DEFAULTS="$DEFAULTS;sdkconfig.defaults.$PARENT"
[ "$VARIANT" = "$TARGET" ] || DEFAULTS="$DEFAULTS;sdkconfig.defaults.$VARIANT"

cd "$PROJECT"
# IDF_TARGET only takes effect on the first configure of a build directory; the
# sdkconfig it creates there then pins the target for that directory.
python3 "$PROJECT/../scripts/check-builtins.py"

# $BUILD/sdkconfig is generated from sdkconfig.defaults*, but once it exists
# ESP-IDF prefers it, so an edit to the defaults would silently never apply.
# Nothing hand-made lives in it (menuconfig changes belong in the defaults), so
# regenerate it whenever a defaults file is newer.
for d in sdkconfig.defaults sdkconfig.defaults."$TARGET" ${PARENT:+sdkconfig.defaults."$PARENT"} sdkconfig.defaults."$VARIANT"; do
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

idf.py -B "$BUILD" -D "SDKCONFIG=$BUILD/sdkconfig" -D "IDF_TARGET=$TARGET" \
    -D "SDKCONFIG_DEFAULTS=$DEFAULTS" -D "ESPVIM_VARIANT=$VARIANT" "${RECONF[@]}" build

# A new Vim session resets Vim's globals between linker-made bounds: check, from
# the link map, that they hold all of them and nothing else.
python3 "$PROJECT/../scripts/check-vim-sections.py" "$BUILD"

# The Cardputer's app shares 8 MB of flash with other apps under a launcher,
# which installs the app image alone. Fail if it outgrows its slot in
# partitions-cardputer.csv, warn when it gets big for sharing, and write both
# release images: the app alone (for the launcher, pixi run
# launcher-install-cardputer) and one merged at 0x0 (bootloader, table and app,
# for esptool or M5Burner on a device without the launcher).
if [ "$VARIANT" = cardputer ]; then
    APP="$BUILD/esp-vim.bin"
    SIZE=$(stat -c %s "$APP")
    SLOT=$((0x300000))
    echo "vim-build: app $SIZE bytes ($((SIZE * 100 / SLOT))% of its $SLOT-byte slot)"
    if [ "$SIZE" -gt "$SLOT" ]; then
        echo "vim-build: the app no longer fits partitions-cardputer.csv" >&2
        exit 1
    fi
    [ "$SIZE" -le $((2560 * 1024)) ] || echo "vim-build: warning: over 2.5 MB -- less room for other apps"
    cp "$APP" "$BUILD/esp-vim-cardputer.bin"
    (cd "$BUILD" && python3 -m esptool --chip esp32s3 merge_bin -o esp-vim-cardputer-merged.bin \
        @flash_args >/dev/null)
    echo "vim-build: $BUILD/esp-vim-cardputer.bin (for the launcher), esp-vim-cardputer-merged.bin (at 0x0)"
fi
