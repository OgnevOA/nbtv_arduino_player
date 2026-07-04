// Persistent device settings in NVS (flash), plus SoftAP provisioning.
#pragma once

#include <Arduino.h>

struct Settings {
    String wifi_ssid;
    String wifi_pass;
    String server_url;   // e.g. http://truenas.lan:32125
    String token;        // shared X-Device-Token (optional)
    float  speed;        // disc-lock trim, default 0.95
    bool   invert;       // signal polarity for sync-positive kits
    float  gain;         // digital output level 0..1 (sync-separator headroom)
};

void settings_load(Settings &s);
void settings_save(const Settings &s);
bool settings_have_wifi(const Settings &s);

// Blocking SoftAP captive portal: serve a form, save creds, reboot on submit.
void settings_provision();
