# Changelog

## [Unreleased]

### Added
- **Custom Tile Server:** A `custom` map source with a `{z}/{x}/{y}` URL template field in the settings page (also previews in the Leaflet trip map). Used by the emulator test harness (`tools/mock-tile-server.py`, `tools/emu-inject-settings.py`) to render maps without external endpoints.
- **pypkjs Fake-GPS Patcher:** `tools/patch-pypkjs-gps.py` swaps the emulator's IP-based geolocation for a file-driven one (`~/.pebble-fake-gps`), so the app's real GPS path runs in the emulator with live-movable positions, repeating `watchPosition`, and proper `PositionError` objects.
- **Phone Link & Data-Stream Watchdog:** Recording lives entirely in the phone JS, so a dropped Bluetooth link or a suspended companion app used to stop the recording silently while the watch kept showing the last received state. The watch now detects both a real disconnect and a quiet JS runtime (no status message for 120s during a recording), buzzes a distinct long-long-long pattern, turns the GPS indicator red and names the failure in the footer; reconnecting gives a short confirmation pulse.

### Fixed
- **Dashboard Broke at 5+ Fields:** The grid only handled 1–4 fields; with 5 (the default), fields overprinted the GPS-coords row and the rest fell off-screen. The grid now scales to any count, giving an odd count's last field the full row width, and the settings page no longer caps the selection at 4.
- **Hung Tile Fetches Wedged the Map:** Tile downloads had no deadline; one stalled request left the map on "Loading map…" forever with no log output. Fetches now time out after 10s and fail like any other fetch error, and a skipped render logs its reason (map hidden / arrow-only) once per episode.
- **Custom Tile Server Changes Kept Stale State:** Cached tiles and the render signature were keyed by the source name alone, so all custom URLs shared one namespace — switching servers reused the old server's tiles and often skipped redrawing entirely. Both are now keyed by the URL.
- **Route IDs Overflowed int32:** New routes got `id: Date.now()` (~1.8e12), which exceeds AppMessage's int32 range; `Pebble.sendAppMessage` then threw on every status send, killing navigation and map updates until storage was wiped. Ids are now masked to 31 bits at creation, and `sendStatusMessage` catches serialization throws so one bad value can no longer take down the GPS callback chain.
- **Missing `gpsInterval` Persisted as "undefined":** The only setting read without a default now falls back to 5 seconds.
- **Emulator JS Crash on GPS Failure:** pypkjs delivers geolocation errors without an error object; the unguarded `err.message` access tore down the whole JS runtime.
- **No Tile Was Ever Cached:** PebbleKit JS exposes no `btoa`/`atob`, so every fetched tile threw `ReferenceError` right after decoding and never reached localStorage — each map update re-downloaded its tiles. Base64 is now encoded and decoded in pure JS.
- **Turn Detection Compared Against a Stale Bearing:** A typo (`previousBaring`) left the lookahead measuring every upcoming segment against the first one, so gradual curves accumulating more than 30° were announced as turns and real turns after any drift were mislocated — wrong instructions plus spurious popups and vibration.
- **Right Turns Announced as Left:** The bearing difference used `(d + 180) % 360 - 180`, but JS `%` keeps the dividend's sign, so differences below -180° (e.g. 350° → 100°, a 110° right turn) fell outside the range and flipped direction.
- **Crash on Reaching the End of the Track:** The remaining-distance and turn-lookahead math dereferenced the point after the closest one, which does not exist at the final point — arriving at the destination threw a `TypeError` out of the GPS callback. Lookahead is now clamped to the track end.
- **Dashboard Layer Freed Twice:** `main_window_unload` destroyed `s_dashboard_layer` in two places, a double free on app exit or window reload.
- **Lost Map-Visibility Reports Suspended Streaming:** A visibility report that was accepted but later NACKed left the phone out of sync, and with the phone idle and stationary nothing triggered the opportunistic retry, so map streaming could stay suspended until a zoom press. The watch now rolls back and resends on send failure, bounded to three attempts per transition.

### Changed
- **English Is the Default Language:** The watch and the settings page default to English; German remains selectable. Installs that never explicitly picked a language switch to English on update.
- **Settings Page Localization:** Static page text is translated from `data-i18n` attributes instead of per-element assignments, so a new label needs only its attribute and a dictionary entry. `tools/check-i18n.js` fails on keys missing from a dictionary, unused entries, and lookups of absent elements.
- **Settings Page Bundled Into the App:** The settings page is embedded in the app and opened as a data URI instead of being fetched from `sirtob1.github.io`. It no longer needs a connection (except the trip history map), cannot drift from the installed version, and stops routes and trip history from travelling through a third-party URL. Settings are injected into the page rather than passed as query parameters, removing the URL-length limit that truncated the routes list.

### Performance
- **GPS Duty-Cycled When Not Navigating:** `watchPosition` with high accuracy ran continuously from app start, keeping the receiver powered even with no route; the configured interval only discarded fixes in JS. The continuous watch is now kept only while navigating or recording, with a one-shot fix every 30s otherwise.
- **Map Frames Compressed:** Frames were sent as raw GColor8 bytes (16–46 KB) in 3000-byte chunks, holding the Bluetooth radio in its high-power state for most of every update. Frames are now PackBits-encoded (with a raw fallback) and chunks grow to 7100 bytes (3000 on aplite), so a typical frame fits in one or two messages instead of six-plus. The watch stages chunks in a heap buffer and decodes only on completion, so an aborted transfer can no longer leave a half-updated image on screen, and a content hash lets the phone skip pixel-identical frames.
- **No Rendering While the Map Is Hidden:** Arrow Only mode, the dashboard, the big-nav popup and the route menu hide the map, yet the phone kept rendering and transmitting full frames every fix — the battery-saving modes paid the full transfer cost. The watch now reports map visibility, and the phone suspends rendering and streaming while hidden.
- **Unchanged Viewports Skipped:** Every fix rendered and pushed a fresh image even while standing still. The phone remembers the centre (±2 px) and a signature of the other render inputs of the last frame the watch fully received, and returns early on a match; renders drawn with missing tiles are not remembered.
- **Decoded Tiles Held in Memory:** Viewport tiles were read from localStorage and base64+PNG decoded on every render, the bulk of phone-side CPU per update. A six-entry LRU of decoded tiles now covers the four-tile viewport plus panning across a boundary, dropped when the map source changes.
- **Compass Powered Only When the Arrow Is Visible:** The magnetometer stayed subscribed for the whole app lifetime with a 2° filter, redrawing the window on wrist jitter even where the arrow ignores the compass. It is now subscribed on demand, filtered at 6°, and skips redraws while GPS course is the bearing source.
- **Status Payloads Diffed:** Every fix shipped the full ~15–21 key status message, including settings that never change, costing the watch a flash write, a dozen relabels and a full redraw each time. Only changed keys are sent now (vibration events always pass through, keys recorded on ACK so failures retry), and the watch gates its relabeling and redraws on actual transitions.
- **Track Indexed Once Instead of Per Fix:** Each fix ran four O(n) passes over the whole GPX track with fresh geodesy allocations — tens of thousands of trig calls per interval on a long track. A one-time index (prefix sums, segment bearings) is rebuilt only when the route changes; the closest-point search scans a ±40 point window with a full-scan fallback beyond 75 m. Verified numerically identical to the previous loops.
- **Polylines Clipped, Tile Rows Block-Copied:** Route segments had no viewport test, so Bresenham walked every off-screen pixel; both overlays now Liang-Barsky clip to the viewport plus brush margin, which also keeps segments crossing it with both endpoints outside. Tile stitching copies rows with typed-array `set`/`subarray` instead of bounds-checking all 65k pixels per tile. Verified pixel-identical.
- **Prefetch at the Displayed Zoom:** The route prefetcher downloaded 3×3 blocks at a hardcoded zoom 15 while the map shows 12–17, so nearly every visible tile still had to be fetched live on the trail. Prefetch now follows the current zoom (debounced 10s, capped at 200 tiles, superseded runs abandoned), and failed tiles are negative-cached for a minute in both the render and prefetch paths.
- **Bounded Track Persistence:** Every breadcrumb re-serialized the entire recorded track, so write cost grew with trip length; moving-time stats were written on every fix and the closest-index even when unchanged. The track now persists as a base array plus a ≤40-point tail, with a full rewrite every 40 points (~400 m); stats piggyback on the 10 m breadcrumb cadence and unchanged indices are not written. Existing stored tracks still load.
- **GPS Outages Reported Once:** In poor signal the geolocation watch fires an error every timeout (~10s), each one sending a full status message and triggering a watch redraw. Only the transition into the error state is sent; a successful fix re-arms it.
- **Tile Cache Bounded and Self-Calibrating:** Cached tiles grew until the localStorage quota was hit, at which point caching stopped working permanently. Tiles are now tracked in a persisted LRU index (lazily flushed) and evicted to make room on render, while the prefetcher stops at the budget instead of evicting its own work. The budget itself is discovered rather than guessed: tiles store freely until a write throws, 90% of the fill level at that point is persisted as the cap, each session raises the loaded cap by 10% to re-probe freed space, and any later quota error clamps it back down.

### Removed
- Stale build scaffolding and checked-in artifacts: `build.sh`, `docker_build.sh`, `coding_guidelines.md`, `.agents/`, `docs/issues/`, and the prebuilt `pebble.pbw` / `pebble_2.6.pbw`.

## [2.8.0] - 2026-08-05

### Changed
- **UI & Navigation Updates (by @patrickvbe):** 
  - **Dashboard & Layout Polish:** Introduced dynamic font scaling in the dashboard based on the number of items. Improved readability for battery and zoom fields, refined the distance formatting, and made the track recording indicator more distinct (open circle when not recording).
  - **Navigation Feedback:** The directional text in the footer now includes the exact distance. The big pop-up arrow behavior has been synced with the turn vibration to prevent overlapping alerts, and its on-map color and size have been tweaked for better contrast.

### Fixed
- **GPS Precision (by @patrickvbe):** Fixed the nearest point and section detection logic. This eliminates false "off-track" warnings and correctly calculates traveled vs. remaining distances on very long track sections.
- **Battery Optimizations (by @patrickvbe):** The app now strictly obeys the configured `gpsInterval` setting to skip overly frequent GPS polling, preserving both watch and phone battery life.
- **Map Sources Rendering White (by @ChrisBoomhower):** Fixed two map sources that displayed a blank white background instead of tiles.
  - **HikeBikeMap → CyclOSM:** The HikeBikeMap tile server (`tiles.wmflabs.org`) was decommissioned by the Wikimedia Foundation and no longer resolves, so the source never returned tiles. Replaced it with **CyclOSM** (`tile-cyclosm.openstreetmap.fr`), a live, free, HTTPS outdoor/hiking-and-cycling basemap with global coverage, and renamed the settings dropdown option accordingly (English & German). Previously-saved `hikebikemap` selections are transparently aliased to CyclOSM so existing users are not reset.
  - **MtbMap over HTTPS:** The MtbMap URL used cleartext `http://`, which is blocked by mobile WebViews / PebbleKit JS (iOS ATS, Android cleartext restrictions). Switched to `https://` (the server already supports it). Note: MtbMap's tile coverage is Europe-only, so it will still render blank in regions without data (e.g. North America) — this is a data-coverage limitation of the source, not a bug.
- Applied the same fixes to the Leaflet map preview in the settings page (`getLeafletTileUrl`) so the in-settings preview matches what the watch renders.

## [2.7.0] - 2026-07-12

### Fixed
- **Pebble Round Layout Support:** Adapted the watch app's UI layout (Header, Footer, Dashboard) to properly fit the round display (Chalk platform) without cutting off text elements or navigation arrows.
- **Dashboard Fields Visibility & Layout (US-11):** Fixed an issue where the Battery and Distance to Destination fields were invisible due to 8-bit truncation. Improved the grid layout to perfectly center the 3rd item when exactly 3 fields are active, and optimized line breaks for Elevation Gain/Loss fields.
- **Settings Page Routes Rendering:** Fixed an issue where the saved routes would not be visible in the Settings Page after a long GPX track was imported. The URL parameters passed to the Settings Page were sometimes getting truncated by the Pebble app due to massive track points in the latest recorded trips. The route parameter has been moved earlier in the URL payload to guarantee delivery.

## [2.6.0] - 2026-07-11

### Added
- **Navigation View Modes (Auto-Popup & Permanent Arrow):** Replaced the simple "Auto-Popup Navigation" setting with a 3-way dropdown for "Navigation View". Users can now choose to see the map, enable an auto-popup full-screen navigation view that shows a large arrow and distance when approaching a turn (< 50m), or permanently display the large arrow (Arrow Only) for quick reading and battery savings.
- **Directional Vibration Patterns:** Added distinct vibration patterns for turns. A left turn triggers a double pulse, while a right turn triggers a single long pulse. Off-route alerts now trigger 3 rapid pulses.
- **Fully Dynamic Dashboard & New Metrics:** Completely overhauled the dashboard grid architecture. Users can now select up to 4 optional fields from 10 different metrics (Speed, Average Speed, Distance, Elevation Gain/Loss, Duration, Altitude, Battery, Compass Heading, Distance to Destination) to display alongside fixed coordinates. The watch automatically adjusts grid layout and maximizes font sizes.

### Fixed
- **Navigation Menu Logic & Dashboard UI:** Fixed click handler logic so that pressing Select while the permanent "Arrow Only" mode is active correctly overlays the Dashboard, and pressing it again cycles to the Routes menu. Resolved overlapping issues and font scaling on the dashboard so characters and units scale alongside numeric metrics properly.

## [2.5.0] - 2026-06-14

### Added
- **Copy GPX to Clipboard Workaround**: Added a "Copy" / "Kopieren" button to the recorded trips list in settings. Since embedded mobile WebViews (especially on iOS) often restrict downloading files or opening local data/Blob URIs, users can now copy the raw GPX XML string directly to their clipboard and paste it into a file as a workaround.

### Changed
- **Capped Zoom Level**: Capped the maximum zoom level to 17 (previously 18) across the watchapp C code and the Leaflet settings map view of recorded routes, to prevent map rendering issues at extreme zoom levels.

## [2.4.0] - 2026-06-14

### Changed
- **Inline Recorded Trips Mapping and GPX Downloads**: Redesigned the "View" map modal and GPX file downloads to execute locally and inline within the settings WebView. This eliminates the legacy behavior of closing and reopening the settings WebView, completely preventing transition crashes and settings page collapse.
- **Embedded Route Compression**: Companion app now compresses coordinates for the latest 8 trips using a custom Base36 delta-compression scheme and embeds them inside the initial settings URL payload, keeping URL length well within Pebble limits.

## [2.3.0] - 2026-06-14

### Added
- **Real-Time Traveled Route Overlay (Breadcrumbs)**: Added real-time rendering of the user's traveled path (breadcrumb trail) on the watch map in a distinct Cobalt Blue color (3px thickness). Draw rendering takes place underneath the planned route line to preserve upcoming path legibility.
- **Show Traveled Route Settings Toggle**: Introduced a new settings toggle under the "Allgemein" section of the configuration page to show/hide the breadcrumb overlay dynamically. Includes full German/English localizations.
- **Graphics Rendering Optimizations**: Integrated a decimation filter to cap maximum drawn path points to 500 and a 50px viewport bounding box padding/clipping filter to keep map panning and zooming smooth.

### Fixed
- **Settings Page Closing on Action triggers**: Added a 350ms delay to all WebView re-open commands (viewing, deleting, downloading, and toggling recording) to allow the mobile OS to finish tearing down the closed WebView before requesting a new one. This prevents the settings screen from closing completely and returning the user to the app overview.

## [2.1.0] - 2026-06-14

### Added
- **Interactive Map View for Recorded Trips**: Added a "View" button to the recorded trips list in Settings, allowing users to view completed walks on an interactive Leaflet map overlay using their active map source style. Displays total distance, walking duration, calculated average speed, and elevation gain/loss.

## [2.0.0] - 2026-06-08

### Added
- **Fullscreen Map Mode**: Added a new settings configuration toggle to enable fullscreen map view. Hides header and footer overlays on the watch to maximize visible map area, dynamically adjusting layers and centering the compass chevron on the expanded viewport.

## [1.9.0] - 2026-06-07

### Removed
- **GPX File Selector**: Removed the unreliable file selection input and dropzone from the settings page. Copy-pasting the raw GPX XML content is now the sole route input method.

## [1.8.0] - 2026-06-07

### Fixed
- **Watch Route Load Failure**: Fixed a bug where activated routes from the watch menu failed to load ("No GPX route loaded" displayed on screen). This was caused by 64-bit JS timestamps (`Date.now()`) overflowing Pebble's 32-bit integer data types during transmission. Resolves the issue by performing bitwise 32-bit integer matching for route IDs on the companion side.

## [1.7.0] - 2026-06-07

### Fixed
- **Accidental Route Overwrite Prevention**: Selecting an inactive route now prompts the user with a confirmation window ("Start navigation?") or switch warning ("Switch route? Saves current trip.") instead of activating immediately. This prevents active workout recordings from being silently discarded.

## [1.6.0] - 2026-06-07

### Added
- **On-Watch Route Activation Confirmation**: Selecting an inactive route from the watch route menu now prompts the user with a confirmation window ("Start navigation?" / "Navi starten?") to prevent accidental starts.
- **Route Switch Safety Confirmation**: If a route navigation is already active and the user selects a different route, the watch displays a warning window ("Switch route? Saves current trip." / "Route wechseln? Speichert aktuelle.") to prevent overriding active tracks accidentally.
- **On-Watch Stop Confirmation Window**: Re-selecting the active route in the watch menu displays a confirmation dialog asking the user whether to stop navigation ("Stop navigation?" / "Navi stoppen?").
- **Auto-Save Walked Track on Stop/Switch**: Confirming the stop or switch deactivates the current route, stops recording, and automatically saves the walked track/trip to the phone's trips history before activating the new route.
- **Vibration Feedback**: Triggers vibes_short_pulse on start/switch and vibes_double_pulse on stop.
- **Menu Stack Popping on Confirm**: Automatically removes the route menu from the stack when confirming, returning the user directly to the main map screen.

## [1.5.0] - 2026-06-07

### Added
- **Dynamic Multilingual Support**: Live English/German translations across both PebbleKit JS companion app and watchapp (coordinates titles, dashboard fields, navigation text).
- **Dashboard Enhancements**: Dual-column responsive layout showing average speed, walked/remaining distance, and elevation profile in large bold fonts.
- **Sensor-Fused Direction Arrow**: Dynamic central compass arrow that aligns using the hardware magnetic compass when stationary and switches to GPS direction when in motion.
- **Offline GPX Downloads**: Settings page allows downloading walks recorded on the watch as valid `.gpx` files.
- **Douglas-Peucker Simplification**: Optimizes GPX paths in settings page to fit within watch memory limits.
- **Battery Status**: Live battery level percentage displayed in the top-right header.

### Fixed
- **App Launch Crashes**: Fixed battery-state and early AppMessage null-pointer crashes.

## [1.4.0] - 2026-06-07

### Fixed
- **GPS Connection Issues**: Encapsulates geolocation watchPosition in robust try-catch block and configures options (enabling high accuracy, 10s timeout, 10s maximum age) to prevent timeout failures.
- **LocalStorage robustness**: Wrapped JSON.parse for routes and trips in try-catch blocks to prevent startup crashes when storage gets corrupted.

## [1.3.0] - 2026-06-07

### Added
- **Free Map Source Selection**: Settings page now offers selection of free map sources without API keys (OpenTopoMap, HikeBikeMap, MtbMap, OpenStreetMap).
- **Persistent Dropdown Selection**: Settings page preserves the selected map source and interval after closing and returning.
