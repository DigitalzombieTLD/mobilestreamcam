/*
 * mjpeg_stream_spi.c
 *
 * OV5647 -> HW JPEG encoder -> SPI master -> XIAO ESP32-C3 (WiFi MJPEG server).
 * No inference code: every JPEG frame is copied out of the sensor datapath
 * buffer and sent with the Himax SPI protocol (DATA_TYPE_JPG).
 *
 * Capture is re-triggered right after the copy so that sensor capture and JPEG
 * encoding overlap with the (blocking) SPI transfer of the previous frame.
 * Frames that are invalid / too large are dropped instead of stalling.
 */
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "powermode_export.h"

#include "WE2_device.h"
#include "spi_master_protocol.h"
#include "hx_drv_spi.h"
#include "spi_eeprom_comm.h"
#include "board.h"
#include "xprintf.h"
#include "WE2_core.h"
#include "hx_drv_scu.h"
#include "hx_drv_swreg_aon.h"
#include "hx_drv_xdma.h"
#include "sensor_dp_lib.h"
#include "hx_drv_pmu_export.h"
#include "hx_drv_pmu.h"
#include "powermode.h"
#include "system_WE2_ARMCM55.h"

#include "common_config.h"
#include "cisdp_sensor.h"
#include "event_handler.h"
#include "memory_manage.h"
#include "mjpeg_stream_spi.h"

#define GROVE_VISION_AI_II

#define SYSTICK_RELOAD			(0x1000000UL)

static uint8_t g_xdma_abnormal, g_cdm_fifoerror, g_wdt1_timeout, g_wdt2_timeout, g_wdt3_timeout;
static uint8_t g_hxautoi2c_error, g_inp1bitparer_abnormal;
static volatile uint8_t g_frame_ready;
static uint8_t g_spi_master_initial_status;

static uint8_t *g_tx_buf;

/* statistics */
static uint32_t g_stat_frames, g_stat_bytes, g_stat_dropped, g_stat_tx_fail, g_stat_total;
static uint32_t g_stat_last_size;
static uint32_t g_stat_tick_start_sys, g_stat_tick_start_loop;

#ifdef GROVE_VISION_AI_II
/*
 * Grove Vision AI V2 -> XIAO ESP32-C3 header:
 *   PB4  SPI_M_SCLK  -> XIAO D8  / GPIO8  (SCK)
 *   PB2  SPI_M_DO    -> XIAO D10 / GPIO10 (MOSI)
 *   PB3  SPI_M_DI    <- XIAO D9  / GPIO9  (MISO)
 *   PB11 SPI_M_CS    -> XIAO D1  / GPIO3  (CS)
 */
static void spi_m_pinmux_cfg(SCU_PINMUX_CFG_T *pinmux_cfg)
{
	pinmux_cfg->pin_pb2 = SCU_PB2_PINMUX_SPI_M_DO_1;
	pinmux_cfg->pin_pb3 = SCU_PB3_PINMUX_SPI_M_DI_1;
	pinmux_cfg->pin_pb4 = SCU_PB4_PINMUX_SPI_M_SCLK_1;
	pinmux_cfg->pin_pb11 = SCU_PB11_PINMUX_SPI_M_CS;
}
#else
static void spi_m_pinmux_cfg(SCU_PINMUX_CFG_T *pinmux_cfg)
{
	pinmux_cfg->pin_pb2 = SCU_PB2_PINMUX_SPI_M_DO_1;
	pinmux_cfg->pin_pb3 = SCU_PB3_PINMUX_SPI_M_DI_1;
	pinmux_cfg->pin_pb4 = SCU_PB4_PINMUX_SPI_M_SCLK_1;
	pinmux_cfg->pin_pb5 = SCU_PB5_PINMUX_SPI_M_CS_1;
}
#endif

static void pinmux_init(void)
{
	SCU_PINMUX_CFG_T pinmux_cfg;

	hx_drv_scu_get_all_pinmux_cfg(&pinmux_cfg);
	spi_m_pinmux_cfg(&pinmux_cfg);
	hx_drv_scu_set_all_pinmux_cfg(&pinmux_cfg, 1);
}

static void dp_var_init(void)
{
	g_xdma_abnormal = 0;
	g_cdm_fifoerror = 0;
	g_wdt1_timeout = 0;
	g_wdt2_timeout = 0;
	g_wdt3_timeout = 0;
	g_inp1bitparer_abnormal = 0;
	g_frame_ready = 0;
	g_hxautoi2c_error = 0;
	g_spi_master_initial_status = 0;
}

static void stats_reset(void)
{
	g_stat_frames = 0;
	g_stat_bytes = 0;
	g_stat_dropped = 0;
	g_stat_tx_fail = 0;
	SystemGetTick(&g_stat_tick_start_sys, &g_stat_tick_start_loop);
}

/* Print fps / size statistics once MJPEG_STATS_PERIOD_MS has elapsed. */
static void stats_update(void)
{
	uint32_t sys_now, loop_now;
	uint64_t cycles, period_cycles;

	SystemGetTick(&sys_now, &loop_now);
	cycles = (uint64_t)(loop_now - g_stat_tick_start_loop) * SYSTICK_RELOAD
			+ (uint64_t)g_stat_tick_start_sys - (uint64_t)sys_now;
	period_cycles = (uint64_t)SystemCoreClock / 1000 * MJPEG_STATS_PERIOD_MS;
	if (SystemCoreClock == 0 || cycles < period_cycles)
		return;

	uint32_t ms = (uint32_t)(cycles * 1000 / SystemCoreClock);
	uint32_t fps_x10 = (uint32_t)((uint64_t)g_stat_frames * 10000 / ms);
	uint32_t avg = g_stat_frames ? g_stat_bytes / g_stat_frames : 0;

	xprintf("MJPEG %dx%d: %d.%d fps, avg %d B/frame (last %d), sent %d, dropped %d, tx_fail %d, total %d\r\n",
			MJPEG_FRAME_WIDTH, MJPEG_FRAME_HEIGHT, fps_x10 / 10, fps_x10 % 10, avg,
			g_stat_last_size, g_stat_frames, g_stat_dropped, g_stat_tx_fail, g_stat_total);
	stats_reset();
}

static void mjpeg_send_frame(void)
{
	uint32_t jpeg_sz = 0, jpeg_addr = 0;

	cisdp_get_jpginfo(&jpeg_sz, &jpeg_addr);
	g_stat_total++;

	if (jpeg_sz < 4 || jpeg_sz > MJPEG_MAX_FRAME_BYTES || g_tx_buf == NULL) {
		/* drop: invalid or too large for the receiver */
		g_stat_dropped++;
		sensordplib_retrigger_capture();
		return;
	}

	/* Copy out of the datapath buffer so capture can restart immediately. */
	memcpy(g_tx_buf, (const void *)jpeg_addr, jpeg_sz);
	sensordplib_retrigger_capture();

	g_stat_last_size = jpeg_sz;

	if (g_spi_master_initial_status == 0) {
		if (hx_drv_spi_mst_open_speed(MJPEG_SPI_CLK_HZ) != 0) {
			xprintf("SPI master init fail\r\n");
			g_stat_tx_fail++;
			return;
		}
		g_spi_master_initial_status = 1;
	}

	if (hx_drv_spi_mst_protocol_write_sp((uint32_t)g_tx_buf, jpeg_sz, DATA_TYPE_JPG) == 0) {
		g_stat_frames++;
		g_stat_bytes += jpeg_sz;
	} else {
		g_stat_tx_fail++;
	}
}

static void dp_eventhdl_cb(EVT_INDEX_E event)
{
	uint16_t err;

	switch (event) {
	case EVT_INDEX_1BITPARSER_ERR:
		hx_drv_inp1bitparser_get_errstatus(&err);
		xprintf("EVT_INDEX_1BITPARSER_ERR err=0x%x\r\n", err);
		hx_drv_inp1bitparser_clear_int();
		hx_drv_inp1bitparser_set_enable(0);
		g_inp1bitparer_abnormal = 1;
		break;
	case EVT_INDEX_EDM_WDT1_TIMEOUT:
		xprintf("EVT_INDEX_EDM_WDT1_TIMEOUT\r\n");
		g_wdt1_timeout = 1;
		break;
	case EVT_INDEX_EDM_WDT2_TIMEOUT:
		xprintf("EVT_INDEX_EDM_WDT2_TIMEOUT\r\n");
		g_wdt2_timeout = 1;
		break;
	case EVT_INDEX_EDM_WDT3_TIMEOUT:
		xprintf("EVT_INDEX_EDM_WDT3_TIMEOUT\r\n");
		g_wdt3_timeout = 1;
		break;
	case EVT_INDEX_CDM_FIFO_ERR:
		xprintf("EVT_INDEX_CDM_FIFO_ERR\r\n");
		g_cdm_fifoerror = 1;
		break;
	case EVT_INDEX_XDMA_WDMA1_ABNORMAL:
	case EVT_INDEX_XDMA_WDMA2_ABNORMAL:
	case EVT_INDEX_XDMA_WDMA3_ABNORMAL:
	case EVT_INDEX_XDMA_RDMA_ABNORMAL:
		xprintf("EVT_INDEX_XDMA_ABNORMAL\r\n");
		g_xdma_abnormal = 1;
		break;
	case EVT_INDEX_XDMA_FRAME_READY:
		g_frame_ready = 1;
		break;
	case EVT_INDEX_HXAUTOI2C_ERR:
		xprintf("EVT_INDEX_HXAUTOI2C_ERR\r\n");
		g_hxautoi2c_error = 1;
		break;
	default:
		break;
	}

	if (g_frame_ready == 1) {
		g_frame_ready = 0;
		mjpeg_send_frame();
		stats_update();
	}

	if (g_inp1bitparer_abnormal || g_wdt1_timeout || g_wdt2_timeout || g_wdt3_timeout
			|| g_cdm_fifoerror || g_xdma_abnormal || g_hxautoi2c_error) {
		/* datapath error: restart the whole pipeline with a system reset */
		xprintf("datapath error, resetting\r\n");
		cisdp_sensor_stop();
		NVIC_SystemReset();
	}
}

static void app_start(void)
{
	if (cisdp_sensor_init() < 0) {
		xprintf("\r\nCIS Init fail\r\n");
		APP_BLOCK_FUNC();
	}

	dp_var_init();

	if (cisdp_dp_init(true, SENSORDPLIB_PATH_INT_INP_HW5X5_JPEG, dp_eventhdl_cb, 4, MJPEG_DP_SUBSAMPLE) < 0) {
		xprintf("\r\nDATAPATH Init fail\r\n");
		APP_BLOCK_FUNC();
	}

	g_tx_buf = (uint8_t *)mm_reserve_align(MJPEG_MAX_FRAME_BYTES, 0x20);
	if (g_tx_buf == NULL) {
		xprintf("TX buffer alloc fail\r\n");
		APP_BLOCK_FUNC();
	}

	stats_reset();
	event_handler_init();
	cisdp_sensor_start();
	event_handler_start();
}

void app_main(void)
{
	uint32_t wakeup_event, wakeup_event1, freq = 0;

	hx_drv_pmu_get_ctrl(PMU_pmu_wakeup_EVT, &wakeup_event);
	hx_drv_pmu_get_ctrl(PMU_pmu_wakeup_EVT1, &wakeup_event1);
	hx_drv_swreg_aon_get_pllfreq(&freq);
	xprintf("mjpeg_stream_spi: wakeup_event=0x%x, WakeupEvt1=0x%x, freq=%d\r\n", wakeup_event, wakeup_event1, freq);

	pinmux_init();

	if (!((wakeup_event == PMU_WAKEUP_NONE) && (wakeup_event1 == PMU_WAKEUPEVENT1_NONE))) {
		hx_lib_pm_ctrl_fromPMUtoCPU(NULL);
	}

	hx_lib_spi_eeprom_open(USE_DW_SPI_MST_Q);
	hx_lib_spi_eeprom_enable_XIP(USE_DW_SPI_MST_Q, true, FLASH_QUAD, true);

#ifndef CPU_24MHZ_VERSION
	EPII_set_memory(0x56100030, 0x4037);
	EPII_set_memory(0x56100034, 0x0);
	EPII_set_memory(0x56100038, 0xc1b8);
#endif

#ifdef __GNU__
	extern char __mm_start_addr__;
	mm_set_initial((int)(&__mm_start_addr__), 0x00200000 - ((int)(&__mm_start_addr__) - 0x34000000));
#else
	static uint8_t mm_start_addr __attribute__((section(".bss.mm_start_addr")));
	mm_set_initial((int)(&mm_start_addr), 0x00200000 - ((int)(&mm_start_addr) - 0x34000000));
#endif

	xprintf("MJPEG stream %dx%d, SPI %d Hz, max frame %d B\r\n",
			MJPEG_FRAME_WIDTH, MJPEG_FRAME_HEIGHT, MJPEG_SPI_CLK_HZ, MJPEG_MAX_FRAME_BYTES);
	app_start();
}
