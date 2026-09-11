#!/usr/bin/env python3
"""Minimal static server that never lets the browser cache responses.

`python -m http.server` sends no Cache-Control, so browsers may serve a stale
copy of edited .html files. This handler sends `Cache-Control: no-store` on
every response, making browser reloads always fetch fresh bytes.

Usage:
    python serve_nocache.py [port]   (serves the current directory; default 8081)
"""
import http.server
import socketserver
import sys
import time
import urllib.parse

LOG_FILE = "director_log.txt"


class NoCacheHandler(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cache-Control", "no-store, no-cache, max-age=0")
        self.send_header("Pragma", "no-cache")
        self.send_header("Expires", "0")
        super().end_headers()

    def _append(self, line):
        with open(LOG_FILE, "a") as f:
            f.write(line + "\n")

    def do_GET(self):
        # GET /log?m=<url-encoded line>  -> append to director_log.txt
        if self.path.startswith("/log?"):
            q = urllib.parse.parse_qs(self.path[5:])
            for msg in q.get("m", []):
                self._append(msg)
            self.send_response(200)
            self.end_headers()
            self.wfile.write(b"ok")
            return
        super().do_GET()

    def do_POST(self):
        # POST body = one line (sendBeacon/fetch POST) -> append to log
        ln = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(ln).decode("utf-8", "replace") if ln else ""
        if body:
            self._append(body)
        self.send_response(200)
        self.end_headers()
        self.wfile.write(b"ok")


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8081
    with socketserver.TCPServer(("", port), NoCacheHandler) as httpd:
        httpd.allow_reuse_address = True
        print(f"no-cache http server on port {port}, serving {'.' if False else '[cwd]'}")
        httpd.serve_forever()