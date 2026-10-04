#pragma once
#include <TFT_eSPI.h>
#include "flights.h"

void radarBegin(TFT_eSPI &tft, int radiusNm);
void radarSetRadius(int radiusNm);
// -1 / +1 if (x, y) is on the zoom out / in button, else 0.
int radarButtonHit(int x, int y);
// One animation frame of the scope. selected = index into snap.planes or -1.
void radarDrawScope(const FlightSnapshot &snap, int selected, uint32_t now);
// Right-hand panel; only repaints when its text changes.
void radarDrawPanel(const FlightSnapshot &snap, int selected, bool autoSelect, const Route &route, uint32_t now);
// Index of the plane drawn nearest screen point (x, y), or -1 if none is close.
int radarHitTest(const FlightSnapshot &snap, int x, int y, uint32_t now);
// Full-screen two-line message (setup, errors).
void radarMessage(const char *line1, const char *line2, const char *line3 = "");
