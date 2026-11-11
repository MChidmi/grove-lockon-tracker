#!/usr/bin/env python3
"""Flash a firmware image onto the Grove Vision AI V2 through its bootloader (XMODEM).

    python3 flash.py out/grove-sd-demo1.img
    python3 flash.py --port /dev/ttyACM0 ../../grove-ai-fw/restore/grove_vision_ai_v2_20250102.img

Same handshake as the SDK's xmodem/xmodem_send.py, but needs only pyserial:
opening the port resets the board; its bootloader waits about 100 ms for a key, so we
answer "1" until it offers XMODEM, send the image in 128-byte CRC-16 blocks, then answer
"y" to reboot into the new firmware. The bootloader itself is never written, so if a
transfer fails, run this again (or flash the restore image).

Nothing else may have the port open: stop the web server or press "Release port" first.
"""
import argparse
import os
import sys
import time

import serial
from serial.tools import list_ports

SOH, EOT, ACK, NAK, CAN = 0x01, 0x04, 0x06, 0x15, 0x18
BLOCK = 128
XMODEM_PROMPT = "Send data using the xmodem protocol"
REBOOT_PROMPT = "reboot system?"


def crc16_xmodem(data):
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else crc << 1
    return crc & 0xFFFF


def find_port():
    for p in list_ports.comports():
        if p.vid == 0x1A86 and p.pid == 0x55D3:   # the board's CH343 USB-serial chip
            return p.device
    return "/dev/ttyACM0"


class Board:
    def __init__(self, port, baud):
        self.ser = serial.Serial(port, baud, timeout=0.02)   # opening resets the board
        self.buf = b""

    def lines(self):
        """Yields complete lines as they arrive (non-blocking)."""
        chunk = self.ser.read(512)
        if chunk:
            self.buf += chunk
        while b"\n" in self.buf:
            line, self.buf = self.buf.split(b"\n", 1)
            text = line.decode(errors="replace").strip()
            if text:
                yield text

    def wait_text(self, text, timeout, poke=None):
        """Waits for text in the output. poke is sent after every line and every 30 ms."""
        end = time.time() + timeout
        last_poke = 0.0
        while time.time() < end:
            if poke and time.time() - last_poke > 0.03:
                self.ser.write(poke)
                last_poke = time.time()
            for line in self.lines():
                print(f"  board: {line}")
                if poke:
                    self.ser.write(poke)
                if text in line:
                    return True
            if text.encode() in self.buf:   # prompt without a newline yet
                print(f"  board: {self.buf.decode(errors='replace').strip()}")
                self.buf = b""
                return True
        return False

    def read_byte(self, timeout):
        end = time.time() + timeout
        while time.time() < end:
            b = self.ser.read(1)
            if b:
                return b[0]
        return None


def xmodem_send(board, data):
    ser = board.ser
    # The receiver asks for CRC mode with 'C' (or plain checksum with NAK).
    mode = None
    end = time.time() + 30
    while time.time() < end and mode is None:
        b = board.read_byte(1)
        if b == ord("C"):
            mode = "crc"
        elif b == NAK:
            mode = "sum"
        elif b == CAN:
            raise RuntimeError("Board cancelled the transfer")
    if mode is None:
        raise RuntimeError("Board never asked for data (no 'C')")

    blocks = (len(data) + BLOCK - 1) // BLOCK
    t0 = time.time()
    for i in range(blocks):
        payload = data[i * BLOCK:(i + 1) * BLOCK].ljust(BLOCK, b"\x1a")
        num = (i + 1) & 0xFF
        if mode == "crc":
            crc = crc16_xmodem(payload)
            packet = bytes([SOH, num, 0xFF - num]) + payload + bytes([crc >> 8, crc & 0xFF])
        else:
            packet = bytes([SOH, num, 0xFF - num]) + payload + bytes([sum(payload) & 0xFF])
        for attempt in range(10):
            ser.write(packet)
            reply = board.read_byte(10)
            if reply == ACK:
                break
            if reply == CAN:
                raise RuntimeError(f"Board cancelled at block {i + 1}")
        else:
            ser.write(bytes([CAN, CAN, CAN]))
            raise RuntimeError(f"Block {i + 1} was not accepted after 10 tries")
        if i % 64 == 0 or i == blocks - 1:
            pct = (i + 1) * 100 // blocks
            rate = (i + 1) * BLOCK / max(time.time() - t0, 0.001) / 1024
            print(f"\r  sending {pct:3d}%  ({i + 1}/{blocks} blocks, {rate:.0f} KB/s)", end="", flush=True)
    print()
    for _ in range(10):
        ser.write(bytes([EOT]))
        if board.read_byte(5) == ACK:
            return
    raise RuntimeError("Board did not acknowledge the end of the transfer")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image", help="firmware image (output.img from the SDK image tool)")
    ap.add_argument("--port", default=None, help="serial port (default: find the board by its USB ID)")
    ap.add_argument("--baud", type=int, default=921600)
    opts = ap.parse_args()

    data = open(opts.image, "rb").read()
    if data[:4] != b"ckBS":
        sys.exit(f"{opts.image} does not look like a WE2 boot image (no 'ckBS' header)")
    port = opts.port or find_port()
    print(f"Flashing {os.path.basename(opts.image)} ({len(data)} bytes) through {port}")

    try:
        board = Board(port, opts.baud)
    except serial.SerialException as err:
        sys.exit(f"Could not open {port}: {err}\nIs the web server still using it? Stop it or press 'Release port'.")

    print("Waiting for the bootloader (the board resets when the port opens)...")
    if not board.wait_text(XMODEM_PROMPT, 15, poke=b"1\r"):
        sys.exit("The bootloader did not offer XMODEM. Press the board's reset button and run this again.")
    time.sleep(1)
    board.ser.reset_input_buffer()
    board.ser.write(b"1\r")

    try:
        xmodem_send(board, data)
    except RuntimeError as err:
        sys.exit(f"Transfer failed: {err}. The bootloader is intact: run this again.")
    print("Image sent. Rebooting into the new firmware...")
    if board.wait_text(REBOOT_PROMPT, 20):
        board.ser.write(b"y\r")
    end = time.time() + 6
    while time.time() < end:
        for line in board.lines():
            print(f"  board: {line}")
    print("Done.")


if __name__ == "__main__":
    main()
