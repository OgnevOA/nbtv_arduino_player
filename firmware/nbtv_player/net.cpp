#include "net.h"
#include "nbtv_config.h"
#include "frame_buffer.h"

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
static char  st_id[24]    = "";
static uint16_t st_seq    = 0;
static int   st_buf_ms    = 0;
static int   st_underruns = 0;
static float st_speed     = 0.95f;

// --- stream task control ---
static volatile bool s_stream_run = false;
static volatile bool s_stream_active = false;
static char s_stream_id[24] = "";
static bool s_stream_loop = false;
static TaskHandle_t s_stream_task = nullptr;

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
    if (!strcmp(cmd, "play"))          c.type = CMD_PLAY;
    else if (!strcmp(cmd, "preload"))  c.type = CMD_PRELOAD;
    else if (!strcmp(cmd, "stop"))     c.type = CMD_STOP;
    else if (!strcmp(cmd, "freeze"))   c.type = CMD_FREEZE;
    else if (!strcmp(cmd, "loop"))     c.type = CMD_LOOP;
    else if (!strcmp(cmd, "speed"))    c.type = CMD_SPEED;
    else if (!strcmp(cmd, "reboot"))   c.type = CMD_REBOOT;
    else if (!strcmp(cmd, "testcard")) c.type = CMD_TESTCARD;
    else return;  // "none" / unknown -> no event

    const char *id = doc["id"] | "";
    strncpy(c.id, id, sizeof(c.id) - 1);
    c.flag  = doc["loop"] | (doc["value"] | false);
    c.value = doc["value"] | 0.0f;
    xQueueSend(s_cmd_q, &c, 0);
}

static void build_status_json(char *out, size_t n) {
    // Snapshot under the lock, then build JSON outside (no allocations in the
    // critical section).
    char state[16], id[24];
    uint16_t seq; int buf_ms, under; float speed;
    portENTER_CRITICAL(&s_mux);
    strncpy(state, st_state, sizeof(state));
    strncpy(id, st_id, sizeof(id));
    seq = st_seq; buf_ms = st_buf_ms; under = st_underruns; speed = st_speed;
    portEXIT_CRITICAL(&s_mux);

    JsonDocument doc;
    doc["state"]     = state;
    doc["id"]        = id;
    doc["frame_seq"] = seq;
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

        // Long-poll for the next command.
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

        // Post status about once a second.
        if (millis() - last_status > 1000) {
            last_status = millis();
            char body[256];
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

// --- raw HTTP + chunked reader for the pixel stream ------------------------
struct ChunkReader {
    WiFiClient *c;
    bool chunked = false;
    long chunk_left = 0;   // bytes remaining in current chunk (chunked mode)

    bool fill_chunk_header() {
        // Read hex length line "<hex>\r\n".
        String line = c->readStringUntil('\n');
        line.trim();
        if (line.length() == 0) return false;
        chunk_left = strtol(line.c_str(), nullptr, 16);
        return chunk_left > 0;  // 0 => last chunk
    }

    // Read exactly n bytes into buf; returns false on connection loss/timeout.
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
            if (r <= 0) { delay(2); continue; }
            got += r;
            if (chunked) {
                chunk_left -= r;
                if (chunk_left == 0) {         // consume trailing CRLF
                    c->read(); c->read();
                }
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
    for (;;) {
        if (!s_stream_run) { delay(50); continue; }

        // Parse host/port/path from base URL (http only).
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
            Serial.printf("[stream] connect %s:%d failed\n", host.c_str(), port);
            delay(500);
            continue;
        }

        String path = String("/stream?id=") + s_stream_id +
                      (s_stream_loop ? "&loop=1" : "");
        String req = String("GET ") + path + " HTTP/1.1\r\n" +
                     "Host: " + host + "\r\n" + "Connection: close\r\n";
        if (s_token.length()) req += "X-Device-Token: " + s_token + "\r\n";
        req += "\r\n";
        client.print(req);

        bool chunked = false;
        if (!read_headers(client, chunked)) { client.stop(); continue; }

        ChunkReader rd{&client, chunked, 0};
        uint8_t hdr[NBTV_STREAM_HDR];
        uint32_t dl = millis() + 8000;
        if (!rd.read_full(hdr, NBTV_STREAM_HDR, dl) ||
            memcmp(hdr, NBTV_MAGIC, 4) != 0) {
            Serial.println("[stream] bad/missing header; retrying");
            client.stop(); continue;
        }

        Serial.printf("[stream] playing id=%s loop=%d chunked=%d\n",
                      s_stream_id, s_stream_loop, chunked);
        s_stream_active = true;
        uint8_t fh[NBTV_FRAME_HDR];
        uint8_t frame[NBTV_FRAME_BYTES];
        while (s_stream_run) {
            dl = millis() + 8000;
            if (!rd.read_full(fh, NBTV_FRAME_HDR, dl)) break;
            // Re-lock on the frame anchor if we drifted.
            while (!(fh[0] == NBTV_FRAME_SYNC0 && fh[1] == NBTV_FRAME_SYNC1)) {
                memmove(fh, fh + 1, NBTV_FRAME_HDR - 1);
                if (!rd.read_full(fh + NBTV_FRAME_HDR - 1, 1, dl)) { goto done; }
            }
            uint16_t len = fh[4] | (fh[5] << 8);
            if (len != NBTV_FRAME_BYTES) break;  // corrupt; reconnect
            dl = millis() + 8000;
            if (!rd.read_full(frame, NBTV_FRAME_BYTES, dl)) break;
            // Block until the buffer has room -> TCP backpressure.
            while (s_stream_run && !fb_push(frame, 200)) { /* full: wait */ }
        }
    done:
        client.stop();
        s_stream_active = false;
        Serial.println("[stream] connection closed");
        // Non-loop stream ended: stop requesting until told to play again.
        if (s_stream_run && !s_stream_loop) s_stream_run = false;
    }
}

// ---------------------------------------------------------------------------
void net_begin(const Settings &s) {
    s_base = s.server_url;
    while (s_base.endsWith("/")) s_base.remove(s_base.length() - 1);
    s_token = s.token;
    s_cmd_q = xQueueCreate(8, sizeof(Command));
    xTaskCreatePinnedToCore(poll_task, "nbtv_poll", 6144, nullptr, 3, nullptr, 0);
    xTaskCreatePinnedToCore(stream_task, "nbtv_stream", 6144, nullptr, 4,
                            &s_stream_task, 0);
}

bool net_next_command(Command &out) {
    return xQueueReceive(s_cmd_q, &out, 0) == pdTRUE;
}

void net_set_status(const char *state, const char *id, uint16_t frame_seq,
                    int buffer_ms, int underruns, float speed) {
    portENTER_CRITICAL(&s_mux);
    strncpy(st_state, state, sizeof(st_state) - 1);
    strncpy(st_id, id, sizeof(st_id) - 1);
    st_seq = frame_seq; st_buf_ms = buffer_ms;
    st_underruns = underruns; st_speed = speed;
    portEXIT_CRITICAL(&s_mux);
}

void net_stream_start(const char *id, bool loop) {
    strncpy(s_stream_id, id, sizeof(s_stream_id) - 1);
    s_stream_loop = loop;
    fb_reset();
    s_stream_run = true;
}

void net_stream_stop() {
    s_stream_run = false;
    fb_reset();
}

bool net_stream_active() { return s_stream_active || s_stream_run; }
