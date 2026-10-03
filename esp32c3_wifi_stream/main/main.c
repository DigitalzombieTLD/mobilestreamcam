#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "config.h"
#include "frame_store.h"
#include "http_stream.h"
#include "spi_rx.h"
#include "wifi_sta.h"

static const char *TAG = "main";

static void on_got_ip(void)
{
    http_stream_start(); /* no-op if already running */
}

static unsigned stack_hwm(const char *name)
{
    TaskHandle_t t = xTaskGetHandle(name);
    return t ? (unsigned)uxTaskGetStackHighWaterMark(t) : 0;
}

/* Once per second: log the heap every HEALTH_LOG_EVERY_S, and act if the heap stays low
 * (free heap or largest free block below LOW_HEAP_BYTES): first drop the stream client,
 * then restart the chip so the camera recovers by itself. Runs in the esp_timer task. */
static void health_cb(void *arg)
{
    static uint32_t tick, low_s;
    tick++;
    size_t free_b = esp_get_free_heap_size();
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);

    if ((tick % HEALTH_LOG_EVERY_S) == 0) {
        ESP_LOGI(TAG, "heap free %u, min %u, largest block %u | clients %d | stack hwm httpd %u stream %u spi_rx %u",
                 (unsigned)free_b, (unsigned)esp_get_minimum_free_heap_size(), (unsigned)largest,
                 http_stream_clients(), stack_hwm("httpd"), stack_hwm("stream"), stack_hwm("spi_rx"));
    }

    if (free_b < LOW_HEAP_BYTES || largest < LOW_HEAP_BYTES) {
        low_s++;
        if (low_s == LOW_HEAP_DROP_CLIENT_S) {
            ESP_LOGW(TAG, "heap low (free %u, largest %u): dropping stream client", (unsigned)free_b, (unsigned)largest);
            http_stream_drop_client();
        } else if (low_s >= LOW_HEAP_RESTART_S) {
            ESP_LOGE(TAG, "heap still low (free %u, largest %u): restarting", (unsigned)free_b, (unsigned)largest);
            esp_restart();
        }
    } else {
        low_s = 0;
    }
}

void app_main(void)
{
    esp_log_level_set("wifi", ESP_LOG_WARN);
    ESP_LOGI(TAG, "mobilestreamcam ESP32-C3 WiFi MJPEG relay");
    ESP_LOGI(TAG, "free heap at boot: %u", (unsigned)esp_get_free_heap_size());
    frame_store_init();
    spi_rx_start();
    wifi_sta_start(on_got_ip);

    const esp_timer_create_args_t targs = { .callback = health_cb, .name = "health" };
    esp_timer_handle_t timer;
    ESP_ERROR_CHECK(esp_timer_create(&targs, &timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer, HEALTH_PERIOD_MS * 1000ULL));
    ESP_LOGI(TAG, "free heap after init: %u", (unsigned)esp_get_free_heap_size());
}
