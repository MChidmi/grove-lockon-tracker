// A USB serial port over WebUSB, for browsers that have WebUSB but whose Web Serial doesn't list
// USB devices (Chrome on Android). The board's CH343 chip is a standard USB CDC-ACM device: one
// "communications" interface (line settings) and one "data" interface (two bulk endpoints).
// Without an OS driver in the way (Android), the page can drive it directly, as serial apps do.
// A plain script (not a module), so the page also opens straight from a file:// path.

class UsbSerialPort {
  constructor(device) {
    this.device = device;
    this.opened = false;
  }

  static async request(filter) {
    return new UsbSerialPort(await navigator.usb.requestDevice({ filters: [{ vendorId: filter.usbVendorId, productId: filter.usbProductId }] }));
  }

  async open(baudRate) {
    const d = this.device;
    await d.open();
    if (!d.configuration) await d.selectConfiguration(1);
    this.comm = undefined;
    this.data = undefined;
    for (const iface of d.configuration.interfaces) {
      const alt = iface.alternate ?? iface.alternates[0];
      const bulk = alt.endpoints.filter((e) => e.type === 'bulk');
      const epIn = bulk.find((e) => e.direction === 'in'), epOut = bulk.find((e) => e.direction === 'out');
      if (alt.interfaceClass === 2 && this.comm === undefined) this.comm = iface.interfaceNumber;
      // the CDC data interface (class 10); a vendor-mode chip has one interface with bulk in + out
      if (epIn && epOut && (this.data === undefined || alt.interfaceClass === 10)) {
        this.data = iface.interfaceNumber;
        this.epIn = epIn.endpointNumber;
        this.epOut = epOut.endpointNumber;
        this.inSize = Math.max(epIn.packetSize, 64) * 64;          // read up to 64 packets at once
      }
    }
    if (this.data === undefined) throw new Error('No serial data interface on this USB device');
    if (this.comm !== undefined && this.comm !== this.data) await d.claimInterface(this.comm);
    await d.claimInterface(this.data);
    if (this.comm !== undefined) {
      // CDC SET_LINE_CODING: baud rate (32-bit LE), 1 stop bit, no parity, 8 data bits
      const lc = new DataView(new ArrayBuffer(7));
      lc.setUint32(0, baudRate, true);
      lc.setUint8(6, 8);
      await this._control(0x20, 0, lc.buffer);
      // CDC SET_CONTROL_LINE_STATE: DTR + RTS on, as opening a serial port on a computer does
      await this._control(0x22, 0x03);
    }
    this.opened = true;
  }

  _control(request, value, data) {
    const setup = { requestType: 'class', recipient: 'interface', request, value, index: this.comm };
    return data ? this.device.controlTransferOut(setup, data) : this.device.controlTransferOut(setup);
  }

  async write(bytes) {
    for (let i = 0; i < bytes.length; i += 16384) {
      const r = await this.device.transferOut(this.epOut, bytes.subarray(i, i + 16384));
      if (r.status !== 'ok') throw new Error(`USB write: ${r.status}`);
    }
  }

  // Calls onData(Uint8Array) for everything received until close(). Two reads are kept queued
  // so no data waits while the page handles the previous piece.
  async readLoop(onData, onError) {
    const read = () => this.device.transferIn(this.epIn, this.inSize);
    const queue = [read(), read()];
    while (this.opened) {
      let r;
      try {
        r = await queue.shift();
      } catch (err) {
        if (this.opened) onError(err);
        break;
      }
      if (!this.opened) break;
      if (r.status === 'stall') {
        try { await this.device.clearHalt('in', this.epIn); } catch { /* next read tells */ }
      }
      if (r.data && r.data.byteLength) onData(new Uint8Array(r.data.buffer, r.data.byteOffset, r.data.byteLength));
      queue.push(read());
    }
    for (const p of queue) p.catch(() => {});
  }

  async close() {
    if (!this.opened) return;
    this.opened = false;
    try { if (this.comm !== undefined) await this._control(0x22, 0); } catch { /* unplugged */ }
    try { await this.device.close(); } catch { /* unplugged */ }
  }
}

globalThis.UsbSerialPort = UsbSerialPort;
