#!/usr/bin/env python3
"""
Regenerate the README screenshots from the real firmware.

Boots the Vim firmware under esp-emu, drives it over the emulated UART exactly
as a person at a serial terminal would, feeds every byte the device sends into
a terminal emulator (pyte), and renders that terminal's screen to SVG. Nothing
is mocked: what you see is what the device drew.

Usage: pixi run screenshots [esp32p4|esp32s3]      (build the firmware first)
Writes docs/images/*.svg.
"""

import html
import sys
from pathlib import Path

import pyte

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "esp-vim" / "test"))
from uart_session import Session  # noqa: E402

OUT = ROOT / "docs" / "images"
ROWS, COLS = 28, 96

# xterm's default palette, light terminal: Vim assumes a light background when
# nothing answers its query, so this is how its default colours are meant to look.
NAMED = {
    "black": "000000", "red": "cd0000", "green": "00cd00", "brown": "cdcd00",
    "blue": "0000ee", "magenta": "cd00cd", "cyan": "00cdcd", "white": "e5e5e5",
    "brightblack": "7f7f7f", "brightred": "ff0000", "brightgreen": "00ff00",
    "brightbrown": "ffff00", "brightblue": "5c5cff", "brightmagenta": "ff00ff",
    "brightcyan": "00ffff", "brightwhite": "ffffff",
}
FG, BG = "1a1a1a", "fdfdfd"
CW, CH, FONT = 8.4, 18, 14          # cell width/height and font size, px
PAD, BAR = 12, 28                   # margin, title bar height


class Screen(pyte.Screen):
    """pyte, minus a crash: Vim sends private-mode SGR sequences (CSI ? 4 m,
    CSI > 4;2 m: the modifyOtherKeys handshake). They change nothing on
    screen, and pyte's SGR handler does not accept the private flag."""

    def select_graphic_rendition(self, *attrs, private=False):
        if not private:
            super().select_graphic_rendition(*attrs)


class Tap:
    """Stands in for the session's log file: every byte from the device goes
    through write(), so it feeds the terminal emulator."""

    def __init__(self):
        self.screen = Screen(COLS, ROWS)
        self.stream = pyte.ByteStream(self.screen)

    def write(self, data):
        self.stream.feed(data)

    def flush(self):
        pass

    def close(self):
        pass


def color(c, default):
    if c == "default":
        return default
    return NAMED.get(c, c if len(c) == 6 else default)


def render(screen, title, path):
    w = PAD * 2 + COLS * CW + 6
    h = BAR + PAD * 2 + ROWS * CH
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{w:.0f}" height="{h:.0f}" '
           f'viewBox="0 0 {w:.0f} {h:.0f}" font-family="DejaVu Sans Mono, Menlo, Consolas, monospace" '
           f'font-size="{FONT}">',
           f'<rect width="100%" height="100%" rx="8" fill="#{BG}" stroke="#c8c8c8"/>',
           f'<path d="M0 8 a8 8 0 0 1 8 -8 H{w - 8:.0f} a8 8 0 0 1 8 8 V{BAR} H0 Z" fill="#e6e6e6"/>',
           *[f'<circle cx="{18 + i * 18}" cy="{BAR / 2}" r="6" fill="#{c}"/>'
             for i, c in enumerate(("ff5f57", "febc2e", "28c840"))],
           f'<text x="{w / 2:.0f}" y="{BAR / 2 + 5}" text-anchor="middle" fill="#555" '
           f'font-family="sans-serif" font-size="13">{html.escape(title)}</text>']
    y0 = BAR + PAD
    for row in range(ROWS):
        line = screen.buffer[row]
        col = 0
        while col < COLS:
            ch = line[col]
            fg, bg = color(ch.fg, FG), color(ch.bg, BG)
            if ch.reverse:
                fg, bg = bg, fg
            bold = ch.bold
            # a run of cells with the same look
            end = col + 1
            while end < COLS:
                n = line[end]
                nfg, nbg = color(n.fg, FG), color(n.bg, BG)
                if n.reverse:
                    nfg, nbg = nbg, nfg
                if (nfg, nbg, n.bold) != (fg, bg, bold):
                    break
                end += 1
            x = PAD + col * CW
            y = y0 + row * CH
            if bg != BG:
                out.append(f'<rect x="{x:.1f}" y="{y}" width="{(end - col) * CW:.1f}" '
                           f'height="{CH}" fill="#{bg}"/>')
            text = "".join(line[c].data for c in range(col, end))
            if text.strip():
                out.append(f'<text x="{x:.1f}" y="{y + CH - 5}" fill="#{fg}"'
                           f'{" font-weight=\"bold\"" if bold else ""} xml:space="preserve" '
                           f'textLength="{(end - col) * CW:.1f}" lengthAdjust="spacingAndGlyphs">'
                           f'{html.escape(text)}</text>')
            col = end
    cur = screen.cursor
    if not cur.hidden:
        out.append(f'<rect x="{PAD + cur.x * CW:.1f}" y="{y0 + cur.y * CH}" width="{CW:.1f}" '
                   f'height="{CH}" fill="#1a1a1a" fill-opacity="0.45"/>')
    out.append("</svg>")
    path.write_text("\n".join(out) + "\n")
    print(f"  wrote {path.relative_to(ROOT)}")


# A plain Python script: the device is an editor, so the screenshot shows
# editing -- not firmware code, which would suggest you can build on it.
SCRIPT_PY = [
    '"""Summarise a notes file: word count and TODO items."""',
    '',
    'import sys',
    '',
    '',
    'def summarise(path):',
    '    todos = []',
    '    words = 0',
    '    with open(path) as f:',
    '        for n, line in enumerate(f, 1):',
    '            words += len(line.split())',
    '            if "TODO" in line:',
    '                todos.append((n, line.strip()))',
    '    return words, todos',
    '',
    '',
    'if __name__ == "__main__":',
    '    words, todos = summarise(sys.argv[1] if len(sys.argv) > 1 else "notes.md")',
    '    print(f"{words} words, {len(todos)} TODOs")',
    '    for n, text in todos:',
    '        print(f"  line {n}: {text}")',
]


def main():
    target = sys.argv[1] if len(sys.argv) > 1 else "esp32p4"
    import os
    os.environ["ESPVIM_TARGET"] = target
    chip = "ESP32-" + target[len("esp32"):].upper()
    OUT.mkdir(parents=True, exist_ok=True)
    tap = Tap()
    with Session(term_size=(ROWS, COLS)) as s:
        s.log = tap
        s.expect("ESPVIM-READY", 90)
        s.quiet(2.0)

        # Some files to work with, written by Vim script on the device.
        lines = "[" + ", ".join("'" + l.replace("'", "''") + "'" for l in SCRIPT_PY) + "]"
        s.type(f":call mkdir('/fat/scripts', 'p') | call writefile({lines}, '/fat/scripts/summarise.py')\r")
        s.type(":call writefile(['# Notes', '', 'TODO: write the README.'], '/fat/notes.md')"
               " | call writefile(['{\"wifi\": false}'], '/fat/config.json')\r")
        s.quiet(1.0)

        s.type(":e /fat/scripts/summarise.py\r")
        s.quiet(1.5)
        s.type("gg")
        s.type(":EspInfo\r")
        s.quiet(1.5)
        s.type(":echo ''\r")
        s.quiet(1.0)
        render(tap.screen, f"Vim on {chip}: editing, with :EspInfo", OUT / "editing.svg")

        s.type(":only | enew\r:EspFiles /fat /fat/scripts\r")
        s.quiet(2.0)
        s.type("jj ")
        s.quiet(1.0)
        render(tap.screen, f"Vim on {chip}: :EspFiles", OUT / "files.svg")

        s.type("q:qa!\r")
        s.expect("ESPVIM-EXIT", 60)


if __name__ == "__main__":
    main()
