// Thread-safe ring buffer of pixel frames (producer: net task; consumer: output).
//
// The producer blocks when full, giving TCP backpressure that paces the whole
// stream. The consumer never blocks forever: on underrun it reuses the last
// frame so the NBTV sync output never stops.
#pragma once

#include <stdint.h>
#include "nbtv_config.h"

void fb_init();
void fb_reset();                         // drop all buffered frames

// Producer: copy one NBTV_FRAME_BYTES frame in; blocks up to timeout_ms if full.
bool fb_push(const uint8_t *frame, uint32_t timeout_ms);

// Consumer: copy the next frame out; returns false immediately if empty.
bool fb_pop(uint8_t *frame);

int  fb_count();                         // filled slots (for buffer_ms status)
