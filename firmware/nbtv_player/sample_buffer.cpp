#include "sample_buffer.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// 24000 samples = 0.5 s at 48 kHz = 48 KB. Enough to ride out WiFi jitter
// without eating too much DRAM.
static const int CAP = 24000;

static int16_t s_buf[CAP];
static volatile int s_head = 0;   // read index
static volatile int s_tail = 0;   // write index
static volatile int s_count = 0;  // filled samples
static SemaphoreHandle_t s_mutex;

void sb_init() {
    s_mutex = xSemaphoreCreateMutex();
    s_head = s_tail = s_count = 0;
}

void sb_reset() {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_head = s_tail = s_count = 0;
    xSemaphoreGive(s_mutex);
}

int sb_push(const int16_t *src, int n, uint32_t timeout_ms) {
    uint32_t deadline = millis() + timeout_ms;
    for (;;) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        int free = CAP - s_count;
        int w = n < free ? n : free;
        for (int i = 0; i < w; ++i) {
            s_buf[s_tail] = src[i];
            s_tail = (s_tail + 1) % CAP;
        }
        s_count += w;
        xSemaphoreGive(s_mutex);
        if (w > 0 || timeout_ms == 0) return w;
        if (millis() > deadline) return 0;
        delay(2);  // full: wait for the consumer to drain (backpressure)
    }
}

int sb_read(int16_t *dst, int n) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int r = n < s_count ? n : s_count;
    for (int i = 0; i < r; ++i) {
        dst[i] = s_buf[s_head];
        s_head = (s_head + 1) % CAP;
    }
    s_count -= r;
    xSemaphoreGive(s_mutex);
    return r;
}

int sb_count() { return s_count; }
int sb_capacity() { return CAP; }
