// SD card access and camera capture through the grove-sd demo firmware
// (firmware/grove_sd_demo). Files move in base64 chunks checked with CRC32.
import { mkdir, writeFile } from 'node:fs/promises';
import { join } from 'node:path';
import { Worker } from 'node:worker_threads';
import zlib from 'node:zlib';
import { fromBoardTime } from './boardtime.js';
import { DeviceError } from './grove.js';

const READ_CHUNK = 12288;    // the firmware's maximum
const WRITE_CHUNK = 8192;    // keeps the command line (base64 + path) under the firmware's 16 KB limit
const RETRIES = 3;

const quote = (s) => `"${String(s).replace(/\\/g, '\\\\').replace(/"/g, '\\"')}"`;

export function cleanPath(p) {
  const s = String(p ?? '/').replace(/\\/g, '/');
  const parts = [];
  for (const part of s.split('/')) {
    if (!part || part === '.') continue;
    if (part === '..') parts.pop();
    else parts.push(part);
  }
  return `/${parts.join('/')}`;
}

// Sensor area used for captures (the last 8 of its 1944 lines fail too often, see
// firmware/README.md) and the board's tile size.
const SENSOR_W = 2592;
const SENSOR_H = 1936;
const TILE_W = 640;
const TILE_H = 480;

// How to take a W x H photo:
// - Up to 320x240: the whole view in one 320x240 tile, subsampled 8x on the board (sub 8).
// - Up to 640x480: the whole view in one 640x480 tile, subsampled 4x (sub 4).
// - Up to 1280x960: the whole view subsampled 2x (sub 2), 2x2 tiles at most.
// - Larger: native pixels from the centre of the sensor (sub 1), like the OV5647's own 1080p.
// When the board's single tile is already the wanted size (320x240, 640x480), the board saves
// it as full.jpg itself (direct): nothing to download, join or write back. Otherwise the
// board's image has the target's aspect ratio, is at least one tile and sits on the JPEG's
// 16-pixel block grid; the server joins the tiles without re-compressing them when the size
// already matches, or crops and scales once when it doesn't.
const ceil16 = (n) => Math.ceil(n / 16) * 16;
export function planCapture(W, H) {
  for (const [sub, maxW, maxH, tw, th] of [[8, 320, 240, 320, 240], [4, 640, 480, 640, 480], [2, 1280, 960, 640, 480]]) {
    if (W > maxW || H > maxH) continue;
    let w, h;
    if (W * maxH >= H * maxW) { w = maxW; h = Math.min(maxH, ceil16((maxW * H) / W)); }
    else { h = maxH; w = Math.min(maxW, ceil16((maxH * W) / H)); }
    w = Math.max(w, tw);
    h = Math.max(h, th);
    if (sub === 8) { w = 320; h = 240; }           // the board's 8x mode makes exactly one 320x240 tile
    const x = Math.floor((SENSOR_W - w * sub) / 8) * 4;     // centred, multiple of 4
    const y = Math.floor((SENSOR_H - h * sub) / 8) * 4;
    const direct = w === W && h === H && w <= tw && h <= th;
    return { sub, w, h, x, y, direct, view: 'whole view' };
  }
  const w = Math.min(SENSOR_W, ceil16(Math.max(W, TILE_W))), h = Math.min(SENSOR_H, ceil16(Math.max(H, TILE_H)));
  return { sub: 1, w, h, x: Math.floor((SENSOR_W - w) / 4) * 2, y: Math.floor((SENSOR_H - h) / 4) * 2, direct: false, view: 'centre crop' };
}

function stitchInWorker(job) {
  return new Promise((resolve, reject) => {
    const worker = new Worker(new URL('./stitch-worker.js', import.meta.url), { workerData: job });
    worker.once('message', (m) => (m.error ? reject(new Error(m.error))
      : resolve({ jpeg: Buffer.from(m.jpeg), grain: m.grain, method: m.method })));
    worker.once('error', reject);
  });
}

export class SdCard {
  constructor(grove, { captureDir } = {}) {
    this.g = grove;
    this.captureDir = captureDir;
  }

  _need() {
    if (this.g.flavor !== 'grove-sd') {
      throw new Error('The board is not running the Grove SD demo firmware, so the SD card is not reachable.');
    }
  }

  _needCamera() {
    this._need();
    if ((this.g.info.demo ?? 1) < 2) throw new Error('Capturing needs the demo-2 firmware (this board runs demo-1).');
  }

  _progress(job, done, total, extra = {}) {
    this.g.emit('progress', { ...job, done, total, ...extra });
  }

  // ---- low level (call inside grove.op) ------------------------------------

  async _readChunk(path, offset) {
    for (let attempt = 1; ; attempt++) {
      try {
        const d = await this.g._send(`READ=${quote(path)},${offset},${READ_CHUNK}`, { timeout: 5000 });
        const buf = Buffer.from(d.data, 'base64');
        if (buf.length !== d.len || zlib.crc32(buf) !== d.crc) throw new Error(`Damaged chunk at ${offset}`);
        return { buf, size: d.size };
      } catch (err) {
        if (err instanceof DeviceError || attempt >= RETRIES) throw err;
        this.g.emit('notice', `SD read retry ${attempt} at ${path}:${offset} (${err.message})`);
      }
    }
  }

  // Reads a whole file, handing each chunk to onChunk as it arrives.
  async _read(path, onChunk, job) {
    let offset = 0;
    let size = Infinity;
    while (offset < size) {
      if (job?.aborted) throw new Error('Cancelled');
      const { buf, size: total } = await this._readChunk(path, offset);
      size = total;
      if (!buf.length && offset < size) throw new Error(`File ended early at ${offset} of ${size}`);
      offset += buf.length;
      await onChunk(buf, offset, size);
      if (job) this._progress(job, offset, size);
    }
    return size;
  }

  async _readAll(path, job) {
    const parts = [];
    await this._read(path, (b) => { parts.push(b); }, job);
    return Buffer.concat(parts);
  }

  async _write(path, data, job) {
    let offset = 0;
    let failures = 0;
    do {
      const chunk = data.subarray(offset, offset + WRITE_CHUNK);
      try {
        const d = await this.g._send(
          `WRITE=${quote(path)},${offset},"${chunk.toString('base64')}",${zlib.crc32(chunk)}`,
          { timeout: 10000 },
        );
        offset = d.size;
        failures = 0;
      } catch (err) {
        if (++failures > RETRIES) throw err;
        this.g.emit('notice', `SD write retry ${failures} at ${path}:${offset} (${err.message})`);
        // The chunk may have been written even though the reply was lost: ask where the file ends.
        if (offset > 0) {
          const st = await this.g._send(`FSTAT=${quote(path)}`).catch(() => null);
          if (st && st.size === offset + chunk.length) offset = st.size;
          else if (st && st.size !== offset) throw new Error(`Upload out of step: file is ${st.size} bytes, expected ${offset}`);
        }
      }
      if (job) this._progress(job, offset, data.length);
    } while (offset < data.length);
  }

  // ---- public API --------------------------------------------------------------

  info() {
    this._need();
    return this.g.op(() => this.g._send('SD?', { timeout: 10000 }));
  }

  mount() {
    this._need();
    return this.g.op(() => this.g._send('SDMOUNT', { timeout: 10000 }));
  }

  list(path) {
    this._need();
    const p = cleanPath(path);
    return this.g.op(async () => {
      const entries = [];
      for (let start = 0; ;) {
        const d = await this.g._send(`LS=${quote(p)},${start}`, { timeout: 10000 });
        for (const [name, size, dir, mtime] of d.entries) {
          entries.push({ name, size, dir: Boolean(dir), mtime: fromBoardTime(mtime) });
        }
        if (!d.more || !d.entries.length) break;
        start += d.entries.length;
      }
      entries.sort((a, b) => (b.dir - a.dir) || a.name.localeCompare(b.name, undefined, { numeric: true }));
      return { path: p, entries };
    });
  }

  stat(path) {
    this._need();
    return this.g.op(async () => {
      const d = await this.g._send(`FSTAT=${quote(cleanPath(path))}`);
      return { ...d, dir: Boolean(d.dir), mtime: fromBoardTime(d.mtime) };
    });
  }

  // Streams a file: onChunk(buffer, bytesSoFar, total). job.aborted = true stops it.
  read(path, onChunk, job) {
    this._need();
    return this.g.op(() => this.g._paused(() => this._read(cleanPath(path), onChunk, job)));
  }

  write(path, data, job) {
    this._need();
    return this.g.op(() => this.g._paused(async () => {
      try {
        await this._write(cleanPath(path), data, job);
      } finally {
        if (job) this.g.emit('progress', { ...job, finished: true });
      }
    }));
  }

  remove(path, recursive = false) {
    this._need();
    // Card changes pause the live view: while frames stream, an RM went unanswered about half the time.
    return this.g.op(() => this.g._paused(() => this.g._send(`RM=${quote(cleanPath(path))}${recursive ? ',1' : ''}`, { timeout: 60000 })));
  }

  // Erases the card. `confirm` must be the string "ERASE".
  format(kind = 'FAT32', confirm = '') {
    this._need();
    if (confirm !== 'ERASE') throw new Error('Formatting erases the card: send confirm "ERASE" to go ahead.');
    if (kind !== 'FAT32' && kind !== 'exFAT') throw new Error('Format must be FAT32 or exFAT');
    return this.g.op(() => this.g._paused(() => this.g._send(`FORMAT="${kind}"`, { timeout: 180000 })));
  }

  mkdir(path) {
    this._need();
    return this.g.op(() => this.g._paused(() => this.g._send(`MKDIR=${quote(cleanPath(path))}`)));
  }

  move(from, to) {
    this._need();
    return this.g.op(() => this.g._paused(() => this.g._send(`MV=${quote(cleanPath(from))},${quote(cleanPath(to))}`)));
  }

  // Captures a high-resolution image as tiles on the card, joins them here, and writes the
  // joined JPEG back next to the tiles as full.jpg (and keeps a local copy).
  capture({ width = 640, height = 480 } = {}) {
    this._needCamera();
    const job = { id: `capture-${Date.now()}`, op: 'capture', label: `Capturing ${width}x${height}` };
    return this.g.op(() => this.g._paused(() => this._capture(job, width, height).finally(() => {
      this._progress(job, 1, 1, { stage: 'Done', finished: true });
    })));
  }

  // A one-tile photo that the board saved as full.jpg itself: just keep a local copy.
  async _finishDirect(job, m, width, height, t0, tCapture) {
    const sdPath = `${m.dir}/full.jpg`;
    const name = `${m.dir.split('/').pop()}.jpg`;
    let localUrl = null;
    this._progress(job, 0, 1, { stage: 'Copying full.jpg from the card' });
    if (this.captureDir) {
      const jpeg = await this._readAll(sdPath);
      await mkdir(this.captureDir, { recursive: true });
      await writeFile(join(this.captureDir, name), jpeg);
      localUrl = `/captures/${encodeURIComponent(name)}`;
    }
    return {
      dir: m.dir, sdPath, width, height, view: m.view, sub: m.sub, tiles: 1, retaken: 0, size: m.tiles[0][3], localUrl,
      joined: 'direct', ms: { capture: tCapture - t0, copy: Date.now() - tCapture },
    };
  }

  async _capture(job, width, height) {
    const t0 = Date.now();
    const onEvent = (msg) => {
      if (msg.name === 'CAPTURE') this._progress(job, msg.data.tile, msg.data.of, { stage: 'Taking tiles' });
    };
    this.g.on('event', onEvent);
    let m;
    try {
      const p = planCapture(width, height);
      m = await this.g._send(`CAPTURE=${p.w},${p.h},${p.x},${p.y},${p.sub},${p.direct ? 1 : 0}`, { timeout: 120000 });
      m.view = p.view;
    } finally {
      this.g.off('event', onEvent);
    }
    const tCapture = Date.now();
    if (m.direct) return this._finishDirect(job, m, width, height, t0, tCapture);

    const sub = m.sub ?? 1;     // the board's subsample factor (1 = native pixels)
    const job2 = { width: m.width, height: m.height, outW: width, outH: height };
    const tiles = [];
    for (const [i, [file, x, y]] of m.tiles.entries()) {
      this._progress(job, i, m.tiles.length, { stage: 'Downloading tiles' });
      tiles.push({ x: (x - m.x) / sub, y: (y - m.y) / sub, jpeg: await this._readAll(`${m.dir}/${file}`) });
    }
    const tDownload = Date.now();

    this._progress(job, 0, 1, { stage: 'Joining tiles' });
    let { jpeg, grain, method } = await stitchInWorker({ ...job2, tiles });

    // Now and then one tile's frame arrives with its colour pattern shifted a pixel (pink,
    // grainy). Spot those by their grain and take them again, up to twice.
    let retaken = 0;
    for (let round = 0; round < 2; round++) {
      const sorted = [...grain].sort((a, b) => a - b);
      const median = sorted[Math.floor(sorted.length / 2)];
      const bad = grain.map((g, i) => (g > 5 && g > 3 * median ? i : -1)).filter((i) => i >= 0);
      if (!bad.length) break;
      for (const i of bad) {
        const [file, sx, sy] = m.tiles[i];
        this._progress(job, retaken, retaken + bad.length, { stage: `Re-taking tile ${file}` });
        this.g.emit('notice', `Capture: tile ${file} looks shifted (grain ${grain[i].toFixed(1)} vs median ${median.toFixed(1)}), taking it again`);
        const tileSize = sub === 8 ? '320,240' : '640,480';
        const r = await this.g._send(`CAPTURE=${tileSize},${sx},${sy},${sub},0`, { timeout: 30000 });
        const fresh = await this._readAll(`${r.dir}/${r.tiles[0][0]}`);
        await this._write(`${m.dir}/${file}`, fresh);           // replace the tile on the card
        await this.g._send(`RM="${r.dir}",1`, { timeout: 30000 });
        tiles[i] = { ...tiles[i], jpeg: fresh };
        retaken++;
      }
      ({ jpeg, grain, method } = await stitchInWorker({ ...job2, tiles }));
    }
    const tStitch = Date.now();

    const sdPath = `${m.dir}/full.jpg`;
    await this._write(sdPath, jpeg, { ...job, stage: 'Saving full.jpg to the card' });
    const name = `${m.dir.split('/').pop()}.jpg`;
    let localUrl = null;
    if (this.captureDir) {
      await mkdir(this.captureDir, { recursive: true });
      await writeFile(join(this.captureDir, name), jpeg);
      localUrl = `/captures/${encodeURIComponent(name)}`;
    }
    return {
      dir: m.dir, sdPath, width, height, view: m.view, sub, tiles: m.tiles.length, retaken, size: jpeg.length, localUrl,
      joined: method,
      ms: { capture: tCapture - t0, download: tDownload - tCapture, stitch: tStitch - tDownload, save: Date.now() - tStitch },
    };
  }
}
