"""Temporarily serve one firmware image to one board, then exit.

No directory listings or other files are exposed. Run only for an authorized
OTA transfer; the board's `ota` command still initiates the installation.
"""

import argparse
from http.server import BaseHTTPRequestHandler, HTTPServer
from ipaddress import IPv4Address
from pathlib import Path
import shutil
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument("--bind", type=IPv4Address, required=True)
    parser.add_argument("--peer", type=IPv4Address, required=True)
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--timeout", type=int, default=180)
    args = parser.parse_args()
    image = args.image.resolve(strict=True)
    if not image.is_file() or image.suffix.lower() != ".bin":
        parser.error("image must be a firmware .bin file")
    if args.timeout <= 0 or not 1 <= args.port <= 65535:
        parser.error("timeout must be positive and port must be in 1..65535")
    completed = False

    class BoardServer(HTTPServer):
        def verify_request(self, request, client_address):
            request.settimeout(30)
            return client_address[0] == str(args.peer)

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            nonlocal completed
            if self.path != "/" + image.name:
                self.send_error(404)
                return
            self.connection.settimeout(30)
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(image.stat().st_size))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            with image.open("rb") as source:
                shutil.copyfileobj(source, self.wfile)
            self.wfile.flush()
            completed = True

    with BoardServer((str(args.bind), args.port), Handler) as server:
        server.timeout = 1
        deadline = time.monotonic() + args.timeout
        print(f"Serving only {image.name} to {args.peer}; transfer window {args.timeout}s", flush=True)
        while not completed and time.monotonic() < deadline:
            server.handle_request()
    if not completed:
        raise SystemExit("No completed transfer before the deadline")
    print("Transfer complete; server stopped", flush=True)


if __name__ == "__main__":
    main()
