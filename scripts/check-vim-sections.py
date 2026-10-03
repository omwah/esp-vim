#!/usr/bin/env python3
"""
Check, after a link, that a new Vim session will reset ALL of Vim's globals.

esp_vim_session_begin() (components/vim/port/esp_shims.c) restores libvim.a's
.data from a snapshot and zeroes its .bss, between the bounds that
components/vim/linker.lf puts around them: _vim_{data,bss}_{start,end}. A
global outside them keeps what the last session left in it -- a pointer into
the heap that session freed, say -- and memory inside them that isn't Vim's is
overwritten. Both have happened: on a P4 before revision 3, ESP-IDF made each
placement twice and the bounds covered half of Vim's data (vim_sections.ld.in).

So, from the link map, every non-empty writable input section of libvim.a
(.data*, .sdata*, .bss*, .sbss*, COMMON) must lie inside its bounds, each bound
must be assigned exactly once, and nothing else may lie inside them.

Usage: scripts/check-vim-sections.py BUILD_DIR      (run by scripts/vim-build.sh)
"""

import re
import sys
from pathlib import Path

INPUT = re.compile(r"^ (\.[\w.$]+|COMMON)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S+)")
NAME_ONLY = re.compile(r"^ (\.[\w.$]+|COMMON)\s*$")
CONT = re.compile(r"^\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S+)")
SYMBOL = re.compile(r"^\s+0x([0-9a-f]+)\s+(_vim_(?:data|bss)_(?:start|end)) = ")


def kind(section):
    if re.match(r"\.s?data(\.|$)", section):
        return "data"
    if re.match(r"\.s?bss(\.|$)", section) or section == "COMMON":
        return "bss"
    return None


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: scripts/check-vim-sections.py BUILD_DIR")
    maps = list(Path(sys.argv[1]).glob("*.map"))
    if len(maps) != 1:
        sys.exit(f"check-vim-sections: expected one .map in {sys.argv[1]}, found {len(maps)}")
    lines = maps[0].read_text(errors="replace").splitlines()

    # Only the "Linker script and memory map" part lists placements.
    try:
        lines = lines[next(i for i, l in enumerate(lines) if l.startswith("Linker script and memory map")):]
    except StopIteration:
        sys.exit("check-vim-sections: no memory map in " + str(maps[0]))

    assigned, sections, pending = {}, [], None
    for line in lines:
        m = SYMBOL.match(line)
        if m:
            assigned.setdefault(m.group(2), []).append(int(m.group(1), 16))
            continue
        m = INPUT.match(line)
        if m:
            sections.append((m.group(1), int(m.group(2), 16), int(m.group(3), 16), m.group(4)))
            pending = None
            continue
        m = NAME_ONLY.match(line)
        if m:
            pending = m.group(1)
            continue
        m = CONT.match(line)
        if m and pending:
            sections.append((pending, int(m.group(1), 16), int(m.group(2), 16), m.group(3)))
        pending = None

    errors = []
    bounds = {}
    for k in ("data", "bss"):
        for edge in ("start", "end"):
            name = f"_vim_{k}_{edge}"
            got = assigned.get(name, [])
            if len(got) != 1:
                errors.append(f"{name} is assigned {len(got)} times"
                              + (f" ({', '.join(hex(a) for a in got)})" if got else ""))
        if all(len(assigned.get(f"_vim_{k}_{e}", [])) >= 1 for e in ("start", "end")):
            bounds[k] = (assigned[f"_vim_{k}_start"][-1], assigned[f"_vim_{k}_end"][-1])

    counts = {"data": 0, "bss": 0}
    for name, addr, size, obj in sections:
        k = kind(name)
        if k is None or size == 0 or k not in bounds:
            continue
        lo, hi = bounds[k]
        ours = "libvim.a(" in obj
        inside = lo <= addr and addr + size <= hi
        if ours:
            counts[k] += 1
            if not inside:
                errors.append(f"{name} ({obj.split('(')[-1].rstrip(')')}) at {addr:#x}+{size:#x}"
                              f" is outside _vim_{k} {lo:#x}..{hi:#x}")
        elif lo < addr + size and addr < hi:
            errors.append(f"{name} of {obj} at {addr:#x}+{size:#x} is inside _vim_{k} {lo:#x}..{hi:#x}")

    if errors:
        print("check-vim-sections: a new Vim session would not reset Vim's globals properly:",
              file=sys.stderr)
        for e in errors[:40]:
            print("  " + e, file=sys.stderr)
        if len(errors) > 40:
            print(f"  ... and {len(errors) - 40} more", file=sys.stderr)
        sys.exit(1)
    d, b = bounds["data"], bounds["bss"]
    print(f"check-vim-sections: {counts['data']} data sections in {d[1] - d[0]} bytes, "
          f"{counts['bss']} bss in {b[1] - b[0]} bytes, all inside their bounds")


if __name__ == "__main__":
    main()
