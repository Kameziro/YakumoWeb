#!/usr/bin/env python3
"""Serves the web build locally with the headers it needs.

The port starts threads, which run on SharedArrayBuffer, and browsers only
allow that on a cross-origin isolated page. Listens on 127.0.0.1 only.

    python web/serve.py [directory] [port]      # defaults: build/web 8765
"""
import functools
import http.server
import sys


class IsolatedHandler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {**http.server.SimpleHTTPRequestHandler.extensions_map, ".wasm": "application/wasm"}

    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cache-Control", "no-cache")
        super().end_headers()


def main():
    directory = sys.argv[1] if len(sys.argv) > 1 else "build/web"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 8765
    handler = functools.partial(IsolatedHandler, directory=directory)
    server = http.server.ThreadingHTTPServer(("127.0.0.1", port), handler)
    print(f"Serving {directory} on http://127.0.0.1:{port}/Yakumo.html")
    server.serve_forever()


if __name__ == "__main__":
    main()
