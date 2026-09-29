#!/usr/bin/env python3
"""
Git gate (docs/PLAN.md, Phase 8): the esp_git_*() builtins and the :EspGit*
commands, in the emulator, against a host.

The host serves a bare repository -- a packed, delta-compressed history --
three ways: smart HTTP (git http-backend behind a small CGI bridge), git://
(git daemon) and SSH (the unprivileged sshd of interactive.py), and commits
into it itself, so the device's pulls get thin packs, merges and a conflict.
Checked: a local repository (init, status, add, commit, diff, branch,
checkout, show, gc), the status, commit and log windows, clone/push/pull
over each transport with git fsck on the host after every push, the SSH
host-key question, and CTRL-C stopping a clone cleanly.

    python esp-vim/test/git.py [--https URL]

--https also clones a public repository over HTTPS (needs internet access).
"""

import argparse
import getpass
import http.server
import os
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


def host_commit(bare, root, name, text, msg, edit=None):
    """Commit on the host side: clone {bare}, change {name} (or run {edit}),
    commit and push back. Returns the new head."""
    work = Path(tempfile.mkdtemp(dir=root, prefix="hostwork-"))
    git("clone", "-q", str(bare), str(work))
    git("config", "user.name", "Host", cwd=work)
    git("config", "user.email", "host@localhost", cwd=work)
    if edit:
        edit(work)
    else:
        (work / name).write_text(text)
    git("add", "-A", cwd=work)
    git("commit", "-q", "-m", msg, cwd=work)
    git("push", "-q", "origin", "main", cwd=work)
    head = git("rev-parse", "HEAD", cwd=work)
    shutil.rmtree(work)
    return head


def fsck_ok(bare):
    r = subprocess.run(["git", "fsck", "--strict"], cwd=bare, capture_output=True, text=True)
    return r.returncode == 0, r.stderr.strip()[:120]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--https", help="also clone this public repository over HTTPS")
    opts = ap.parse_args()
    if DEVICE:
        print("  SKIP  git: needs the emulator's network to the host (ESPVIM_DEVICE is set)")
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
    head = git("rev-parse", "main", cwd=bare)
    ncommits = int(git("rev-list", "--count", "main", cwd=bare))
    print(f"git session (variant {VARIANT}): host repo {head[:12]}, {ncommits} commits")

    srv, http_port = start_git_http(tmp / "srv")
    daemon, git_port = start_git_daemon(tmp / "srv")
    sshroot = tmp / "sshd"
    sshroot.mkdir()
    sshd, ssh_port = start_sshd(sshroot)
    H = HOST_FROM_DEVICE
    http_url = f"http://{H}:{http_port}/http.git"
    try:
        with Session(term_size=(40, 120), log=str(log)) as s:
            def probe(tag, expr, timeout=120):
                s.type(f":echo '{tag}' . '=' . ({expr}) . '|'\r")
                return s.expect(rf"{tag}=([^|]*)\|".encode(), timeout).group(1).decode()

            def run(cmd, timeout=300):
                """An Ex command; returns v:errmsg after it ('' when fine)."""
                s.type(":let v:errmsg = ''\r")
                s.type(f":{cmd}\r")
                s.quiet(1.5, timeout=timeout)
                return probe("ER", "substitute(v:errmsg, '|', '/', 'g')")

            def ok(cmd, label, timeout=300):
                err = run(cmd, timeout)
                check(err == "", label, err)
                return err == ""

            s.expect("ESPVIM-READY", 120)
            s.quiet(2.0)

            # -- who is committing, device-wide (/fat/.gitconfig)
            ok("call esp_git_config('', 'user.name', 'Device Tester') | call esp_git_config('', 'user.email', 'dev@localhost')",
               "esp_git_config() sets the device-wide author")
            check(probe("CN", "esp_git_config('', 'user.name')") == "Device Tester",
                  "  ... and reads it back")

            # -- a local repository
            ok("call esp_git_init('/fat/work') | call writefile(['one'], '/fat/work/a.txt')",
               "esp_git_init() makes a repository")
            got = probe("S0", "join(map(esp_git_status('/fat/work').files, 'v:val.x . v:val.y . v:val.path'), ',')")
            check(got == "??a.txt", "status: a new file is untracked", got)
            ok("call esp_git_add('/fat/work', ['/fat/work/a.txt']) | call esp_git_commit('/fat/work', 'First commit')",
               "add and commit")
            got = probe("L1", "len(esp_git_log('/fat/work')) . ':' . esp_git_log('/fat/work')[0].summary . ':' . esp_git_status('/fat/work').branch")
            check(got == "1:First commit:main", "the log has it, on branch main", got)
            ok("call writefile(['one', 'two'], '/fat/work/a.txt')", "change the file")
            got = probe("S1", "join(map(esp_git_status('/fat/work').files, 'v:val.x . v:val.y . v:val.path'), ',')")
            check(got == " Ma.txt", "status: modified in the working tree", repr(got))
            got = probe("D1", "join(filter(esp_git_diff('/fat/work'), 'v:val =~# \"^[-+][^-+]\"'), ',')")
            check(got == "+two", "diff shows the new line", got)
            ok("call esp_git_add('/fat/work') | call esp_git_commit('/fat/work', 'Second')", "add everything, commit")
            ok("call esp_git_branch('/fat/work', 'feature') | call esp_git_checkout('/fat/work', 'feature')"
               " | call writefile(['feature'], '/fat/work/b.txt') | call esp_git_add('/fat/work')"
               " | call esp_git_commit('/fat/work', 'On a branch')", "branch, check out, commit on it")
            ok("call esp_git_checkout('/fat/work', 'main')", "check out main again")
            got = probe("CO", "filereadable('/fat/work/b.txt') . ':' . esp_git_status('/fat/work').branch . ':' . len(esp_git_branch('/fat/work'))")
            check(got == "0:main:2", "the branch's file is gone on main; two branches", got)
            got = probe("SH", "join(esp_git_show('/fat/work', 'feature')[0:4], '/')")
            check(got.startswith("commit ") and "Author: Device Tester <dev@localhost>" in got,
                  "esp_git_show() of a branch", got[:80])

            # -- the commands and their windows
            ok("e /fat/work/a.txt | call setline(1, 'ONE') | w | EspGitStatus", ":EspGitStatus opens its window")
            got = probe("SW", "bufname('') . ':' . getline(2)")
            check(got == "EspGitStatus: M a.txt", "  ... listing the change", got)
            s.type("a")                                  # stage it, from the window
            s.quiet(1.5)
            got = probe("SA", "getline(2)")
            check(got == "M  a.txt", "  'a' stages the file", got)
            s.type("q")
            s.quiet(1.0)
            s.type(":EspGitCommit\r")
            s.quiet(1.5)
            s.type("Committed from its buffer")
            s.send(b"\x1b")
            s.type(":w\r")
            s.quiet(2.0)
            got = probe("CB", "esp_git_log('/fat/work', 1)[0].summary . ':' . bufname('')")
            check(got == "Committed from its buffer:/fat/work/a.txt", ":EspGitCommit's buffer commits on :w", got)
            ok("EspGitLog", ":EspGitLog opens")
            got = probe("LW", "bufname('') . ':' . (line('$') - 1)")
            check(got == "EspGitLog:3", "  ... with the three commits on main", got)
            s.type("j\r")                                # show the second one
            s.quiet(1.5)
            got = probe("LS", "bufname('') . ':' . &filetype . ':' . (getline(1) =~# '^commit ')")
            check(got == "EspGitShow:diff:1", "  <CR> shows a commit", got)
            s.type(":only | enew!\r")
            s.quiet(1.0)

            # -- gc: the loose objects into a pack
            got = probe("GC", "string(esp_git_gc('/fat/work'))")
            check("'loose_removed': " in got and "'packed': " in got and "'packed': 0" not in got,
                  "esp_git_gc() packs the loose objects", got)
            got = probe("G2", "len(esp_git_log('/fat/work')) . ':' . len(esp_git_status('/fat/work').files)"
                              " . ':' . len(glob('/fat/work/.git/objects/pack/*.pack', 0, 1))")
            check(got == "3:0:1", "  ... and the history and the tree are intact", got)

            # -- HTTP: clone, push, pull (fast-forward, merge, conflict)
            ok(f"call esp_git_clone('{http_url}', '/fat/h')", "clone over HTTP")
            got = probe("HC", "esp_git_log('/fat/h', 1)[0].id . ':' . len(esp_git_log('/fat/h')) . ':' . esp_git_status('/fat/h').upstream")
            check(got == f"{head}:{ncommits}:origin/main", "  ... the host's head, history and upstream", got)
            ok("call writefile(['from the device'], '/fat/h/device.txt') | call esp_git_add('/fat/h')"
               " | call esp_git_commit('/fat/h', 'Device commit') | call esp_git_push('/fat/h')", "commit and push")
            dev = probe("HD", "esp_git_log('/fat/h', 1)[0].id")
            fine, why = fsck_ok(served["http"])
            check(git("rev-parse", "main", cwd=served["http"]) == dev and fine,
                  "  ... the host has it, and git fsck passes", why)

            new = host_commit(served["http"], tmp, "host.txt", "from the host\n", "Host commit")
            ok("let g:r = esp_git_pull('/fat/h')", "pull a host commit")
            got = probe("P1", "g:r.result . ':' . g:r.head . ':' . join(readfile('/fat/h/host.txt'))")
            check(got == f"fast-forward:{new}:from the host", "  ... a fast-forward (a thin pack)", got)

            new = host_commit(served["http"], tmp, "host2.txt", "again\n", "Second host commit")
            ok("call writefile(['device 2'], '/fat/h/device2.txt') | call esp_git_add('/fat/h')"
               " | call esp_git_commit('/fat/h', 'Device again') | let g:r = esp_git_pull('/fat/h')",
               "commits on both sides, then pull")
            got = probe("P2", "g:r.result . ':' . esp_git_log('/fat/h', 1)[0].parents . ':' . filereadable('/fat/h/host2.txt')")
            check(got == "merged:2:1", "  ... a merge commit with both parents", got)
            ok("call esp_git_push('/fat/h')", "  push the merge")
            fine, why = fsck_ok(served["http"])
            subject = git("log", "-1", "--merges", "--format=%s", "main", cwd=served["http"])
            check(fine and git("rev-list", "--count", "--merges", "main", cwd=served["http"]) == "1"
                  and subject.startswith("Merge branch 'main' of http"),
                  "  ... the host has the merge, as git words it, and git fsck passes", f"{subject!r} {why}")

            host_commit(served["http"], tmp, "device.txt", "the host's line\n", "Host edits device.txt")
            ok("call writefile(['the device line'], '/fat/h/device.txt') | call esp_git_add('/fat/h')"
               " | call esp_git_commit('/fat/h', 'Device edits it too') | let g:r = esp_git_pull('/fat/h')",
               "edits to the same line on both sides, then pull")
            got = probe("P3", "g:r.result . ':' . join(g:r.conflicts) . ':' . esp_git_status('/fat/h').state"
                              " . ':' . join(map(filter(esp_git_status('/fat/h').files, 'v:val.x == \"U\"'), 'v:val.path'))")
            check(got == "conflicts:device.txt:merge:device.txt", "  ... a conflict, left to resolve", got)
            got = probe("P4", "join(readfile('/fat/h/device.txt'), '/')")
            check(got.startswith("<<<<<<<") and "the device line" in got and "the host's line" in got,
                  "  ... with conflict markers in the file", got[:80])
            err = run("call esp_git_commit('/fat/h', 'too soon')")
            check("conflicts" in err, "committing before fixing it is refused", err)
            ok("call writefile(['resolved'], '/fat/h/device.txt') | call esp_git_add('/fat/h', ['/fat/h/device.txt'])"
               " | call esp_git_commit('/fat/h', 'Resolved') | call esp_git_push('/fat/h')",
               "resolve, add, commit (finishing the merge) and push")
            got = probe("P5", "esp_git_log('/fat/h', 1)[0].parents . ':' . esp_git_status('/fat/h').state")
            fine, why = fsck_ok(served["http"])
            check(got == "2:" and fine and (served["http"] / "HEAD").exists()
                  and git("show", "main:device.txt", cwd=served["http"]) == "resolved",
                  "  ... a two-parent commit, the merge state cleared, the host has it", f"{got} {why}")

            # -- git://
            ok(f"call esp_git_clone('git://{H}:{git_port}/git.git', '/fat/g') | call writefile(['x'], '/fat/g/x.txt')"
               " | call esp_git_add('/fat/g') | call esp_git_commit('/fat/g', 'Over git://') | call esp_git_push('/fat/g')",
               "clone, commit and push over git://")
            fine, why = fsck_ok(served["git"])
            check(fine and git("show", "main:x.txt", cwd=served["git"]) == "x", "  ... the host has it", why)

            # -- SSH, with the host key asked about the first time
            if sshd is not None:
                s.type(":let g:pub = esp_ssh_keygen()\r")
                s.quiet(2.0, timeout=120)
                n = int(probe("PL", "len(g:pub)"))
                pub = "".join(probe(f"P{i}", f"strpart(g:pub, {i * 60}, 60)") for i in range((n + 59) // 60))
                (sshroot / "authorized_keys").write_text(pub + "\n")
                url = f"ssh://{getpass.getuser()}@{H}:{ssh_port}{served['ssh']}"
                s.type(":let v:errmsg = ''\r")
                s.type(f":EspGitClone {url} /fat/s\r")
                s.expect(r"Trust it and remember it\?", 120)
                s.type("y")
                s.quiet(2.0, timeout=300)
                got = probe("SC", "v:errmsg . ':' . len(esp_git_log('/fat/s'))")
                check(got == f":{ncommits}", ":EspGitClone over SSH asks about the host key, then clones", got)
                ok("call writefile(['ssh'], '/fat/s/s.txt') | call esp_git_add('/fat/s')"
                   " | call esp_git_commit('/fat/s', 'Over SSH') | call esp_git_push('/fat/s')",
                   "push over SSH (the host now known)")
                fine, why = fsck_ok(served["ssh"])
                check(fine and git("show", "main:s.txt", cwd=served["ssh"]) == "ssh", "  ... the host has it", why)
            else:
                print("  SKIP  ssh (no sshd on the host)")

            if opts.https:
                ok(f"call esp_git_clone('{opts.https}', '/fat/https')", "clone over HTTPS (the certificate verified)")

            # -- CTRL-C stops a clone, and it leaves nothing behind
            s.type(f":let v:errmsg = '' | call esp_git_clone('git://{H}:{git_port}/big.git', '/fat/big')\r")
            s.expect(r"Receiving objects|Resolving deltas|Checking out", 120)
            s.send(b"\x03")
            s.expect(r"Press ENTER", 120)        # after an interrupt, as Vim always does
            s.send(b"\r")
            s.quiet(2.0, timeout=120)
            got = probe("IC", "(v:errmsg =~? 'interrupt') . ':' . isdirectory('/fat/big')")
            check(got == "1:0", "CTRL-C stops a clone, and it leaves nothing", got)
            ok(f"call esp_git_clone('git://{H}:{git_port}/big.git', '/fat/big')", "a larger clone, to the end")
            got = probe("BC", "esp_git_log('/fat/big', 1)[0].id")
            check(got == git("rev-parse", "main", cwd=big), "  ... the host's head", got)
            heap = probe("HP", "string(esp_heap())")
            print(f"  (heap after: {heap[:160]})")
    except Exception as e:
        check(False, "session completed", f"{type(e).__name__}: {e}")
    finally:
        srv.shutdown()
        os.killpg(daemon.pid, 15)
        if sshd is not None:
            sshd.kill()

    if failures:
        print(f"git: {failures} check(s) FAILED -- UART log kept at {log}")
        sys.exit(1)
    shutil.rmtree(tmp, ignore_errors=True)
    print("git: all checks passed")


if __name__ == "__main__":
    main()
