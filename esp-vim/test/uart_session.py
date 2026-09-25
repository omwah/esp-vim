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
EMU = PROJECT.parent / "build-deps" / "esp-emu" / "esp-emu"
C6_IMAGE = PROJECT.parent / "build-deps" / "c6-coprocessor" / "build" / "merged.bin"

# The build variant under test (scripts/vim-build.sh), the chip it runs on, and
# the PSRAM its emulated board gets. "tab5" is the P4 with its WiFi through an
# ESP32-C6 co-processor, emulated as a second esp-emu linked by esp-hosted's
# SDIO bridge (scripts/c6-build.sh makes the C6 firmware).
VARIANT = os.environ.get("ESPVIM_TARGET", "esp32p4")
TARGET = {"tab5": "esp32p4", "es3c28p": "esp32s3"}.get(VARIANT, VARIANT)
COPROCESSOR = VARIANT == "tab5"
PSRAM = {"esp32p4": "32M", "esp32s3": "8M"}

# A real board instead of the emulator: ESPVIM_DEVICE=<serial port>, with the
# matching build flashed. Each session erases the board's storage and NVS and
# resets it, so it starts as fresh as an emulator run. Tests that need the
# emulator's extras (--inject, --uart1-tcp, the soft AP, Bumble) skip.
# ESPVIM_WIFI=ssid:password lets network checks join a real network.
DEVICE = os.environ.get("ESPVIM_DEVICE")


class _SerialSock:
    """Just enough of a socket over a serial port for Session."""

    def __init__(self, port):
        import serial                       # pyserial: ESP-IDF's environment has it
        self.port = serial.Serial(port, 115200, timeout=0.2)

    def recv(self, n):
        data = self.port.read(n)
        if not data:
            raise socket.timeout()
        return data

    def sendall(self, data):
        self.port.write(data)

    def close(self):
        self.port.close()


def _device_fresh_start(port):
    """Erase storage and NVS (the emulator starts from a pristine image too).
    parttool ends with esptool's reset into the app: a hand-made DTR/RTS pulse
    on the USB Serial/JTAG port leaves the chip in download mode instead."""
    parttool = os.path.join(os.environ.get("IDF_PATH", ""),
                            "components", "partition_table", "parttool.py")
    for part in ("storage", "nvs"):
        subprocess.run([sys.executable, parttool, "--port", port,
                        "erase_partition", "--partition-name", part],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


# Options that belong to the radio: on a co-processor build they go to the C6.
RADIO_OPTS = {"--wifi-ssid", "--wifi-password"}
# esp-emu's soft access point when not given --wifi-ssid / --wifi-password.
EMU_AP = ("myssid", "mypassword")


class Session:
    def __init__(self, port=None, chip=None, psram=None, extra=(), term_size=None,
                 reuse=False, save_state=False, log=None, hostfwd=()):
        # A free port by default: back-to-back sessions (the gate runs several)
        # must not wait for the previous emulator to let go of a fixed one.
        if port is None:
            with socket.socket() as so:
                so.bind(("127.0.0.1", 0))
                port = so.getsockname()[1]
        self.port = port
        chip = chip or TARGET
        psram = psram or PSRAM[chip]
        self.term_size = term_size          # (rows, cols) to answer CSI 6n with, or None
        self.buf = b""
        self._scan = b""        # rolling tail: a query can straddle two recv()s
        self.log = open(log, "wb") if log else None
        self.c6 = self._tmp = self.proc = None
        if DEVICE:
            _device_fresh_start(DEVICE)     # ends with a reset into the app
            self.sock = _SerialSock(DEVICE)
            return
        args = [str(RUN_EMU), str(PROJECT), "--chip", chip, "--variant", VARIANT,
                "--psram", psram, "--timeout", "1500s"]
        if reuse:
            args.append("--reuse")
        if save_state:
            args.append("--save-state")
        # --net user: slirp networking with no host setup. The P4's Ethernet
        # comes up on it (DHCP gives 192.168.4.2), and the host's 127.0.0.1 is
        # reachable from the device as the gateway, 192.168.4.1.
        # hostfwd: (host_port, device_port) pairs, so the host can reach a
        # server on the device (QEMU syntax; esp-emu supports it undocumented).
        net = "user" + "".join(f",hostfwd=tcp:127.0.0.1:{h}-:{g}" for h, g in hostfwd)
        if COPROCESSOR:
            # The network is the C6's: slirp and the soft AP attach to it, and
            # the P4 reaches both through esp-hosted. Slave first, then host.
            radio, rest, it = [], [], iter(extra)
            for a in it:
                if a in RADIO_OPTS:
                    radio += [a, next(it)]
                else:
                    rest.append(a)
            self._tmp = tempfile.mkdtemp(prefix="espvim-hosted-")
            sock_path = os.path.join(self._tmp, "hosted.sock")
            if not C6_IMAGE.exists():
                raise RuntimeError(f"no C6 firmware at {C6_IMAGE} -- run scripts/c6-build.sh")
            self.c6 = subprocess.Popen(
                [str(EMU), "--chip", "esp32c6", "--firmware", str(C6_IMAGE),
                 "--hosted", f"bridge:slave:{sock_path}", "--net", net, *radio],
                stdin=subprocess.PIPE, stdout=self._c6_log(log), stderr=subprocess.STDOUT,
                start_new_session=True)
            for _ in range(100):
                if os.path.exists(sock_path) or self.c6.poll() is not None:
                    break
                time.sleep(0.1)
            if not os.path.exists(sock_path):
                self._kill(self.c6)
                raise RuntimeError("the C6 emulator did not open its esp-hosted socket")
            args += ["--", "--uart-tcp", f"127.0.0.1:{port}",
                     "--hosted", f"bridge:host:{sock_path}", *rest]
        else:
            args += ["--", "--uart-tcp", f"127.0.0.1:{port}", "--net", net, *extra]
        # stdin held open: an immediate EOF would reach the emulator's console.
        self.proc = subprocess.Popen(args, stdin=subprocess.PIPE,
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                     start_new_session=True)
        try:
            self.sock = self._connect()
        except Exception:
            self.sock = None
            self.close()
            raise

    @staticmethod
    def _c6_log(log):
        """The C6's console, next to the UART log when there is one."""
        return open(f"{log}.c6", "wb") if log else subprocess.DEVNULL

    def _connect(self, deadline=180):
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

    # Vim hides and shows the cursor every time a timer wakes it (the serial
    # window polls ten times a second): that alone is not "the device is busy".
    # (A chunk can split a sequence, so test the bytes, not whole sequences.)
    _CURSOR_ONLY = re.compile(rb"^[\x1b\[?25hl]+$")

    def quiet(self, settle=1.0, timeout=30.0):
        """Wait until the device has been silent for `settle` seconds (screen drawn)."""
        end = time.time() + timeout
        last = time.time()
        while time.time() < end:
            before = len(self.buf)
            if self._pump() and not self._CURSOR_ONLY.match(self.buf[before:]):
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

    def join_wifi(self, timeout=90):
        """Make sure the device has a network; returns whether it does. On a
        WiFi build, join esp-emu's default soft AP (or on a real board, the
        network in ESPVIM_WIFI), as :EspWifiConnect would, and wait for an
        address. Other builds are taken as networked (Ethernet under --net)."""
        def probe(tag, expr):
            self.type(f":echo '{tag}' . '=' . {expr} . '|'\r")
            return self.expect(rf"{tag}=([^|]*)\|".encode(), 60).group(1).decode()

        iface = probe("WI", "esp_net_status().iface")
        if iface == "none":
            return False
        if iface != "wifi":
            return not DEVICE
        if DEVICE:
            if ":" not in os.environ.get("ESPVIM_WIFI", ""):
                return False
            ssid, password = os.environ["ESPVIM_WIFI"].split(":", 1)
        else:
            ssid, password = EMU_AP
        self.type(f":call esp_wifi_connect('{ssid}', '{password}')\r")
        end = time.time() + timeout
        while time.time() < end:
            if probe("WU", "(esp_net_status().up ? 1 : 0)") == "1":
                return True
            time.sleep(1)
        raise TimeoutError(f"WiFi did not come up on '{ssid}'")

    def close(self):
        try:
            if self.sock:
                self.sock.close()
        except OSError:
            pass
        if self.proc:
            self._kill(self.proc)
        if self.c6:
            self._kill(self.c6)
        if self._tmp:
            shutil.rmtree(self._tmp, ignore_errors=True)
        if self.log:
            self.log.close()

    @staticmethod
    def _kill(proc):
        try:
            os.killpg(proc.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        proc.wait(timeout=20)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
