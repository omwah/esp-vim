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
#   cwd      relative mkdir, :cd, getcwd(), and a ".."-relative :w -- exercises
#            the userspace working directory in port/esp_shims.c end to end
#   runtime  exercise the generated filetype detection (listed, special name,
#            unlisted, shebang, Kconfig), syntax, the system vimrc and netrw
#   timeout  an exponential backtracking search must be abandoned by search()'s
#            300 ms timeout -- proves the esp_timer-backed SIGALRM in esp_shims.c
#
# Every scenario must also stay under a PSRAM ceiling. Forcing the NFA regexp
# engine used ~20 MB just to open a file; this is what stops that returning.
#
# Also fails if any stub fires, on any abort/assert/panic, and on ANY Vim error
# message (Exxx:) on screen -- that is what catches a runtime file gone missing.
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

PSRAM_CEILING_MB=8

check_clean() {
    local name="$1" f start end used
    for f in "$OUT/$name.1" "$OUT/$name.2"; do
        if grep -aq 'stub called' "$f"; then
            fail "$name: stub fired -- $(grep -ao 'stub called: [a-z_]*' "$f" | head -1)"
        fi
        if grep -aqE 'abort\(\) was called|assert failed|Guru Meditation' "$f"; then
            fail "$name: crash in $(basename "$f")"
        fi
    done
    local errs
    errs="$(grep -aoE 'E[0-9]{2,4}: [^[:cntrl:]]*' "$OUT/$name.1" | sort -u)"
    if [ -n "$errs" ]; then
        fail "$name: Vim reported errors: $(echo "$errs" | tr '\n' ';')"
    fi
    start="$(grep -aoE 'ESPVIM-HEAP [^\r]*psram_free=[0-9]+' "$OUT/$name.1" | grep -oE 'psram_free=[0-9]+' | cut -d= -f2)"
    end="$(grep -aoE 'ESPVIM-EXIT [^\r]*psram_free=[0-9]+' "$OUT/$name.1" | grep -oE 'psram_free=[0-9]+' | cut -d= -f2)"
    if [ -n "$start" ] && [ -n "$end" ]; then
        used=$(( (start - end) / 1048576 ))
        if [ "$used" -ge "$PSRAM_CEILING_MB" ]; then
            fail "$name: Vim used ${used} MB of PSRAM (ceiling ${PSRAM_CEILING_MB} MB)"
        fi
    fi
    if grep -aq 'ESPVIM-EXIT rc=0 ' "$OUT/$name.1"; then
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

echo "runtime round trip"
# Filetype detection is generated from esp-vim/filetypes.conf, so check each way
# a type can be found -- and one that must NOT be:
#   new.c    listed extension, plus its syntax file   -> c/c
#   Makefile listed special filename                  -> make
#   x.php    deliberately not in filetypes.conf       -> none
#   runme    no extension, "#!/bin/sh" first line     -> sh   (scripts.vim)
#   Kconfig  ESP-IDF config language                  -> kconfig
# then the system vimrc (utf-8, noswapfile) and netrw (:Explore == 2).
RT=":let g:r = []"$'\r'
RT+=":e /fat/new.c | call add(g:r, &ft . '/' . get(b:, 'current_syntax', 'none'))"$'\r'
RT+=":e /fat/Makefile | call add(g:r, &ft)"$'\r'
RT+=":e /fat/x.php | call add(g:r, empty(&ft) ? 'none' : &ft)"$'\r'
RT+=":call writefile(['#!/bin/sh'], '/fat/runme') | e /fat/runme | call add(g:r, &ft)"$'\r'
RT+=":e /fat/Kconfig | call add(g:r, &ft)"$'\r'
RT+=":call writefile([join(g:r, ':') . ':' . &encoding . ':' . &swapfile . ':' . exists(':Explore')], '/fat/vimtest.txt')"$'\r'
RT+=":qa!"$'\r'
run_pair runtime "$RT"
check_clean runtime
got="$(artifact runtime)"
want="c/c:make:none:sh:kconfig:utf-8:0:2"
[ "$got" = "$want" ] \
    && pass "runtime: generated filetype detection, syntax, system vimrc, netrw" \
    || fail "runtime: expected '$want', got '$got'"

echo "timeout round trip"
# \v(a|aa)+b against 40 a's is exponential for the backtracking engine; without
# a working timeout search() would run for minutes. Expect "found=0 ms=<1000".
run_pair timeout ":call setline(1, repeat('a', 40))"$'\r'":let t = reltime() | let r = search('\v(a|aa)+b', 'n', 0, 300) | call writefile(['found=' . r . ' ms=' . float2nr(reltimefloat(reltime(t)) * 1000)], '/fat/vimtest.txt')"$'\r'":qa!"$'\r'
check_clean timeout
got="$(artifact timeout)"
ms="${got##*ms=}"
if [[ "$got" == found=0\ ms=* ]] && [ "${ms:-99999}" -lt 1000 ]; then
    pass "timeout: pathological search abandoned after ${ms} ms"
else
    fail "timeout: expected 'found=0 ms=<1000', got '$got'"
fi

echo
if [ "$fails" -eq 0 ]; then
    echo "roundtrip: all checks passed"
else
    echo "roundtrip: $fails check(s) FAILED (logs discarded; re-run a scenario by hand to inspect)"
    exit 1
fi
