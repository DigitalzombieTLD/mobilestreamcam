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
#define NVS_KEY_RES             "res"
#define NVS_KEY_QUALITY         "quality"
#define NVS_KEY_PRESET          "preset"   /* legacy combined preset id, read only for migration */

/* id (stable, stored in NVS; 1-5 match the former preset ids), name, frame size, width, height, max frame bytes
 * (limits are generous because quality is independent; frame slots cap them at FRAME_MAX_BYTES anyway) */
static const camera_resolution_t s_resolutions[CAMERA_RESOLUTION_COUNT] = {
    { 6, "QVGA",  FRAMESIZE_QVGA,   320,  240,  64 * 1024 },
    { 2, "VGA",   FRAMESIZE_VGA,    640,  480, 128 * 1024 },
    { 1, "SVGA",  FRAMESIZE_SVGA,   800,  600, 192 * 1024 },
    { 3, "XGA",   FRAMESIZE_XGA,   1024,  768, 256 * 1024 },
    { 4, "SXGA",  FRAMESIZE_SXGA,  1280, 1024, 320 * 1024 },
    { 5, "UXGA",  FRAMESIZE_UXGA,  1600, 1200, 320 * 1024 },
};

/* former combined presets 1..5: resolution id, quality */
static const uint8_t s_legacy[5][2] = { { 1, 10 }, { 2, 12 }, { 3, 12 }, { 4, 14 }, { 5, 16 } };

static SemaphoreHandle_t s_lock;          /* held by camera_task around every capture, and by camera_set_settings */
static atomic_int s_waiters;              /* camera_set_settings callers waiting for s_lock */
#define DEFAULT_RES_ID  (camera_resolution_by_id(CAM_RESOLUTION) ? CAM_RESOLUTION : 2)
#define DEFAULT_QUALITY (camera_quality_valid(CAM_JPEG_QUALITY) ? CAM_JPEG_QUALITY : 12)
static atomic_uint s_active = ((unsigned)2 << 8) | 12;              /* (resolution id << 8) | quality: one word, so readers always see a coherent pair */
static bool s_ready;
static bool s_persist;

const camera_resolution_t *camera_resolution_at(int index)
{
    return (index >= 0 && index < CAMERA_RESOLUTION_COUNT) ? &s_resolutions[index] : NULL;
}

const camera_resolution_t *camera_resolution_by_id(int id)
{
    for (int i = 0; i < CAMERA_RESOLUTION_COUNT; i++) {
        if (s_resolutions[i].id == id) {
            return &s_resolutions[i];
        }
    }
    return NULL;
}

bool camera_quality_valid(int quality)
{
    return quality >= CAMERA_QUALITY_MIN && quality <= CAMERA_QUALITY_MAX;
}

static camera_settings_t unpack(unsigned v)
{
    camera_settings_t s = { camera_resolution_by_id(v >> 8), v & 0xFF };
    if (!s.resolution || !camera_quality_valid(s.quality)) {   /* cannot happen: only validated pairs are stored */
        s.resolution = camera_resolution_by_id(2);
        s.quality = 12;
    }
    return s;
}

void camera_get_settings(camera_settings_t *out)
{
    *out = unpack(atomic_load(&s_active));
}

bool camera_settings_persistent(void)
{
    return s_persist;
}

static camera_settings_t load_stored_settings(void)
{
    camera_settings_t s = { camera_resolution_by_id(DEFAULT_RES_ID), DEFAULT_QUALITY };
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
        s_persist = true;   /* namespace is created by the first save */
    } else {
        ESP_LOGW(TAG, "NVS unavailable (0x%x): selection will not be persisted", err);
    }
    if (err == ESP_OK) {
        uint8_t r, q, legacy;
        if (nvs_get_u8(h, NVS_KEY_RES, &r) == ESP_OK && nvs_get_u8(h, NVS_KEY_QUALITY, &q) == ESP_OK &&
            camera_resolution_by_id(r) && camera_quality_valid(q)) {
            s.resolution = camera_resolution_by_id(r);
            s.quality = q;
        } else if (nvs_get_u8(h, NVS_KEY_PRESET, &legacy) == ESP_OK && legacy >= 1 && legacy <= 5) {
            s.resolution = camera_resolution_by_id(s_legacy[legacy - 1][0]);
            s.quality = s_legacy[legacy - 1][1];
            ESP_LOGI(TAG, "migrated legacy preset %d", legacy);
        } else {
            ESP_LOGW(TAG, "no valid stored settings, using defaults");
        }
        nvs_close(h);
    }
    return s;
}

static esp_err_t save_settings(const camera_settings_t *s)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(h, NVS_KEY_RES, s->resolution->id);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, NVS_KEY_QUALITY, s->quality);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/* Sensor settings only; the caller holds s_lock */
static bool sensor_apply(const camera_settings_t *s)
{
    sensor_t *sen = esp_camera_sensor_get();
    return sen && sen->set_framesize && sen->set_quality &&
           sen->set_framesize(sen, (framesize_t)s->resolution->frame_size) == 0 &&
           sen->set_quality(sen, s->quality) == 0;
}

/* Wait until the driver delivers frames of the selected size (frames captured before the change may still be queued).
 * The caller holds s_lock; frames are returned immediately. */
static bool sensor_settled(const camera_settings_t *s)
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
        if (w == s->resolution->width && h == s->resolution->height) {
            return true;
        }
    }
    return false;
}

esp_err_t camera_set_settings(int resolution_id, int quality, bool *saved)
{
    camera_settings_t want = { camera_resolution_by_id(resolution_id), (uint8_t)quality };
    *saved = false;
    if (!want.resolution || !camera_quality_valid(quality)) {
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

    camera_settings_t prev;
    camera_get_settings(&prev);
    esp_err_t ret = ESP_OK;
    if (want.resolution != prev.resolution || want.quality != prev.quality) {
        if (!sensor_apply(&want)) {
            ret = ESP_FAIL;
        } else if (!sensor_settled(&want)) {
            ret = ESP_ERR_INVALID_RESPONSE;
        }
        if (ret == ESP_OK) {
            atomic_store(&s_active, ((unsigned)want.resolution->id << 8) | want.quality);
        } else {
            ESP_LOGE(TAG, "%s q%u failed: 0x%x, restoring %s q%u", want.resolution->name, want.quality, ret,
                     prev.resolution->name, prev.quality);
            if (!sensor_apply(&prev) || !sensor_settled(&prev)) {
                ESP_LOGE(TAG, "restoring %s q%u failed too", prev.resolution->name, prev.quality);
            }
        }
    }
    xSemaphoreGive(s_lock);

    if (ret == ESP_OK) {
        if (!s_persist) {
            ESP_LOGW(TAG, "%s q%u active, NVS unavailable: not saved", want.resolution->name, want.quality);
        } else {
            esp_err_t e = save_settings(&want);
            *saved = (e == ESP_OK);
            if (e != ESP_OK) {
                ESP_LOGE(TAG, "saving settings failed: 0x%x", e);
            }
        }
        ESP_LOGI(TAG, "%s %ux%u q%u active, saved %d", want.resolution->name, want.resolution->width,
                 want.resolution->height, want.quality, (int)*saved);
    }
    return ret;
}

static void camera_task(void *arg)
{
    frame_slot_t *slot = frame_store_writer_slot();
    for (;;) {
        if (atomic_load(&s_waiters) > 0) {
            vTaskDelay(pdMS_TO_TICKS(10));   /* let camera_set_settings take the lock */
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
        camera_settings_t cur;
        camera_get_settings(&cur);
        uint32_t limit = cur.resolution->max_bytes;
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
    camera_settings_t preset = load_stored_settings();
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
        .jpeg_quality = preset.quality,
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
    if (!s_lock || !sensor_apply(&preset)) {
        ESP_LOGE(TAG, "applying %s q%u failed: camera disabled", preset.resolution->name, preset.quality);
        esp_camera_deinit();
        return;
    }
    atomic_store(&s_active, ((unsigned)preset.resolution->id << 8) | preset.quality);
    s_ready = true;
    xTaskCreate(camera_task, "camera", CAMERA_TASK_STACK_BYTES, NULL, 5, NULL);
    ESP_LOGI(TAG, "OV2640 ready, %s %ux%u, JPEG quality %u, persistence %s", preset.resolution->name,
             preset.resolution->width, preset.resolution->height, preset.quality, s_persist ? "on" : "off");
}
