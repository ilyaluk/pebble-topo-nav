var png = require('./png');
var graphics = require('./graphics');

// State Variables
var CHUNK_SIZE = 3000;
var gpsInterval = 5;
var gpxTrack = [];
var currentLocation = null;
var currentZoom = 15;
var isSendingMap = false;
var gpsWatchId = null;

// Haptic feedback states
var lastVibratedTurnIdx = -1;
var hasVibratedOffRoute = false;

// Average speed & walked route state
var totalMovingDistance = 0;
var totalMovingTimeSec = 0;
var lastPositionTime = null;
var lastPositionCoords = null;
var closestTrackPointIdx = -1;

// Utility: Convert ArrayBuffer to Base64 string
function arrayBufferToBase64(buffer) {
  var binary = '';
  var bytes = new Uint8Array(buffer);
  var len = bytes.byteLength;
  for (var i = 0; i < len; i++) {
    binary += String.fromCharCode(bytes[i]);
  }
  return btoa(binary);
}

// Utility: Convert Base64 string to Uint8Array
function base64ToUint8Array(base64) {
  var binaryString = atob(base64);
  var len = binaryString.length;
  var bytes = new Uint8Array(len);
  for (var i = 0; i < len; i++) {
    bytes[i] = binaryString.charCodeAt(i);
  }
  return bytes;
}

// Haversine formula to compute distance in meters
function haversineDistance(lat1, lon1, lat2, lon2) {
  var R = 6371000; // Radius of the Earth in meters
  var dLat = ((lat2 - lat1) * Math.PI) / 180;
  var dLon = ((lon2 - lon1) * Math.PI) / 180;
  var a =
    Math.sin(dLat / 2) * Math.sin(dLat / 2) +
    Math.cos((lat1 * Math.PI) / 180) *
      Math.cos((lat2 * Math.PI) / 180) *
      Math.sin(dLon / 2) *
      Math.sin(dLon / 2);
  var c = 2 * Math.atan2(Math.sqrt(a), Math.sqrt(1 - a));
  return R * c;
}

// Calculate bearing between two points (0 to 360)
function getBearing(lat1, lon1, lat2, lon2) {
  var dLon = ((lon2 - lon1) * Math.PI) / 180;
  var lat1Rad = (lat1 * Math.PI) / 180;
  var lat2Rad = (lat2 * Math.PI) / 180;
  var y = Math.sin(dLon) * Math.cos(lat2Rad);
  var x =
    Math.cos(lat1Rad) * Math.sin(lat2Rad) -
    Math.sin(lat1Rad) * Math.cos(lat2Rad) * Math.cos(dLon);
  var brng = (Math.atan2(y, x) * 180) / Math.PI;
  return (brng + 360) % 360;
}

// Initialize the Pebble application
Pebble.addEventListener('ready', function() {
  console.log('TopoNav PebbleKit JS is ready!');
  
  // Load settings from LocalStorage
  var storedInterval = localStorage.getItem('gpsInterval');
  if (storedInterval) gpsInterval = parseInt(storedInterval);
  
  var storedTrack = localStorage.getItem('gpxTrack');
  if (storedTrack) {
    try {
      gpxTrack = JSON.parse(storedTrack);
      console.log('Loaded GPX track from storage: ' + gpxTrack.length + ' points.');
    } catch (e) {
      gpxTrack = [];
    }
  }

  // Start GPS tracking
  restartGPSTracking();
});

// Settings config page trigger
Pebble.addEventListener('showConfiguration', function() {
  // We use a query parameter cache-buster to prevent mobile WebViews from caching config.html
  Pebble.openURL('https://sirtob1.github.io/pebble-topo-nav/src/pkjs/config.html?v=' + Date.now());
});

// Settings config page closed
Pebble.addEventListener('webviewclosed', function(e) {
  if (e && e.response) {
    try {
      var settings = JSON.parse(decodeURIComponent(e.response));
      console.log('Received settings: ' + JSON.stringify(settings).substring(0, 100) + '...');
      
      gpsInterval = settings.gpsInterval;
      localStorage.setItem('gpsInterval', gpsInterval);
      
      gpxTrack = settings.gpxTrack || [];
      localStorage.setItem('gpxTrack', JSON.stringify(gpxTrack));
      
      // Reset navigation haptics states
      lastVibratedTurnIdx = -1;
      hasVibratedOffRoute = false;
      
      // Cache tiles in background
      if (gpxTrack.length > 0) {
        cacheTrackTiles(gpxTrack);
      }
      
      // Restart GPS tracking with new interval
      restartGPSTracking();
      
      // Force immediate watch update
      updateWatchNavigationAndMap();
    } catch (err) {
      console.log('Error parsing configuration response: ' + err);
    }
  }
});

// Watch messages listener (Up/Down Zoom, Map requests)
Pebble.addEventListener('appmessage', function(e) {
  var dict = e.payload;
  console.log('Received AppMessage from watch: ' + JSON.stringify(dict));
  
  if (dict.ZOOM_LEVEL !== undefined) {
    currentZoom = dict.ZOOM_LEVEL;
  }
  
  if (dict.REQUEST_MAP_UPDATE !== undefined) {
    updateWatchNavigationAndMap();
  }
});

// Restart GPS tracking with current interval
function restartGPSTracking() {
  if (gpsWatchId !== null) {
    navigator.geolocation.clearWatch(gpsWatchId);
  }
  
  var options = {
    enableHighAccuracy: true,
    maximumAge: 1000,
    timeout: 5000
  };
  
  gpsWatchId = navigator.geolocation.watchPosition(
    onGPSSuccess,
    onGPSError,
    options
  );
  
  console.log('GPS tracking started with interval: ' + gpsInterval + 's');
}

function onGPSSuccess(position) {
  var lat = position.coords.latitude;
  var lon = position.coords.longitude;
  var now = Date.now();
  
  if (lastPositionTime && lastPositionCoords) {
    var dt = (now - lastPositionTime) / 1000;
    if (dt > 0.5) {
      var ds = haversineDistance(
        lastPositionCoords.latitude,
        lastPositionCoords.longitude,
        lat,
        lon
      );
      
      var currentSpeed = position.coords.speed !== null && position.coords.speed !== undefined ? position.coords.speed : (ds / dt);
      
      if (currentSpeed > 0.5 && ds > 0.5) {
        totalMovingDistance += ds;
        totalMovingTimeSec += dt;
      }
    }
  }
  
  lastPositionTime = now;
  lastPositionCoords = { latitude: lat, longitude: lon };

  currentLocation = {
    lat: lat,
    lon: lon,
    speed: position.coords.speed || 0,
    altitude: position.coords.altitude || 0
  };
  
  updateWatchNavigationAndMap();
}

function onGPSError(err) {
  console.log('GPS Error: ' + err.message);
  
  // Notify watch of lost connection
  Pebble.sendAppMessage({
    GPS_CONNECTED: 0,
    NAV_INSTRUCTION: 'Kein GPS-Signal',
    NAV_DISTANCE: '---'
  });
}

// Perform calculations and send updates to watch
function updateWatchNavigationAndMap() {
  if (!currentLocation) {
    Pebble.sendAppMessage({
      GPS_CONNECTED: 0
    });
    return;
  }
  
  var avgSpeedKmh = totalMovingTimeSec > 0 ? (totalMovingDistance / totalMovingTimeSec) * 3.6 : 0;
  var payload = {
    GPS_CONNECTED: 1,
    AVG_SPEED: avgSpeedKmh.toFixed(1) + ' km/h'
  };

  var offRoute = false;
  var vibrateAlert = 0;

  if (gpxTrack.length > 0) {
    // 1. Find the closest point on the track
    var minDist = Infinity;
    var closestIdx = -1;
    
    for (var k = 0; k < gpxTrack.length; k++) {
      var d = haversineDistance(
        currentLocation.lat,
        currentLocation.lon,
        gpxTrack[k].lat,
        gpxTrack[k].lon
      );
      if (d < minDist) {
        minDist = d;
        closestIdx = k;
      }
    }
    
    // Save closest index for map rendering (gray out walked part)
    closestTrackPointIdx = closestIdx;
    
    // Calculate elevation stats based on GPX track elevations
    var gainMade = 0;
    var lossMade = 0;
    for (var i = 0; i < closestIdx; i++) {
      if (gpxTrack[i].ele !== undefined && gpxTrack[i+1].ele !== undefined) {
        var diff = gpxTrack[i+1].ele - gpxTrack[i].ele;
        if (diff > 0) gainMade += diff;
        else lossMade += Math.abs(diff);
      }
    }
    
    var gainRemaining = 0;
    var lossRemaining = 0;
    for (var i = closestIdx; i < gpxTrack.length - 1; i++) {
      if (gpxTrack[i].ele !== undefined && gpxTrack[i+1].ele !== undefined) {
        var diff = gpxTrack[i+1].ele - gpxTrack[i].ele;
        if (diff > 0) gainRemaining += diff;
        else lossRemaining += Math.abs(diff);
      }
    }
    
    payload.ELEVATION_GAIN = Math.round(gainMade) + 'm / ' + Math.round(gainRemaining) + 'm';
    payload.ELEVATION_LOSS = Math.round(lossMade) + 'm / ' + Math.round(lossRemaining) + 'm';
    
    // Check if user is Off-Route (> 50 meters)
    if (minDist > 50) {
      offRoute = true;
      payload.OFF_ROUTE = 1;
      payload.NAV_INSTRUCTION = 'ABSEITS DER ROUTE!';
      payload.NAV_DISTANCE = Math.round(minDist) + 'm';
      payload.NAV_BEARING = -1;
      
      // Trigger off-route vibration alert once
      if (!hasVibratedOffRoute) {
        vibrateAlert = 2; // Off-Route alert
        hasVibratedOffRoute = true;
      }
    } else {
      payload.OFF_ROUTE = 0;
      hasVibratedOffRoute = false; // Reset off-route vibration state once back on route
      
      // 2. Look ahead for significant turns
      var turnIdx = -1;
      var distToTurn = 0;
      var turnBearingDiff = 0;
      
      // Check next 20 trackpoints for bearing changes
      var lookAheadLimit = Math.min(closestIdx + 20, gpxTrack.length - 2);
      for (var idx = closestIdx; idx < lookAheadLimit; idx++) {
        var b1 = getBearing(gpxTrack[idx].lat, gpxTrack[idx].lon, gpxTrack[idx + 1].lat, gpxTrack[idx + 1].lon);
        var b2 = getBearing(gpxTrack[idx + 1].lat, gpxTrack[idx + 1].lon, gpxTrack[idx + 2].lat, gpxTrack[idx + 2].lon);
        
        var diff = (b2 - b1 + 180) % 360 - 180; // diff in [-180, 180]
        if (Math.abs(diff) > 30) {
          turnIdx = idx + 1;
          turnBearingDiff = diff;
          break;
        }
      }
      
      if (turnIdx !== -1) {
        distToTurn = haversineDistance(
          currentLocation.lat,
          currentLocation.lon,
          gpxTrack[turnIdx].lat,
          gpxTrack[turnIdx].lon
        );
        
        payload.NAV_DISTANCE = Math.round(distToTurn) + 'm';
        
        // Formulate instruction text
        var dirText = turnBearingDiff > 0 ? 'rechts abbiegen' : 'links abbiegen';
        payload.NAV_INSTRUCTION = 'In Kürze ' + dirText;
        
        // Map bearing to 0=straight, 90=right, 180=uturn, 270=left
        payload.NAV_BEARING = turnBearingDiff > 0 ? 90 : 270;
        
        // Trigger turn haptic vibration at ~50 meters (only once per turn)
        if (distToTurn <= 50 && lastVibratedTurnIdx !== turnIdx) {
          vibrateAlert = 1; // Turn alert
          lastVibratedTurnIdx = turnIdx;
        }
      } else {
        payload.NAV_INSTRUCTION = 'Folge dem Weg';
        payload.NAV_DISTANCE = '---';
        payload.NAV_BEARING = 0; // straight
      }
      
      // Calculate remaining trip distance
      var remDist = 0;
      for (var j = closestIdx; j < gpxTrack.length - 1; j++) {
        remDist += haversineDistance(gpxTrack[j].lat, gpxTrack[j].lon, gpxTrack[j + 1].lat, gpxTrack[j + 1].lon);
      }
      payload.TRIP_DISTANCE = (remDist / 1000).toFixed(1) + ' km';
    }
  } else {
    payload.NAV_INSTRUCTION = 'Keine GPX-Route geladen';
    payload.NAV_DISTANCE = '---';
    payload.TRIP_DISTANCE = '--- km';
    payload.ELEVATION_GAIN = '---m / ---m';
    payload.ELEVATION_LOSS = '---m / ---m';
    closestTrackPointIdx = -1;
  }
  
  if (vibrateAlert !== 0) {
    payload.VIBRATE_ALERT = vibrateAlert;
  }
  
  // Send status/nav values first
  Pebble.sendAppMessage(payload, function() {
    // Render and send the map image afterward
    renderAndSendMap();
  }, function(e) {
    console.log('AppMessage send failed: ' + JSON.stringify(e));
    // Still try to send the map
    renderAndSendMap();
  });
}

// Render viewport tiles and send map image in chunks
function renderAndSendMap() {
  if (isSendingMap || !currentLocation) return;
  isSendingMap = true;
  
  // 1. Identify which tiles are needed for the current viewport
  var centerPix = graphics.latLonToPixels(currentLocation.lat, currentLocation.lon, currentZoom);
  var tlX = centerPix.x - 100;
  var tlY = centerPix.y - 75;
  var tileXMin = Math.floor(tlX / 256);
  var tileXMax = Math.floor((tlX + 200) / 256);
  var tileYMin = Math.floor(tlY / 256);
  var tileYMax = Math.floor((tlY + 150) / 256);
  
  var tileCache = {};
  var tilesToFetch = [];
  
  for (var tx = tileXMin; tx <= tileXMax; tx++) {
    for (var ty = tileYMin; ty <= tileYMax; ty++) {
      var key = currentZoom + '/' + tx + '/' + ty;
      var cachedBase64 = localStorage.getItem('tile_' + key);
      
      if (cachedBase64) {
        try {
          var bytes = base64ToUint8Array(cachedBase64);
          var decoded = png.decodePNG(bytes);
          tileCache[key] = decoded;
        } catch (err) {
          console.log('Cached tile decode error (' + key + '): ' + err);
          tilesToFetch.push({ key: key, z: currentZoom, x: tx, y: ty });
        }
      } else {
        // Tile not cached, fetch online
        tilesToFetch.push({ key: key, z: currentZoom, x: tx, y: ty });
      }
    }
  }
  
  // 2. Fetch missing tiles if online
  if (tilesToFetch.length > 0) {
    var fetchedCount = 0;
    
    function checkCompleted() {
      fetchedCount++;
      if (fetchedCount === tilesToFetch.length) {
        doRenderAndChunkSend(tileCache);
      }
    }
    
    tilesToFetch.forEach(function(item) {
      var subdomains = ['a', 'b', 'c'];
      var sub = subdomains[Math.floor(Math.random() * 3)];
      var url = 'https://' + sub + '.tile.opentopomap.org/' + item.z + '/' + item.x + '/' + item.y + '.png';
      
      var xhr = new XMLHttpRequest();
      xhr.open('GET', url, true);
      xhr.responseType = 'arraybuffer';
      
      xhr.onload = function() {
        if (xhr.status === 200) {
          try {
            var bytes = new Uint8Array(xhr.response);
            var decoded = png.decodePNG(bytes);
            tileCache[item.key] = decoded;
            
            // Cache downloaded tile in localStorage
            var base64 = arrayBufferToBase64(xhr.response);
            localStorage.setItem('tile_' + item.key, base64);
          } catch (e) {
            console.log('Error decoding fetched tile ' + item.key + ': ' + e);
          }
        }
        checkCompleted();
      };
      xhr.onerror = function() {
        console.error('Failed to fetch tile: ' + item.key);
        checkCompleted();
      };
      xhr.send();
    });
  } else {
    doRenderAndChunkSend(tileCache);
  }
}

// Draw route overlays and trigger the AppMessage transmission loop
function doRenderAndChunkSend(tileCache) {
  try {
    var gcolor8Map = graphics.renderViewport(
      currentLocation.lat,
      currentLocation.lon,
      currentZoom,
      gpxTrack,
      tileCache,
      closestTrackPointIdx
    );
    
    // Chunked Transmission Loop
    var totalSize = gcolor8Map.length; // 30,000 bytes
    var totalChunks = Math.ceil(totalSize / CHUNK_SIZE); // 10 chunks
    
    function sendChunk(chunkIdx) {
      if (chunkIdx >= totalChunks) {
        console.log('Map image fully transmitted to watch!');
        isSendingMap = false;
        return;
      }
      
      var start = chunkIdx * CHUNK_SIZE;
      var end = Math.min(start + CHUNK_SIZE, totalSize);
      var chunkData = Array.prototype.slice.call(gcolor8Map.subarray(start, end));
      
      var payload = {
        MAP_DATA_CHUNK: chunkData,
        CHUNK_INDEX: chunkIdx,
        TOTAL_CHUNKS: totalChunks
      };
      
      var retries = 0;
      function transmit() {
        Pebble.sendAppMessage(payload, function() {
          // Success, send next chunk
          sendChunk(chunkIdx + 1);
        }, function(err) {
          console.warn('Chunk ' + chunkIdx + ' failed sending. Retrying...');
          retries++;
          if (retries < 3) {
            setTimeout(transmit, 150);
          } else {
            console.error('Failed transmitting map chunk ' + chunkIdx + ' after 3 retries.');
            isSendingMap = false;
          }
        });
      }
      transmit();
    }
    
    sendChunk(0);
  } catch (err) {
    console.error('Map rendering / transmission crashed: ' + err.stack);
    isSendingMap = false;
  }
}

// Background Tile Cacher along the GPX Track
function cacheTrackTiles(track) {
  var zoom = 15;
  var tileKeys = [];
  var seen = {};
  
  for (var i = 0; i < track.length; i++) {
    var pt = track[i];
    var tile = graphics.latLonToPixels(pt.lat, pt.lon, zoom);
    var tileX = Math.floor(tile.x / 256);
    var tileY = Math.floor(tile.y / 256);
    
    // Cache 3x3 block around track points
    for (var dx = -1; dx <= 1; dx++) {
      for (var dy = -1; dy <= 1; dy++) {
        var key = zoom + '/' + (tileX + dx) + '/' + (tileY + dy);
        if (!seen[key]) {
          seen[key] = true;
          tileKeys.push({ key: key, z: zoom, x: tileX + dx, y: tileY + dy });
        }
      }
    }
  }
  
  console.log('Background Tile Cache Scheduler: ' + tileKeys.length + ' tiles detected.');
  
  var idx = 0;
  function downloadNext() {
    if (idx >= tileKeys.length) {
      console.log('Offline tile caching fully completed!');
      return;
    }
    
    var item = tileKeys[idx];
    var storeKey = 'tile_' + item.key;
    
    if (localStorage.getItem(storeKey)) {
      idx++;
      downloadNext();
      return;
    }
    
    var subdomains = ['a', 'b', 'c'];
    var sub = subdomains[Math.floor(Math.random() * 3)];
    var url = 'https://' + sub + '.tile.opentopomap.org/' + item.z + '/' + item.x + '/' + item.y + '.png';
    
    var xhr = new XMLHttpRequest();
    xhr.open('GET', url, true);
    xhr.responseType = 'arraybuffer';
    
    xhr.onload = function() {
      if (xhr.status === 200) {
        try {
          var base64 = arrayBufferToBase64(xhr.response);
          localStorage.setItem(storeKey, base64);
        } catch (e) {
          console.warn('LocalStorage full, stopping offline caching.');
          return;
        }
      }
      idx++;
      setTimeout(downloadNext, 120); // rate limiting request
    };
    xhr.onerror = function() {
      idx++;
      setTimeout(downloadNext, 120);
    };
    xhr.send();
  }
  
  downloadNext();
}
