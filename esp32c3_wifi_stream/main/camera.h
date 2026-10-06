#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint8_t id;           /* stable resolution identifier, the value stored in NVS and accepted by the HTTP API */
    const char *name;
    int frame_size;       /* framesize_t */
    uint16_t width, height;
    uint32_t max_bytes;   /* larger frames are dropped */
} camera_resolution_t;

#define CAMERA_RESOLUTION_COUNT 6
#define CAMERA_QUALITY_MIN      10    /* JPEG quality 10..40 is accepted; lower number = better quality / bigger frames */
#define CAMERA_QUALITY_MAX      40
#define CAM_STR_(x)             #x
#define CAM_STR(x)              CAM_STR_(x)

typedef struct {
    const camera_resolution_t *resolution;   /* never NULL */
    uint8_t quality;
} camera_settings_t;

/* Call after nvs_flash_init() (if NVS is unusable the defaults are used and nothing is persisted) */
void camera_start(void);

/* Resolutions are listed in ascending size; index 0..CAMERA_RESOLUTION_COUNT-1 */
const camera_resolution_t *camera_resolution_at(int index);
const camera_resolution_t *camera_resolution_by_id(int id);   /* NULL if id is not supported */
bool camera_quality_valid(int quality);
void camera_get_settings(camera_settings_t *out);             /* currently applied, coherent pair */
bool camera_settings_persistent(void);                        /* true if NVS is available to store the selection */

/* Apply resolution + quality together at runtime (no reboot) and store both in NVS. Takes at most a few seconds.
 * ESP_OK: applied; *saved tells whether it was also stored. On error the previous settings stay active and nothing is
 * stored: ESP_ERR_INVALID_ARG (unsupported resolution id or quality outside the range), ESP_ERR_INVALID_STATE (no camera),
 * ESP_ERR_TIMEOUT (camera busy), ESP_FAIL / ESP_ERR_INVALID_RESPONSE (the sensor rejected the change or did not deliver
 * frames of the selected size). */
esp_err_t camera_set_settings(int resolution_id, int quality, bool *saved);
