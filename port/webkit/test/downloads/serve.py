#!/usr/bin/env python3
"""Synthetic downloads for testing the browser's download path (README:
"Downloads"). Every file is generated: block i (4 KiB) starts with its index
and the server's "version", so a resume that joins the wrong bytes changes
the SHA-256, and `--sha SIZE` prints the checksum a correct file has.

    port/webkit/test/downloads/serve.py [--port 8781]
    port/webkit/test/downloads/serve.py --sha 1048576 [--version 1]

GET /file/<size>[/<name>]?options    the file (HEAD too)
    name=     the suggested name (else the path's last part, else file-<size>.bin)
    attach=1  Content-Disposition: attachment; filename=...
    type=     Content-Type (default application/octet-stream)
    noranges=1  ignore Range (200 every time, no Accept-Ranges)
    nolen=1   no Content-Length (the end of the body ends it)
    weak=1    a weak ETag (W/"...": no If-Range)
    novalidator=1  neither ETag nor Last-Modified
    rate=KB   at most KB KiB/s (the slow mode, for pausing)
    totalrate=KB  all answers for this path together at most KB KiB/s (a
              host's limit for one address, or the line's)
    drop=N[,M...]  close the connection when the file offset reaches N (each
              point once per server run and file: a resume goes past it)
    stall=N   stop sending at offset N and keep the connection open (once)
    stallall=N  the same at every request that reaches offset N (never recovers)
    maxconn=N  at most N answers for this path at once; more get `busy`
    busy=429|503  the answer beyond maxconn (default 429)
    retryafter=S  Retry-After: S on those answers
    changeafter=N  after N requests for this path, the next version of the
              file (ETag, Last-Modified and content), as /version does for all
    cookie=NAME=VALUE  403 unless the request's Cookie header has it
    fakesize=N  a 206's Content-Range says the file is N bytes (a broken or
              hostile server)
GET /signed/<size>/<name>?exp=T&...  the file while time() < T, else 403
    (an expiring CDN address)
GET /release/<size>/<name>?...       302 to /signed/... valid for 20 s (a
    release link that redirects to a signed address; the options pass on;
    ttl=S: valid for S s instead)
GET /version?v=N                     the files change (ETag, Last-Modified
    and content): a resume with the old validator gets a 200
GET /real/<name>?options              a real file from --root (by its name
    only), with the options above that don't change the bytes (noranges,
    cookie, attach, rate, maxconn); for the archive peek (peek-test.sh)
GET /stats[?reset=1]                 JSON per path: requests, bytes sent,
    busy answers (maxconn); reset=1 clears the counts
GET /page                            links for every case
GET /auto?case=attr|blob|href[&href=URL]  a page that clicks its own download
    link (href: any address, e.g. /real/app.zip?attach=1)
"""
import argparse
import hashlib
import http.server
import json
import os
import socketserver
import struct
import sys
import threading
import time
import urllib.parse

BLOCK = 4096
FILL = bytes((i * 131 + 7) & 0xFF for i in range(BLOCK))
state = {"version": 1, "fired": set()}
lock = threading.Lock()
active = {}   # path: answers being sent (maxconn)
stats = {}    # path: {"requests", "bytes", "busy"} (/stats)
requests = {}  # path: GET requests so far (changeafter)
paced = {}     # path: [bytes, first time] for totalrate


def count(path, key, n=1):
    with lock:
        entry = stats.setdefault(path, {"requests": 0, "bytes": 0, "busy": 0})
        entry[key] += n


def block(i, version):
    return struct.pack(">QQ", i, version) + FILL[16:]


def generate(start, end, version):
    """Bytes [start, end) of the file, in pieces."""
    pos = start
    while pos < end:
        i = pos // BLOCK
        chunk_end = min(end, (i + 64) * BLOCK)
        data = b"".join(block(j, version) for j in range(i, (chunk_end + BLOCK - 1) // BLOCK))
        off = pos - i * BLOCK
        piece = data[off:off + (chunk_end - pos)]
        yield piece
        pos += len(piece)


def read_real(path, start, end):
    with open(path, "rb") as f:
        f.seek(start)
        left = end - start
        while left > 0:
            piece = f.read(min(left, 1 << 20))
            if not piece:
                return
            left -= len(piece)
            yield piece


def sha(size, version):
    h = hashlib.sha256()
    for piece in generate(0, size, version):
        h.update(piece)
    return h.hexdigest()


def http_date(t):
    return time.strftime("%a, %d %b %Y %H:%M:%S GMT", time.gmtime(t))


PAGE = """<!doctype html><html><head><meta charset=utf-8><title>Download tests</title>
<style>body{font:26px sans-serif;background:#07061f;color:#f4f2ff;margin:48px}a{color:#a77bff;display:block;margin:14px 0}</style></head><body>
<h1>Download tests</h1>
<a href="/file/1048576/plain-1mb.bin">1 MB octet-stream (a type the browser can't show)</a>
<a href="/file/5242880/report.txt?attach=1&type=text/plain">5 MB text as an attachment</a>
<a href="/file/3145728/attr.dat?type=text/plain" download="from-attribute.txt">3 MB text with a download attribute</a>
<a href="/file/20971520/archive.zip?type=application/zip">20 MB .zip</a>
<a href="/file/52428800/slow.bin?rate=1024">50 MB at 1 MB/s (pause and resume)</a>
<a href="/file/31457280/drops.bin?drop=4194304,12582912,20971520">30 MB, connection dropped 3 times</a>
<a href="/file/10485760/noranges.bin?noranges=1&drop=3145728">10 MB, no range support, dropped once</a>
<a href="/file/10485760/stall.bin?stall=2097152">10 MB that stalls at 2 MB</a>
<a href="/release/8388608/release-asset.bin?rate=512">8 MB release link (302 to an address valid for 20 s)</a>
<a href="/file/2147483648/big-2gb.bin">2 GB</a>
<a href="/file/1073741824/big-1gb.bin">1 GB</a>
<a href="/file/4096/shown.txt?type=text/plain">4 KB text (shown, not downloaded)</a>
<a href="#" id="blob">A file made by the page (blob: with a download attribute)</a>
<script>document.getElementById('blob').onclick=function(e){e.preventDefault();var b=new Blob(['made by the page\\n'.repeat(1000)],{type:'text/plain'});
var a=document.createElement('a');a.href=URL.createObjectURL(b);a.download='made-by-page.txt';document.body.appendChild(a);a.click();}</script>
</body></html>"""


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        sys.stdout.write("%s %s\n" % (time.strftime("%H:%M:%S"), fmt % args))
        sys.stdout.flush()

    def do_HEAD(self):
        self.handle_get(head=True)

    def do_GET(self):
        self.handle_get(head=False)

    def send_simple(self, code, text, extra=None):
        body = text.encode()
        self.send_response(code)
        self.send_header("Content-Type", "text/html; charset=utf-8" if text.startswith("<") else "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def handle_get(self, head):
        u = urllib.parse.urlsplit(self.path)
        q = dict(urllib.parse.parse_qsl(u.query))
        parts = [p for p in u.path.split("/") if p]
        self.log_message("%s %s Range=%s If-Range=%s UA=%.40s", self.command, self.path, self.headers.get("Range"),
                         self.headers.get("If-Range"), self.headers.get("User-Agent"))
        if not parts or parts[0] == "page":
            return self.send_simple(200, PAGE)
        if parts[0] == "auto":
            # a page that clicks one of its own download links when it loads
            # (for log-only tests: the pad can't click remotely)
            case = q.get("case", "attr")
            script = ("var b=new Blob(['made by the page\\n'.repeat(1000)],{type:'text/plain'});a.href=URL.createObjectURL(b);a.download='made-by-page.txt';"
                      if case == "blob" else "a.href=%s;" % json.dumps(q.get("href", "/")) if case == "href"
                      else "a.href='/file/3145728/attr.dat?type=text/plain';a.download='from-attribute.txt';")
            return self.send_simple(200, "<!doctype html><title>auto %s</title><body><a id=a>link</a><script>var a=document.getElementById('a');%s"
                                         "setTimeout(function(){a.click()},500)</script>" % (case, script))
        if parts[0] == "stats":
            with lock:
                text = json.dumps(stats, indent=1, sort_keys=True) + "\n"
                if q.get("reset"):
                    stats.clear()
            return self.send_simple(200, text)
        if parts[0] == "version":
            with lock:
                state["version"] = int(q.get("v", state["version"] + 1))
            return self.send_simple(200, "version %d\n" % state["version"])
        if parts[0] == "release" and len(parts) >= 2:
            count(u.path, "requests")
            q2 = dict(q)
            q2["exp"] = str(int(time.time()) + int(q.get("ttl", "20")))
            q2["sig"] = hashlib.sha1(q2["exp"].encode()).hexdigest()[:16]
            loc = "/signed/%s?%s" % ("/".join(parts[1:]), urllib.parse.urlencode(q2))
            return self.send_simple(302, "see %s\n" % loc, {"Location": loc})
        if parts[0] == "signed":
            if int(q.get("exp", "0")) < time.time():
                return self.send_simple(403, "this signed address has expired\n")
            parts = ["file"] + parts[1:]
        real = None
        if parts[0] == "real" and len(parts) == 2 and state.get("root"):
            real = os.path.join(state["root"], os.path.basename(parts[1]))
            if not os.path.isfile(real):
                return self.send_simple(404, "not found\n")
            parts = ["file", str(os.path.getsize(real)), parts[1]]
        if parts[0] != "file" or len(parts) < 2:
            return self.send_simple(404, "not found\n")
        size = int(parts[1])
        name = q.get("name") or (parts[2] if len(parts) > 2 else "file-%d.bin" % size)
        version = state["version"]
        self.real = real
        if not head:
            count(u.path, "requests")
        if q.get("changeafter"):
            with lock:
                if not head:
                    requests[u.path] = requests.get(u.path, 0) + 1
                if requests.get(u.path, 0) > int(q["changeafter"]):
                    version += 1
        if q.get("cookie"):
            want = q["cookie"].strip()
            have = [c.strip() for c in (self.headers.get("Cookie") or "").split(";")]
            if want not in have:
                return self.send_simple(403, "this file needs the cookie %s\n" % want)
        if q.get("maxconn") and not head:
            with lock:
                busy = active.get(u.path, 0) >= int(q["maxconn"])
                if not busy:
                    active[u.path] = active.get(u.path, 0) + 1
            if busy:
                count(u.path, "busy")
                code = int(q.get("busy", "429"))
                extra = {"Retry-After": q["retryafter"]} if q.get("retryafter") else None
                return self.send_simple(code, "too many connections\n", extra)
            try:
                return self.send_file(head, q, u, size, name, version)
            finally:
                self.release(u.path)
        return self.send_file(head, q, u, size, name, version)

    def release(self, path):
        """The end of a maxconn answer (once: before its last piece, or when it ends)."""
        if getattr(self, "released", None) == path:
            return
        self.released = path
        with lock:
            active[path] = active.get(path, 1) - 1

    def send_file(self, head, q, u, size, name, version):
        self.released = None
        key = (u.path, version)
        start, end, partial = 0, size, False
        rng = self.headers.get("Range")
        if_range = self.headers.get("If-Range")
        etag = 'W/"v%d-%d"' % (version, size) if q.get("weak") else '"v%d-%d"' % (version, size)
        modified = http_date(1700000000 + version * 86400)
        if rng and not q.get("noranges"):
            valid = if_range is None or if_range == etag or if_range == modified
            if valid and rng.startswith("bytes="):
                a, _, b = rng[6:].partition("-")
                if a:
                    start = int(a)
                    end = min(size, int(b) + 1) if b else size
                else:
                    start = max(0, size - int(b))
                if start >= size:
                    return self.send_simple(416, "", {"Content-Range": "bytes */%d" % size})
                partial = True
        self.send_response(206 if partial else 200)
        self.send_header("Content-Type", q.get("type", "application/octet-stream"))
        if not q.get("nolen"):
            self.send_header("Content-Length", str(end - start))
        if partial:
            self.send_header("Content-Range", "bytes %d-%d/%d" % (start, end - 1, int(q.get("fakesize", size))))
        if not q.get("noranges"):
            self.send_header("Accept-Ranges", "bytes")
        if not q.get("novalidator"):
            self.send_header("ETag", etag)
            self.send_header("Last-Modified", modified)
        if q.get("attach"):
            self.send_header("Content-Disposition", 'attachment; filename="%s"' % name)
        self.send_header("Connection", "close")
        self.end_headers()
        if head:
            return
        rate = float(q.get("rate", "0")) * 1024
        totalrate = float(q.get("totalrate", "0")) * 1024
        drops = [int(x) for x in q.get("drop", "").split(",") if x]
        stall = int(q.get("stall", "-1"))
        stallall = int(q.get("stallall", "-1"))
        sent, t0, pos = 0, time.time(), start
        try:
            for piece in (read_real(self.real, start, end) if getattr(self, "real", None) else generate(start, end, version)):
                while piece:
                    if pos == stallall:
                        self.log_message("stalling at %d (every time)", stallall)
                        time.sleep(600)
                        return
                    cut = len(piece)
                    for d in drops + ([stall] if stall >= 0 else []):
                        if pos < d < pos + cut and (key, d) not in state["fired"]:
                            cut = d - pos
                    if pos < stallall < pos + cut:
                        cut = stallall - pos
                    n = min(cut, 65536)
                    if q.get("maxconn") and pos + n == end:
                        self.release(u.path)
                    self.wfile.write(piece[:n])
                    count(u.path, "bytes", n)
                    piece = piece[n:]
                    pos += n
                    sent += n
                    for d in drops:
                        if pos == d and (key, d) not in state["fired"]:
                            state["fired"].add((key, d))
                            self.log_message("dropping the connection at %d", d)
                            self.wfile.flush()
                            self.connection.shutdown(2)
                            return
                    if pos == stall and (key, stall) not in state["fired"]:
                        state["fired"].add((key, stall))
                        self.log_message("stalling at %d", stall)
                        time.sleep(600)
                        return
                    if rate:
                        ahead = sent / rate - (time.time() - t0)
                        if ahead > 0:
                            time.sleep(ahead)
                    if totalrate:
                        with lock:
                            now = time.time()
                            entry = paced.setdefault(u.path, [0, now])
                            if now - entry[0] / totalrate - entry[1] > 1:
                                entry[:] = [0, now - 0.0]  # idle a while: no saved-up burst
                            entry[0] += n
                            ahead = entry[0] / totalrate - (now - entry[1])
                        if ahead > 0:
                            time.sleep(ahead)
        except (BrokenPipeError, ConnectionResetError) as e:
            self.log_message("client went at %d (%s)", pos, e.__class__.__name__)
            return
        took = time.time() - t0
        self.log_message("sent %d bytes [%d-%d) in %.1f s (%.1f MB/s)", sent, start, end, took, sent / 1048576 / max(took, 1e-6))


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True
    request_queue_size = 128


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8781)
    ap.add_argument("--sha", type=int, help="print the SHA-256 of a file of this size and exit")
    ap.add_argument("--version", type=int, default=1)
    ap.add_argument("--root", help="the folder /real/<name> serves")
    a = ap.parse_args()
    state["root"] = a.root
    if a.sha is not None:
        print(sha(a.sha, a.version))
        return
    print("serving on :%d (page: /page)" % a.port, flush=True)
    Server(("0.0.0.0", a.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
