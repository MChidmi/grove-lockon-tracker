/*
 * Grove SD demo firmware for the Seeed Grove Vision AI V2 (Himax WE2).
 *
 *   demo-1 (GROVE_DEMO=1): SD card file manager over the USB serial port.
 *   demo-2 (GROVE_DEMO=2): demo-1, plus the camera: a live 640x480 preview stream and
 *                          high-resolution captures (Full HD by default) saved to the SD
 *                          card as 640x480 JPEG tiles.
 *
 * It speaks the same line protocol as Seeed's SSCMA firmware (see proto.h), so the Grove
 * web server can drive it. No AI, no models.
 */
#include <stdio.h>
#include <string.h>
#include "WE2_device.h"
#include "WE2_core.h"
#include "board.h"
#include "xprintf.h"
#include "mmc_we2.h"
#include "proto.h"
#include "sdcmd.h"
#include "grove_sd_demo.h"
#if GROVE_DEMO >= 2
#include "camera.h"
#endif

#define FW_NAME     "grove-sd"
#define FW_VERSION  "0.1.0"

static void reply_version(void)
{
    jw_begin(0, "VER?", PE_OK);
    jw_obj_open();
    jw_key("at_api"); jw_str("v0");
    jw_key("software"); jw_str(FW_NAME "-demo" GROVE_DEMO_STR " " FW_VERSION);
    jw_key("hardware"); jw_str("1");
    jw_key("firmware"); jw_str(FW_NAME);
    jw_key("demo"); jw_int(GROVE_DEMO);
    jw_key("build"); jw_str(__DATE__ " " __TIME__);
    jw_obj_close();
    jw_end();
}

static void reply_stat(const char *name)
{
    jw_begin(0, name, PE_OK);
    jw_obj_open();
    jw_key("boot_count"); jw_int(0);
    jw_key("is_ready"); jw_int(1);
    jw_key("sd"); jw_int(sd_is_mounted());
    jw_obj_close();
    jw_end();
}

static void reply_help(void)
{
    static const char *const lines[] = {
        "  AT+ID? / AT+NAME? / AT+VER? / AT+STAT?    device info",
        "  AT+SD?                                    SD card status",
        "  AT+SDMOUNT                                mount the card again (after swapping it)",
        "  AT+LS=\"<dir>\"[,<start>]                   list a folder",
        "  AT+FSTAT=\"<path>\"                         file info",
        "  AT+READ=\"<path>\",<offset>,<len>           read up to 12288 bytes (base64)",
        "  AT+WRITE=\"<path>\",<offset>,\"<b64>\"[,<crc>]  write/append a chunk",
        "  AT+RM=\"<path>\"[,1]                        delete (,1 = folder and contents)",
        "  AT+MKDIR=\"<path>\"                         create a folder",
        "  AT+MV=\"<from>\",\"<to>\"                     rename or move",
        "  AT+TIME=<unix seconds>                    set the clock for file timestamps",
#if GROVE_DEMO >= 2
        "  AT+INVOKE=-1,0,0 / AT+BREAK               start / stop the live view (see PREVIEW)",
        "  AT+CAPTURE[=<w>,<h>[,<x>,<y>[,<sub>[,<d>]]]] capture to the SD card (sub 1|2|4|8 scales down; one tile = full.jpg)",
        "  AT+PREVIEW=<gray>[,<w>[,<fast>[,<fps>]]]   live view: gray 1|0, 320|640, fast camera mode 1|0, 15|30|45|60|70",
        "  AT+MOTION=<on>,<action>[,<pin>[,<ms>[,<sens>]]] / AT+MOTION?  motion detection (action 0 none, 1 save, 2 pulse)",
        "  AT+MOTIONTEST / AT+PULSE                  run the motion action once / one pulse on the pin",
        "  AT+FRAME?                                 the latest live frame as raw grayscale (base64)",
        "  AT+FORMAT=\"FAT32\"|\"exFAT\"              erase and format the SD card",
        "  AT+CAMRESET                               power-cycle and set up the camera again",
        "  AT+TRACK=1[,<w>,<h>[,<x>,<y>]] / =0 / AT+TRACK? lock on a box of the live view (default centre) / stop / position",
        "  AT+TRACKBTN=<pin>[,<level>]               a header pin (0 PA0, 1 PB6, 2 PB7) as the lock-on button; -1 off",
        "  AT+VIDEO=<fps> / AT+VIDEO?                live frames sent per second (0 = tracking results only)",
        "  AT+CAMREG?=<reg> / AT+CAMREG=<reg>,<val>  read / write an OV5647 register (diagnostic)",
#endif
        "  AT+RST                                    reboot",
    };
    for (unsigned i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
        proto_write(lines[i], (uint32_t)strlen(lines[i]));
        proto_write("\r\n", 2);
    }
}

static void board_reboot(void)
{
    proto_write("Rebooting\r\n", 11);
    proto_tx_flush();
    for (volatile int i = 0; i < 2000000; i++) {}   /* let the UART drain */
    NVIC_SystemReset();
}

static void dispatch(char *line)
{
    proto_cmd_t c;
    if (proto_parse(line, &c) != 0) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Could not parse: %.60s", line);
        reply_msg("AT", PE_INVAL, msg);
        return;
    }
    const char *n = c.name;
    if (!strcmp(n, "ID?")) { jw_begin(0, "ID?", PE_OK); jw_str(FW_NAME); jw_end(); }
    else if (!strcmp(n, "NAME?")) { jw_begin(0, "NAME?", PE_OK); jw_str("Grove Vision AI V2"); jw_end(); }
    else if (!strcmp(n, "VER?")) reply_version();
    else if (!strcmp(n, "STAT?")) reply_stat("STAT?");
    else if (!strcmp(n, "HELP?")) reply_help();
    else if (!strcmp(n, "RST")) board_reboot();
    else if (!strcmp(n, "CLOCKS?")) {     /* diagnostics: SD SPI clock */
        uint32_t now_hz, set_hz; int crc;
        mmc_spi_clock_info(&now_hz, &set_hz, &crc);
        jw_begin(0, "CLOCKS?", PE_OK);
        jw_obj_open(); jw_key("sspim_hz"); jw_int(now_hz); jw_key("sd_set_for_hz"); jw_int(set_hz);
        jw_key("sd_crc"); jw_int(crc); jw_obj_close();
        jw_end();
    }
    else if (sd_handle(&c)) {}
#if GROVE_DEMO >= 2
    else if (camera_handle(&c)) {}
#else
    else if (!strcmp(n, "BREAK")) reply_ok("BREAK");
    else if (!strcmp(n, "INVOKE?")) { jw_begin(0, "INVOKE?", PE_OK); jw_int(0); jw_end(); }
#endif
    else {
        char msg[64];
        snprintf(msg, sizeof(msg), "Unknown command: AT+%s", n);
        jw_begin(2, "AT", PE_INVAL);
        jw_str(msg);
        jw_end();
    }
}

int app_main(void)
{
    proto_init();
    xprintf("Grove SD demo %d (" FW_VERSION ", built " __DATE__ " " __TIME__ ")\r\n", GROVE_DEMO);
    /* The camera powers up on first use (preview or capture), not here. */
    sd_mount();
    xprintf("SD card: %s\r\n", sd_is_mounted() ? "mounted" : "not mounted");
    reply_stat("INIT@STAT?");   /* same "ready" announcement SSCMA sends at boot */

    uint32_t hb_ms = millis();
    for (;;) {
        proto_service();
        char *line = proto_poll_line();
        if (line) dispatch(line);
        /* Diagnostic heartbeat: if nothing has arrived for 10 s, say so (every 10 s). It shows
         * whether a board that stopped answering is still running, and whether bytes still
         * reach it. Quiet while the preview streams. */
        uint32_t now = millis();
#if GROVE_DEMO >= 2
        if (camera_is_streaming()) hb_ms = now;      /* frames going out already show it's alive */
#endif
        if (now - g_rx_last_ms > 10000 && now - hb_ms > 10000) {
            hb_ms = now;
            xprintf("heartbeat: no input for %lu s, %lu bytes received so far\r\n",
                    (unsigned long)((now - g_rx_last_ms) / 1000), (unsigned long)g_rx_total);
        }
#if GROVE_DEMO >= 2
        camera_poll();
        camera_button_poll();
#endif
    }
    return 0;
}
