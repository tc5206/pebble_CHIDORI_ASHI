var Clay = require('@rebble/clay');
var clayConfig = require('./config');
var clay = new Clay(clayConfig);

var currentItineraries = [];
var currentItinIndex = 0;
var currentLocation = null;

/*
 * ============================================================
 * config.json
 * ============================================================
 */
var destinationStationId = 'scrape-jreast-keihin-tohoku:odpt.Station:JR-East.KeihinTohokuNegishi.Tokyo';
var transitApiUrl = 'https://api.transit.ls8h.com';
var arrivalSearchTime = '25:05';
var alarmMinutesBefore = 30;

/*
 * ============================================================
 * Search settings
 * ============================================================
 */
var SEARCH_RADIUS_METERS = 2000;
var REVERSE_RADIUS_METERS = 500;
var GRID_SIZE = 7;

/*
 * ============================================================
 * String Truncation for AppMessage (libpebble2 UnicodeDecodeError fix)
 * ============================================================
 */
function truncateForPebble(str) {
  if (!str) return "";
  var maxBytes = 32;
  var result = "";
  var bytes = 0;
  for (var i = 0; i < str.length; i++) {
    var code = str.charCodeAt(i);
    var charBytes = (code <= 0x7f) ? 1 : (code <= 0x7ff) ? 2 : 3;
    if (code >= 0xD800 && code <= 0xDBFF) { charBytes = 4; }
    if (bytes + charBytes > maxBytes) break;
    result += str.charAt(i);
    bytes += charBytes;
    if (charBytes === 4) i++;
  }
  return result;
}

/*
 * ============================================================
 * Pebble events
 * ============================================================
 */
Pebble.addEventListener('ready', function(e) {
  console.log("Pebble event: ready");
  fetchTransitData();
});

Pebble.addEventListener('webviewclosed', function(e) {
  console.log("Pebble event: webviewclosed");
  currentLocation = null;
  fetchTransitData();
});

Pebble.addEventListener('appmessage', function(e) {
  var payload = e.payload;
  console.log("Pebble event: appmessage received");

  var nextKey = (typeof messageKeys !== 'undefined') ? messageKeys.KEY_REQUEST_NEXT : 'KEY_REQUEST_NEXT';
  var prevKey = (typeof messageKeys !== 'undefined') ? messageKeys.KEY_REQUEST_PREV : 'KEY_REQUEST_PREV';
  var switchKey = (typeof messageKeys !== 'undefined') ? messageKeys.KEY_REQUEST_SWITCH : 'KEY_REQUEST_SWITCH';
  var timelineKey = (typeof messageKeys !== 'undefined') ? messageKeys.KEY_REQUEST_ADD_TIMELINE : 'KEY_REQUEST_ADD_TIMELINE';

  if (payload[switchKey] !== undefined || payload.KEY_REQUEST_SWITCH !== undefined) {
    console.log("Action: Switch/Refresh requested");
    currentLocation = null;
    currentItineraries = [];
    currentItinIndex = 0;
    fetchTransitData();
    return;
  }

  if (payload[nextKey] !== undefined || payload.KEY_REQUEST_NEXT !== undefined) {
    console.log("Action: Next itinerary requested");
    if (currentItineraries.length > 0) {
      currentItinIndex = (currentItinIndex + 1) % currentItineraries.length;
      sendItineraryToPebble(currentItineraries[currentItinIndex]);
    }
    return;
  }

  if (payload[prevKey] !== undefined || payload.KEY_REQUEST_PREV !== undefined) {
    console.log("Action: Prev itinerary requested");
    if (currentItineraries.length > 0) {
      currentItinIndex = (currentItinIndex - 1 + currentItineraries.length) % currentItineraries.length;
      sendItineraryToPebble(currentItineraries[currentItinIndex]);
    }
    return;
  }

  var timelineAction = payload[timelineKey] !== undefined ? parseInt(payload[timelineKey], 10) : -1;

  if (timelineAction === 2) {
    console.log("Action: Add alarm timeline requested");
    var alarmTimeKey = (typeof messageKeys !== 'undefined') ? messageKeys.KEY_HOUR : 'KEY_HOUR';
    var alarmUnixTime = payload[alarmTimeKey] !== undefined ? payload[alarmTimeKey] : payload.KEY_HOUR;
    addAlarmToTimeline(alarmUnixTime);
    return;
  }

  if (timelineAction === 3) {
    console.log("Action: Remove alarm timeline requested");
    removeAlarmFromTimeline();
    return;
  }

  if (timelineAction === 1 || payload.KEY_REQUEST_ADD_TIMELINE === 1) {
    console.log("Action: Add timeline requested");
    addCurrentTrainToTimeline();
    return;
  }
});

/*
 * ============================================================
 * Main search
 * ============================================================
 */
function fetchTransitData() {
  console.log("fetchTransitData: Started");
  var settings = JSON.parse(localStorage.getItem('clay-settings') || '{}');
	transitApiUrl = settings.TRANSIT_API_URL || 'https://api.transit.ls8h.com';
  destinationStationId = settings.DESTINATION_STATION_ID || 'scrape-jreast-keihin-tohoku:odpt.Station:JR-East.KeihinTohokuNegishi.Tokyo';
  arrivalSearchTime = settings.ARRIVAL_SEARCH_TIME || '25:05';
  var configuredAlarmMinutes = parseInt( settings.ALARM_MINUTES_BEFORE, 10);
  if (isFinite(configuredAlarmMinutes) && configuredAlarmMinutes >= 0) {
		alarmMinutesBefore = configuredAlarmMinutes;
  } else {
    alarmMinutesBefore = 30;
  }
	
  console.log("fetchTransitData: Configuration loaded");
  if (!currentLocation) {
		console.log("fetchTransitData: Requesting GPS position");
		
		navigator.geolocation.getCurrentPosition(
      function(pos) {
        currentLocation = pos.coords;
        console.log("GPS Success: Lat=" + pos.coords.latitude + ", Lon=" + pos.coords.longitude);
        findNearestRailStation( transitApiUrl );
      },
      function(err) {
        console.log("GPS Error code=" + err.code);
        currentItineraries = [];
        currentItinIndex = 0;
        sendDataToPebble({ station: "GPS Error", hour: -1, min: 0 });
      },
      { timeout: 15000, maximumAge: 60000 }
    );
  } else {
    console.log("fetchTransitData: Using cached GPS position");

    findNearestRailStation( transitApiUrl );
  }
}

/*
 * ============================================================
 * Distance calculation
 * ============================================================
 */
function getDistanceMeters(lat1, lon1, lat2, lon2) {
  var R = 6371000;
  var radLat1 = lat1 * Math.PI / 180;
  var radLat2 = lat2 * Math.PI / 180;
  var dLat = (lat2 - lat1) * Math.PI / 180;
  var dLon = (lon2 - lon1) * Math.PI / 180;
  var a = Math.sin(dLat / 2) * Math.sin(dLat / 2) + Math.cos(radLat1) * Math.cos(radLat2) * Math.sin(dLon / 2) * Math.sin(dLon / 2);
  var c = 2 * Math.atan2(Math.sqrt(a), Math.sqrt(1 - a));
  return R * c;
}

/*
 * ============================================================
 * Find nearest railway station
 * ============================================================
 */
function findNearestRailStation(baseUrl) {
  var originLat = parseFloat(currentLocation.latitude);
  var originLon = parseFloat(currentLocation.longitude);

  if (!isFinite(originLat) || !isFinite(originLon)) {
    console.log("findNearestRailStation: Invalid GPS");
    sendDataToPebble({ station: "GPS Error", hour: -1, min: 0 });
    return;
  }

  console.log("Station search origin: " + originLat + "," + originLon);
  var step = (SEARCH_RADIUS_METERS * 2) / (GRID_SIZE - 1);
  var probes = [];

  for (var y = 0; y < GRID_SIZE; y++) {
    var northMeters = -SEARCH_RADIUS_METERS + step * y;
    for (var x = 0; x < GRID_SIZE; x++) {
      var eastMeters = -SEARCH_RADIUS_METERS + step * x;
      var lat = originLat + northMeters / 111320;
      var lon = originLon + eastMeters / (111320 * Math.cos(originLat * Math.PI / 180));
      probes.push({ lat: lat, lon: lon });
    }
  }

  console.log("Station search requests=" + probes.length);
  var candidates = [];

  function processBatch(startIndex) {
    if (startIndex >= probes.length) {
      selectNearestRailStation(candidates, baseUrl, originLat, originLon);
      return;
    }

    var endIndex = Math.min(startIndex + GRID_SIZE, probes.length);
    var pending = endIndex - startIndex;

    for (var i = startIndex; i < endIndex; i++) {
      (function(probe) {
        requestReversePlaces(baseUrl, probe.lat, probe.lon, function(places) {
          if (places && places.length > 0) {
            for (var j = 0; j < places.length; j++) {
              var place = places[j];
              if (place.kind !== "station" || !place.name) continue;

              var stationLat = parseFloat(place.lat);
              var stationLon = parseFloat(place.lon);

              if (!isFinite(stationLat) || !isFinite(stationLon)) continue;

              var distance = getDistanceMeters(originLat, originLon, stationLat, stationLon);
              if (distance > SEARCH_RADIUS_METERS) continue;

              candidates.push({
                reverseId: place.id || "",
                name: place.name,
                lat: stationLat,
                lon: stationLon,
                distance: distance
              });
            }
          }
          pending--;
          if (pending === 0) {
            processBatch(endIndex);
          }
        });
      })(probes[i]);
    }
  }

  processBatch(0);
}

/*
 * ============================================================
 * places/reverse
 * ============================================================
 */
function requestReversePlaces(baseUrl, lat, lon, callback) {
  var url = baseUrl + "/api/v1/places/reverse?lat=" + encodeURIComponent(lat) + "&lon=" + encodeURIComponent(lon) + "&limit=10&radiusMeters=" + REVERSE_RADIUS_METERS;
  var req = new XMLHttpRequest();
  req.open('GET', url, true);
  req.onload = function() {
    if (req.status !== 200) {
      callback([]);
      return;
    }
    try {
      var res = JSON.parse(req.responseText);
      var places = res.places || res.items || [];
      callback(Array.isArray(places) ? places : []);
    } catch (e) {
      callback([]);
    }
  };
  req.onerror = function() { callback([]); };
  req.send();
}

/*
 * ============================================================
 * Select nearest physical railway station
 * ============================================================
 */
function selectNearestRailStation(candidates, baseUrl, originLat, originLon) {
  console.log("Rail station candidates=" + candidates.length);

  if (!candidates || candidates.length === 0) {
    console.log("No railway station within 2km");
    sendDataToPebble({ station: "No Rail Station", hour: -1, min: 0 });
    return;
  }

  var unique = {};
  for (var i = 0; i < candidates.length; i++) {
    var candidate = candidates[i];
    var foundKey = null;

    for (var key in unique) {
      if (!unique.hasOwnProperty(key)) continue;
      var existing = unique[key];
      if (existing.name === candidate.name) {
        var stationDistance = getDistanceMeters(existing.lat, existing.lon, candidate.lat, candidate.lon);
        if (stationDistance < 100) {
          foundKey = key;
          break;
        }
      }
    }

    if (foundKey === null) {
      var newKey = candidate.name + "|" + candidate.lat.toFixed(5) + "|" + candidate.lon.toFixed(5);
      unique[newKey] = candidate;
    } else {
      if (candidate.distance < unique[foundKey].distance) {
        unique[foundKey] = candidate;
      }
    }
  }

  var nearest = null;
  for (var id in unique) {
    if (!unique.hasOwnProperty(id)) continue;
    var station = unique[id];
    if (!nearest || station.distance < nearest.distance) {
      nearest = station;
    }
  }

  if (!nearest) {
    sendDataToPebble({ station: "No Rail Station", hour: -1, min: 0 });
    return;
  }

  console.log("Selected station name=" + nearest.name);
  console.log("Selected station distance=" + Math.round(nearest.distance) + "m");

  resolveStationIds(baseUrl, nearest, function(stationIds) {
    if (!stationIds || stationIds.length === 0) {
      console.log("No station IDs resolved for " + nearest.name);
      sendDataToPebble({ station: "No Rail Station", hour: -1, min: 0 });
      return;
    }

    console.log("Resolved station IDs=" + stationIds.length);
    queryStationPlans(baseUrl, stationIds, 0, []);
  });
}

/*
 * ============================================================
 * Resolve actual station IDs
 * ============================================================
 */
function resolveStationIds(baseUrl, nearest, callback) {
  var url = baseUrl + "/api/v1/locations/suggest?q=" + encodeURIComponent(nearest.name);
  console.log("resolveStationIds: Requesting station name");
  var req = new XMLHttpRequest();
  req.open('GET', url, true);
  req.onload = function() {
    if (req.status !== 200) {
      console.log("locations/suggest status=" + req.status);
      callback([]);
      return;
    }
    try {
      var res = JSON.parse(req.responseText);
      var stations = res.stations || [];
      var result = [];
      var seen = {};

      for (var i = 0; i < stations.length; i++) {
        var station = stations[i];
        if (!station || station.kind !== "station" || !station.id) continue;
        if (seen[station.id]) continue;
        
        seen[station.id] = true;
        result.push({
          id: station.id,
          name: station.name || nearest.name,
          feedName: station.feedName || ""
        });
      }
      console.log("resolveStationIds: station IDs=" + result.length);
      callback(result);
    } catch (e) {
      console.log("resolveStationIds: JSON parse error");
      callback([]);
    }
  };
  req.onerror = function() {
    console.log("resolveStationIds: Network Error");
    callback([]);
  };
  req.send();
}

/*
 * ============================================================
 * Query all railway IDs of the selected station
 * ============================================================
 */
function queryStationPlans(baseUrl, stationIds, index, allJourneys) {
  if (index >= stationIds.length) {
    finishStationPlans(allJourneys);
    return;
  }

  var stationId = stationIds[index].id;
  console.log("queryStationPlans: " + (index + 1) + "/" + stationIds.length);

  queryPlanForStation(baseUrl, stationId, function(journeys) {
    if (journeys && journeys.length > 0) {
      for (var i = 0; i < journeys.length; i++) {
        journeys[i]._chidoriFromStationId = stationId;
        allJourneys.push(journeys[i]);
      }
    }
    queryStationPlans(baseUrl, stationIds, index + 1, allJourneys);
  });
}

/*
 * ============================================================
 * Query /plan for one exact station ID
 * ============================================================
 */
function queryPlanForStation(
  baseUrl,
  fromStationId,
  callback
) {
  var url =
    baseUrl +
    "/api/v1/plan?from=" +
    encodeURIComponent(fromStationId) +
    "&to=" +
    encodeURIComponent(destinationStationId) +
    "&type=arrival&time=" +
    encodeURIComponent(arrivalSearchTime) +
    "&allowModes=rail&maxTransfers=8&numItineraries=6";

  console.log(
    "queryPlan: arrival search"
  );

  console.log(
    "queryPlan: from=" +
    fromStationId
  );

  console.log(
    "queryPlan: to=" +
    destinationStationId
  );

  console.log(
    "queryPlan: time=" +
    arrivalSearchTime
  );

  var req =
    new XMLHttpRequest();

  req.open(
    'GET',
    url,
    true
  );

  req.onload = function() {
    console.log(
      "plan status=" + req.status
    );

    if (req.status !== 200) {
      console.log(
        "plan error for station=" +
        fromStationId
      );

      callback([]);
      return;
    }

    try {
      var res =
        JSON.parse(
          req.responseText
        );

      var journeys =
        res.journeys ||
        res.itineraries ||
        [];

      console.log(
        "plan journeys=" +
        journeys.length
      );

      journeys =
        filterRailJourneys(
          journeys
        );

      /*
       * arrival検索の結果から、
       * 乗車駅の発車時刻が最も遅い経路を選ぶ
       */
      journeys.sort(
        function(a, b) {
          return (
            getJourneyDepartureSecs(b) -
            getJourneyDepartureSecs(a)
          );
        }
      );

      /*
       * 最大6件
       */
      if (journeys.length > 6) {
        journeys =
          journeys.slice(0, 6);
      }

      callback(journeys);

    } catch (e) {
      console.log(
        "queryPlanForStation: JSON parse error"
      );

      callback([]);
    }
  };

  req.onerror = function() {
    console.log(
      "queryPlanForStation: Network Error"
    );

    callback([]);
  };

  req.send();
}

/*
 * ============================================================
 * Filter rail-only journeys (Fixed: Allows routes with at least one rail leg)
 * ============================================================
 */
function filterRailJourneys(journeys) {
  var result = [];
  for (var i = 0; i < journeys.length; i++) {
    var journey = journeys[i];
    if (!journey || !journey.legs || journey.legs.length === 0) {
      continue;
    }
    var hasRail = false;
    for (var j = 0; j < journey.legs.length; j++) {
      var mode = (journey.legs[j].mode || "").toUpperCase();
      if (mode === "RAIL" || mode === "TRAIN" || mode === "SUBWAY" || mode === "METRO" || mode === "LIGHT_RAIL") {
        hasRail = true;
        break;
      }
    }
    if (hasRail) {
      result.push(journey);
    }
  }
  return result;
}

/*
 * ============================================================
 * Finish station search
 * ============================================================
 */
function finishStationPlans(journeys) {
  console.log("queryPlan: Rail itineraries=" + (journeys ? journeys.length : 0));

  if (!journeys || journeys.length === 0) {
    console.log("queryPlan: No valid rail routes found.");
    currentItineraries = [];
    currentItinIndex = 0;
    sendDataToPebble({ station: "No Rail Route", hour: -1, min: 0 });
    return;
  }

  /*
   * 「目的地に到着できる経路」の中から、
   * 乗車駅の発車時刻が最も遅い順に並べる。
   */
  journeys.sort(function(a, b) {
    return (getJourneyDepartureSecs(b) - getJourneyDepartureSecs(a));
  });

  /*
   * 最大6件。
   */
  if (journeys.length > 6) {
    journeys = journeys.slice(0, 6);
  }

  currentItineraries = journeys;
  currentItinIndex = 0;

  console.log("queryPlan: Selected itineraries=" + currentItineraries.length);
  sendItineraryToPebble(currentItineraries[0]);
}

/*
 * ============================================================
 * Get journey departure time
 * ============================================================
 */
function getJourneyDepartureSecs(journey) {
  if (journey && journey.departureSecs !== undefined) {
    return parseInt(journey.departureSecs, 10);
  }
  if (journey && journey.legs && journey.legs.length > 0) {
    for (var i = 0; i < journey.legs.length; i++) {
      if (journey.legs[i].departureSecs !== undefined) {
        return parseInt(journey.legs[i].departureSecs, 10);
      }
    }
  }
  return -1;
}

/*
 * ============================================================
 * Send itinerary to Pebble
 * ============================================================
 */
function sendItineraryToPebble(itin) {
  if (!itin || !itin.legs || itin.legs.length === 0) {
    sendDataToPebble({ station: "No Leg Data", hour: -1, min: 0 });
    return;
  }

  var transitLeg = null;
  for (var i = 0; i < itin.legs.length; i++) {
    var mode = (itin.legs[i].mode || "").toUpperCase();
    if (mode === "RAIL" || mode === "TRAIN" || mode === "SUBWAY" || mode === "METRO" || mode === "LIGHT_RAIL") {
      transitLeg = itin.legs[i];
      break;
    }
  }

  if (!transitLeg) {
    sendDataToPebble({ station: "No Rail Leg", hour: -1, min: 0 });
    return;
  }

  var lastLeg = itin.legs[itin.legs.length - 1];

  var stationName = transitLeg.from ? (transitLeg.from.name || "Station") : "Station";
  var destName = lastLeg.to ? (lastLeg.to.name || "") : "";
  var lineName = transitLeg.routeShortName || transitLeg.routeName || transitLeg.mode || "";

  // UnicodeDecodeError 対策：AppMessage送信前に安全なバイト長へ切り詰め
  stationName = truncateForPebble(stationName);
  destName = truncateForPebble("To: " + destName);
  lineName = truncateForPebble(lineName);

  var depHour = -1;
  var depMin = 0;

  if (transitLeg.departureSecs !== undefined) {
    var totalSecs = parseInt(transitLeg.departureSecs, 10);
    totalSecs = totalSecs % 86400;
    if (totalSecs < 0) { totalSecs += 86400; }
    depHour = Math.floor(totalSecs / 3600);
    depMin = Math.floor((totalSecs % 3600) / 60);
  } else if (transitLeg.startTime) {
    var depTime = new Date(transitLeg.startTime);
    depHour = depTime.getHours();
    depMin = depTime.getMinutes();
  }

  var dict = {};
  var stationKey = (typeof messageKeys !== 'undefined') ? messageKeys.KEY_STATION : 'KEY_STATION';
  var typeKey = (typeof messageKeys !== 'undefined') ? messageKeys.KEY_TYPE_TEXT : 'KEY_TYPE_TEXT';
  var destKey = (typeof messageKeys !== 'undefined') ? messageKeys.KEY_DEST : 'KEY_DEST';
  var hourKey = (typeof messageKeys !== 'undefined') ? messageKeys.KEY_HOUR : 'KEY_HOUR';
  var minKey = (typeof messageKeys !== 'undefined') ? messageKeys.KEY_MIN : 'KEY_MIN';

	dict[stationKey] = stationName;
	dict[typeKey] = lineName;
	dict[destKey] = destName;
	dict[hourKey] = depHour;
	dict[minKey] = depMin;

	var alarmMinutesKey =
			(typeof messageKeys !== 'undefined') ?
			messageKeys.KEY_ALARM_MINUTES_BEFORE :
	'KEY_ALARM_MINUTES_BEFORE';

	dict[alarmMinutesKey] = alarmMinutesBefore;

  console.log("Sending itinerary: " + stationName + " " + depHour + ":" + (depMin < 10 ? "0" + depMin : depMin));

  Pebble.sendAppMessage(dict, function() {}, function(e) {
    console.log("sendItineraryToPebble: Message failed");
  });
}

/*
 * ============================================================
 * Generic status/error message
 * ============================================================
 */
function sendDataToPebble(data) {
  var dict = {};

  var stationKey =
    (typeof messageKeys !== 'undefined') ?
      messageKeys.KEY_STATION :
      'KEY_STATION';

  var hourKey =
    (typeof messageKeys !== 'undefined') ?
      messageKeys.KEY_HOUR :
      'KEY_HOUR';

  var minKey =
    (typeof messageKeys !== 'undefined') ?
      messageKeys.KEY_MIN :
      'KEY_MIN';

  var alarmMinutesKey =
    (typeof messageKeys !== 'undefined') ?
      messageKeys.KEY_ALARM_MINUTES_BEFORE :
      'KEY_ALARM_MINUTES_BEFORE';

  dict[stationKey] = data.station;
  dict[hourKey] = data.hour;
  dict[minKey] = data.min;
  dict[alarmMinutesKey] = alarmMinutesBefore;

  Pebble.sendAppMessage( dict, function() {}, function() {}
  );
}

/*
 * ============================================================
 * Alarm Timeline
 * ============================================================
 */
var ALARM_TIMELINE_ID = 'chidori-ashi-alarm';

function addAlarmToTimeline(alarmUnixTime) {
  console.log("addAlarmToTimeline: alarmUnixTime=" + alarmUnixTime);

  var alarmSeconds = parseInt(alarmUnixTime, 10);

  if (!isFinite(alarmSeconds) || alarmSeconds <= 0) {
    console.log("addAlarmToTimeline: invalid alarm time");
    return;
  }

  var alarmDate = new Date(alarmSeconds * 1000);

  if (isNaN(alarmDate.getTime())) {
    console.log("addAlarmToTimeline: invalid Date");
    return;
  }

  var stationName = "終電";
  var destName = "下車駅";

  if (currentItineraries.length > 0) {
    var itin = currentItineraries[currentItinIndex];

    if (itin && itin.legs && itin.legs.length > 0) {

      var transitLeg = null;

      for (var i = 0; i < itin.legs.length; i++) {
        var mode =
          (itin.legs[i].mode || "").toUpperCase();

        if (
          mode === "RAIL" ||
          mode === "TRAIN" ||
          mode === "SUBWAY" ||
          mode === "METRO" ||
          mode === "LIGHT_RAIL"
        ) {
          transitLeg = itin.legs[i];
          break;
        }
      }

      if (transitLeg && transitLeg.from) {
        stationName =
          transitLeg.from.name || stationName;
      }

      var lastLeg =
        itin.legs[itin.legs.length - 1];

      if (lastLeg && lastLeg.to) {
        destName =
          lastLeg.to.name || destName;
      }
    }
  }

  var pin = {
    "id": ALARM_TIMELINE_ID,
    "time": alarmDate.toISOString(),
    "layout": {
      "type": "genericPin",
      "title":
        "終電" +
        alarmMinutesBefore +
        "分前",
      "body":
        stationName +
        " → " +
        destName,
      "tinyIcon":
        "system://images/SCHEDULED_EVENT"
    }
  };

  console.log(
    "addAlarmToTimeline: inserting " +
    ALARM_TIMELINE_ID
  );

  Pebble.insertTimelinePin(
    pin,
    function() {
      console.log(
        "addAlarmToTimeline: success"
      );
    },
    function() {
      console.log(
        "addAlarmToTimeline: failed"
      );
    }
  );
}

function removeAlarmFromTimeline() {
  console.log(
    "removeAlarmFromTimeline: deleting " +
    ALARM_TIMELINE_ID
  );

  Pebble.deleteTimelinePin(
    ALARM_TIMELINE_ID,
    function() {
      console.log(
        "removeAlarmFromTimeline: success"
      );
    },
    function() {
      console.log(
        "removeAlarmFromTimeline: failed"
      );
    }
  );
}

/*
 * ============================================================
 * Timeline
 * ============================================================
 */
function addCurrentTrainToTimeline() {
  console.log("addCurrentTrainToTimeline: Started");
  if (currentItineraries.length === 0) {
    sendTimelineResult(0);
    return;
  }

  var itin = currentItineraries[currentItinIndex];
  if (!itin || !itin.legs || itin.legs.length === 0) {
    sendTimelineResult(0);
    return;
  }

  var transitLeg = null;
  for (var i = 0; i < itin.legs.length; i++) {
    var mode = (itin.legs[i].mode || "").toUpperCase();
    if (mode === "RAIL" || mode === "TRAIN" || mode === "SUBWAY" || mode === "METRO" || mode === "LIGHT_RAIL") {
      transitLeg = itin.legs[i];
      break;
    }
  }

  if (!transitLeg) {
    sendTimelineResult(0);
    return;
  }

  var stationName = transitLeg.from ? (transitLeg.from.name || "Station") : "Station";
  var departureDate = null;

  if (transitLeg.departureSecs !== undefined) {
    departureDate = new Date();
    departureDate.setHours(0, 0, 0, 0);
    departureDate.setTime(departureDate.getTime() + (parseInt(transitLeg.departureSecs, 10) * 1000));
  } else if (transitLeg.startTime) {
    departureDate = new Date(transitLeg.startTime);
  }

  if (!departureDate || isNaN(departureDate.getTime())) {
    sendTimelineResult(0);
    return;
  }

  var pinId = 'chidori-ashi-' + departureDate.getTime();
  var pin = {
    "id": pinId,
    "time": departureDate.toISOString(),
    "layout": {
      "type": "genericPin",
      "title": stationName,
      "body": "Chidori-ashi",
      "tinyIcon": "system://images/SCHEDULED_EVENT"
    }
  };

	console.log("addCurrentTrainToTimeline: Inserting pin");

	Pebble.insertTimelinePin(pin);
  sendTimelineResult(1);
	
/*Pebble.insertTimelinePin( pin,
  function() { sendTimelineResult(1); },
  function() { sendTimelineResult(0); }
);*/
	
}

/*
 * ============================================================
 * Timeline result
 * ============================================================
 */
function sendTimelineResult(status) {
  var dict = {};
  var resultKey = (typeof messageKeys !== 'undefined') ? messageKeys.KEY_TIMELINE_RESULT : 'KEY_TIMELINE_RESULT';
  dict[resultKey] = status;
  Pebble.sendAppMessage(dict, function() {}, function() {});
}