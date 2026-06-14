# Changelog

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
