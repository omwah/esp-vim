#!/usr/bin/env python3
"""
WiFi gate (Phase 6f): scan, connect, use and forget a network, against the
soft access point esp-emu provides (--wifi-ssid / --wifi-password, defaults
myssid / mypassword). Native on the ESP32-S3; on the tab5 variant through an
emulated ESP32-C6 co-processor (esp-hosted over SDIO). Skipped on builds without
WiFi (the plain P4 build uses Ethernet).
"""

import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from uart_session import Session, VARIANT, DEVICE  # noqa: E402
from interactive import start_http_server  # noqa: E402

failures = 0
SSID, PASSWORD = "esp-vim-test", "correct-horse"


def check(ok, label, detail=""):
    global failures
    print(f"  {'PASS' if ok else 'FAIL'}  {label}{(' -- ' + detail) if detail else ''}")
    if not ok:
        failures += 1


def main():
    if DEVICE:
        print("  SKIP  wifi: needs the emulator (run on a board: ESPVIM_DEVICE is set)")
        return
    log = Path(tempfile.mkstemp(prefix="vim-wifi-", suffix=".log")[1])
    www = Path(tempfile.mkdtemp(prefix="vim-www-"))
    (www / "hello.txt").write_text("over wifi\n")
    srv, base = start_http_server(www)
    print(f"wifi session (variant: {VARIANT}, soft AP '{SSID}')")
    try:
        with Session(term_size=(40, 120), log=str(log),
                     extra=("--wifi-ssid", SSID, "--wifi-password", PASSWORD)) as s:
            def probe(tag, expr, timeout=60):
                s.type(f":echo '{tag}' . '=' . {expr} . '|'\r")
                return s.expect(rf"{tag}=([^|]*)\|".encode(), timeout).group(1).decode()

            s.expect("ESPVIM-READY", 120)
            s.quiet(2.0)
            if probe("IF", "esp_net_status().iface") != "wifi":
                print("  SKIP  wifi: this build's network is not WiFi")
                return

            s.type(":let g:aps = esp_wifi_scan()\r")
            s.quiet(3.0, timeout=120)
            got = probe("SC", "join(map(copy(g:aps), 'v:val.ssid . \"/\" . v:val.auth'), ',')")
            check(SSID + "/wpa2" in got, "a scan finds the access point, with its security", got)
            s.type(f":EspWifiConnect {SSID} {PASSWORD}\r")
            s.quiet(3.0, timeout=120)
            got = ""
            for _ in range(30):
                got = probe("UP", "(esp_net_status().up ? 1 : 0) . ':' . esp_net_status().ssid . ':' . esp_net_status().ip")
                if got.startswith("1:"):
                    break
                time.sleep(1)
            check(got.startswith(f"1:{SSID}:"), ":EspWifiConnect joins the network and gets an address", got)
            s.type(f":call esp_http_get('{base}/hello.txt', '/fat/w.txt')\r")
            s.quiet(2.0, timeout=120)
            got = probe("HG", "join(readfile('/fat/w.txt'))")
            check(got == "over wifi", "HTTP works over WiFi", f"got {got!r}")
            s.type(":EspWifiDisconnect\r")
            s.quiet(3.0, timeout=60)
            got = probe("DC", "(esp_net_status().up ? 1 : 0) . ':' . esp_net_status().ssid")
            check(got == "0:", ":EspWifiDisconnect leaves and forgets the network", f"up:ssid {got!r}")
    except Exception as e:
        check(False, "session completed", f"{type(e).__name__}: {e}")
    finally:
        srv.shutdown()

    if not failures:
        for f in (log, Path(f"{log}.c6")):
            f.unlink(missing_ok=True)

    if failures:
        print(f"wifi: {failures} check(s) FAILED -- UART log kept at {log}")
        sys.exit(1)
    print("wifi: all checks passed")


if __name__ == "__main__":
    main()
