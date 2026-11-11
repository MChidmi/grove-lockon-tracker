# Third-party code

| Files | From | Licence |
|---|---|---|
| `firmware/grove_sd_demo/cis_sensor/cis_ov5647/*` (except `OV5647_mipi_2lane_640x480.i`), `hardfault_handler.c`, `grove_sd_demo.ld`, `drv_user_defined.mk`, `common_config.h` | Himax, [Seeed_Grove_Vision_AI_Module_V2](https://github.com/HimaxWiseEyePlus/Seeed_Grove_Vision_AI_Module_V2) SDK examples (`allon_jpeg_encode`, `allon_sensor_tflm_fatfs`), modified | MIT |
| `firmware/grove_sd_demo/cis_sensor/cis_ov5647/OV5647_mipi_2lane_640x480.i` | Seeed Studio, [SSCMA-Micro](https://github.com/Seeed-Studio/SSCMA-Micro) branch 1.0.x (`porting/himax/we2/drivers`) | MIT (headers of that branch); the repository today is Apache-2.0 |

The build also uses, without including them here:
- the WE2 SDK itself (MIT), including FatFs (ChaN, FatFs licence) and CMSIS (Apache-2.0);
- the Arm GNU toolchain.

The web interface uses express, ws, serialport and jpeg-js (MIT) from npm, and OpenCV (Apache-2.0) for the object suggestion.
