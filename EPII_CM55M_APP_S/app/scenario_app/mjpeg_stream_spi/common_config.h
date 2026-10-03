/*
 * common_config.h - mjpeg_stream_spi configuration
 *
 * HX6538 captures OV5647 frames, JPEG-encodes them with the HW encoder and
 * pushes every frame to the XIAO ESP32-C3 over SPI (HX6538 = SPI master).
 */
#ifndef MJPEG_STREAM_SPI_COMMON_CONFIG_H_
#define MJPEG_STREAM_SPI_COMMON_CONFIG_H_

/*
 * Resolution. The OV5647 driver only provides a 640x480 binned sensor mode, so
 * the datapath resolution is selected by the INP subsample ratio:
 *   0 = 640x480 (default), 1 = 320x240, 2 = 160x120
 * (JPEG HW encoder: multiples of 16, see cisdp_cfg.h for limits.)
 */
#define MJPEG_RES_MODE				0

#if (MJPEG_RES_MODE == 0)
#define MJPEG_FRAME_WIDTH			640
#define MJPEG_FRAME_HEIGHT			480
#define MJPEG_DP_SUBSAMPLE			APP_DP_RES_YUV640x480_INP_SUBSAMPLE_1X
#define MJPEG_MAX_FRAME_BYTES		(60 * 1024)
#elif (MJPEG_RES_MODE == 1)
#define MJPEG_FRAME_WIDTH			320
#define MJPEG_FRAME_HEIGHT			240
#define MJPEG_DP_SUBSAMPLE			APP_DP_RES_YUV640x480_INP_SUBSAMPLE_2X
#define MJPEG_MAX_FRAME_BYTES		(19200)
#else
#error "MJPEG_RES_MODE must be 0 (640x480) or 1 (320x240)"
#endif

/*
 * JPEG quality: JPEG_ENC_QTABLE_10X (smaller frames, default) or
 * JPEG_ENC_QTABLE_4X (better quality, ~2.5x bigger frames; at 640x480 these can
 * exceed MJPEG_MAX_FRAME_BYTES and are then dropped).
 */
#define MJPEG_JPEG_QTABLE			JPEG_ENC_QTABLE_10X

/*
 * MJPEG_MAX_FRAME_BYTES (above): frames larger than this limit are dropped.
 * 640x480 limit is the ESP32-C3 per-frame buffer (60 KB); the sensor datapath
 * JPEG buffer itself is 76800 bytes (640x480) / 19200 bytes (320x240).
 */

/* SPI master clock towards the ESP32-C3 (Hz). Lower it if frames are corrupt. */
#define MJPEG_SPI_CLK_HZ			(12000000)

/* Print fps / frame size statistics on UART every N ms */
#define MJPEG_STATS_PERIOD_MS		(1000)

#define FRAME_CHECK_DEBUG			1

#endif /* MJPEG_STREAM_SPI_COMMON_CONFIG_H_ */
