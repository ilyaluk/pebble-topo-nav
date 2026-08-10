#!/usr/bin/env python3
"""Replace the installed pypkjs geolocation with a file-driven fake.

Stock pypkjs resolves position by IP lookup (api.ipify.org + a bundled
GeoLite database): useless offline, blocked in sandboxes, watchPosition
fires only once, and failure callbacks get no error object. This patch
makes the emulator's navigator.geolocation read fixes from a local file,
so the app's real GPS code paths run unmodified.

    python3 tools/patch-pypkjs-gps.py            # apply (idempotent)
    python3 tools/patch-pypkjs-gps.py --restore  # put the original back
    pebble kill                                  # then respawn pypkjs

Fix file: $PYPKJS_FAKE_GPS_FILE or ~/.pebble-fake-gps, containing
"lat,lon[,altitude[,speed[,heading]]]". Rewrite it any time to move the
position; the app sees the new fix on its next poll. A missing file
yields a proper POSITION_UNAVAILABLE error. watchPosition polls every
$PYPKJS_FAKE_GPS_INTERVAL seconds (default 1).
"""

import argparse
import shutil
import subprocess
import sys

MARKER = "# PYPKJS-FAKE-GPS PATCH"

REPLACEMENT = MARKER + '''
# Installed by pebble-topo-nav/tools/patch-pypkjs-gps.py; original kept
# alongside as geolocation.py.orig. Reads fixes from a local file instead
# of IP-based lookup; see the patcher's docstring for the file format.

import STPyV8 as v8
import gevent
import os
import os.path
import time

Position = lambda runtime, *args: v8.JSObject.create(runtime.context.locals.Position, args)
Coordinates = lambda runtime, *args: v8.JSObject.create(runtime.context.locals.Coordinates, args)
PositionError = lambda runtime, *args: v8.JSObject.create(runtime.context.locals.PositionError, args)

DEFAULT_FIX_FILE = os.path.expanduser('~/.pebble-fake-gps')


class Geolocation(object):
    def __init__(self, runtime):
        self.runtime = runtime
        self._watches = {}
        self._next_watch_id = 1

        runtime.run_js("""
            Position = (function(coords, timestamp) {
                this.coords = coords;
                this.timestamp = timestamp;
            });
        """)

        runtime.run_js("""
            Coordinates = (function(long, lat, accuracy, altitude, speed, heading) {
                this.longitude = long;
                this.latitude = lat;
                this.accuracy = accuracy;
                this.altitude = altitude;
                this.speed = speed;
                this.heading = heading;
            });
        """)

        runtime.run_js("""
            PositionError = (function(code, message) {
                this.code = code;
                this.message = message;
                this.PERMISSION_DENIED = 1;
                this.POSITION_UNAVAILABLE = 2;
                this.TIMEOUT = 3;
            });
        """)

    def _read_fix(self):
        path = os.environ.get('PYPKJS_FAKE_GPS_FILE', DEFAULT_FIX_FILE)
        with open(path) as f:
            parts = [float(p) for p in f.read().strip().split(',')]
        if len(parts) < 2:
            raise ValueError('need at least "lat,lon" in %s' % path)
        parts += [0.0] * (5 - len(parts))
        return parts[:5]

    def _deliver(self, success, failure):
        try:
            lat, lon, alt, speed, heading = self._read_fix()
        except (IOError, OSError, ValueError) as e:
            if callable(failure):
                self.runtime.enqueue(failure, PositionError(self.runtime, 2, 'fake-gps: %s' % e))
        else:
            coords = Coordinates(self.runtime, lon, lat, 5.0, alt, speed, heading)
            self.runtime.enqueue(success, Position(self.runtime, coords, round(time.time() * 1000)))

    def _enabled(self):
        return True

    def getCurrentPosition(self, success, failure=None, options=None):
        self.runtime.group.spawn(self._deliver, success, failure)

    def watchPosition(self, success, failure=None, options=None):
        watch_id = self._next_watch_id
        self._next_watch_id += 1
        interval = float(os.environ.get('PYPKJS_FAKE_GPS_INTERVAL', '1'))

        def loop():
            while watch_id in self._watches:
                self._deliver(success, failure)
                gevent.sleep(interval)

        self._watches[watch_id] = True
        self.runtime.group.spawn(loop)
        return watch_id

    def clearWatch(self, watch_id):
        self._watches.pop(int(watch_id), None)
'''


def find_geolocation_py():
    pebble = shutil.which("pebble")
    if not pebble:
        sys.exit("pebble CLI not found on PATH")
    with open(pebble) as f:
        shebang = f.readline().strip()
    if not shebang.startswith("#!"):
        sys.exit("cannot resolve pebble-tool venv from %s" % pebble)
    venv_python = shebang[2:]
    out = subprocess.check_output(
        [venv_python, "-c",
         "import pypkjs.javascript.navigator.geolocation as g; print(g.__file__)"],
        text=True)
    return out.strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--restore", action="store_true", help="reinstall the original module")
    args = ap.parse_args()

    target = find_geolocation_py()
    backup = target + ".orig"

    if args.restore:
        try:
            shutil.copyfile(backup, target)
        except FileNotFoundError:
            sys.exit("no backup at %s -- nothing to restore" % backup)
        print("restored %s" % target)
    else:
        with open(target) as f:
            current = f.read()
        if current.startswith(MARKER):
            print("already patched: %s" % target)
            return
        shutil.copyfile(target, backup)
        with open(target, "w") as f:
            f.write(REPLACEMENT)
        print("patched %s (backup at %s)" % (target, backup))
    print("run `pebble kill` so the next command respawns pypkjs")


if __name__ == "__main__":
    main()
