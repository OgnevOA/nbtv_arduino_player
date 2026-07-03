// NBTV portable player firmware - M5 Atom Lite + Atomic Audio-3.5 Base.
//
// Arduino IDE: open this folder as a sketch (nbtv_player.ino). The other .cpp/.h
// files in this folder are compiled automatically. Board: "M5Stack-ATOM".
// Install libraries via Library Manager: M5Atomic-EchoBase, M5Unified, ArduinoJson.
//
// The output loop ALWAYS emits a valid NBTV waveform (live frame, frozen frame,
// or test card), so the mechanical disc never loses lock regardless of network
// state. Networking runs on core 0; this render/output loop runs on core 1.
#include <Arduino.h>
#include <WiFi.h>

#include "nbtv_config.h"
#include "nbtv_render.h"
#include "audio_out.h"
#include "frame_buffer.h"
#include "settings.h"
#include "button.h"
#include "net.h"

enum State { ST_IDLE, ST_PLAYING, ST_FROZEN };

static Settings g_set;
static State    g_state = ST_IDLE;

static uint8_t  g_test[NBTV_FRAME_BYTES];
static uint8_t  g_cur[NBTV_FRAME_BYTES];
static uint8_t  g_last[NBTV_FRAME_BYTES];
static bool     g_have_last = false;
static int16_t  g_samples[NBTV_SAMPLES];

static int      g_underruns = 0;
static uint16_t g_frameseq = 0;
static char     g_cur_id[24] = "";

// Fine speed steps for on-device disc-lock trimming (long-press cycles these).
static const float SPEED_PRESETS[] = {
    0.90f, 0.91f, 0.92f, 0.93f, 0.94f, 0.95f,
    0.96f, 0.97f, 0.98f, 0.99f, 1.00f};
static int g_speed_idx = 5;  // 0.95

// Time-stretch output buffer. The codec runs at a FIXED 48000 Hz; "speed" is
// achieved by resampling each frame to 48000/(12.5*speed) samples (more samples
// = longer frame period = slower effective fps = matches a slower disc).
static const int MAX_OUT = 5200;
static int16_t g_out[MAX_OUT];

// Output level and band-limit now live in Settings (NVS), so serial/button
// tuning survives reboots. See g_set.gain / g_set.lowpass.

static float effective_fps() { return NBTV_BASE_FPS * g_set.speed; }

static int stretch_for(float speed) {
    int n = (int)lroundf((float)NBTV_SAMPLES / speed);
    if (n > MAX_OUT) n = MAX_OUT;
    if (n < 1) n = 1;
    return n;
}

static void emit(const uint8_t *frame) {
    nbtv_render_frame(frame, g_samples, g_set.invert);
    if (g_set.lowpass) nbtv_bandlimit(g_samples, NBTV_SAMPLES);
    // Linear-resample the composite (picture + sync) to set the frame period.
    int n = stretch_for(g_set.speed);
    float step = (n > 1) ? (float)(NBTV_SAMPLES - 1) / (n - 1) : 0.0f;
    for (int i = 0; i < n; ++i) {
        float pos = i * step;
        int i0 = (int)pos;
        int i1 = (i0 + 1 < NBTV_SAMPLES) ? i0 + 1 : NBTV_SAMPLES - 1;
        float f = pos - i0;
        float s = (g_samples[i0] * (1.0f - f) + g_samples[i1] * f) * g_set.gain;
        if (s > 32767.0f) s = 32767.0f;
        if (s < -32768.0f) s = -32768.0f;
        g_out[i] = (int16_t)lroundf(s);
    }
    audio_write(g_out, n);
}

static void handle_command(const Command &c) {
    switch (c.type) {
        case CMD_PLAY:
            Serial.printf("[cmd] play id=%s loop=%d\n", c.id, c.flag);
            strncpy(g_cur_id, c.id, sizeof(g_cur_id) - 1);
            g_state = ST_PLAYING;
            g_underruns = 0;
            net_stream_start(c.id, c.flag);
            break;
        case CMD_STOP:
            Serial.println("[cmd] stop");
            g_state = ST_IDLE;
            net_stream_stop();
            break;
        case CMD_FREEZE:
            Serial.println("[cmd] freeze");
            g_state = ST_FROZEN;
            net_stream_stop();
            break;
        case CMD_TESTCARD:
            Serial.println("[cmd] testcard");
            g_state = ST_IDLE;
            net_stream_stop();
            break;
        case CMD_SPEED:
            g_set.speed = c.value;
            settings_save(g_set);
            Serial.printf("[cmd] speed=%.4f (%.2f fps)\n", g_set.speed,
                          effective_fps());
            break;
        case CMD_LOOP:
            // Loop is handled server-side (it keeps re-sending the stream).
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
    Serial.printf("[tune] speed=%.4f (%.2f fps)  gain=%.2f  invert=%d  lowpass=%d  state=%d\n",
                  g_set.speed, effective_fps(), g_set.gain, g_set.invert,
                  (int)g_set.lowpass, (int)g_state);
}

// Live bench tuning over the USB serial monitor (mirrors the desktop --tune):
//   +/-  speed FINE  (0.0002)       ,/.  speed COARSE (0.005)
//   [/]  output level down/up       i    invert polarity
//   l    toggle low-pass (sync fix) t    show test card    p  print status
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
            case 'l': case 'L': g_set.lowpass = !g_set.lowpass; changed = true; break;
            case 't': case 'T': g_state = ST_IDLE; net_stream_stop(); break;
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
            Serial.println("[btn] single -> idle/test card");
            if (g_state == ST_PLAYING) { g_state = ST_IDLE; net_stream_stop(); }
            else { g_state = ST_IDLE; }   // show test card
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
            g_set.invert = !g_set.invert;  // handy field polarity toggle
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
    Serial.println("\n=== NBTV portable player ===");
    Serial.printf("build: %s %s\n", __DATE__, __TIME__);

    button_init();
    settings_load(g_set);
    Serial.printf("[cfg] ssid='%s' url='%s' token=%s speed=%.4f invert=%d\n",
                  g_set.wifi_ssid.c_str(), g_set.server_url.c_str(),
                  g_set.token.length() ? "set" : "none",
                  g_set.speed, g_set.invert);

    // Provision on first boot (no creds) or if the button is held at power-on.
    if (!settings_have_wifi(g_set) || button_down_now()) {
        Serial.println("[cfg] no credentials / button held -> setup portal "
                       "(AP 'NBTV-Setup')");
        settings_provision();  // blocks; reboots on save
    }

    for (int i = 0; i < (int)(sizeof(SPEED_PRESETS) / sizeof(SPEED_PRESETS[0])); ++i)
        if (fabsf(SPEED_PRESETS[i] - g_set.speed) < 0.005f) g_speed_idx = i;

    nbtv_test_card(g_test);
    fb_init();
    bool codec_ok = audio_begin();  // fixed 48000 Hz; emit before WiFi is up
    Serial.printf("[audio] codec init %s @ %d Hz; speed=%.4f (%.2f fps)\n",
                  codec_ok ? "OK" : "FAILED", NBTV_BASE_RATE, g_set.speed,
                  effective_fps());
    Serial.println("[out] emitting test card; disc should lock now");
    Serial.println("[tune] keys: +/- speed fine  ,/. speed coarse  "
                   "[/] level  i invert  t testcard  p print");

    WiFi.setAutoReconnect(true);
    Serial.printf("[wifi] connecting to '%s'...\n", g_set.wifi_ssid.c_str());
    bool wifi_ok = net_wifi_connect(g_set, 15000);
    if (wifi_ok)
        Serial.printf("[wifi] connected, IP=%s rssi=%d\n",
                      WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
    else
        Serial.println("[wifi] NOT connected (will keep retrying; test card runs)");
    net_begin(g_set);
    Serial.printf("[net] server=%s (long-poll + status started)\n",
                  g_set.server_url.c_str());
}

void loop() {
    Command c;
    while (net_next_command(c)) handle_command(c);
    handle_serial();
    handle_button();

    const uint8_t *frame = g_test;
    const char *state_name = "idle";

    if (g_state == ST_PLAYING) {
        if (fb_pop(g_cur)) {
            memcpy(g_last, g_cur, NBTV_FRAME_BYTES);
            g_have_last = true;
            g_frameseq++;
            frame = g_cur;
        } else {
            g_underruns++;              // hold last frame; sync stays live
            frame = g_have_last ? g_last : g_test;
        }
        state_name = (fb_count() > 0 || net_stream_active()) ? "playing"
                                                             : "buffering";
        // Stream finished and drained -> back to test card.
        if (!net_stream_active() && fb_count() == 0) {
            g_state = ST_IDLE;
            Serial.println("[out] stream ended + drained -> test card");
        }
    } else if (g_state == ST_FROZEN) {
        frame = g_have_last ? g_last : g_test;
        state_name = "frozen";
    }

    int buffer_ms = (int)(fb_count() * (1000.0f / NBTV_BASE_FPS));
    net_set_status(state_name, g_cur_id, g_frameseq, buffer_ms,
                   g_underruns, g_set.speed);

    // Throttled status line (~1 Hz) so it doesn't drown the 12.5 fps loop.
    static uint32_t last_log = 0;
    if (millis() - last_log >= 1000) {
        last_log = millis();
        Serial.printf("[st] %-9s buf=%2d/%dms seq=%u under=%d spd=%.3f/%.2ffps "
                      "gain=%.2f wifi=%d rssi=%d heap=%u\n",
                      state_name, fb_count(), buffer_ms, (unsigned)g_frameseq,
                      g_underruns, g_set.speed, effective_fps(), g_set.gain,
                      net_wifi_ok() ? 1 : 0,
                      net_wifi_ok() ? (int)WiFi.RSSI() : 0,
                      (unsigned)ESP.getFreeHeap());
    }

    emit(frame);  // blocks ~1 frame; paces the whole loop to the disc clock
}
