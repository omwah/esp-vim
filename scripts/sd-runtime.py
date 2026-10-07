#!/usr/bin/env python3
"""
Write Vim's runtime for an SD card: {dest}/vim/runtime/.

A board built without the /vimrt partition (CONFIG_ESP_VIM_FLASH_RUNTIME off:
the Cardputer, whose flash is shared with other apps under a launcher) reads
$VIMRUNTIME from /sd/vim/runtime instead. This writes it there: the same
curated runtime make-runtime-image.py makes for a partition, for the given
variant (its help reflowed to that screen's width).

{dest} is the card, mounted on this computer, or a folder to copy to the card
afterwards. vim/runtime under it is replaced whole -- it is the firmware's, not
yours; personal files go in vim/ (vim/vimrc, vim/plugin, ...), next to it. For
Vim's full help too: pixi run sd-help DEST.

Usage: scripts/sd-runtime.py DEST [VARIANT]   (or: pixi run sd-runtime DEST)
       VARIANT defaults to cardputer.
"""

import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def die(msg):
    print(f"sd-runtime: {msg}", file=sys.stderr)
    sys.exit(1)


def main():
    if len(sys.argv) not in (2, 3):
        die("usage: scripts/sd-runtime.py DEST [VARIANT]   (the SD card, or a folder for it)")
    dest = Path(sys.argv[1])
    variant = sys.argv[2] if len(sys.argv) == 3 else "cardputer"
    if not dest.is_dir():
        die(f"{dest} is not a folder")

    subprocess.run([sys.executable, str(ROOT / "scripts" / "make-runtime-image.py"), variant],
                   check=True)
    src = ROOT / "build-deps" / f"vimrt-{variant}"

    out = dest / "vim" / "runtime"
    if out.exists():
        shutil.rmtree(out)
    # copyfile only: a FAT card keeps no permissions, and copying them fails.
    shutil.copytree(src, out, copy_function=shutil.copyfile)
    n = sum(1 for p in out.rglob("*") if p.is_file())
    print(f"sd-runtime: {n} files in {out}")


if __name__ == "__main__":
    main()
