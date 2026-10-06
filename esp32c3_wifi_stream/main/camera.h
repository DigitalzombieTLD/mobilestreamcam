#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint8_t id;           /* preset identifier (1..CAMERA_PRESET_COUNT), the only value stored in NVS */
    const char *name;
    int frame_size;       /* framesize_t */
    uint16_t width, height;
    uint8_t quality;      /* JPEG quality 0-63, lower = better quality / bigger frames */
    uint32_t max_bytes;   /* larger frames are dropped */
} camera_preset_t;

#define CAMERA_PRESET_COUNT 5

/* Call after nvs_flash_init() (if NVS is unusable the default preset is used and nothing is persisted) */
void camera_start(void);

const camera_preset_t *camera_preset_by_id(int id);   /* NULL if id is not a valid preset */
const camera_preset_t *camera_active_preset(void);    /* currently applied preset (never NULL) */
bool camera_settings_persistent(void);                /* true if NVS is available to store the selection */

/* Apply preset id at runtime (no reboot) and store it in NVS. Takes at most a few seconds.
 * ESP_OK: applied; *saved tells whether it was also stored. On error the previous preset stays active and nothing is
 * stored: ESP_ERR_INVALID_ARG (unknown id), ESP_ERR_INVALID_STATE (no camera), ESP_ERR_TIMEOUT (camera busy),
 * ESP_FAIL / ESP_ERR_INVALID_RESPONSE (the sensor rejected the change or did not deliver the new frame size). */
esp_err_t camera_set_preset(int id, bool *saved);
