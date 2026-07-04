// Networking: WiFi, device command long-poll, status POST, and the PCM radio
// stream (samples land in the sample ring buffer).
#pragma once

#include <Arduino.h>
#include "settings.h"

enum CmdType { CMD_NONE, CMD_SPEED, CMD_INVERT, CMD_REBOOT };

struct Command {
    CmdType type = CMD_NONE;
    float   value = 0.0f;   // speed value (CMD_SPEED)
};

// Connect to WiFi; returns true on success within timeout.
bool net_wifi_connect(const Settings &s, uint32_t timeout_ms);
bool net_wifi_ok();

// Start background tasks (radio stream + command long-poll + status).
void net_begin(const Settings &s);

// Main-loop consumption of the next queued command (non-blocking).
bool net_next_command(Command &out);

// Values reported in the next /status POST.
void net_set_status(const char *state, int buffer_ms, int underruns, float speed);

// True while the radio stream is connected and delivering PCM.
bool net_stream_active();
