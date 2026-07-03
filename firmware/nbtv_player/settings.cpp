#include "settings.h"

#include <Preferences.h>
#include <WiFi.h>
#include <WebServer.h>

// Optional compile-time credentials from secrets.h (gitignored). Absent in CI.
#if defined(__has_include)
#  if __has_include("secrets.h")
#    include "secrets.h"
#  endif
#endif

#ifndef NBTV_SPEED
#define NBTV_SPEED 0.95f
#endif
#ifndef NBTV_GAIN
#define NBTV_GAIN 0.05f
#endif

static const char *NS = "nbtv";

// Apply non-empty secrets over the loaded settings. Connection fields (ssid,
// pass, url, token) are refreshed from secrets on every boot so a re-flash can
// update them; speed stays under NVS so button/Telegram tuning persists.
static void apply_secrets(Settings &s) {
#ifdef NBTV_WIFI_SSID
    if (sizeof(NBTV_WIFI_SSID) > 1) s.wifi_ssid = NBTV_WIFI_SSID;
#endif
#ifdef NBTV_WIFI_PASS
    if (sizeof(NBTV_WIFI_PASS) > 1) s.wifi_pass = NBTV_WIFI_PASS;
#endif
#ifdef NBTV_SERVER_URL
    if (sizeof(NBTV_SERVER_URL) > 1) s.server_url = NBTV_SERVER_URL;
#endif
#ifdef NBTV_DEVICE_TOKEN
    if (sizeof(NBTV_DEVICE_TOKEN) > 1) s.token = NBTV_DEVICE_TOKEN;
#endif
}

void settings_load(Settings &s) {
    Preferences p;
    p.begin(NS, true);
    s.wifi_ssid  = p.getString("ssid", "");
    s.wifi_pass  = p.getString("pass", "");
    s.server_url = p.getString("url", "");
    s.token      = p.getString("token", "");
    s.speed      = p.getFloat("speed", NBTV_SPEED);
    s.invert     = p.getBool("invert", false);
    s.gain       = p.getFloat("gain", NBTV_GAIN);
    s.lowpass    = p.getBool("lp", true);
    p.end();
    apply_secrets(s);
}

void settings_save(const Settings &s) {
    Preferences p;
    p.begin(NS, false);
    p.putString("ssid", s.wifi_ssid);
    p.putString("pass", s.wifi_pass);
    p.putString("url", s.server_url);
    p.putString("token", s.token);
    p.putFloat("speed", s.speed);
    p.putBool("invert", s.invert);
    p.putFloat("gain", s.gain);
    p.putBool("lp", s.lowpass);
    p.end();
}

bool settings_have_wifi(const Settings &s) {
    return s.wifi_ssid.length() > 0 && s.server_url.length() > 0;
}

static const char *FORM =
    "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>NBTV setup</title>"
    "<style>body{font-family:sans-serif;max-width:26em;margin:2em auto;padding:0 1em}"
    "input{width:100%;padding:.5em;margin:.3em 0;box-sizing:border-box}"
    "button{padding:.6em 1.2em}</style>"
    "<h2>NBTV player setup</h2>"
    "<form method=POST action=/save>"
    "WiFi SSID<input name=ssid>"
    "WiFi password<input name=pass type=password>"
    "Server URL<input name=url placeholder='http://truenas.lan:8080'>"
    "Device token (optional)<input name=token>"
    "Speed<input name=speed value='0.95'>"
    "<button type=submit>Save &amp; reboot</button></form>";

void settings_provision() {
    Settings s;
    settings_load(s);

    WiFi.mode(WIFI_AP);
    WiFi.softAP("NBTV-Setup");
    WebServer server(80);

    server.on("/", HTTP_GET, [&server]() { server.send(200, "text/html", FORM); });
    server.on("/save", HTTP_POST, [&server, &s]() {
        s.wifi_ssid  = server.arg("ssid");
        s.wifi_pass  = server.arg("pass");
        s.server_url = server.arg("url");
        s.token      = server.arg("token");
        if (server.arg("speed").length()) s.speed = server.arg("speed").toFloat();
        settings_save(s);
        server.send(200, "text/html", "<p>Saved. Rebooting...</p>");
        delay(800);
        ESP.restart();
    });
    // Captive-portal-ish: send the form for any path.
    server.onNotFound([&server]() { server.send(200, "text/html", FORM); });
    server.begin();

    for (;;) {
        server.handleClient();
        delay(2);
    }
}
