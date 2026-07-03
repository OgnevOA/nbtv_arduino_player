// Firmware credentials template.
//
// Copy this file to "secrets.h" (same folder) and fill in your values.
// secrets.h is gitignored so your credentials never get committed.
//
// If secrets.h is absent, the firmware falls back to the on-device SoftAP
// setup portal ("NBTV-Setup"). If present, these values are used directly and
// the portal is skipped (hold the button at power-on to force the portal).
//
// Leave a value as an empty string ("") to skip it and use NVS / the portal
// for that field instead.
#pragma once

#define NBTV_WIFI_SSID    "your-wifi-ssid"
#define NBTV_WIFI_PASS    "your-wifi-password"
#define NBTV_SERVER_URL   "http://truenas.lan:32125"
#define NBTV_DEVICE_TOKEN ""            // must match the server's DEVICE_TOKEN
#define NBTV_SPEED        0.95f         // default disc-lock speed (tunable later)
