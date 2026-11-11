#!/usr/bin/env python3
"""Lock on the object in the centre of the view and print its position, frame by frame.

Talks straight to the Grove Vision AI V2 running this project's firmware (no web server):

    pip install pyserial
    python3 track_print.py [--port /dev/ttyACM0] [--box 80] [--fps 70]

Prints one line per camera frame: frame number, state, centre x/y (live-frame pixels, 320x240),
box size and confidence (PSR). Ctrl+C stops tracking.
Note: opening the port resets the board; the script waits for it to start.
"""
import argparse
import json
import sys
import time

import serial


def send(port, cmd):
    port.write(f'AT+{cmd}\r\n'.encode())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', default='/dev/ttyACM0')
    ap.add_argument('--box', type=int, default=80, help='lock-on box width (height = 3/4 of it)')
    ap.add_argument('--fps', type=int, default=70, choices=[15, 30, 45, 60, 70], help='camera rate (70 = max)')
    a = ap.parse_args()

    port = serial.Serial(a.port, 921600, timeout=0.1)
    # The board restarts when the port opens: wait for its "ready" line.
    t0, buf = time.time(), b''
    while time.time() - t0 < 10:
        buf += port.read(4096)
        if b'INIT@STAT?' in buf:
            break
    time.sleep(0.5)
    port.reset_input_buffer()

    send(port, 'PREVIEW=1,320,1,%d' % a.fps)  # grayscale, 320x240, fast camera mode
    send(port, 'VIDEO=0')                      # no video: only tracking results over USB
    send(port, 'INVOKE=-1,0,1')                # start the live view (frames without images)
    time.sleep(1.0)                            # let the exposure settle
    send(port, f'TRACK=1,{a.box},{a.box * 3 // 4}')
    print(f'locking on the centre {a.box}x{a.box * 3 // 4} box; Ctrl+C to stop', file=sys.stderr)

    count, t_start = 0, time.time()
    buf = b''
    try:
        while True:
            buf += port.read(4096)
            while b'\n' in buf:
                line, buf = buf.split(b'\n', 1)
                line = line.strip()
                if not line.startswith(b'{'):
                    continue
                try:
                    msg = json.loads(line)
                except ValueError:
                    continue
                if msg.get('name') not in ('TRACKPOS', 'INVOKE'):
                    continue
                d = msg.get('data', {})
                t = d.get('track')
                if not t or t.get('state') in (None, 'off', 'armed'):
                    continue
                count += 1
                rate = count / max(time.time() - t_start, 1e-3)
                print(f"{d.get('n', 0):7d} {t['state']:6s} x={t['x']:4d} y={t['y']:4d} "
                      f"w={t['w']:3d} h={t['h']:3d} psr={t.get('psr', 0):5.1f}  ({rate:4.1f}/s)")
    except KeyboardInterrupt:
        pass
    finally:
        send(port, 'TRACK=0')
        send(port, 'BREAK')
        port.close()


if __name__ == '__main__':
    main()
