#!/usr/bin/env bash
#
# Build the ESP32-C6 co-processor firmware: the radio half of the Tab5's WiFi
# (and later Bluetooth). The P4 reaches it over SDIO through esp-hosted.
#
# The firmware IS esp-hosted's own co-processor ("slave") example, created from
# the component registry into build-deps/ (git-ignored) -- no third-party source
# is committed, and nothing in it is changed. The version must match the host
# side, pinned in esp-vim/components/esp_net/idf_component.yml.
#
#   scripts/c6-build.sh    -> build-deps/c6-coprocessor/build/merged.bin
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ESP_HOSTED_VERSION="2.12.13"
DIR="$REPO_ROOT/build-deps/c6-coprocessor"

# Recreate the project when it is missing or from another esp-hosted version.
if [ "$(cat "$DIR/.esp-hosted-version" 2>/dev/null)" != "$ESP_HOSTED_VERSION" ]; then
    mkdir -p "$REPO_ROOT/build-deps"
    rm -rf "$DIR"
    ( cd "$REPO_ROOT/build-deps" && idf.py create-project-from-example \
        "espressif/esp_hosted==$ESP_HOSTED_VERSION:slave" )
    mv "$REPO_ROOT/build-deps/slave" "$DIR"
    echo "$ESP_HOSTED_VERSION" > "$DIR/.esp-hosted-version"
fi

idf.py -C "$DIR" -B "$DIR/build" -D IDF_TARGET=esp32c6 build merge-bin -o merged.bin
