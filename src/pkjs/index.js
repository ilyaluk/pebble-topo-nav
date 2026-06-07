var png = require('./png');
var graphics = require('./graphics');

// State Variables
var CHUNK_SIZE = 3000;
var gpsInterval = 5;
var gpxTrack = [];
var currentLocation = null;
var currentZoom = 17;
var isSendingMap = false;
var gpsWatchId = null;

// Navigation & Recording State
var isNavigating = false;
var recordedTrack = [];
var currentSpeed = 0;
var currentHeading = -1;

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

  // Load walked stats from LocalStorage to allow seamless resumption
  var storedMovingDist = localStorage.getItem('totalMovingDistance');
  if (storedMovingDist) totalMovingDistance = parseFloat(storedMovingDist);
  
  var storedMovingTime = localStorage.getItem('totalMovingTimeSec');
  if (storedMovingTime) totalMovingTimeSec = parseFloat(storedMovingTime);
  
  var storedClosestIdx = localStorage.getItem('closestTrackPointIdx');
  if (storedClosestIdx) closestTrackPointIdx = parseInt(storedClosestIdx);

  isNavigating = localStorage.getItem('isNavigating') === 'true';
  var storedRecordedTrack = localStorage.getItem('recordedTrack');
  if (storedRecordedTrack) {
    try {
      recordedTrack = JSON.parse(storedRecordedTrack);
      console.log('Loaded recorded track: ' + recordedTrack.length + ' points.');
    } catch (e) {
      recordedTrack = [];
    }
  }

  // Load active route points if set
  var activeRouteIdStr = localStorage.getItem('activeRouteId');
  if (activeRouteIdStr) {
    var activeId = parseInt(activeRouteIdStr, 10);
    var savedRoutes = JSON.parse(localStorage.getItem('savedRoutes') || '[]');
    var activeRoute = savedRoutes.filter(function(r) { return r.id === activeId; })[0];
    if (activeRoute) {
      gpxTrack = activeRoute.points || [];
      console.log('Loaded active route: ' + activeRoute.name + ' (' + gpxTrack.length + ' points).');
    } else {
      gpxTrack = [];
    }
  } else {
    // Legacy fallback
    var storedTrack = localStorage.getItem('gpxTrack');
    if (storedTrack) {
      try {
        gpxTrack = JSON.parse(storedTrack);
        console.log('Loaded legacy GPX track: ' + gpxTrack.length + ' points.');
      } catch (e) {
        gpxTrack = [];
      }
    }
  }

  // Start GPS tracking
  restartGPSTracking();

  // Sync routes to watch on startup
  setTimeout(syncRoutesToWatch, 1000);
});

// Helper to open the configuration url with all required parameters
function openConfigPage(downloadGpxData, downloadName) {
  var interval = localStorage.getItem('gpsInterval') || '5';
  var lang = localStorage.getItem('language') || 'de';
  var savedTrips = JSON.parse(localStorage.getItem('savedTrips') || '[]');
  
  // Serialize only metadata to stay within query param limits
  var tripsMeta = savedTrips.map(function(t) {
    return {
      id: t.id,
      date: t.date,
      distance: t.distance,
      duration: t.duration,
      pointsCount: t.points ? t.points.length : 0
    };
  });

  // Serialize saved routes list metadata
  var savedRoutes = JSON.parse(localStorage.getItem('savedRoutes') || '[]');
  var routesMeta = savedRoutes.map(function(r) {
    var dist = 0;
    if (r.points) {
      for (var k = 0; k < r.points.length - 1; k++) {
        dist += haversineDistance(r.points[k].lat, r.points[k].lon, r.points[k+1].lat, r.points[k+1].lon);
      }
    }
    return {
      id: r.id,
      name: r.name,
      distance: dist,
      pointsCount: r.points ? r.points.length : 0
    };
  });
  
  var url = 'https://sirtob1.github.io/pebble-topo-nav/src/pkjs/config.html?v=' + Date.now() + 
            '&interval=' + interval + 
            '&lang=' + lang + 
            '&is_nav=' + (isNavigating ? 'true' : 'false') + 
            '&trips=' + encodeURIComponent(JSON.stringify(tripsMeta)) +
            '&routes=' + encodeURIComponent(JSON.stringify(routesMeta)) +
            '&active_route_id=' + (localStorage.getItem('activeRouteId') || '0');
            
  if (downloadGpxData && downloadName) {
    url += '&download_gpx=' + encodeURIComponent(downloadGpxData) + 
           '&download_name=' + encodeURIComponent(downloadName);
  }
  
  console.log('Opening config page with url: ' + url.substring(0, 150) + '...');
  Pebble.openURL(url);
}

// Sync routes list to watch
function syncRoutesToWatch() {
  var savedRoutes = JSON.parse(localStorage.getItem('savedRoutes') || '[]');
  var activeRouteId = parseInt(localStorage.getItem('activeRouteId') || '0', 10);
  
  console.log('Syncing ' + savedRoutes.length + ' routes to watch. Active ID: ' + activeRouteId);
  
  Pebble.sendAppMessage({
    ACTIVE_ROUTE_ID: activeRouteId
  }, function() {
    Pebble.sendAppMessage({
      ROUTE_COUNT: savedRoutes.length
    }, function() {
      var idx = 0;
      function sendNext() {
        if (idx >= savedRoutes.length || idx >= 15) {
          console.log('Route list fully synced to watch.');
          return;
        }
        var route = savedRoutes[idx];
        Pebble.sendAppMessage({
          ROUTE_INDEX: idx,
          ROUTE_ID: route.id,
          ROUTE_NAME: route.name.substring(0, 31)
        }, function() {
          idx++;
          setTimeout(sendNext, 100);
        }, function(err) {
          console.warn('Failed to sync route index ' + idx + ', retrying...');
          setTimeout(sendNext, 250);
        });
      }
      if (savedRoutes.length > 0) {
        sendNext();
      }
    });
  });
}

function activateSavedRoute(routeId) {
  var savedRoutes = JSON.parse(localStorage.getItem('savedRoutes') || '[]');
  var foundRoute = savedRoutes.filter(function(r) { return r.id === routeId; })[0];
  
  if (foundRoute) {
    localStorage.setItem('activeRouteId', routeId.toString());
    gpxTrack = foundRoute.points || [];
    localStorage.setItem('gpxTrack', JSON.stringify(gpxTrack));
    console.log('Activated route: ' + foundRoute.name);
  } else if (routeId === 0) {
    localStorage.setItem('activeRouteId', '0');
    gpxTrack = [];
    localStorage.setItem('gpxTrack', JSON.stringify(gpxTrack));
    console.log('Deactivated current route.');
  }

  // Reset navigation stats for the new route
  totalMovingDistance = 0;
  totalMovingTimeSec = 0;
  localStorage.removeItem('totalMovingDistance');
  localStorage.removeItem('totalMovingTimeSec');
  closestTrackPointIdx = -1;
  localStorage.removeItem('closestTrackPointIdx');
  lastPositionTime = null;
  lastPositionCoords = null;
  lastVibratedTurnIdx = -1;
  hasVibratedOffRoute = false;
  
  if (gpxTrack.length > 0) {
    cacheTrackTiles(gpxTrack);
  }
  
  syncRoutesToWatch();
  updateWatchNavigationAndMap();
}

function deleteSavedRoute(routeId) {
  var savedRoutes = JSON.parse(localStorage.getItem('savedRoutes') || '[]');
  var updated = savedRoutes.filter(function(r) {
    return r.id !== routeId;
  });
  localStorage.setItem('savedRoutes', JSON.stringify(updated));
  console.log('Deleted route ID: ' + routeId);
  
  var activeId = parseInt(localStorage.getItem('activeRouteId') || '0', 10);
  if (activeId === routeId) {
    activateSavedRoute(0);
  } else {
    syncRoutesToWatch();
  }
}

// Coordinate delta compression for URL transmission
function compressTrack(points) {
  if (!points || points.length === 0) return '';
  var segments = [];
  var lastLat = 0;
  var lastLon = 0;
  var lastEle = 0;
  var lastTime = 0;
  
  for (var i = 0; i < points.length; i++) {
    var p = points[i];
    var latVal = Math.round(p.lat * 1000000);
    var lonVal = Math.round(p.lon * 1000000);
    var eleVal = Math.round(p.ele || 0);
    var timeVal = Math.round(p.time || 0);
    
    if (i === 0) {
      segments.push(latVal + ',' + lonVal + ',' + eleVal + ',' + timeVal);
    } else {
      var dLat = latVal - lastLat;
      var dLon = lonVal - lastLon;
      var dEle = eleVal - lastEle;
      var dTime = timeVal - lastTime;
      segments.push(dLat + ',' + dLon + ',' + dEle + ',' + dTime);
    }
    
    lastLat = latVal;
    lastLon = lonVal;
    lastEle = eleVal;
    lastTime = timeVal;
  }
  return segments.join('|');
}

function triggerGpxDownload(tripId) {
  var savedTrips = JSON.parse(localStorage.getItem('savedTrips') || '[]');
  var trip = null;
  for (var i = 0; i < savedTrips.length; i++) {
    if (savedTrips[i].id === tripId) {
      trip = savedTrips[i];
      break;
    }
  }
  if (trip) {
    var compressed = compressTrack(trip.points);
    var tripDateStr = new Date(trip.date).toISOString().replace(/:/g, '-').split('.')[0];
    var downloadName = 'route_' + tripDateStr;
    openConfigPage(compressed, downloadName);
  } else {
    openConfigPage();
  }
}

function deleteSavedTrip(tripId) {
  var savedTrips = JSON.parse(localStorage.getItem('savedTrips') || '[]');
  var updated = savedTrips.filter(function(t) {
    return t.id !== tripId;
  });
  localStorage.setItem('savedTrips', JSON.stringify(updated));
}

function toggleRecordingState() {
  isNavigating = !isNavigating;
  localStorage.setItem('isNavigating', isNavigating ? 'true' : 'false');
  
  if (isNavigating) {
    recordedTrack = [];
    localStorage.setItem('recordedTrack', JSON.stringify(recordedTrack));
    
    totalMovingDistance = 0;
    totalMovingTimeSec = 0;
    localStorage.setItem('totalMovingDistance', 0);
    localStorage.setItem('totalMovingTimeSec', 0);
    
    closestTrackPointIdx = -1;
    localStorage.setItem('closestTrackPointIdx', -1);
    lastPositionTime = null;
    lastPositionCoords = null;
    
    // Confirm start with short vibe
    Pebble.sendAppMessage({
      RECORDING_STATE: 1,
      VIBRATE_ALERT: 1
    });
  } else {
    if (recordedTrack.length > 0) {
      var savedTrips = JSON.parse(localStorage.getItem('savedTrips') || '[]');
      var newTrip = {
        id: Date.now(),
        date: new Date().toISOString(),
        distance: totalMovingDistance,
        duration: totalMovingTimeSec,
        points: recordedTrack
      };
      savedTrips.push(newTrip);
      localStorage.setItem('savedTrips', JSON.stringify(savedTrips));
      console.log('Saved new trip with ' + recordedTrack.length + ' points.');
    }
    
    recordedTrack = [];
    localStorage.setItem('recordedTrack', JSON.stringify(recordedTrack));
    
    // Confirm stop with double vibe
    Pebble.sendAppMessage({
      RECORDING_STATE: 0,
      VIBRATE_ALERT: 2
    });
  }
  
  updateWatchNavigationAndMap();
}

// Settings config page trigger
Pebble.addEventListener('showConfiguration', function() {
  openConfigPage();
});

// Settings config page closed
Pebble.addEventListener('webviewclosed', function(e) {
  if (e && e.response) {
    try {
      var responseStr = decodeURIComponent(e.response);
      console.log('WebView closed response: ' + responseStr);
      
      if (responseStr === 'toggle_nav') {
        toggleRecordingState();
        openConfigPage();
        return;
      }
      
      if (responseStr.indexOf('delete_') === 0 && responseStr.indexOf('delete_route_') === -1) {
        var deleteId = parseInt(responseStr.substring(7), 10);
        deleteSavedTrip(deleteId);
        openConfigPage();
        return;
      }
      
      if (responseStr.indexOf('download_') === 0) {
        var downloadId = parseInt(responseStr.substring(9), 10);
        triggerGpxDownload(downloadId);
        return;
      }
      
      if (responseStr.indexOf('delete_route_') === 0) {
        var delRouteId = parseInt(responseStr.substring(13), 10);
        deleteSavedRoute(delRouteId);
        openConfigPage();
        return;
      }
      
      if (responseStr.indexOf('activate_route_') === 0) {
        var actRouteId = parseInt(responseStr.substring(15), 10);
        activateSavedRoute(actRouteId);
        openConfigPage();
        return;
      }
      
      var settings = JSON.parse(responseStr);
      console.log('Received settings: ' + JSON.stringify(settings).substring(0, 100) + '...');
      
      gpsInterval = settings.gpsInterval;
      localStorage.setItem('gpsInterval', gpsInterval);
      
      var lang = settings.language || 'de';
      localStorage.setItem('language', lang);
      
      if (settings.newRoute) {
        var savedRoutes = JSON.parse(localStorage.getItem('savedRoutes') || '[]');
        var newR = {
          id: Date.now(),
          name: settings.newRoute.name || ('Route ' + (savedRoutes.length + 1)),
          points: settings.newRoute.points
        };
        savedRoutes.push(newR);
        localStorage.setItem('savedRoutes', JSON.stringify(savedRoutes));
        console.log('Added new route: ' + newR.name);
        
        // Auto-activate the newly added route
        activateSavedRoute(newR.id);
      } else {
        // Just sync routes to verify in case config changed
        syncRoutesToWatch();
      }
      
      if (settings.gpxTrack !== undefined) {
        // Legacy upload compatibility (clean up when done)
        gpxTrack = settings.gpxTrack;
        localStorage.setItem('gpxTrack', JSON.stringify(gpxTrack));
        if (gpxTrack.length > 0) {
          var savedRoutes = JSON.parse(localStorage.getItem('savedRoutes') || '[]');
          var autoId = Date.now();
          var newR = {
            id: autoId,
            name: 'Import ' + new Date().toLocaleDateString(),
            points: gpxTrack
          };
          savedRoutes.push(newR);
          localStorage.setItem('savedRoutes', JSON.stringify(savedRoutes));
          localStorage.setItem('activeRouteId', autoId.toString());
          syncRoutesToWatch();
          cacheTrackTiles(gpxTrack);
        } else {
          activateSavedRoute(0);
        }
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

// Watch messages listener (Up/Down Zoom, Map requests, Recording control)
Pebble.addEventListener('appmessage', function(e) {
  var dict = e.payload;
  console.log('Received AppMessage from watch: ' + JSON.stringify(dict));
  
  if (dict.ZOOM_LEVEL !== undefined) {
    currentZoom = dict.ZOOM_LEVEL;
  }
  
  if (dict.REQUEST_MAP_UPDATE !== undefined) {
    updateWatchNavigationAndMap();
  }
  
  if (dict.RECORDING_STATE !== undefined) {
    toggleRecordingState();
  }
  
  if (dict.ROUTE_ID !== undefined) {
    activateSavedRoute(dict.ROUTE_ID);
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
  
  currentSpeed = position.coords.speed !== null && position.coords.speed !== undefined ? position.coords.speed : 0;
  currentHeading = position.coords.heading !== null && position.coords.heading !== undefined ? position.coords.heading : -1;
  
  if (lastPositionTime && lastPositionCoords) {
    var dt = (now - lastPositionTime) / 1000;
    if (dt > 0.5) {
      var ds = haversineDistance(
        lastPositionCoords.latitude,
        lastPositionCoords.longitude,
        lat,
        lon
      );
      
      var calculatedSpeed = ds / dt;
      if (position.coords.speed === null || position.coords.speed === undefined) {
        currentSpeed = calculatedSpeed;
      }
      
      if (currentHeading === -1 && calculatedSpeed > 0.8 && ds > 0.8) {
        currentHeading = getBearing(lastPositionCoords.latitude, lastPositionCoords.longitude, lat, lon);
      }
      
      if (currentSpeed > 0.5 && ds > 0.5) {
        if (isNavigating) {
          totalMovingDistance += ds;
          totalMovingTimeSec += dt;
          localStorage.setItem('totalMovingDistance', totalMovingDistance);
          localStorage.setItem('totalMovingTimeSec', totalMovingTimeSec);
        }
      }
    }
  }
  
  lastPositionTime = now;
  lastPositionCoords = { latitude: lat, longitude: lon };

  currentLocation = {
    lat: lat,
    lon: lon,
    speed: currentSpeed,
    altitude: position.coords.altitude || 0
  };
  
  // Record coordinates walked during navigation/tracking (10m threshold)
  if (isNavigating) {
    var lastPt = recordedTrack[recordedTrack.length - 1];
    var shouldAdd = false;
    if (!lastPt) {
      shouldAdd = true;
    } else {
      var d = haversineDistance(lastPt.lat, lastPt.lon, lat, lon);
      if (d >= 10) {
        shouldAdd = true;
      }
    }
    if (shouldAdd) {
      recordedTrack.push({
        lat: lat,
        lon: lon,
        ele: position.coords.altitude || 0,
        time: now
      });
      localStorage.setItem('recordedTrack', JSON.stringify(recordedTrack));
    }
  }
  
  updateWatchNavigationAndMap();
}

function onGPSError(err) {
  console.log('GPS Error: ' + err.message);
  
  var isEnglish = localStorage.getItem('language') === 'en';
  // Notify watch of lost connection
  Pebble.sendAppMessage({
    GPS_CONNECTED: 0,
    NAV_INSTRUCTION: isEnglish ? 'No GPS Signal' : 'Kein GPS-Signal',
    NAV_DISTANCE: '---',
    LANGUAGE: isEnglish ? 1 : 0
  });
}

// Perform calculations and send updates to watch
function updateWatchNavigationAndMap() {
  var isEnglish = localStorage.getItem('language') === 'en';
  var activeRouteId = parseInt(localStorage.getItem('activeRouteId') || '0', 10);
  
  if (!currentLocation) {
    Pebble.sendAppMessage({
      GPS_CONNECTED: 0,
      LANGUAGE: isEnglish ? 1 : 0,
      RECORDING_STATE: isNavigating ? 1 : 0,
      ACTIVE_ROUTE_ID: activeRouteId
    });
    return;
  }
  
  var avgSpeedKmh = totalMovingTimeSec > 0 ? (totalMovingDistance / totalMovingTimeSec) * 3.6 : 0;
  var payload = {
    GPS_CONNECTED: 1,
    AVG_SPEED: avgSpeedKmh.toFixed(1) + ' km/h',
    GPS_COORDS: currentLocation.lat.toFixed(5) + ', ' + currentLocation.lon.toFixed(5),
    LANGUAGE: isEnglish ? 1 : 0,
    RECORDING_STATE: isNavigating ? 1 : 0,
    GPS_SPEED: Math.round(currentSpeed * 100),
    GPS_HEADING: Math.round(currentHeading),
    ACTIVE_ROUTE_ID: activeRouteId
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
    localStorage.setItem('closestTrackPointIdx', closestTrackPointIdx);
    
    // Calculate walked and remaining distance along the track
    var walkedDist = 0;
    for (var i = 0; i < closestIdx; i++) {
      walkedDist += haversineDistance(gpxTrack[i].lat, gpxTrack[i].lon, gpxTrack[i + 1].lat, gpxTrack[i + 1].lon);
    }
    
    var remDist = 0;
    for (var j = closestIdx; j < gpxTrack.length - 1; j++) {
      remDist += haversineDistance(gpxTrack[j].lat, gpxTrack[j].lon, gpxTrack[j + 1].lat, gpxTrack[j + 1].lon);
    }
    payload.TRIP_DISTANCE = (walkedDist / 1000).toFixed(1) + ' / ' + (remDist / 1000).toFixed(1);
    
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
      payload.NAV_INSTRUCTION = isEnglish ? 'OFF ROUTE!' : 'ABSEITS DER ROUTE!';
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
        var dirText = '';
        if (isEnglish) {
          dirText = turnBearingDiff > 0 ? 'turn right' : 'turn left';
          payload.NAV_INSTRUCTION = dirText.charAt(0).toUpperCase() + dirText.slice(1) + ' soon';
        } else {
          dirText = turnBearingDiff > 0 ? 'rechts abbiegen' : 'links abbiegen';
          payload.NAV_INSTRUCTION = 'In Kürze ' + dirText;
        }
        
        // Map bearing to 0=straight, 90=right, 180=uturn, 270=left
        payload.NAV_BEARING = turnBearingDiff > 0 ? 90 : 270;
        
        // Trigger turn haptic vibration at ~50 meters (only once per turn)
        if (distToTurn <= 50 && lastVibratedTurnIdx !== turnIdx) {
          vibrateAlert = 1; // Turn alert
          lastVibratedTurnIdx = turnIdx;
        }
      } else {
        payload.NAV_INSTRUCTION = isEnglish ? 'Follow the path' : 'Folge dem Weg';
        payload.NAV_DISTANCE = '---';
        payload.NAV_BEARING = 0; // straight
      }
    }
  } else {
    payload.NAV_INSTRUCTION = isEnglish ? 'No GPX route loaded' : 'Keine GPX-Route geladen';
    payload.NAV_DISTANCE = '---';
    payload.TRIP_DISTANCE = '--- / ---';
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
