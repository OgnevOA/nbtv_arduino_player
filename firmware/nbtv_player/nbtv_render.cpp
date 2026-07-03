#include "nbtv_render.h"
#include "nbtv_config.h"

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static inline int16_t clamp16(float s) {
    if (s > 32767.0f) s = 32767.0f;
    if (s < -32768.0f) s = -32768.0f;
    return (int16_t)lroundf(s);
}

void nbtv_render_frame(const uint8_t *frame, int16_t *out, bool invert) {
    const float scale = (float)(NBTV_ROWS_TX - 1) / (float)(NBTV_ACTIVE_SPL - 1);
    for (int l = 0; l < NBTV_LINES; ++l) {
        const uint8_t *line = frame + l * NBTV_ROWS_TX;  // 48 rows, bottom->top
        int16_t *dst = out + l * NBTV_SPL;

        // Active region: interpolate 48 transmitted rows -> 114 samples.
        for (int i = 0; i < NBTV_ACTIVE_SPL; ++i) {
            float pos = i * scale;              // 0 .. 47
            int i0 = (int)pos;
            int i1 = (i0 + 1 < NBTV_ROWS_TX) ? i0 + 1 : NBTV_ROWS_TX - 1;
            float frac = pos - i0;
            float v = line[i0] * (1.0f - frac) + line[i1] * frac;  // 0..255
            dst[i] = clamp16(v * (float)NBTV_WHITE / 255.0f);
        }

        // Sync region: blacker-than-black, except the last line (frame sync =
        // missing pulse, held at black). Band-limiting (below) softens these
        // edges just like the PC --lowpass did.
        int16_t syncv = (l == NBTV_LINES - 1) ? NBTV_BLACK : NBTV_SYNC;
        for (int i = NBTV_ACTIVE_SPL; i < NBTV_SPL; ++i) dst[i] = syncv;

        if (invert) {
            for (int i = 0; i < NBTV_SPL; ++i) dst[i] = -dst[i];
        }
    }
}

// --- full-signal band-limit (the on-device equivalent of mtv.py --lowpass) ---
// A linear-phase windowed-sinc FIR (~10 kHz) applied to the WHOLE composite,
// sync included. This kills the out-of-band ringing from sharp white<->sync
// edges that makes a real sync separator false-trigger and never fully lock.
// Stateful across frames (carries a tail) so there is no seam every frame.
static const int   BL_TAPS   = 63;
static const float BL_CUTOFF = 10000.0f;
static float s_bl[BL_TAPS];
static float s_ring[BL_TAPS];   // circular delay line, persists across frames
static int   s_widx = 0;        // write index of the newest sample
static bool  s_bl_ready = false;

static void bl_build() {
    float fc = BL_CUTOFF / (float)NBTV_BASE_RATE;
    float sum = 0.0f;
    for (int i = 0; i < BL_TAPS; ++i) {
        float m = i - (BL_TAPS - 1) / 2.0f;
        float s = (m == 0.0f) ? 2.0f * fc
                              : sinf(2.0f * (float)M_PI * fc * m) / ((float)M_PI * m);
        float w = 0.42f - 0.5f * cosf(2.0f * (float)M_PI * i / (BL_TAPS - 1))
                        + 0.08f * cosf(4.0f * (float)M_PI * i / (BL_TAPS - 1));
        s_bl[i] = s * w;
        sum += s_bl[i];
    }
    for (int i = 0; i < BL_TAPS; ++i) s_bl[i] /= sum;   // unity DC gain
    for (int i = 0; i < BL_TAPS; ++i) s_ring[i] = 0;
    s_widx = 0;
    s_bl_ready = true;
}

void nbtv_bandlimit(int16_t *samples, int count) {
    if (!s_bl_ready) bl_build();
    for (int i = 0; i < count; ++i) {
        s_ring[s_widx] = (float)samples[i];       // insert newest (in-place safe)
        float acc = 0.0f;
        int idx = s_widx;
        for (int k = 0; k < BL_TAPS; ++k) {       // coef[0]*newest .. coef[N]*oldest
            acc += s_bl[k] * s_ring[idx];
            if (--idx < 0) idx = BL_TAPS - 1;
        }
        samples[i] = clamp16(acc);
        if (++s_widx >= BL_TAPS) s_widx = 0;
    }
}

// Smooth grey test card: a horizontal brightness wedge, kept low in
// high-frequency content so the sync separator has an easy time.
void nbtv_test_card(uint8_t *frame) {
    for (int l = 0; l < NBTV_LINES; ++l) {
        uint8_t *line = frame + l * NBTV_ROWS_TX;  // 48 rows, bottom->top
        for (int r = 0; r < NBTV_ROWS_TX; ++r) {
            float wedge = (float)r / (float)(NBTV_ROWS_TX - 1);   // 0..1 up column
            float band  = 0.5f + 0.5f * sinf((float)l / NBTV_LINES * 2.0f * (float)M_PI);
            float v = 0.15f + 0.7f * (0.6f * wedge + 0.4f * band);
            if (v < 0.0f) v = 0.0f; if (v > 1.0f) v = 1.0f;
            line[r] = (uint8_t)lroundf(v * 255.0f);
        }
    }
}
