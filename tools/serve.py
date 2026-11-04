#!/usr/bin/env python3
"""Serves web/ on localhost, for the page in a desktop browser with the Pico on USB (Web Serial and Web MIDI
need https or localhost).

usage: tools/serve.py [port] [-q]   (default 8460; -q: no request log). Compile the page first:
       npm run build (or watch) in web/.
"""
import os
import sys
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "web")


class Handler(SimpleHTTPRequestHandler):
    extensions_map = {**SimpleHTTPRequestHandler.extensions_map, ".mjs": "text/javascript", ".js": "text/javascript"}

    def end_headers(self):
        self.send_header("Cache-Control", "no-store")  # an edit shows on reload
        super().end_headers()

    def log_message(self, format, *args):
        if "-q" not in sys.argv:
            super().log_message(format, *args)


def main():
    port = int(next((a for a in sys.argv[1:] if a.isdigit()), 8460))
    server = ThreadingHTTPServer(("127.0.0.1", port), partial(Handler, directory=ROOT))
    print(f"serving web/ on http://127.0.0.1:{port}/")
    server.serve_forever()


if __name__ == "__main__":
    main()
