# What it is

Map navigation + GPX route guidance + activity recorder app for Pebble smartwatches. Split architecture: C watchapp + PebbleKit JS phone companion. Built with the Pebble SDK.

# Components

- src/c/main.c — watch UI: map layer, header/footer, dashboard, big-nav popup, route-selection MenuLayer + confirm window. Reassembles the map framebuffer from AppMessage chunks; handles clicks (zoom up/down, select = views/menu, long-select = record), CompassService, BatteryStateService, connection/data-stream watchdog.
- src/pkjs/index.js — companion orchestrator: GPS watcher (geolocation), navigation state (closest-track-point index, turn/off-route detection + vibration triggers, avg speed/elevation stats), recording with localStorage persistence, route management, and chunked AppMessage transfer of rendered frames.
- src/pkjs/graphics.js — Web Mercator math, fetches map tiles from tile server, stitches them, draws route/breadcrumb lines, downsamples to 200×150 GColor8 (64-color) framebuffer.
- src/pkjs/png.js — pure-JS PNG decoder since PebbleKit JS has no Canvas/Node libs.
- src/pkjs/config.html — webview settings page: GPX upload, route naming, trip history map, GPX export. `pebble build` wraps it into the gitignored src/pkjs/config_page.js; index.js injects settings into the `/*$$PARAMS$$*/` placeholder and opens it as a data: URI, so no hosting is needed.
- package.json — app metadata and messageKeys

Data flow: config.html → index.js (settings/GPX) → graphics.js renders frame → png.js decodes tiles → chunks sent to main.c via AppMessage; watch sends back zoom/route/view-mode changes and requests map updates.

# Prerequisites

Install Pebble CLI and dependencies, if not already installed:
```
uv tool install pebble-tool
pebble sdk install latest
```


# Building and testing

Build the app:
```
pebble build
```

Check settings-page translations (keys, unused entries, markup/English drift):
```
node tools/check-i18n.js
```

# Emulator testing

Default platform: emery (Pebble Time 2). Never pass --vnc (mixed display flags kill and respawn QEMU mid-session).

One-time setup — emulator geolocation is IP-based and dead offline; make it read a local file instead:
```
python3 tools/patch-pypkjs-gps.py && pebble kill   # re-apply after pebble-tool upgrades
echo "47.3769,8.5417" > ~/.pebble-fake-gps          # lat,lon[,alt[,speed[,heading]]]
```
Rewrite the file to move; the app picks it up every 30s idle, every gpsInterval (5s) while recording.

- Build, run, capture logs in one step: `pebble build && pebble install --emulator emery --logs > /tmp/pebble-logs.txt 2>&1 &` — C APP_LOG + pkjs console.log from app start; the stream survives reinstalls, dies on `pebble kill`. (`QemuInboundPacket.footer` warnings are noise.)
- Usually auto-launches; check with `pebble screenshot out.png --no-open`. If on the watchface: `pebble emu-button click select` twice (launcher).
- Buttons: `pebble emu-button click up|down|select|back`; long-press select: `--duration 800` (record toggle). Zoom starts at 17 = max, so test with `down`. `back` on the map view exits the app.
- Map e2e without external endpoints (app must be foreground):
  ```
  python3 tools/mock-tile-server.py &
  python3 tools/emu-inject-settings.py --preset e2e
  ```
  Presets send a complete settings JSON — partial payloads corrupt state. Extra pairs merge over the preset: `--preset e2e '{"navViewMode":2}'`. Inject a route via newRoute `{name, points:[{lat,lon,ele}]}`; adding auto-activates it and stops any recording.
- `pebble send-app-message` injects a raw phone→watch dict without pkjs: run from the project dir, numeric keys from build/appinfo.json.
- `emu-battery --percent N` works. `emu-bt-connection --connected no` kills pypkjs and orphans QEMU — session over, run it last if at all.
- Exiting the app logs `Heap Usage for App <TopoNav>` — free C-side leak check.
- Cleanup: `pebble kill` then `pebble wipe` (wipe while running does nothing); after kill, the next command needs `--emulator emery` again.

# Docs references:

Platform documentation: https://github.com/coredevices/sdk-docs, prefer to clone it and query locally than to fetch from internet. Don't forget to check for repo freshness.

Unlikely you'll need those, but in case of deep troubleshooting:
PebbleOS source: https://github.com/coredevices/pebbleos
Mobile app source: https://github.com/coredevices/mobileapp

Keep references to those repos in memory.

# Conventions

Commit when making changes to the codebase.

Do not write AI-sloppy comments, e.g. process narration like "used to"/"now", diff-justification, restating code.

Keep files maintaintained:
- This file (AGENTS.md) should be extremely succinct, do not add extra details, use existing text as reference.
- CHANGELOG.md should be maintained, keep it short and to the point.
