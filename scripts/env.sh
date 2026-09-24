#!/usr/bin/env bash
#
# Enter the full build environment: pixi host tools + ESP-IDF.
#
# Must be SOURCED, not executed, since it modifies the current shell:
#
#   . scripts/env.sh
#
# Two layers, and both are needed:
#   - pixi    host tools pinned in pixi.toml (ncurses, socat, openssh, python).
#             ESP-IDF's own venv is built on pixi's Python, so IDF will not work
#             without this active.
#   - ESP-IDF the cross toolchain and idf.py, installed out-of-tree.

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
    echo "scripts/env.sh must be sourced, not executed:" >&2
    echo "    . scripts/env.sh" >&2
    exit 1
fi

_env_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
: "${IDF_PATH:=$HOME/esp/esp-idf}"

if [ ! -d "$IDF_PATH" ]; then
    echo "env.sh: ESP-IDF not found at $IDF_PATH" >&2
    echo "  Set IDF_PATH, or install it -- see docs/PLAN.md." >&2
    return 1
fi

# pixi shell-hook gives us the env without spawning a subshell, so a single
# `. scripts/env.sh` leaves the caller's shell usable.
if command -v pixi >/dev/null 2>&1; then
    eval "$(cd "$_env_root" && pixi shell-hook 2>/dev/null)"
else
    echo "env.sh: pixi not found -- host tools (ncurses, socat, sshd) unavailable," >&2
    echo "  and ESP-IDF's venv was built against pixi's Python." >&2
fi

# shellcheck disable=SC1091
. "$IDF_PATH/export.sh" >/dev/null 2>&1 || {
    echo "env.sh: ESP-IDF export.sh failed" >&2
    return 1
}

echo "env: python $(python --version 2>&1 | cut -d' ' -f2)"
echo "     idf     $(idf.py --version 2>/dev/null || cat "$IDF_PATH/version.txt" 2>/dev/null)"
echo "     target  $(riscv32-esp-elf-gcc -dumpversion 2>/dev/null) (riscv32-esp-elf)"
unset _env_root
