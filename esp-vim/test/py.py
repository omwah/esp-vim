#!/usr/bin/env python3
"""
Python gate (Phase 7): MicroPython inside Vim, in one emulator session.

:EspPy, :EspPyRun (output window, traceback to quickfix), :EspPyReset; the vim
module (command, eval, call, vim.error, buffers with undo, the cursor, g:
variables and options); values converted both ways; the esp module; files and
imports relative to Vim's directory; input(); CTRL-C out of a loop and out of
time.sleep(); deep recursion and running out of memory raise, and the editor
carries on.
"""

import re
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from uart_session import Session, TARGET  # noqa: E402

failures = 0


def check(ok, label, detail=""):
    global failures
    print(f"  {'PASS' if ok else 'FAIL'}  {label}{(' -- ' + detail) if detail else ''}")
    if not ok:
        failures += 1


def vlist(lines):
    """A Vim List literal of single-quoted Strings."""
    return "[" + ", ".join("'" + ln.replace("'", "''") + "'" for ln in lines) + "]"


def vstr(text):
    return "'" + text.replace("'", "''") + "'"


def main():
    log = Path(tempfile.mkstemp(prefix="vim-py-", suffix=".log")[1])
    print(f"python session (target: {TARGET})")
    try:
        with Session(term_size=(40, 150), log=str(log)) as s:
            def probe(tag, expr, timeout=60):
                s.type(f":echo '{tag}' . '=' . {expr} . '|'\r")
                return s.expect(rf"{tag}=([^|]*)\|".encode(), timeout).group(1).decode()

            def pyexec(lines, var="g:r"):
                """esp_py_exec() of these lines, output to messages; result in var."""
                s.type(f":let {var} = esp_py_exec({vlist(lines)})\r")
                s.quiet(1.0, timeout=120)

            s.expect("ESPVIM-READY", 90)
            s.quiet(1.5)

            # -- basics ---------------------------------------------------
            t0 = time.time()
            got = probe("E1", "esp_py_eval('6 * 7')", timeout=120)
            check(got == "42", "esp_py_eval() of an expression (Python starts on first use)",
                  f"got {got!r}, {time.time() - t0:.1f} s")
            s.type(":EspPy print('hi' + 'there', 2 ** 70)\r")
            s.expect(rb"hithere 1180591620717411303424", 30)
            check(True, ":EspPy prints (and ints are unbounded)")
            s.type(":EspPy 'value' * 2\r")
            s.expect(rb"'valuevalue'", 30)
            check(True, ":EspPy shows an expression's value, as Python's prompt does")
            s.type(":EspPy counter = 41\r")
            s.quiet(0.5)
            got = probe("E2", "esp_py_eval('counter + 1')")
            check(got == "42", "state carries from one command to the next", f"got {got!r}")

            # -- values, both ways ---------------------------------------
            # (Compared inside Vim: a probe's text is what the screen shows.)
            want = "[1, 2.5, 'x', 0z01FF, {'a': v:none}, (1, 2), v:true, -1099511627776]"
            got = probe("V1", "(string(esp_py_eval(" + vstr(
                "[1, 2.5, 'x', b'\\x01\\xff', {'a': None}, (1, 2), True, -2**40]") + ")) ==# "
                + vstr(want) + ")")
            check(got == "1", "Python values become Vim values", want)
            want = "[1, {'k': 0.25}, 0z0102, v:false, v:none, (3, ), 'é']"   # Vim's string() of a 1-tuple
            got = probe("V2", "(string(esp_py_eval(" + vstr(
                "vim.eval('[1, {\"k\": 0.25}, 0z0102, v:false, v:null, (3,), \"é\"]')") + ")) ==# "
                + vstr(want) + ")")
            check(got == "1", "Vim values become Python values (and back)", want)
            got = probe("V3", "esp_py_eval(" + vstr("vim.call('strlen', 'abcd') + len(vim.call('range', 5))") + ")")
            check(got == "9", "vim.call() calls Vim functions with Python arguments", f"got {got!r}")

            # -- vim.error ------------------------------------------------
            pyexec(["try:",
                    "    vim.command('nosuchcommand')",
                    "except vim.error as e:",
                    "    vim.vars['pyerr'] = str(e)"])
            got = probe("VE", "g:pyerr")
            check("E492" in got, "a failing Vim command raises vim.error with its message", f"got {got!r}")
            got = probe("VO", "g:r.ok")
            check(got == "1", "a caught error is not a failure", f"ok={got!r}")

            # -- buffers, the cursor, variables, options ----------------
            s.type(":enew | call setline(1, ['a', 'b', 'c'])\r")
            s.quiet(0.5)
            pyexec(["b = vim.current.buffer",
                    "b[1] = 'B'",
                    "b[2:] = ['c1', 'c2']",
                    "del b[0]",
                    "b.append('z', 0)",
                    "vim.vars['pylen'] = len(b)"])
            got = probe("B1", "join(getline(1, '$'), ',') . ':' . g:pylen")
            check(got == "z,B,c1,c2:4", "a buffer is a list of lines: index, slice, del, append",
                  f"got {got!r}")
            s.type(":undo\r")
            s.quiet(0.5)
            got = probe("B2", "join(getline(1, '$'), ',')")
            check(got == "a,b,c", "and one :undo takes back the script's changes", f"got {got!r}")
            pyexec(["vim.current.window.cursor = (2, 0)", "vim.vars['pycur'] = list(vim.current.window.cursor)"])
            got = probe("B3", "line('.') . ',' . col('.') . ':' . string(g:pycur)")
            check(got == "2,1:[2, 0]", "the window's cursor can be read and set", f"got {got!r}")
            pyexec(["vim.vars['pyd'] = {'n': [1, 2], 's': 'x'}",
                    "vim.options['shiftwidth'] = 3",
                    "vim.vars['pyts'] = vim.options['tabstop']"])
            got = probe("B4", "(g:pyd == {'n': [1, 2], 's': 'x'}) . ':' . &shiftwidth . ':' . g:pyts")
            check(got == "1:3:8", "vim.vars and vim.options", f"got {got!r}")
            s.type(":bwipe!\r")
            s.quiet(0.5)

            # -- the esp module -------------------------------------------
            got = probe("M1", "esp_py_eval(" + vstr("esp.heap()['internal']['total'] > 0") + ")")
            check(got == "v:true", "esp.heap() is esp_heap()", f"got {got!r}")
            got = probe("M2", "esp_py_eval(" + vstr("hasattr(esp, 'nosuchfunction')") + ")")
            check(got == "v:false", "esp.<name> exists only for real esp_<name>() functions", f"got {got!r}")
            got = probe("M3", "esp_py_eval(" + vstr(
                "__import__('hashlib').sha256(b'abc').digest().hex()[:16] + "
                "__import__('json').dumps({'a': [1]})") + ")")
            check(got == 'ba7816bf8f01cfea{"a": [1]}', "hashlib and json", f"got {got!r}")

            # -- files and imports, relative to Vim's directory ----------
            s.type(":call mkdir('/fat/pyt', 'p') | cd /fat/pyt"
                   " | call writefile(['def hello():', '    return 5'], 'pymod.py')\r")
            s.quiet(1.0)
            pyexec(["import os, pymod",
                    "with open('out.txt', 'w') as f:",
                    "    f.write('from python')",
                    "vim.vars['pyf'] = [os.getcwd(), pymod.hello(), sorted(os.listdir('.'))]"])
            got = probe("F1", "string(g:pyf) . ':' . join(readfile('/fat/pyt/out.txt'))")
            check(got == "['/fat/pyt', 5, ['out.txt', 'pymod.py']]:from python",
                  "open(), os and import work from Vim's current directory", f"got {got!r}")

            # -- :EspPyRun: output window, traceback to quickfix ----------
            s.type(":enew | call setline(1, ['print(\"line\", 1)', 'vim.current.buffer.append(\"added\")',"
                   " 'def f():', '    return 1 / 0', 'f()'])\r")
            s.quiet(0.5)
            src = probe("R0", "bufnr('%')")
            s.type(":EspPyRun\r")
            s.expect(rb"ZeroDivisionError", 60)
            s.quiet(1.0)
            got = probe("R1", "join(getbufline(bufnr('^\\[Python\\]$'), 1, 2), '~')")
            check(got == "line 1~Traceback (most recent call last):",
                  ":EspPyRun output and traceback in the [Python] window", f"got {got!r}")
            got = probe("R2", "join(map(getqflist(), 'v:val.bufnr . \":\" . v:val.lnum'), ',')")
            check(got == f"{src}:4,{src}:5", "the traceback is in quickfix, innermost first", f"got {got!r}")
            got = probe("R3", "getline('$') . ':' . bufnr('%')")
            check(got == f"added:{src}", "the script ran with the edited buffer current", f"got {got!r}")
            s.type(":%bwipe!\r")
            s.quiet(1.0)

            # -- input() --------------------------------------------------
            s.type(":EspPy print('got', input('Name? '))\r")
            s.expect(rb"Name\? ", 30)
            s.type("Ada\r")
            s.expect(rb"got Ada", 30)
            check(True, "input() asks on Vim's command line")

            # -- CTRL-C ---------------------------------------------------
            s.type(":EspPy while True: pass\r")
            time.sleep(3)
            s.send(b"\x03")
            s.expect(rb"KeyboardInterrupt", 30)
            s.quiet(1.0)
            s.type("\r")
            got = probe("C1", "esp_py_eval('1 + 1')")
            check(got == "2", "CTRL-C stops a loop, and Python and Vim carry on", f"got {got!r}")
            s.type(":EspPy import time; time.sleep(60)\r")
            time.sleep(3)
            t0 = time.time()
            s.send(b"\x03")
            s.expect(rb"KeyboardInterrupt", 30)
            check(time.time() - t0 < 10, "CTRL-C stops time.sleep()", f"{time.time() - t0:.1f} s")
            s.quiet(1.0)
            s.type("\r")

            # -- :EspPyRepl ---------------------------------------------------
            s.type(":EspPyRepl\r")
            s.expect(rb">>> ", 60)
            s.quiet(1.0)
            s.type("6*7\r")
            s.quiet(1.0)
            s.type("for i in range(3):\r")
            s.quiet(1.0)
            s.type("print(i*i)\r")
            s.quiet(1.0)
            s.type("\r")                                  # the indentation alone ends it
            s.quiet(1.0)
            s.type("1/0\r")
            s.quiet(1.0)
            s.send(b"\x1bOA")                             # <Up>: the last line typed
            time.sleep(0.5)
            s.type("\r")
            s.quiet(1.0)
            s.type("vim.cur\t")                           # <Tab> completes
            s.quiet(1.0)
            s.send(b"\x1b")
            s.quiet(1.0)
            got = probe("P1", "join(getline(2, 9), '~')")
            want = ">>> 6*7~42~>>> for i in range(3):~...     print(i*i)~...     ~0~1~4"
            check(got == want, ":EspPyRepl runs lines and blocks, with ... and indentation", f"got {got!r}")
            got = probe("P2", "len(filter(getline(1, '$'), 'v:val =~# \"^ZeroDivisionError\"')) . ':' . getline('$')")
            check(got == "2:>>> vim.current", "a traceback shows, <Up> brings the line back, <Tab> completes",
                  f"got {got!r}")
            s.type("A")                                    # CTRL-C drops the line being typed
            s.type("abc")
            s.quiet(1.0)            # typed after a pause, as a person does: Vim reads a CTRL-C
            s.send(b"\x03")        # in a burst of typeahead as an interrupt, mapped or not
            s.quiet(1.0)
            s.send(b"\x1b")
            s.quiet(1.0)
            got = probe("P3", "getline(line('$') - 1) . '~' . getline('$')")
            check(got == "KeyboardInterrupt~>>> ", "CTRL-C at the prompt drops the line", f"got {got!r}")
            s.type("A")                                    # CTRL-C still stops a running line
            s.type("while True: pass\r\r")
            time.sleep(3)
            s.send(b"\x03")
            s.quiet(2.0, timeout=60)
            s.send(b"\x1b")
            s.quiet(1.0)
            got = probe("P5", "getline(line('$') - 1) . '~' . getline('$')")
            check(got.startswith("KeyboardInterrupt") and got.endswith("~>>> "),
                  "CTRL-C stops a loop run from the REPL", f"got {got!r}")
            got = probe("P4", "esp_py_eval('i')")
            check(got == "2", "the REPL shares the interpreter with :EspPy", f"got {got!r}")
            s.type(":bwipe!\r")
            s.quiet(1.0)

            # -- limits -------------------------------------------------------
            pyexec(["def deep(n):", "    return deep(n + 1)", "deep(0)"])
            got = probe("L1", "g:r.error")
            check("RuntimeError" in got and "recursion" in got, "deep recursion raises RuntimeError",
                  f"got {got!r}")
            pyexec(["x = bytearray(64 * 1024 * 1024)"])
            got = probe("L2", "g:r.error")
            check(got.startswith("MemoryError"), "a script that runs out of heap gets MemoryError",
                  f"got {got!r}")

            # -- :EspPyReset ----------------------------------------------
            s.type(":EspPy keep = 1\r:EspPyReset\r")
            s.quiet(1.0)
            pyexec(["vim.vars['pyk'] = 'keep' in globals()"])
            got = probe("Z1", "g:pyk . ':' . esp_py_heap().running")
            check(got == "v:false:1", ":EspPyReset gives a new interpreter", f"got {got!r}")
            s.type(":EspHeap\r")
            s.expect(rb"Python", 30)
            check(True, ":EspHeap shows the Python heap")
            s.quiet(1.0)

            # -- a new Vim session (after :q) gets a new interpreter ------
            s.type(":EspPy keep = 2\r:qa!\r")
            s.expect(rb"ESPVIM-EXIT rc=0 ", 60)
            s.expect(rb"Press any key to start a new session\.", 30)
            s.quiet(0.5)
            s.send(b"x")
            s.expect("ESPVIM-READY", 90)
            s.quiet(1.5)
            got = probe("S1", "esp_py_eval(" + vstr("'keep' in globals()") + ") . ':' . esp_py_eval('3 * 3')")
            check(got == "v:false:9", "after :q, the next session starts Python afresh", f"got {got!r}")

            s.type(":qa!\r")
            s.expect(rb"ESPVIM-EXIT rc=0 ", 60)

        data = log.read_bytes()
        panics = len(re.findall(rb"Guru Meditation|abort\(\) was called|assert failed", data))
        check(panics == 0, "no crash", f"{panics} panics")
    except Exception as e:
        check(False, "session completed", f"{type(e).__name__}: {e}")

    if failures:
        print(f"py: {failures} check(s) FAILED -- UART log kept at {log}")
        sys.exit(1)
    log.unlink()
    print("py: all checks passed")


if __name__ == "__main__":
    main()
