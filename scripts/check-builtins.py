#!/usr/bin/env python3
"""
Fail if Vim's builtin function table is out of order.

find_internal_func() binary-searches global_functions[] in evalfunc.c with
strcmp. Our patch (0008) inserts the esp_*() rows by hand; a row out of place
does not fail to compile -- it silently breaks lookup of UNRELATED builtins,
which then report E117 far from the cause. Run by scripts/vim-build.sh.
"""
import re
import sys
from pathlib import Path

src = Path(__file__).resolve().parent.parent / "build-deps" / "vim" / "src" / "evalfunc.c"
text = src.read_text(encoding="utf-8", errors="replace")
table = text[text.index("global_functions[] ="):]
table = table[:table.index("\n};")]
names = re.findall(r'^\s*\{"(\w+)",', table, re.M)
bad = [f"{a} >= {b}" for a, b in zip(names, names[1:]) if a.encode() >= b.encode()]
if bad:
    sys.exit("check-builtins: evalfunc.c global_functions[] out of order:\n  " + "\n  ".join(bad))
print(f"check-builtins: {len(names)} builtins, sorted")
