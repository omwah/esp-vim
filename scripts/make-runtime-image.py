#!/usr/bin/env python3
"""
Build the curated $VIMRUNTIME for the read-only /vimrt partition.

Vim's runtime tree is 51 MB; the partition is 3 MB. This copies a deliberate
subset out of build-deps/vim/runtime (extracted by prepare-deps.sh -- never
committed), overlays OUR files from esp-vim/runtime-image/, and writes the
result to build-deps/vimrt/, which the firmware build turns into a FAT image.

Curation is safe because Vim finds almost all runtime files with `runtime!`,
which silently does nothing when a file is absent: a filetype with no ftplugin
simply has no ftplugin. What must NOT go missing is a file another file
depends on -- cpp syntax sources c syntax, markdown sources html, plugins call
autoload functions -- so dependencies are resolved to a fixed point rather
than hand-picked.

Fails if the estimated FAT footprint will not fit the partition.

Usage: scripts/make-runtime-image.py        (or: pixi run runtime)
"""

import csv
import fnmatch
import os
import re
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "build-deps" / "vim" / "runtime"
OURS = ROOT / "esp-vim" / "runtime-image"
OUT = ROOT / "build-deps" / "vimrt"
PARTITIONS = ROOT / "esp-vim" / "partitions.csv"
FILETYPES_CONF = ROOT / "esp-vim" / "filetypes.conf"
PARTITION_NAME = "vimrt"
CLUSTER = 4096          # fatfsgen --sector_size 4096; every file rounds up to this

# Top-level runtime files Vim itself sources for startup, filetype detection,
# syntax and plugin/indent loading.
# filetype.vim is NOT here: it is generated from esp-vim/filetypes.conf.
ROOT_FILES = [
    "defaults.vim", "ftoff.vim", "ftplugin.vim", "ftplugof.vim",
    "indent.vim", "indoff.vim", "scripts.vim",
]

# Loaded BY NAME from Vim's C code, so the Vimscript dependency resolver cannot
# see them. Found by grepping src/*.c and src/*.h for "*.vim" literals:
#   colors/lists/default.vim  highlight.c sources it to resolve GUI colour names
#                             (e.g. SlateBlue in the default highlight groups);
#                             +termguicolors does this even in a terminal, and a
#                             missing file shows "E254: Cannot allocate color".
#   optwin.vim                behind the :options command.
# Deliberately NOT shipped: menu.vim (only the GUI loads it), evim.vim (vim -y).
C_REFERENCED = ["colors/lists/default.vim", "optwin.vim"]

SYNTAX_INFRA = ["syntax.vim", "synload.vim", "syncolor.vim", "nosyntax.vim", "manual.vim"]

COLORS = ["default", "desert", "habamax", "slate", "lunaperche", "retrobox"]

# Plugins that work without external programs. netrw's remote transports are
# rerouted to native code in Phase 6; local browsing works today.
PLUGINS = ["matchparen.vim", "spellfile.vim", "netrwPlugin.vim"]
PACKS = ["netrw"]            # pack/dist/opt/<name>, minus doc/

# Autoload trees always shipped. None: the stock filetype detector
# (autoload/dist/ft.vim, 80 KB) is not used by the generated filetype.vim, and
# what scripts.vim / indent/vim.vim need is found by the dependency resolver.
AUTOLOAD_ALWAYS = []

# Files the dependency resolver would pull in but which are never loaded by
# default. Each group states why leaving it out is safe. Without these the image
# was 99% of the partition; phpcomplete.vim alone is 346 KB.
EXCLUDE = [
    # Omni-completion engines. Only loaded by CTRL-X CTRL-O; ftplugins merely name
    # them in 'omnifunc'. Invoking omni completion for these filetypes gives E117.
    "autoload/*complete.vim",
    # Archive handlers: need external tar/unzip, which do not exist here. netrw
    # references them, hence the resolver finds them.
    "autoload/tar.vim", "autoload/zip.vim", "autoload/vimball.vim", "syntax/tar.vim",
    # Scripts embedded in Vim script. syntax/vim.vim only includes these when
    # g:vimsyn_embed asks for them, and its default is "lP" (Lua, Python), both
    # of which ARE shipped.
    "syntax/perl.vim", "syntax/pod.vim", "syntax/ruby.vim", "syntax/tcl.vim",
    "syntax/scheme.vim", "syntax/chicken.vim",
    # VBScript inside HTML, and Doxygen comments (only with g:load_doxygen_syntax).
    "syntax/vb.vim", "syntax/doxygen.vim",
]


def excluded(rel):
    rel = str(rel)
    return any(fnmatch.fnmatch(rel, pat) for pat in EXCLUDE)

REF_SYNTAX = re.compile(r"syntax/([A-Za-z0-9_]+)\.vim")
REF_FTPLUGIN = re.compile(r"ftplugin/([A-Za-z0-9_]+)\.vim")
REF_INDENT = re.compile(r"indent/([A-Za-z0-9_]+)\.vim")
REF_COMPILER = re.compile(r"^\s*compiler!?\s+([A-Za-z0-9_]+)", re.M)
REF_AUTOLOAD = re.compile(r"\b((?:[A-Za-z0-9_]+#)+)[A-Za-z0-9_]+\s*\(")


def die(msg):
    print(f"make-runtime-image: {msg}", file=sys.stderr)
    sys.exit(1)


def read_filetypes():
    """Parse esp-vim/filetypes.conf into [(filetype, [patterns]), ...] in file order."""
    if not FILETYPES_CONF.is_file():
        die(f"missing {FILETYPES_CONF}")
    out, seen = [], set()
    for n, line in enumerate(FILETYPES_CONF.read_text().splitlines(), 1):
        line = line.split("#", 1)[0].strip() if not line.lstrip().startswith("#") else ""
        if not line:
            continue
        ft, *pats = line.split()
        if not pats:
            die(f"{FILETYPES_CONF.name}:{n}: '{ft}' has no patterns")
        if not re.fullmatch(r"[A-Za-z0-9_]+", ft):
            die(f"{FILETYPES_CONF.name}:{n}: bad filetype name '{ft}'")
        if ft in seen:
            die(f"{FILETYPES_CONF.name}:{n}: '{ft}' listed twice")
        if any("," in p for p in pats):
            die(f"{FILETYPES_CONF.name}:{n}: a pattern contains ',' (autocmd separator)")
        seen.add(ft)
        out.append((ft, pats))
    return out


def generate_filetype_vim(filetypes):
    """The replacement for stock filetype.vim: only the rules we ship."""
    rules = "\n".join(
        f"au BufNewFile,BufRead {','.join(pats)}\tsetf {ft}" for ft, pats in filetypes)
    # A raw string: the body is Vim script full of regex backslashes, and not one
    # of them is meant as a Python escape.
    return FILETYPE_VIM_TEMPLATE.replace("@RULES@", rules)


FILETYPE_VIM_TEMPLATE = r'''" Vim filetype detection for ESP32-P4 / M5Stack Tab5.
"
" GENERATED by scripts/make-runtime-image.py from esp-vim/filetypes.conf.
" Do not edit here -- edit the .conf and rebuild.
"
" Replaces Vim's stock filetype.vim (~1600 rules) with only the filetypes this
" device ships syntax for. To add one on the device, put a standard ftdetect
" script in /fat/.vim/ftdetect/.

if exists("did_load_filetypes")
  finish
endif
let did_load_filetypes = 1

let s:cpo_save = &cpo
set cpo&vim

if !exists("g:ft_ignore_pat")
  let g:ft_ignore_pat = '\.\(Z\|gz\|bz2\|zip\|tgz\)$'
endif

augroup filetypedetect

" Backup and temporary names: detect as the original ("foo.c.orig", "foo.c~").
au BufNewFile,BufRead ?\+.orig,?\+.bak,?\+.old,?\+.new
TAB\ exe "doau filetypedetect BufRead " . fnameescape(expand("<afile>:r"))
au BufNewFile,BufRead *~
TAB\ let s:name = expand("<afile>") |
TAB\ let s:short = substitute(s:name, '\~\+$', '', '') |
TAB\ if s:name != s:short && s:short != "" |
TAB\   exe "doau filetypedetect BufRead " . fnameescape(s:short) |
TAB\ endif |
TAB\ unlet! s:name s:short

" From esp-vim/filetypes.conf. First match wins.
@RULES@

" On-device additions: /fat/.vim/ftdetect/*.vim (and any other 'runtimepath').
runtime! ftdetect/*.vim

" No name matched: guess from the contents, e.g. a "#!" line (scripts.vim).
au BufNewFile,BufRead,StdinReadPost *
TAB\ if !did_filetype() && expand("<amatch>") !~ g:ft_ignore_pat
TAB\ | runtime! scripts.vim | endif

augroup END

let &cpo = s:cpo_save
unlet s:cpo_save
'''.replace("TAB", "\t")


def partition_size(name):
    with open(PARTITIONS, newline="") as f:
        for row in csv.reader(f):
            if not row or row[0].lstrip().startswith("#"):
                continue
            if row[0].strip() == name:
                return int(row[4].strip(), 0)
    die(f"partition '{name}' not in {PARTITIONS}")


class Image:
    def __init__(self):
        self.files = set()

    def add(self, rel):
        rel = Path(rel)
        if excluded(rel):
            return False
        if (SRC / rel).is_file():
            self.files.add(rel)
            return True
        return False

    def add_tree(self, rel, skip=()):
        base = SRC / rel
        for p in base.rglob("*"):
            if p.is_file() and not any(part in skip for part in p.relative_to(base).parts) \
                    and not excluded(p.relative_to(SRC)):
                self.files.add(p.relative_to(SRC))

    def text(self, rel):
        try:
            return (SRC / rel).read_text(encoding="utf-8", errors="replace")
        except OSError:
            return ""

    def resolve(self):
        """Pull in referenced syntax/ftplugin/indent/autoload files until nothing new appears."""
        seen = set()
        while True:
            pending = [f for f in self.files if f not in seen and f.suffix == ".vim"]
            if not pending:
                return
            for f in pending:
                seen.add(f)
                t = self.text(f)
                for m in REF_SYNTAX.findall(t):
                    self.add(f"syntax/{m}.vim")
                for m in REF_FTPLUGIN.findall(t):
                    self.add(f"ftplugin/{m}.vim")
                for m in REF_INDENT.findall(t):
                    self.add(f"indent/{m}.vim")
                # Some ftplugins run ":compiler X" unconditionally; a missing file is
                # E666. They only set 'makeprg'/'errorformat', so they are harmless
                # even though :make itself cannot run here.
                for m in REF_COMPILER.findall(t):
                    self.add(f"compiler/{m}.vim")
                for m in REF_AUTOLOAD.findall(t):
                    parts = m.rstrip("#").split("#")
                    self.add("autoload/" + "/".join(parts) + ".vim")


def main():
    if not (SRC / "defaults.vim").is_file():
        die(f"no Vim runtime at {SRC} -- run: pixi run deps")

    img = Image()
    for f in ROOT_FILES:
        img.add(f) or die(f"missing required runtime file {f}")
    for f in C_REFERENCED:
        img.add(f) or die(f"missing C-referenced runtime file {f}")
    for f in SYNTAX_INFRA:
        img.add(f"syntax/{f}") or die(f"missing syntax/{f}")
    filetypes = read_filetypes()
    for ft, _ in filetypes:
        got = [img.add(f"{d}/{ft}.vim") for d in ("syntax", "ftplugin", "indent")]
        if not any(got):
            print(f"  warning: filetype '{ft}' has no syntax, ftplugin or indent file",
                  file=sys.stderr)
    for c in COLORS:
        img.add(f"colors/{c}.vim")
    for p in PLUGINS:
        img.add(f"plugin/{p}") or die(f"missing plugin/{p}")
    img.add("autoload/spellfile.vim")
    for a in AUTOLOAD_ALWAYS:
        img.add_tree(f"autoload/{a}")
    for pack in PACKS:
        img.add_tree(f"pack/dist/opt/{pack}", skip={"doc"})

    img.resolve()

    # Materialise.
    if OUT.exists():
        shutil.rmtree(OUT)
    for rel in sorted(img.files):
        dst = OUT / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(SRC / rel, dst)

    (OUT / "filetype.vim").write_text(generate_filetype_vim(filetypes))

    # Overlay our own files (system vimrc, and later plugin/esp.vim etc.).
    ours = 0
    for p in OURS.rglob("*"):
        if p.is_file() and not p.name.startswith("."):
            dst = OUT / p.relative_to(OURS)
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(p, dst)
            ours += 1

    # Budget check: FAT rounds every file up to a whole cluster, and this tree is
    # hundreds of small files, so raw bytes badly understate the real footprint.
    files = [p for p in OUT.rglob("*") if p.is_file()]
    dirs = [p for p in OUT.rglob("*") if p.is_dir()] + [OUT]
    raw = sum(p.stat().st_size for p in files)
    clusters = sum(max(1, -(-p.stat().st_size // CLUSTER)) for p in files) + len(dirs)
    est = clusters * CLUSTER
    cap = partition_size(PARTITION_NAME)
    # Leave room for the FAT itself, the root directory and long-name entries.
    budget = int(cap * 0.90)

    (ROOT / "build-deps" / "vimrt.manifest").write_text(
        "".join(f"{p.relative_to(OUT)}\t{p.stat().st_size}\n" for p in sorted(files)))

    print(f"vimrt: {len(files)} files ({ours} ours), {len(dirs)} dirs, "
          f"{len(filetypes)} filetypes from {FILETYPES_CONF.name}")
    print(f"  raw {raw / 1024:.0f} KB, FAT footprint ~{est / 1024:.0f} KB "
          f"of {cap / 1024:.0f} KB partition ({100 * est / cap:.0f}%)")
    print(f"  written to {OUT.relative_to(ROOT)}/, listing in build-deps/vimrt.manifest")
    if est > budget:
        die(f"estimated {est} bytes exceeds 90% of the {cap}-byte '{PARTITION_NAME}' partition")


if __name__ == "__main__":
    main()
