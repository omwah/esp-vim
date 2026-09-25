#!/usr/bin/env python3
"""
Peripheral gate (Phase 6f): :EspSerial against a host socket bridged to UART1
(esp-emu --uart1-tcp), and :EspI2cScan / :EspSensors / :EspAdc as far as the
emulator models those peripherals -- correct answers or clean errors, never a
hang or a crash. One emulator session.
"""

import re
import socket
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from uart_session import Session, TARGET, DEVICE  # noqa: E402

failures = 0
# An ADC-capable pin, and one that is not, per chip.
ADC_PIN = {"esp32p4": 16, "esp32s3": 1}[TARGET]
NOT_ADC = {"esp32p4": 5, "esp32s3": 21}[TARGET]
# Free pins for an I2C bus in the emulator build. (The P4 default, GPIO31/32,
# is the Tab5's internal bus -- but the emulator build enables the Ethernet
# MAC, whose RMII pins include GPIO31, so it is correctly refused there.)
I2C_PINS = {"esp32p4": "7, 8", "esp32s3": "8, 9"}[TARGET]


def check(ok, label, detail=""):
    global failures
    print(f"  {'PASS' if ok else 'FAIL'}  {label}{(' -- ' + detail) if detail else ''}")
    if not ok:
        failures += 1


def free_port():
    with socket.socket() as so:
        so.bind(("127.0.0.1", 0))
        return so.getsockname()[1]


def main():
    if DEVICE:
        print("  SKIP  hw: needs the emulator (run on a board: ESPVIM_DEVICE is set)")
        return
    log = Path(tempfile.mkstemp(prefix="vim-hw-", suffix=".log")[1])
    uport = free_port()
    print(f"hardware session (target: {TARGET}, UART1 on 127.0.0.1:{uport})")
    try:
        with Session(term_size=(40, 120), log=str(log), extra=("--uart1-tcp", f"127.0.0.1:{uport}")) as s:
            def probe(tag, expr):
                s.type(f":echo '{tag}' . '=' . {expr} . '|'\r")
                return s.expect(rf"{tag}=([^|]*)\|".encode(), 60).group(1).decode()

            s.expect("ESPVIM-READY", 90)
            s.quiet(1.5)

            # Serial: the host end of UART1 is a TCP socket.
            host = socket.create_connection(("127.0.0.1", uport), timeout=30)
            s.type(":EspSerial 1 115200\r")
            s.quiet(1.5)
            got = probe("SB", "bufname('%') . ':' . get(b:, 'esp_serial_port', 0)")
            check(got == "EspSerial-1:1", ":EspSerial opens UART1 in a window", f"got {got!r}")
            host.sendall(b"hello from the host\r\nsecond line\r\n")
            time.sleep(2)
            got = probe("SR", "join(getline(1, '$'), '~')")
            check("hello from the host~second line" in got, "what arrives on the port appears in the window",
                  f"buffer {got!r}")
            s.type(":EspSerialSend ping from vim\r")
            host.settimeout(15)
            data = b""
            try:
                while b"\r\n" not in data:
                    data += host.recv(100)
            except socket.timeout:
                pass
            check(data == b"ping from vim\r\n", ":EspSerialSend writes to the port", f"host got {data!r}")
            s.type(":call esp#hw#SerialClose(1)\r")
            s.quiet(1.0)
            got = probe("SC", "bufnr('EspSerial-1')")
            check(got == "-1", "closing the port closes its window", f"bufnr {got!r}")
            s.type(":let v:errmsg = '' | silent! call esp_serial_open(0, 115200)\r")
            s.quiet(0.5)
            got = probe("S0", "(v:errmsg =~# 'console')")
            check(got == "1", "UART0, the console, is refused", f"got {got!r}")
            host.close()

            # I2C: a scan finishes (the emulator's buses are empty).
            s.type(f":let v:errmsg = '' | silent! let g:d = esp_i2c_scan() | let g:dflt = v:errmsg"
                   f" | let v:errmsg = '' | let g:i2c = esp_i2c_scan({I2C_PINS})\r")
            s.quiet(2.0, timeout=120)
            got = probe("IC", "type(g:i2c) . ':' . v:errmsg")
            check(got == "3:", "an I2C scan runs to the end without error", f"type:error {got!r}")
            s.type(f":let v:errmsg = '' | let g:sn = esp_sensors({I2C_PINS})\r")
            s.quiet(2.0, timeout=120)
            got = probe("SN", "type(g:sn) . ':' . v:errmsg")
            check(got == "3:", "sensor identification runs without error", f"type:error {got!r}")

            # ADC: a real ADC pin reads (or errors cleanly); a non-ADC pin is refused.
            s.type(f":let v:errmsg = '' | silent! let g:adc = esp_adc_read({ADC_PIN})\r")
            s.quiet(1.5)
            got = probe("AD", "(exists('g:adc') && has_key(g:adc, 'raw') ? 'raw' : 'err:' . v:errmsg)")
            check(got == "raw" or got.startswith("err:esp_adc_read"), "an ADC pin reads, or fails cleanly",
                  f"got {got!r}")
            s.type(f":let v:errmsg = '' | silent! call esp_adc_read({NOT_ADC})\r")
            s.quiet(1.0)
            got = probe("NA", "(v:errmsg =~# 'not an ADC pin')")
            check(got == "1", "a pin with no ADC is refused", f"got {got!r}")

            s.type(":qa!\r")
            s.expect(rb"ESPVIM-EXIT rc=0 ", 60)

        data = log.read_bytes()
        panics = len(re.findall(rb"Guru Meditation|abort\(\) was called|assert failed", data))
        check(panics == 0, "no crash", f"{panics} panics")
    except Exception as e:
        check(False, "session completed", f"{type(e).__name__}: {e}")

    if failures:
        print(f"hw: {failures} check(s) FAILED -- UART log kept at {log}")
        sys.exit(1)
    log.unlink()
    print("hw: all checks passed")


if __name__ == "__main__":
    main()
