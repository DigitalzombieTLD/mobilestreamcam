#include "esp_log.h"
#include "nvs_flash.h"
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

void app_main(void)
{
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        r = nvs_flash_init();
    }
    ESP_ERROR_CHECK(r);

    ESP_LOGI(TAG, "mobilestreamcam ESP32-C3 WiFi MJPEG relay");
    frame_store_init();
    spi_rx_start();
    wifi_sta_start(on_got_ip);
}
