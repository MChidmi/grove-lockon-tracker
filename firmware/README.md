# Firmware

Custom firmware for the Seeed Grove Vision AI V2 (Himax WE2, Cortex-M55) with an OV5647 camera,
replacing Seeed's SSCMA firmware. It runs no neural network; the tracker and the motion detector
are classic image processing on the M55.

It's built in two variants (`GROVE_DEMO`):

- **1: SD card file manager** over the USB serial port: list, read (download), write (upload),
  delete, make folders, rename.
- **2: the full firmware** (`prebuilt/grove-lockon-tracker.img`). Everything in 1, plus the camera:
  - **Lock-on object tracking** on every camera frame (about 60 per second): see *Object tracking*
    and *The fast live view* below.
  - **A live view** of the whole field of view: 320×240 (default) or 640×480, grayscale (default)
    or colour, up to 68 fps from the camera, with video to the host limited to what the serial link
    carries (or off).
  - **Motion detection,** which can save a photo or pulse a pin.
  - **Photos to the SD card** at any size from 160×120 to 2592×1936. VGA and QVGA are saved by the
    board directly; larger sizes are joined by the host.

All commands are in [../docs/AT-COMMANDS.md](../docs/AT-COMMANDS.md).

## Build and flash

```sh
./build.sh 1 --flash      # or: ./build.sh 2 --flash
```

Before flashing, press **Release port** in the website's Settings, or stop the server. Only one program can use the port.

`build.sh` does three things:

1. It copies `grove_sd_demo/` into the Himax SDK at `$GROVE_FW_DIR/sdk` (default `~/grove-ai-fw/sdk`; get it with `./fetch_sdk.sh`) as a scenario app, and `fatfs_port/grove_sd/` in as a FatFs port.
2. It compiles with the Arm GNU 13.2 toolchain (arm-none-eabi, from developer.arm.com) unpacked in `$GROVE_FW_DIR/toolchain`, which takes about 10 s.
3. It packages the result with the SDK's `we2_local_image_gen`. The image ends up at `out/grove-sd-demo<N>.img`.

That packaging tool is x86-64 only, so on this ARM Jetson it runs through `box64` 0.2.6, with two workarounds:

- **Its helper tools.** Under this box64, x86 programs mis-parse their command-line options. So the tool's bundled `objcopy`/`objdump` in `we2_image_gen_local/arm_none_eabi/` are swapped for native ARM64 binutils, with the originals kept as `*.x86`. They must be plain files, never symlinks, because the tool copies them over its own folder on every run.
- **`BOX64_PREFER_EMULATED=1`.** Without it, writing `layout.xml` crashes inside box64's native expat wrapper.

## The SD card driver

`fatfs_port/grove_sd/sd_spi.c` is this firmware's own SD-over-SPI driver. It replaces the SDK's `middleware/fatfs/port/mmc_spi`, which is never compiled, and the app selects it with `FATFS_PORT_LIST = grove_sd`.

The SDK driver didn't work with the card in this board. Reads worked, but the card locked up busy before the first write, and only a power cycle recovered it. The SDK driver received data in the SPI block's receive-only mode, which sends a "dummy" byte on the card's data-in line instead of the 0xFF the SD spec requires. It also asserted (halting the firmware) when the card was initialised a second time, and it had untimed waits.

The new driver is a clean implementation of SD SPI mode:

- **Transfers:** everything is full duplex, sending 0xFF while receiving.
- **Chip select:** a GPIO (PB5 = GPIO16) driven directly.
- **Waits:** each one has a time limit.
- **Re-initialising:** safe, for remounts and card swaps.

It has been tested on a 4 GB SDHC card (FAT16) and a 64 GB SDXC card (FAT32). Set `SD_DEBUG 1` at the top of the file to log the init steps to the serial port.

Other driver details:

- **CRC checking:** CMD59 turns it on, with CRC7 on commands and CRC16 on data.
- **Retries:** reads and writes are retried 3 times.
- **Writes:** single-block only.
- **SPI clock:** the driver re-applies it if the system clock changes.

## Going back to Seeed's firmware

```sh
python3 flash.py grove_vision_ai_v2_20250102.img      # Seeed's image, from the SenseCraft / SSCMA releases
```

That is the official image the board shipped with (SSCMA 2025.01.02, SHA-256 `bf96bc0a…f8345`). The detection model stays in flash at 0x400000, because the firmware slots don't overlap it. The SenseCraft AI web tool can also restore the firmware.

## Why Full HD is captured in tiles

The WE2's image pipeline (demosaic, then JPEG encoder) handles 640×480 at most. Himax's own `allon_jpeg_encode` example saves a 1280×960 capture as four 640×480 JPEGs. So a capture works like this:

1. The sensor runs in its full 2592×1944 mode. For each tile, the WE2's input crop is moved to a new 640×480 window at full resolution.
2. Before the first tile, frames run until the sensor's auto exposure stops changing (at least 0.8 s after the sensor starts). Otherwise the first tiles come out darker.
3. Each tile is its own sensor frame. After moving the crop, the first two frames are dropped. If the datapath reports an error, the tile is taken again, up to 3 times.
4. The region is covered by `ceil(w/640) × ceil(h/480)` tiles. The last column and row are pulled back so they overlap instead of running past the edge. Full HD takes 3×3 = 9 tiles, and the full sensor takes 5×5 = 25. All offsets are even, so the colour pattern stays aligned.
5. The tiles are saved as `/DCIM/IMG_nnnn/r<row>c<col>.jpg`, with `tiles.json` describing them.
6. The server downloads the tiles and joins them, in `lib/stitch-worker.js`. It scores each tile for pixel-level colour grain. A tile far grainier than the rest is one whose colour pattern came through shifted, so the server takes it again (`CAPTURE=640,480,x,y`), up to twice. It then writes `full.jpg` back into the same folder and keeps a copy in `../captures/`.

Three camera settings matter here:

- **The MIPI receiver runs at 200 MHz (from the PLL), not 96 MHz.** At 96 MHz, a 2592-pixel line takes about 27 µs to leave the receiver but arrives in about 19 µs. The receiver's FIFO then dropped a line now and then, and every dropped line swapped the colour order for the rest of the frame (pink/green bands). Himax's own IMX708/IMX477 drivers use the PLL for the same reason.
- **The last 8 sensor lines are never captured.** A crop that ends on line 1944 often fails with datapath error -40 (WDMA2 frame end not reached): 4 errors in 8 tiles, against 0 in 8 for tiles ending 8 lines higher. So the largest capture is 2592×1936.
- **MCLK is set to always run** (`SENSORCTRL_MCLKCTRL_NONAOS`). The default mode gates it whenever the sensor controller is idle, and the OV5647 doesn't answer I2C without it.

What a size shows depends on how large it is (`planCapture()` in `lib/sd.js`):

| Size | How it is taken | Tiles |
|---|---|---|
| Up to 320×240 | The whole view (2560×1920 of the sensor), subsampled 8× on the board (`sub` 8) into one 320×240 picture | 1 |
| Up to 640×480 | The whole view, subsampled 4× (`sub` 4) into one 640×480 picture | 1 |
| Up to 1280×960 | The whole view, subsampled 2× (`sub` 2) | 4 |
| Larger | Native pixels from the centre of the sensor (`sub` 1) | up to 25 |

How `full.jpg` is made:

- **Saved directly (QVGA 320×240, VGA 640×480):** the board's single picture is already the finished photo. The board writes it as `full.jpg`, with no tiles and no `tiles.json`, and the server only copies it to `../captures/`.
- **Joined without re-compressing (every size that needs no scaling: 1280×720, 1280×960, and all the centre crops):** `lib/jpeg-codec.js` copies the tiles' compressed 8×8 blocks into one JPEG and only redoes the Huffman coding, with tables optimized for that image. The pixels are exactly the board's. The file is smaller than the tiles together, because the overlaps are dropped and the Huffman tables are optimized. The board's areas are kept on the JPEG's 16-pixel block grid for this: 1920×1080 is taken as 1920×1088 and the bottom 8 lines are cut off.
- **Scaled (160×120, 800×600, 1024×768):** the tiles are decoded, joined and shrunk with area averaging, then encoded once. This uses 4:2:0 colour and the board's own quantization tables, so the quality matches the tiles. Re-encoding a tile this way gives 54.6 dB PSNR against the original.

For comparison: before this, everything was re-encoded with jpeg-js at quality 92 and full-resolution colour. A 640×480 `full.jpg` was then 72 KB from a 34 KB tile, and 1280×720 was 209 KB, where it is now 85 KB.

Full HD is therefore a centred crop at native resolution, not a scaled-down full frame. The OV5647's own 1080p mode works that way too. It shows about 74% of the lens's horizontal field of view (1920 of 2592 pixels). For a smaller size, the server first crops the board's image to the target's aspect ratio, then shrinks it with area averaging.

## Protocol

This firmware uses the same framing as Seeed's SSCMA firmware, so the server's driver works with both:

- **Commands:** `AT+NAME[?|=args]\r\n`. Arguments are `"quoted strings"` with `\"` and `\\` escapes, or plain integers.
- **Replies:** `\r{"type":0,"name":"NAME","code":0,"data":…}\n`.
- **Events:** the same format with `"type":1`.
- **At boot:** the firmware announces itself with `INIT@STAT?`, like SSCMA.
- **Identity:** `VER?` returns `"firmware":"grove-sd"` and `"demo":1|2`.

| Command | What it does |
|---|---|
| `SD?` / `SDMOUNT` | Card status: file system, label, size, free space. `SDMOUNT` mounts the card again after a swap |
| `LS="<dir>"[,<start>]` | List up to 40 entries per call, as `[name,size,is_dir,mtime]`, plus `more` |
| `FSTAT="<path>"` | Info about one file |
| `READ="<path>",<off>,<len>` | Up to 12288 bytes as base64, with a CRC32 |
| `WRITE="<path>",<off>,"<b64>",<crc>` | Offset 0 creates or truncates the file. Otherwise it appends at the end. The CRC32 is checked |
| `RM="<path>"[,1]` / `MKDIR` / `MV` | Delete (`,1` deletes a folder with its contents) / make folders, including missing parents / rename |
| `TIME=<s>` | Sets the clock used for file timestamps. The server sends this on connect |
| `INVOKE=-1,0,<0\|1>` / `BREAK` / `INVOKE?` | (demo-2) Start, stop or query the preview stream. `INVOKE` events carry a base64 JPEG |
| `CAPTURE[=w,h[,x,y[,sub[,direct]]]]` | (demo-2) Tiled capture to `/DCIM`. `w`×`h` is the result's size, at least one tile. `sub` is 1, 2, 4 or 8: the sensor window is `w·sub`×`h·sub` at (`x`,`y`), subsampled to size. Tiles are 640×480, or 320×240 at `sub` 8 (only 320×240 allowed). With `direct` 1 (the default), a one-tile capture is saved as `full.jpg` with no `tiles.json`. It sends `CAPTURE` progress events, then replies with the manifest (`"direct":1` for those) |
| `PREVIEW=<1\|0>[,<320\|640>[,<fast>[,<fps>]]]` / `PREVIEW?` | (demo-2) Live view in grayscale (1, the default) or colour, at 320×240 (the default) or 640×480. `fast` 1 (the default) runs the camera in its 640×480 mode, at `fps` 15, 30, 45 or 60 (the default). `PREVIEW?` also reports frames processed and sent, and the average wait and work time per frame |
| `CAMREG?=<reg>` / `CAMREG=<reg>,<val>` | (demo-2, diagnostic) Read or write an OV5647 register |
| `CAMRESET` | (demo-2) Turns the camera's power off and on (AON_GPIO1), sets it up again, and reports the chip id with the power off and on |
| `FORMAT="FAT32"\|"exFAT"` | **Erases the card.** Clears the partition tables, including GPT, then makes one file system labelled `GROVE`. The website asks you to type ERASE first |
| `SDTEST` | (demo-2, diagnostic) Raw write test at LBA 2,000,000 and up, in four stages: camera off, clocks set, camera powered, preview running. `SDTEST="FORMAT"` formats the card as FAT32 if every stage passes |
| `CLOCKS?` | Diagnostic: the SD SPI source clock and whether CRC is on |
| `MOTION=<on>,<action>[,<pin>[,<ms>[,<sens>]]]` / `MOTION?` | (demo-2) Motion detection on the live view. Action 0 nothing, 1 save a VGA photo to `/MOTION`, 2 pulse a pin (0 PA0, 1 PB6, 2 PB7) for `ms`. Sensitivity 1–100. Sends a `MOTION` event per detection (at most one per 2 s) |
| `MOTIONTEST` / `PULSE` | (demo-2) Run the selected motion action once / send one pulse on the selected pin |
| `TRACK=1[,<w>,<h>]` / `TRACK=0` / `TRACK?` | (demo-2) Lock on whatever is inside the centre box (default ¼ of the live frame's width, 4:3) at the next live frame / stop / latest result |
| `TRACKBTN=<pin>[,<level>]` | (demo-2) A header pin as the lock-on button (0 PA0, 1 PB6, 2 PB7; -1 off), pressed = `level` (default 0, i.e. a button to GND with a pull-up). Press: lock on, or stop if tracking |
| `FRAME?` | (demo-2) The live view's latest grayscale frame, uncompressed, as base64 (diagnostic) |
| `RST` | Reboot |

FAT32 and exFAT cards both work. File names are UTF-8.

## Motion detection

`motion.c` reads only the DC coefficients (8×8 block averages) of each live JPEG, so it costs about 1 ms per frame. It averages them into a 20×15 grid and compares that with a slowly updated background, after scaling the background by the median brightness ratio, so exposure changes don't count. Motion = at least 3 cells changed in two frames in a row. Tested with generated frames (`sim/` harness): a 40×60 px object triggers it, noise and brightness changes up to ±25% don't.

## The fast live view (60 fps)

The live view runs the camera in its own 640×480 mode (a register table from Seeed's SSCMA-Micro), not the 5 MP mode, and switches back to 5 MP only while it takes a photo. That costs about 0.2 s per switch, plus the exposure settle.

- **Frame rate.** It's set with the sensor's frame length (VTS register 0x380E/F): 1968 lines → 15.9 fps, 1070 → 29.4, 712 → 44.2, 535 → 58.8. The exposure can't be longer than a frame, so the allowed number of anti-flicker steps (0x3A0D/0x3A0E) follows the frame rate. In the dark, 15 fps exposes up to 1757 lines, against 502 at 60 fps.
- **No frame is missed while tracking.** Frames are copied into one of two slots. The camera is re-armed as soon as a frame is ready, even in the middle of the tracker's work (it pauses ~250 times per frame to collect received bytes and to take the next frame).
- **Output never blocks.** Everything the board sends goes through a 32 KB queue that the main loop drains as the UART's transmit FIFO has room. The SDK's `xprintf` goes through it too, so log lines never land inside a frame.

Measured (QVGA grayscale live view): 58.9 frames per second processed, 35 sent without tracking. With tracking: 59 tracked per second (none skipped in 471), about 30 sent, tracker 14 ms per frame, and 120 of 120 commands answered meanwhile. With the 640×480 live size, about 30 per second.

## Object tracking

`tracker.c` is a class-agnostic single-object tracker: a MOSSE correlation filter with scale search, in plain C. It runs on the live view's frames, using the uncompressed grayscale copy of each frame that the WE2 writes next to the JPEG (WDMA3), so tracking costs no extra camera frames.

- **Lock-on:** `TRACK=1`, the page's Lock on button, or a header button (`TRACKBTN`). Whatever is inside the centre box becomes the target.
- **Each frame:** the window around the last position is correlated with the learned filter at 3 scales. The peak gives the new centre, to a fraction of a pixel, and the size. The filter then learns the new appearance a little, which follows gradual rotation, deformation and lighting changes.
- **Colour model** (colour live view, `PREVIEW=0,320,…`): MOSSE sees brightness only. In colour mode the WDMA3 copy is YUV420 (Y, then U and V planes), and the tracker also keeps two colour histograms (4 brightness × 16 U × 16 V bins): the target's and its surroundings'. They give each colour a "target" likelihood. Every frame the likelihood over the search window, averaged over the target's size, is blended into MOSSE's response (weight `TRACKCOLOR`, default 60%) before the peak is taken. This is the approach of the Staple tracker (Bertinetto et al., CVPR 2016). The weight is scaled down by how well the histograms separate target from background, so in a dull or dark scene colour steps out of the way. Lost / found still go by MOSSE's PSR alone. Cost on the board: ~1.5 ms per frame (55 instead of 59 positions per second at the 60 fps setting with video).
- **Confidence:** the peak-to-sidelobe ratio (PSR). Below 5.5 for 8 frames the target counts as lost: the board reports it once and stops tracking.
- **Speed:** at the default settings the board tracks every camera frame, **about 59 per second**. Live frames go to the page at up to ~30 per second; frames that don't fit on the serial link send only their tracking result, as a `TRACKPOS` event with the same `"track"` field.
- **Output:** every live frame carries `"track":{"state":"locked","x":…,"y":…,"w":…,"h":…,"psr":…,"ms":…}`. `x`, `y` is the centre of the target in live-frame pixels (320×240 by default). `TRACK` events report state changes (`locked`, `lost`, `off`), and the page draws the box and centre.

Measured:

| Test | Result |
|---|---|
| PC, generated 320×240 sequences, 60 frames each | Moving: 0.8 px mean centre error. Growing ×1.9 / shrinking ×0.53: 0.7 / 0.9 px. Light changing ±50%: 0.4 px. Rotating 180° while moving: 5.3 px mean, 12 px max. Fast (4.7 px/frame + rotation + growth): 6.7 px mean, 18 px max. No lock lost in any |
| Board, still scene | Centre steady to 1 px over 90+ frames, PSR 38–99, 25 ms per frame on the M55. Live view unchanged at ~8 fps (the tracker runs while the next frame is taken) |

`sim/track_harness.c` builds `tracker.c` on a PC and runs it over raw YUV420 frames (`-c`: with colour), for testing changes before flashing. `sim/track_eval.py <photo folder>` generates test videos from real photos (moving, turning, deforming, same-brightness, look-alikes crossing, occlusion, light changes, colourless) and scores the tracker with and without colour:

| Scenario (6 videos × 300 frames each) | Brightness only | With colour (60%) |
|---|---|---|
| Moving | 84.3% | 94.4% |
| Turning a full circle | 48.0% | 62.0% |
| Stretching / tilting | 90.2% | 100% |
| Same brightness as the background, other colour | 63.4% | 78.4% |
| Two look-alikes (other colours) crossing | 83.9% | 99.9% |
| A bar passing over half of it | 85.6% | 89.0% |
| Light ±40% | 100% | 100% |
| Nearly colourless scene | 100% | 100% |
| **All** | **81.9%** | **90.5%** |

(Frames where it was locked within 10 px of the true centre; a lost ends the run.) Turning stays the hardest: the filter tends to follow a textured spot on the object, ~14 px off its centre, that circles round as it turns. Pulling the estimate towards the middle of the target-coloured pixels was tried and made the other cases worse, so it isn't in.

## Testing without the board

`sim/fake_board.py` imitates this firmware on a virtual serial port, using a folder as the SD card and a synthetic camera:

```sh
python3 sim/fake_board.py --sd /tmp/fake-sd --demo 2 --link /tmp/grove-sim
SERIAL_PORT=/tmp/grove-sim PORT=3001 node ../test/full-web-ui/server.js   # the full test interface
```

## Status

**demo-1 tested on the real board** on 2026-11-06, with the 4 GB SDHC card ("APPSD", FAT16):

| Test | Result |
|---|---|
| Mount, card info, folder listing | Works |
| Upload then download 5 KB, 100 KB, 1 MB | Byte-for-byte identical |
| Speed | About 60 KB/s up and 56 KB/s down, so 1 MB takes about 17–18 s |
| UTF-8 file name, empty file, new folder, rename/move | Works |
| Recursive delete | Works |
| Missing file, non-empty folder delete | Proper errors |
| Website Files tab | Works |

**demo-2 tested on the real board** on 2026-11-06, with a 64 GB SDXC card formatted FAT32 on the board:

| Test | Result |
|---|---|
| Live view | QVGA 320×240 at about 9.3 fps, grayscale or colour (6 KB per frame). Here the camera is the limit. VGA 640×480 at about 3–6 fps (10–16 KB per frame), limited by the 921600-baud serial link. The next frame is taken while the current one is being sent |
| VGA / QVGA photo, saved directly | About 2.6 s / 2.1 s end to end. 34 KB / 10 KB, nothing but `full.jpg` in the folder |
| 1280×720, 1920×1080 photo, joined losslessly | About 7 s / 14 s. 85 KB / 176 KB |
| Full HD 1920×1080 (9 tiles) | About 12–15 s end to end: about 4 s taking tiles, 3 s downloading them, 0.6 s joining, 5 s writing `full.jpg` back. About 250–300 KB |
| 5 MP 2592×1936 (25 tiles) | About 26–30 s. About 520 KB |
| Colour bands, dark first tiles | None in over 40 captures since the 200 MHz receiver clock and the exposure settle |
| Soft reboot (`RST`) | The camera comes back each time |

Known problems:

- **Board freeze while saving `full.jpg`.** Now and then the board stops answering in the middle of the server's `WRITE` sequence: 2 of 16 captures in one run, 0 of 17 in the next. It even ignores `RST`. The website's watchdog recovers it by itself, with a hardware reset through the serial port, in about 1–2 minutes, but that capture is lost (its tiles stay on the card). It only happens in demo-2 and only during the host's writes. The board's own tile writes never froze. The firmware prints `heartbeat: no input for N s` when nothing has arrived for 10 s while the preview is off. At the next freeze, that will show whether the board is still running (its serial input died) or has hung.
- **Deaf camera.** Now and then the OV5647 stops answering I2C (-64), and the board reports "The camera is not answering". Turning its power off and on (AON_GPIO1), soft reset and hardware reset didn't fix it. Re-flashing, or a few minutes' wait, always did, and a USB replug should. It might be the ribbon cable or heat.
- The tiles are taken one after another, about 0.25 s apart, so anything that moves during that time shows a seam.
