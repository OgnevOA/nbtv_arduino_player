// NBTVA 32-line timing + M5 Atom Lite / Atomic Audio-3.5 pin map.
//
// In the radio architecture the device just plays a PCM stream, so it only
// needs the base sample rate / frame rate. The full signal (sync + levels +
// band-limit) is synthesized on the server (server/app/nbtv.py + render.py).
#pragma once

#include <stdint.h>

// --- NBTVA 32-line timing ----------------------------------------------------
static const int   NBTV_BASE_RATE   = 48000;  // I2S rate at speed 1.0
static const float NBTV_BASE_FPS    = 12.5f;  // 750 rpm
static const int   NBTV_LINES       = 32;     // lines per frame
static const int   NBTV_SPL         = 120;    // samples per line
static const int   NBTV_SAMPLES     = NBTV_LINES * NBTV_SPL;   // 3840 / frame

// --- Hardware pin map: M5 Atom Lite (ESP32) + Atomic Audio-3.5 Base ----------
// From M5EchoBase ATOM (CONFIG_IDF_TARGET_ESP32) init:
//   I2C SDA=25 SCL=21 ; I2S DIN=23 WS=19 DOUT=22 BCK=33 ; no external MCLK.
static const int PIN_I2C_SDA = 25;
static const int PIN_I2C_SCL = 21;
static const int PIN_I2S_DIN = 23;   // ASDOUT (mic; unused here)
static const int PIN_I2S_WS  = 19;   // LRCK
static const int PIN_I2S_DOUT = 22;  // DSDIN (DAC)
static const int PIN_I2S_BCK = 33;   // SCLK
static const int PIN_BUTTON  = 39;   // Atom Lite top button (active low)

static const uint8_t ES8311_ADDR = 0x18;
static const uint8_t IO_EXP_ADDR  = 0x43;  // PI4IOE5V6408 (codec/amp power)
