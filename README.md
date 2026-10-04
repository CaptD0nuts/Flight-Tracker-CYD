# Flight Tracker CYD

A standalone flight radar for the **Cheap Yellow Display** (ESP32-2432S028R, 2.8" 320x240 touch). It pulls nearby aircraft straight from the free [adsb.lol](https://adsb.lol) community ADS-B feed over WiFi and draws them on a sweeping radar scope. No antenna, server, or other hardware needed.

- Green radar scope with a rotating sweep, range rings and a north mark
- Aircraft drawn as amber arrows pointing along their track; between updates they glide forward using their ground speed, so the scope stays smooth
- Side panel for the nearest plane (or the one you tap): flight number, route, airline, aircraft type, altitude, speed, distance and bearing
- Routes come from [adsbdb](https://www.adsbdb.com) (callsign to origin/destination), cached on the board
- **−** / **+** buttons zoom the range through 5, 10, 25, 50 and 100 nautical miles; the choice is saved across power cycles

## Hardware

Any ESP32-2432S028R "Cheap Yellow Display" (the version with the ILI9341 screen and XPT2046 resistive touch). Plain ESP32, 4 MB flash. The ESP32 only does 2.4 GHz WiFi.

## Build and flash

Uses [PlatformIO](https://platformio.org).

```sh
pio run -t upload --upload-port COM4    # your serial port
```

## Setup: WiFi and location

The board needs your WiFi and the spot to center the radar on. Two ways:

**Option A: settings file (easiest when flashing from a computer).**
Copy `include/secrets.example.h` to `include/secrets.h`, fill in your WiFi name and password and your latitude/longitude, then build and flash. `secrets.h` is in `.gitignore`, so it never gets committed.

To get coordinates: in Google Maps, long-press your location and copy the two numbers. West longitudes are negative.

**Option B: phone setup page.**
Without `secrets.h` (or if its WiFi fails), the board opens a hotspot called `FLIGHT-RADAR-SETUP`. Join it from your phone, open `http://192.168.4.1` (turn mobile data off if the page won't load), choose **Configure WiFi**, pick your network, and fill in latitude, longitude and radius on the same page. Pasting both coordinates into the latitude box (`40.7128, -74.0060`) works too.

To reopen the setup page later, hold a finger on the screen while plugging the board in, and keep holding for 3 seconds.

## Using it

| Action | Result |
|---|---|
| Tap a plane | Shows its details in the panel |
| Tap empty scope | Back to auto (nearest plane) |
| Tap **−** (bottom-left of the scope) | Zoom out |
| Tap **+** (bottom-right of the scope) | Zoom in |

Press firmly: the resistive touch ignores very light taps.

The status line at the bottom of the panel shows how many aircraft are in range (`3 IN 25NM`). It turns red with `NO SIGNAL` if the feed hasn't answered for two minutes.

## Notes

- adsb.lol asks for a user agent with contact info and rate-limits busy clients. The board polls every 30 seconds and backs off for a minute if it gets HTTP 429. If you fork this, change `USER_AGENT` in `include/config.h` to your own contact.
- HTTPS certificates aren't checked (`setInsecure`); the data is public and read-only.
- At 100 nm near a major airport the reply can be large for the ESP32's memory; the board keeps the nearest 40 aircraft.

## License

MIT, see [LICENSE](LICENSE). Libraries pulled in by PlatformIO (TFT_eSPI, ArduinoJson, WiFiManager, XPT2046_Touchscreen) keep their own licenses.
