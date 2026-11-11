#ifndef GROVE_SD_MOTION_H
#define GROVE_SD_MOTION_H

#include <stdint.h>
#include "proto.h"

#define MOTION_NOTHING 0
#define MOTION_SAVE    1
#define MOTION_PULSE   2

int motion_frame(const uint8_t *jpg, uint32_t size);  /* 1 = motion event (cooldown applied) */
void motion_restart(void);                              /* relearn the background */
void motion_poll(void);                                 /* ends a running pulse */
void motion_pulse(void);                                /* one pulse on the selected pin */
int motion_score(void);                                 /* changed cells in the last frame, -1 = off */
int motion_cells(void);
int motion_action(void);
const char *motion_action_name(void);
int motion_handle(const proto_cmd_t *c);                /* MOTION, MOTION?, PULSE; 1 if handled */

#endif
