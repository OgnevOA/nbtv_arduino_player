// Audio (NBTV signal) output via the Atomic Audio-3.5 Base ES8311 codec.
//
// Codec bring-up (ES8311 registers, IO-expander power, I2S setup) is delegated
// to M5Stack's proven M5EchoBase driver for this exact board.
//
// IMPORTANT: the ES8311 only accepts STANDARD sample rates (its clock table has
// no entry for 48000*speed), and M5's I2S is 16-bit STEREO. So we run the codec
// fixed at 48000 Hz and interleave mono -> stereo here. "Speed" (disc lock) is
// done by time-stretching the rendered frame (see nbtv_player.ino), not by
// changing the sample clock.
#pragma once

#include <stdint.h>

// Bring up the codec + I2S at the fixed native rate (48000 Hz). Returns success.
bool audio_begin();

// Blocking write of `count` int16 MONO samples (interleaved to stereo + sent to
// I2S DMA, which paces our render loop).
void audio_write(const int16_t *samples, int count);
