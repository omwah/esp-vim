#!/usr/bin/env python3
"""
Drive Vim interactively over esp-emu's UART-over-TCP bridge.

--inject can only fire bytes the instant a marker appears, before Vim has drawn
anything -- and Vim deliberately skips redraws while typeahead is pending. Real
use is different: the screen is up, THEN a person types, sometimes while Vim is
busy. This is a tiny expect() over the --uart-tcp socket that can do that, and
can also play the part of a terminal (answer a cursor-position query).

Library use (see phase5_checks() below), or run directly for the Phase 5 checks:
    esp-vim/test/uart_session.py            # from an env with idf.py (pixi run ...)
"""

import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
PROJECT = HERE.parent
RUN_EMU = PROJECT.parent / "scripts" / "run-emu.sh"

# The chip under test, and the PSRAM its emulated board gets.
TARGET = os.environ.get("ESPVIM_TARGET", "esp32p4")
PSRAM = {"esp32p4": "32M", "esp32s3": "8M"}


class Session:
    def __init__(self, port=5599, chip=None, psram=None, extra=(), term_size=None,
                 reuse=False, save_state=False, log=None):
        self.port = port
        chip = chip or TARGET
        psram = psram or PSRAM[chip]
        self.term_size = term_size          # (rows, cols) to answer CSI 6n with, or None
        self.buf = b""
        self._scan = b""        # rolling tail: a query can straddle two recv()s
        self.log = open(log, "wb") if log else None
        args = [str(RUN_EMU), str(PROJECT), "--chip", chip, "--psram", psram,
                "--timeout", "180s"]
        if reuse:
            args.append("--reuse")
        if save_state:
            args.append("--save-state")
        args += ["--", "--uart-tcp", f"127.0.0.1:{port}", *extra]
        # stdin held open: an immediate EOF would reach the emulator's console.
        self.proc = subprocess.Popen(args, stdin=subprocess.PIPE,
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                     start_new_session=True)
        self.sock = self._connect()

    def _connect(self, deadline=60):
        end = time.time() + deadline
        while time.time() < end:
            try:
                s = socket.create_connection(("127.0.0.1", self.port), timeout=1)
                s.settimeout(0.2)
                return s
            except OSError:
                if self.proc.poll() is not None:
                    raise RuntimeError("emulator exited before the UART socket opened")
                time.sleep(0.2)
        raise RuntimeError("could not connect to the emulator's UART socket")

    def _pump(self):
        try:
            data = self.sock.recv(65536)
        except socket.timeout:
            return False
        if not data:
            raise RuntimeError("UART socket closed")
        if self.log:
            self.log.write(data)
            self.log.flush()
        self.buf += data
        # Behave like a terminal for the firmware's size probe: it parks the
        # cursor at 999;999 and asks where it is (CSI 6n). Answer only that
        # query -- Vim sends its own CSI 6n later (the 'ambiwidth' check), and a
        # size-shaped reply there would be wrong.
        self._scan = (self._scan + data)[-64:]
        if self.term_size and b"\x1b[999;999H\x1b[6n" in self._scan:
            self._scan = b""
            rows, cols = self.term_size
            self.send(f"\x1b[{rows};{cols}R".encode())
        return True

    def expect(self, pattern, timeout=30.0):
        """Wait for a regex (bytes) in output since the last expect; return the match."""
        rx = re.compile(pattern if isinstance(pattern, bytes) else pattern.encode(), re.S)
        end = time.time() + timeout
        while True:
            m = rx.search(self.buf)
            if m:
                self.buf = self.buf[m.end():]
                return m
            if time.time() > end:
                tail = self.buf[-300:]
                raise TimeoutError(f"timed out waiting for {pattern!r}; last output: {tail!r}")
            self._pump()

    def quiet(self, settle=1.0, timeout=30.0):
        """Wait until the device has been silent for `settle` seconds (screen drawn)."""
        end = time.time() + timeout
        last = time.time()
        while time.time() < end:
            if self._pump():
                last = time.time()
            elif time.time() - last >= settle:
                return
        raise TimeoutError("device never went quiet")

    def send(self, data):
        if isinstance(data, str):
            data = data.encode()
        self.sock.sendall(data)

    def type(self, text, delay=0.02):
        """Send like a person would: one key at a time, so Vim reads each separately."""
        for ch in text.encode() if isinstance(text, str) else text:
            self.send(bytes([ch]))
            time.sleep(delay)

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass
        try:
            os.killpg(self.proc.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        self.proc.wait(timeout=20)
        if self.log:
            self.log.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
