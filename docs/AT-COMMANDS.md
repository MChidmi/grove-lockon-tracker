# AT commands

The firmware talks over the board's USB serial port (CH343, 921600 baud, 8N1). The framing follows
Seeed's SSCMA firmware, so the same host code can drive both.

## Framing

- **Command:** `AT+NAME`, `AT+NAME?` or `AT+NAME=args`, ending with `\r\n`. Arguments are separated by
  commas. They are plain integers or `"quoted strings"`; inside quotes, `\"` and `\\` are escapes.
  Lines can be up to 16 KB long.
- **Reply:** one line, `\r{"type":0,"name":"NAME","code":0,"data":…}\n`. `name` repeats the
  command (with its `?`).
- **Event:** the same format with `"type":1`. Events are not replies; the board sends them on its own.
- **Log lines:** anything not starting with `\r{` is a log line and can be ignored.
- **At boot:** the board announces itself with the event-style line `INIT@STAT?`.

### Reply codes

| code | name | meaning |
|---|---|---|
| 0 | OK | done |
| 1 | AGAIN | try again |
| 2 | LOGIC | not possible in the current state |
| 3 | TIMEOUT | timed out |
| 4 | IO | hardware / SD card / camera error (`data` is a message) |
| 5 | INVAL | bad arguments (`data` is the usage) |
| 6 | NOMEM | out of memory, or the line was too long |
| 7 | BUSY | busy |
| 8 | NOTSUP | unknown command |
| 9 | PERM | not allowed |

## Device

| Command | Reply `data` |
|---|---|
| `ID?` | `"grove-sd"` |
| `NAME?` | `"Grove Vision AI V2"` |
| `VER?` | `{"software", "firmware":"grove-sd", "demo":2, "built"}` |
| `STAT?` | `{"boot_count", "is_ready", "sd"}` |
| `HELP?` | the command list, as log lines |
| `RST` | reboots (reply first) |
| `CLOCKS?` | diagnostic: SD SPI clock and CRC state |

## Live view

| Command | What it does |
|---|---|
| `INVOKE=-1,0,<n>` | Starts the live view: a stream of `INVOKE` events, one per frame (`<n>` = 1: no image in them) |
| `BREAK` | Stops the live view (and tracking) |
| `INVOKE?` | `1` while it runs |
| `PREVIEW=<gray>[,<w>[,<fast>[,<fps>]]]` | `gray` 1 grayscale (default) / 0 colour; `w` 320 (default) or 640; `fast` 1 (default) = the camera's own 640×480 mode, 0 = cropped from the 5 MP mode; `fps` 15, 30, 45, 60 (default) or 70 (= max, 68 fps, anti-flicker off) |
| `PREVIEW?` | The above, plus `processed` / `sent` frame counters and `wait_ms` / `work_ms` (average time per frame waiting for the camera / working on it, since the last `PREVIEW?`) |
| `VIDEO=<fps>` / `VIDEO?` | Live frames sent per second at most (1–60, default 30). `0` = tracking results only, the least USB traffic and the lowest latency |
| `FRAME?` | The latest live frame as raw 8-bit grayscale: `{"width","height","ms","image":"<base64>"}` |
| `CAMRESET` | Power-cycles the camera and sets it up again: `{"id_power_off","id_power_on","ok"}` (chip id 0x5647 = 22087) |
| `CAMREG?=<reg>` / `CAMREG=<reg>,<val>` | Diagnostic: read or write an OV5647 register |

**`INVOKE` event** (one per sent frame):

```json
{"type":1,"name":"INVOKE","code":0,"data":{
  "count":812, "perf":[0,0,16], "resolution":[320,240], "motion":-1, "n":1630,
  "boxes":[[161,120,80,60,100,0]],
  "track":{"state":"locked","x":161,"y":120,"w":80,"h":60,"psr":45.8,"ms":14},
  "image":"<base64 JPEG>"}}
```

- **`n`:** the frame number. All frames are processed, but not all are sent.
- **`motion`:** the motion score, or -1 when motion detection is off.
- **`boxes`:** SSCMA-style `[centre x, centre y, w, h, score, target]`.

## Tracking

| Command | What it does |
|---|---|
| `TRACK=1[,<w>,<h>[,<x>,<y>]]` | Locks on whatever is in the box of `w`×`h` centred at `x`,`y` (default: the frame centre, a quarter of the frame width, 4:3) at the next live frame. Needs the live view running |
| `TRACK=0` | Stops tracking |
| `TRACK?` | `{"state","x","y","w","h","psr"(×10),"ms","frame":[w,h]}` |
| `TRACKBTN=<pin>[,<level>]` | A header pin as the lock-on button: 0 PA0, 1 PB6, 2 PB7, -1 off. Pressed = `level` (default 0: a button to GND, with a pull-up). Pressing locks on the centre box, or stops tracking |

- **`state`:** `off`, `armed` (locks on at the next frame) or `locked`. (`lost` appears only in the `TRACK` event: see below.)
- **`x`, `y`:** the target's centre, in live-frame pixels.
- **`psr`:** the confidence. Above ~6 is a good lock; below 5.5 for 8 frames in a row (~130 ms at 60 fps) counts as lost.

**Events:**
- `TRACK`, on a state change: `{"state","x","y","w","h"}`. When the target is lost the board sends `TRACK` with `"state":"lost"` and the last position, once, and **stops tracking** (state `off` from then on). `TRACK=1` or the button locks on again.
- `TRACKPOS`, for every frame that isn't sent as a whole: `{"n", "boxes", "track":{…}}`. Together with `INVOKE`, that gives the target position for every camera frame. With `VIDEO=0` and nothing tracked, a `TRACKPOS` with `"track":{"state":"off"}` comes every 0.5 s as a sign of life.

## Motion detection

| Command | What it does |
|---|---|
| `MOTION=<on>,<action>[,<pin>[,<ms>[,<sens>]]]` | `on` 0/1. `action`: 0 nothing, 1 save a VGA photo to `/MOTION/MOT_nnnn.jpg`, 2 pulse a pin. `pin`: 0 PA0, 1 PB6, 2 PB7. `ms`: pulse length 1–10000. `sens`: sensitivity 1–100 |
| `MOTION?` | Settings, `score`, `events`, `frames`, `bad_frames`, `brightness` |
| `MOTIONTEST` | Runs the selected action once, as if something moved |
| `PULSE` | One pulse on the selected pin |

**Event `MOTION`:** `{"score","cells":300,"action","file"?,"error"?}`, at most one every 2 s.

## SD card

| Command | What it does |
|---|---|
| `SD?` | Card status: file system, label, size, free space |
| `SDMOUNT` | Mounts the card again (after a swap) |
| `LS="<dir>"[,<start>]` | Up to 40 entries per call: `[[name,size,is_dir,mtime],…]` plus `"more"` |
| `FSTAT="<path>"` | One file's info |
| `READ="<path>",<off>,<len>` | Up to 12288 bytes as base64, with a CRC32 |
| `WRITE="<path>",<off>,"<b64>"[,<crc32>]` | Offset 0 creates or truncates; otherwise appends at the end. The CRC32 of the data is checked |
| `RM="<path>"[,1]` | Delete (`,1`: a folder with its contents) |
| `MKDIR="<path>"` | Make folders, including missing parents |
| `MV="<from>","<to>"` | Rename / move |
| `TIME=<seconds>` | Local time, for file dates |
| `FORMAT="FAT32"` / `FORMAT="exFAT"` | **Erases the card**: one partition, label `GROVE` |
| `SDTEST[="FORMAT"]` | Diagnostic: raw write test at LBA 2,000,000+ (formats as FAT32 if asked and every stage passes) |

## Photos

`CAPTURE[=<w>,<h>[,<x>,<y>[,<sub>[,<direct>]]]]` takes a photo to `/DCIM/IMG_nnnn/`:

- **Size.** `w`×`h` is the result's size. It's taken as 640×480 tiles; with `sub` 8, a single 320×240.
- **Scale.** `sub` 1, 2, 4 or 8: the sensor window is `w·sub`×`h·sub` at (`x`,`y`) (default: centred), scaled down by the camera hardware.
- **Direct.** With `direct` 1 (the default), a one-tile photo is saved straight away as `full.jpg`. Larger photos are saved as tiles plus `tiles.json`, for the host to join.
- **Progress.** `CAPTURE` events report `{"tile","of"}` as tiles are taken. The reply is the manifest: `{"dir","width","height","x","y","sub","direct","tile_w","tile_h","tiles":[[file,x,y,size],…],"ms"}`.

The camera switches to its 5 MP mode for a photo and back afterwards.
