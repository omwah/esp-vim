"""
Reflow a Vim help file to a narrower width, for small screens (the 53-column
display on a 2.8" CYD board). Used by make-runtime-image.py; the help is written
for Vim's usual 78 columns, and this renders the same text narrower instead of
keeping a second copy by hand.

It knows the constructs esp-vim's help.txt uses:
  - rules (==== / ----), shortened to the width;
  - headings and command lines ending in *tags*: the tags are right-aligned, on
    a line of their own when both don't fit;
  - lines of right-aligned tags on their own;
  - paragraphs, rewrapped; their indent is quartered (a tab becomes 2 columns);
  - two-column tables ("term<Tab><Tab>description", continuation lines under the
    description, also terms on a line of their own): kept as columns when the
    terms are short enough, otherwise each description goes under its term;
  - examples (a line ending in ">" up to a line starting with "<"): kept as they
    are, re-indented; a trailing tab-separated comment moves to its own line.
Anything that still can't fit (a long URL, a long command in an example) is left
long rather than broken, and reported.
"""

import re

TS = 8
RULE = re.compile(r"[=-]{10,}")
TAGS = r"(?:\*[^*\s]+\*)(?:\s+\*[^*\s]+\*)*"
TAG_LINE = re.compile(rf"\s*({TAGS})\s*")
HEADED = re.compile(rf"(\S.*?)\t+({TAGS})\s*")
ITEM = re.compile(r"(\s*)(\S[^\t]*?)\t+(\S.*)")


def width(s):
    return len(s.expandtabs(TS))


def indent(s):
    return width(s) - width(s.lstrip())


def narrow_indent(col):
    return col // 4


def words_of(lines):
    """The words of some lines, each with the space that follows it: two after
    a sentence (as Vim's help has them, and when a line ends in . ! or ?), one
    otherwise."""
    out = []
    for l in lines:
        parts = re.split(r"( {2,})", l.strip())
        for k in range(0, len(parts), 2):
            ws = parts[k].split(" ")
            gap_after = len(parts[k + 1]) if k + 1 < len(parts) else None
            for j, word in enumerate(ws):
                if not word:
                    continue
                last = j == len(ws) - 1
                if last and gap_after is not None:
                    gap = 2
                elif last:                      # end of the source line
                    gap = 2 if re.search(r"[.!?][)\"']?$", word) else 1
                else:
                    gap = 1
                out.append((word, gap))
    return out


def wrap(words, first, rest, w):
    """Words (from words_of) into lines of width w: `first` starts line 1,
    `rest` the others."""
    out, line = [], first
    empty, gap = True, 1
    for word, next_gap in words:
        if not empty and width(line + " " * gap + word) > w:
            out.append(line)
            line, empty = rest + word, False
        else:
            line = line + (" " * gap if not empty else "") + word
            empty = False
        gap = next_gap
    if not empty or not out:
        out.append(line)
    return out


class Reflow:
    def __init__(self, w):
        self.w = w
        self.out = []
        self.long = []

    def emit(self, line):
        if width(line) > self.w:
            self.long.append(line)
        self.out.append(line)

    def right(self, left, tags):
        """`left` with `tags` right-aligned; tags on their own line if need be."""
        if width(left) + 1 + len(tags) <= self.w:
            self.emit(left + " " * (self.w - width(left) - len(tags)) + tags)
        else:
            self.emit(" " * max(0, self.w - len(tags)) + tags)
            for l in wrap(words_of([left]), "", "    ", self.w):
                self.emit(l)

    def example(self, lines, base):
        """Example lines, re-indented to at least `base` + 2 (they must start
        with white space to stay an example)."""
        for l in lines:
            if not l.strip():
                self.emit("")
                continue
            ind = max(base + 2, narrow_indent(indent(l)))
            code, _, comment = l.strip().partition("\t")
            comment = comment.strip()
            if comment and width(" " * ind + l.strip()) > self.w:
                self.emit(" " * ind + code)
                for c in wrap(words_of([comment]), " " * (ind + 4), " " * (ind + 4), self.w):
                    self.emit(c)
            else:
                self.emit(" " * ind + (code + "  " + comment if comment else code))

    def run(self, text):
        lines = text.split("\n")
        i, n = 0, len(lines)
        while i < n:
            l = lines[i]
            if not l.strip():
                self.out.append("")
                i += 1
            elif l.startswith(" vim:"):
                self.out.append(re.sub(r"tw=\d+", f"tw={self.w}", l))
                i += 1
            elif RULE.fullmatch(l):
                self.out.append(l[0] * self.w)
                i += 1
            elif i == 0 and l.startswith("*"):         # "*help.txt*<Tab>For Vim ..."
                tag, _, rest = l.partition("\t")
                for x in wrap(words_of([rest]), tag + "\t", "\t", self.w):
                    self.emit(x)
                i += 1
            elif TAG_LINE.fullmatch(l):
                tags = TAG_LINE.fullmatch(l).group(1)
                self.emit(" " * max(0, self.w - len(tags)) + tags)
                i += 1
            elif not l[0].isspace() and HEADED.fullmatch(l):
                m = HEADED.fullmatch(l)
                self.right(m.group(1), m.group(2))
                i += 1
            elif ITEM.fullmatch(l) or self.term_only(lines, i):
                i = self.table(lines, i)
            else:
                i = self.paragraph(lines, i)
        return "\n".join(self.out)

    # -------------------------------------------------------------- tables --

    @staticmethod
    def term_only(lines, i):
        """A term on a line of its own, its description indented below it by at
        least two tab stops more (e.g. "\t'nobackup' 'nowritebackup'")."""
        l = lines[i]
        if "\t" in l.strip() or i + 1 >= len(lines) or not lines[i + 1].strip():
            return False
        return indent(lines[i + 1]) >= indent(l) + 16 and not l.rstrip().endswith(">")

    def items(self, lines, i):
        """Parse the table starting at line i: [(term, desc_lines, example, tail)],
        and the index after it. Items share the term indent; blank lines between
        items are allowed."""
        t = indent(lines[i])
        d = None
        items = []
        while i < len(lines):
            l = lines[i]
            if not l.strip():
                j = i + 1
                if j < len(lines) and lines[j].strip() and indent(lines[j]) == t \
                        and (ITEM.fullmatch(lines[j]) or self.term_only(lines, j)):
                    i = j
                    items.append(None)          # keep the blank line
                    continue
                break
            if indent(l) != t or not (ITEM.fullmatch(l) or self.term_only(lines, i)):
                break
            m = ITEM.fullmatch(l)
            if m:
                term, first = m.group(2).rstrip(), m.group(3)
                dcol = width(l) - width(m.group(3))
                desc = [first]
            else:
                term, desc = l.strip(), []
                dcol = indent(lines[i + 1])
            d = dcol if d is None else d
            i += 1
            example, tail = [], []
            # continuation lines, an example, and text after the example
            while i < len(lines) and lines[i].strip() and indent(lines[i]) >= dcol \
                    and not lines[i].lstrip().startswith("<"):
                if desc and desc[-1].rstrip().endswith(">") and not example:
                    while i < len(lines) and lines[i].strip() and not lines[i].startswith("<"):
                        example.append(lines[i])
                        i += 1
                    break
                desc.append(lines[i].strip())
                i += 1
            if example and i < len(lines) and lines[i].startswith("<"):
                tail.append(lines[i][1:].strip())
                i += 1
                while i < len(lines) and lines[i].strip() and indent(lines[i]) >= dcol:
                    tail.append(lines[i].strip())
                    i += 1
            items.append((term, desc, example, tail))
        while items and items[-1] is None:
            items.pop()
        return t, items, i

    def table(self, lines, i):
        t, items, i = self.items(lines, i)
        T = narrow_indent(t)
        terms = [it[0] for it in items if it]
        widest = max(len(x) for x in terms)
        # Columns if the descriptions keep at least 24 columns.
        D = T + widest + 2
        columns = D <= self.w - 24
        for it in items:
            if it is None:
                self.out.append("")
                continue
            term, desc, example, tail = it
            words = words_of(desc)
            if columns:
                first = " " * T + term + " " * (D - T - len(term))
                dind = D
            else:
                for x in wrap(words_of([term]), " " * T, " " * (T + 2), self.w):
                    self.emit(x)        # a term too long for a line, at its spaces
                first = " " * (T + 4)
                dind = T + 4
            if words:
                for x in wrap(words, first, " " * dind, self.w):
                    self.emit(x)
            elif columns:
                self.emit(first.rstrip())
            if example:
                self.example(example, dind)
            if tail:
                for k, x in enumerate(wrap(words_of(tail), "<" + " " * (dind - 1),
                                           " " * dind, self.w)):
                    self.emit(x)
        return i

    # ---------------------------------------------------------- paragraphs --

    def paragraph(self, lines, i):
        t = indent(lines[i])
        prefix = ""
        l = lines[i]
        if l.startswith("<"):                   # text after an example
            prefix, l = "<", l[1:]
        para = [l.strip()]
        i += 1
        # A command synopsis (":Cmd {arg}") is a line of its own.
        if not para[0].startswith(":") or t:
            while i < len(lines) and lines[i].strip() and indent(lines[i]) == t \
                    and not lines[i].lstrip().startswith(":") \
                    and not ITEM.fullmatch(lines[i]) and not RULE.fullmatch(lines[i]) \
                    and not para[-1].endswith(">"):
                para.append(lines[i].strip())
                i += 1
        T = narrow_indent(t)
        words = words_of(para)
        first = prefix + " " * max(0, T - len(prefix))
        for x in wrap(words, first, " " * T, self.w):
            self.emit(x)
        if para[-1].endswith(">"):              # an example follows
            ex = []
            while i < len(lines) and not lines[i].startswith("<") \
                    and (not lines[i].strip() or lines[i][0].isspace()):
                ex.append(lines[i])
                i += 1
            while ex and not ex[-1].strip():
                ex.pop()
                i -= 1
            self.example(ex, T)
        return i


def reflow(text, w):
    """Returns (text, lines that are still wider than w)."""
    r = Reflow(w)
    return r.run(text), r.long
