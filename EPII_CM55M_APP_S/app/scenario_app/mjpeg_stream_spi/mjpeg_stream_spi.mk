override SCENARIO_APP_SUPPORT_LIST := $(APP_TYPE)

APPL_DEFINES += -DMJPEG_STREAM_SPI
APPL_DEFINES += -Daudvidpre_ret_pll400_timer1 -DIP_xdma
APPL_DEFINES += -DEVT_DATAPATH

EVENTHANDLER_SUPPORT = event_handler
EVENTHANDLER_SUPPORT_LIST += evt_datapath

##
# library support feature (no TFLM / inference libraries on purpose)
##
LIB_SEL = pwrmgmt sensordp spi_ptl spi_eeprom hxevent

MID_SEL =

override OS_SEL:=
override TRUSTZONE := y
override TRUSTZONE_TYPE := security
override TRUSTZONE_FW_TYPE := 1
override CIS_SEL := HM_COMMON
override EPII_USECASE_SEL := drv_user_defined

CIS_SUPPORT_INAPP = cis_sensor
CIS_SUPPORT_INAPP_MODEL = cis_ov5647

ifeq ($(strip $(TOOLCHAIN)), arm)
override LINKER_SCRIPT_FILE := $(SCENARIO_APP_ROOT)/$(APP_TYPE)/mjpeg_stream_spi.sct
else#TOOLChain
override LINKER_SCRIPT_FILE := $(SCENARIO_APP_ROOT)/$(APP_TYPE)/mjpeg_stream_spi.ld
endif
