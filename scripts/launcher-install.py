#!/usr/bin/env python3
"""
Install an app image on a device running bmorcelli/Launcher, over its web
interface (WebUI) -- the way its own page does, without a browser.

    scripts/launcher-install.py IP [BIN] [--variant V] [--name NAME] [--replace] [--boot]
    scripts/launcher-install.py IP --sd-runtime [--variant V]

IP     the device's address, as Launcher's WebUI screen shows it
BIN    the app image (default: the build variant's esp-vim-V.bin)
--variant  a build variant made as a launcher app (ESP_VIM_LAUNCHER_APP;
       default cardputer, the one there is so far)
--name the app's name in Launcher's menu (default: esp-vim)
--replace  first delete any installed app of that name, so a new build takes
       its place rather than adding a copy. Launcher applies a partition change
       by restarting: start its WebUI again when asked.
--boot restart into the app once it is installed (installing already makes it
       the one Launcher boots).
--sd-runtime  instead of installing, copy Vim's runtime to the SD card in the
       device, through the WebUI's file manager: build-deps/vimrt-V
       (pixi run sd-runtime makes it) to /vim/runtime. The
       same as pixi run sd-runtime with the card in this computer.

The WebUI login is Launcher's default (admin / launcher) unless
$LAUNCHER_USER and $LAUNCHER_PASS say otherwise.

The protocol (Launcher's src/webInterface.cpp and webUi/scripts.js):
  POST /login            username, password -> cookie ESP32SESSION
  GET  /partitions       the table as JSON (--replace)
  POST /partitions       action=delete offset=N, then action=apply (restarts)
  POST /OTA              command=0, size, manifest (JSON: the parts to install)
  POST /OTAFILE          the file, multipart field "file1" -> "OK"
  GET  /reboot
  POST /                 folder, file: one file to the SD card (--sd-runtime)
Only standard-library Python: no requests, no curl.
"""

import argparse
import http.cookiejar
import json
import os
import struct
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_VARIANT = "cardputer"


def die(msg):
    print(f"launcher-install: {msg}", file=sys.stderr)
    sys.exit(1)


def say(msg):
    print(f"launcher-install: {msg}")


def image_size(data):
    """Length of the ESP app image at the start of data, as Launcher's page
    measures it (measureEspImageSize): header, segments, checksum, hash."""
    if len(data) < 24 or data[0] != 0xE9 or not 0 < data[1] <= 16:
        die("not an ESP app image (no 0xE9 header): give the app .bin, not a merged one")
    cursor = 24
    for _ in range(data[1]):
        (size,) = struct.unpack_from("<I", data, cursor + 4)
        cursor += 8 + size
        if cursor > len(data):
            die("app image truncated")
    end = (cursor + 15) // 16 * 16 + 1
    if data[23]:
        end += 32                       # SHA-256 appended
    end = (end + 15) // 16 * 16
    return min(end, len(data))


def multipart(fields, files=()):
    """(body, content type) for fields {name: value} and files
    [(field, filename, bytes)]."""
    boundary = uuid.uuid4().hex
    out = []
    for k, v in fields.items():
        out += [f"--{boundary}\r\nContent-Disposition: form-data; name=\"{k}\"\r\n\r\n".encode(),
                str(v).encode(), b"\r\n"]
    for field, name, data in files:
        out += [f"--{boundary}\r\nContent-Disposition: form-data; name=\"{field}\"; "
                f"filename=\"{name}\"\r\nContent-Type: application/octet-stream\r\n\r\n".encode(),
                data, b"\r\n"]
    out.append(f"--{boundary}--\r\n".encode())
    return b"".join(out), f"multipart/form-data; boundary={boundary}"


class Launcher:
    def __init__(self, ip):
        self.base = f"http://{ip}"
        self.jar = http.cookiejar.CookieJar()
        self.http = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(self.jar))

    def request(self, method, path, body=None, ctype=None, timeout=30):
        req = urllib.request.Request(self.base + path, data=body, method=method)
        if ctype:
            req.add_header("Content-Type", ctype)
        try:
            with self.http.open(req, timeout=timeout) as r:
                return r.status, r.read().decode(errors="replace")
        except urllib.error.HTTPError as e:
            return e.code, e.read().decode(errors="replace")
        except (urllib.error.URLError, OSError) as e:
            die(f"{method} {path}: {e}")

    def form(self, path, fields, timeout=30):
        body = urllib.parse.urlencode(fields).encode()
        return self.request("POST", path, body, "application/x-www-form-urlencoded", timeout)

    def alive(self):
        try:
            with urllib.request.urlopen(self.base + "/ping", timeout=3) as r:
                return r.status == 200
        except (urllib.error.URLError, OSError):
            return False

    def login(self):
        user = os.environ.get("LAUNCHER_USER", "admin")
        password = os.environ.get("LAUNCHER_PASS", "launcher")
        self.form("/login", {"username": user, "password": password})
        if not any(c.name == "ESP32SESSION" for c in self.jar):
            die("login refused: set $LAUNCHER_USER and $LAUNCHER_PASS to the WebUI's login")

    def apps_named(self, name):
        status, text = self.request("GET", "/partitions")
        if status != 200:
            die(f"GET /partitions: {status} {text}")
        table = json.loads(text)
        return [e for e in table["entries"]
                if e["type"] == 0 and e.get("appName") == name and not e.get("running")]

    def replace(self, name):
        old = self.apps_named(name)
        if not old:
            say(f"no installed '{name}' to replace")
            return
        for e in old:
            say(f"deleting '{name}' ({e['label']} at {e['offset']:#x}, {e['size'] // 1024} KB)")
            status, text = self.form("/partitions", {"action": "delete", "offset": e["offset"]})
            if status != 200:
                die(f"delete {e['label']}: {status} {text}")
        status, text = self.form("/partitions", {"action": "apply"})
        if status != 200:
            die(f"apply: {status} {text}")
        say("partition table written; Launcher restarts")
        time.sleep(3)
        say("start the WebUI on the device again (it waits up to 3 minutes)")
        for _ in range(180):
            if self.alive():
                break
            time.sleep(1)
        else:
            die("the WebUI didn't come back; start it and run again without --replace")
        self.jar.clear()
        self.login()

    def install(self, path, name):
        """Install, logging in again and starting over once if Launcher drops
        the session meanwhile: it keeps one, and a login elsewhere (a browser
        on its WebUI) ends ours."""
        for attempt in (1, 2):
            status, text = self.try_install(path, name)
            if status == 200 and text == "OK":
                say(f"installed as '{name}'")
                return
            if status != 401 or attempt == 2:
                die(f"install: {status} {text}")
            say("Launcher dropped the session (a browser on its WebUI?): logging in again")
            self.jar.clear()
            self.login()

    def try_install(self, path, name):
        data = path.read_bytes()
        size = image_size(data)
        manifest = {
            "sourceName": f"{name}.bin",
            "parts": [{"kind": "app", "label": "app", "subtype": 0, "sourceOffset": 0,
                       "copySize": size, "declaredSize": size}],
        }
        body, ctype = multipart({"command": 0, "size": len(data), "manifest": json.dumps(manifest)})
        # Launcher reads and plans the partition table before it answers: give
        # it time (it took over 30 s once, straight after a large SD upload).
        status, text = self.request("POST", "/OTA", body, ctype, timeout=120)
        if status != 200 or text != "OK":
            return status, f"POST /OTA: {text}"
        say(f"sending {path.name}, {len(data)} bytes ...")
        body, ctype = multipart({}, [("file1", f"{name}.bin", data)])
        status, text = self.request("POST", "/OTAFILE", body, ctype, timeout=300)
        return status, text if status == 200 else f"POST /OTAFILE: {text}"

    def upload_tree(self, src, dest):
        files = sorted(p for p in src.rglob("*") if p.is_file())
        for n, f in enumerate(files, 1):
            rel = f.relative_to(src)
            folder = dest if rel.parent == Path(".") else f"{dest}/{rel.parent.as_posix()}"
            body, ctype = multipart({"folder": folder}, [("file", f.name, f.read_bytes())])
            for attempt in range(3):
                status, text = self.request("POST", "/", body, ctype, timeout=60)
                if status == 200 and text == "OK":
                    break
                time.sleep(1 + attempt)
            else:
                die(f"{rel}: {status} {text}")
            print(f"\r  {n}/{len(files)} {rel}".ljust(70)[:70], end="", flush=True)
        print()
        say(f"{len(files)} files in {dest} on the SD card")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("ip")
    ap.add_argument("bin", nargs="?", type=Path)
    ap.add_argument("--variant", default=DEFAULT_VARIANT)
    ap.add_argument("--name", default="esp-vim")
    ap.add_argument("--replace", action="store_true")
    ap.add_argument("--boot", action="store_true")
    ap.add_argument("--sd-runtime", action="store_true")
    a = ap.parse_args()
    if a.bin is None:
        a.bin = ROOT / "esp-vim" / f"build-{a.variant}" / f"esp-vim-{a.variant}.bin"

    if a.sd_runtime:
        src = ROOT / "build-deps" / f"vimrt-{a.variant}"
        if not (src / "vimrc").is_file():
            die(f"{src}: not made yet (python scripts/make-runtime-image.py {a.variant})")
        dev = Launcher(a.ip)
        if not dev.alive():
            die(f"no Launcher WebUI at {a.ip} (start it from Launcher's menu)")
        dev.login()
        dev.upload_tree(src, "/vim/runtime")
        return

    if not a.bin.is_file():
        die(f"{a.bin}: no such file (build it first: scripts/vim-build.sh {a.variant})")

    dev = Launcher(a.ip)
    if not dev.alive():
        die(f"no Launcher WebUI at {a.ip} (start it from Launcher's menu)")
    dev.login()
    if a.replace:
        dev.replace(a.name)
    dev.install(a.bin, a.name)
    if a.boot:
        if dev.request("GET", "/reboot")[0] == 401:
            dev.jar.clear()
            dev.login()
            dev.request("GET", "/reboot")
        say("restarting into it")
    else:
        say("it boots next: restart the device, or choose it in Launcher")


if __name__ == "__main__":
    main()
