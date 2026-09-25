#!/usr/bin/env python3
"""
Interactive emulator gate: Vim used the way a person uses it, over the UART.

esp-vim/test/roundtrip.sh proves persistence with keys injected before the
screen exists. This proves the console: keys typed AFTER the screen is drawn,
a terminal that reports its size, mouse reporting, CTRL-C, and the cost of
Vim's constant "is a key waiting?" poll. One emulator session, driven through
--uart-tcp by uart_session.py.

Exit status is non-zero if any check fails. The raw UART log is kept on
failure (path printed) since that is what you need to see why.
"""

import functools
import http.server
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from uart_session import Session, TARGET, PSRAM  # noqa: E402

failures = 0
# The chip's display name, as the firmware derives it: esp32p4 -> ESP32-P4.
CHIP = "ESP32-" + TARGET[len("esp32"):].upper()
# The console UART's TX pin: :EspGpio must refuse to touch it.
CONSOLE_TX = {"esp32p4": 37, "esp32s3": 43}[TARGET]


# Every value read back from the screen ends in an explicit terminator ('|').
# The UART delivers output in arbitrary chunks, and a bare \d+ happily matches
# "40x12" while the final "0" of 120 is still in flight.


def check(ok, label, detail=""):
    global failures
    print(f"  {'PASS' if ok else 'FAIL'}  {label}{(' -- ' + detail) if detail else ''}")
    if not ok:
        failures += 1


# The host as seen from the emulated device (slirp's gateway).
HOST_FROM_DEVICE = "192.168.4.1"


def start_http_server(root):
    """Serve {root} on a free 127.0.0.1 port; the device sees it at the gateway."""
    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=str(root))
    handler.log_message = lambda *a: None
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, f"http://{HOST_FROM_DEVICE}:{srv.server_address[1]}"


def make_spell_file(root):
    """A tiny English spell file built by a host Vim, so the spell-download
    check needs no internet. Returns False (check skipped) with no host Vim."""
    vim = shutil.which("vim")
    if not vim:
        return False
    words = root / "words.txt"
    words.write_text("hello\nworld\nvim\neditor\n")
    r = subprocess.run([vim, "-u", "NONE", "-i", "NONE", "-N", "-es",
                        "-c", "set encoding=utf-8",
                        "-c", f"mkspell! {root / 'en'} {words}", "-c", "qa!"],
                       capture_output=True, timeout=60)
    return (root / "en.utf-8.spl").is_file()


def main():
    log = Path(tempfile.mkstemp(prefix="vim-interactive-", suffix=".log")[1])
    www = Path(tempfile.mkdtemp(prefix="vim-www-"))
    (www / "hello.txt").write_text("hello from the host\n")
    (www / "page.txt").write_text("line one\nline two\n")
    have_spell = make_spell_file(www)
    srv, base = start_http_server(www)
    print(f"interactive session (target: {TARGET}, PSRAM {PSRAM[TARGET]})")
    try:
        with Session(term_size=(40, 120), log=str(log)) as s:
            # Echo an expression's value between markers and return it. The
            # 'X' . '=' split keeps the marker out of the echoed command line.
            def probe(tag, expr):
                s.type(f":echo '{tag}' . '=' . {expr} . '|'\r")
                return s.expect(rf"{tag}=([^|]*)\|".encode(), 30).group(1).decode()
            term = s.expect(rb"ESPVIM-TERM ([^\r\n]*\))", 60).group(1).decode()
            s.expect("ESPVIM-READY", 60)
            s.expect(f"Vim running on {CHIP}", 60)
            check(True, f"intro screen has the title line 'Vim running on {CHIP}'")
            s.quiet(1.5)

            # 0. Default highlight colours use xterm numbering: blue is 4/12.
            #    With PC numbering (t_Co set before t_AF) they were 1/9, red.
            s.type(":echo 'H' . '=' . matchstr(execute('hi NonText'), 'ctermfg=\\d\\+') . ':'"
                   " . matchstr(execute('hi Directory'), 'ctermfg=\\d\\+') . '|'\r")
            got = s.expect(rb"H=([^|]*)\|", 30).group(1).decode()
            check(got == "ctermfg=12:ctermfg=4", "default colours use xterm numbering (blue is blue)",
                  f"NonText:Directory {got!r}")

            # 1. The terminal's size reaches Vim.
            s.type(":echo 'SIZE=' . &lines . 'x' . &columns . '|'\r")
            got = s.expect(rb"SIZE=(\d+x\d+)\|", 30).group(1).decode()
            check(got == "40x120", "terminal size probe adopted by Vim",
                  f"firmware said '{term}', Vim has {got}")

            # 2. Typing into a drawn screen: insert mode, ESC, then read it back.
            s.type("ihello typed interactively\x1b")
            s.quiet(1.0)
            # 'L' . '=' so the marker exists only in Vim's OUTPUT, never in the
            # echoed command line -- which would otherwise match first.
            s.type(":echo 'L' . '=' . getline(1) . '|'\r")
            got = s.expect(rb"L=([^|]*)\|", 30).group(1).decode()
            check(got == "hello typed interactively", "insert-mode typing after the screen is drawn",
                  f"line 1 is {got!r}")
            s.type(":enew!\r")
            s.quiet(1.0)

            # 2b. Backspace. Terminals send DEL (0x7f) for it; ESP-IDF's
            #     tcgetattr() reported no erase character, so Vim only knew ^H
            #     and DEL did nothing. Both must work.
            for name, key in (("DEL", b"\x7f"), ("^H", b"\x08")):
                s.type(":enew!\r")
                s.quiet(0.8)
                s.type("iabcd")
                s.send(key)
                s.send(key)
                s.send(b"\x1b")
                s.quiet(0.8)
                s.type(":echo 'L' . '=' . getline(1) . '|'\r")
                got = s.expect(rb"L=([^|]*)\|", 30).group(1).decode()
                check(got == "ab", f"Backspace sent as {name} deletes in insert mode",
                      f"'abcd' + 2x{name} left {got!r}")

            # 2c. Filename completion on a RELATIVE path. mch_getperm() and
            #     mch_isdir() call stat() directly, so until stat() itself was
            #     interposed they missed the userspace CWD and completion found
            #     nothing. Complete a just-written file with Tab, press Enter,
            #     and check Vim really opened it.
            s.type(":enew!\r:w zebra.txt\r:enew!\r")
            s.quiet(1.0)
            s.type(":e z\t")
            s.quiet(1.0)
            s.type("\r")
            s.quiet(1.0)
            s.type(":echo 'F' . '=' . expand('%') . '|' . isdirectory('.') . '|'\r")
            m = s.expect(rb"F=([^|]*)\|(\d)\|", 30)
            check(m.group(1) == b"zebra.txt" and m.group(2) == b"1",
                  "Tab completes a relative filename (:e z<Tab>)",
                  f"opened {m.group(1).decode()!r}, isdirectory('.')={m.group(2).decode()}")
            s.type(":enew!\r")
            s.quiet(0.8)

            # 2c'. File identity. FATFS gives every file st_dev = st_ino = 0, so
            #      Vim thought any two existing files were the same one. Now:
            #      switch between two existing files, overwrite one with :w
            #      (E949 "File changed while writing" if fstat and stat disagree),
            #      and -- FAT is case-insensitive -- IDA.TXT is the same file.
            s.type(":enew! | call setline(1, 'A') | w ida.txt\r")
            s.type(":enew! | call setline(1, 'B') | w idb.txt\r")
            s.type(":e ida.txt\r")
            s.quiet(1.0)
            s.type(":echo 'I' . '=' . expand('%:t') . ':' . getline(1) . '|'\r")
            got = s.expect(rb"I=([^|]*)\|", 30).group(1).decode()
            check(got == "ida.txt:A", "switch between two existing files", f"got {got!r}")
            s.type(":call setline(1, 'A2') | w\r")
            s.quiet(1.0)
            s.type(":echo 'W' . '=' . readfile('ida.txt')[0] . '|'\r")
            got = s.expect(rb"W=([^|]*)\|", 30).group(1).decode()
            check(got == "A2", "overwrite an existing file with :w", f"file holds {got!r}")
            s.type(":e IDA.TXT\r")
            s.quiet(1.0)
            s.type(":echo 'C' . '=' . bufname('%') . '|' . len(getbufinfo({'buflisted': 1})) . '|'\r")
            m = s.expect(rb"C=([^|]*)\|(\d+)\|", 30)
            check(m.group(1) == b"ida.txt", "IDA.TXT is recognised as ida.txt (FAT is case-insensitive)",
                  f"buffer {m.group(1).decode()!r}")

            # 2d. :help. Only the device's own amended help.txt ships, with a
            #     generated doc/tags; check it opens with help highlighting, a
            #     subject jumps, and CTRL-] follows a link inside it.
            s.type(":help\r")
            s.quiet(1.0)
            s.type(":echo 'H' . '=' . expand('%:t') . '|' . &ft . '|' . get(b:, 'current_syntax', 'none') . '|'\r")
            m = s.expect(rb"H=([^|]*)\|([^|]*)\|([^|]*)\|", 30)
            check(m.groups() == (b"help.txt", b"help", b"help"), ":help opens help.txt, highlighted",
                  "file/filetype/syntax = " + "/".join(g.decode() for g in m.groups()))
            s.type(":help esp-missing\r")
            s.quiet(1.0)
            s.type(":echo 'J' . '=' . (getline('.') =~# '\\*esp-missing\\*') . '|'\r")
            check(s.expect(rb"J=(\d)\|", 30).group(1) == b"1", ":help <subject> jumps to its tag")
            s.type("gg/|esp-keys|\rl\x1d")               # onto the link, then CTRL-]
            s.quiet(1.0)
            s.type(":echo 'K' . '=' . (getline('.') =~# '\\*esp-keys\\*') . '|'\r")
            check(s.expect(rb"K=(\d)\|", 30).group(1) == b"1", "CTRL-] follows a help link")
            s.type(":echo 'W' . '=' . (search('" + CHIP + "', 'nw') > 0) . (search('Tab5', 'nw') > 0) . '|'\r")
            check(s.expect(rb"W=(\d\d)\|", 30).group(1) == b"10",
                  f"help.txt names the {CHIP} and not the Tab5")
            # Every help subject the intro screen advertises must exist, as
            # must netrw's manual which help.txt links to.
            for subject, want in (("version9", "version9.txt"), ("sponsor", "sponsor.txt"),
                                  ("Kuwasha", "uganda.txt"), ("netrw", "netrw.txt")):
                s.type(f":help {subject}\r")
                s.quiet(1.0)
                s.type(":echo 'F' . '=' . expand('%:t') . '|'\r")
                got = s.expect(rb"F=([^|]*)\|", 30).group(1).decode()
                check(got == want, f":help {subject} opens {want}", f"got {got!r}")
            s.type(":only\r:enew!\r")
            s.quiet(1.0)
            s.type(":echo 'V' . '=' . (execute('version') =~# '" + CHIP + "') . '|'\r")
            check(s.expect(rb"V=(\d)\|", 30).group(1) == b"1", f":version names the {CHIP}")

            # 2f. The busy indicator (counted in the log at the end): 1.5 s of
            #     work must draw spinner frames.
            s.type(":let t = reltime() | while reltimefloat(reltime(t)) < 1.5 | endwhile"
                   " | echo 'B' . '=done|'\r")
            s.expect(rb"B=done\|", 60)
            s.quiet(1.0)

            # 2e. Directory browsing, which the help promises: netrw on /fat.
            s.type(":e /fat\r")
            s.quiet(1.5)
            s.type(":echo 'D' . '=' . &ft . '|'\r")
            got = s.expect(rb"D=([^|]*)\|", 30).group(1).decode()
            check(got == "netrw", ":e /fat opens the netrw directory browser", f"filetype {got!r}")
            s.type(":enew!\r")
            s.quiet(1.0)

            # 2g. The :Esp commands (Phase 6a) and the esp_*() builtins behind them.
            s.type(":echo 'I' . '=' . esp_info().chip . '|' . (esp_info().cores > 0) . '|'\r")
            m = s.expect(rb"I=([^|]*)\|(\d)\|", 30)
            check(m.groups() == (CHIP.encode(), b"1"), "esp_info() names the chip",
                  "/".join(g.decode() for g in m.groups()))
            s.type(":EspInfo\r")
            s.quiet(1.0)
            s.type(":echo 'X' . '=' . get(b:, 'esp_view', '') . '|' . (getline(2) =~# '" + CHIP + "') . '|'\r")
            m = s.expect(rb"X=([^|]*)\|(\d)\|", 30)
            check(m.groups() == (b"Info", b"1"), ":EspInfo opens its view with the chip",
                  "/".join(g.decode() for g in m.groups()))
            s.type("q")
            s.quiet(0.5)
            s.type(":echo 'W' . '=' . winnr('$') . '|'\r")
            check(s.expect(rb"W=(\d+)\|", 30).group(1) == b"1", "q closes the view")
            s.type(":EspHeap\r")
            s.quiet(1.0)
            s.type(":echo 'M' . '=' . b:esp_view . (esp_heap().psram.total > 0) . (esp_heap().vim.used > 0) . '|'\r")
            got = s.expect(rb"M=([^|]*)\|", 30).group(1).decode()
            check(got == "Heap11", ":EspHeap shows PSRAM and Vim's own use", f"got {got!r}")
            s.type(":close\r:EspTasks\r")
            s.quiet(1.0)
            s.type(":echo 'T' . '=' . b:esp_view . (search('^vim ', 'nw') > 0) . '|'\r")
            got = s.expect(rb"T=([^|]*)\|", 30).group(1).decode()
            check(got == "Tasks1", ":EspTasks lists the vim task", f"got {got!r}")
            s.type(":close\r")
            # NVS: a number and a string round trip, then erase.
            s.type(":EspNvs test answer 42\r:EspNvs test greet hello world\r")
            s.quiet(1.0)
            s.type(":echo 'N' . '=' . esp_nvs_get('test', 'answer') . ':' . type(esp_nvs_get('test', 'answer'))"
                   " . ':' . esp_nvs_get('test', 'greet') . '|'\r")
            got = s.expect(rb"N=([^|]*)\|", 30).group(1).decode()
            check(got == "42:0:hello world", ":EspNvs stores a number and a string", f"got {got!r}")
            s.type(":EspNvs! test answer\r")
            s.quiet(0.5)
            s.type(":echo 'E' . '=' . esp_nvs_get('test', 'answer', 'gone') . '|'\r")
            got = s.expect(rb"E=([^|]*)\|", 30).group(1).decode()
            check(got == "gone", ":EspNvs! erases a key", f"got {got!r}")
            # GPIO: drive a free pin both ways and read it back; refuse the console.
            s.type(":let g:pin = esp_gpio_pins()[-1] | execute 'EspGpio' g:pin 'on'\r")
            s.quiet(0.5)
            s.type(":let g:hi = esp_gpio_read(g:pin) | call esp_gpio_write(g:pin, 0)"
                   " | echo 'G' . '=' . g:pin . ':' . g:hi . esp_gpio_read(g:pin) . '|'\r")
            got = s.expect(rb"G=([^|]*)\|", 30).group(1).decode()
            check(got.endswith(":10"), ":EspGpio drives a pin high and low", f"pin:levels {got!r}")
            # (Not :try -- ":catch" takes the rest of the line as its pattern.)
            s.type(f":let v:errmsg = '' | silent! let g:r = esp_gpio_read({CONSOLE_TX})"
                   " | echo 'U' . '=' . g:r . ':' . (v:errmsg =~# 'in use') . '|'\r")
            got = s.expect(rb"U=([^|]*)\|", 30).group(1).decode()
            check(got == "-1:1", f"console TX pin {CONSOLE_TX} is refused (-1 and an error)",
                  f"value:error {got!r}")

            # 2g+. The README's Vim-script example runs as printed: read it out of
            #      README.md, write it to the device, :source it. It ends with
            #      GPIO 21 high (i = 9).
            readme = (Path(__file__).resolve().parents[2] / "README.md").read_text()
            block = re.search(r"```vim\n(\" blink GPIO 21.*?)```", readme, re.S).group(1)
            vimlist = "[" + ", ".join("'" + l.replace("'", "''") + "'" for l in block.splitlines()) + "]"
            s.type(f":call writefile({vimlist}, '/fat/blink.vim') | source /fat/blink.vim\r")
            s.quiet(2.0, timeout=60)
            s.type(":echo 'G' . '=' . esp_gpio_read(21) . '|'\r")
            got = s.expect(rb"G=([^|]*)\|", 30).group(1).decode()
            check(got == "1", "the README's blink.vim example runs", f"GPIO 21 reads {got!r}")

            # Diff mode (xdiff is built in; there is no external diff program).
            s.type(":call writefile(['one', 'two', 'three'], '/fat/d1.txt')"
                   " | call writefile(['one', 'TWO', 'three'], '/fat/d2.txt')\r")
            s.type(":e /fat/d1.txt | diffthis | vsplit /fat/d2.txt | diffthis\r")
            s.quiet(1.5)
            s.type(":echo 'Y' . '=' . &diff . (diff_hlID(2, 1) > 0) . (diff_hlID(1, 1) > 0) . '|'\r")
            got = s.expect(rb"Y=([^|]*)\|", 30).group(1).decode()
            check(got == "110", ":diffthis highlights only the changed line", f"diff:changed:same {got!r}")
            s.type(":diffoff! | only | enew!\r")
            s.quiet(1.0)

            # 2j. Network (Phase 6c): the P4's Ethernet under --net user, the
            #     HTTP client, netrw's http:// reads, spell download.
            got = ""
            for _ in range(20):                      # DHCP may still be running
                got = probe("NS", "(esp_net_status().up ? 1 : 0) . ':' . esp_net_status().ip")
                if got.startswith("1"):
                    break
                time.sleep(1)
            check(got.startswith("1:"), "network is up with an address", f"up:ip {got!r}")
            s.type(f":call mkdir('/fat/net', 'p') | EspGet {base}/hello.txt /fat/net/hello.txt\r")
            s.quiet(1.5, timeout=60)
            got = probe("HG", "join(readfile('/fat/net/hello.txt'))")
            check(got == "hello from the host", ":EspGet downloads a file", f"got {got!r}")
            s.type(f":let v:errmsg = '' | silent! call esp_http_get('{base}/missing.txt', '/fat/net/missing.txt')\r")
            s.quiet(1.0)
            got = probe("HM", "(v:errmsg =~# 'HTTP 404') . filereadable('/fat/net/missing.txt') . filereadable('/fat/net/missing.txt.part')")
            check(got == "100", "a 404 is an error and leaves no file behind", f"404:file:part {got!r}")
            s.type(f":e {base}/page.txt\r")
            s.quiet(2.0, timeout=60)
            got = probe("HE", "getline(1) . ':' . getline(2)")
            check(got == "line one:line two", ":e http://... opens the page through netrw", f"got {got!r}")
            s.type(":bwipe!\r")
            # HTTPS against a real site: TLS plus certificate checking against
            # ESP-IDF's CA bundle. Needs the internet, so skipped when the host
            # itself cannot reach the site.
            try:
                import urllib.request
                urllib.request.urlopen("https://example.com/", timeout=5).read(1)
                online = True
            except Exception:
                online = False
            if online:
                s.type(":let g:r = esp_http_get('https://example.com/')\r")
                s.quiet(2.0, timeout=90)
                got = probe("HS", "get(g:r, 'status', 0) . ':' . (get(g:r, 'body', '') =~? 'example domain')")
                check(got == "200:1", "https:// works, with certificate checking", f"status:body {got!r}")
            else:
                print("  SKIP  https (the host has no internet access)")
            if have_spell:
                s.type(f":let g:spellfile_URL = '{base}' | set spell spelllang=en\r")
                # spellfile.vim asks a few questions, in an order that depends on
                # what exists: create the spell directory, download, which
                # directory, and a missing .sug file ends in "Press ENTER".
                answers = [(b"Shall I create", "y"), (b"downloading it", "y"),
                           (b"In which directory", "1"), (b"getting the .sug", "n"),
                           (b"Press ENTER", "\r")]
                end = time.time() + 180
                while time.time() < end:
                    s.quiet(2.0, timeout=120)
                    tail = s.buf[-500:]
                    for pat, key in answers:
                        if pat in tail:
                            s.buf = b""
                            s.type(key)
                            break
                    else:
                        break
                got = probe("SP", "filereadable(expand('~/.vim/spell/en.utf-8.spl')) . ':' . spellbadword('helo world')[0]")
                check(got == "1:helo", "spell checking downloads its dictionary", f"file:bad-word {got!r}")
                s.type(":set nospell\r")
            else:
                print("  SKIP  spell download (no host vim to build a test spell file)")
            s.type(":call esp_fs_delete('/fat/net') | enew!\r")
            s.quiet(1.0)

            # 2h. esp_fs path validation (Phase 6b). Every one of these must be
            #     refused: into read-only /vimrt, ".." out of /fat, a storage
            #     root itself, outside all roots, a directory into itself.
            s.type(":call mkdir('/fat/fm/sub', 'p') | call writefile(['alpha'], '/fat/fm/a.txt')"
                   " | call writefile(['beta'], '/fat/fm/b.txt') | call mkdir('/fat/fm2', 'p')\r")
            s.quiet(1.0)
            s.type(":let g:n = 0 | for C in [function('esp_fs_copy', ['/fat/fm/a.txt', '/vimrt/a.txt']),"
                   " function('esp_fs_delete', ['/fat/../vimrt/vimrc']), function('esp_fs_delete', ['/fat']),"
                   " function('esp_fs_list', ['/fat/../../etc']), function('esp_fs_copy', ['/fat/fm', '/fat/fm/sub/x'])]"
                   " | let v:errmsg = '' | silent! call C() | let g:n += !empty(v:errmsg) | endfor"
                   " | echo 'Z' . '=' . g:n . filereadable('/vimrt/vimrc') . '|'\r")
            got = s.expect(rb"Z=([^|]*)\|", 30).group(1).decode()
            check(got == "51", "esp_fs refuses read-only, traversal, roots, outside, into-itself",
                  f"refused:vimrc-intact {got!r} (want '51')")

            # 2i. :EspFiles, driven by keys: F-keys for some operations, the
            #     letter aliases for others (the Tab5 keyboard has no F-keys).
            # Function keys go in ONE write, as a real terminal sends them. Typed a
            # byte at a time (20 ms apart), host jitter can stretch the sequence
            # past 'ttimeoutlen' and Vim then sees ESC and stray characters.
            F = {"F3": "\x1bOR", "F5": "\x1b[15~", "F7": "\x1b[18~", "F8": "\x1b[19~", "F10": "\x1b[21~"}
            s.type(":EspFiles /fat/fm /fat/fm2\r")
            s.quiet(1.5)
            got = probe("P", "tabpagenr('$') . ':' . b:espfiles.dir . ':' . getbufvar(winbufnr(2), 'espfiles').dir")
            check(got == "2:/fat/fm:/fat/fm2", ":EspFiles opens two panes in a tab", f"got {got!r}")
            s.type("/a\\.txt\r"); s.send(F["F5"])                    # F5: copy a.txt -> other pane
            s.quiet(1.0)
            s.type("\r")                                        # accept the offered destination
            s.quiet(1.5)
            got = probe("C", "join(readfile('/fat/fm2/a.txt'))")
            check(got == "alpha", "F5 copies to the other pane", f"/fat/fm2/a.txt holds {got!r}")
            s.type("gg/b\\.txt\rc")                             # c: the same by letter
            s.quiet(1.0)
            s.type("\r")
            s.quiet(1.5)
            s.type(":call writefile(['old'], '/fat/fm2/b.txt')\r")   # make it differ
            s.quiet(0.5)
            s.type("c")                                          # again: now it exists
            s.quiet(1.0)
            s.type("\r")
            s.quiet(1.0)
            s.type("y")                                          # "Overwrite?" Yes
            s.quiet(1.5)
            got = probe("B", "join(readfile('/fat/fm2/b.txt'))")
            check(got == "beta", "c copies too, and asks before overwriting", f"got {got!r}")
            s.type("gg/a\\.txt\rr")                             # r: move, renaming
            s.quiet(1.0)
            s.type("\x15/fat/fm/renamed.txt\r")                 # CTRL-U, then a new name
            s.quiet(1.5)
            got = probe("R", "filereadable('/fat/fm/a.txt') . filereadable('/fat/fm/renamed.txt')")
            check(got == "01", "r renames a file", f"a.txt:renamed.txt {got!r}")
            s.send(F["F7"])                                      # F7: mkdir
            s.quiet(1.0)
            s.type("newdir\r")
            s.quiet(1.5)
            got = probe("K", "isdirectory('/fat/fm/newdir') . (getline('.') =~# 'newdir/')")
            check(got == "11", "F7 makes a directory and puts the cursor on it", f"got {got!r}")
            s.type("gg/sub\\/\rd")                              # d: delete a directory
            s.quiet(1.0)
            s.type("y")
            s.quiet(1.5)
            got = probe("X", "isdirectory('/fat/fm/sub')")
            check(got == "0", "d deletes a directory after asking", f"still there: {got!r}")
            s.type("gg/renamed\r"); s.send(F["F3"])                    # F3: view
            s.quiet(1.5)
            got = probe("V", "expand('%:t') . ':' . &readonly . ':' . tabpagenr('$')")
            check(got == "renamed.txt:1:3", "F3 views a file read-only in a new tab", f"got {got!r}")
            s.type("q")
            s.quiet(1.0)
            s.type("\t")                                        # to the right pane
            s.quiet(0.5)
            s.type("gg/a\\.txt\r ")                              # tag a.txt ...
            s.type("gg/b\\.txt\rt")                              # ... and b.txt
            s.quiet(1.0)
            got = probe("T", "len(b:espfiles.tags)")
            check(got == "2", "Space and t tag entries", f"{got} tagged")
            s.send(F["F8"])                                      # F8: delete both
            s.quiet(1.0)
            s.type("y")
            s.quiet(1.5)
            got = probe("D", "len(esp_fs_list('/fat/fm2'))")
            check(got == "0", "F8 deletes the tagged entries", f"{got} left in /fat/fm2")
            s.send(F["F10"])                                     # F10: quit
            s.quiet(1.0)
            got = probe("Q", "tabpagenr('$')")
            check(got == "1", "F10 closes the file manager", f"{got} tab pages")
            s.type(":call esp_fs_delete('/fat/fm') | call esp_fs_delete('/fat/fm2') | enew!\r")
            s.quiet(1.0)

            # 3. The zero-timeout input poll is cheap. ESP-IDF's select() rounds
            #    "don't wait" up to a tick; the port short-circuits it. Compared
            #    with a plain loop so emulator speed does not matter. Before the
            #    fix this was ~400 ms against ~9 ms.
            s.type(":let t = reltime() | for i in range(200) | call getchar(0) | endfor"
                   " | echo 'POLL=' . float2nr(reltimefloat(reltime(t)) * 1000) . '|'\r")
            poll = int(s.expect(rb"POLL=(\d+)\|", 60).group(1))
            s.type(":let t = reltime() | for i in range(200) | let x = i | endfor"
                   " | echo 'BASE=' . float2nr(reltimefloat(reltime(t)) * 1000) . '|'\r")
            base = int(s.expect(rb"BASE=(\d+)\|", 60).group(1))
            check(poll <= 5 * base + 20, "zero-timeout input poll is not tick-bound",
                  f"200 polls {poll} ms vs 200 plain iterations {base} ms")

            # 4. CTRL-C interrupts a running command and the editor recovers.
            s.type(":let g:n = 0 | while 1 | let g:n += 1 | endwhile\r")
            time.sleep(3)
            s.send(b"\x03")
            s.quiet(1.5)            # Vim flushes typeahead on interrupt
            s.type(":echo 'N=' . g:n . '|'\r")
            n = int(s.expect(rb"N=(\d+)\|", 30).group(1))
            check(n > 0, "CTRL-C interrupts a running loop", f"loop reached n={n}")

            s.type(":qa!\r")
            s.expect(rb"ESPVIM-EXIT rc=0 ", 30)
            check(True, "clean exit after interactive use")

            # 8. :q is not a dead end. Vim restarts in place (no reboot) with ALL
            #    of its state back at power-on: .data restored, .bss zeroed, heap
            #    arena re-created, leftover files closed. Each session below is
            #    dirtied first -- a global, an option, the working directory,
            #    extra buffers, :help, a directory listing, an armed regexp
            #    timer -- and the next must not see any of it, nor leak memory.
            starts = []
            for cycle in range(1, 4):
                s.expect(rb"Vim is forever\.", 30)
                s.expect(rb"Press any key to start a new session\.", 30)
                s.quiet(0.5)
                s.send(b"x")                               # "press any key"
                m = s.expect(rb"ESPVIM-HEAP session=(\d+) int_free=(\d+) ", 60)
                starts.append((int(m.group(1)), int(m.group(2))))
                s.expect("ESPVIM-READY", 60)
                s.quiet(1.5)
                s.type(":echo 'S' . '=' . exists('g:dirty') . ':' . getcwd() . ':' . &tabstop"
                       " . ':' . len(getbufinfo({'buflisted': 1})) . '|'\r")
                got = s.expect(rb"S=([^|]*)\|", 30).group(1).decode()
                check(got == "0:/fat:8:1", f"session {cycle + 1} starts at power-on state",
                      f"globals:cwd:tabstop:buffers = {got!r} (want '0:/fat:8:1')")
                s.type(":let g:dirty = 1 | set tabstop=3 | cd /vimrt | e /vimrt/vimrc"
                       " | e /vimrt/defaults.vim | help | only | e /fat"
                       " | call search('\\v(a|aa)+b', 'n', 0, 50)\r")
                s.quiet(1.5)
                s.type(":qa!\r")
                s.expect(rb"ESPVIM-EXIT rc=0 ", 30)

            sessions = [n for n, _ in starts]
            check(sessions == [2, 3, 4], "each :q starts a new session in place (no reboot)",
                  f"sessions seen: {sessions}")
            ints = [f for _, f in starts]
            check(max(ints) - min(ints) <= 1024, "no internal-RAM leak across sessions",
                  f"internal free at session start: {ints}")

        data = log.read_bytes()
        # 5. No "OOPS": Vim's fallback tgoto() emits that for a termcap string
        #    it cannot expand (patch 0006 fixed the one that did).
        check(b"OOPS" not in data, "no unexpandable termcap strings (\"OOPS\")",
              f"{data.count(b'OOPS')} found" if b"OOPS" in data else "")
        # 2f. The busy spinner: drawn on the bottom row, one cell in from the
        #     right (row 40, col 119), inside DECSC/DECRC so Vim's cursor is
        #     untouched.
        spun = data.count(b"\x1b7\x1b[40;119H\x1b[7m")
        check(spun >= 2, "busy spinner turns during long commands", f"{spun} frames drawn")
        # 6. Mouse reporting enabled (button tracking + SGR) and disabled at exit.
        on = b"\x1b[?1002h" in data and b"\x1b[?1006h" in data
        off = b"\x1b[?1002l" in data and b"\x1b[?1006l" in data
        check(on and off, "SGR mouse reporting enabled, and disabled on exit")
        # 6b. No crash and no reboot at any point. A panic reboots the chip and
        #     Vim comes back fresh, so later checks can fail in confusing ways --
        #     or pass by accident. Catch it directly: one boot, no panic.
        boots = data.count(b"ESPVIM-HEAP session=1 ")
        panics = len(re.findall(rb"Guru Meditation|abort\(\) was called|assert failed", data))
        check(boots == 1 and panics == 0, "no crash or reboot during the session",
              f"session-1 starts: {boots}, panics: {panics}")
        # 7. No Vim error messages anywhere in the session.
        errs = sorted(set(m.decode(errors="replace")
                          for m in re.findall(rb"E\d{2,4}: [^\x1b\r\n]*", data)))
        check(not errs, "no Vim error messages", "; ".join(errs))
    except Exception as e:           # a hang or crash is a failure, not a traceback
        check(False, "session completed", f"{type(e).__name__}: {e}")

    if failures:
        print(f"interactive: {failures} check(s) FAILED -- UART log kept at {log}")
        return 1
    log.unlink()
    print("interactive: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
