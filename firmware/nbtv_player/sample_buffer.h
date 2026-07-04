// Thread-safe ring buffer of int16 PCM samples (producer: net task; consumer:
// the audio output loop).
//
// The producer blocks when full, giving TCP backpressure that paces the radio
// stream to the device's real playback rate. The consumer never blocks: on
// underrun sb_read simply returns fewer samples and the output loop holds level.
#pragma once

#include <stdint.h>

void sb_init();
void sb_reset();

// Producer: write up to n samples; blocks up to timeout_ms waiting for room.
// Returns the number actually written (call again for any remainder).
int sb_push(const int16_t *src, int n, uint32_t timeout_ms);

// Consumer: read up to n samples; non-blocking. Returns the number read.
int sb_read(int16_t *dst, int n);

int sb_count();      // filled samples (for buffer_ms status)
int sb_capacity();   // total ring capacity in samples
