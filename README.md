# grove-lockon-tracker

**An on-device lock-on object tracker for the Seeed Grove Vision AI V2** (Himax WiseEye2 WE2:
Arm Cortex-M55 + Ethos-U55, with an OV5647 camera).

Point the camera, press the button, and whatever is inside the centre box becomes the target. The
board then follows it and reports the **centre (x, y) of the object for every camera frame, about
60 times a second**, over USB. The tracker is class-agnostic: no training, no model, any object. It
keeps the lock while the target moves, turns, deforms and changes size.

Everything runs on the board itself. A computer is only needed to read the position (or to watch
the optional web interface).

## What's in it

- **Firmware** for the Grove Vision AI V2 (`firmware/`), replacing Seeed's SSCMA firmware:
  - **Lock-on tracker:** a MOSSE correlation filter with scale search and loss detection, running at
    the camera's frame rate. It learns the target from one frame and keeps updating as it changes.
  - **Fast camera mode:** the OV5647's own 640×480 mode, tuned to 60–68 fps, with every frame tracked.
  - **Positions-only output:** with video off, only the tracking results cross USB, about 9 KB/s.
  - **A hardware lock-on button** on a header pin.
  - **Extras:** motion detection (save a photo or pulse a pin), photos to a microSD card up to
    2592×1936, an SD card file manager.
  - **An AT command protocol** (SSCMA-compatible framing): [docs/AT-COMMANDS.md](docs/AT-COMMANDS.md).
- **Web interface** (`server.js`, `public/`), Node.js:
  - live view, with the tracked box and its coordinates;
  - lock on / stop;
  - **object suggestion**: before you lock on, it outlines the object it thinks you mean, so the tracker
    learns the object rather than its background;
  - camera settings, photos, SD card files.

  See [docs/WEB-UI.md](docs/WEB-UI.md).
- **Example:** [`examples/track_print.py`](examples/track_print.py) locks on and prints the position
  for every frame. It talks straight to the board with pyserial and needs no server.

## Performance

Measured on the board (QVGA grayscale live view):

| | |
|---|---|
| Camera frame rate | 58.9 fps (60 setting), 68 fps (max) |
| Tracked positions per second | **59** (60 fps camera, with video); **~64** (max camera, positions only) |
| Tracker time per frame | ~14 ms on the Cortex-M55 |
| USB traffic | ~75 KB/s with video, ~9 KB/s positions only |
| Accuracy (generated test sequences) | Moving: 0.8 px. Growing/shrinking ×1.9 / ×0.5: ~1 px. Light ±50%: 0.4 px. Rotating 180°: ~6 px. No lock lost in any |

**Limits:**
- **Exposure.** At 60+ fps the camera can expose for at most ~15 ms, so it needs light. Lower camera
  rates (15–45 fps) expose longer.
- **Lost targets.** Sudden flips, full occlusion, or two similar objects crossing can lose or swap
  the lock. The board reports `lost` instead of guessing.

## Hardware

- **Board:** Seeed Studio Grove Vision AI V2.
- **Camera:** OV5647 (the Raspberry Pi camera v1 type), on the board's CSI connector.
- **Optional:**
  - a microSD card (photos, motion snapshots);
  - a push button from PA0, PB6 or PB7 to GND, with a pull-up to 3.3 V;
  - a Linux computer for the web interface. It was developed on a Jetson Orin Nano; anything that runs Node.js 20+ will do.

## Quick start

1. **Flash the firmware:** no build needed. The board resets itself into its bootloader when the
   port opens, so no buttons are needed:

   ```sh
   pip install pyserial
   python3 firmware/flash.py firmware/prebuilt/grove-lockon-tracker.img      # --port /dev/ttyACM0
   ```

2. **Track from a script:**

   ```sh
   python3 examples/track_print.py --fps 70
   #   1630 locked x= 161 y= 120 w= 80 h= 60 psr= 45.8  (63.3/s)
   ```

3. **Or use the web interface:**

   ```sh
   npm install
   python3 -m venv --system-site-packages .venv && .venv/bin/pip install -r requirements.txt   # object suggestion (optional)
   node server.js          # then open http://<this computer>:3000
   ```

   On the Preview tab: aim the dashed box at the object, press **Lock on**, and watch the box and
   coordinates follow it. `deploy/install.sh` installs it as a service that starts on boot.

## Using the position in your own project

After `AT+TRACK=1`, every camera frame yields one JSON line from the board, either an `INVOKE`
event (a frame with its image) or a `TRACKPOS` event (the result only):

```json
{"type":1,"name":"TRACKPOS","code":0,"data":{"n":1631,"track":{"state":"locked","x":161,"y":120,"w":80,"h":60,"psr":45.8,"ms":14}}}
```

- **`x`, `y`:** the target's centre, in live-frame pixels (320×240).
- **`state`:** `locked` or `lost`.
- **`psr`:** the confidence.

A typical session:

```
AT+PREVIEW=1,320,1,70   grayscale, 320x240, fast camera mode, max frame rate
AT+VIDEO=0              tracking results only over USB
AT+INVOKE=-1,0,1        start the live view
AT+TRACK=1,80,60        lock on the 80x60 box in the centre
...                     one TRACKPOS / INVOKE line per frame
AT+TRACK=0              stop
```

The full reference is in [docs/AT-COMMANDS.md](docs/AT-COMMANDS.md).

## Building the firmware

The firmware builds with Himax's WE2 SDK (MIT) and the Arm GNU toolchain. `firmware/build.sh` copies the
app into the SDK, builds it, and packs the flash image. [firmware/README.md](firmware/README.md)
covers:
- getting the SDK (`firmware/fetch_sdk.sh`);
- building on an ARM host (the SDK's image tool is x86-only and runs under box64 there);
- how each part works: the SD driver, the fast camera mode, the tracker and the motion detector.

`firmware/sim/` has a fake board for testing the web interface without hardware, and a test rig
that runs the tracker on a PC (`track_harness.c`).

## Layout

```
firmware/
  grove_sd_demo/        the firmware app (camera.c, tracker.c, motion.c, sdcmd.c, proto.c, ...)
  fatfs_port/grove_sd/  SD-over-SPI driver
  build.sh, flash.py    build / flash (XMODEM, no buttons)
  prebuilt/             ready-to-flash images
  sim/                  fake board, tracker test rig
server.js, lib/, public/  web interface (lib/jpeg-codec.js: lossless tile joining; lib/suggest_worker.py: object suggestion)
examples/               track_print.py
test/                   object suggestion test (generated scenes)
docs/                   AT-COMMANDS.md, WEB-UI.md
deploy/                 systemd service
```

## Licence

MIT, see [LICENSE](LICENSE). Parts come from Himax's WE2 SDK examples (MIT) and Seeed Studio's
SSCMA-Micro; see [THIRD_PARTY.md](THIRD_PARTY.md).
