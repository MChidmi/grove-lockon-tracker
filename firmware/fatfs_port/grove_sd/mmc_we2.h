/* SD card over SPI for FatFs (Grove SD demo port). diskio.c includes this file by name. */
#ifndef _MMC_DEFINED
#define _MMC_DEFINED

#include "ff.h"
#include "diskio.h"

#ifdef __cplusplus
extern "C" {
#endif

DSTATUS mmc_disk_initialize(void);
DSTATUS mmc_disk_status(void);
DRESULT mmc_disk_read(BYTE *buff, LBA_t sector, UINT count);
DRESULT mmc_disk_write(const BYTE *buff, LBA_t sector, UINT count);
DRESULT mmc_disk_ioctl(BYTE cmd, void *buff);
void mmc_disk_timerproc(void);
void mmc_spi_clock_info(uint32_t *now_hz, uint32_t *set_for_hz, int *crc_on);

#ifdef __cplusplus
}
#endif

#endif
