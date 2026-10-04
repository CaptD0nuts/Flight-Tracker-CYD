#pragma once

// adsb.lol rejects generic user agents ("include valid contact info").
#define USER_AGENT "cyd-flight-radar (github.com/CaptD0nuts)"

#define FLIGHT_POLL_MS   30000   // adsb.lol point query interval; 15s drew 429s
#define FLIGHT_STALE_MS  120000  // no good fetch for this long = NO SIGNAL (rides out a 429 back-off)
#define MAX_PLANES       40      // nearest N kept; the rest are dropped
#define DEFAULT_RADIUS_NM 10
// Range steps for the on-screen -/+ buttons (adsb.lol allows up to 250).
#define RADIUS_STEPS {5, 10, 25, 50, 100}

// Hotspot WiFiManager opens when no saved network works or no location is set.
#define SETUP_AP_NAME "FLIGHT-RADAR-SETUP"
#define PORTAL_HOLD_MS 3000      // hold the screen this long while powering on to reopen setup
