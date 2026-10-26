#!/usr/bin/env python3
import http.server
import socketserver
import os
import sys
from pathlib import Path

SERVE_DIR = Path("dist/store-0.1.1.0/Qinglan-MicrosoftStore-Package-40282772918/msixvc").resolve()
PORT = 8888

class CorsRequestHandler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(SERVE_DIR), **kwargs)

    def end_headers(self):
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Access-Control-Allow-Methods', 'GET, HEAD, OPTIONS')
        self.send_header('Access-Control-Allow-Headers', '*')
        self.send_header('Access-Control-Allow-Private-Network', 'true')
        super().end_headers()

    def do_OPTIONS(self):
        self.send_response(204)
        self.end_headers()

def run():
    print(f"Serving {SERVE_DIR} on http://127.0.0.1:{PORT}")
    socketserver.TCPServer.allow_reuse_address = True
    with socketserver.TCPServer(('127.0.0.1', PORT), CorsRequestHandler) as httpd:
        httpd.serve_forever()

if __name__ == "__main__":
    run()
