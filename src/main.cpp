// Flight radar on the Cheap Yellow Display: nearby aircraft from adsb.lol on
// a sweeping radar scope, with details for the nearest (or tapped) plane.
#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include "config.h"
#include "flights.h"
#include "radar.h"
#if __has_include("secrets.h")
#include "secrets.h"  // optional WiFi + location filled in on the laptop, not in git
#endif

// The touch controller sits on its own SPI bus (VSPI pins).
#define TOUCH_CS   33
#define TOUCH_IRQ  36
#define TOUCH_MOSI 32
#define TOUCH_MISO 39
#define TOUCH_CLK  25

// Raw XPT2046 range mapped to screen pixels. Rough defaults for the CYD
// (same as the Ono-Sendai deck); plane hit radius is generous.
#define TOUCH_X_MIN 200
#define TOUCH_X_MAX 3700
#define TOUCH_Y_MIN 240
#define TOUCH_Y_MAX 3800

#define FRAME_MS 40

TFT_eSPI tft;
SPIClass touchSpi(VSPI);
XPT2046_Touchscreen touch(TOUCH_CS, TOUCH_IRQ);
Preferences prefs;

static float g_lat = NAN, g_lon = NAN;
static int g_radius = DEFAULT_RADIUS_NM;

static FlightSnapshot g_snap;   // big; keep it off the loop task's stack
static char g_selHex[7] = "";   // empty = auto (nearest)
static bool g_wasTouched = false;

static bool locationValid(float lat, float lon) {
  return !isnan(lat) && !isnan(lon) && lat >= -90 && lat <= 90 && lon >= -180 && lon <= 180 &&
         !(lat == 0 && lon == 0);
}

static void loadSettings() {
  prefs.begin("radar", true);
  g_lat = prefs.getFloat("lat", NAN);
  g_lon = prefs.getFloat("lon", NAN);
  g_radius = prefs.getInt("radius", DEFAULT_RADIUS_NM);
  prefs.end();
}

static void onPortal(WiFiManager *) {
  radarMessage("SETUP", "Join WiFi: " SETUP_AP_NAME, "pick your network + enter lat/lon");
}

// WiFi + location setup. force = open the setup hotspot even if saved WiFi
// works (no location yet, or the user held the screen).
static void connect(bool force) {
  char latBuf[16] = "", lonBuf[16] = "", radBuf[6];
  if (!isnan(g_lat)) snprintf(latBuf, sizeof latBuf, "%.4f", g_lat);
  if (!isnan(g_lon)) snprintf(lonBuf, sizeof lonBuf, "%.4f", g_lon);
  snprintf(radBuf, sizeof radBuf, "%d", g_radius);
  WiFiManagerParameter pLat("lat", "Latitude (e.g. 40.7128)", latBuf, 15);
  WiFiManagerParameter pLon("lon", "Longitude (e.g. -74.0060, west is negative)", lonBuf, 15);
  WiFiManagerParameter pRad("radius", "Radius in nautical miles (2-50)", radBuf, 5);

  WiFiManager wm;
  wm.addParameter(&pLat);
  wm.addParameter(&pLon);
  wm.addParameter(&pRad);
  wm.setAPCallback(onPortal);
  wm.setConfigPortalTimeout(300);
  bool ok = force ? wm.startConfigPortal(SETUP_AP_NAME) : wm.autoConnect(SETUP_AP_NAME);

  // Google Maps copies "40.7128, -74.0060" as one string: accept both numbers
  // pasted into the latitude box.
  const char *latStr = pLat.getValue();
  const char *lonStr = pLon.getValue();
  char *end;
  float lat = strtof(latStr, &end);
  if (end == latStr) lat = NAN;
  while (*end == ' ') ++end;
  if (*end == ',') lonStr = end + 1;
  float lon = strtof(lonStr, &end);
  if (end == lonStr) lon = NAN;
  int rad = constrain(atoi(pRad.getValue()), 2, 50);
  bool valid = locationValid(lat, lon);
  // Lengths only: never log the coordinates themselves.
  Serial.printf("portal: wifi %s, lat field %d chars, lon field %d chars, location %s\n",
                ok ? "connected" : "not connected", (int)strlen(pLat.getValue()), (int)strlen(pLon.getValue()),
                valid ? "valid" : "INVALID");
  if (!valid && !locationValid(g_lat, g_lon)) {
    radarMessage("LOCATION NOT SAVED", isnan(lat) ? "latitude missing" : isnan(lon) ? "longitude missing" : "lat/lon out of range",
                 "e.g. 40.7128 and -74.0060");
    delay(6000);
  }
  if (valid && (lat != g_lat || lon != g_lon || rad != g_radius)) {
    prefs.begin("radar", false);
    prefs.putFloat("lat", lat);
    prefs.putFloat("lon", lon);
    prefs.putInt("radius", rad);
    prefs.end();
    Serial.println("location saved");  // never log the coordinates themselves
  }
  if (!ok || force) {
    // Timed out, or settings may have changed: start clean with what's saved.
    ESP.restart();
  }
}

static int selectedIndex() {
  if (g_snap.count == 0) return -1;
  if (g_selHex[0]) {
    for (int i = 0; i < g_snap.count; ++i)
      if (strcmp(g_snap.planes[i].hex, g_selHex) == 0) return i;
    g_selHex[0] = 0;  // picked plane left range: back to auto
  }
  return 0;  // nearest
}

void setup() {
  Serial.begin(115200);
  tft.init();
  tft.setRotation(1);  // landscape, USB port on the right
  touchSpi.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  touch.begin(touchSpi);
  touch.setRotation(1);
  radarBegin(tft, DEFAULT_RADIUS_NM);
  radarMessage("FLIGHT RADAR", "connecting to WiFi...");

  // Holding the screen while powering on reopens the setup page. Only checked
  // at boot, so handling the board while it runs can't trigger it.
  bool forceSetup = false;
  if (touch.touched()) {
    radarMessage("SETUP", "keep holding to open setup...");
    uint32_t t0 = millis();
    while (touch.touched() && millis() - t0 < PORTAL_HOLD_MS) delay(50);
    forceSetup = touch.touched();
  }

  loadSettings();
#ifdef SECRET_LAT
  // Location from secrets.h, unless one was saved through the setup page.
  if (!locationValid(g_lat, g_lon) && locationValid(SECRET_LAT, SECRET_LON)) {
    g_lat = SECRET_LAT;
    g_lon = SECRET_LON;
  }
#endif
  bool connected = false;
#ifdef SECRET_WIFI_SSID
  if (!forceSetup && strlen(SECRET_WIFI_SSID) && strcmp(SECRET_WIFI_SSID, "PUT-WIFI-NAME-HERE") != 0) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(SECRET_WIFI_SSID, SECRET_WIFI_PASS);
    for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; ++i) delay(500);
    connected = WiFi.status() == WL_CONNECTED;
    Serial.printf("secrets.h wifi: %s\n", connected ? "connected" : "failed, falling back to setup page");
  }
#endif
  bool haveLocation = locationValid(g_lat, g_lon);
  if (forceSetup || !connected || !haveLocation) connect(forceSetup || !haveLocation);
  Serial.printf("wifi up, ip %s, radius %d nm\n", WiFi.localIP().toString().c_str(), g_radius);

  radarBegin(tft, g_radius);
  flightsBegin(g_lat, g_lon, g_radius);
}

void loop() {
  uint32_t start = millis();
  flightsSnapshot(g_snap);

  bool touched = touch.touched();
  if (touched && !g_wasTouched) {
    TS_Point p = touch.getPoint();
    Serial.printf("raw x=%d y=%d z=%d\n", p.x, p.y, p.z);
    int x = constrain(map(p.x, TOUCH_X_MIN, TOUCH_X_MAX, 0, 319), 0, 319);
    int y = constrain(map(p.y, TOUCH_Y_MIN, TOUCH_Y_MAX, 0, 239), 0, 239);
    int zoom = radarButtonHit(x, y);
    if (zoom) {
      // "-" zooms out (bigger radius), "+" zooms in.
      static const int steps[] = RADIUS_STEPS;
      const int n = sizeof steps / sizeof steps[0];
      int i = 0;
      while (i < n - 1 && steps[i] < g_radius) ++i;
      i = constrain(i - zoom, 0, n - 1);
      if (steps[i] != g_radius) {
        g_radius = steps[i];
        prefs.begin("radar", false);
        prefs.putInt("radius", g_radius);
        prefs.end();
        radarSetRadius(g_radius);
        flightsSetRadius(g_radius);
        Serial.printf("radius %d nm\n", g_radius);
      }
    } else {
      int hit = radarHitTest(g_snap, x, y, start);
      Serial.printf("tap x=%d y=%d hit=%d\n", x, y, hit);
      if (hit >= 0) strlcpy(g_selHex, g_snap.planes[hit].hex, sizeof g_selHex);
      else g_selHex[0] = 0;
    }
  }
  g_wasTouched = touched;

  int sel = selectedIndex();
  Route route = {};
  if (sel >= 0 && g_snap.planes[sel].flight[0]) flightsRoute(g_snap.planes[sel].flight, route);
  radarDrawScope(g_snap, sel, start);
  radarDrawPanel(g_snap, sel, g_selHex[0] == 0, route, start);

  uint32_t spent = millis() - start;
  if (spent < FRAME_MS) delay(FRAME_MS - spent);
}
