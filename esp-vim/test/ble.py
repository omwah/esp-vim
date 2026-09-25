#!/usr/bin/env python3
"""
Bluetooth LE gate (Phase 6f): the device scans and finds a peripheral.

esp-emu hands the firmware's Bluetooth controller to a TCP HCI server
(--ble-hci); ble_peer.py is that server, a Bumble virtual controller sharing a
virtual radio link with a Bumble peripheral that advertises a known name from a
known address. Skipped on builds without Bluetooth (the P4 builds: the Tab5's
Bluetooth is on its ESP32-C6).
"""

import os
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from uart_session import Session, VARIANT, PROJECT, DEVICE  # noqa: E402

HERE = Path(__file__).resolve().parent
PIXI_PYTHON = PROJECT.parent / ".pixi" / "envs" / "default" / "bin" / "python"
BUILD = PROJECT / f"build-{VARIANT}"
NAME, ADDR = "esp-vim-beacon", "F0:F1:F2:F3:F4:F5"

failures = 0


def check(ok, label, detail=""):
    global failures
    print(f"  {'PASS' if ok else 'FAIL'}  {label}{(' -- ' + detail) if detail else ''}")
    if not ok:
        failures += 1


def free_port():
    with socket.socket() as so:
        so.bind(("127.0.0.1", 0))
        return so.getsockname()[1]


def start_peer(log):
    """ble_peer.py on a free port, in its own session; returns (proc, port)."""
    port = free_port()
    proc = subprocess.Popen([str(PIXI_PYTHON), str(HERE / "ble_peer.py"), str(port), NAME, ADDR],
                            stdout=subprocess.PIPE, stderr=open(log, "wb"), text=True,
                            start_new_session=True)
    if proc.stdout.readline().strip() != "BLE-PEER-READY":
        proc.kill()
        raise RuntimeError(f"ble_peer.py did not start -- see {log}")
    return proc, port


def main():
    if DEVICE:
        print("  SKIP  ble: needs the emulator (run on a board: ESPVIM_DEVICE is set)")
        return
    config = (BUILD / "sdkconfig").read_text() if (BUILD / "sdkconfig").exists() else ""
    if "CONFIG_BT_NIMBLE_ENABLED=y" not in config:
        print(f"ble (variant: {VARIANT})")
        print("  SKIP  ble: this build has no Bluetooth")
        return
    log = Path(tempfile.mkstemp(prefix="vim-ble-", suffix=".log")[1])
    peer_log = Path(f"{log}.peer")
    peer, port = start_peer(peer_log)
    print(f"ble session (variant: {VARIANT}, Bumble controller on 127.0.0.1:{port})")
    try:
        with Session(term_size=(40, 120), log=str(log),
                     extra=("--elf", str(BUILD / "esp-vim.elf"),
                            "--ble-hci", f"tcp:127.0.0.1:{port}")) as s:
            def probe(tag, expr, timeout=90):
                s.type(f":echo '{tag}' . '=' . {expr} . '|'\r")
                return s.expect(rf"{tag}=([^|]*)\|".encode(), timeout).group(1).decode()

            s.expect("ESPVIM-READY", 120)
            s.quiet(2.0)
            got = probe("SC", "join(map(esp_ble_scan(3), "
                              "'v:val.name . \"/\" . v:val.addr . \"/\" . v:val.addr_type'), ',')")
            want = f"{NAME}/{ADDR.lower()}/public"
            check(want in got.split(","), "esp_ble_scan() finds the peripheral, "
                  "with its name, address and address type", got)
            s.type(":EspBleScan 2\r")
            try:
                s.expect(rf"{NAME}\s+{ADDR.lower()}\s+public".encode(), 90)
                check(True, ":EspBleScan lists it")
            except TimeoutError as e:
                check(False, ":EspBleScan lists it", str(e)[-200:])
            s.type("q")
            s.quiet(1.0)
            got = probe("ER", "string(esp_ble_scan(0))")
            check(got == "[]", "a scan time out of range is refused", got)
    except Exception as e:
        check(False, "session completed", f"{type(e).__name__}: {e}")
    finally:
        os.killpg(peer.pid, 15)
        peer.wait(timeout=10)

    if failures:
        print(f"ble: {failures} check(s) FAILED -- UART log kept at {log}")
        sys.exit(1)
    for f in (log, peer_log):
        f.unlink(missing_ok=True)
    print("ble: all checks passed")


if __name__ == "__main__":
    main()
