/*
 * SD card commands for the Grove SD demo firmware (demo-1).
 *
 *   SD?                          card status: {"mounted","fs","label","total_kb","free_kb"}
 *   SDMOUNT                      (re)mount the card, after swapping it
 *   LS="<dir>"[,<start>]         {"path","start","entries":[[name,size,is_dir,mtime],...],"more"}
 *   FSTAT="<path>"               {"name","size","dir","mtime"}
 *   READ="<path>",<off>,<len>    {"offset","len","size","crc","data":"<base64>"}  len <= IO_CHUNK
 *   WRITE="<path>",<off>,"<b64>"[,<crc>]   offset 0 creates/truncates; otherwise appends at the
 *                                current end (off must equal the file size). {"offset","len","size"}
 *   RM="<path>"[,1]              delete a file or empty folder; ,1 deletes a folder and its contents
 *   MKDIR="<path>"               creates missing parent folders too
 *   MV="<from>","<to>"           rename or move
 *   TIME=<unix seconds>          sets the clock used for file timestamps
 * mtime values are seconds since 1970 in the board's local time (no time zone).
 */
#include <stdio.h>
#include <string.h>
#include "WE2_device.h"
#include "board.h"
#include "ff.h"
#include "diskio.h"
#include "proto.h"
#include "sdcmd.h"

#define LS_MAX     40
#define IO_CHUNK   12288
#define PATH_MAX_  256

/* FatFs sector buffers live in shared SRAM (.bss.NoInit, see the .ld), not the core's
 * private DTCM: the SPI driver may move them by DMA. FatFs initialises them itself. */
#define SD_DMA_BUF __attribute__((section(".bss.NoInit"))) __ALIGNED(32)
static FATFS g_fs SD_DMA_BUF;
static int g_mounted;
static FRESULT g_mount_err = FR_NOT_READY;
static FIL g_file SD_DMA_BUF;
static DIR g_dir;
static FILINFO g_fno;
static uint8_t g_io[IO_CHUNK] SD_DMA_BUF;
static char g_path[PATH_MAX_];
static uint32_t g_time_base;     /* unix seconds at g_time_ms */
static uint32_t g_time_ms;

/* ---- time ------------------------------------------------------------------- */

static uint32_t now_unix(void)
{
    return g_time_base ? g_time_base + (millis() - g_time_ms) / 1000 : 0;
}

/* Days since 1970-01-01 to civil date (Howard Hinnant's algorithm). */
static void civil_from_days(int32_t z, int *y, unsigned *m, unsigned *d)
{
    z += 719468;
    int32_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int32_t yy = (int32_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (int)(yy + (*m <= 2));
}

static int32_t days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    int32_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int32_t)doe - 719468;
}

/* FatFs asks for the current time when it writes a file. */
DWORD get_fattime(void)
{
    uint32_t t = now_unix();
    if (!t) t = 1767225600u;   /* 2026-01-01 until the host sets the clock */
    int y; unsigned m, d;
    civil_from_days((int32_t)(t / 86400), &y, &m, &d);
    uint32_t s = t % 86400;
    return ((DWORD)(y - 1980) << 25) | ((DWORD)m << 21) | ((DWORD)d << 16)
         | ((DWORD)(s / 3600) << 11) | ((DWORD)((s / 60) % 60) << 5) | ((DWORD)((s % 60) / 2));
}

static uint32_t fat_to_unix(WORD fdate, WORD ftime)
{
    if (!fdate) return 0;
    int32_t days = days_from_civil(1980 + (fdate >> 9), (fdate >> 5) & 15, fdate & 31);
    return (uint32_t)days * 86400u + (ftime >> 11) * 3600u + ((ftime >> 5) & 63) * 60u + (ftime & 31) * 2u;
}

/* ---- helpers -------------------------------------------------------------- */

static const char *fr_text(FRESULT r)
{
    switch (r) {
    case FR_DISK_ERR: return "SD card I/O error";
    case FR_INT_ERR: return "File system internal error";
    case FR_NOT_READY: return "No SD card, or it is not responding";
    case FR_NO_FILE: return "No such file";
    case FR_NO_PATH: return "No such folder";
    case FR_INVALID_NAME: return "Invalid name";
    case FR_DENIED: return "Access denied (folder not empty, or disk full)";
    case FR_EXIST: return "Already exists";
    case FR_INVALID_OBJECT: return "Invalid object";
    case FR_WRITE_PROTECTED: return "SD card is write protected";
    case FR_INVALID_DRIVE: return "Invalid drive";
    case FR_NOT_ENABLED: return "File system not mounted";
    case FR_NO_FILESYSTEM: return "No FAT32/exFAT file system on the card";
    case FR_TIMEOUT: return "Timed out";
    case FR_LOCKED: return "File is locked";
    case FR_NOT_ENOUGH_CORE: return "Out of memory";
    default: return "File system error";
    }
}

static void reply_fr(const char *name, FRESULT r)
{
    char msg[96];
    int code = (r == FR_DISK_ERR || r == FR_NOT_READY || r == FR_INT_ERR || r == FR_NO_FILESYSTEM) ? PE_IO
             : (r == FR_DENIED || r == FR_EXIST || r == FR_WRITE_PROTECTED) ? PE_PERM : PE_INVAL;
    snprintf(msg, sizeof(msg), "%s (FR %d)", fr_text(r), (int)r);
    reply_msg(name, code, msg);
}

static int sd_ready(const char *name)
{
    if (!g_mounted) sd_mount();
    if (!g_mounted) { reply_fr(name, g_mount_err); return 0; }
    return 1;
}

static const char *arg_path(const proto_cmd_t *c, int i)
{
    if (i >= c->argc || !c->quoted[i] || !c->argv[i][0]) return NULL;
    return c->argv[i];
}

/* ---- mount ---------------------------------------------------------------------- */

/* The SD driver (fatfs_port/grove_sd) sets up its own pins and SPI on each mount. */
void sd_mount(void)
{
    if (g_mounted) f_unmount("");
    g_mounted = 0;
    g_mount_err = f_mount(&g_fs, "", 1);
    if (g_mount_err == FR_OK) g_mounted = 1;
}

int sd_is_mounted(void) { return g_mounted; }

/* ---- commands ------------------------------------------------------------------------ */

static void cmd_sd_status(const proto_cmd_t *c)
{
    if (!g_mounted) sd_mount();
    jw_begin(0, (c && c->name[0] == 'F') ? "FORMAT" : (c && c->name[2] == 'M') ? "SDMOUNT" : "SD?", PE_OK);
    jw_obj_open();
    jw_key("mounted"); jw_int(g_mounted);
    if (!g_mounted) {
        jw_key("error"); jw_str(fr_text(g_mount_err));
        jw_key("fr"); jw_int(g_mount_err);
        /* The card answered but has no file system we can mount: describe it (read-only),
         * so it's clear what a format would erase. */
        if (!(disk_status(0) & STA_NOINIT)) {
            LBA_t sectors = 0;
            if (disk_ioctl(0, GET_SECTOR_COUNT, &sectors) == RES_OK) { jw_key("total_kb"); jw_int((long long)sectors / 2); }
            if (disk_read(0, g_io, 0, 1) == RES_OK) {
                jw_key("mbr_signature"); jw_int(g_io[510] == 0x55 && g_io[511] == 0xAA);
                jw_key("partitions"); jw_arr_open();
                for (int k = 0; k < 4; k++) {
                    const BYTE *e = g_io + 446 + 16 * k;
                    if (k) jw_comma();
                    jw_arr_open();
                    jw_int(e[4]); jw_comma();                      /* type: 0x0B/0x0C FAT32, 0x07 exFAT/NTFS, 0x83 Linux, 0xEE GPT */
                    jw_int(e[8] | (e[9] << 8) | (e[10] << 16) | ((uint32_t)e[11] << 24)); jw_comma();
                    jw_int(e[12] | (e[13] << 8) | (e[14] << 16) | ((uint32_t)e[15] << 24));
                    jw_arr_close();
                }
                jw_arr_close();
                char oem[9];
                memcpy(oem, g_io + 3, 8); oem[8] = 0;               /* set if sector 0 is itself a boot sector */
                for (int k = 0; k < 8; k++) if (oem[k] < 0x20 || oem[k] > 0x7e) oem[k] = '.';
                jw_key("sector0_oem"); jw_str(oem);
            }
            /* GPT (protective MBR type 0xEE): list the partitions by name. */
            if (disk_read(0, g_io, 1, 1) == RES_OK && !memcmp(g_io, "EFI PART", 8)) {
                uint32_t entries_lba = g_io[72] | (g_io[73] << 8) | (g_io[74] << 16) | ((uint32_t)g_io[75] << 24);
                uint32_t n_entries = g_io[80] | (g_io[81] << 8) | (g_io[82] << 16) | ((uint32_t)g_io[83] << 24);
                uint32_t esize = g_io[84] | (g_io[85] << 8) | (g_io[86] << 16) | ((uint32_t)g_io[87] << 24);
                jw_key("gpt"); jw_arr_open();
                int shown = 0;
                if (esize == 128 && entries_lba >= 2 && n_entries <= 256) {
                    for (uint32_t blk = 0; blk < 8 && shown < 16; blk++) {      /* first 32 entries */
                        if (disk_read(0, g_io, entries_lba + blk, 1) != RES_OK) break;
                        for (int k = 0; k < 4 && shown < 16; k++) {
                            const BYTE *e = g_io + 128 * k;
                            int used = 0;
                            for (int b = 0; b < 16; b++) used |= e[b];
                            if (!used) continue;
                            char name[37];
                            for (int c = 0; c < 36; c++) {           /* UTF-16LE name, ASCII part */
                                BYTE ch = e[56 + 2 * c];
                                name[c] = (ch >= 0x20 && ch < 0x7f && !e[57 + 2 * c]) ? (char)ch : (ch ? '?' : 0);
                                if (!ch) break;
                            }
                            name[36] = 0;
                            uint32_t first = e[32] | (e[33] << 8) | (e[34] << 16) | ((uint32_t)e[35] << 24);
                            uint32_t last = e[40] | (e[41] << 8) | (e[42] << 16) | ((uint32_t)e[43] << 24);
                            if (shown++) jw_comma();
                            jw_arr_open();
                            jw_str(name); jw_comma();
                            jw_int((long long)(last - first + 1) / 2048); jw_comma();   /* MB */
                            /* type GUID, first 4 bytes: EBD0A0A2 Microsoft data (FAT/exFAT/NTFS),
                             * 0FC63DAF Linux filesystem, C12A7328 EFI system */
                            char guid[9];
                            snprintf(guid, sizeof(guid), "%02X%02X%02X%02X", e[3], e[2], e[1], e[0]);
                            jw_str(guid);
                            jw_arr_close();
                        }
                    }
                }
                jw_arr_close();
            }
        }
    } else {
        DWORD free_clust = 0;
        FATFS *fs;
        FRESULT r = f_getfree("", &free_clust, &fs);
        const char *type = g_fs.fs_type == FS_FAT12 ? "FAT12" : g_fs.fs_type == FS_FAT16 ? "FAT16"
                         : g_fs.fs_type == FS_FAT32 ? "FAT32" : g_fs.fs_type == FS_EXFAT ? "exFAT" : "?";
        jw_key("fs"); jw_str(type);
        char label[24] = "";
        f_getlabel("", label, NULL);
        jw_key("label"); jw_str(label);
        if (r == FR_OK) {
            uint64_t sect = (uint64_t)g_fs.csize * 512u / 1024u;   /* KB per cluster */
            jw_key("total_kb"); jw_int((long long)((uint64_t)(g_fs.n_fatent - 2) * sect));
            jw_key("free_kb"); jw_int((long long)((uint64_t)free_clust * sect));
        }
        /* Card identity, to tell SDSC from SDHC and spot odd cards. */
        BYTE ctype = 0, cid[16];
        if (disk_ioctl(0, MMC_GET_TYPE, &ctype) == RES_OK) {
            jw_key("card");
            jw_str((ctype & CT_SDC2) ? ((ctype & CT_BLOCK) ? "SDHC/SDXC" : "SDSC v2")
                   : (ctype & CT_SDC1) ? "SDSC v1" : (ctype & CT_MMC3) ? "MMC" : "?");
        }
        if (disk_ioctl(0, MMC_GET_CID, cid) == RES_OK) {
            char name[6] = { (char)cid[3], (char)cid[4], (char)cid[5], (char)cid[6], (char)cid[7], 0 };
            for (int k = 0; k < 5; k++) if (name[k] < 0x20 || name[k] > 0x7e) name[k] = '?';
            jw_key("card_name"); jw_str(name);
            jw_key("card_mid"); jw_int(cid[0]);
            jw_key("card_serial"); jw_int(((uint32_t)cid[9] << 24) | ((uint32_t)cid[10] << 16) | ((uint32_t)cid[11] << 8) | cid[12]);
        }
        BYTE ocr[4];
        if (disk_ioctl(0, MMC_GET_OCR, ocr) == RES_OK) {
            jw_key("ocr"); jw_int(((uint32_t)ocr[0] << 24) | ((uint32_t)ocr[1] << 16) | ((uint32_t)ocr[2] << 8) | ocr[3]);
        }
    }
    jw_key("time"); jw_int(now_unix());
    jw_obj_close();
    jw_end();
}

static void cmd_mount(const proto_cmd_t *c)
{
    sd_mount();
    if (g_mounted) cmd_sd_status(c);
    else reply_fr("SDMOUNT", g_mount_err);
}

static void cmd_ls(const proto_cmd_t *c)
{
    const char *path = arg_path(c, 0);
    long start = 0;
    if (!path) path = "/";
    if (c->argc > 1 && proto_arg_int(c, 1, &start) != 0) { reply_msg("LS", PE_INVAL, "Bad start index"); return; }
    if (!sd_ready("LS")) return;
    FRESULT r = f_opendir(&g_dir, path);
    if (r != FR_OK) { reply_fr("LS", r); return; }
    jw_begin(0, "LS", PE_OK);
    jw_obj_open();
    jw_key("path"); jw_str(path);
    jw_key("start"); jw_int(start);
    jw_key("entries"); jw_arr_open();
    long idx = 0, shown = 0;
    int more = 0;
    for (;;) {
        r = f_readdir(&g_dir, &g_fno);
        if (r != FR_OK || g_fno.fname[0] == 0) break;
        if (idx++ < start) continue;
        if (shown == LS_MAX || jw_space() < 600) { more = 1; break; }
        if (shown) jw_comma();
        jw_arr_open();
        jw_str(g_fno.fname); jw_comma();
        jw_int((long long)g_fno.fsize); jw_comma();
        jw_int((g_fno.fattrib & AM_DIR) ? 1 : 0); jw_comma();
        jw_int(fat_to_unix(g_fno.fdate, g_fno.ftime));
        jw_arr_close();
        shown++;
    }
    f_closedir(&g_dir);
    jw_arr_close();
    jw_key("more"); jw_int(more);
    jw_obj_close();
    jw_end();
}

static void cmd_fstat(const proto_cmd_t *c)
{
    const char *path = arg_path(c, 0);
    if (!path) { reply_msg("FSTAT", PE_INVAL, "Usage: FSTAT=\"<path>\""); return; }
    if (!sd_ready("FSTAT")) return;
    FRESULT r = f_stat(path, &g_fno);
    if (r != FR_OK) { reply_fr("FSTAT", r); return; }
    jw_begin(0, "FSTAT", PE_OK);
    jw_obj_open();
    jw_key("name"); jw_str(g_fno.fname);
    jw_key("size"); jw_int((long long)g_fno.fsize);
    jw_key("dir"); jw_int((g_fno.fattrib & AM_DIR) ? 1 : 0);
    jw_key("mtime"); jw_int(fat_to_unix(g_fno.fdate, g_fno.ftime));
    jw_obj_close();
    jw_end();
}

static void cmd_read(const proto_cmd_t *c)
{
    const char *path = arg_path(c, 0);
    long off, len;
    if (!path || proto_arg_int(c, 1, &off) || proto_arg_int(c, 2, &len) || off < 0 || len <= 0) {
        reply_msg("READ", PE_INVAL, "Usage: READ=\"<path>\",<offset>,<length>");
        return;
    }
    if (len > IO_CHUNK) len = IO_CHUNK;
    if (!sd_ready("READ")) return;
    FRESULT r = f_open(&g_file, path, FA_READ | FA_OPEN_EXISTING);
    if (r != FR_OK) { reply_fr("READ", r); return; }
    UINT got = 0;
    FSIZE_t size = f_size(&g_file);
    r = f_lseek(&g_file, (FSIZE_t)off);
    if (r == FR_OK && (FSIZE_t)off < size) r = f_read(&g_file, g_io, (UINT)len, &got);
    f_close(&g_file);
    if (r != FR_OK) { reply_fr("READ", r); return; }
    uint32_t crc = crc32_final(crc32_update(CRC32_INIT, g_io, got));
    jw_begin(0, "READ", PE_OK);
    jw_obj_open();
    jw_key("offset"); jw_int(off);
    jw_key("len"); jw_int(got);
    jw_key("size"); jw_int((long long)size);
    jw_key("crc"); jw_int(crc);
    jw_key("data");
    char *dst = jw_reserve(((got + 2) / 3) * 4 + 2);
    if (!dst) { reply_msg("READ", PE_NOMEM, "Chunk too large"); return; }
    dst[0] = '"';
    uint32_t n = b64_encode(g_io, got, dst + 1);
    dst[n + 1] = '"';
    jw_commit(n + 2);
    jw_obj_close();
    jw_end();
}

static void cmd_write(const proto_cmd_t *c)
{
    const char *path = arg_path(c, 0);
    long off;
    uint32_t want_crc = 0;
    int has_crc = 0;
    if (!path || proto_arg_int(c, 1, &off) || off < 0 || c->argc < 3 || !c->quoted[2]) {
        reply_msg("WRITE", PE_INVAL, "Usage: WRITE=\"<path>\",<offset>,\"<base64>\"[,<crc32>]");
        return;
    }
    if (c->argc > 3) {
        if (proto_arg_u32(c, 3, &want_crc)) { reply_msg("WRITE", PE_INVAL, "Bad CRC"); return; }
        has_crc = 1;
    }
    int32_t n = b64_decode(c->argv[2], (uint32_t)strlen(c->argv[2]), g_io, sizeof(g_io));
    if (n < 0) { reply_msg("WRITE", PE_INVAL, "Bad base64 data, or chunk over 12288 bytes"); return; }
    if (has_crc && want_crc != crc32_final(crc32_update(CRC32_INIT, g_io, (uint32_t)n))) {
        reply_msg("WRITE", PE_INVAL, "CRC mismatch: chunk was damaged on the way, send it again");
        return;
    }
    if (!sd_ready("WRITE")) return;
    FRESULT r = f_open(&g_file, path, off == 0 ? (FA_WRITE | FA_CREATE_ALWAYS) : (FA_WRITE | FA_OPEN_EXISTING));
    if (r != FR_OK) { reply_fr("WRITE", r); return; }
    FSIZE_t size = f_size(&g_file);
    if ((FSIZE_t)off != size) {
        f_close(&g_file);
        char msg[80];
        snprintf(msg, sizeof(msg), "Offset %ld is not the end of the file (size %lu)", off, (unsigned long)size);
        reply_msg("WRITE", PE_INVAL, msg);
        return;
    }
    UINT put = 0;
    r = f_lseek(&g_file, size);
    if (r == FR_OK) r = f_write(&g_file, g_io, (UINT)n, &put);
    FRESULT rc = f_close(&g_file);
    if (r == FR_OK) r = rc;
    if (r == FR_OK && put != (UINT)n) r = FR_DENIED;   /* disk full */
    if (r != FR_OK) { reply_fr("WRITE", r); return; }
    jw_begin(0, "WRITE", PE_OK);
    jw_obj_open();
    jw_key("offset"); jw_int(off);
    jw_key("len"); jw_int(put);
    jw_key("size"); jw_int((long long)(size + put));
    jw_obj_close();
    jw_end();
}

/* Delete a folder and everything in it. path is a writable buffer of PATH_MAX_ bytes. */
static FRESULT delete_tree(char *path)
{
    DIR dir;
    FILINFO fno;
    size_t len = strlen(path);
    FRESULT r = f_opendir(&dir, path);
    if (r != FR_OK) return r;
    for (;;) {
        r = f_readdir(&dir, &fno);
        if (r != FR_OK || fno.fname[0] == 0) break;
        if (len + 1 + strlen(fno.fname) >= PATH_MAX_) { r = FR_INVALID_NAME; break; }
        snprintf(path + len, PATH_MAX_ - len, "/%s", fno.fname);
        r = (fno.fattrib & AM_DIR) ? delete_tree(path) : f_unlink(path);
        path[len] = 0;
        if (r != FR_OK) break;
    }
    f_closedir(&dir);
    if (r == FR_OK) r = f_unlink(path);
    return r;
}

static void cmd_rm(const proto_cmd_t *c)
{
    const char *path = arg_path(c, 0);
    long recursive = 0;
    if (!path || (c->argc > 1 && proto_arg_int(c, 1, &recursive))) {
        reply_msg("RM", PE_INVAL, "Usage: RM=\"<path>\"[,1]");
        return;
    }
    if (!strcmp(path, "/") || !strcmp(path, "")) { reply_msg("RM", PE_PERM, "Refusing to delete the root folder"); return; }
    if (!sd_ready("RM")) return;
    FRESULT r = f_stat(path, &g_fno);
    if (r == FR_OK) {
        if ((g_fno.fattrib & AM_DIR) && recursive) {
            snprintf(g_path, sizeof(g_path), "%s", path);
            r = delete_tree(g_path);
        } else {
            r = f_unlink(path);
        }
    }
    if (r != FR_OK) { reply_fr("RM", r); return; }
    reply_ok("RM");
}

static void cmd_mkdir(const proto_cmd_t *c)
{
    const char *path = arg_path(c, 0);
    if (!path) { reply_msg("MKDIR", PE_INVAL, "Usage: MKDIR=\"<path>\""); return; }
    if (!sd_ready("MKDIR")) return;
    FRESULT r = (FRESULT)sd_mkdirs(path);
    if (r != FR_OK) { reply_fr("MKDIR", r); return; }
    reply_ok("MKDIR");
}

static void cmd_mv(const proto_cmd_t *c)
{
    const char *from = arg_path(c, 0), *to = arg_path(c, 1);
    if (!from || !to) { reply_msg("MV", PE_INVAL, "Usage: MV=\"<from>\",\"<to>\""); return; }
    if (!sd_ready("MV")) return;
    FRESULT r = f_rename(from, to);
    if (r != FR_OK) { reply_fr("MV", r); return; }
    reply_ok("MV");
}

static void cmd_time(const proto_cmd_t *c)
{
    long t;
    if (proto_arg_int(c, 0, &t) || t < 946684800L) { reply_msg("TIME", PE_INVAL, "Usage: TIME=<unix seconds>"); return; }
    g_time_base = (uint32_t)t;
    g_time_ms = millis();
    jw_begin(0, "TIME", PE_OK);
    jw_int(now_unix());
    jw_end();
}

/* FORMAT="FAT32" (or "exFAT"): erase the card and make a fresh file system. Needs the
 * literal argument as a guard against accidents. */
/* Erases the card and makes one fresh partition (MBR) with a FAT32 or exFAT file system.
 * Old GPT headers (start and end of the card) are blanked first, so computers don't keep
 * seeing the previous partition table. Returns an FRESULT. */
int sd_format(BYTE fmt)
{
    if (g_mounted) f_unmount("");
    g_mounted = 0;
    if (disk_status(0) & STA_NOINIT) disk_initialize(0);
    LBA_t sectors = 0;
    if (disk_ioctl(0, GET_SECTOR_COUNT, &sectors) == RES_OK && sectors > 2) {
        memset(g_io, 0, 512);
        disk_write(0, g_io, 1, 1);               /* primary GPT header */
        disk_write(0, g_io, sectors - 1, 1);     /* backup GPT header */
    }
    MKFS_PARM opt = { .fmt = fmt, .n_fat = 1, .align = 0, .n_root = 0, .au_size = 32768 };  /* 32 KB clusters: small FAT, little to write */
    FRESULT r = f_mkfs("", &opt, g_io, sizeof(g_io));
    sd_mount();
    if (r == FR_OK && g_mounted) f_setlabel("GROVE");
    return (int)r;
}

static void cmd_format(const proto_cmd_t *c)
{
    const char *kind = arg_path(c, 0);
    BYTE fmt = 0;
    if (kind && !strcmp(kind, "FAT32")) fmt = FM_FAT32;
    else if (kind && !strcmp(kind, "exFAT")) fmt = FM_EXFAT;
    if (!fmt) { reply_msg("FORMAT", PE_INVAL, "Usage: FORMAT=\"FAT32\" or FORMAT=\"exFAT\" (erases the card)"); return; }
    FRESULT r = (FRESULT)sd_format(fmt);
    if (r != FR_OK) { reply_fr("FORMAT", r); return; }
    cmd_sd_status(c);
}

int sd_handle(const proto_cmd_t *c)
{
    const char *n = c->name;
    if (!strcmp(n, "SD?")) cmd_sd_status(c);
    else if (!strcmp(n, "SDMOUNT")) cmd_mount(c);
    else if (!strcmp(n, "LS")) cmd_ls(c);
    else if (!strcmp(n, "FSTAT")) cmd_fstat(c);
    else if (!strcmp(n, "READ")) cmd_read(c);
    else if (!strcmp(n, "WRITE")) cmd_write(c);
    else if (!strcmp(n, "RM")) cmd_rm(c);
    else if (!strcmp(n, "MKDIR")) cmd_mkdir(c);
    else if (!strcmp(n, "MV")) cmd_mv(c);
    else if (!strcmp(n, "TIME")) cmd_time(c);
    else if (!strcmp(n, "FORMAT")) cmd_format(c);
    else return 0;
    return 1;
}

/* Used by the camera (demo-2) to store tiles. */
int sd_write_file(const char *path, const void *data, uint32_t len)
{
    UINT put = 0;
    FRESULT r = f_open(&g_file, path, FA_WRITE | FA_CREATE_ALWAYS);
    if (r != FR_OK) return (int)r;
    r = f_write(&g_file, data, len, &put);
    FRESULT rc = f_close(&g_file);
    if (r == FR_OK) r = rc;
    if (r == FR_OK && put != len) r = FR_DENIED;
    return (int)r;
}

/* mkdir -p: creates each missing level of path. Returns an FRESULT. */
int sd_mkdirs(const char *path)
{
    snprintf(g_path, sizeof(g_path), "%s", path);
    FRESULT r = FR_OK;
    for (char *p = g_path + 1; ; p++) {
        if (*p == '/' || *p == 0) {
            char saved = *p;
            *p = 0;
            r = f_mkdir(g_path);
            if (r == FR_EXIST) r = FR_OK;
            *p = saved;
            if (r != FR_OK || saved == 0) break;
        }
    }
    return (int)r;
}

const char *sd_error_text(int fr) { return fr_text((FRESULT)fr); }

int sd_exists(const char *path)
{
    FILINFO fno;
    return f_stat(path, &fno) == FR_OK;
}
