#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"

typedef struct {
    uint8_t *data;       /* DMA-capable buffer, receives the raw SPI transfer */
    uint32_t capacity;   /* bytes in data */
    uint32_t offset;     /* start of the JPEG inside data (after the PTL header) */
    uint32_t size;       /* JPEG size in bytes */
    uint32_t seq;        /* frame sequence number (0 = not published yet) */
    int refs;            /* readers currently using the slot */
    bool is_latest;
} frame_slot_t;

typedef struct {
    uint32_t frames;       /* complete frames received */
    uint32_t bad;          /* frames rejected (bad header / not a JPEG) */
    uint32_t dropped;      /* complete frames dropped because no slot was free */
    uint32_t last_size;
    uint16_t width, height;
    float fps;
} frame_stats_t;

void frame_store_init(void);

/* Writer side (spi_rx task) */
frame_slot_t *frame_store_writer_slot(void);
/* Publish the writer slot as the latest frame. On success *next receives the new writer slot.
 * Returns false (and keeps the same writer slot) if no free slot exists: the frame is dropped. */
bool frame_store_publish(uint32_t offset, uint32_t size, frame_slot_t **next);
void frame_store_count_bad(void);

/* Reader side (HTTP): wait until a frame newer than after_seq is available. Must be released. */
frame_slot_t *frame_store_acquire_latest(uint32_t after_seq, uint32_t timeout_ms);
void frame_store_release(frame_slot_t *slot);

void frame_store_get_stats(frame_stats_t *out);
