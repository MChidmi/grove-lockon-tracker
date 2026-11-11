override SCENARIO_APP_SUPPORT_LIST := $(APP_TYPE)

# 1 = SD card file manager, 2 = demo 1 plus the camera (preview + tiled high-res capture)
GROVE_DEMO ?= 1

APPL_DEFINES += -DGROVE_SD_DEMO
APPL_DEFINES += -DGROVE_DEMO=$(GROVE_DEMO)

MID_SEL = fatfs
FATFS_PORT_LIST = grove_sd     # our SD-over-SPI driver: firmware/fatfs_port/grove_sd
# diskio.c only maps drive 0 to mmc_disk_*() when it sees this define (it is not used anywhere else).
APPL_DEFINES += -DFATFS_PORT_mmc_spi
CMSIS_DRIVERS_LIST = SPI

override OS_SEL :=
override TRUSTZONE := y
override TRUSTZONE_TYPE := security
override TRUSTZONE_FW_TYPE := 1
override EPII_USECASE_SEL := drv_user_defined

ifeq ($(GROVE_DEMO), 1)
LIB_SEL = pwrmgmt
else
APPL_DEFINES += -DIP_xdma
APPL_DEFINES += -DEVT_DATAPATH
APPL_DEFINES += -DDBG_LESS
EVENTHANDLER_SUPPORT = event_handler
EVENTHANDLER_SUPPORT_LIST += evt_datapath
LIB_SEL = pwrmgmt sensordp spi_ptl spi_eeprom tpgdp hxevent
override CIS_SEL := HM_COMMON
CIS_SUPPORT_INAPP = cis_sensor
CIS_SUPPORT_INAPP_MODEL = cis_ov5647
endif

ifeq ($(strip $(TOOLCHAIN)), arm)
$(error Only the GNU toolchain is set up for this app)
else
override LINKER_SCRIPT_FILE := $(SCENARIO_APP_ROOT)/$(APP_TYPE)/grove_sd_demo.ld
endif
