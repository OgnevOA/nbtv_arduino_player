// NBTV waveform synthesis: pixel frame (1536 bytes) -> 3840 int16 samples.
//
// This is the on-device port of mtv.py's frames_to_signal(): it maps 8-bit
// luminance to signal levels, inserts blacker-than-black line sync, omits the
// last line's sync as the missing-pulse frame marker, and interpolates each
// line's 48 transmitted rows up to 114 active samples.
#pragma once

#include <stdint.h>

// Render one pixel frame into `out` (must hold NBTV_SAMPLES int16 samples).
// `frame` is NBTV_FRAME_BYTES of 8-bit grey in scan order (line-major).
// `invert` negates the composite for sync-positive kits.
void nbtv_render_frame(const uint8_t *frame, int16_t *out, bool invert);

// Apply the full-signal ~10 kHz band-limit (on-device equivalent of the PC
// --lowpass) to `count` composite samples in place. Stateful across calls.
void nbtv_bandlimit(int16_t *samples, int count);

// Fill `frame` (NBTV_FRAME_BYTES) with a built-in test card in scan order.
void nbtv_test_card(uint8_t *frame);
