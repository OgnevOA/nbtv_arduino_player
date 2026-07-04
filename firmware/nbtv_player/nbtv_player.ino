// NBTV portable player firmware - M5 Atom Lite + Atomic Audio-3.5 Base.
//
// Arduino IDE: open this folder as a sketch (nbtv_player.ino). The other .cpp/.h
// files in this folder are compiled automatically. Board: "M5Stack-ATOM".
// Install libraries via Library Manager: M5Atomic-EchoBase, M5Unified, ArduinoJson.
//
// Radio architecture: the SERVER synthesizes the finished NBTV composite signal
// (sync + levels + band-limit + headroom) and streams it as lossless mono PCM.
// This device is a thin client: it buffers the PCM, resamples it for disc-lock
// (speed), applies gain/invert, and pushes it to the codec. Networking runs on
// core 0; this output loop runs on core 1.
#include <Arduino.h>
#include <WiFi.h>

#include "nbtv_config.h"
#include "audio_out.h"
#include "sample_buffer.h"
#include "settings.h"
#include "button.h"
#include "net.h"

static Settings g_set;
static int      g_underruns = 0;

// Fine speed presets for on-device disc-lock trimming (long-press cycles these).
static const float SPEED_PRESETS[] = {
    0.90f, 0.91f, 0.92f, 0.93f, 0.94f, 0.95f,
    0.96f, 0.97f, 0.98f, 0.99f, 1.00f};
static int g_speed_idx = 5;  // 0.95

// Output block: 1024 samples ~= 21 ms; audio_write blocks, pacing the loop.
static const int OUT_BLOCK = 1024;
static int16_t g_out[OUT_BLOCK];

static float effective_fps() { return NBTV_BASE_FPS * g_set.speed; }

// --- continuous resampler pulling from the sample ring --------------------
// Playback runs at a FIXED 48000 Hz codec rate; "speed" is achieved by
// consuming `speed` input samples per output sample (>1 = faster disc).
static int16_t s_in[512];
static int  s_in_len = 0, s_in_pos = 0;

static bool pull_input(int16_t &v) {
    if (s_in_pos >= s_in_len) {
        s_in_len = sb_read(s_in, (int)(sizeof(s_in) / sizeof(s_in[0])));
        s_in_pos = 0;
        if (s_in_len == 0) return false;   // underrun
    }
    v = s_in[s_in_pos++];
    return true;
}

static float   s_frac = 0.0f;
static int16_t s_prev = 0, s_cur = 0;
static bool    s_primed = false;

static void emit_block() {
    // Prime with the first two input samples once data is available.
    if (!s_primed) {
        int16_t a;
        if (!pull_input(a)) {                 // nothing buffered yet -> silence
            memset(g_out, 0, sizeof(g_out));
            audio_write(g_out, OUT_BLOCK);
            return;
        }
        s_prev = a;
        s_cur = pull_input(a) ? a : s_prev;
        s_frac = 0.0f;
        s_primed = true;
    }

    bool underran = false;
    for (int i = 0; i < OUT_BLOCK; ++i) {
        float out = s_prev + (s_cur - s_prev) * s_frac;
        if (g_set.invert) out = -out;
        out *= g_set.gain;
        if (out > 32767.0f) out = 32767.0f;
        if (out < -32768.0f) out = -32768.0f;
        g_out[i] = (int16_t)lroundf(out);

        s_frac += g_set.speed;
        while (s_frac >= 1.0f) {
            s_frac -= 1.0f;
            s_prev = s_cur;
            int16_t nv;
            if (pull_input(nv)) s_cur = nv;
            else underran = true;             // hold last sample
        }
    }
    if (underran) g_underruns++;
    audio_write(g_out, OUT_BLOCK);
}

static void handle_command(const Command &c) {
    switch (c.type) {
        case CMD_SPEED:
            g_set.speed = c.value;
            settings_save(g_set);
            Serial.printf("[cmd] speed=%.4f (%.2f fps)\n", g_set.speed,
                          effective_fps());
            break;
        case CMD_INVERT:
            g_set.invert = !g_set.invert;
            settings_save(g_set);
            Serial.printf("[cmd] invert=%d\n", g_set.invert);
            break;
        case CMD_REBOOT:
            Serial.println("[cmd] reboot");
            ESP.restart();
            break;
        default:
            break;
    }
}

static void print_tune() {
    Serial.printf("[tune] speed=%.4f (%.2f fps)  gain=%.2f  invert=%d\n",
                  g_set.speed, effective_fps(), g_set.gain, g_set.invert);
}

// Live bench tuning over the USB serial monitor:
//   +/-  speed FINE (0.0002)     ,/.  speed COARSE (0.005)
//   [/]  output level down/up    i    invert polarity     p  print status
static void handle_serial() {
    bool changed = false;
    while (Serial.available()) {
        char ch = (char)Serial.read();
        switch (ch) {
            case '+': case '=': g_set.speed += 0.0002f; changed = true; break;
            case '-': case '_': g_set.speed -= 0.0002f; changed = true; break;
            case '.': case '>': g_set.speed += 0.005f;  changed = true; break;
            case ',': case '<': g_set.speed -= 0.005f;  changed = true; break;
            case ']': g_set.gain += 0.05f; changed = true; break;
            case '[': g_set.gain -= 0.05f; changed = true; break;
            case 'i': case 'I': g_set.invert = !g_set.invert; changed = true; break;
            case 'p': case 'P': break;  // just print below
            default: continue;
        }
        if (g_set.speed < 0.80f) g_set.speed = 0.80f;
        if (g_set.speed > 1.20f) g_set.speed = 1.20f;
        if (g_set.gain < 0.02f) g_set.gain = 0.02f;
        if (g_set.gain > 1.00f) g_set.gain = 1.00f;
        if (changed) settings_save(g_set);
        print_tune();
    }
}

static void handle_button() {
    switch (button_poll()) {
        case BTN_SINGLE:
            print_tune();
            break;
        case BTN_LONG:
            g_speed_idx = (g_speed_idx + 1) % (int)(sizeof(SPEED_PRESETS) /
                                                    sizeof(SPEED_PRESETS[0]));
            g_set.speed = SPEED_PRESETS[g_speed_idx];
            settings_save(g_set);
            Serial.printf("[btn] long -> speed=%.4f (%.2f fps)\n", g_set.speed,
                          effective_fps());
            break;
        case BTN_DOUBLE:
            g_set.invert = !g_set.invert;
            settings_save(g_set);
            Serial.printf("[btn] double -> invert=%d\n", g_set.invert);
            break;
        default:
            break;
    }
}

void setup() {
    Serial.begin(115200);
    delay(300);  // let USB CDC settle so the banner isn't lost
    Serial.println("\n=== NBTV portable player (radio) ===");
    Serial.printf("build: %s %s\n", __DATE__, __TIME__);

    button_init();
    settings_load(g_set);
    Serial.printf("[cfg] ssid='%s' url='%s' token=%s\n"
                  "[cfg] speed=%.4f gain=%.2f invert=%d\n",
                  g_set.wifi_ssid.c_str(), g_set.server_url.c_str(),
                  g_set.token.length() ? "set" : "none",
                  g_set.speed, g_set.gain, g_set.invert);

    // Provision on first boot (no creds) or if the button is held at power-on.
    if (!settings_have_wifi(g_set) || button_down_now()) {
        Serial.println("[cfg] no credentials / button held -> setup portal "
                       "(AP 'NBTV-Setup')");
        settings_provision();  // blocks; reboots on save
    }

    for (int i = 0; i < (int)(sizeof(SPEED_PRESETS) / sizeof(SPEED_PRESETS[0])); ++i)
        if (fabsf(SPEED_PRESETS[i] - g_set.speed) < 0.005f) g_speed_idx = i;

    sb_init();
    bool codec_ok = audio_begin();  // fixed 48000 Hz
    Serial.printf("[audio] codec init %s @ %d Hz; speed=%.4f (%.2f fps)\n",
                  codec_ok ? "OK" : "FAILED", NBTV_BASE_RATE, g_set.speed,
                  effective_fps());
    Serial.println("[tune] keys: +/- speed fine  ,/. speed coarse  [/] level  "
                   "i invert  p print");

    WiFi.setAutoReconnect(true);
    Serial.printf("[wifi] connecting to '%s'...\n", g_set.wifi_ssid.c_str());
    bool wifi_ok = net_wifi_connect(g_set, 15000);
    if (wifi_ok)
        Serial.printf("[wifi] connected, IP=%s rssi=%d\n",
                      WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
    else
        Serial.println("[wifi] NOT connected (will keep retrying)");
    net_begin(g_set);
    Serial.printf("[net] server=%s (radio + long-poll + status)\n",
                  g_set.server_url.c_str());
}

void loop() {
    Command c;
    while (net_next_command(c)) handle_command(c);
    handle_serial();
    handle_button();

    int filled = sb_count();
    int buffer_ms = filled / (NBTV_BASE_RATE / 1000);   // 48 samples per ms
    const char *state_name = filled > 0 ? "playing"
                           : (net_stream_active() ? "buffering" : "nolink");
    net_set_status(state_name, buffer_ms, g_underruns, g_set.speed);

    static uint32_t last_log = 0;
    if (millis() - last_log >= 1000) {
        last_log = millis();
        Serial.printf("[st] %-9s buf=%dms under=%d spd=%.4f/%.2ffps gain=%.2f "
                      "inv=%d wifi=%d rssi=%d heap=%u\n",
                      state_name, buffer_ms, g_underruns, g_set.speed,
                      effective_fps(), g_set.gain, g_set.invert,
                      net_wifi_ok() ? 1 : 0,
                      net_wifi_ok() ? (int)WiFi.RSSI() : 0,
                      (unsigned)ESP.getFreeHeap());
    }

    emit_block();  // blocks ~21 ms; paces the loop to the codec clock
}
