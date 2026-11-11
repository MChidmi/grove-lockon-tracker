#!/usr/bin/env python3
"""Fake Grove Vision AI V2 running the grove-sd demo firmware, on a virtual serial port.

Lets the web server and page be tested without flashing the real board. It speaks the
same protocol as firmware/grove_sd_demo (see proto.h and sdcmd.c), with a local folder
standing in for the SD card, and for demo 2 a synthetic camera.

    python3 fake_board.py --sd /tmp/fake-sd --demo 2 --link /tmp/grove-sim
    SERIAL_PORT=/tmp/grove-sim npm start

--slow limits output to the real link speed (921600 baud, about 92 KB/s).
"""
import argparse
import base64
import io
import json
import math
import os
import pty
import select
import shutil
import sys
import termios
import time
import tty
import zlib

import numpy as np
from PIL import Image, ImageDraw

SENSOR_W, SENSOR_H = 2592, 1944
CAPTURE_H = 1936   # the firmware leaves out the sensor's last 8 lines
TILE_W, TILE_H = 640, 480
IO_CHUNK = 12288
LS_MAX = 40

E_OK, E_IO, E_INVAL, E_NOMEM, E_PERM = 0, 4, 5, 6, 9


class Board:
    def __init__(self, fd, sd_root, demo, slow):
        self.fd = fd
        self.sd = os.path.abspath(sd_root)
        self.demo = demo
        self.slow = slow
        self.buf = b""
        self.streaming = False
        self.stream_image = True
        self.frame_no = 0
        self.next_frame = 0.0
        self.time_base = None
        self.scene = None

    # ---- output ----------------------------------------------------------

    def send(self, data: bytes):
        view = memoryview(data)
        while view:
            n = os.write(self.fd, view[:4096])
            if self.slow:
                time.sleep(n / 92000)
            view = view[n:]

    def msg(self, type_, name, code, data):
        self.send(("\r" + json.dumps({"type": type_, "name": name, "code": code, "data": data},
                                     separators=(",", ":")) + "\n").encode())

    def reply(self, name, data=None, code=E_OK):
        self.msg(0, name, code, data)

    def log(self, text):
        self.send((text + "\r\n").encode())

    def boot(self):
        self.log("1st BL Modem Build DATE=Nov 30 2023 (fake)")
        self.log(f"Grove SD demo {self.demo} (fake board)")
        self.log("SD card: mounted")
        self.reply("INIT@STAT?", {"boot_count": 0, "is_ready": 1, "sd": 1})

    # ---- SD helpers ----------------------------------------------------------

    def real(self, path):
        p = os.path.normpath(os.path.join(self.sd, path.lstrip("/")))
        if p != self.sd and not p.startswith(self.sd + os.sep):
            raise ValueError("Invalid name")
        return p

    def fr(self, name, err):
        if isinstance(err, FileNotFoundError):
            self.reply(name, "No such file (FR 4)", E_INVAL)
        elif isinstance(err, FileExistsError):
            self.reply(name, "Already exists (FR 8)", E_PERM)
        elif isinstance(err, OSError) and err.errno == 39:
            self.reply(name, "Access denied (folder not empty, or disk full) (FR 7)", E_PERM)
        else:
            self.reply(name, f"{err} (FR 6)", E_INVAL)

    def local_mtime(self, st):
        # FAT stores local wall-clock time; tm_gmtoff is the real offset at that moment
        # (time.timezone is wrong for zones like Africa/Casablanca).
        return int(st.st_mtime + time.localtime(st.st_mtime).tm_gmtoff)

    # ---- commands -------------------------------------------------------------

    def handle(self, line):
        if not line.upper().startswith("AT+"):
            return self.reply("AT", f"Could not parse: {line[:60]}", E_INVAL)
        body = line[3:]
        name, args = body, []
        if "=" in body:
            name, rest = body.split("=", 1)
            args = parse_args(rest)
        name = name.upper()
        fn = getattr(self, "cmd_" + name.replace("?", "_Q").replace("@", "_"), None)
        if fn is None:
            return self.msg(2, "AT", E_INVAL, f"Unknown command: AT+{name}")
        try:
            fn(name, args)
        except (OSError, ValueError) as err:
            self.fr(name, err)

    def cmd_ID_Q(self, n, a): self.reply(n, "grove-sd")
    def cmd_NAME_Q(self, n, a): self.reply(n, "Grove Vision AI V2")
    def cmd_STAT_Q(self, n, a): self.reply(n, {"boot_count": 0, "is_ready": 1, "sd": 1})

    def cmd_VER_Q(self, n, a):
        self.reply(n, {"at_api": "v0", "software": f"grove-sd-demo{self.demo} 0.1.0 (fake)", "hardware": "1",
                       "firmware": "grove-sd", "demo": self.demo, "build": "fake board"})

    def cmd_HELP_Q(self, n, a):
        for l in ["  AT+SD? / AT+LS / AT+READ / AT+WRITE / AT+RM / AT+MKDIR / AT+MV / AT+TIME (fake board)"]:
            self.log(l)

    def cmd_RST(self, n, a):
        self.streaming = False
        time.sleep(0.3)
        self.boot()

    def cmd_PREVIEW(self, n, a):
        if not a or a[0] not in ("0", "1") or (len(a) > 1 and a[1] not in ("320", "640")):
            return self.reply(n, "Usage: PREVIEW=<1|0>[,<320|640>]", E_INVAL)
        self.preview_gray = a[0] == "1"
        if len(a) > 1:
            self.preview_w = int(a[1])
        self.cmd_PREVIEW_Q(n, a)

    def cmd_PREVIEW_Q(self, n, a):
        w = getattr(self, "preview_w", 320)
        self.reply(n, {"gray": 1 if getattr(self, "preview_gray", True) else 0, "width": w, "height": w * 3 // 4})

    # Motion detection settings (the fake board doesn't detect anything; MOTIONTEST fires an event).
    MOTION_ACTIONS = ["nothing", "save", "pulse"]
    MOTION_PINS = ["PA0", "PB6", "PB7"]

    def motion_state(self):
        m = getattr(self, "motion", {"on": 0, "action": 0, "pin": 0, "pulse_ms": 100, "sens": 50, "events": 0})
        self.motion = m
        return {"on": m["on"], "action": self.MOTION_ACTIONS[m["action"]], "pin": self.MOTION_PINS[m["pin"]],
                "pulse_ms": m["pulse_ms"], "sensitivity": m["sens"], "score": 0 if m["on"] else -1,
                "cells": 300, "events": m["events"]}

    def cmd_MOTION(self, n, a):
        try:
            on, action = int(a[0]), int(a[1])
            pin = int(a[2]) if len(a) > 2 else 0
            ms = int(a[3]) if len(a) > 3 else 100
            sens = int(a[4]) if len(a) > 4 else 50
            assert on in (0, 1) and 0 <= action <= 2 and 0 <= pin <= 2 and 1 <= ms <= 10000 and 1 <= sens <= 100
        except (IndexError, ValueError, AssertionError):
            return self.reply(n, "Usage: MOTION=<on>,<action>[,<pin>[,<ms>[,<sensitivity>]]]", E_INVAL)
        st = self.motion_state()
        self.motion.update(on=on, action=action, pin=pin, pulse_ms=ms, sens=sens)
        self.reply(n, self.motion_state())

    def cmd_MOTION_Q(self, n, a): self.reply(n, self.motion_state())

    def cmd_PULSE(self, n, a): self.reply(n, self.motion_state())

    def cmd_MOTIONTEST(self, n, a):
        st = self.motion_state()
        data = {"score": 0, "cells": 300, "action": st["action"]}
        if st["action"] == "save":
            os.makedirs(self.real("/MOTION"), exist_ok=True)
            k = 1
            while os.path.exists(self.real(f"/MOTION/MOT_{k:04d}.jpg")):
                k += 1
            buf = io.BytesIO()
            self.sensor_frame().resize((640, 480)).save(buf, "JPEG", quality=85)
            with open(self.real(f"/MOTION/MOT_{k:04d}.jpg"), "wb") as f:
                f.write(buf.getvalue())
            data["file"] = f"/MOTION/MOT_{k:04d}.jpg"
        self.motion["events"] += 1
        self.msg(1, "MOTION", 0, data)
        self.reply(n, "")

    # Tracking: the fake board "tracks" a point drifting from the centre.
    def track_state(self):
        t = getattr(self, "track", None)
        if not t:
            return {"state": "off", "frame": [getattr(self, "preview_w", 320), getattr(self, "preview_w", 320) * 3 // 4]}
        return {"state": t["state"], "x": int(t["x"]), "y": int(t["y"]), "w": t["w"], "h": t["h"], "psr": 120, "ms": 3,
                "frame": [getattr(self, "preview_w", 320), getattr(self, "preview_w", 320) * 3 // 4]}

    def cmd_TRACK(self, n, a):
        on = int(a[0]) if a else -1
        if on not in (0, 1):
            return self.reply(n, "Usage: TRACK=1[,<box w>,<box h>] or TRACK=0", E_INVAL)
        pw = getattr(self, "preview_w", 320)
        if on:
            if not self.streaming:
                return self.reply(n, "Start the live view first; tracking runs on its frames", E_IO)
            w = int(a[1]) if len(a) > 1 else pw // 4
            h = int(a[2]) if len(a) > 2 else w * 3 // 4
            self.track = {"state": "locked", "x": pw / 2, "y": pw * 3 / 8, "w": w, "h": h, "n": 0}
            self.msg(1, "TRACK", 0, {"state": "locked", "x": pw // 2, "y": pw * 3 // 8, "w": w, "h": h})
        else:
            self.track = None
            self.msg(1, "TRACK", 0, {"state": "off"})
        self.reply(n, self.track_state())

    def cmd_TRACK_Q(self, n, a): self.reply(n, self.track_state())

    def cmd_TRACKBTN(self, n, a): self.reply(n, {"pin": int(a[0]) if a else -1, "pressed_level": 0})

    def cmd_BREAK(self, n, a):
        self.streaming = False
        self.reply(n, None)

    def cmd_INVOKE_Q(self, n, a): self.reply(n, 1 if self.streaming else 0)

    def cmd_TIME(self, n, a):
        self.time_base = int(a[0])
        self.reply(n, self.time_base)

    def cmd_SD_Q(self, n, a):
        du = shutil.disk_usage(self.sd)
        self.reply(n, {"mounted": 1, "fs": "FAT32", "label": "FAKESD", "total_kb": du.total // 1024,
                       "free_kb": du.free // 1024, "time": self.time_base or 0})

    def cmd_SDMOUNT(self, n, a): self.cmd_SD_Q(n, a)

    def cmd_LS(self, n, a):
        path = a[0] if a else "/"
        start = int(a[1]) if len(a) > 1 else 0
        names = sorted(os.listdir(self.real(path)))
        entries = []
        for name in names[start:start + LS_MAX]:
            st = os.stat(os.path.join(self.real(path), name))
            is_dir = os.path.isdir(os.path.join(self.real(path), name))
            entries.append([name, 0 if is_dir else st.st_size, int(is_dir), self.local_mtime(st)])
        self.reply(n, {"path": path, "start": start, "entries": entries, "more": int(start + LS_MAX < len(names))})

    def cmd_FSTAT(self, n, a):
        p = self.real(a[0])
        st = os.stat(p)
        self.reply(n, {"name": os.path.basename(p), "size": 0 if os.path.isdir(p) else st.st_size,
                       "dir": int(os.path.isdir(p)), "mtime": self.local_mtime(st)})

    def cmd_READ(self, n, a):
        p, off, ln = self.real(a[0]), int(a[1]), min(int(a[2]), IO_CHUNK)
        with open(p, "rb") as f:
            size = os.fstat(f.fileno()).st_size
            f.seek(off)
            data = f.read(ln)
        self.reply(n, {"offset": off, "len": len(data), "size": size, "crc": zlib.crc32(data),
                       "data": base64.b64encode(data).decode()})

    def cmd_WRITE(self, n, a):
        p, off = self.real(a[0]), int(a[1])
        data = base64.b64decode(a[2])
        if len(data) > IO_CHUNK:
            return self.reply(n, "Bad base64 data, or chunk over 12288 bytes", E_INVAL)
        if len(a) > 3 and int(a[3]) != zlib.crc32(data):
            return self.reply(n, "CRC mismatch: chunk was damaged on the way, send it again", E_INVAL)
        if off == 0:
            open(p, "wb").close()
        size = os.path.getsize(p)
        if off != size:
            return self.reply(n, f"Offset {off} is not the end of the file (size {size})", E_INVAL)
        with open(p, "ab") as f:
            f.write(data)
        self.reply(n, {"offset": off, "len": len(data), "size": size + len(data)})

    def cmd_RM(self, n, a):
        if a[0] in ("", "/"):
            return self.reply(n, "Refusing to delete the root folder", E_PERM)
        p = self.real(a[0])
        if os.path.isdir(p):
            shutil.rmtree(p) if len(a) > 1 and a[1] == "1" else os.rmdir(p)
        else:
            os.remove(p)
        self.reply(n, None)

    def cmd_MKDIR(self, n, a):
        os.makedirs(self.real(a[0]), exist_ok=True)
        self.reply(n, None)

    def cmd_MV(self, n, a):
        dst = self.real(a[1])
        if os.path.exists(dst):
            raise FileExistsError(dst)
        os.rename(self.real(a[0]), dst)
        self.reply(n, None)

    # ---- camera (demo 2) --------------------------------------------------------

    def need_camera(self, n):
        if self.demo < 2:
            self.msg(2, "AT", E_INVAL, f"Unknown command: AT+{n}")
            return False
        return True

    def cmd_INVOKE(self, n, a):
        if not self.need_camera(n):
            return
        self.stream_image = len(a) < 3 or a[2] != "1"
        self.streaming = True
        self.frame_no = 0
        self.reply(n, {"sensor": {"id": 1, "type": 1, "state": 1, "opt_id": 0, "opt_detail": "640x480 preview"}})

    def sensor_frame(self):
        """A test scene the size of the full sensor, with a grid to check tile alignment."""
        if self.scene is None:
            xs = np.linspace(0, 255, SENSOR_W, dtype=np.float32)
            ys = np.linspace(0, 255, SENSOR_H, dtype=np.float32)
            rgb = np.empty((SENSOR_H, SENSOR_W, 3), dtype=np.uint8)
            rgb[..., 0] = xs[None, :]
            rgb[..., 1] = ys[:, None]
            rgb[..., 2] = 128
            img = Image.fromarray(rgb, "RGB")
            d = ImageDraw.Draw(img)
            for x in range(0, SENSOR_W, 108):
                d.line([(x, 0), (x, SENSOR_H)], fill=(255, 255, 255), width=3)
            for y in range(0, SENSOR_H, 108):
                d.line([(0, y), (SENSOR_W, y)], fill=(255, 255, 255), width=3)
            d.ellipse([SENSOR_W // 2 - 400, SENSOR_H // 2 - 400, SENSOR_W // 2 + 400, SENSOR_H // 2 + 400],
                      outline=(255, 60, 60), width=12)
            self.scene = img
        return self.scene

    def stream_tick(self):
        if not self.streaming or time.time() < self.next_frame:
            return
        self.next_frame = time.time() + 0.2
        self.frame_no += 1
        pw = getattr(self, "preview_w", 320)
        data = {"count": self.frame_no, "perf": [0, 0, 0], "boxes": [], "resolution": [pw, pw * 3 // 4]}
        t = getattr(self, "track", None)
        if t:
            import math
            t["n"] += 1
            t["x"] = pw / 2 + 40 * math.sin(t["n"] / 15.0)
            t["y"] = pw * 3 / 8 + 25 * math.cos(t["n"] / 11.0)
            st = self.track_state(); st.pop("frame")
            data["track"] = st
            data["boxes"] = [[st["x"], st["y"], st["w"], st["h"], 100, 0]]
        if self.stream_image:
            img = self.sensor_frame().resize((648, 486)).crop((4, 3, 644, 483)).resize((pw, pw * 3 // 4))
            if getattr(self, "preview_gray", True):
                img = img.convert("L").convert("RGB")
            ImageDraw.Draw(img).text((10, 10), f"fake preview #{self.frame_no}", fill=(255, 255, 0))
            buf = io.BytesIO()
            img.save(buf, "JPEG", quality=70)
            data["image"] = base64.b64encode(buf.getvalue()).decode()
        self.msg(1, "INVOKE", 0, data)

    def cmd_CAPTURE(self, n, a):
        if not self.need_camera(n):
            return
        # Like the firmware: <w>,<h> is the result size, <sub> (1, 2 or 4) the subsample
        # factor, so the sensor window is (w*sub) x (h*sub) at (x, y).
        w = int(a[0]) if len(a) > 0 else 1920
        h = int(a[1]) if len(a) > 1 else 1080
        sub = int(a[4]) if len(a) > 4 else 1
        direct = int(a[5]) if len(a) > 5 else 1
        if sub not in (1, 2, 4, 8) or (sub == 8 and (w, h) != (320, 240)):
            return self.reply(n, "Usage: CAPTURE[=<w>,<h>[,<x>,<y>[,<sub 1|2|4|8>[,<direct>]]]]", E_INVAL)
        tw, th = (320, 240) if sub == 8 else (TILE_W, TILE_H)
        align = 2 if sub == 1 else 4
        x = int(a[2]) if len(a) > 2 else ((SENSOR_W - w * sub) // 2) & ~(align - 1)
        y = int(a[3]) if len(a) > 3 else ((CAPTURE_H - h * sub) // 2) & ~(align - 1)
        if (w % 2 or h % 2 or x % align or y % align or w < tw or h < th or x < 0 or y < 0
                or x + w * sub > SENSOR_W or y + h * sub > CAPTURE_H):
            return self.reply(n, "Region must be even, at least 640x480, and (times sub) inside 2592x1936", E_INVAL)
        was = self.streaming
        self.streaming = False
        t0 = time.time()
        idx = 1
        while os.path.exists(self.real(f"/DCIM/IMG_{idx:04d}")):
            idx += 1
        folder = f"/DCIM/IMG_{idx:04d}"
        os.makedirs(self.real(folder))
        cols, rows = math.ceil(w / tw), math.ceil(h / th)
        whole = bool(direct) and cols == 1 and rows == 1   # one tile: saved as full.jpg
        tiles = []
        for r in range(rows):
            for c in range(cols):
                tx = x + sub * min(c * tw, w - tw)
                ty = y + sub * min(r * th, h - th)
                buf = io.BytesIO()
                window = self.sensor_frame().crop((tx, ty, tx + tw * sub, ty + th * sub))
                (window.resize((tw, th)) if sub > 1 else window).save(buf, "JPEG", quality=90)
                name = "full.jpg" if whole else f"r{r}c{c}.jpg"
                with open(self.real(f"{folder}/{name}"), "wb") as f:
                    f.write(buf.getvalue())
                tiles.append([name, tx, ty, len(buf.getvalue())])
                self.msg(1, "CAPTURE", 0, {"tile": len(tiles), "of": rows * cols})
                time.sleep(0.15)
        manifest = {"dir": folder, "width": w, "height": h, "x": x, "y": y, "sub": sub, "direct": int(whole), "tile_w": tw, "tile_h": th,
                    "tiles": tiles, "ms": int((time.time() - t0) * 1000)}
        if not whole:
            with open(self.real(f"{folder}/tiles.json"), "w") as f:
                json.dump(manifest, f)
        self.reply(n, manifest)
        self.streaming = was

    # ---- main loop ----------------------------------------------------------------

    def feed(self, data):
        self.buf += data
        while b"\n" in self.buf:
            line, self.buf = self.buf.split(b"\n", 1)
            line = line.strip().decode(errors="replace")
            if line:
                self.handle(line)


def parse_args(rest):
    """AT argument list: "quoted" strings (with \\ escapes) or bare words, comma separated."""
    out, i = [], 0
    while i <= len(rest):
        while i < len(rest) and rest[i] == " ":
            i += 1
        if i < len(rest) and rest[i] == '"':
            i += 1
            s = []
            while i < len(rest) and rest[i] != '"':
                if rest[i] == "\\" and i + 1 < len(rest):
                    i += 1
                s.append(rest[i])
                i += 1
            out.append("".join(s))
            i += 1
            while i < len(rest) and rest[i] == " ":
                i += 1
            if i < len(rest) and rest[i] == ",":
                i += 1
                continue
            break
        j = rest.find(",", i)
        if j < 0:
            out.append(rest[i:].strip())
            break
        out.append(rest[i:j].strip())
        i = j + 1
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--sd", required=True, help="folder that acts as the SD card")
    ap.add_argument("--demo", type=int, default=2, choices=(1, 2))
    ap.add_argument("--link", default="/tmp/grove-sim", help="symlink to create for the virtual port")
    ap.add_argument("--slow", action="store_true", help="limit output to ~92 KB/s like the real link")
    opts = ap.parse_args()
    os.makedirs(opts.sd, exist_ok=True)

    master, slave = pty.openpty()
    tty.setraw(slave)
    attrs = termios.tcgetattr(slave)
    attrs[3] &= ~termios.ECHO
    termios.tcsetattr(slave, termios.TCSANOW, attrs)
    name = os.ttyname(slave)
    if os.path.lexists(opts.link):
        os.remove(opts.link)
    os.symlink(name, opts.link)
    print(f"fake board (demo {opts.demo}) on {name} -> {opts.link}, SD card folder {opts.sd}", flush=True)

    board = Board(master, opts.sd, opts.demo, opts.slow)
    board.boot()
    try:
        while True:
            r, _, _ = select.select([master], [], [], 0.02)
            if r:
                try:
                    data = os.read(master, 65536)
                except OSError:
                    data = b""
                if data:
                    board.feed(data)
            board.stream_tick()
    except KeyboardInterrupt:
        pass
    finally:
        os.remove(opts.link)


if __name__ == "__main__":
    main()
