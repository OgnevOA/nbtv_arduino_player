#include "net.h"
#include "nbtv_config.h"
#include "sample_buffer.h"

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

static String s_base;    // e.g. http://host:port
static String s_token;

static QueueHandle_t s_cmd_q;
static volatile uint32_t s_last_seq = 0;

// --- status shared with the poll task ---
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static char  st_state[16] = "boot";
static int   st_buf_ms    = 0;
static int   st_underruns = 0;
static float st_speed     = 0.95f;

static volatile bool s_stream_active = false;

// ---------------------------------------------------------------------------
bool net_wifi_connect(const Settings &s, uint32_t timeout_ms) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(s.wifi_ssid.c_str(), s.wifi_pass.c_str());
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeout_ms) {
        delay(200);
    }
    return WiFi.status() == WL_CONNECTED;
}

bool net_wifi_ok() { return WiFi.status() == WL_CONNECTED; }

// ---------------------------------------------------------------------------
static void apply_json_command(JsonDocument &doc) {
    const char *cmd = doc["cmd"] | "none";
    uint32_t seq = doc["seq"] | 0;
    if (seq) s_last_seq = seq;

    Command c;
    if (!strcmp(cmd, "speed"))       c.type = CMD_SPEED;
    else if (!strcmp(cmd, "invert")) c.type = CMD_INVERT;
    else if (!strcmp(cmd, "reboot")) c.type = CMD_REBOOT;
    else return;  // "none" / unknown -> no event

    c.value = doc["value"] | 0.0f;
    xQueueSend(s_cmd_q, &c, 0);
}

static void build_status_json(char *out, size_t n) {
    char state[16];
    int buf_ms, under; float speed;
    portENTER_CRITICAL(&s_mux);
    strncpy(state, st_state, sizeof(state));
    buf_ms = st_buf_ms; under = st_underruns; speed = st_speed;
    portEXIT_CRITICAL(&s_mux);

    JsonDocument doc;
    doc["state"]     = state;
    doc["buffer_ms"] = buf_ms;
    doc["underruns"] = under;
    doc["speed"]     = speed;
    doc["rssi"]      = WiFi.RSSI();
    serializeJson(doc, out, n);
}

static void poll_task(void *) {
    uint32_t last_status = 0;
    for (;;) {
        if (!net_wifi_ok()) { delay(500); continue; }

        HTTPClient http;
        String url = s_base + "/control/poll?since=" + String(s_last_seq);
        http.begin(url);
        http.setTimeout(30000);
        if (s_token.length()) http.addHeader("X-Device-Token", s_token);
        int code = http.GET();
        if (code == 200) {
            JsonDocument doc;
            if (deserializeJson(doc, http.getStream()) == DeserializationError::Ok) {
                apply_json_command(doc);
            }
        }
        http.end();

        if (millis() - last_status > 1000) {
            last_status = millis();
            char body[192];
            build_status_json(body, sizeof(body));
            HTTPClient sp;
            sp.begin(s_base + "/status");
            sp.addHeader("Content-Type", "application/json");
            if (s_token.length()) sp.addHeader("X-Device-Token", s_token);
            sp.POST((uint8_t *)body, strlen(body));
            sp.end();
        }
        delay(10);
    }
}

// --- raw HTTP + chunked reader for the PCM radio stream --------------------
struct ChunkReader {
    WiFiClient *c;
    bool chunked = false;
    long chunk_left = 0;

    bool fill_chunk_header() {
        String line = c->readStringUntil('\n');
        line.trim();
        if (line.length() == 0) return false;
        chunk_left = strtol(line.c_str(), nullptr, 16);
        return chunk_left > 0;   // 0 => last chunk
    }

    // Read exactly n bytes; returns false on connection loss/timeout.
    bool read_full(uint8_t *buf, int n, uint32_t deadline) {
        int got = 0;
        while (got < n) {
            if (!c->connected() && c->available() == 0) return false;
            if (millis() > deadline) return false;
            if (chunked && chunk_left == 0) {
                if (!fill_chunk_header()) return false;
            }
            int want = n - got;
            if (chunked && want > chunk_left) want = (int)chunk_left;
            int r = c->read(buf + got, want);
            if (r <= 0) { delay(1); continue; }
            got += r;
            if (chunked) {
                chunk_left -= r;
                if (chunk_left == 0) { c->read(); c->read(); }  // trailing CRLF
            }
        }
        return true;
    }
};

static bool read_headers(WiFiClient &c, bool &chunked) {
    chunked = false;
    uint32_t start = millis();
    while (c.connected() && millis() - start < 8000) {
        String line = c.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) return true;              // end of headers
        String low = line; low.toLowerCase();
        if (low.startsWith("transfer-encoding:") && low.indexOf("chunked") >= 0)
            chunked = true;
    }
    return false;
}

static void stream_task(void *) {
    static uint8_t pcm[2048];   // 1024 samples per read
    for (;;) {
        if (!net_wifi_ok()) { delay(500); continue; }

        // Parse host/port from base URL (http only).
        String host; int port = 80; String url = s_base;
        url.replace("http://", "");
        int slash = url.indexOf('/');
        String hostport = slash >= 0 ? url.substring(0, slash) : url;
        int colon = hostport.indexOf(':');
        if (colon >= 0) { host = hostport.substring(0, colon);
                          port = hostport.substring(colon + 1).toInt(); }
        else host = hostport;

        WiFiClient client;
        if (!client.connect(host.c_str(), port)) {
            Serial.printf("[radio] connect %s:%d failed\n", host.c_str(), port);
            delay(1000);
            continue;
        }

        String req = String("GET /radio.wav HTTP/1.1\r\n") +
                     "Host: " + host + "\r\n" + "Connection: close\r\n";
        if (s_token.length()) req += "X-Device-Token: " + s_token + "\r\n";
        req += "\r\n";
        client.print(req);

        bool chunked = false;
        if (!read_headers(client, chunked)) { client.stop(); continue; }

        ChunkReader rd{&client, chunked, 0};
        // Discard the 44-byte WAV header at the start of the body.
        uint8_t wav[44];
        if (!rd.read_full(wav, 44, millis() + 8000)) { client.stop(); continue; }

        Serial.printf("[radio] streaming (chunked=%d)\n", chunked);
        s_stream_active = true;
        while (net_wifi_ok()) {
            if (!rd.read_full(pcm, sizeof(pcm), millis() + 8000)) break;
            const int16_t *s = (const int16_t *)pcm;
            int total = sizeof(pcm) / 2;
            int off = 0;
            while (off < total) {                 // push with backpressure
                int w = sb_push(s + off, total - off, 1000);
                off += w;
                if (!net_wifi_ok()) break;
            }
        }
        client.stop();
        s_stream_active = false;
        Serial.println("[radio] connection closed; reconnecting");
        delay(200);
    }
}

// ---------------------------------------------------------------------------
void net_begin(const Settings &s) {
    s_base = s.server_url;
    while (s_base.endsWith("/")) s_base.remove(s_base.length() - 1);
    s_token = s.token;
    s_cmd_q = xQueueCreate(8, sizeof(Command));
    xTaskCreatePinnedToCore(poll_task, "nbtv_poll", 6144, nullptr, 3, nullptr, 0);
    xTaskCreatePinnedToCore(stream_task, "nbtv_radio", 6144, nullptr, 4, nullptr, 0);
}

bool net_next_command(Command &out) {
    return xQueueReceive(s_cmd_q, &out, 0) == pdTRUE;
}

void net_set_status(const char *state, int buffer_ms, int underruns, float speed) {
    portENTER_CRITICAL(&s_mux);
    strncpy(st_state, state, sizeof(st_state) - 1);
    st_buf_ms = buffer_ms; st_underruns = underruns; st_speed = speed;
    portEXIT_CRITICAL(&s_mux);
}

bool net_stream_active() { return s_stream_active; }
