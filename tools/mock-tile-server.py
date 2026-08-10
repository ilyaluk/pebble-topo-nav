#!/usr/bin/env python3
"""Local mock tile server for emulator e2e tests (see AGENTS.md).

Serves deterministic 256x256 RGB PNGs at /{z}/{x}/{y}.png so map rendering
can be tested without hitting external tile servers. Tiles are checkerboards
whose colors encode the tile coordinates, with a black border, so stitching,
panning and zooming are visually verifiable in watch screenshots.

Usage:
    python3 tools/mock-tile-server.py [--port 8747]

Point the app at it with the harness settings injection:
    mapSource=custom, customTileUrl=http://localhost:8747/{z}/{x}/{y}.png
"""

import argparse
import re
import struct
import zlib
from http.server import BaseHTTPRequestHandler, HTTPServer

TILE = 256
CELL = 32
BORDER = 4

# Base palette rotated by zoom so zoom changes are visible.
PALETTE = [
    (0xE8, 0xD0, 0xA0), (0xA8, 0xD8, 0xA8), (0xA0, 0xC8, 0xE8),
    (0xE0, 0xB0, 0xB0), (0xD0, 0xB8, 0xE0), (0xE8, 0xE8, 0xB0),
]


def tile_png(z, x, y):
    base = PALETTE[(x + y + z) % len(PALETTE)]
    alt = tuple(max(0, c - 0x30) for c in base)
    rows = []
    for py in range(TILE):
        row = bytearray(b"\x00")  # filter type 0
        for px in range(TILE):
            if px < BORDER or px >= TILE - BORDER or py < BORDER or py >= TILE - BORDER:
                row += b"\x20\x20\x20"
            else:
                c = base if ((px // CELL) + (py // CELL)) % 2 == 0 else alt
                row += bytes(c)
        rows.append(bytes(row))
    raw = b"".join(rows)

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data)))

    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", TILE, TILE, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 6))
            + chunk(b"IEND", b""))


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        m = re.match(r"^/(\d+)/(-?\d+)/(-?\d+)\.png$", self.path)
        if not m:
            self.send_error(404)
            return
        body = tile_png(*(int(g) for g in m.groups()))
        self.send_response(200)
        self.send_header("Content-Type", "image/png")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        print("[tile]", fmt % args, flush=True)


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8747)
    args = ap.parse_args()
    print("mock tile server on http://localhost:%d/{z}/{x}/{y}.png" % args.port,
          flush=True)
    HTTPServer(("127.0.0.1", args.port), Handler).serve_forever()
