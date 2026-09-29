#!/usr/bin/env python3
"""
libgit2 feasibility gate (stage 6z, docs/PLAN.md), on the p4git build
(scripts/vim-build.sh p4git: the P4 build plus CONFIG_ESP_VIM_GIT_SELFTEST).

The host serves a bare repository -- with a packed, delta-compressed history --
three ways: smart HTTP (git http-backend behind a small CGI bridge), git://
(git daemon) and SSH (the unprivileged sshd of interactive.py). The device's
self-test task (components/esp_git/esp_git_selftest.c) inits, commits, checks
status and checks out locally, then clones, pushes and fetches over each, and
prints GIT-ST lines with its time, heap and stack figures. This side checks
what arrived: the clone's head and history against the host's, and each push
with git fsck and git show.

    ESPVIM_TARGET=p4git python esp-vim/test/git.py [--https URL]

--https also clones a public repository over HTTPS (needs internet access).
"""

import argparse
import getpass
import http.server
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from uart_session import Session, VARIANT, DEVICE  # noqa: E402
from interactive import start_sshd, HOST_FROM_DEVICE  # noqa: E402

failures = 0


def check(ok, label, detail=""):
    global failures
    print(f"  {'PASS' if ok else 'FAIL'}  {label}{(' -- ' + detail) if detail else ''}")
    if not ok:
        failures += 1


def git(*args, cwd=None, check=True):
    r = subprocess.run(["git", *args], cwd=cwd, capture_output=True, text=True)
    if check and r.returncode:
        raise RuntimeError(f"git {' '.join(args)}: {r.stderr.strip()}")
    return r.stdout.strip()


def make_big_repo(root):
    """A repository whose pack is several of the device's 256 KB pack windows:
    four files of random base64 (compressing to about three quarters), each
    rewritten in part over ten commits."""
    import base64
    import random
    rnd = random.Random(6)
    work = root / "bigwork"
    git("init", "-q", "-b", "main", str(work))
    git("config", "user.name", "Host", cwd=work)
    git("config", "user.email", "host@localhost", cwd=work)
    blobs = [bytearray(base64.encodebytes(rnd.randbytes(96 * 1024))) for _ in range(4)]
    for n in range(10):
        for i, b in enumerate(blobs):
            at = rnd.randrange(len(b) - 8192)
            b[at:at + 8192] = base64.encodebytes(rnd.randbytes(6144))[:8192]
            (work / f"data-{i}.txt").write_bytes(bytes(b))
        git("add", "-A", cwd=work)
        git("commit", "-q", "-m", f"Big commit {n}", cwd=work)
    bare = root / "srv" / "big.git"
    git("clone", "-q", "--bare", str(work), str(bare))
    git("gc", "-q", cwd=bare)
    return bare


def make_repo(root):
    """A bare repository whose history packs into deltas (ofs-delta): one text
    file rewritten a little in each of 30 commits, and a few others."""
    work = root / "work"
    git("init", "-q", "-b", "main", str(work))
    git("config", "user.name", "Host", cwd=work)
    git("config", "user.email", "host@localhost", cwd=work)
    lines = [f"line {i}: the quick brown fox jumps over the lazy dog\n" for i in range(400)]
    for n in range(30):
        lines[n * 13 % 400] = f"line {n * 13 % 400}: changed in commit {n}\n"
        (work / "story.txt").write_text("".join(lines))
        (work / f"note-{n % 5}.txt").write_text(f"note {n}\n")
        git("add", "-A", cwd=work)
        git("commit", "-q", "-m", f"Commit {n}", cwd=work)
    bare = root / "repo.git"
    git("clone", "-q", "--bare", str(work), str(bare))
    git("gc", "-q", "--aggressive", cwd=bare)
    git("config", "http.receivepack", "true", cwd=bare)
    return bare


class GitCGI(http.server.BaseHTTPRequestHandler):
    """Smart HTTP: every request goes to git http-backend (CGI)."""
    root = None
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def _body(self):
        if self.headers.get("Transfer-Encoding", "").lower() == "chunked":
            data = b""
            while True:
                size = int(self.rfile.readline().strip().split(b";")[0], 16)
                if size == 0:
                    self.rfile.readline()
                    return data
                data += self.rfile.read(size)
                self.rfile.readline()
        return self.rfile.read(int(self.headers.get("Content-Length", 0)))

    def _run(self):
        path, _, query = self.path.partition("?")
        body = self._body() if self.command == "POST" else b""
        env = dict(os.environ, GIT_PROJECT_ROOT=str(self.root), GIT_HTTP_EXPORT_ALL="1",
                   PATH_INFO=path, QUERY_STRING=query, REQUEST_METHOD=self.command,
                   CONTENT_TYPE=self.headers.get("Content-Type", ""),
                   CONTENT_LENGTH=str(len(body)), REMOTE_ADDR="127.0.0.1",
                   GIT_PROTOCOL=self.headers.get("Git-Protocol", ""))
        exe = Path(git("--exec-path")) / "git-http-backend"
        out = subprocess.run([str(exe)], input=body, env=env, capture_output=True).stdout
        head, _, payload = out.partition(b"\r\n\r\n")
        if not _:
            head, _, payload = out.partition(b"\n\n")
        status, headers = 200, []
        for line in head.decode("latin-1").splitlines():
            k, _, v = line.partition(":")
            if k.lower() == "status":
                status = int(v.split()[0])
            elif k:
                headers.append((k, v.strip()))
        self.send_response(status)
        for k, v in headers:
            self.send_header(k, v)
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    do_GET = do_POST = _run


def start_git_http(root):
    handler = type("H", (GitCGI,), {"root": root})
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, srv.server_address[1]


def start_git_daemon(root):
    import socket
    with socket.socket() as so:
        so.bind(("127.0.0.1", 0))
        port = so.getsockname()[1]
    proc = subprocess.Popen(["git", "daemon", "--reuseaddr", "--export-all", "--enable=receive-pack",
                             f"--base-path={root}", "--listen=127.0.0.1", f"--port={port}", str(root)],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
    return proc, port


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--https", help="also clone this public repository over HTTPS")
    opts = ap.parse_args()
    if DEVICE:
        print("  SKIP  git: needs the emulator (ESPVIM_DEVICE is set)")
        return
    if VARIANT != "p4git":
        print(f"  SKIP  git: the self-test is in the p4git build, not {VARIANT} (ESPVIM_TARGET=p4git)")
        return

    tmp = Path(tempfile.mkdtemp(prefix="vim-git-"))
    log = tmp / "uart.log"
    bare = make_repo(tmp)
    served = {}
    for kind in ("http", "git", "ssh"):
        served[kind] = tmp / "srv" / f"{kind}.git"
        served[kind].parent.mkdir(exist_ok=True)
        shutil.copytree(bare, served[kind])
    big = make_big_repo(tmp)
    big_head = git("rev-parse", "main", cwd=big)
    big_pack = sum(p.stat().st_size for p in (big / "objects" / "pack").glob("*.pack"))
    head = git("rev-parse", "main", cwd=bare)
    ncommits = int(git("rev-list", "--count", "main", cwd=bare))
    deltas = git("verify-pack", "-v", *[str(p) for p in (bare / "objects" / "pack").glob("*.idx")])
    ndeltas = sum(1 for ln in deltas.splitlines() if re.match(r"^[0-9a-f]{40} \w+ +\d+ \d+ \d+ \d+ [0-9a-f]{40}", ln))
    print(f"git session (variant {VARIANT}): host repo {head[:12]}, {ncommits} commits, {ndeltas} deltas in its pack;"
          f" big repo pack {big_pack // 1024} KB")

    srv, http_port = start_git_http(tmp / "srv")
    daemon, git_port = start_git_daemon(tmp / "srv")
    sshroot = tmp / "sshd"
    sshroot.mkdir()
    sshd, ssh_port = start_sshd(sshroot)
    results, allocs = {}, {}
    try:
        with Session(term_size=(40, 120), log=str(log)) as s:
            def probe(tag, expr, timeout=60):
                s.type(f":echo '{tag}' . '=' . {expr} . '|'\r")
                return s.expect(rf"{tag}=([^|]*)\|".encode(), timeout).group(1).decode()

            s.expect("ESPVIM-READY", 120)
            s.quiet(2.0)
            conf = ["dir=/fat/gt", "name=esp-vim test", "email=test@localhost",
                    f"http=http://{HOST_FROM_DEVICE}:{http_port}/http.git",
                    f"git=git://{HOST_FROM_DEVICE}:{git_port}/git.git",
                    f"big=git://{HOST_FROM_DEVICE}:{git_port}/big.git"]
            if sshd is not None:
                s.type(":let g:pub = esp_ssh_keygen()\r")
                s.quiet(2.0, timeout=120)
                # In pieces: an :echo wider than the screen wraps.
                n = int(probe("PL", "len(g:pub)"))
                pub = "".join(probe(f"P{i}", f"strpart(g:pub, {i * 60}, 60)") for i in range((n + 59) // 60))
                (sshroot / "authorized_keys").write_text(pub + "\n")
                conf.append(f"ssh=ssh://{getpass.getuser()}@{HOST_FROM_DEVICE}:{ssh_port}{served['ssh']}")
            else:
                print("  SKIP  ssh (no sshd on the host)")
            if opts.https:
                conf.append(f"https={opts.https}")
            s.type(":call writefile(" + repr(conf).replace('"', "'") + ", '/fat/gittest.conf')\r")
            s.expect("GIT-ST-BEGIN", 60)
            # A panic's text can come out of both cores at once, interleaved, so
            # watch for what follows it: the ROM's banner as the chip restarts.
            end = s.expect(r"GIT-ST-END ([^\r\n]*)\r?\n|ESP-ROM:", 1800)
            if not end.group(0).startswith(b"GIT-ST-END"):
                raise RuntimeError("the device crashed and restarted (panic in the UART log)")
            text = log.read_text(errors="replace")
            for m in re.finditer(r"GIT-ST (\S+) (ok|FAIL) ([^\r\n\x1b]*)", text):
                results[m.group(1)] = (m.group(2) == "ok", dict(re.findall(r"(\w+)=('[^']*'|\S+)", m.group(3))),
                                       m.group(3))
            for m in re.finditer(r"GIT-ALLOC (\d+) (\S+)", text):
                allocs[m.group(2)] = max(allocs.get(m.group(2), 0), int(m.group(1)))
            summary = dict(re.findall(r"(\w+)=(\S+)", end.group(1).decode()))
    except Exception as e:
        check(False, "session completed", f"{type(e).__name__}: {e}")
        summary = {}
    finally:
        srv.shutdown()
        os.killpg(daemon.pid, 15)
        if sshd is not None:
            sshd.kill()

    def step(name, label, extra=lambda f: (True, "")):
        ok, fields, raw = results.get(name, (False, {}, "(not run)"))
        good, why = extra(fields) if ok else (False, "")
        check(ok and good, label, why or raw.strip()[:160])
        return fields

    step("libinit", "git_libgit2_init")
    step("init", "init: FAT's config probes: no file modes, no symlinks",
         lambda f: (f.get("filemode") == "0" and f.get("symlinks") == "0",
                    f"filemode={f.get('filemode')} symlinks={f.get('symlinks')}"))
    step("commit", "two commits in a new repository")
    step("status", "status sees the modified file",
         lambda f: (f.get("entries") == "1" and f.get("modified") == "1", str(f)))
    step("checkout", "a branch at HEAD~1, checked out, brings the first version back",
         lambda f: (f.get("file") == "'hello from the device'", f.get("file", "")))
    for kind in ("http", "git", "ssh"):
        if kind == "ssh" and sshd is None:
            continue
        repo = served[kind]
        step(f"clone-{kind}", f"clone over {kind}: the host's head and history",
             lambda f: (f.get("head") == head and f.get("commits") == str(ncommits),
                        f"head={f.get('head', '')[:12]} commits={f.get('commits')} objects={f.get('objects')}"))
        pushed = step(f"push-{kind}", f"push over {kind}")
        if pushed.get("head"):
            now = git("rev-parse", "main", cwd=repo)
            fsck = subprocess.run(["git", "fsck", "--strict"], cwd=repo, capture_output=True, text=True)
            shown = git("show", f"main:device-{kind}.txt", cwd=repo, check=False)
            check(now == pushed["head"] and fsck.returncode == 0 and shown == "pushed from the device",
                  f"  the host has the device's commit ({kind}), and git fsck passes",
                  f"host main={now[:12]} fsck={fsck.returncode} {fsck.stderr.strip()[:80]}")
        step(f"fetch-{kind}", f"fetch over {kind}")
    if opts.https:
        step("clone-https", "clone over HTTPS, the server's certificate verified (ESP-IDF's bundle)",
             lambda f: (f.get("cert") == "1", f"cert={f.get('cert')} head={f.get('head', '')[:12]}"))
    step("clone-big", f"clone a {big_pack // 1024} KB pack through 256 KB windows",
         lambda f: (f.get("head") == big_head, f"head={f.get('head', '')[:12]} peak={int(f.get('peak', 0)) // 1024} KB"))

    print("\nmeasurements (esp-emu, P4):")
    for name, (ok, fields, raw) in results.items():
        print(f"  {name:12} {'ok  ' if ok else 'FAIL'} {fields.get('ms', '?'):>7} ms  "
              f"libgit2 heap peak {int(fields.get('peak', 0)) // 1024:>5} KB")
    if summary:
        print(f"  whole run: heap peak {int(summary.get('peak', 0)) // 1024} KB, stack used "
              f"{summary.get('stack')} of the task's bytes, leaked {summary.get('leaked')} B, "
              f"internal RAM low {int(summary.get('int_min', 0)) // 1024} KB")
        for site, n in sorted(allocs.items(), key=lambda kv: -kv[1]):
            print(f"  allocation of {n // 1024} KB at {site}")
        check(summary.get("leaked") == "0", "libgit2 frees everything by git_libgit2_shutdown")

    if failures:
        print(f"git: {failures} check(s) FAILED -- UART log kept at {log}")
        sys.exit(1)
    shutil.rmtree(tmp, ignore_errors=True)
    print("git: all checks passed")


if __name__ == "__main__":
    main()
