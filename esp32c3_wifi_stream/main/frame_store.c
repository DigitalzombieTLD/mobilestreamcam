#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "config.h"
#include "frame_store.h"

static const char *TAG = "frame_store";

#define SLOT_CAPACITY  (((FRAME_MAX_BYTES + PTL_HEADER_LEN + 64) + 3) & ~3)

static frame_slot_t s_slots[FRAME_SLOT_COUNT];
static frame_slot_t *s_writer;
static SemaphoreHandle_t s_lock;
static uint32_t s_seq;
static frame_stats_t s_stats;
static int64_t s_win_start_us;
static uint32_t s_win_frames;

static void parse_jpeg_size(const uint8_t *d, uint32_t len, uint16_t *w, uint16_t *h)
{
    uint32_t i = 2;
    while (i + 9 < len && i < 2048) {
        if (d[i] != 0xFF) { i++; continue; }
        uint8_t m = d[i + 1];
        if (m >= 0xC0 && m <= 0xC2) {
            *h = (d[i + 5] << 8) | d[i + 6];
            *w = (d[i + 7] << 8) | d[i + 8];
            return;
        }
        if (m == 0xFF) { i++; continue; }
        if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { i += 2; continue; }
        uint32_t seg = (d[i + 2] << 8) | d[i + 3];
        if (seg < 2) {
            return;
        }
        i += 2 + seg;
    }
}

void frame_store_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    for (int i = 0; i < FRAME_SLOT_COUNT; i++) {
        s_slots[i].data = heap_caps_malloc(SLOT_CAPACITY, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!s_slots[i].data) {
            ESP_LOGE(TAG, "slot %d alloc failed (%d B) - lower FRAME_SLOT_COUNT/FRAME_MAX_BYTES", i, SLOT_CAPACITY);
            abort();
        }
        s_slots[i].capacity = SLOT_CAPACITY;
    }
    s_writer = &s_slots[0];
    s_win_start_us = esp_timer_get_time();
    ESP_LOGI(TAG, "%d slots x %d B, free heap %u", FRAME_SLOT_COUNT, SLOT_CAPACITY,
             (unsigned)esp_get_free_heap_size());
}

frame_slot_t *frame_store_writer_slot(void)
{
    return s_writer;
}

bool frame_store_publish(uint32_t offset, uint32_t size, frame_slot_t **next)
{
    bool ok = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);

    frame_slot_t *free_slot = NULL;
    for (int i = 0; i < FRAME_SLOT_COUNT; i++) {
        frame_slot_t *s = &s_slots[i];
        if (s != s_writer && !s->is_latest && s->refs == 0) {
            free_slot = s;
            break;
        }
    }

    if (free_slot) {
        for (int i = 0; i < FRAME_SLOT_COUNT; i++) {
            s_slots[i].is_latest = false;
        }
        s_writer->offset = offset;
        s_writer->size = size;
        s_writer->seq = ++s_seq;
        s_writer->is_latest = true;
        parse_jpeg_size(s_writer->data + offset, size, &s_stats.width, &s_stats.height);

        s_stats.frames++;
        s_stats.last_size = size;
        s_win_frames++;
        int64_t now = esp_timer_get_time();
        if (now - s_win_start_us >= 1000000) {
            s_stats.fps = s_win_frames * 1e6f / (float)(now - s_win_start_us);
            s_win_frames = 0;
            s_win_start_us = now;
        }
        s_writer = free_slot;
        ok = true;
    } else {
        s_stats.dropped++;
    }
    *next = s_writer;
    xSemaphoreGive(s_lock);
    return ok;
}

void frame_store_count_bad(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_stats.bad++;
    xSemaphoreGive(s_lock);
}

frame_slot_t *frame_store_acquire_latest(uint32_t after_seq, uint32_t timeout_ms)
{
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    for (;;) {
        frame_slot_t *found = NULL;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (int i = 0; i < FRAME_SLOT_COUNT; i++) {
            if (s_slots[i].is_latest && s_slots[i].seq > after_seq) {
                s_slots[i].refs++;
                found = &s_slots[i];
                break;
            }
        }
        xSemaphoreGive(s_lock);
        if (found) {
            return found;
        }
        if (esp_timer_get_time() >= deadline) {
            return NULL;
        }
        vTaskDelay(pdMS_TO_TICKS(4));
    }
}

void frame_store_release(frame_slot_t *slot)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (slot->refs > 0) {
        slot->refs--;
    }
    xSemaphoreGive(s_lock);
}

void frame_store_get_stats(frame_stats_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_stats;
    /* fps decays to 0 if no frames arrive */
    if (esp_timer_get_time() - s_win_start_us > 3000000) {
        out->fps = 0;
    }
    xSemaphoreGive(s_lock);
}
