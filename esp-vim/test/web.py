#!/usr/bin/env python3
"""
Web interface gate (Phase 6e): the device's HTTPS server driven from the host,
as a browser would, through esp-emu's port forwarding.

Checks the security model (no password -> no server, certificate fingerprint,
login backoff, sessions, CSRF, path validation), the file operations, settings
reaching Vim, and the live status. One emulator session.
"""

import hashlib
import http.client
import json
import socket
import ssl
import sys
import tempfile
import time
import urllib.parse
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from uart_session import Session, TARGET  # noqa: E402

failures = 0
CHIP = "ESP32-" + TARGET[len("esp32"):].upper()


def check(ok, label, detail=""):
    global failures
    print(f"  {'PASS' if ok else 'FAIL'}  {label}{(' -- ' + detail) if detail else ''}")
    if not ok:
        failures += 1


class Browser:
    """Just enough of a browser: one cookie, the CSRF token, HTTPS without CA
    checking (the certificate is self-signed; its fingerprint is checked)."""

    def __init__(self, port):
        self.port = port
        self.cookie = None
        self.csrf = ""
        self.ctx = ssl.create_default_context()
        self.ctx.check_hostname = False
        self.ctx.verify_mode = ssl.CERT_NONE

    def fingerprint(self):
        with socket.create_connection(("127.0.0.1", self.port), timeout=60) as raw:
            with self.ctx.wrap_socket(raw) as tls:
                der = tls.getpeercert(binary_form=True)
        d = hashlib.sha256(der).hexdigest().upper()
        return ":".join(d[i:i + 2] for i in range(0, 64, 2))

    def request(self, method, path, body=None, csrf=True, raw=False):
        c = http.client.HTTPSConnection("127.0.0.1", self.port, context=self.ctx, timeout=120)
        h = {}
        if self.cookie:
            h["Cookie"] = self.cookie
        if csrf and method != "GET" and self.csrf:
            h["X-CSRF-Token"] = self.csrf
        if body is not None and not raw:
            body = json.dumps(body)
            h["Content-Type"] = "application/json"
        c.request(method, path, body=body, headers=h)
        r = c.getresponse()
        data = r.read()
        for k, v in r.getheaders():
            if k.lower() == "set-cookie" and v.startswith("sid="):
                self.cookie = v.split(";")[0] if "Max-Age=0" not in v else None
        c.close()
        try:
            j = json.loads(data)
        except ValueError:
            j = None
        return r.status, j, data


def q(p):
    return urllib.parse.quote(p, safe="")


def main():
    log = Path(tempfile.mkstemp(prefix="vim-web-", suffix=".log")[1])
    with socket.socket() as so:
        so.bind(("127.0.0.1", 0))
        hport = so.getsockname()[1]
    print(f"web session (target: {TARGET}, host port {hport} -> device 443)")
    try:
        with Session(term_size=(40, 120), log=str(log), hostfwd=[(hport, 443)]) as s:
            def probe(tag, expr):
                s.type(f":echo '{tag}' . '=' . {expr} . '|'\r")
                return s.expect(rf"{tag}=([^|]*)\|".encode(), 60).group(1).decode()

            s.expect("ESPVIM-READY", 90)
            s.quiet(1.5)
            if probe("IF", "esp_net_status().iface") == "none":
                print("  SKIP  web interface: this build has no network interface yet")
                log.unlink()
                return
            s.join_wifi()
            for _ in range(30):
                if probe("NU", "(esp_net_status().up ? 1 : 0)") == "1":
                    break
                time.sleep(1)

            # 1. No password, no server; a short password is refused.
            s.type(":let v:errmsg = '' | silent! let g:w = esp_web_start()\r")
            s.quiet(1.0)
            got = probe("NP", "(v:errmsg =~# 'no password') . (esp_web_info().running ? 1 : 0)")
            check(got == "10", "refuses to start without a password", f"error:running {got!r}")
            s.type(":let v:errmsg = '' | silent! call esp_web_passwd('short')\r")
            s.quiet(1.0)
            got = probe("SP", "(v:errmsg =~# 'at least 8') . (esp_web_info().password_set ? 1 : 0)")
            check(got == "10", "refuses a password under 8 characters", f"error:set {got!r}")

            # 2. Start; the certificate the host sees is the one Vim describes.
            # As a user would: :EspWebStart also starts the tick that applies
            # settings and publishes the status.
            s.type(":call esp_web_passwd('correct horse battery') | EspWebStart\r")
            s.quiet(3.0, timeout=240)
            fp_dev = probe("FP", "esp_web_info().fingerprint")
            b = Browser(hport)
            fp_host = b.fingerprint()
            check(fp_dev == fp_host and len(fp_host) == 95,
                  "certificate fingerprint shown in Vim matches the one served", f"{fp_dev[:23]}...")

            # 3. Logged out: the page loads, the API does not.
            st, _, page = b.request("GET", "/")
            check(st == 200 and b"esp-vim" in page, "the page is served", f"status {st}")
            st, j, _ = b.request("GET", "/api/session")
            check(st == 200 and j["logged_in"] is False and j["chip"] == CHIP,
                  "a fresh browser is not logged in", str(j))
            st, _, _ = b.request("GET", "/api/list?path=" + q("/fat"))
            check(st == 401, "the API refuses a browser that has not logged in", f"status {st}")

            # 4. Wrong password, then back-off, then the right one.
            st, _, _ = b.request("POST", "/api/login", {"password": "wrong password"})
            check(st == 401, "a wrong password is refused", f"status {st}")
            st, j, _ = b.request("POST", "/api/login", {"password": "correct horse battery"})
            check(st == 429, "logins are refused for a while after a failure", f"status {st} {j}")
            time.sleep(2.5)
            st, j, _ = b.request("POST", "/api/login", {"password": "correct horse battery"})
            ok = st == 200 and bool(b.cookie) and len(j.get("csrf", "")) == 32
            check(ok, "the right password logs in, with a cookie and a CSRF token", f"status {st}")
            b.csrf = j.get("csrf", "")

            # 5. Files.
            s.type(":call mkdir('/fat/web', 'p') | call writefile(['from vim'], '/fat/web/device.txt')\r")
            s.quiet(1.0)
            st, j, _ = b.request("GET", "/api/list?path=" + q("/fat/web"))
            names = [e["name"] for e in (j or {}).get("entries", [])]
            check(st == 200 and names == ["device.txt"], "lists a directory", f"{st} {names}")
            st, _, _ = b.request("PUT", "/api/file?path=" + q("/fat/web/up.txt"), b"uploaded\n", csrf=False, raw=True)
            check(st == 403, "a change without the CSRF token is refused", f"status {st}")
            st, _, _ = b.request("PUT", "/api/file?path=" + q("/fat/web/up.txt"), b"uploaded\n", raw=True)
            got = probe("UP", "join(readfile('/fat/web/up.txt'))")
            check(st == 200 and got == "uploaded", "uploads a file", f"{st} {got!r}")
            st, _, _ = b.request("PUT", "/api/file?path=" + q("/fat/web/up.txt"), b"again\n", raw=True)
            st2, _, _ = b.request("PUT", "/api/file?path=" + q("/fat/web/up.txt") + "&overwrite=1", b"again\n", raw=True)
            got = probe("OW", "join(readfile('/fat/web/up.txt'))")
            check(st == 409 and st2 == 200 and got == "again", "replaces a file only when asked",
                  f"{st}/{st2} {got!r}")
            st, _, data = b.request("GET", "/api/file?path=" + q("/fat/web/device.txt"))
            check(st == 200 and data == b"from vim\n", "downloads a file", f"{st} {data[:20]!r}")
            b.request("POST", "/api/mkdir?path=" + q("/fat/web/sub"))
            b.request("POST", "/api/move?from=" + q("/fat/web/up.txt") + "&to=" + q("/fat/web/sub/moved.txt"))
            got = probe("MV", "isdirectory('/fat/web/sub') . filereadable('/fat/web/sub/moved.txt') . filereadable('/fat/web/up.txt')")
            check(got == "110", "makes a folder and moves a file into it", f"dir:moved:old {got!r}")
            st, _, _ = b.request("POST", "/api/delete?path=" + q("/fat/web/sub"))
            got = probe("DL", "isdirectory('/fat/web/sub')")
            check(st == 200 and got == "0", "deletes a folder and its contents", f"{st} {got!r}")
            st, j, _ = b.request("POST", "/api/delete?path=" + q("/fat/../vimrt/vimrc"))
            got = probe("TR", "filereadable('/vimrt/vimrc')")
            check(st == 400 and got == "1", "refuses a path outside the writable storage", f"{st} {j}")

            # 6. Settings reach Vim within a couple of seconds; bad ones are refused.
            st, j, _ = b.request("POST", "/api/settings", {"tabstop": 99})
            check(st == 400, "refuses an out-of-range setting", f"{st} {j}")
            st, _, _ = b.request("POST", "/api/settings", {"tabstop": 3, "number": True})
            time.sleep(3)
            got = probe("ST", "&tabstop . ':' . &number")
            check(st == 200 and got == "3:1", "settings saved in the browser are applied in Vim", f"{st} {got!r}")

            # 7. Live status.
            s.type(":call writefile(['one two three', 'four five', 'six'], '/fat/web/s.txt') | e /fat/web/s.txt | 2\r")
            time.sleep(3)
            st, j, _ = b.request("GET", "/api/status")
            ok = (st == 200 and j["file"].endswith("s.txt") and j["lines"] == 3
                  and j["words"] == 6 and j["line"] == 2)
            check(ok, "the live status shows the file, lines, words and cursor", str(j))

            # 8. Logout, then stop.
            b.request("POST", "/api/logout")
            st, _, _ = b.request("GET", "/api/list?path=" + q("/fat"))
            check(st == 401, "logging out ends the session", f"status {st}")
            s.type(":EspWebStop\r")
            s.quiet(1.5)
            try:
                b.request("GET", "/api/session")
                stopped = False
            except (OSError, http.client.HTTPException):
                stopped = True
            check(stopped, ":EspWebStop stops the server")

        data = log.read_bytes()
        import re
        errs = sorted(set(m.decode(errors="replace") for m in re.findall(rb"E\d{2,4}: [^\x1b\r\n]*", data)))
        check(not errs, "no Vim error messages", "; ".join(errs))
    except Exception as e:
        check(False, "session completed", f"{type(e).__name__}: {e}")

    if failures:
        print(f"web: {failures} check(s) FAILED -- UART log kept at {log}")
        sys.exit(1)
    log.unlink()
    print("web: all checks passed")


if __name__ == "__main__":
    main()
