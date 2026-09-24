#!/usr/bin/env python3
"""
Build the curated $VIMRUNTIME for the read-only /vimrt partition.

Vim's runtime tree is 51 MB; the partition is 3 MB. This copies a deliberate
subset out of build-deps/vim/runtime (extracted by prepare-deps.sh -- never
committed), overlays OUR files from esp-vim/runtime-image/, and writes the
result to build-deps/vimrt-<target>/, which the firmware build turns into a
FAT image. It is per target only because :help names the chip it runs on.

Curation is safe because Vim finds almost all runtime files with `runtime!`,
which silently does nothing when a file is absent: a filetype with no ftplugin
simply has no ftplugin. What must NOT go missing is a file another file
depends on -- cpp syntax sources c syntax, markdown sources html, plugins call
autoload functions -- so dependencies are resolved to a fixed point rather
than hand-picked.

Fails if the estimated FAT footprint will not fit the partition.

Usage: scripts/make-runtime-image.py [esp32p4|esp32s3 ...]
       (no target: every supported one; or: pixi run runtime)
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
TARGETS = ["esp32p4", "esp32s3"]
OUT = None                  # build-deps/vimrt-<target>, set per target in main()
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

# Help buffers: Vim sets 'filetype' to "help" itself when :help opens a file,
# independently of filetypes.conf, so these ship regardless.
HELP_SUPPORT = ["syntax/help.vim", "ftplugin/help.vim"]

# Upstream help files shipped next to our help.txt. The intro screen names
# ":help version9", ":help sponsor" and ":help Kuwasha" (uganda.txt), so those
# must exist or the first screen you see lies; netrw is the file browser.
#   source (under build-deps/vim/runtime)  ->  name in /vimrt/doc
UPSTREAM_DOCS = {
    "doc/version9.txt": "version9.txt",
    "doc/uganda.txt": "uganda.txt",
    "doc/sponsor.txt": "sponsor.txt",
    "pack/dist/opt/netrw/doc/netrw.txt": "netrw.txt",
}
# version9.txt is 2 MB, nearly all of it the one-line-per-patch lists; the
# partition has well under 1 MB spare. Those sections keep their heading and
# tags (so |patches-9.2| still lands somewhere) and lose their body.
TRIM_SECTIONS = {"version9.txt": re.compile(r"^PATCHES\s")}

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
# Vim9 script imports name a FILE, not a "name#func(" call, so the autoload rule
# below never sees them. Three forms: relative ("./x", "../x" -- from the
# importing file), `import autoload 'x.vim'` (autoload/ on 'runtimepath') and a
# plain `import 'x.vim'` (import/ on 'runtimepath'). Missing this shipped a
# runtime where every Vim-script buffer failed: indent/vim.vim imports
# ../autoload/dist/vimindent.vim.
REF_IMPORT = re.compile(r"^\s*import\s+(autoload\s+)?['\"]([^'\"]+)['\"]", re.M)
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


HELP_TEMPLATE = OURS / "doc" / "help.txt.in"
TAG_DEF = re.compile(r"(?:^|(?<=\s))\*([^*\s|]+)\*(?=\s|$)", re.M)
TAG_LINK = re.compile(r"(?<!\\)\|([#-)!+-~]+)\|")


def chip_name(target):
    """esp32p4 -> ESP32-P4, as the component CMakeLists does for the C side."""
    m = re.fullmatch(r"esp32(.+)", target)
    return f"ESP32-{m.group(1).upper()}" if m else target.upper()


def trim_sections(text, heading):
    """Drop the body of every section whose heading matches, keeping the rule,
    the heading line (with its tags) and a note saying where the rest is."""
    out, lines, i = [], text.split("\n"), 0
    while i < len(lines):
        out.append(lines[i])
        if re.fullmatch(r"=+", lines[i]) and i + 1 < len(lines) and heading.match(lines[i + 1]):
            out += [lines[i + 1], "",
                    "The list of individual patches is not included on this device, to",
                    "save flash.  It is at https://vimhelp.org/version9.txt.html", ""]
            i += 2
            while i < len(lines) and not re.fullmatch(r"=+", lines[i]) \
                    and not lines[i].startswith(" vim:"):
                i += 1
            continue
        i += 1
    return "\n".join(out)


def copy_upstream_docs():
    for src, name in UPSTREAM_DOCS.items():
        path = SRC / src
        if not path.is_file():
            die(f"missing upstream help file {src}")
        text = path.read_text(encoding="utf-8", errors="replace")
        if name in TRIM_SECTIONS:
            text = trim_sections(text, TRIM_SECTIONS[name])
        (OUT / "doc").mkdir(exist_ok=True)
        (OUT / "doc" / name).write_text(text, encoding="utf-8")


def render_help(filetypes, chip):
    """doc/help.txt from esp-vim/runtime-image/doc/help.txt.in.

    The device's help is an amended version of Vim's help.txt: the navigation
    preamble is Vim's, the rest describes this device. The filetype table is
    generated from filetypes.conf so it can never disagree with detection.
    """
    if not HELP_TEMPLATE.is_file():
        die(f"missing {HELP_TEMPLATE}")
    lines = []
    for ft, pats in filetypes:
        row, first = "", True
        for pat in pats:
            head = f"\t{ft:<12}\t" if first else "\t\t\t"
            if row and len((row + " " + pat).expandtabs(8)) > 76:
                lines.append(row)
                row, first = "", False
                head = "\t\t\t"
            row = (row + " " + pat) if row else head + pat
            first = False
        lines.append(row)
    text = (HELP_TEMPLATE.read_text().replace("@FILETYPES@", "\n".join(lines))
            .replace("@CHIP@", chip))
    left = re.search(r"@[A-Z]+@", text)
    if left:
        die(f"unrendered placeholder {left.group(0)} in {HELP_TEMPLATE.name}")
    (OUT / "doc").mkdir(exist_ok=True)
    (OUT / "doc" / "help.txt").write_text(text)


def write_help_tags():
    """Generate doc/tags the way :helptags would, and refuse dangling links.

    :help finds everything -- help.txt itself included -- through doc/tags, so
    without it ":help" is E149 even though the file is there.

    Our own help must not link anywhere that is missing. Upstream files are
    written against Vim's full 12 MB of documentation, so most of their links
    necessarily point at files not on the device; those are turned into plain
    text (the bars are what make it a link) rather than left as links that
    answer E149 when followed.
    """
    docs = sorted((OUT / "doc").glob("*.txt"))
    tags = {}
    for d in docs:
        for t in TAG_DEF.findall(d.read_text(encoding="utf-8", errors="replace")):
            if t in tags and tags[t] != d.name:
                die(f"help tag *{t}* defined in both {tags[t]} and {d.name}")
            tags[t] = d.name
    dangling, unlinked = [], 0
    upstream = set(UPSTREAM_DOCS.values())
    for d in docs:
        text = d.read_text(encoding="utf-8", errors="replace")
        if d.name in upstream:
            def plain(m):
                nonlocal unlinked
                if m.group(1) in tags:
                    return m.group(0)
                unlinked += 1
                return m.group(1)
            # Leave example blocks alone: there "|x|" is code (a regex, a
            # mapping), and help syntax does not treat it as a link either. A
            # block opens after a line ending in " >" and closes at a line
            # starting with "<" or with any non-blank, as in syntax/help.vim.
            out, example = [], False
            for line in text.split("\n"):
                if example and (line.startswith("<") or line[:1] not in ("", " ", "\t")):
                    example = False
                out.append(line if example else TAG_LINK.sub(plain, line))
                if line == ">" or line.endswith(" >") or line.endswith("\t>"):
                    example = True
            d.write_text("\n".join(out), encoding="utf-8")
            continue
        for n, line in enumerate(text.splitlines(), 1):
            for link in TAG_LINK.findall(line):
                if link not in tags:
                    dangling.append(f"{d.name}:{n}: |{link}|")
    if dangling:
        die("help links to tags that do not exist on the device:\n  " + "\n  ".join(dangling))

    def esc(t):
        return t.replace("\\", "\\\\").replace("/", "\\/")
    rows = sorted(f"{t}\t{f}\t/*{esc(t)}*" for t, f in tags.items())
    (OUT / "doc" / "tags").write_text("\n".join(rows) + "\n")
    return len(tags), unlinked


def partition_size(name):
    with open(PARTITIONS, newline="") as f:
        for row in csv.reader(f):
            if not row or row[0].lstrip().startswith("#"):
                continue
            if row[0].strip() == name:
                return int(row[4].strip(), 0)
    die(f"partition '{name}' not in {PARTITIONS}")


def import_target(importer, spec, is_autoload):
    """Runtime-relative path a Vim9 import refers to."""
    if spec.startswith(("./", "../")):
        return os.path.normpath(str(Path(importer).parent / spec))
    return ("autoload/" if is_autoload else "import/") + spec


class Image:
    def __init__(self):
        self.files = set()
        self.imports = []           # (importer, target) pairs, validated later

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
                for auto, spec in REF_IMPORT.findall(t):
                    target = import_target(f, spec, bool(auto))
                    self.imports.append((str(f), target))
                    self.add(target)
                for m in REF_AUTOLOAD.findall(t):
                    parts = m.rstrip("#").split("#")
                    self.add("autoload/" + "/".join(parts) + ".vim")


def main():
    if not (SRC / "defaults.vim").is_file():
        die(f"no Vim runtime at {SRC} -- run: pixi run deps")
    targets = sys.argv[1:] or TARGETS
    for t in targets:
        if t not in TARGETS:
            die(f"unknown target {t!r} (known: {' '.join(TARGETS)})")
    for t in targets:
        build(t)


def build(target):
    global OUT
    OUT = ROOT / "build-deps" / f"vimrt-{target}"
    chip = chip_name(target)

    img = Image()
    for f in ROOT_FILES:
        img.add(f) or die(f"missing required runtime file {f}")
    for f in C_REFERENCED:
        img.add(f) or die(f"missing C-referenced runtime file {f}")
    for f in HELP_SUPPORT:
        img.add(f) or die(f"missing {f}")
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

    # Every Vim9 import in the image must resolve to a shipped file. A missing
    # one is not a lazy load that might never happen: Vim checks the file when
    # the importing script is sourced, and every buffer of that type errors.
    broken = sorted({f"{imp} -> {tgt}" for imp, tgt in img.imports
                     if Path(tgt) not in img.files})
    if broken:
        die("Vim9 imports that would not resolve on the device:\n  " + "\n  ".join(broken))

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
        # *.in files are templates, rendered below rather than copied.
        if p.is_file() and not p.name.startswith(".") and p.suffix != ".in":
            dst = OUT / p.relative_to(OURS)
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(p, dst)
            ours += 1

    render_help(filetypes, chip)
    copy_upstream_docs()
    ntags, unlinked = write_help_tags()

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

    (ROOT / "build-deps" / f"vimrt-{target}.manifest").write_text(
        "".join(f"{p.relative_to(OUT)}\t{p.stat().st_size}\n" for p in sorted(files)))

    print(f"vimrt-{target}: {len(files)} files ({ours} ours), {len(dirs)} dirs, "
          f"{len(filetypes)} filetypes from {FILETYPES_CONF.name}")
    print(f"  raw {raw / 1024:.0f} KB, FAT footprint ~{est / 1024:.0f} KB "
          f"of {cap / 1024:.0f} KB partition ({100 * est / cap:.0f}%)")
    docs = ", ".join(["help.txt"] + sorted(UPSTREAM_DOCS.values()))
    print(f"  help for {chip}: {docs}; {ntags} tags")
    print(f"    our links all resolve; {unlinked} upstream links to absent docs made plain text")
    print(f"  written to {OUT.relative_to(ROOT)}/, listing in build-deps/vimrt-{target}.manifest")
    if est > budget:
        die(f"estimated {est} bytes exceeds 90% of the {cap}-byte '{PARTITION_NAME}' partition")


if __name__ == "__main__":
    main()
