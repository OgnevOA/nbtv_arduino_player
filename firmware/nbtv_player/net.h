// Networking: WiFi, command long-poll, status POST, and the pixel-frame stream.
#pragma once

#include <Arduino.h>
#include "settings.h"

enum CmdType {
    CMD_NONE, CMD_PLAY, CMD_PRELOAD, CMD_STOP, CMD_FREEZE,
    CMD_LOOP, CMD_SPEED, CMD_REBOOT, CMD_TESTCARD,
};

struct Command {
    CmdType type = CMD_NONE;
    char    id[24] = {0};
    bool    flag = false;   // loop on/off
    float   value = 0.0f;   // speed value
};

// Connect to WiFi; returns true on success within timeout.
bool net_wifi_connect(const Settings &s, uint32_t timeout_ms);
bool net_wifi_ok();

// Start background tasks (command long-poll + periodic status).
void net_begin(const Settings &s);

// Main-loop consumption of the next queued command (non-blocking).
bool net_next_command(Command &out);

// Values reported in the next /status POST.
void net_set_status(const char *state, const char *id, uint16_t frame_seq,
                    int buffer_ms, int underruns, float speed);

// Pixel-frame stream control (frames land in the frame buffer).
void net_stream_start(const char *id, bool loop);
void net_stream_stop();
bool net_stream_active();
