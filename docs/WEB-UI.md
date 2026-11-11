# Web interface

A small Node.js website for the Seeed Grove Vision AI V2 (SSCMA firmware) connected over USB.

- **Preview**: the live camera feed with detection boxes, labels and scores. It also shows the frame rate and inference time, and has a score filter and a snapshot button.
- **Settings**: device info, camera resolution, model slot, the board's detection thresholds (read-only), the LED trigger rule, reboot, releasing the serial port, watchdog history, and an AT command console.
- **Files**: browse, upload, download, rename and delete files on the board's SD card, and capture photos to it at any size from 160×120 to 2592×1936. VGA 640×480 is the default, and the board saves it directly. This needs the custom firmware in [`firmware/`](../firmware/README.md). The site detects which firmware is on the board and shows the matching tabs.


## Object suggestion before lock-on

With the SD demo 2 firmware, the Preview tab's Tracking card guesses which object you mean inside the dashed lock-on box while you aim, and outlines it (green: likely, yellow: maybe). With "Snap to the object" on, Lock on takes that tight box, so the tracker learns the object rather than the background around it.

It runs on this server in `lib/suggest_worker.py`, about three times a second and 50–90 ms each, using OpenCV. One-time setup:

```sh
python3 -m venv --system-site-packages .venv && .venv/bin/pip install -r requirements.txt
```

Method: colour (or, on grayscale frames, texture) histograms of the box against a ring around it give each pixel a likelihood of belonging to the object. GrabCut then refines that into an outline, and the piece covering the box centre is kept. A score from 0–100 (separation from the surroundings, and an outline on real image edges) decides what is shown.

`test/suggest_test.py` checks it on generated scenes: real photos with objects from other photos pasted near the box, and empty boxes. It finds the right object (box overlap of at least 0.5) for 72–78% of clearly visible objects. In busy photos it also outlines something in about a third of the "empty" boxes, mostly labelled "maybe", which is why it suggests rather than decides.

## Start on boot (recommended)

From a terminal on the Jetson (not with sudo; the script asks for your password itself):

```sh
cd ~/grove-ai
./deploy/install.sh
```

Then open http://localhost:3000, or the Jetson's LAN address on port 3000. The script installs three systemd units:

- `grove-web.service` runs the server as you at boot, with serial-port access. If it crashes or exits, systemd restarts it after 3 s, and it never stops retrying.
- `grove-web-health.timer` (with `grove-web-health.service`) asks the server for `/api/health` every minute. If it fails to answer twice in a row (a hung process), it restarts the service.

Useful commands:

```sh
journalctl -u grove-web -f          # live log, including every watchdog action
sudo systemctl restart grove-web    # restart by hand
./deploy/install.sh --print         # show the unit files
./deploy/install.sh --uninstall     # stop starting at boot
```

The service starts Node from its full path, which comes from nvm. If you switch Node versions with nvm, run `./deploy/install.sh` again.

## Run by hand

```sh
npm install
npm start      # or: sg dialout -c "npm start" if you were only just added to dialout
```

Stop the boot service first (`sudo systemctl stop grove-web`), because only one program can use the serial port at a time.

| Variable      | Default        | Meaning                                                                                |
|---------------|----------------|----------------------------------------------------------------------------------------|
| `PORT`        | `3000`         | HTTP port                                                                              |
| `HOST`        | `0.0.0.0`      | Use `127.0.0.1` to keep it off the network                                             |
| `SERIAL_PORT` | (auto)         | Fixed serial device. By default the server finds the board by its USB ID (`1a86:55d3`) |

There is no login. Anyone who can reach the port can change the board's settings.

## Freeze watchdog

Moving the camera can flex its ribbon cable enough to stall the board's capture loop. The board still answers commands, but stops sending frames. The server notices after 4 s without a frame and tries these fixes in order, moving to the next only if frames don't come back:

1. Restart the stream (`AT+BREAK`, then `AT+INVOKE`). This fixes the usual camera-cable freeze in under a second.
2. Re-initialise the camera (`AT+SENSOR=...`).
3. Reboot the board (`AT+RST`). This is needed if the camera lost contact and came back.
4. Reopen the serial port. Reopening hardware-resets the board, which helps if it stops answering commands at all.

If none of these work (for example, the camera is unplugged), it keeps alternating steps 3 and 4. It waits longer between attempts each time, up to a minute. Pausing the preview turns the watchdog off. The page shows "No frames for N s" and then "Recovering…" while this happens. Settings → Connection shows how many freezes were fixed and the last action taken.

If freezes keep happening, secure the ribbon cable so it can't flex where it plugs in.

All four steps were tested on this board by freezing the stream on purpose.

## Board quirks this code works around

- **Opening the serial port reboots the board.** For the first 100 ms after a reboot, any input drops the bootloader into firmware-download mode. The server sends nothing until the firmware announces itself (`INIT@STAT?`).
- **The camera is only detected at boot.** If it is connected while the board is powered, the watchdog's reboot step or **Reboot board** picks it up.
- **The detection thresholds can't be changed.** Firmware 2025.01.02 has no `AT+TSCORE`/`AT+TIOU`. The Preview slider can only hide results that score below the board's own threshold.
- **`AT+ACTION` accepts any text without checking it.** The LED rule builder writes the same `max_score(...)` form that SenseCraft uses.
- **`AT+LED=` never replies, and `AT+YIELD` does not pause the stream.**
- **Only one program can use the port at a time.** To flash a new model with SenseCraft AI, press **Release port**, flash it, then press **Reconnect**.

## Layout

- `server.js`: Express REST API (`/api/*`, including `/api/health`) and WebSocket (`/ws`) for the live results.
- `lib/grove.js`: serial driver. It parses replies, queues commands, runs and pauses the stream, runs the freeze watchdog, and reconnects when the board is unplugged.
- `public/`: the page itself (plain HTML, CSS and JS, no build step).
- `deploy/install.sh`: installs the systemd units.
- `lib/sd.js`, `lib/stitch-worker.js`: SD card transfers (chunked, CRC-checked) and joining capture tiles.
- `firmware/`: the custom SD demo firmware, its build and flash scripts, and a fake board for testing (see its README).
