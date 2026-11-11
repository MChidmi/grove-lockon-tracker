#ifndef GROVE_SD_CAMERA_H
#define GROVE_SD_CAMERA_H

#include "proto.h"

/* demo-2 only (GROVE_DEMO >= 2) */
void camera_init(void);
void camera_poll(void);                     /* sends preview frames while streaming */
int camera_handle(const proto_cmd_t *cmd);  /* INVOKE, INVOKE?, BREAK, CAPTURE; 1 if handled */
int camera_is_streaming(void);   /* 1 while the preview stream runs */
void camera_button_poll(void);   /* the lock-on button (TRACKBTN) */

#endif
