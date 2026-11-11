// Driver for the Seeed Grove Vision AI V2 running SSCMA firmware.
//
// Protocol: send `AT+<CMD>\r\n` at 921600 baud. The board answers with one
// JSON object per line: type 0 = command reply, type 1 = event (inference
// results), type 2 = protocol error. Anything else on the line is raw log text.
import { EventEmitter } from 'node:events';
import { StringDecoder } from 'node:string_decoder';
import { SerialPort } from 'serialport';
import { toBoardTime } from './boardtime.js';

const BOOT_TIMEOUT_MS = 3000;
const REBOOT_TIMEOUT_MS = 8000;
const RETRY_MS = 2000;
const MAX_LINE = 4 * 1024 * 1024;
// The board's USB-serial chip (CH343), used to find the port if it re-enumerates as another ttyACM.
const USB_IDS = { vendorId: '1a86', productId: '55d3' };
const DEFAULT_PATH = '/dev/ttyACM0';

// Watchdog: frames normally arrive every 70-160 ms, so this long without one means the stream froze.
export const STALL_MS = 4000;
const WATCH_EVERY_MS = 1000;
const BACKOFF_STEP_MS = 15000;
const BACKOFF_MAX_MS = 60000;

// Recovery steps, mildest first. Each runs only if the one before didn't bring frames back.
// A frozen camera capture loop is usually fixed by the first step. A camera that lost
// contact needs a reboot. A board that stops answering commands needs the port reopened,
// which also hardware-resets it through the USB-serial control lines.
const STEPS = ['restart-stream', 'reinit-camera', 'reboot-board', 'reopen-port'];
const STEP_LABELS = {
  'restart-stream': 'Restarting the stream',
  'reinit-camera': 'Re-initialising the camera',
  'reboot-board': 'Rebooting the board',
  'reopen-port': 'Reopening the serial port (hardware reset)',
};

const ERROR_NAMES = {
  1: 'try again', 2: 'logic error', 3: 'timed out', 4: 'I/O error', 5: 'invalid argument',
  6: 'out of memory', 7: 'busy', 8: 'not supported', 9: 'not permitted',
};

// Commands the console refuses: they would hang the board (OTA), poke raw
// registers, or fight the stream state this driver manages.
const CONSOLE_BLOCKED = /^(OTA|SETREG|INVOKE=|SAMPLE=|BREAK|RST)\b/i;

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

// The board names its reply after the command up to "=": "SENSOR=1,1,2" -> "SENSOR", "ID?" -> "ID?".
const replyName = (cmd) => (cmd.includes('=') ? cmd.slice(0, cmd.indexOf('=')) : cmd);

const quote = (s) => `"${s.replace(/\\/g, '\\\\').replace(/"/g, '\\"')}"`;

function openErrorMessage(err, path) {
  const msg = err.message || String(err);
  if (/EACCES|Permission denied/i.test(msg)) {
    return `Permission denied on ${path}. Your user must be in the "dialout" group: run `
      + '"sudo usermod -aG dialout $USER" and log in again, or start the server with '
      + '"sg dialout -c \'npm start\'".';
  }
  if (/ENOENT|No such file/i.test(msg)) return `${path} not found. Is the board plugged in?`;
  if (/lock|busy|temporarily unavailable/i.test(msg)) {
    return `${path} is in use by another program (another copy of this server, or the SenseCraft web tool).`;
  }
  return `Could not open ${path}: ${msg}`;
}

// AT+INFO? holds the model card SenseCraft wrote when it flashed the model, as base64 JSON.
function decodeModelInfo(data) {
  if (!data?.info) return null;
  try {
    return JSON.parse(Buffer.from(data.info, 'base64').toString('utf8'));
  } catch {
    return { raw: data.info };
  }
}

// Motion detection on our grove-sd firmware: the board watches its live view and, on motion,
// does nothing (the page shows it), saves a VGA photo to /MOTION, or pulses a pin.
export const MOTION_ACTIONS = ['nothing', 'save', 'pulse'];
export const MOTION_PINS = ['PA0', 'PB6', 'PB7'];
export const MOTION_DEFAULTS = { on: false, action: 'nothing', pin: 'PA0', pulseMs: 100, sensitivity: 50 };

export function cleanMotion(m) {
  const out = {
    on: Boolean(m.on),
    action: String(m.action ?? MOTION_DEFAULTS.action),
    pin: String(m.pin ?? MOTION_DEFAULTS.pin),
    pulseMs: Number(m.pulseMs ?? MOTION_DEFAULTS.pulseMs),
    sensitivity: Number(m.sensitivity ?? MOTION_DEFAULTS.sensitivity),
  };
  if (!MOTION_ACTIONS.includes(out.action)) throw new Error(`Action must be one of: ${MOTION_ACTIONS.join(', ')}`);
  if (!MOTION_PINS.includes(out.pin)) throw new Error(`Pin must be one of: ${MOTION_PINS.join(', ')}`);
  if (!Number.isInteger(out.pulseMs) || out.pulseMs < 1 || out.pulseMs > 10000) throw new Error('Pulse length must be 1 to 10000 ms');
  if (!Number.isInteger(out.sensitivity) || out.sensitivity < 1 || out.sensitivity > 100) throw new Error('Sensitivity must be 1 to 100');
  return out;
}

const motionCmd = (m) => `MOTION=${m.on ? 1 : 0},${MOTION_ACTIONS.indexOf(m.action)},${MOTION_PINS.indexOf(m.pin)},${m.pulseMs},${m.sensitivity}`;

export class DeviceError extends Error {
  constructor(cmd, reply, logs = []) {
    const code = reply?.code;
    const detail = typeof reply?.data === 'string' ? `: ${reply.data}` : '';
    // Name the command, not its arguments: a WRITE carries kilobytes of base64.
    let msg = `AT+${replyName(cmd)} failed (code ${code}${ERROR_NAMES[code] ? `, ${ERROR_NAMES[code]}` : ''})${detail}`;
    if (logs.some((l) => /iic|i2c/i.test(l))) {
      msg += '. The camera is not answering. If you just connected it, reboot the board.';
    }
    super(msg);
    this.reply = reply;
    this.logs = logs;
  }
}

export class Grove extends EventEmitter {
  constructor({ path } = {}) {
    super();
    this.fixedPath = path || null; // null = find the board by its USB IDs
    this.path = path || DEFAULT_PATH;
    this.baudRate = 921600;
    this.port = null;
    this.state = 'disconnected'; // disconnected | connecting | ready | released
    this.error = null;
    this.info = {};
    // gray, size: live view in grayscale and its width, 320 (QVGA) or 640 (VGA). Our
    // grove-sd firmware only; smaller frames give more fps.
    this.stream = { want: true, running: false, image: true, gray: true, size: 320, fps: 60, video: 30 };
    // Motion detection on the board's live view (grove-sd firmware): what to do on motion.
    this.motion = { ...MOTION_DEFAULTS };
    this.motionLast = null;
    this.motionEvents = 0;
    this.waiters = new Set();
    this.cmdChain = Promise.resolve();
    this.opChain = Promise.resolve();
    this.opsPending = 0;
    this.decoder = new StringDecoder('utf8');
    this.buf = '';
    this.retryTimer = null;
    this.released = false;
    this.connecting = false;
    this.booting = false;
    this.reopening = false;
    this.lastFrameAt = 0;
    this.lastSensor = null; // last camera setting that streamed, for re-initialising it
    this.flavor = 'sscma';  // which firmware the board runs: 'sscma' (Seeed) or 'grove-sd' (our demo)
    this.wd = { stalled: false, busy: false, level: 0, nextAt: 0, attempts: 0, fixes: 0, last: null };
    this.wdTimer = null;
  }

  status() {
    const { stalled, busy, level, attempts, fixes, last } = this.wd;
    return {
      state: this.state,
      error: this.error,
      path: this.path,
      stream: { ...this.stream },
      watchdog: { stallMs: STALL_MS, stalled, busy, level, attempts, fixes, last },
      firmware: { flavor: this.flavor, demo: this.info.demo ?? null, streams: this.supportsStream() },
      motion: { ...this.motion, last: this.motionLast, events: this.motionEvents },
    };
  }

  health() {
    return {
      ...this.status(),
      lastFrameAgoMs: this.lastFrameAt ? Date.now() - this.lastFrameAt : null,
      uptimeS: Math.round(process.uptime()),
    };
  }

  _setState(state, error = null) {
    this.state = state;
    this.error = error;
    this._emitStatus();
  }

  _emitStatus() {
    this.emit('status', this.status());
  }

  // ---- connection ---------------------------------------------------------

  start() {
    this.released = false;
    if (!this.wdTimer) this.wdTimer = setInterval(() => this._watchdogTick(), WATCH_EVERY_MS);
    this._connect();
  }

  _scheduleRetry() {
    if (this.released || this.retryTimer) return;
    this.retryTimer = setTimeout(() => {
      this.retryTimer = null;
      this._connect();
    }, RETRY_MS);
  }

  async _resolvePath() {
    if (this.fixedPath) return this.fixedPath;
    try {
      const found = (await SerialPort.list()).find((p) => p.vendorId?.toLowerCase() === USB_IDS.vendorId
        && p.productId?.toLowerCase() === USB_IDS.productId);
      if (found) return found.path;
    } catch { /* fall back to the default */ }
    return DEFAULT_PATH;
  }

  async _connect() {
    if (this.port || this.released || this.connecting) return;
    this.connecting = true;
    try {
      await this._open();
    } finally {
      this.connecting = false;
    }
  }

  async _open() {
    this._setState('connecting', this.error);
    this.path = await this._resolvePath();
    const port = new SerialPort({ path: this.path, baudRate: this.baudRate, autoOpen: false });
    port.on('data', (chunk) => this._onData(chunk));
    port.on('close', () => this._onClose(port));
    port.on('error', (err) => this.emit('notice', `Serial error: ${err.message}`));
    try {
      await new Promise((resolve, reject) => port.open((e) => (e ? reject(e) : resolve())));
    } catch (err) {
      const msg = openErrorMessage(err, this.path);
      if (msg !== this.error) this.emit('notice', msg);
      this._setState('disconnected', msg);
      this._scheduleRetry();
      return;
    }
    this.port = port;
    this.buf = '';
    this.stream.running = false;

    // Opening the port reboots the board. Its bootloader drops into X-Modem
    // (firmware download) mode if it receives any byte in its first 100 ms, so
    // send nothing until the firmware announces itself.
    this.booting = true;
    await this._waitFor((m) => m.name === 'INIT@STAT?', BOOT_TIMEOUT_MS).catch(() => {});
    await sleep(200);
    this.booting = false;
    if (this.port !== port) return;
    this.emit('notice', `Connected to ${this.path}`);

    await this.op(async () => {
      await this._refresh();
      this._setState('ready');
      if (this.stream.want) await this._startInvoke();
    }).catch((err) => this.emit('notice', err.message));
  }

  _onClose(port) {
    if (this.port !== port) return;
    this.port = null;
    this.stream.running = false;
    for (const w of this.waiters) w.reject(new Error('Board disconnected'));
    this.waiters.clear();
    if (this.released) {
      this._setState('released');
    } else if (this.reopening) {
      this.reopening = false;
      this._setState('disconnected', null);
      this._connect();
    } else {
      this.emit('notice', `Lost connection to ${this.path}, retrying...`);
      this._setState('disconnected', 'Board disconnected');
      this._scheduleRetry();
    }
  }

  // ---- parsing ------------------------------------------------------------

  _onData(chunk) {
    this.buf += this.decoder.write(chunk);
    let i;
    while ((i = this.buf.indexOf('\n')) !== -1) {
      const line = this.buf.slice(0, i).trim();
      this.buf = this.buf.slice(i + 1);
      if (line) this._onLine(line);
    }
    if (this.buf.length > MAX_LINE) this.buf = '';
  }

  _onLine(line) {
    if (line.startsWith('{')) {
      let msg;
      try {
        msg = JSON.parse(line);
      } catch {
        this.emit('line', line.slice(0, 500));
        return;
      }
      this._onMessage(msg);
    } else {
      this.emit('line', line);
    }
  }

  _onMessage(msg) {
    for (const w of this.waiters) {
      if (w.test(msg)) w.resolve(msg);
    }
    if (msg.type === 1 && msg.name === 'INVOKE') {
      this._onFrame();
      this.emit('result', msg.data);
    } else if (msg.type === 1) {
      this.emit('event', msg); // e.g. CAPTURE progress from the SD demo firmware
      if (msg.name === 'TRACK') this.emit('track', msg.data);
      if (msg.name === 'TRACKPOS') { this._onFrame(); this.emit('trackpos', msg.data); }
      if (msg.name === 'MOTION') {
        this.motionLast = { at: Date.now(), ...msg.data, ok: msg.code === 0 };
        this.motionEvents++;
        this.emit('motion', this.motionLast);
      }
    } else if (msg.name === 'INIT@STAT?' && !this.booting && this.state === 'ready') {
      // The board rebooted on its own (reset button, brown-out): restore the stream.
      this.emit('notice', 'Board restarted');
      this.stream.running = false;
      this._emitStatus();
      this.op(async () => {
        await sleep(200);
        await this._refresh();
        if (this.stream.want) await this._startInvoke();
      }).catch((err) => this.emit('notice', err.message));
    }
  }

  _waitFor(test, timeout) {
    return new Promise((resolve, reject) => {
      const w = {
        test,
        resolve: (v) => { clearTimeout(w.timer); this.waiters.delete(w); resolve(v); },
        reject: (e) => { clearTimeout(w.timer); this.waiters.delete(w); reject(e); },
      };
      w.timer = setTimeout(() => w.reject(new Error('Timed out waiting for the board')), timeout);
      this.waiters.add(w);
    });
  }

  // ---- watchdog -----------------------------------------------------------

  _onFrame() {
    this.lastFrameAt = Date.now();
    const wd = this.wd;
    if (!wd.stalled) return;
    wd.stalled = false;
    wd.level = 0;
    wd.nextAt = 0;
    wd.fixes += 1;
    if (wd.last) wd.last.ok = true;
    this.emit('notice', `Frames are back (fixed by: ${wd.last?.label ?? 'unknown'})`);
    this._emitStatus();
  }

  _watchdogTick() {
    const wd = this.wd;
    if (this.state !== 'ready' || !this.stream.want || this.booting || wd.busy || this.opsPending) return;
    if (!this.supportsStream()) return;
    const now = Date.now();
    if (now - this.lastFrameAt < STALL_MS || now < wd.nextAt) return;

    // After the full ladder fails (camera unplugged, say), keep alternating the two
    // strongest steps, backing off so a dead camera doesn't reboot the board nonstop.
    const step = wd.level < STEPS.length ? STEPS[wd.level] : STEPS[2 + ((wd.level - STEPS.length) % 2)];
    wd.level += 1;
    if (wd.level >= STEPS.length) {
      wd.nextAt = now + Math.min(BACKOFF_MAX_MS, BACKOFF_STEP_MS * (wd.level - STEPS.length + 1));
    }
    const frozenS = this.lastFrameAt ? Math.round((now - this.lastFrameAt) / 1000) : null;
    wd.stalled = true;
    wd.busy = true;
    wd.attempts += 1;
    wd.last = { at: now, step, label: STEP_LABELS[step], ok: null };
    this.emit('notice', `Watchdog: no frames${frozenS ? ` for ${frozenS} s` : ''}. ${STEP_LABELS[step]}...`);
    this._emitStatus();

    this.op(() => this._recover(step))
      .then(() => { this.lastFrameAt = Date.now(); }) // give the step STALL_MS to produce a frame
      .catch((err) => {
        this.emit('notice', `Watchdog: ${STEP_LABELS[step]} failed: ${err.message}`);
        if (wd.last) wd.last.ok = false;
        this.lastFrameAt = 0; // escalate on the next tick
      })
      .finally(() => {
        wd.busy = false;
        this._emitStatus();
      });
  }

  async _recover(step) {
    if (step === 'restart-stream') {
      await this._send('BREAK');
      this.stream.running = false;
      await this._startInvoke();
    } else if (step === 'reinit-camera' && this.flavor === 'grove-sd') {
      // Our firmware: power-cycle the camera and set it up again (the only cure for a dead link).
      await this._send('BREAK');
      this.stream.running = false;
      await this._send('CAMRESET', { timeout: 10000 });
      await this._startInvoke();
    } else if (step === 'reinit-camera') {
      const s = this.lastSensor ?? { id: 1, opt: this.info.sensor?.opt_id ?? 2 };
      await this._send('BREAK');
      this.stream.running = false;
      await this._send(`SENSOR=${s.id},1,${s.opt}`, { timeout: 5000 });
      await this._startInvoke();
    } else if (step === 'reboot-board') {
      await this._rebootBoard();
    } else if (step === 'reopen-port') {
      if (!this.port) return;
      this.reopening = true;
      const port = this.port;
      await new Promise((resolve) => port.close(() => resolve()));
      // _onClose reconnects, which reboots the board and restarts the stream.
    }
  }

  // ---- commands -----------------------------------------------------------

  // Low-level: one command on the wire at a time.
  _exec(cmd, { timeout = 3000 } = {}) {
    const run = async () => {
      if (!this.port) throw new Error('Board not connected');
      const name = replyName(cmd);
      const logs = [];
      const onLine = (l) => logs.push(l);
      this.on('line', onLine);
      try {
        const reply = this._waitFor((m) => (m.type === 0 && m.name === name) || m.type === 2, timeout);
        this.port.write(`AT+${cmd}\r\n`);
        return { reply: await reply.catch(() => null), logs };
      } finally {
        this.off('line', onLine);
      }
    };
    const p = this.cmdChain.then(run);
    this.cmdChain = p.catch(() => {});
    return p;
  }

  async _send(cmd, opts) {
    const { reply, logs } = await this._exec(cmd, opts);
    if (!reply) throw new Error(`No reply to AT+${cmd}`);
    if (reply.code !== 0) throw new DeviceError(cmd, reply, logs);
    return reply.data;
  }

  // High-level operations run one at a time so a settings change can pause
  // and resume the stream without another request interleaving.
  op(fn) {
    this.opsPending += 1;
    const p = this.opChain.then(fn).finally(() => { this.opsPending -= 1; });
    this.opChain = p.catch(() => {});
    return p;
  }

  async _paused(fn) {
    if (this.stream.running) await this._stopInvoke();
    try {
      return await fn();
    } finally {
      if (this.stream.want && this.port) await this._startInvoke().catch((e) => this.emit('notice', e.message));
    }
  }

  // Seeed's firmware and SD demo 2 stream camera frames. SD demo 1 has no camera.
  supportsStream() {
    return this.flavor !== 'grove-sd' || (this.info.demo ?? 1) >= 2;
  }

  async _startInvoke() {
    if (!this.supportsStream()) return;
    this.lastFrameAt = Date.now(); // the watchdog's grace period starts now
    if (this.flavor === 'grove-sd') {
      // Older builds of our firmware have no PREVIEW command (always colour): ignore that.
      await this._send(this._previewCmd()).catch(() => {});
      await this._send(`VIDEO=${this.stream.video}`).catch(() => {});
      await this._send(motionCmd(this.motion)).catch(() => {});
    }
    const data = await this._send(`INVOKE=-1,0,${this.stream.image ? 0 : 1}`).catch((err) => {
      this.stream.running = false;
      this._emitStatus();
      throw err;
    });
    this.stream.running = true;
    this.lastFrameAt = Date.now();
    const s = data?.sensor;
    if (s?.id && s.opt_id >= 0) this.lastSensor = { id: s.id, opt: s.opt_id };
    if (data?.algorithm) {
      this.info = { ...this.info, algorithm: data.algorithm };
      this.emit('info', this.info);
    }
    this._emitStatus();
  }

  async _stopInvoke() {
    if (!this.stream.running) return;
    await this._send('BREAK');
    this.stream.running = false;
    this._emitStatus();
  }

  async _rebootBoard() {
    if (!this.port) throw new Error('Board not connected');
    this.booting = true;
    this.stream.running = false;
    this._emitStatus();
    try {
      const booted = this._waitFor((m) => m.name === 'INIT@STAT?', REBOOT_TIMEOUT_MS);
      this.port.write('AT+RST\r\n');
      await booted;
      await sleep(200);
    } finally {
      this.booting = false;
    }
    this.emit('notice', 'Board rebooted');
    await this._refresh();
    if (this.stream.want) await this._startInvoke();
  }

  async _refresh() {
    const q = (cmd) => this._send(cmd).catch((err) => {
      this.emit('notice', err.message);
      return null;
    });
    const version = await q('VER?');
    this.flavor = version?.firmware === 'grove-sd' ? 'grove-sd' : 'sscma';
    if (this.flavor === 'grove-sd') {
      // Our own SD demo firmware (firmware/grove_sd_demo): no models or detection.
      await q(`TIME=${toBoardTime()}`);
      const info = {
        flavor: this.flavor,
        demo: version.demo ?? 1,
        id: await q('ID?'),
        name: await q('NAME?'),
        version,
        stat: await q('STAT?'),
        sd: await q('SD?'),
      };
      this.info = info;
      this.emit('info', info);
      return info;
    }
    const info = {
      flavor: this.flavor,
      id: await q('ID?'),
      name: await q('NAME?'),
      version,
      stat: await q('STAT?'),
      sensors: await q('SENSORS?'),
      sensor: await q('SENSOR?'),
      models: await q('MODELS?'),
      model: await q('MODEL?'),
      modelInfo: decodeModelInfo(await q('INFO?')),
      action: (await q('ACTION?'))?.action ?? null,
      algorithm: this.info.algorithm ?? null,
    };
    this.info = info;
    this.emit('info', info);
    return info;
  }

  // ---- public API ---------------------------------------------------------

  refresh() {
    return this.op(() => this._refresh());
  }

  setStream({ running, image, gray, size, fps, video }) {
    if (running && this.state === 'ready' && !this.supportsStream()) {
      throw new Error('This firmware (SD demo 1) has no camera stream. Flash demo 2 for the preview.');
    }
    return this.op(async () => {
      const newSize = Number(size);
      if (size !== undefined && newSize !== 320 && newSize !== 640) throw new Error('Live size must be 320 or 640');
      const newFps = Number(fps);
      if (fps !== undefined && ![15, 30, 45, 60, 70].includes(newFps)) throw new Error('Camera fps must be 15, 30, 45, 60 or 70 (max)');
      const newVideo = Number(video);
      if (video !== undefined && !(Number.isInteger(newVideo) && newVideo >= 0 && newVideo <= 60)) throw new Error('Video must be 0-60 frames per second');
      if (video !== undefined && newVideo !== this.stream.video) {
        this.stream.video = newVideo;
        if (this.flavor === 'grove-sd' && this.port) await this._send(`VIDEO=${newVideo}`);
      }
      const grayChanged = typeof gray === 'boolean' && gray !== this.stream.gray;
      const sizeChanged = size !== undefined && newSize !== this.stream.size;
      const fpsChanged = fps !== undefined && newFps !== this.stream.fps;
      if (grayChanged || sizeChanged || fpsChanged) {
        if (grayChanged) this.stream.gray = gray;
        if (sizeChanged) this.stream.size = newSize;
        if (fpsChanged) this.stream.fps = newFps;
        // The firmware switches a running preview over by itself.
        if (this.flavor === 'grove-sd' && this.port) await this._send(this._previewCmd());
      }
      if (typeof image === 'boolean' && image !== this.stream.image) {
        this.stream.image = image;
        if (this.stream.running) {
          await this._stopInvoke();
          await this._startInvoke();
        }
      }
      if (typeof running === 'boolean') {
        this.stream.want = running;
        if (!running) {
          this.wd.stalled = false;
          this.wd.level = 0;
          this.wd.nextAt = 0;
        }
        if (running && !this.stream.running && this.port) await this._startInvoke();
        if (!running && this.stream.running) await this._stopInvoke();
      }
      this._emitStatus();
      return this.stream;
    });
  }

  // Motion detection settings (see MOTION_DEFAULTS). Partial updates are fine.
  setMotion(changes) {
    const next = cleanMotion({ ...this.motion, ...changes });
    return this.op(async () => {
      if (this.flavor === 'grove-sd' && this.port) await this._send(motionCmd(next));
      this.motion = next;
      this._emitStatus();
      return this.motion;
    });
  }

  // Object tracking on the live view (grove-sd firmware): lock on the centre box, or stop.
  // x, y: the box centre in live-frame pixels (default: the frame centre).
  track(on, { w, h, x, y } = {}) {
    if (this.flavor !== 'grove-sd') throw new Error('Tracking needs the SD demo 2 firmware.');
    let size = on && w ? `,${Math.round(w)},${Math.round(h || (w * 3) / 4)}` : '';
    if (size && Number.isFinite(x) && Number.isFinite(y)) size += `,${Math.round(x)},${Math.round(y)}`;
    return this.op(() => this._send(`TRACK=${on ? 1 : 0}${size}`, { timeout: 5000 }));
  }

  trackStatus() {
    return this.op(() => this._send('TRACK?', { timeout: 5000 }));
  }

  // The lock-on button: a header pin (PA0, PB6 or PB7), or null for none.
  trackButton(pin, pressedLevel = 0) {
    const pins = ['PA0', 'PB6', 'PB7'];
    const i = pin == null ? -1 : pins.indexOf(String(pin));
    if (pin != null && i < 0) throw new Error(`Pin must be one of: ${pins.join(', ')}`);
    return this.op(() => this._send(`TRACKBTN=${i},${pressedLevel ? 1 : 0}`, { timeout: 5000 }));
  }

  // Runs the selected motion action once, as if something moved (or one test pulse).
  testMotion() {
    if (this.flavor !== 'grove-sd') throw new Error('Motion detection needs the SD demo 2 firmware.');
    return this.op(() => this._send(this.motion.action === 'nothing' ? 'PULSE' : 'MOTIONTEST', { timeout: 15000 }));
  }

  _previewCmd() {
    return `PREVIEW=${this.stream.gray ? 1 : 0},${this.stream.size},1,${this.stream.fps}`;
  }

  setSensor(id, opt) {
    return this.op(() => this._paused(async () => {
      await this._send(`SENSOR=${id},1,${opt}`, { timeout: 5000 });
      this.info = { ...this.info, sensor: await this._send('SENSOR?'), sensors: await this._send('SENSORS?') };
      this.emit('info', this.info);
    }));
  }

  setModel(id) {
    return this.op(() => this._paused(async () => {
      await this._send(`MODEL=${id}`, { timeout: 5000 });
      await this._refresh();
    }));
  }

  setAction(expression) {
    if (expression.length > 512) throw new Error('Expression is too long (max 512 characters)');
    return this.op(async () => {
      await this._send(`ACTION=${quote(expression)}`);
      this.info = { ...this.info, action: (await this._send('ACTION?'))?.action ?? null };
      this.emit('info', this.info);
    });
  }

  // AT+LED= never replies on this firmware; only a protocol error comes back.
  setLed(on) {
    return this.op(async () => {
      const { reply } = await this._exec(`LED=${on ? 1 : 0}`, { timeout: 500 });
      if (reply && reply.code !== 0) throw new DeviceError(`LED=${on ? 1 : 0}`, reply);
    });
  }

  reboot() {
    return this.op(() => this._rebootBoard());
  }

  console(command, timeout = 2500) {
    const cmd = command.trim().replace(/^AT\+/i, '');
    if (!cmd) throw new Error('Type a command, for example ID? or HELP?');
    if (CONSOLE_BLOCKED.test(cmd)) {
      throw new Error(`AT+${cmd} is blocked here. Use the Preview controls or the Reboot button instead.`);
    }
    return this.op(() => this._exec(cmd, { timeout: Math.min(Math.max(Number(timeout) || 2500, 500), 120000) }));
  }

  release() {
    return this.op(async () => {
      if (this.stream.running) await this._stopInvoke().catch(() => {});
      this.released = true;
      clearTimeout(this.retryTimer);
      this.retryTimer = null;
      if (this.port) {
        await new Promise((resolve) => this.port.close(() => resolve()));
      } else {
        this._setState('released');
      }
    });
  }

  connect() {
    if (this.port) return Promise.resolve();
    this.released = false;
    this.error = null;
    this._connect();
    return Promise.resolve();
  }

  async shutdown() {
    this.released = true;
    clearTimeout(this.retryTimer);
    clearInterval(this.wdTimer);
    if (!this.port) return;
    // Leave the board idle rather than streaming into a closed port.
    await Promise.race([this._send('BREAK').catch(() => {}), sleep(1000)]);
    await new Promise((resolve) => this.port.close(() => resolve()));
  }
}
