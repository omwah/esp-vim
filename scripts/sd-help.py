#!/usr/bin/env python3
"""
Write Vim's full help for an SD card: {dest}/vim/doc/.

The firmware's /vimrt has room for only the device's own help.txt and a few
upstream files (scripts/make-runtime-image.py). With a card mounted, the stock
vimrc puts /sd/vim on 'runtimepath' ahead of /vimrt, so :help also finds
whatever is in /sd/vim/doc -- this writes the rest of it there: every doc/*.txt
of the Vim the firmware is built from (build-deps/vim, so the help matches the
binary), netrw's, and the doc/tags that :help looks them up by.

Vim's own help.txt is left out: the device's (in /vimrt) is the first page and
describes this device. So is any tag the device's help.txt defines, so a lookup
of one of those still lands there.

{dest} is the card, mounted on this computer, or a folder to copy to the card
afterwards (by the web interface, scp, or USB): the files go in vim/doc under
it, and anything else there is left alone.

Usage: scripts/sd-help.py DEST        (or: pixi run sd-help DEST)
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "build-deps" / "vim" / "runtime"
DEVICE_HELP = ROOT / "esp-vim" / "runtime-image" / "doc" / "help.txt.in"
EXTRA = [SRC / "pack" / "dist" / "opt" / "netrw" / "doc" / "netrw.txt"]
# As in make-runtime-image.py: a *tag* definition, and a |link|.
TAG_DEF = re.compile(r"(?:^|(?<=\s))\*([^*\s|]+)\*(?=\s|$)", re.M)
TAG_LINK = re.compile(r"(?<!\\)\|([#-)!+-~]+)\|")


def die(msg):
    print(f"sd-help: {msg}", file=sys.stderr)
    sys.exit(1)


def main():
    if len(sys.argv) != 2:
        die("usage: scripts/sd-help.py DEST   (the SD card, or a folder for it)")
    dest = Path(sys.argv[1])
    if not dest.is_dir():
        die(f"{dest} is not a folder")
    if not (SRC / "doc" / "help.txt").is_file():
        die(f"Vim's runtime not found at {SRC}. Run: scripts/prepare-deps.sh")
    docs = sorted(p for p in (SRC / "doc").glob("*.txt") if p.name != "help.txt") + EXTRA
    device = set(TAG_DEF.findall(DEVICE_HELP.read_text(encoding="utf-8")))

    out = dest / "vim" / "doc"
    out.mkdir(parents=True, exist_ok=True)
    tags, shadowed, size = {}, 0, 0
    for d in docs:
        text = d.read_bytes()
        (out / d.name).write_bytes(text)
        size += len(text)
        for t in TAG_DEF.findall(text.decode("utf-8", errors="replace")):
            if t in device:
                shadowed += 1
            elif t not in tags:
                tags[t] = d.name

    # Links that still go nowhere: to tags only Vim's help.txt has, or to
    # things this build leaves out. Counted, not changed: these are Vim's
    # files as they are.
    known = set(tags) | device
    dangling = 0
    for d in docs:
        for line in (out / d.name).read_text(encoding="utf-8", errors="replace").splitlines():
            dangling += sum(1 for l in TAG_LINK.findall(line) if l not in known)

    # Sorted by byte, as :helptags writes it: :help binary-searches the file.
    def esc(t):
        return t.replace("\\", "\\\\").replace("/", "\\/")
    rows = sorted((f"{t}\t{f}\t/*{esc(t)}*" for t, f in tags.items()),
                  key=lambda r: r.encode("utf-8"))
    (out / "tags").write_text("\n".join(rows) + "\n", encoding="utf-8")

    print(f"wrote {len(docs)} help files ({size / 1048576:.1f} MB) and {len(tags)} tags "
          f"to {out}")
    print(f"  {shadowed} tags left to the device's help.txt; "
          f"{dangling} links point at things this build does not have")
    print("On the device, with the card in: :help usr_toc")


if __name__ == "__main__":
    main()
