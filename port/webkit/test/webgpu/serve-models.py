#!/usr/bin/env python3
"""Serve a directory of model files to the test pages, with CORS headers.

    serve-models.py <directory> [port]        (default port 8765)

ml-transformers.html?models=http://<this machine>:8765/ then takes the model
from here instead of huggingface.co. The directory mirrors the hub's paths:
<directory>/Xenova/all-MiniLM-L6-v2/resolve/main/{config.json,tokenizer.json,
tokenizer_config.json,onnx/model.onnx,onnx/model_fp16.onnx}.
"""
import http.server
import os
import sys


class Handler(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Headers", "*")
        self.send_header("Access-Control-Expose-Headers", "Content-Length, Content-Range, ETag")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def do_OPTIONS(self):
        self.send_response(204)
        self.end_headers()


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    os.chdir(sys.argv[1])
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 8765
    http.server.ThreadingHTTPServer(("", port), Handler).serve_forever()
