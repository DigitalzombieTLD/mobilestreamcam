/*
 * camera.c - capture JPEG frames from the OV2640 and publish them to the frame store.
 *
 * The driver frame buffer is copied into a frame store slot (PSRAM) and returned right away, so a
 * slow network client can never keep a camera buffer. CAMERA_GRAB_LATEST always yields the newest frame.
 */
#include <string.h>
#include "esp_camera.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "config.h"
#include "frame_store.h"
#include "camera.h"

static const char *TAG = "camera";

#define CAMERA_TASK_STACK_BYTES 4096

static void camera_task(void *arg)
{
    frame_slot_t *slot = frame_store_writer_slot();
    for (;;) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGW(TAG, "frame capture failed");
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (fb->format != PIXFORMAT_JPEG || fb->len < 4 || fb->len > slot->capacity ||
            fb->buf[0] != 0xFF || fb->buf[1] != 0xD8) {
            ESP_LOGW(TAG, "bad frame: format %d, %u bytes", (int)fb->format, (unsigned)fb->len);
            frame_store_count_bad();
            esp_camera_fb_return(fb);
            continue;
        }
        memcpy(slot->data, fb->buf, fb->len);
        uint32_t len = fb->len;
        esp_camera_fb_return(fb);
        frame_store_publish(0, len, &slot); /* on drop, the same slot is reused */
    }
}

void camera_start(void)
{
    camera_config_t cfg = {
        .pin_pwdn = CAM_PIN_PWDN,
        .pin_reset = CAM_PIN_RESET,
        .pin_xclk = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SIOD,
        .pin_sccb_scl = CAM_PIN_SIOC,
        .pin_d7 = CAM_PIN_D7,
        .pin_d6 = CAM_PIN_D6,
        .pin_d5 = CAM_PIN_D5,
        .pin_d4 = CAM_PIN_D4,
        .pin_d3 = CAM_PIN_D3,
        .pin_d2 = CAM_PIN_D2,
        .pin_d1 = CAM_PIN_D1,
        .pin_d0 = CAM_PIN_D0,
        .pin_vsync = CAM_PIN_VSYNC,
        .pin_href = CAM_PIN_HREF,
        .pin_pclk = CAM_PIN_PCLK,
        .xclk_freq_hz = CAM_XCLK_FREQ_HZ,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_JPEG,
        .frame_size = CAM_FRAME_SIZE,
        .jpeg_quality = CAM_JPEG_QUALITY,
        .fb_count = CAM_FB_COUNT,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_LATEST,
    };
    esp_err_t err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "camera init failed: 0x%x (check the pin map / camera ribbon cable / PSRAM)", err);
        return; /* WiFi and HTTP still start; /status shows 0 frames */
    }
    xTaskCreate(camera_task, "camera", CAMERA_TASK_STACK_BYTES, NULL, 5, NULL);
    ESP_LOGI(TAG, "OV2640 ready, JPEG frame size %d, quality %d", (int)CAM_FRAME_SIZE, CAM_JPEG_QUALITY);
}
