import { readFileSync } from 'node:fs';
import { readdir, stat, writeFile } from 'node:fs/promises';
import { createServer } from 'node:http';
import { networkInterfaces } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import express from 'express';
import { WebSocketServer } from 'ws';
import { Grove, cleanMotion } from './lib/grove.js';
import { Suggester } from './lib/suggest.js';
import { SdCard, cleanPath } from './lib/sd.js';

const PORT = Number(process.env.PORT ?? 3000);
const HOST = process.env.HOST ?? '0.0.0.0';
// Unset = find the board by its USB IDs, so it is found again if it comes back as another ttyACM.
const SERIAL_PORT = process.env.SERIAL_PORT || undefined;
// A browser that can't keep up skips frames instead of queueing them.
const MAX_BUFFERED = 1_000_000;
// Lets browsers notice a dead connection even while the stream is paused.
const TICK_MS = 5000;

// Local copies of joined captures (the full-resolution JPEG is also written back to the SD card).
const CAPTURE_DIR = process.env.CAPTURE_DIR || fileURLToPath(new URL('./captures', import.meta.url));
// Uploads cross the board's ~70 KB/s serial link, so keep them reasonable.
const MAX_UPLOAD = '32mb';

// Lock-on preferences: box width as a fraction of the frame (1/div), and whether Lock on snaps
// to the suggested object.
const trackPrefs = { div: 4, snap: true };
const cleanTrackPrefs = (t) => ({
  div: [2, 3, 4].includes(Number(t.div)) ? Number(t.div) : trackPrefs.div,
  snap: t.snap === undefined ? trackPrefs.snap : Boolean(t.snap),
});

// Settings that should survive a restart (live view format, motion detection).
const SETTINGS_FILE = process.env.SETTINGS_FILE || fileURLToPath(new URL('./settings.json', import.meta.url));

const grove = new Grove({ path: SERIAL_PORT });
try {
  const saved = JSON.parse(readFileSync(SETTINGS_FILE, 'utf8'));
  if (typeof saved.stream?.gray === 'boolean') grove.stream.gray = saved.stream.gray;
  if (saved.stream?.size === 320 || saved.stream?.size === 640) grove.stream.size = saved.stream.size;
  if ([15, 30, 45, 60, 70].includes(saved.stream?.fps)) grove.stream.fps = saved.stream.fps;
  if (Number.isInteger(saved.stream?.video) && saved.stream.video >= 0 && saved.stream.video <= 60) grove.stream.video = saved.stream.video;
  if (saved.motion) grove.motion = cleanMotion(saved.motion);
  if (saved.track) Object.assign(trackPrefs, cleanTrackPrefs(saved.track));
} catch { /* no settings saved yet, or unreadable: keep the defaults */ }
const saveSettings = () => writeFile(SETTINGS_FILE, `${JSON.stringify({
  stream: { gray: grove.stream.gray, size: grove.stream.size, fps: grove.stream.fps, video: grove.stream.video }, motion: grove.motion, track: trackPrefs,
}, null, 2)}\n`).catch((err) => console.log(`[grove] Could not save settings: ${err.message}`));
const sd = new SdCard(grove, { captureDir: CAPTURE_DIR });
const app = express();
app.use(express.json({ limit: '64kb' }));
app.use(express.static(fileURLToPath(new URL('./public', import.meta.url))));
app.use('/captures', express.static(CAPTURE_DIR));

const int = (v, name) => {
  const n = Number(v);
  if (!Number.isInteger(n) || n < 0) throw new Error(`${name} must be a whole number`);
  return n;
};

const api = (fn) => async (req, res) => {
  try {
    const args = { ...req.query, ...(req.body && !Buffer.isBuffer(req.body) ? req.body : {}) };
    res.json({ ok: true, result: (await fn(args, req)) ?? null });
  } catch (err) {
    res.status(400).json({ ok: false, error: err.message });
  }
};

const MIME = { jpg: 'image/jpeg', jpeg: 'image/jpeg', png: 'image/png', bmp: 'image/bmp', gif: 'image/gif',
  txt: 'text/plain; charset=utf-8', log: 'text/plain; charset=utf-8', csv: 'text/csv', json: 'application/json' };
const mimeFor = (name) => MIME[name.split('.').pop().toLowerCase()] ?? 'application/octet-stream';
const evenInt = (v, name, min, max) => {
  const n = Number(v);
  if (!Number.isInteger(n) || n % 2 || n < min || n > max) throw new Error(`${name} must be an even number from ${min} to ${max}`);
  return n;
};

app.get('/api/state', (req, res) => res.json({ status: grove.status(), info: grove.info }));
// For the systemd health check: answering at all proves the server isn't hung.
app.get('/api/health', (req, res) => res.json(grove.health()));
app.post('/api/refresh', api(() => grove.refresh()));
app.post('/api/stream', api(async (b) => {
  const r = await grove.setStream({ running: b.running, image: b.image, gray: b.gray, size: b.size, fps: b.fps, video: b.video });
  if (b.gray !== undefined || b.size !== undefined || b.fps !== undefined || b.video !== undefined) saveSettings();
  return r;
}));
app.post('/api/motion', api(async (b) => {
  const r = await grove.setMotion(b);
  saveSettings();
  return r;
}));
app.post('/api/motion/test', api(() => grove.testMotion()));
app.post('/api/track', api((b) => {
  // snap: use the suggested object box (if one is fresh and shown) instead of the plain box
  if (b.on && b.snap && suggestion.data?.confident && Date.now() - suggestion.at < 2000) {
    const [x, y, w, h] = suggestion.data.box;
    return grove.track(true, { w, h, x, y });
  }
  return grove.track(Boolean(b.on), { w: Number(b.w) || 0, h: Number(b.h) || 0, x: Number(b.x), y: Number(b.y) });
}));
app.get('/api/track/prefs', api(() => trackPrefs));
app.post('/api/track/prefs', api((b) => {
  Object.assign(trackPrefs, cleanTrackPrefs(b));
  saveSettings();
  return trackPrefs;
}));
app.get('/api/track/suggest', api(() => suggestion.data));
app.get('/api/track', api(() => grove.trackStatus()));
app.post('/api/track/button', api((b) => grove.trackButton(b.pin ?? null, b.pressedLevel ? 1 : 0)));
app.post('/api/sensor', api((b) => grove.setSensor(int(b.id, 'Sensor id'), int(b.opt, 'Resolution option'))));
app.post('/api/model', api((b) => grove.setModel(int(b.id, 'Model id'))));
app.post('/api/action', api((b) => grove.setAction(String(b.expression ?? '').trim())));
app.post('/api/led', api((b) => grove.setLed(Boolean(b.on))));
app.post('/api/reboot', api(() => grove.reboot()));
app.post('/api/console', api((b) => grove.console(String(b.command ?? ''), b.timeout)));
app.post('/api/release', api(() => grove.release()));
app.post('/api/connect', api(() => grove.connect()));

// ---- SD card and capture (grove-sd demo firmware) ---------------------------

app.get('/api/sd', api(() => sd.info()));
app.post('/api/sd/mount', api(() => sd.mount()));
app.get('/api/sd/list', api((a) => sd.list(a.path ?? '/')));
app.delete('/api/sd/file', api((a) => sd.remove(String(a.path ?? ''), a.recursive === '1' || a.recursive === true)));
app.post('/api/sd/mkdir', api((a) => sd.mkdir(String(a.path ?? ''))));
app.post('/api/sd/format', api((a) => sd.format(String(a.format ?? 'FAT32'), String(a.confirm ?? ''))));
app.post('/api/sd/move', api((a) => sd.move(String(a.from ?? ''), String(a.to ?? ''))));
app.put('/api/sd/file', express.raw({ type: () => true, limit: MAX_UPLOAD }), api((a, req) => {
  const path = cleanPath(a.path);
  const name = path.split('/').pop();
  return sd.write(path, Buffer.isBuffer(req.body) ? req.body : Buffer.alloc(0),
    { id: `up-${Date.now()}`, op: 'upload', label: `Uploading ${name}` });
}));

// Streams a file off the card as it arrives over the serial link.
app.get('/api/sd/file', async (req, res) => {
  const path = cleanPath(req.query.path);
  const name = path.split('/').pop() || 'file';
  const job = { id: `dl-${Date.now()}`, op: 'download', label: `Downloading ${name}` };
  res.on('close', () => { if (!res.writableFinished) job.aborted = true; });
  let started = false;
  try {
    await sd.read(path, async (buf, done, total) => {
      if (!started) {
        started = true;
        res.setHeader('Content-Type', mimeFor(name));
        res.setHeader('Content-Length', total);
        res.setHeader('Content-Disposition',
          `${req.query.inline ? 'inline' : 'attachment'}; filename*=UTF-8''${encodeURIComponent(name)}`);
      }
      if (!res.write(buf)) await new Promise((r) => { res.once('drain', r); res.once('close', r); });
    }, job);
    res.end();
  } catch (err) {
    if (!started) res.status(400).json({ ok: false, error: err.message });
    else res.destroy(err);
  } finally {
    grove.emit('progress', { ...job, finished: true });
  }
});

app.post('/api/capture', api((a) => sd.capture({
  width: evenInt(a.width ?? 640, 'Width', 160, 2592),
  height: evenInt(a.height ?? 480, 'Height', 120, 1936),   // the sensor's last 8 lines can't be captured reliably
})));

app.get('/api/captures', api(async () => {
  const files = await readdir(CAPTURE_DIR, { withFileTypes: true }).catch(() => []);
  const out = [];
  for (const f of files) {
    if (!f.isFile() || !/\.jpe?g$/i.test(f.name)) continue;
    const st = await stat(join(CAPTURE_DIR, f.name));
    out.push({ name: f.name, size: st.size, mtime: st.mtimeMs, url: `/captures/${encodeURIComponent(f.name)}` });
  }
  return out.sort((x, y) => y.mtime - x.mtime);
}));

const server = createServer(app);
const wss = new WebSocketServer({ server, path: '/ws' });

function broadcast(msg, { droppable = false } = {}) {
  const text = JSON.stringify(msg);
  for (const ws of wss.clients) {
    if (ws.readyState !== ws.OPEN) continue;
    if (droppable && ws.bufferedAmount > MAX_BUFFERED) continue;
    ws.send(text);
  }
}

wss.on('connection', (ws) => {
  ws.alive = true;
  ws.on('pong', () => { ws.alive = true; });
  ws.send(JSON.stringify({ type: 'status', status: grove.status() }));
  ws.send(JSON.stringify({ type: 'info', info: grove.info }));
});

// Drop browsers that stopped answering (closed laptop, dropped Wi-Fi), and tick the live ones.
setInterval(() => {
  for (const ws of wss.clients) {
    if (!ws.alive) {
      ws.terminate();
      continue;
    }
    ws.alive = false;
    ws.ping();
  }
  broadcast({ type: 'tick' });
}, TICK_MS);

grove.on('status', (status) => broadcast({ type: 'status', status }));
grove.on('info', (info) => broadcast({ type: 'info', info }));
grove.on('result', (data) => {
  broadcast({ type: 'result', data }, { droppable: true });
  if (data?.image) latestFrame = { image: data.image, resolution: data.resolution, track: data.track, at: Date.now() };
});
grove.on('line', (line) => {
  console.log(`[board] ${line}`);   // the board's own log (also in journalctl when run as a service)
  broadcast({ type: 'log', source: 'device', line });
});
grove.on('progress', (p) => broadcast({ type: 'progress', ...p }));
// Tracking results of frames that weren't sent (the camera outruns the link): pass them on.
grove.on('trackpos', (p) => broadcast({ type: 'trackpos', data: p }, { droppable: true }));
grove.on('track', (t) => {
  console.log(`[grove] Tracking ${t.state}${t.state !== 'off' ? ` at ${t.x},${t.y}` : ''}`);
  broadcast({ type: 'track', data: t });
});
grove.on('motion', (m) => {
  const what = m.error ? `failed: ${m.error}` : m.file ? `saved ${m.file}` : m.action === 'pulse' ? 'pulse sent' : 'no action';
  console.log(`[grove] Motion (${m.score} of ${m.cells} cells): ${what}`);
  broadcast({ type: 'motion', data: m });
});
grove.on('notice', (line) => {
  console.log(`[grove] ${line}`);
  broadcast({ type: 'log', source: 'server', line });
});

// ---- object suggestion before lock-on (lib/suggest.js) ------------------------------------
// While nobody is tracking and a browser is watching, guess the object in the lock-on box a few
// times a second and send the outline to the page. Lock on can then use the tight box.
const suggester = new Suggester({ log: (line) => console.log(`[grove] ${line}`) });
let latestFrame = null;
const suggestion = { data: null, at: 0, busy: false };
const SUGGEST_EVERY_MS = 300;

function smoothBox(prev, next) {
  if (!prev) return next;
  const [ax, ay, aw, ah] = prev, [bx, by, bw, bh] = next;
  const ix = Math.max(0, Math.min(ax + aw / 2, bx + bw / 2) - Math.max(ax - aw / 2, bx - bw / 2));
  const iy = Math.max(0, Math.min(ay + ah / 2, by + bh / 2) - Math.max(ay - ah / 2, by - bh / 2));
  const iou = (ix * iy) / (aw * ah + bw * bh - ix * iy);
  if (iou < 0.5) return next;                        // a different object: jump
  return prev.map((v, i) => v * 0.5 + next[i] * 0.5);  // the same one: steady the box
}

setInterval(() => {
  if (suggestion.busy || !wss.clients.size || !latestFrame || Date.now() - latestFrame.at > 1500) return;
  const t = latestFrame.track;
  if (t && t.state !== 'off') {                      // tracking or arming: no suggestions
    if (suggestion.data) { suggestion.data = null; broadcast({ type: 'suggest', data: null }); }
    return;
  }
  const [w, h] = latestFrame.resolution ?? [320, 240];
  const bw = Math.round(w / trackPrefs.div), bh = Math.round((bw * 3) / 4);
  const frame = latestFrame;
  suggestion.busy = true;
  suggester.suggest(frame.image, [w / 2, h / 2, bw, bh])
    .then((r) => {
      if (r.confident && suggestion.data?.confident && suggestion.data.frame?.[0] === r.frame?.[0]) {
        r.box = smoothBox(suggestion.data.box, r.box);
      }
      suggestion.data = { ...r, request: [w / 2, h / 2, bw, bh] };
      suggestion.at = Date.now();
      broadcast({ type: 'suggest', data: suggestion.data });
    })
    .catch(() => {})
    .finally(() => { suggestion.busy = false; });
}, SUGGEST_EVERY_MS);

server.listen(PORT, HOST, () => {
  const urls = [`http://localhost:${PORT}`];
  if (HOST === '0.0.0.0') {
    for (const addrs of Object.values(networkInterfaces())) {
      for (const a of addrs ?? []) {
        if (a.family === 'IPv4' && !a.internal) urls.push(`http://${a.address}:${PORT}`);
      }
    }
  } else if (HOST !== 'localhost' && HOST !== '127.0.0.1') {
    urls[0] = `http://${HOST}:${PORT}`;
  }
  console.log(`Grove Vision web UI:\n  ${urls.join('\n  ')}`);
  grove.start();
});

let stopping = false;
async function stop() {
  if (stopping) return;
  stopping = true;
  console.log('Stopping...');
  suggester.stop();
  await grove.shutdown().catch(() => {});
  process.exit(0);
}
process.on('SIGINT', stop);
process.on('SIGTERM', stop);
