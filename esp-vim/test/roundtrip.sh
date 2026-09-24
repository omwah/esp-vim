#!/usr/bin/env bash
#
# Emulator regression gate: Vim edits, saves, survives a reboot, reads it back.
#
# Two scenarios, each a pair of emulator runs (docs/PLAN.md Phase 5):
#   run 1  fresh image, keystrokes injected, --save-state keeps the flash
#   run 2  the SAVED image boots, app_main prints /fat/vimtest.txt as
#          ESPVIM-ARTIFACT<<...>>, and we assert on it
#
#   edit  insert mode, ESC, :w with an absolute path, :q!
#   cwd   relative mkdir, :cd, getcwd(), and a ".."-relative :w -- exercises the
#         userspace working directory in port/esp_shims.c end to end
#
# Also fails if any stub fires, or on any abort/assert/panic.
#
# Usage: esp-vim/test/roundtrip.sh          (from anywhere; builds nothing)

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT="$(cd "$HERE/.." && pwd)"
RUN_EMU="$PROJECT/../scripts/run-emu.sh"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

fails=0
pass() { printf '  PASS  %s\n' "$1"; }
fail() { printf '  FAIL  %s\n' "$1"; fails=$((fails + 1)); }

# esp-emu's --inject understands only \n and \r escapes, so ESC must be a real
# 0x1b byte in the argument -- a literal "\x1b" is typed as four characters.
ESC=$'\033'

run_pair() {
    local name="$1" keys="$2"
    # stdin is held open by `sleep`: an immediate EOF on the UART makes Vim quit.
    sleep 40 | timeout 150 "$RUN_EMU" "$PROJECT" --save-state --timeout 35s -- \
        --inject-on ESPVIM-READY --inject "$keys" --exit-on ESPVIM-END \
        > "$OUT/$name.1" 2>&1
    sleep 25 | timeout 120 "$RUN_EMU" "$PROJECT" --reuse --timeout 20s \
        --exit-on ESPVIM-READY > "$OUT/$name.2" 2>&1
}

artifact() {
    grep -ao 'ESPVIM-ARTIFACT<<[^>]*' "$OUT/$1.2" | head -1 | sed 's/^ESPVIM-ARTIFACT<<//' | tr -d '\r\n'
}

check_clean() {
    local name="$1" f
    for f in "$OUT/$name.1" "$OUT/$name.2"; do
        if grep -aq 'stub called' "$f"; then
            fail "$name: stub fired -- $(grep -ao 'stub called: [a-z_]*' "$f" | head -1)"
        fi
        if grep -aqE 'abort\(\) was called|assert failed|Guru Meditation' "$f"; then
            fail "$name: crash in $(basename "$f")"
        fi
    done
    if grep -aq 'ESPVIM-EXIT rc=0' "$OUT/$name.1"; then
        pass "$name: Vim exited cleanly (rc=0)"
    else
        fail "$name: no clean exit in run 1"
    fi
}

echo "edit round trip"
run_pair edit "ihello from interactive vim${ESC}:w /fat/vimtest.txt"$'\r'":q!"$'\r'
check_clean edit
got="$(artifact edit)"
[ "$got" = "hello from interactive vim" ] \
    && pass "edit: read back after reboot" \
    || fail "edit: expected 'hello from interactive vim', got '$got'"

echo "cwd round trip"
run_pair cwd ":call mkdir('sub')"$'\r'":cd sub"$'\r'":call setline(1, 'cwd=' . getcwd())"$'\r'":w ../vimtest.txt"$'\r'":q!"$'\r'
check_clean cwd
got="$(artifact cwd)"
[ "$got" = "cwd=/fat/sub" ] \
    && pass "cwd: relative mkdir, :cd, getcwd() and ../ write" \
    || fail "cwd: expected 'cwd=/fat/sub', got '$got'"

echo
if [ "$fails" -eq 0 ]; then
    echo "roundtrip: all checks passed"
else
    echo "roundtrip: $fails check(s) FAILED (logs discarded; re-run a scenario by hand to inspect)"
    exit 1
fi
