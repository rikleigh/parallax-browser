#!/usr/bin/env python3
"""Serve test media (HLS playlists and segments, MP4) from the Mac to the PS5.

    tools/serve-media.py [--port 8771] [dir]    # dir: toolchain/wpe-webkit/media

Then on the PS5: file:///data/ps5webkit/video.html?src=http://<mac>:8771/hls-ts/master.m3u8
Each request is logged with its User-Agent and Range, so a run shows who
fetched what: WebKit's NetworkProcess (webkitwebsrc, the browser's user
agent) or something else. HLS content types are set explicitly (Python's
table has no .m4s), and every response allows cross-origin use.
"""
import argparse
import functools
import http.server
import os
import sys
import time

TYPES = {
    ".m3u8": "application/vnd.apple.mpegurl",
    ".ts": "video/mp2t",
    ".m4s": "video/iso.segment",
    ".mp4": "video/mp4",
    ".m4a": "audio/mp4",
    ".webm": "video/webm",
}


class Handler(http.server.SimpleHTTPRequestHandler):
    protocol_version = "HTTP/1.1"  # keep-alive (responses carry Content-Length)

    def guess_type(self, path):
        return TYPES.get(os.path.splitext(path)[1].lower()) or super().guess_type(path)

    def end_headers(self):
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def log_message(self, fmt, *args):
        pass

    def log_request(self, code="-", size="-"):
        ua = self.headers.get("User-Agent", "-")
        rng = self.headers.get("Range")
        sys.stdout.write("%s %s %s %s%s  UA: %s\n" % (time.strftime("%H:%M:%S"), self.client_address[0], code,
                         self.path, "  Range: " + rng if rng else "", ua))
        sys.stdout.flush()


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8771)
    ap.add_argument("dir", nargs="?", default=os.path.join(root, "toolchain/wpe-webkit/media"))
    a = ap.parse_args()
    server = http.server.ThreadingHTTPServer(("", a.port), functools.partial(Handler, directory=a.dir))
    print("serving %s on port %d" % (a.dir, a.port), flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
