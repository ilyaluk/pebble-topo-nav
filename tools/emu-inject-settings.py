#!/usr/bin/env python3
"""Inject a settings JSON into the app running in the emulator, headlessly.

Drives the `pebble emu-app-config` flow without a browser: the tool asks the
app for its config page, serves a local /close endpoint, and forwards whatever
hits that endpoint to the app's `webviewclosed` handler. This script posts the
given JSON there directly. The app must be running and foreground.

Usage:
    python3 tools/emu-inject-settings.py --preset e2e
    python3 tools/emu-inject-settings.py --preset e2e '{"navViewMode":2}'
    python3 tools/emu-inject-settings.py '{"gpsInterval":5, ...}'   # full JSON
    python3 tools/emu-inject-settings.py --gps 47.3769,8.5417       # fix file only

Settings must be a COMPLETE object (mirror config.html's settings object) —
the app has no defaults for missing keys. Presets provide that base; a
positional JSON argument is merged over the chosen preset.

--gps writes ~/.pebble-fake-gps (or $PYPKJS_FAKE_GPS_FILE) for the pypkjs
fake-GPS patch; it can be combined with an injection or used alone.
"""

import argparse
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

PRESETS = {
    "e2e": {
        "gpsInterval": 5,
        "language": "en",
        "mapSource": "custom",
        "customTileUrl": "http://localhost:8747/{z}/{x}/{y}.png",
        "fullscreen": False,
        "showBreadcrumbs": True,
        "navViewMode": 0,
        "dashboardFields": 15,
    },
}


def inject(payload):
    before = set(glob.glob(TMP_GLOB))
    env = dict(os.environ, BROWSER="/bin/true")  # keep webbrowser quiet
    proc = subprocess.Popen(["pebble", "emu-app-config"],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            env=env, text=True)

    port = None
    for _ in range(240):
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("payload", nargs="?",
                    help="settings JSON; with --preset, merged over the preset")
    ap.add_argument("--preset", choices=sorted(PRESETS),
                    help="named complete settings base")
    ap.add_argument("--gps", metavar="LAT,LON[,...]",
                    help="write the pypkjs fake-GPS fix file")
    args = ap.parse_args()

    if args.gps:
        path = os.environ.get("PYPKJS_FAKE_GPS_FILE",
                              os.path.expanduser("~/.pebble-fake-gps"))
        with open(path, "w") as f:
            f.write(args.gps + "\n")
        print("fix file: %s <- %s" % (path, args.gps))

    if args.preset or args.payload:
        settings = dict(PRESETS[args.preset]) if args.preset else {}
        if args.payload:
            settings.update(json.loads(args.payload))
        inject(json.dumps(settings))
    elif not args.gps:
        ap.error("nothing to do: give a payload, --preset, or --gps")


if __name__ == "__main__":
    main()
