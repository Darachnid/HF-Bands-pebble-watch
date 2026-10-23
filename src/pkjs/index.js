var URLS = [
  "https://www.hamqsl.com/solarxml.php",
  "http://www.hamqsl.com/solarxml.php"
];
var busy = false;
var gotSolar = false;
var haveData = false;
var retryTimer = null;

function tidy(value) {
  return String(value || "").replace(/\s+/g, " ").trim().replace(/\|/g, "/");
}

function tagText(xml, name) {
  var match = xml.match(new RegExp("<" + name + ">([^<]*)</" + name + ">"));
  return match ? tidy(match[1]) : "";
}

function attr(blob, key) {
  var match = blob.match(new RegExp(key + '="([^"]*)"'));
  return match ? match[1] : "";
}

function bandMap(xml) {
  var bands = {};
  var re = /<band\b([^>]*)>([^<]*)<\/band>/g;
  var match;
  while ((match = re.exec(xml))) {
    var name = attr(match[1], "name");
    if (!bands[name]) bands[name] = {};
    bands[name][attr(match[1], "time")] = tidy(match[2]);
  }
  return bands;
}

function vhfMap(xml) {
  var vhf = {};
  var re = /<phenomenon\b([^>]*)>([^<]*)<\/phenomenon>/g;
  var match;
  while ((match = re.exec(xml))) {
    var key = attr(match[1], "name") + "|" + attr(match[1], "location");
    vhf[key] = tidy(match[2]);
  }
  return vhf;
}

function compact(xml) {
  var bands = bandMap(xml);
  var vhf = vhfMap(xml);
  var hfOrder = ["80m-40m", "30m-20m", "17m-15m", "12m-10m"];
  var vhfOrder = [
    "vhf-aurora|northern_hemi",
    "E-Skip|europe",
    "E-Skip|north_america",
    "E-Skip|europe_6m",
    "E-Skip|europe_4m"
  ];
  var hf = [];
  for (var i = 0; i < hfOrder.length; i++) {
    var row = bands[hfOrder[i]] || {};
    hf.push(row.day || "", row.night || "");
  }
  var openings = [];
  for (var j = 0; j < vhfOrder.length; j++) openings.push(vhf[vhfOrder[j]] || "");
  return [
    tagText(xml, "updated"),
    tagText(xml, "solarflux"),
    tagText(xml, "sunspots"),
    tagText(xml, "aindex"),
    tagText(xml, "kindex"),
    tagText(xml, "xray"),
    tagText(xml, "heliumline"),
    tagText(xml, "protonflux"),
    tagText(xml, "electonflux"),
    tagText(xml, "aurora"),
    tagText(xml, "normalization"),
    tagText(xml, "latdegree"),
    tagText(xml, "solarwind"),
    tagText(xml, "magneticfield"),
    tagText(xml, "geomagfield"),
    tagText(xml, "signalnoise"),
    tagText(xml, "muf") || "NoRpt",
    hf.join(","),
    openings.join(","),
    "N0NBH"
  ].join("|");
}

var outbox = [];
var sending = false;

function enqueue(payload, onOk, onFail) {
  outbox.push({ payload: payload, onOk: onOk, onFail: onFail });
  pump();
}

function pump() {
  if (sending || !outbox.length) return;
  sending = true;
  var item = outbox.shift();
  Pebble.sendAppMessage({ SOLAR: item.payload }, function() {
    sending = false;
    if (item.onOk) item.onOk();
    pump();
  }, function() {
    sending = false;
    console.log("solar send failed");
    if (item.onFail) item.onFail();
    pump();
  });
}

function markSent() {
  gotSolar = true;
  haveData = true;
  console.log("solar sent");
}

function sendFail() {
  if (haveData) return;
  enqueue("ERR");
}

function scheduleRetry() {
  if (gotSolar || haveData || retryTimer) return;
  retryTimer = setTimeout(function() {
    retryTimer = null;
    fetchSolar(false);
  }, 20000);
}

function giveUp(why) {
  busy = false;
  console.log("solar fail " + why);
  sendFail();
  scheduleRetry();
}

function getText(url, ms, ok, fail) {
  var done = false;
  var req = new XMLHttpRequest();
  var timer = setTimeout(function() {
    if (done) return;
    done = true;
    try { req.abort(); } catch (ignore) {}
    fail("timeout");
  }, ms);
  try {
    req.open("GET", url);
    req.onload = function() {
      if (done) return;
      done = true;
      clearTimeout(timer);
      if (req.status >= 200 && req.status < 300 && req.responseText) ok(req.responseText);
      else fail("http " + req.status);
    };
    req.onerror = function() {
      if (done) return;
      done = true;
      clearTimeout(timer);
      fail("error");
    };
    req.send();
  } catch (err) {
    if (done) return;
    done = true;
    clearTimeout(timer);
    fail(String(err));
  }
}

function lastDaily(text) {
  var lines = String(text).split("\n");
  for (var i = lines.length - 1; i >= 0; i--) {
    var line = lines[i].replace(/^\s+|\s+$/g, "");
    if (!line || line.charAt(0) === ":" || line.charAt(0) === "#") continue;
    var parts = line.split(/\s+/);
    if (parts.length >= 5) return parts;
    break;
  }
  return null;
}

function geomagFromK(k) {
  if (k <= 1) return "VR QUIET";
  if (k <= 2) return "QUIET";
  if (k <= 3) return "UNSETTLED";
  if (k <= 4) return "ACTIVE";
  return "STORM";
}

function guessBands(sfi, k) {
  var storm = k >= 5;
  var rough = k >= 4;
  function cell(rank) {
    if (storm) return "Poor";
    if (rough && rank === "Good") return "Fair";
    if (rough && rank === "Fair") return "Poor";
    return rank;
  }
  var day = [
    cell(sfi >= 80 ? "Fair" : "Poor"),
    cell(sfi >= 90 ? "Good" : (sfi >= 70 ? "Fair" : "Poor")),
    cell(sfi >= 110 ? "Good" : (sfi >= 90 ? "Fair" : "Poor")),
    cell(sfi >= 140 ? "Fair" : "Poor")
  ];
  var night = [
    cell("Good"),
    cell(sfi >= 75 ? "Good" : "Fair"),
    cell(sfi >= 100 ? "Fair" : "Poor"),
    cell(sfi >= 150 ? "Fair" : "Poor")
  ];
  var hf = [];
  for (var i = 0; i < 4; i++) hf.push(day[i], night[i]);
  return hf.join(",");
}

function noaaStamp(iso) {
  var match = String(iso || "").match(/(\d{4})-(\d{2})-(\d{2})[T ](\d{2})(\d{2})/);
  var months = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"];
  if (!match) return "NOAA";
  return "NOAA " + Number(match[3]) + " " + months[Number(match[2]) - 1] + " " + match[4] + match[5];
}

function buildNoaa(bag) {
  var flux = bag.flux && bag.flux.length ? bag.flux[bag.flux.length - 1].flux : null;
  var kRow = bag.k && bag.k.length ? bag.k[bag.k.length - 1] : null;
  var daily = lastDaily(bag.daily || "");
  var sfi = flux != null ? Math.round(flux) : (daily ? Math.round(Number(daily[3])) : null);
  var sn = daily ? Math.round(Number(daily[4])) : null;
  if (sfi == null || isNaN(sfi)) return "";
  var k = kRow ? Math.round(kRow.Kp) : 0;
  var a = kRow && kRow.a_running != null ? Math.round(kRow.a_running) : 0;
  var wind = bag.wind && bag.wind.length ? Math.round(bag.wind[bag.wind.length - 1].proton_speed) : "";
  var bz = bag.mag && bag.mag.length ? bag.mag[bag.mag.length - 1].bz_gsm : "";
  var aurora = k >= 5 ? "Open" : "Band Closed";
  return [
    noaaStamp(kRow && kRow.time_tag),
    sfi,
    sn == null || isNaN(sn) ? "--" : sn,
    a,
    k,
    "--",
    "--",
    "--",
    "--",
    k >= 5 ? String(k) : "--",
    "--",
    "--",
    wind,
    bz === "" || bz == null ? "--" : String(bz),
    geomagFromK(k),
    "--",
    "--",
    guessBands(sfi, k),
    [aurora, "Band Closed", "Band Closed", "Band Closed", "Band Closed"].join(","),
    "NOAA"
  ].join("|");
}

function fetchNoaa() {
  var bag = {};
  getText("https://services.swpc.noaa.gov/products/summary/10cm-flux.json", 8000, function(text) {
    try { bag.flux = JSON.parse(text); } catch (ignore) {}
    getText("https://services.swpc.noaa.gov/products/noaa-planetary-k-index.json", 8000, function(text) {
      try { bag.k = JSON.parse(text); } catch (ignore) {}
      getText("https://services.swpc.noaa.gov/text/daily-solar-indices.txt", 8000, function(text) {
        bag.daily = text;
        getText("https://services.swpc.noaa.gov/products/summary/solar-wind-speed.json", 8000, function(text) {
          try { bag.wind = JSON.parse(text); } catch (ignore) {}
          getText("https://services.swpc.noaa.gov/products/summary/solar-wind-mag-field.json", 8000, function(text) {
            try { bag.mag = JSON.parse(text); } catch (ignore) {}
            busy = false;
            var payload = "";
            try { payload = buildNoaa(bag); } catch (err) { console.log("noaa " + err); }
            if (!payload) {
              giveUp("noaa empty");
              return;
            }
            console.log("noaa bytes " + payload.length);
            enqueue(payload, markSent, function() { scheduleRetry(); });
          }, function(err) { giveUp(err); });
        }, function(err) { giveUp(err); });
      }, function(err) { giveUp(err); });
    }, function(err) { giveUp(err); });
  }, function(err) { giveUp(err); });
}

function tryHamqsl(index) {
  if (index >= URLS.length) {
    console.log("hamqsl down, using NOAA");
    fetchNoaa();
    return;
  }
  getText(URLS[index], 5000, function(text) {
    if (text.indexOf("<solar") !== -1 && text.indexOf("solarflux") !== -1) {
      busy = false;
      var payload = compact(text);
      console.log("hamqsl bytes " + payload.length);
      enqueue(payload, markSent, function() { scheduleRetry(); });
      return;
    }
    tryHamqsl(index + 1);
  }, function() {
    tryHamqsl(index + 1);
  });
}

function fetchSolar(manual) {
  if (busy || (gotSolar && !manual)) return;
  busy = true;
  tryHamqsl(0);
}

Pebble.addEventListener("ready", function() {
  // Any reply marks the phone link up. The watch ignores this stub.
  enqueue("0", function() { fetchSolar(false); }, function() { fetchSolar(false); });
});

Pebble.addEventListener("appmessage", function(e) {
  if (!(e.payload && e.payload.REFRESH)) return;
  if (retryTimer) {
    clearTimeout(retryTimer);
    retryTimer = null;
  }
  gotSolar = false;
  fetchSolar(true);
});
