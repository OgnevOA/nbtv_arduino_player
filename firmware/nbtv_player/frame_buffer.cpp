#include "frame_buffer.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static const int SLOTS = 12;  // ~0.96 s at 12.5 fps; 12 * 1536 B = 18 KB

static uint8_t s_buf[SLOTS][NBTV_FRAME_BYTES];
static int s_head = 0, s_tail = 0;
static SemaphoreHandle_t s_free;    // counts empty slots
static SemaphoreHandle_t s_filled;  // counts ready frames
static SemaphoreHandle_t s_mutex;

void fb_init() {
    s_free = xSemaphoreCreateCounting(SLOTS, SLOTS);
    s_filled = xSemaphoreCreateCounting(SLOTS, 0);
    s_mutex = xSemaphoreCreateMutex();
    s_head = s_tail = 0;
}

void fb_reset() {
    // Drain everything back to empty.
    while (xSemaphoreTake(s_filled, 0) == pdTRUE) {
        xSemaphoreGive(s_free);
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_head = s_tail = 0;
    xSemaphoreGive(s_mutex);
}

bool fb_push(const uint8_t *frame, uint32_t timeout_ms) {
    if (xSemaphoreTake(s_free, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memcpy(s_buf[s_tail], frame, NBTV_FRAME_BYTES);
    s_tail = (s_tail + 1) % SLOTS;
    xSemaphoreGive(s_mutex);
    xSemaphoreGive(s_filled);
    return true;
}

bool fb_pop(uint8_t *frame) {
    if (xSemaphoreTake(s_filled, 0) != pdTRUE) return false;  // underrun
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memcpy(frame, s_buf[s_head], NBTV_FRAME_BYTES);
    s_head = (s_head + 1) % SLOTS;
    xSemaphoreGive(s_mutex);
    xSemaphoreGive(s_free);
    return true;
}

int fb_count() {
    return (int)uxSemaphoreGetCount(s_filled);
}
