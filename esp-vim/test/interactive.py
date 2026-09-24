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

import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from uart_session import Session, TARGET, PSRAM  # noqa: E402

failures = 0


# Every value read back from the screen ends in an explicit terminator ('|').
# The UART delivers output in arbitrary chunks, and a bare \d+ happily matches
# "40x12" while the final "0" of 120 is still in flight.


def check(ok, label, detail=""):
    global failures
    print(f"  {'PASS' if ok else 'FAIL'}  {label}{(' -- ' + detail) if detail else ''}")
    if not ok:
        failures += 1


def main():
    log = Path(tempfile.mkstemp(prefix="vim-interactive-", suffix=".log")[1])
    print(f"interactive session (target: {TARGET}, PSRAM {PSRAM[TARGET]})")
    try:
        with Session(term_size=(40, 120), log=str(log)) as s:
            term = s.expect(rb"ESPVIM-TERM ([^\r\n]*\))", 60).group(1).decode()
            s.expect("ESPVIM-READY", 60)
            s.quiet(1.5)

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
            s.expect(rb"ESPVIM-EXIT rc=0", 30)
            check(True, "clean exit after interactive use")

        data = log.read_bytes()
        # 5. No "OOPS": Vim's fallback tgoto() emits that for a termcap string
        #    it cannot expand (patch 0006 fixed the one that did).
        check(b"OOPS" not in data, "no unexpandable termcap strings (\"OOPS\")",
              f"{data.count(b'OOPS')} found" if b"OOPS" in data else "")
        # 6. Mouse reporting enabled (button tracking + SGR) and disabled at exit.
        on = b"\x1b[?1002h" in data and b"\x1b[?1006h" in data
        off = b"\x1b[?1002l" in data and b"\x1b[?1006l" in data
        check(on and off, "SGR mouse reporting enabled, and disabled on exit")
        # 7. No Vim error messages anywhere in the session.
        import re
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
