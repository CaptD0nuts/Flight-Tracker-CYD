// Background fetcher: nearby aircraft from adsb.lol and routes from adsbdb,
// on its own core-0 task so the radar animation never waits on the network.
#include "flights.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <math.h>

static float s_lat, s_lon, s_cosLat;
static int s_radius;                // guarded by s_lock
static bool s_pollNow = false;      // guarded by s_lock

static SemaphoreHandle_t s_lock;
static FlightSnapshot s_snap;     // guarded by s_lock
static Route s_route;             // guarded by s_lock; the one route currently wanted
static FlightSnapshot s_work;     // fetch scratch, task only

// Small route cache so flipping between planes doesn't refetch.
#define ROUTE_CACHE 8
static Route s_cache[ROUTE_CACHE];  // guarded by s_lock
static int s_cacheNext = 0;

static void copyStr(char *dst, size_t n, const char *src) {
  strlcpy(dst, src ? src : "", n);
  // callsigns arrive space-padded ("DAL1353 ")
  for (int i = strlen(dst) - 1; i >= 0 && dst[i] == ' '; --i) dst[i] = 0;
}

static bool httpGet(HTTPClient &http, WiFiClientSecure &client, const char *url) {
  // Public read-only data, so the certificate isn't checked.
  client.setInsecure();
  http.useHTTP10(true);  // no chunked encoding, so the body can be parsed as a stream
  http.setTimeout(10000);
  http.setUserAgent(USER_AGENT);
  return http.begin(client, url);
}

static int fetchPlanes(int radius) {
  char url[112];
  snprintf(url, sizeof url, "https://api.adsb.lol/v2/point/%.4f/%.4f/%d", s_lat, s_lon, radius);
  WiFiClientSecure client;
  HTTPClient http;
  if (!httpGet(http, client, url)) return -100;
  int code = http.GET();
  if (code != 200) {
    http.end();
    return code == 0 ? -101 : code;
  }

  JsonDocument filter;
  JsonObject f = filter["ac"].add<JsonObject>();
  for (const char *k : {"hex", "flight", "r", "t", "alt_baro", "gs", "track", "lat", "lon"}) f[k] = true;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) {
    Serial.printf("adsb.lol json: %s\n", err.c_str());
    return -102;
  }

  int n = 0;
  for (JsonObject a : doc["ac"].as<JsonArray>()) {
    // Skip aircraft on the ground (alt_baro is the string "ground") or without a position.
    if (!a["alt_baro"].is<int>() || a["alt_baro"].as<int>() <= 0) continue;
    if (!a["lat"].is<float>() || !a["lon"].is<float>()) continue;
    Plane p = {};
    copyStr(p.hex, sizeof p.hex, a["hex"]);
    copyStr(p.flight, sizeof p.flight, a["flight"]);
    copyStr(p.reg, sizeof p.reg, a["r"]);
    copyStr(p.type, sizeof p.type, a["t"]);
    p.altFt = a["alt_baro"].as<int>();
    p.gsKt = a["gs"].is<float>() ? a["gs"].as<float>() : NAN;
    p.trackDeg = a["track"].is<float>() ? a["track"].as<float>() : NAN;
    p.xNm = (a["lon"].as<float>() - s_lon) * 60.0f * s_cosLat;
    p.yNm = (a["lat"].as<float>() - s_lat) * 60.0f;
    p.distNm = sqrtf(p.xNm * p.xNm + p.yNm * p.yNm);
    if (n < MAX_PLANES) {
      s_work.planes[n++] = p;
    } else {
      // Full: replace the farthest if this one is closer.
      int far = 0;
      for (int i = 1; i < n; ++i) if (s_work.planes[i].distNm > s_work.planes[far].distNm) far = i;
      if (p.distNm < s_work.planes[far].distNm) s_work.planes[far] = p;
    }
  }
  std::sort(s_work.planes, s_work.planes + n, [](const Plane &a, const Plane &b) { return a.distNm < b.distNm; });
  s_work.count = n;
  return 0;
}

static const char *pick(JsonVariantConst a, JsonVariantConst b, const char *fallback) {
  if (a.is<const char *>() && *a.as<const char *>()) return a.as<const char *>();
  if (b.is<const char *>() && *b.as<const char *>()) return b.as<const char *>();
  return fallback;
}

static void fetchRoute(Route &r) {
  char url[80];
  snprintf(url, sizeof url, "https://api.adsbdb.com/v0/callsign/%s", r.callsign);
  WiFiClientSecure client;
  HTTPClient http;
  if (!httpGet(http, client, url)) return;
  int code = http.GET();
  if (code == 404) {  // adsbdb doesn't know this callsign (private, military, GA...)
    r.done = true;
  } else if (code == 200) {
    JsonDocument doc;
    if (!deserializeJson(doc, http.getStream())) {
      JsonObject fr = doc["response"]["flightroute"];
      r.done = true;
      if (!fr.isNull()) {
        r.known = true;
        char cs[sizeof r.callsign];
        strlcpy(cs, r.callsign, sizeof cs);
        copyStr(r.ident, sizeof r.ident, pick(fr["callsign_iata"], fr["callsign"], cs));
        copyStr(r.from, sizeof r.from, pick(fr["origin"]["iata_code"], fr["origin"]["icao_code"], ""));
        copyStr(r.to, sizeof r.to, pick(fr["destination"]["iata_code"], fr["destination"]["icao_code"], ""));
        copyStr(r.airline, sizeof r.airline, pick(fr["airline"]["name"], JsonVariantConst(), ""));
      }
    }
  }
  // Anything else (timeout, 5xx): leave done=false so it's retried.
  http.end();
}

static Route *cached(const char *callsign) {
  for (auto &c : s_cache) if (c.done && strcmp(c.callsign, callsign) == 0) return &c;
  return nullptr;
}

static void task(void *) {
  uint32_t nextPoll = 0;
  uint32_t lastFetch = 0;
  uint32_t backoffUntil = 0;
  uint32_t routeRetryAt = 0;
  for (;;) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int radius = s_radius;
    if (s_pollNow) {
      // Radius changed: fetch soon, but not more than once per 5s (rapid
      // zoom taps) and never during a rate-limit back-off.
      s_pollNow = false;
      uint32_t soonest = lastFetch + 5000;
      if ((int32_t)(nextPoll - soonest) > 0 && (int32_t)(millis() - backoffUntil) >= 0) nextPoll = soonest;
    }
    xSemaphoreGive(s_lock);
    if (WiFi.status() == WL_CONNECTED && (int32_t)(millis() - nextPoll) >= 0) {
      nextPoll = millis() + FLIGHT_POLL_MS;
      lastFetch = millis();
      int rc = fetchPlanes(radius);
      xSemaphoreTake(s_lock, portMAX_DELAY);
      if (rc == 0) {
        memcpy(s_snap.planes, s_work.planes, sizeof(Plane) * s_work.count);
        s_snap.count = s_work.count;
        s_snap.fetchedMs = millis();
        s_snap.everOk = true;
      }
      s_snap.lastError = rc;
      xSemaphoreGive(s_lock);
      if (rc) Serial.printf("adsb.lol fetch failed: %d\n", rc);
      if (rc == 429) nextPoll = backoffUntil = millis() + 60000;  // rate limited: back off a minute
    }

    Route want;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    want = s_route;
    xSemaphoreGive(s_lock);
    if (want.callsign[0] && !want.done && WiFi.status() == WL_CONNECTED &&
        (int32_t)(millis() - routeRetryAt) >= 0) {
      fetchRoute(want);
      if (!want.done) routeRetryAt = millis() + 10000;
      xSemaphoreTake(s_lock, portMAX_DELAY);
      if (strcmp(s_route.callsign, want.callsign) == 0) s_route = want;
      if (want.done) {
        s_cache[s_cacheNext] = want;
        s_cacheNext = (s_cacheNext + 1) % ROUTE_CACHE;
      }
      xSemaphoreGive(s_lock);
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

void flightsBegin(float lat, float lon, int radiusNm) {
  s_lat = lat;
  s_lon = lon;
  s_cosLat = cosf(lat * (float)M_PI / 180.0f);
  s_radius = radiusNm;
  s_lock = xSemaphoreCreateMutex();
  // TLS needs a deep stack.
  xTaskCreatePinnedToCore(task, "flights", 12288, nullptr, 1, nullptr, 0);
}

void flightsSetRadius(int radiusNm) {
  xSemaphoreTake(s_lock, portMAX_DELAY);
  s_radius = radiusNm;
  s_pollNow = true;
  xSemaphoreGive(s_lock);
}

void flightsSnapshot(FlightSnapshot &out) {
  xSemaphoreTake(s_lock, portMAX_DELAY);
  out.count = s_snap.count;
  memcpy(out.planes, s_snap.planes, sizeof(Plane) * s_snap.count);
  out.fetchedMs = s_snap.fetchedMs;
  out.everOk = s_snap.everOk;
  out.lastError = s_snap.lastError;
  xSemaphoreGive(s_lock);
}

void flightsRoute(const char *callsign, Route &out) {
  xSemaphoreTake(s_lock, portMAX_DELAY);
  if (strcmp(s_route.callsign, callsign) != 0) {
    Route *c = cached(callsign);
    if (c) {
      s_route = *c;
    } else {
      s_route = Route{};
      strlcpy(s_route.callsign, callsign, sizeof s_route.callsign);
    }
  }
  out = s_route;
  xSemaphoreGive(s_lock);
}
