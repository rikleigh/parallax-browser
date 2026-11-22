#!/usr/bin/env python3
"""A stand-in for a homebrew launcher's API on 127.0.0.1, for launchers-test.sh.

  fake.py smp|kraken|titan PORT LOG [STATE]

smp answers ShadowMountPlus's POST /api/v1/* (version, settings, scan,
manual/add, games, games/mount, games/unmount: 409 with "busy"); kraken
and titan answer Kraken
Mount's GET /kraken.appcache (titan also /titan.appcache), /api/settings,
/api/state and POST /api/rescan, /api/rebuild/<ID>. STATE, a JSON file,
holds "scan_paths", "games", "busy" (smp) or "scanpath", "games",
"paused" (kraken, titan); any answers 200 with HTML to everything. Read on
each request. Every request is appended to LOG as "METHOD PATH BODY".
"""
import json
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer

mode, port, log = sys.argv[1], int(sys.argv[2]), sys.argv[3]
state_path = sys.argv[4] if len(sys.argv) > 4 else None


def state():
    if not state_path:
        return {}
    try:
        with open(state_path) as f:
            return json.load(f)
    except OSError:
        return {}


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def reply(self, code, body=b"", kind="application/json", chunked=False):
        self.send_response(code)
        self.send_header("Content-Type", kind)
        if chunked:
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            for i in range(0, len(body), 7):
                part = body[i:i + 7]
                self.wfile.write(b"%x\r\n%s\r\n" % (len(part), part))
            self.wfile.write(b"0\r\n\r\n")
        else:
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    def record(self, body=b""):
        with open(log, "a") as f:
            f.write("%s %s %s\n" % (self.command, self.path, body.decode("utf-8", "replace")))

    def do_GET(self):
        self.record()
        if mode == "smp":
            return self.reply(404)
        if mode == "any":
            return self.reply(200, b"<html>whatever</html>", "text/html")
        if self.path == "/kraken.appcache" or (mode == "titan" and self.path == "/titan.appcache"):
            return self.reply(200, b"CACHE MANIFEST\n", "text/cache-manifest")
        s = state()
        if self.path == "/api/settings":
            return self.reply(200, json.dumps({"scanpath": s.get("scanpath", []), "scan_interval_seconds": 10}).encode())
        if self.path == "/api/state":
            # chunked, as a server might: the client has to put it together
            return self.reply(200, json.dumps({"version": "0.3.1", "paused": s.get("paused", False), "games": s.get("games", []),
                                               "log": ["x"] * 50}).encode(), chunked=True)
        self.reply(404, b"not found", "text/plain")

    def do_POST(self):
        length = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(length) if length else b""
        self.record(body)
        if mode == "smp":
            s = state()
            if self.path == "/api/v1/version":
                return self.reply(200, json.dumps({"status": 0, "api_version": 1, "shadowmount_version": "1.7"}).encode())
            if self.path == "/api/v1/settings":
                return self.reply(200, json.dumps({"status": 0, "scan_paths": s.get("scan_paths", [])}).encode())
            if self.path == "/api/v1/games":
                return self.reply(200, json.dumps({"status": 0, "count": len(s.get("games", [])), "games": s.get("games", [])}).encode())
            if self.path == "/api/v1/games/unmount" and s.get("busy"):
                return self.reply(409, json.dumps({"status": 16, "error": "the title is running"}).encode())
            if self.path in ("/api/v1/scan", "/api/v1/manual/add", "/api/v1/games/mount", "/api/v1/games/unmount"):
                return self.reply(200, json.dumps({"status": 0, "scan_queued": True}).encode())
            return self.reply(404, json.dumps({"status": 2, "error": "no route"}).encode())
        if self.path == "/api/rescan" or self.path.startswith("/api/rebuild/"):
            # as Kraken Mount: no Content-Length, the connection closed
            self.send_response(204)
            self.send_header("Connection", "close")
            self.end_headers()
            self.close_connection = True
            return
        self.reply(404, b"not found", "text/plain")


HTTPServer(("127.0.0.1", port), Handler).serve_forever()
