// The board's AT protocol, without any server: feed it the bytes from the serial port, and it
// hands out replies and events. Works in the browser (Web Serial) and in Node (tests).
// A plain script (not a module), so the page also opens straight from a file:// path.
//
//   const link = new GroveLink((bytes) => writer.write(bytes));
//   reader loop: link.feed(chunk)
//   link.addEventListener('frame', (e) => e.detail)      // INVOKE: a live frame (+ tracking)
//   link.addEventListener('trackpos', ...)              // TRACKPOS: tracking result only
//   link.addEventListener('track', ...)                 // TRACK: locked / lost / off
//   await link.command('TRACK=1,80,60')                 // reply data, or throws

class GroveError extends Error {
  constructor(name, code, data) {
    super(typeof data === 'string' ? `${name}: ${data}` : `${name} failed (code ${code})`);
    this.code = code;
  }
}

// The name a reply carries: the command up to "=" ("PREVIEW=1,320" -> "PREVIEW", "VER?" -> "VER?").
const replyName = (cmd) => cmd.split('=')[0];

class GroveLink extends EventTarget {
  constructor(write) {
    super();
    this.write = write;
    this.decoder = new TextDecoder();
    this.buf = '';
    this.pending = [];                                  // [{ name, resolve, reject, timer }]
    this.encoder = new TextEncoder();
  }

  feed(bytes) {
    this.buf += this.decoder.decode(bytes, { stream: true });
    let nl;
    while ((nl = this.buf.indexOf('\n')) >= 0) {
      const line = this.buf.slice(0, nl).replace(/^\r+|\r+$/g, '');
      this.buf = this.buf.slice(nl + 1);
      if (line) this._line(line);
    }
    if (this.buf.length > 1 << 20) this.buf = '';      // garbage without newlines: drop it
  }

  _line(line) {
    if (line[0] !== '{') {
      this._emit('log', line);
      return;
    }
    let msg;
    try { msg = JSON.parse(line); } catch { this._emit('log', line); return; }
    if (msg.name === 'INIT@STAT?') { this._emit('ready', msg.data); return; }
    if (msg.type === 0) {
      const i = this.pending.findIndex((p) => p.name === msg.name);
      if (i < 0) return;
      const [p] = this.pending.splice(i, 1);
      clearTimeout(p.timer);
      if (msg.code === 0) p.resolve(msg.data);
      else p.reject(new GroveError(msg.name, msg.code, msg.data));
      return;
    }
    if (msg.name === 'INVOKE') this._emit('frame', msg.data);
    else if (msg.name === 'TRACKPOS') this._emit('trackpos', msg.data);
    else if (msg.name === 'TRACK') this._emit('track', msg.data);
    else this._emit('event', msg);
  }

  _emit(type, detail) {
    this.dispatchEvent(new CustomEvent(type, { detail }));
  }

  // Sends AT+<cmd> and waits for its reply. Resolves with the reply's data.
  command(cmd, timeout = 3000) {
    const name = replyName(cmd);
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        const i = this.pending.findIndex((p) => p.timer === timer);
        if (i >= 0) this.pending.splice(i, 1);
        reject(new Error(`No reply to AT+${cmd}`));
      }, timeout);
      this.pending.push({ name, resolve, reject, timer });
      Promise.resolve(this.write(this.encoder.encode(`AT+${cmd}\r\n`))).catch((err) => {
        clearTimeout(timer);
        reject(err);
      });
    });
  }

  // Waits for the board's "ready" line (it restarts when the port opens).
  waitReady(timeout = 8000) {
    return new Promise((resolve) => {
      const done = (ok) => { clearTimeout(t); this.removeEventListener('ready', on); resolve(ok); };
      const on = () => done(true);
      const t = setTimeout(() => done(false), timeout);
      this.addEventListener('ready', on);
    });
  }
}

// Base64 -> bytes (frame images).
function b64bytes(b64) {
  const s = atob(b64);
  const out = new Uint8Array(s.length);
  for (let i = 0; i < s.length; i++) out[i] = s.charCodeAt(i);
  return out;
}

// The board's USB serial chip (WCH CH343).
const USB_FILTER = { usbVendorId: 0x1a86, usbProductId: 0x55d3 };

const GroveLinkLib = { GroveLink, GroveError, replyName, b64bytes, USB_FILTER };
if (typeof module !== 'undefined' && module.exports) module.exports = GroveLinkLib;
else globalThis.GroveLinkLib = GroveLinkLib;
