#!/usr/bin/env python3
"""Inject a settings JSON into the app running in the emulator, headlessly.

Drives the `pebble emu-app-config` flow without a browser: the tool asks the
app for its config page, serves a local /close endpoint, and forwards whatever
hits that endpoint to the app's `webviewclosed` handler. This script posts the
given JSON there directly.

Usage:
    python3 tools/emu-inject-settings.py '{"mapSource":"custom", ...}'
    python3 tools/emu-inject-settings.py < settings.json

The JSON mirrors what config.html submits; see its settings object. Extra
harness-only keys understood by index.js: customTileUrl, mockGps.
"""

import glob
import json
import os
import re
import subprocess
import sys
import time
import urllib.parse
import urllib.request

TMP_GLOB = os.path.expanduser("~/pebble-tool-emu-app-config-*.html")


def main():
    payload = sys.argv[1] if len(sys.argv) > 1 else sys.stdin.read()
    json.loads(payload)  # fail fast on malformed JSON

    before = set(glob.glob(TMP_GLOB))
    env = dict(os.environ, BROWSER="/bin/true")  # keep webbrowser quiet
    proc = subprocess.Popen(["pebble", "emu-app-config"],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            env=env, text=True)

    port = None
    for _ in range(240):  # the app must be running for openURL to answer
        time.sleep(0.25)
        fresh = set(glob.glob(TMP_GLOB)) - before
        if fresh:
            with open(fresh.pop()) as f:
                m = re.search(r"localhost(?:%3A|:)(\d+)(?:%2F|/)close", f.read())
            if m:
                port = int(m.group(1))
                break
        if proc.poll() is not None:
            print(proc.stdout.read())
            sys.exit("emu-app-config exited early -- is the app running?")
    if port is None:
        proc.kill()
        sys.exit("timed out waiting for the config return endpoint")

    url = "http://localhost:%d/close?%s" % (port, urllib.parse.quote(payload))
    with urllib.request.urlopen(url, timeout=10) as resp:
        resp.read()
    proc.wait(timeout=15)
    print("settings delivered")


if __name__ == "__main__":
    main()
