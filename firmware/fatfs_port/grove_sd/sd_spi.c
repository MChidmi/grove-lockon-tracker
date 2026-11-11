/*
 * SD card over SPI for FatFs, written for the Grove SD demo firmware (replaces the SDK's
 * middleware/fatfs/port/mmc_spi). Follows the SD Physical Layer spec's SPI mode and
 * ChaN's reference MMC/SDC driver, with these differences from the SDK port:
 *
 *  - Every transfer is full duplex. While receiving, 0xFF goes out, because the card must
 *    see DI high. The SDK port used the SPI block's receive-only mode, which drives an
 *    undefined level on DI, and some cards then took stray bytes as a command and stayed
 *    busy until power was removed.
 *  - Chip select is a plain GPIO (PB5 = GPIO16) driven here, not through the CMSIS driver.
 *  - Every wait has a time limit, and nothing asserts, so a bad card can't hang the board.
 *  - Initialising again (remount, card swap) closes and reopens the SPI device cleanly.
 *
 * Wiring (Grove Vision AI V2): PB2 MOSI, PB3 MISO, PB4 SCLK, PB5 CS.
 */
#include <string.h>
#include "WE2_device.h"
#include "WE2_core.h"
#include "board.h"
#include "Driver_SPI.h"
#include "hx_drv_gpio.h"
#include "hx_drv_scu.h"
#include "ff.h"
#include "diskio.h"
#include "mmc_we2.h"
#include "xprintf.h"

/* Set to 0 once the driver is proven on hardware. */
#define SD_DEBUG 0
/* Write one sector per command (CMD24) instead of multi-block CMD25: slower, gentlest on cards. */
#define SD_SINGLE_BLOCK_WRITES 1
#define SD_BUSY_MS 5000         /* longest wait for a busy card */
#if SD_DEBUG
#define SD_LOG(...) xprintf(__VA_ARGS__)
#else
#define SD_LOG(...) do {} while (0)
#endif
#define SD_ERR(...) xprintf(__VA_ARGS__)   /* failures: always logged */

extern ARM_DRIVER_SPI Driver_SPI0;

#define SPI_SLOW_HZ       400000      /* card identification: 100-400 kHz */
#define SPI_FAST_HZ       12000000
#define CS_GPIO           GPIO16

/* SD commands (bit 7 set = application command, sent after CMD55) */
#define CMD0    (0)         /* GO_IDLE_STATE */
#define CMD1    (1)         /* SEND_OP_COND (MMC) */
#define ACMD41  (0x80 + 41) /* SEND_OP_COND (SDC) */
#define CMD8    (8)         /* SEND_IF_COND */
#define CMD9    (9)         /* SEND_CSD */
#define CMD10   (10)        /* SEND_CID */
#define CMD12   (12)        /* STOP_TRANSMISSION */
#define ACMD13  (0x80 + 13) /* SD_STATUS */
#define CMD16   (16)        /* SET_BLOCKLEN */
#define CMD17   (17)        /* READ_SINGLE_BLOCK */
#define CMD18   (18)        /* READ_MULTIPLE_BLOCK */
#define ACMD23  (0x80 + 23) /* SET_WR_BLK_ERASE_COUNT */
#define CMD24   (24)        /* WRITE_BLOCK */
#define CMD25   (25)        /* WRITE_MULTIPLE_BLOCK */
#define CMD55   (55)        /* APP_CMD */
#define CMD58   (58)        /* READ_OCR */
#define CMD59   (59)        /* CRC_ON_OFF */

static volatile DSTATUS s_stat = STA_NOINIT;
static BYTE s_type;                /* CT_* flags from diskio.h */
static int s_spi_open;
static uint32_t s_src_hz;          /* SPI master source clock the bus speed was set for */
static uint32_t s_busy_ms = SD_BUSY_MS;   /* shorter while identifying a card */
static BYTE s_ones[512];           /* all 0xFF: sent while receiving */
static BYTE s_sink[512];           /* received bytes we don't need */

/* ---- time (the SDK runs SysTick at full reload and counts wraps) ------------------------ */

extern volatile unsigned long g_time_loop;

static uint32_t sd_ms(void)
{
    uint32_t val, loops, loops2;
    do {
        loops = (uint32_t)g_time_loop;
        val = SysTick->VAL;
        loops2 = (uint32_t)g_time_loop;
    } while (loops != loops2);
    uint64_t reload = (uint64_t)(SysTick->LOAD & SysTick_LOAD_RELOAD_Msk) + 1u;
    uint64_t ticks = (uint64_t)loops * reload + (reload - 1u - val);
    return (uint32_t)(ticks / (SystemCoreClock / 1000u));
}

static void sd_delay_ms(uint32_t ms)
{
    uint32_t t0 = sd_ms();
    while (sd_ms() - t0 < ms) {}
}

/* ---- CRCs (SD SPI mode with CRC checking on: CMD59) -------------------------------- */

/* With CRC on, the card rejects any command or data block damaged on the wires instead of
 * acting on it or writing it, and we check the CRC of everything we read. */
static int s_crc_on;
static uint16_t s_crc16_tab[256];

static BYTE crc7(const BYTE *d, UINT n)
{
    BYTE crc = 0;
    while (n--) {
        BYTE b = *d++;
        for (int i = 0; i < 8; i++) {
            crc <<= 1;
            if ((b ^ crc) & 0x80) crc ^= 0x09;
            b <<= 1;
        }
    }
    return crc & 0x7F;
}

static uint16_t crc16(const BYTE *d, UINT n)
{
    if (!s_crc16_tab[1]) {
        for (int i = 0; i < 256; i++) {
            uint16_t c = (uint16_t)(i << 8);
            for (int k = 0; k < 8; k++) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
            s_crc16_tab[i] = c;
        }
    }
    uint16_t crc = 0;
    while (n--) crc = (uint16_t)((crc << 8) ^ s_crc16_tab[((crc >> 8) ^ *d++) & 0xFF]);
    return crc;
}

/* ---- SPI -------------------------------------------------------------------------- */

/* Full-duplex transfer. tx NULL sends 0xFF; rx NULL discards what comes back. */
static int spi_xfer(const BYTE *tx, BYTE *rx, UINT n)
{
    while (n) {
        UINT k = n > sizeof(s_ones) ? sizeof(s_ones) : n;
        int32_t r = Driver_SPI0.Transfer(tx ? tx : s_ones, rx ? rx : s_sink, k);
        if (r != ARM_DRIVER_OK) { SD_LOG("sd: Transfer(%u) returned %d\r\n", k, (int)r); return -1; }
        uint32_t t0 = sd_ms();
        while (Driver_SPI0.GetStatus().busy || Driver_SPI0.GetDataCount() < k) {
            if (sd_ms() - t0 > 200) {
                SD_LOG("sd: transfer of %u bytes timed out (busy %d, count %u)\r\n", k,
                       (int)Driver_SPI0.GetStatus().busy, (unsigned)Driver_SPI0.GetDataCount());
                return -1;
            }
        }
        if (tx) tx += k;
        if (rx) rx += k;
        n -= k;
    }
    return 0;
}

static BYTE xchg(BYTE d)
{
    BYTE r = 0xFF;
    if (spi_xfer(&d, &r, 1) != 0) return 0xFF;
    return r;
}

static void cs_high(void) { hx_drv_gpio_set_out_value(CS_GPIO, GPIO_OUT_HIGH); }
static void cs_low(void) { hx_drv_gpio_set_out_value(CS_GPIO, GPIO_OUT_LOW); }

/* Waits until the card stops signalling busy (DO low). 1 = ready. */
static int wait_ready(uint32_t ms)
{
    uint32_t t0 = sd_ms();
    do {
        if (xchg(0xFF) == 0xFF) return 1;
    } while (sd_ms() - t0 < ms);
    return 0;
}

static void deselect(void)
{
    cs_high();
    xchg(0xFF);            /* one more clock so the card releases DO */
}

static int select_card(void)
{
    cs_low();
    xchg(0xFF);
    if (wait_ready(s_busy_ms)) return 1;   /* a write keeps the card busy for up to ~500 ms; some cards longer */
    SD_ERR("sd: card busy (DO low) for %lu ms\r\n", (unsigned long)s_busy_ms);
    deselect();
    return 0;
}

/* Receives a data block: waits for the start token, then data, then drops the CRC. */
static int rcvr_datablock(BYTE *buff, UINT len)
{
    BYTE token;
    uint32_t t0 = sd_ms();
    do {
        token = xchg(0xFF);
    } while (token == 0xFF && sd_ms() - t0 < 200);
    if (token != 0xFE) return 0;
    if (spi_xfer(NULL, buff, len) != 0) return 0;
    uint16_t got = (uint16_t)(xchg(0xFF) << 8);
    got |= xchg(0xFF);
    if (s_crc_on && got != crc16(buff, len)) {
        SD_ERR("sd: read CRC mismatch\r\n");
        return 0;
    }
    return 1;
}

/* Sends a 512-byte data block with the given token (0xFE single, 0xFC multi, 0xFD stop). */
static int xmit_datablock(const BYTE *buff, BYTE token)
{
    if (!wait_ready(SD_BUSY_MS)) { SD_ERR("sd: card busy before data token 0x%x\r\n", token); return 0; }
    xchg(token);
    if (token != 0xFD) {
        if (spi_xfer(buff, NULL, 512) != 0) return 0;
        uint16_t crc = crc16(buff, 512);
        xchg((BYTE)(crc >> 8));
        xchg((BYTE)crc);
        BYTE resp = xchg(0xFF);
        if ((resp & 0x1F) != 0x05) {   /* 0x0B: CRC error, 0x0D: write error */
            SD_ERR("sd: data block rejected (resp 0x%x%s)\r\n", resp, (resp & 0x1F) == 0x0B ? ", CRC error" : "");
            return 0;
        }
    }
    return 1;
}

/* Sends a command and returns the R1 response (0xFF if the card never got it). */
static BYTE send_cmd(BYTE cmd, DWORD arg)
{
    BYTE res;
    if (cmd & 0x80) {          /* ACMD<n> = CMD55 then CMD<n> */
        cmd &= 0x7F;
        res = send_cmd(CMD55, 0);
        if (res > 1) return res;
    }
    if (cmd != CMD12) {
        deselect();
        if (!select_card()) return 0xFF;
    }
    BYTE frame[6] = {
        (BYTE)(0x40 | cmd), (BYTE)(arg >> 24), (BYTE)(arg >> 16), (BYTE)(arg >> 8), (BYTE)arg, 0,
    };
    frame[5] = (BYTE)((crc7(frame, 5) << 1) | 1);
    if (spi_xfer(frame, NULL, sizeof(frame)) != 0) return 0xFF;
    if (cmd == CMD12) xchg(0xFF);   /* skip the stuff byte */
    for (int n = 0; n < 10; n++) {  /* NCR is 0-8 bytes */
        res = xchg(0xFF);
        if (!(res & 0x80)) break;
    }
    return res;
}

/* The SPI bus divider is worked out from the SPI master's source clock when it is set.
 * Other code (the camera's clock setup) can change that clock afterwards; if so, set the
 * bus speed again so the card keeps getting the speed it was initialised for. */
static void check_clock(void)
{
    uint32_t hz = 0;
    if (hx_drv_scu_get_freq(SCU_CLK_FREQ_TYPE_LSC_SSPIM, &hz) != SCU_NO_ERROR || hz == s_src_hz) return;
    SD_ERR("sd: SPI source clock changed %lu -> %lu Hz, setting the bus speed again\r\n",
           (unsigned long)s_src_hz, (unsigned long)hz);
    s_src_hz = hz;
    Driver_SPI0.Control(ARM_SPI_SET_BUS_SPEED, (s_stat & STA_NOINIT) ? SPI_SLOW_HZ : SPI_FAST_HZ);
}

/* ---- FatFs disk interface (called from middleware/fatfs/source/diskio.c) ------------ */

DSTATUS mmc_disk_status(void)
{
    return s_stat;
}

static int spi_open(uint32_t hz)
{
    int32_t r;
    if (s_spi_open) {
        Driver_SPI0.Uninitialize();
        s_spi_open = 0;
    }
    if ((r = Driver_SPI0.Initialize(NULL)) != ARM_DRIVER_OK) { SD_LOG("sd: SPI Initialize %d\r\n", (int)r); return -1; }
    if ((r = Driver_SPI0.PowerControl(ARM_POWER_FULL)) != ARM_DRIVER_OK) { SD_LOG("sd: SPI PowerControl %d\r\n", (int)r); return -1; }
    if ((r = Driver_SPI0.Control(ARM_SPI_MODE_MASTER | ARM_SPI_CPOL0_CPHA0 | ARM_SPI_MSB_LSB |
                            ARM_SPI_SS_MASTER_UNUSED | ARM_SPI_DATA_BITS(8), hz)) != ARM_DRIVER_OK) {
        SD_LOG("sd: SPI Control(master) %d\r\n", (int)r);
        return -1;
    }
    s_spi_open = 1;
    Driver_SPI0.Control(ARM_SPI_SET_DEFAULT_TX_VALUE, 0xFF);   /* in case anything uses Receive() */
    return 0;
}

DSTATUS mmc_disk_initialize(void)
{
    BYTE ty = 0, ocr[4];

    hx_drv_scu_set_PB2_pinmux(SCU_PB2_PINMUX_SPI_M_DO_1, 1);
    hx_drv_scu_set_PB3_pinmux(SCU_PB3_PINMUX_SPI_M_DI_1, 1);
    hx_drv_scu_set_PB4_pinmux(SCU_PB4_PINMUX_SPI_M_SCLK_1, 1);
    hx_drv_scu_set_PB5_pinmux(SCU_PB5_PINMUX_GPIO16, 1);
    hx_drv_gpio_set_output(CS_GPIO, GPIO_OUT_HIGH);
    memset(s_ones, 0xFF, sizeof(s_ones));

    s_stat = STA_NOINIT;
    s_type = 0;
    if (spi_open(SPI_SLOW_HZ) != 0) return s_stat;

    /* Power-up: CS high and at least 74 clocks, then CMD0 into SPI mode. */
    cs_high();
    sd_delay_ms(10);
    for (int n = 0; n < 10; n++) xchg(0xFF);

    /* A healthy card is idle (DO high) right after power-up; one stuck busy won't recover,
     * so don't spend long waiting for it here. */
    s_busy_ms = 200;
    BYTE r = 0xFF;
    for (int tries = 0; tries < 5 && r != 1; tries++) r = send_cmd(CMD0, 0);
    SD_LOG("sd: CMD0 -> 0x%x\r\n", r);
    if (r == 1) {
        uint32_t t0 = sd_ms();
        if (send_cmd(CMD8, 0x1AA) == 1) {                 /* SD v2 */
            for (int n = 0; n < 4; n++) ocr[n] = xchg(0xFF);
            if (ocr[2] == 0x01 && ocr[3] == 0xAA) {        /* card works at 2.7-3.6 V */
                while (sd_ms() - t0 < 1000 && send_cmd(ACMD41, 1UL << 30) != 0) {}
                if (sd_ms() - t0 < 1000 && send_cmd(CMD58, 0) == 0) {
                    for (int n = 0; n < 4; n++) ocr[n] = xchg(0xFF);
                    ty = (ocr[0] & 0x40) ? (CT_SDC2 | CT_BLOCK) : CT_SDC2;
                }
            }
        } else {                                           /* SD v1 or MMC v3 */
            BYTE cmd;
            if (send_cmd(ACMD41, 0) <= 1) { ty = CT_SDC1; cmd = ACMD41; }
            else { ty = CT_MMC3; cmd = CMD1; }
            while (sd_ms() - t0 < 1000 && send_cmd(cmd, 0) != 0) {}
            if (sd_ms() - t0 >= 1000 || send_cmd(CMD16, 512) != 0) ty = 0;
        }
    }
    s_busy_ms = SD_BUSY_MS;
    s_crc_on = 0;
    if (ty) {
        if (send_cmd(CMD59, 1) == 0) s_crc_on = 1;   /* CRC_ON_OFF */
        else SD_ERR("sd: card refused CRC checking, continuing without it\r\n");
    }
    deselect();

    s_type = ty;
    SD_LOG("sd: card type 0x%x, CRC %s\r\n", ty, s_crc_on ? "on" : "off");
    if (ty && Driver_SPI0.Control(ARM_SPI_SET_BUS_SPEED, SPI_FAST_HZ) == ARM_DRIVER_OK) {
        s_stat &= ~STA_NOINIT;
        hx_drv_scu_get_freq(SCU_CLK_FREQ_TYPE_LSC_SSPIM, &s_src_hz);
        SD_LOG("sd: SPI source clock %lu Hz\r\n", (unsigned long)s_src_hz);
    } else {
        s_type = 0;
    }
    return s_stat;
}

static DRESULT disk_read_once(BYTE *buff, LBA_t sector, UINT count);
static DRESULT disk_write_once(const BYTE *buff, LBA_t sector, UINT count);

DRESULT mmc_disk_read(BYTE *buff, LBA_t sector, UINT count)
{
    DRESULT r = RES_ERROR;
    for (int attempt = 0; attempt < 3 && r != RES_OK; attempt++) {
        r = disk_read_once(buff, sector, count);
        if (r == RES_NOTRDY || r == RES_PARERR) break;
    }
    return r;
}

static DRESULT disk_read_once(BYTE *buff, LBA_t sector, UINT count)
{
    DWORD sect = (DWORD)sector;
    if (!count) return RES_PARERR;
    if (s_stat & STA_NOINIT) return RES_NOTRDY;
    check_clock();
    if (!(s_type & CT_BLOCK)) sect *= 512;             /* byte-addressed SDSC cards */

    if (count == 1) {
        if (send_cmd(CMD17, sect) == 0 && rcvr_datablock(buff, 512)) count = 0;
    } else if (send_cmd(CMD18, sect) == 0) {
        do {
            if (!rcvr_datablock(buff, 512)) break;
            buff += 512;
        } while (--count);
        send_cmd(CMD12, 0);
    }
    deselect();
    return count ? RES_ERROR : RES_OK;
}

#if FF_FS_READONLY == 0
static uint32_t s_writes;   /* sectors written since boot, for the progress log */

static DRESULT write_with_retries(const BYTE *buff, LBA_t sector, UINT count)
{
    DRESULT r = RES_ERROR;
    for (int attempt = 0; attempt < 3 && r != RES_OK; attempt++) {
        r = disk_write_once(buff, sector, count);
        if (r == RES_NOTRDY || r == RES_PARERR || r == RES_WRPRT) break;
    }
    return r;
}

DRESULT mmc_disk_write(const BYTE *buff, LBA_t sector, UINT count)
{
#if SD_SINGLE_BLOCK_WRITES
    for (UINT i = 0; i < count; i++) {
        DRESULT r = write_with_retries(buff + i * 512u, sector + i, 1);
        if (r != RES_OK) return r;
        if ((++s_writes & 255u) == 0) SD_ERR("sd: %lu sectors written\r\n", (unsigned long)s_writes);
    }
    return RES_OK;
#else
    s_writes += count;
    return write_with_retries(buff, sector, count);
#endif
}

static DRESULT disk_write_once(const BYTE *buff, LBA_t sector, UINT count)
{
    DWORD sect = (DWORD)sector;
    if (!count) return RES_PARERR;
    if (s_stat & STA_NOINIT) return RES_NOTRDY;
    check_clock();
    if (s_stat & STA_PROTECT) return RES_WRPRT;
    if (!(s_type & CT_BLOCK)) sect *= 512;

    UINT asked = count;
    BYTE r1;
    if (count == 1) {
        r1 = send_cmd(CMD24, sect);
        if (r1 == 0 && xmit_datablock(buff, 0xFE)) {
            count = 0;
            /* Wait for programming to finish here, so a card that never finishes is
             * caught at the sector that caused it. */
            if (!wait_ready(SD_BUSY_MS)) {
                SD_ERR("sd: still busy %d ms after writing sector %u\r\n", SD_BUSY_MS, (unsigned)sector);
                count = 1;
            }
        }
    } else {
        if (s_type & CT_SDC) send_cmd(ACMD23, count);   /* pre-erase hint */
        r1 = send_cmd(CMD25, sect);
        if (r1 == 0) {
            do {
                if (!xmit_datablock(buff, 0xFC)) break;
                buff += 512;
            } while (--count);
            if (!xmit_datablock(0, 0xFD)) count = 1;      /* stop token */
        }
    }
    deselect();
    if (count) SD_ERR("sd: write of %u sectors at %u failed (r1 0x%x, %u left)\r\n",
                      (unsigned)asked, (unsigned)sector, r1, (unsigned)count);
    return count ? RES_ERROR : RES_OK;
}
#endif

DRESULT mmc_disk_ioctl(BYTE cmd, void *buff)
{
    DRESULT res = RES_ERROR;
    BYTE csd[16];
    if (s_stat & STA_NOINIT) return RES_NOTRDY;

    switch (cmd) {
    case CTRL_SYNC:                      /* wait for any write to finish */
        if (select_card()) res = RES_OK;
        break;
    case GET_SECTOR_COUNT:
        if (send_cmd(CMD9, 0) == 0 && rcvr_datablock(csd, 16)) {
            if ((csd[0] >> 6) == 1) {    /* CSD v2 (SDHC/SDXC) */
                DWORD csize = csd[9] + ((DWORD)csd[8] << 8) + ((DWORD)(csd[7] & 63) << 16) + 1;
                *(LBA_t *)buff = (LBA_t)csize << 10;
            } else {                      /* CSD v1 (SDSC, MMC) */
                BYTE n = (csd[5] & 15) + ((csd[10] & 128) >> 7) + ((csd[9] & 3) << 1) + 2;
                DWORD csize = (csd[8] >> 6) + ((WORD)csd[7] << 2) + ((WORD)(csd[6] & 3) << 10) + 1;
                *(LBA_t *)buff = (LBA_t)csize << (n - 9);
            }
            res = RES_OK;
        }
        break;
    case GET_BLOCK_SIZE:                 /* erase block size in sectors */
        *(DWORD *)buff = 128;
        res = RES_OK;
        break;
    case MMC_GET_TYPE:
        *(BYTE *)buff = s_type;
        res = RES_OK;
        break;
    case MMC_GET_CSD:
        if (send_cmd(CMD9, 0) == 0 && rcvr_datablock((BYTE *)buff, 16)) res = RES_OK;
        break;
    case MMC_GET_CID:
        if (send_cmd(CMD10, 0) == 0 && rcvr_datablock((BYTE *)buff, 16)) res = RES_OK;
        break;
    case MMC_GET_OCR:
        if (send_cmd(CMD58, 0) == 0) {
            for (int n = 0; n < 4; n++) ((BYTE *)buff)[n] = xchg(0xFF);
            res = RES_OK;
        }
        break;
    default:
        res = RES_PARERR;
    }
    deselect();
    return res;
}

/* For diagnostics: the SPI master source clock now, and the one the bus speed was set for. */
void mmc_spi_clock_info(uint32_t *now_hz, uint32_t *set_for_hz, int *crc_on)
{
    *now_hz = 0;
    hx_drv_scu_get_freq(SCU_CLK_FREQ_TYPE_LSC_SSPIM, now_hz);
    *set_for_hz = s_src_hz;
    *crc_on = s_crc_on;
}

/* Kept for API compatibility with the SDK port; all timing here uses SysTick. */
void mmc_disk_timerproc(void) {}
