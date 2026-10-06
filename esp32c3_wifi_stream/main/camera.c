/*
 * camera.c - capture JPEG frames from the OV2640 and publish them to the frame store.
 *
 * The driver frame buffer is copied into a frame store slot (PSRAM) and returned right away, so a
 * slow network client can never keep a camera buffer. CAMERA_GRAB_LATEST always yields the newest frame.
 */
#include <stdatomic.h>
#include <string.h>
#include "esp_camera.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "config.h"
#include "frame_store.h"
#include "camera.h"

static const char *TAG = "camera";

#define CAMERA_TASK_STACK_BYTES 4096
#define LOCK_TIMEOUT_MS         3000   /* max wait for the camera task to release the camera */
#define VERIFY_FRAMES           6      /* frames inspected after a change (old-size frames may still be queued) */
#define NVS_NAMESPACE           "camcfg"
#define NVS_KEY_PRESET          "preset"

/* id, name, frame size, width, height, JPEG quality, max frame bytes */
static const camera_preset_t s_presets[CAMERA_PRESET_COUNT] = {
    { 1, "SVGA", FRAMESIZE_SVGA,  800,  600, 10, 128 * 1024 },
    { 2, "VGA",  FRAMESIZE_VGA,   640,  480, 12,  96 * 1024 },
    { 3, "XGA",  FRAMESIZE_XGA,  1024,  768, 12, 160 * 1024 },
    { 4, "SXGA", FRAMESIZE_SXGA, 1280, 1024, 14, 224 * 1024 },
    { 5, "UXGA", FRAMESIZE_UXGA, 1600, 1200, 16, 320 * 1024 },
};

static SemaphoreHandle_t s_lock;          /* held by camera_task around every capture, and by camera_set_preset */
static atomic_int s_waiters;              /* camera_set_preset callers waiting for s_lock */
#define DEFAULT_PRESET_ID ((CAM_PRESET >= 1 && CAM_PRESET <= CAMERA_PRESET_COUNT) ? CAM_PRESET : 1)
static atomic_int s_active = DEFAULT_PRESET_ID;
static bool s_ready;
static bool s_persist;

const camera_preset_t *camera_preset_by_id(int id)
{
    return (id >= 1 && id <= CAMERA_PRESET_COUNT) ? &s_presets[id - 1] : NULL;
}

const camera_preset_t *camera_active_preset(void)
{
    return camera_preset_by_id(atomic_load(&s_active));
}

bool camera_settings_persistent(void)
{
    return s_persist;
}

static int load_stored_preset(void)
{
    int id = DEFAULT_PRESET_ID;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
        s_persist = true;   /* namespace is created by the first save */
    } else {
        ESP_LOGW(TAG, "NVS unavailable (0x%x): selection will not be persisted", err);
    }
    if (err == ESP_OK) {
        uint8_t v;
        if (nvs_get_u8(h, NVS_KEY_PRESET, &v) == ESP_OK && camera_preset_by_id(v)) {
            id = v;
        } else {
            ESP_LOGW(TAG, "no valid stored preset, using %d", id);
        }
        nvs_close(h);
    }
    return id;
}

static esp_err_t save_preset(int id)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(h, NVS_KEY_PRESET, (uint8_t)id);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/* Sensor settings only; the caller holds s_lock */
static bool sensor_apply(const camera_preset_t *p)
{
    sensor_t *sen = esp_camera_sensor_get();
    return sen && sen->set_framesize && sen->set_quality &&
           sen->set_framesize(sen, (framesize_t)p->frame_size) == 0 &&
           sen->set_quality(sen, p->quality) == 0;
}

/* Wait until the driver delivers frames of the preset's size (frames captured before the change may still be queued).
 * The caller holds s_lock; frames are returned immediately. */
static bool sensor_settled(const camera_preset_t *p)
{
    for (int i = 0; i < VERIFY_FRAMES; i++) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            return false;
        }
        uint16_t w = 0, h = 0;
        if (fb->format == PIXFORMAT_JPEG) {
            frame_store_parse_jpeg_size(fb->buf, fb->len, &w, &h);
        }
        esp_camera_fb_return(fb);
        if (w == p->width && h == p->height) {
            return true;
        }
    }
    return false;
}

esp_err_t camera_set_preset(int id, bool *saved)
{
    const camera_preset_t *want = camera_preset_by_id(id);
    *saved = false;
    if (!want) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    atomic_fetch_add(&s_waiters, 1);
    BaseType_t got = xSemaphoreTake(s_lock, pdMS_TO_TICKS(LOCK_TIMEOUT_MS));
    atomic_fetch_sub(&s_waiters, 1);
    if (got != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    const camera_preset_t *prev = camera_active_preset();
    esp_err_t ret = ESP_OK;
    if (want != prev) {
        if (!sensor_apply(want)) {
            ret = ESP_FAIL;
        } else if (!sensor_settled(want)) {
            ret = ESP_ERR_INVALID_RESPONSE;
        }
        if (ret == ESP_OK) {
            atomic_store(&s_active, want->id);
        } else {
            ESP_LOGE(TAG, "preset %d (%s) failed: 0x%x, restoring %s", want->id, want->name, ret, prev->name);
            if (!sensor_apply(prev) || !sensor_settled(prev)) {
                ESP_LOGE(TAG, "restoring preset %d failed too", prev->id);
            }
        }
    }
    xSemaphoreGive(s_lock);

    if (ret == ESP_OK) {
        if (!s_persist) {
            ESP_LOGW(TAG, "preset %d active, NVS unavailable: not saved", want->id);
        } else {
            esp_err_t e = save_preset(want->id);
            *saved = (e == ESP_OK);
            if (e != ESP_OK) {
                ESP_LOGE(TAG, "saving preset %d failed: 0x%x", want->id, e);
            }
        }
        ESP_LOGI(TAG, "preset %d (%s %ux%u q%u) active, saved %d", want->id, want->name, want->width, want->height,
                 want->quality, (int)*saved);
    }
    return ret;
}

static void camera_task(void *arg)
{
    frame_slot_t *slot = frame_store_writer_slot();
    for (;;) {
        if (atomic_load(&s_waiters) > 0) {
            vTaskDelay(pdMS_TO_TICKS(10));   /* let camera_set_preset take the lock */
        }
        /* The lock covers only the capture and the copy into the slot, never any network send. */
        xSemaphoreTake(s_lock, portMAX_DELAY);
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            xSemaphoreGive(s_lock);
            ESP_LOGW(TAG, "frame capture failed");
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        uint32_t limit = camera_active_preset()->max_bytes;
        if (limit > slot->capacity) {
            limit = slot->capacity;
        }
        if (fb->format != PIXFORMAT_JPEG || fb->len < 4 || fb->len > limit ||
            fb->buf[0] != 0xFF || fb->buf[1] != 0xD8) {
            ESP_LOGW(TAG, "bad frame: format %d, %u bytes", (int)fb->format, (unsigned)fb->len);
            frame_store_count_bad();
            esp_camera_fb_return(fb);
            xSemaphoreGive(s_lock);
            continue;
        }
        memcpy(slot->data, fb->buf, fb->len);
        uint32_t len = fb->len;
        esp_camera_fb_return(fb);
        xSemaphoreGive(s_lock);
        frame_store_publish(0, len, &slot); /* on drop, the same slot is reused */
    }
}

void camera_start(void)
{
    const camera_preset_t *preset = camera_preset_by_id(load_stored_preset());
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
        .frame_size = CAM_INIT_FRAME_SIZE,  /* buffers for the largest size; the stored preset is applied below */
        .jpeg_quality = preset->quality,
        .fb_count = CAM_FB_COUNT,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_LATEST,
    };
    esp_err_t err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "camera init failed: 0x%x (check the pin map / camera ribbon cable / PSRAM)", err);
        return; /* WiFi and HTTP still start; /status shows 0 frames */
    }
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock || !sensor_apply(preset)) {
        ESP_LOGE(TAG, "applying preset %d (%s) failed: camera disabled", preset->id, preset->name);
        esp_camera_deinit();
        return;
    }
    atomic_store(&s_active, preset->id);
    s_ready = true;
    xTaskCreate(camera_task, "camera", CAMERA_TASK_STACK_BYTES, NULL, 5, NULL);
    ESP_LOGI(TAG, "OV2640 ready, preset %d (%s %ux%u), JPEG quality %u, persistence %s", preset->id, preset->name,
             preset->width, preset->height, preset->quality, s_persist ? "on" : "off");
}
