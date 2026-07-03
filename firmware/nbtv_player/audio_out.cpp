#include "audio_out.h"
#include "nbtv_config.h"

#include <Wire.h>
#include <esp_idf_version.h>
#include <M5EchoBase.h>

// The constructor differs across ESP-IDF versions bundled by the Arduino core.
#if (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0))
static M5EchoBase echo;
#else
static M5EchoBase echo(I2S_NUM_0);
#endif

// Interleave scratch: mono -> stereo. Sized for the largest stretched frame.
static const int MAX_STEREO_FRAMES = 5200;
static int16_t s_stereo[MAX_STEREO_FRAMES * 2];

bool audio_begin() {
    // Fixed STANDARD rate so the ES8311 clock table resolves; ATOM pin map.
    bool ok = echo.init(NBTV_BASE_RATE, PIN_I2C_SDA, PIN_I2C_SCL, PIN_I2S_DIN,
                        PIN_I2S_WS, PIN_I2S_DOUT, PIN_I2S_BCK, Wire);
    if (ok) echo.setSpeakerVolume(100);  // full-scale DAC swing; NBTV needs level
    return ok;
}

void audio_write(const int16_t *samples, int count) {
    if (count > MAX_STEREO_FRAMES) count = MAX_STEREO_FRAMES;
    for (int i = 0; i < count; ++i) {
        s_stereo[2 * i] = samples[i];       // left  = video
        s_stereo[2 * i + 1] = samples[i];   // right = mirror (mono codec)
    }
    echo.play((uint8_t *)s_stereo, count * 2 * (int)sizeof(int16_t));
}
