#ifndef GROVE_SD_SDCMD_H
#define GROVE_SD_SDCMD_H

#include <stdint.h>
#include "proto.h"

void sd_mount(void);
int sd_is_mounted(void);
/* Handles an SD command. Returns 1 if the command was an SD command (and was answered). */
int sd_handle(const proto_cmd_t *cmd);
/* For the camera: write a whole file / create folders. Return an FRESULT (0 = ok). */
int sd_write_file(const char *path, const void *data, uint32_t len);
int sd_mkdirs(const char *path);
const char *sd_error_text(int fr);
int sd_exists(const char *path);
int sd_format(unsigned char fmt);   /* FM_FAT32 / FM_EXFAT; erases the card */

#endif
