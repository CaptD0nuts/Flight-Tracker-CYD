// Radar scope (left, 8-bit sprite redrawn every frame) and info panel (right,
// drawn straight to the screen only when it changes).
#include "radar.h"
#include <math.h>

#define SCOPE_SIZE 220
#define SCOPE_X 6
#define SCOPE_Y 10
#define SCOPE_R 106          // px radius of the outer range ring
#define PANEL_X 232
#define PANEL_W (320 - PANEL_X)
#define SWEEP_MS 4000        // one full turn of the sweep line
#define SWEEP_TRAIL 14       // fading lines behind the sweep
#define EXTRAPOLATE_MAX_S 60 // stop gliding planes forward after this long without data
#define BTN 30               // zoom buttons: squares in the scope's bottom corners

#define C_GRID   0x0320      // dim green
#define C_GRID2  0x01A0      // dimmer green
#define C_SWEEP  0x07E0      // bright green
#define C_PLANE  0xFD20      // amber
#define C_SEL    TFT_WHITE
#define C_LABEL  0xFD20
#define C_GOLD   0xFEA0
#define C_DIM    0x8410      // gray
#define C_SKY    0x351F

static TFT_eSPI *s_tft;
static TFT_eSprite *s_spr;
static int s_radiusNm;
static uint32_t s_zoomShownMs = 0;  // when the range last changed, for the big range readout
static uint32_t s_panelHash = 0;

void radarBegin(TFT_eSPI &tft, int radiusNm) {
  s_tft = &tft;
  s_radiusNm = radiusNm;
  s_spr = new TFT_eSprite(&tft);
  s_spr->setColorDepth(8);
  s_spr->createSprite(SCOPE_SIZE, SCOPE_SIZE);
  tft.fillScreen(TFT_BLACK);
  tft.drawFastVLine(PANEL_X - 4, 0, 240, C_GRID2);
  s_panelHash = 0;
}

void radarSetRadius(int radiusNm) {
  s_radiusNm = radiusNm;
  s_zoomShownMs = millis();
}

int radarButtonHit(int x, int y) {
  // Hit boxes a bit larger than the drawn buttons; touch is roughly calibrated.
  if (y < SCOPE_Y + SCOPE_SIZE - BTN - 10) return 0;
  if (x < SCOPE_X + BTN + 10) return -1;
  if (x > SCOPE_X + SCOPE_SIZE - BTN - 10 && x < PANEL_X - 4) return 1;
  return 0;
}

// Plane position in scope pixels (relative to the scope center), glided
// forward along its track since the last fetch.
static void planePx(const Plane &p, uint32_t fetchedMs, uint32_t now, float &px, float &py) {
  float x = p.xNm, y = p.yNm;
  if (!isnan(p.gsKt) && !isnan(p.trackDeg)) {
    float dt = (now - fetchedMs) / 1000.0f;
    if (dt > EXTRAPOLATE_MAX_S) dt = EXTRAPOLATE_MAX_S;
    float d = p.gsKt * dt / 3600.0f;
    float t = p.trackDeg * (float)M_PI / 180.0f;
    x += d * sinf(t);
    y += d * cosf(t);
  }
  float scale = (float)SCOPE_R / s_radiusNm;
  px = x * scale;
  py = -y * scale;  // north is up
}

static void drawPlane(TFT_eSprite &s, float cx, float cy, float trackDeg, uint16_t color) {
  if (isnan(trackDeg)) {
    s.fillCircle(cx, cy, 2, color);
    return;
  }
  float t = trackDeg * (float)M_PI / 180.0f;
  float fx = sinf(t), fy = -cosf(t);  // forward unit vector, screen coords
  float rx = -fy, ry = fx;            // right of forward
  s.fillTriangle(cx + fx * 6, cy + fy * 6,
                 cx - fx * 4 + rx * 4, cy - fy * 4 + ry * 4,
                 cx - fx * 4 - rx * 4, cy - fy * 4 - ry * 4, color);
}

void radarDrawScope(const FlightSnapshot &snap, int selected, uint32_t now) {
  TFT_eSprite &s = *s_spr;
  const int c = SCOPE_SIZE / 2;
  s.fillSprite(TFT_BLACK);

  // Range rings, crosshair, north mark.
  for (int i = 1; i <= 3; ++i) s.drawCircle(c, c, SCOPE_R * i / 3, i == 3 ? C_GRID : C_GRID2);
  s.drawFastHLine(c - SCOPE_R, c, SCOPE_R * 2, C_GRID2);
  s.drawFastVLine(c, c - SCOPE_R, SCOPE_R * 2, C_GRID2);
  s.setTextColor(C_GRID, TFT_BLACK);
  s.setTextDatum(TC_DATUM);
  s.drawString("N", c, 0, 1);
  s.setTextDatum(TL_DATUM);
  char rng[8];
  snprintf(rng, sizeof rng, "%dNM", s_radiusNm);
  s.drawString(rng, c + 3, c - SCOPE_R + 2, 1);

  // Sweep line with a fading trail behind it.
  float a0 = (now % SWEEP_MS) * 2.0f * (float)M_PI / SWEEP_MS;
  for (int i = SWEEP_TRAIL; i >= 0; --i) {
    float a = a0 - i * 0.04f;
    uint8_t g = 255 - i * (230 / SWEEP_TRAIL);
    s.drawLine(c, c, c + sinf(a) * SCOPE_R, c - cosf(a) * SCOPE_R, s.color565(0, g, 0));
  }

  // Planes. Labels only when the scope isn't crowded, plus the selected one.
  bool labels = snap.count <= 8;
  for (int i = snap.count - 1; i >= 0; --i) {  // farthest first, nearest on top
    const Plane &p = snap.planes[i];
    float px, py;
    planePx(p, snap.fetchedMs, now, px, py);
    if (px * px + py * py > (SCOPE_R + 4) * (SCOPE_R + 4)) continue;  // glided out of range
    float x = c + px, y = c + py;
    bool sel = i == selected;
    drawPlane(s, x, y, p.trackDeg, sel ? C_SEL : C_PLANE);
    if (sel) s.drawRect(x - 9, y - 9, 19, 19, C_SEL);
    if (labels || sel) {
      s.setTextColor(sel ? C_SEL : C_LABEL, TFT_BLACK);
      s.drawString(p.flight[0] ? p.flight : p.hex, x + 9, y - 3, 1);
    }
  }

  // Zoom buttons.
  s.setTextDatum(MC_DATUM);
  s.setTextColor(C_SWEEP, TFT_BLACK);
  s.drawRect(0, SCOPE_SIZE - BTN, BTN, BTN, C_GRID);
  s.drawString("-", BTN / 2, SCOPE_SIZE - BTN / 2, 4);
  s.drawRect(SCOPE_SIZE - BTN, SCOPE_SIZE - BTN, BTN, BTN, C_GRID);
  s.drawString("+", SCOPE_SIZE - BTN / 2, SCOPE_SIZE - BTN / 2, 4);
  s.setTextDatum(TL_DATUM);

  s.fillCircle(c, c, 2, C_SKY);  // you are here

  // Big range readout for a moment after a zoom, so the tap visibly did something.
  if (s_zoomShownMs && now - s_zoomShownMs < 1500) {
    char big[12];
    snprintf(big, sizeof big, "%d NM", s_radiusNm);
    s.setTextDatum(MC_DATUM);
    s.setTextColor(TFT_WHITE, TFT_BLACK);
    s.drawString(big, c, c - 30, 4);
    s.setTextDatum(TL_DATUM);
  }
  s.pushSprite(SCOPE_X, SCOPE_Y);
}

static uint32_t hashStr(uint32_t h, const char *s) {
  while (*s) h = h * 31 + (uint8_t)*s++;
  return h * 31 + 0xFF;
}

static const char *compass(float deg) {
  static const char *pts[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
  return pts[(int)lroundf(deg / 45.0f) % 8];
}

void radarDrawPanel(const FlightSnapshot &snap, int selected, bool autoSelect, const Route &route, uint32_t now) {
  char l[9][32] = {};
  // Status (bottom): count + freshness.
  uint32_t age = snap.everOk ? (now - snap.fetchedMs) / 1000 : 0;
  if (!snap.everOk) snprintf(l[8], 32, snap.lastError ? "NO SIGNAL %d" : "SCANNING...", snap.lastError);
  else if (age > FLIGHT_STALE_MS / 1000) snprintf(l[8], 32, "NO SIGNAL %lus", (unsigned long)age);
  else snprintf(l[8], 32, "%d IN %dNM", snap.count, s_radiusNm);

  if (selected >= 0 && selected < snap.count) {
    const Plane &p = snap.planes[selected];
    const char *ident = route.known && route.ident[0] ? route.ident : p.flight[0] ? p.flight : p.reg[0] ? p.reg : p.hex;
    snprintf(l[0], 32, "%s", ident);
    if (route.known && route.from[0] && route.to[0]) snprintf(l[1], 32, "%s > %s", route.from, route.to);
    else if (p.flight[0] && !route.done) snprintf(l[1], 32, "ROUTE...");
    snprintf(l[2], 32, "%.10s", route.known ? route.airline : "");
    snprintf(l[3], 32, "TYPE %s", p.type[0] ? p.type : "?");
    snprintf(l[4], 32, "ALT %ld'", (long)p.altFt);
    if (!isnan(p.gsKt)) snprintf(l[5], 32, "SPD %dkt", (int)lroundf(p.gsKt));
    float mi = p.distNm * 1.15078f;
    float brg = atan2f(p.xNm, p.yNm) * 180.0f / (float)M_PI;
    if (brg < 0) brg += 360;
    snprintf(l[6], 32, mi < 10 ? "%.1fmi %s" : "%.0fmi %s", mi, compass(brg));
    snprintf(l[7], 32, autoSelect ? "NEAREST" : "TAP=AUTO");
  } else if (snap.everOk) {
    snprintf(l[0], 32, "SKY");
    snprintf(l[1], 32, "CLEAR");
  }

  uint32_t h = 7;
  for (auto &s : l) h = hashStr(h, s);
  if (h == s_panelHash) return;
  s_panelHash = h;

  TFT_eSPI &t = *s_tft;
  t.fillRect(PANEL_X, 0, PANEL_W, 240, TFT_BLACK);
  t.setTextDatum(TL_DATUM);
  t.setTextColor(C_GOLD, TFT_BLACK);
  // Font 4 is ~14px/char; drop to font 2 for long idents so they fit 88px.
  t.drawString(l[0], PANEL_X, 10, strlen(l[0]) <= 6 ? 4 : 2);
  t.setTextColor(TFT_WHITE, TFT_BLACK);
  t.drawString(l[1], PANEL_X, 40, 2);
  t.setTextColor(C_DIM, TFT_BLACK);
  t.drawString(l[2], PANEL_X, 58, 2);
  t.setTextColor(C_SKY, TFT_BLACK);
  for (int i = 3; i <= 6; ++i) t.drawString(l[i], PANEL_X, 86 + (i - 3) * 20, 2);
  t.setTextColor(C_DIM, TFT_BLACK);
  t.drawString(l[7], PANEL_X, 180, 2);
  t.setTextColor(snap.everOk && age <= FLIGHT_STALE_MS / 1000 ? C_SWEEP : TFT_RED, TFT_BLACK);
  t.drawString(l[8], PANEL_X, 216, 1);
}

int radarHitTest(const FlightSnapshot &snap, int x, int y, uint32_t now) {
  const int c = SCOPE_SIZE / 2;
  int best = -1;
  float bestD2 = 22 * 22;  // tap must land within 22px of a plane
  for (int i = 0; i < snap.count; ++i) {
    float px, py;
    planePx(snap.planes[i], snap.fetchedMs, now, px, py);
    float dx = SCOPE_X + c + px - x, dy = SCOPE_Y + c + py - y;
    float d2 = dx * dx + dy * dy;
    if (d2 < bestD2) bestD2 = d2, best = i;
  }
  return best;
}

void radarMessage(const char *line1, const char *line2, const char *line3) {
  TFT_eSPI &t = *s_tft;
  t.fillScreen(TFT_BLACK);
  t.setTextDatum(MC_DATUM);
  t.setTextColor(C_SWEEP, TFT_BLACK);
  t.drawString(line1, 160, 90, 4);
  t.setTextColor(TFT_WHITE, TFT_BLACK);
  t.drawString(line2, 160, 130, 2);
  t.setTextColor(C_DIM, TFT_BLACK);
  t.drawString(line3, 160, 155, 2);
  t.setTextDatum(TL_DATUM);
  s_panelHash = 0;
}
