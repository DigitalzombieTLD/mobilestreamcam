/*
 * spi_rx.c - receive JPEG frames from the HX6538 (SPI master) with a DMA SPI slave.
 *
 * Every queued transaction receives directly into the current writer frame slot (zero copy).
 * The slave driver ends a transaction when the master releases CS, so one HX frame normally
 * arrives as one transaction. If the HX splits a frame into several CS cycles (e.g. header
 * and payload separately) the data is accumulated until the length in the PTL header is reached.
 *
 * PTL framing (UNVERIFIED without hardware): [C0 5A][type][len LE32][payload...]
 */
#include <string.h>
#include "driver/spi_slave.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "config.h"
#include "frame_store.h"
#include "spi_rx.h"

static const char *TAG = "spi_rx";

static inline bool has_magic(const uint8_t *p)
{
    return p[0] == PTL_MAGIC0 && p[1] == PTL_MAGIC1;
}

static void log_bad(const uint8_t *p, uint32_t n, const char *why)
{
    static int budget = 8; /* only the first few, to help hardware bring-up */
    if (budget > 0) {
        budget--;
        ESP_LOGW(TAG, "bad frame (%s), %u bytes", why, (unsigned)n);
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, p, n > 32 ? 32 : n, ESP_LOG_WARN);
    }
}

static void spi_rx_task(void *arg)
{
    frame_slot_t *slot = frame_store_writer_slot();
    uint32_t fill = 0;
    uint32_t idle_logs = 0;

    for (;;) {
        /* DMA needs 4-byte aligned destination and length */
        uint32_t aligned = (fill + 3) & ~3u;
        spi_slave_transaction_t t = {
            .length = (slot->capacity - aligned) * 8,
            .rx_buffer = slot->data + aligned,
        };

        if (spi_slave_queue_trans(SPI2_HOST, &t, portMAX_DELAY) != ESP_OK) {
            ESP_LOGE(TAG, "queue failed");
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        spi_slave_transaction_t *done;
        esp_err_t err;
        while ((err = spi_slave_get_trans_result(SPI2_HOST, &done, pdMS_TO_TICKS(5000))) == ESP_ERR_TIMEOUT) {
            if ((idle_logs++ % 6) == 0) {
                ESP_LOGW(TAG, "no SPI data from the HX6538 yet (check wiring / HX firmware)");
            }
        }
        if (err != ESP_OK) {
            continue;
        }

        uint32_t rx = done->trans_len / 8;
        if (rx == 0) {
            continue;
        }
        if (aligned != fill) {
            memmove(slot->data + fill, slot->data + aligned, rx);
        }

        /* A new PTL header while an unfinished frame is pending: restart on the new frame */
        if (fill > 0 && rx >= PTL_HEADER_LEN && has_magic(slot->data + fill)) {
            frame_store_count_bad();
            memmove(slot->data, slot->data + fill, rx);
            fill = 0;
        }
        fill += rx;

        if (fill < PTL_HEADER_LEN) {
            continue; /* wait for more */
        }
        const uint8_t *h = slot->data;
        if (!has_magic(h)) {
            log_bad(h, fill, "no PTL magic");
            frame_store_count_bad();
            fill = 0;
            continue;
        }
        uint32_t len = h[3] | (h[4] << 8) | (h[5] << 16) | ((uint32_t)h[6] << 24);
        if (h[2] != PTL_TYPE_JPG || len < 4 || len > FRAME_MAX_BYTES) {
            log_bad(h, fill, "type/length");
            frame_store_count_bad();
            fill = 0;
            continue;
        }
        if (fill < PTL_HEADER_LEN + len) {
            continue; /* incomplete, wait for the rest */
        }
        const uint8_t *jpg = slot->data + PTL_HEADER_LEN;
        if (jpg[0] != 0xFF || jpg[1] != 0xD8) {
            log_bad(h, fill, "no JPEG SOI");
            frame_store_count_bad();
            fill = 0;
            continue;
        }

        frame_store_publish(PTL_HEADER_LEN, len, &slot); /* on drop, the same slot is reused */
        fill = 0;
    }
}

void spi_rx_start(void)
{
    spi_bus_config_t bus = {
        .mosi_io_num = SPI_PIN_MOSI,
        .miso_io_num = SPI_PIN_MISO,
        .sclk_io_num = SPI_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = FRAME_MAX_BYTES + 128,
    };
    spi_slave_interface_config_t slv = {
        .mode = SPI_MODE_NUM,
        .spics_io_num = SPI_PIN_CS,
        .queue_size = 2,
        .flags = 0,
    };
    ESP_ERROR_CHECK(spi_slave_initialize(SPI2_HOST, &bus, &slv, SPI_DMA_CH_AUTO));
    xTaskCreate(spi_rx_task, "spi_rx", 4096, NULL, configMAX_PRIORITIES - 2, NULL);
    ESP_LOGI(TAG, "SPI slave ready: SCLK=%d MOSI=%d MISO=%d CS=%d mode=%d",
             SPI_PIN_SCLK, SPI_PIN_MOSI, SPI_PIN_MISO, SPI_PIN_CS, SPI_MODE_NUM);
}
