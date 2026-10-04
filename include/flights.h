#pragma once
#include <Arduino.h>
#include "config.h"

struct Plane {
  char hex[7];       // ICAO address, stable id for selection
  char flight[9];    // callsign as broadcast, may be empty
  char reg[9];
  char type[5];
  int32_t altFt;
  float gsKt;        // ground speed, NAN if unknown
  float trackDeg;    // NAN if unknown
  float xNm, yNm;    // east/north of the center at fetch time
  float distNm;
};

struct FlightSnapshot {
  Plane planes[MAX_PLANES];
  int count;
  uint32_t fetchedMs;  // millis() of the last good fetch
  bool everOk;
  int lastError;       // HTTP code or negative client error of the last failure, 0 if last fetch was fine
};

struct Route {
  char callsign[9];
  bool done;         // lookup finished (known or not)
  bool known;
  char ident[9];     // IATA flight number when known, e.g. DL1234
  char from[5];
  char to[5];
  char airline[28];
};

void flightsBegin(float lat, float lon, int radiusNm);
void flightsSnapshot(FlightSnapshot &out);
// New query radius; fetches at the new range right away.
void flightsSetRadius(int radiusNm);
// Asks the background task for this callsign's route; returns what's known so far.
void flightsRoute(const char *callsign, Route &out);
