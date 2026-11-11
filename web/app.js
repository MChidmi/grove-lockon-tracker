(() => {
// Lock-on tracker page: talks to the board straight from the browser over Web Serial (no server),
// shows the live view with a targeting HUD, and reports the target's centre for every frame.
// ?demo runs a simulated board, to try the page without hardware.
const { GroveLink, b64bytes, USB_FILTER } = globalThis.GroveLinkLib;

const $ = (s) => document.querySelector(s);
const canvas = $('#hud');
const ctx = canvas.getContext('2d');

// ---- preferences (per browser) ------------------------------------------------------------
const prefs = { box: 4, cam: 60, video: 30 };
try { Object.assign(prefs, JSON.parse(localStorage.getItem('lockon-prefs') || '{}')); } catch { /* defaults */ }
const savePrefs = () => { try { localStorage.setItem('lockon-prefs', JSON.stringify(prefs)); } catch { /* fine */ } };

// ---- state --------------------------------------------------------------------------------
const ui = {
  link: null,            // GroveLink or DemoBoard
  port: null, reader: null, writer: null,
  connecting: false,
  frameW: 320, frameH: 240,
  bitmap: null, decoding: false,
  track: { state: 'off' },
  lost: null,            // { x, y, w, h } while TARGET LOST shows
  posTimes: [], videoTimes: [],
};

const msg = (text, error = false) => { const m = $('#msg'); m.textContent = text || ''; m.style.color = error ? 'var(--red)' : 'var(--amber)'; };

function setLink(text, cls = '') {
  const l = $('#link');
  l.textContent = `● ${text}`;
  l.className = cls;
}

function renderButtons() {
  const on = !!ui.link;
  $('#b-connect').textContent = on ? 'Disconnect' : (ui.connecting ? 'Connecting…' : 'Connect');
  $('#b-connect').disabled = ui.connecting;
  const st = ui.track.state;
  $('#b-lock').disabled = !on || st === 'locked' || st === 'armed';
  $('#b-release').disabled = !on || (st === 'off' && !ui.lost);
  for (const b of document.querySelectorAll('#g-box button')) b.classList.toggle('on', Number(b.dataset.box) === prefs.box);
  for (const b of document.querySelectorAll('#g-cam button')) b.classList.toggle('on', Number(b.dataset.cam) === prefs.cam);
  for (const b of document.querySelectorAll('#g-video button')) b.classList.toggle('on', Number(b.dataset.video) === prefs.video);
}

function renderReadout() {
  const t = ui.track, st = ui.lost ? 'lost' : t.state;
  const s = $('#r-state');
  s.textContent = { off: 'STANDBY', armed: 'ACQUIRING', locked: 'LOCKED', lost: 'TARGET LOST' }[st] ?? st.toUpperCase();
  s.className = st;
  const p = ui.lost ?? (t.state === 'locked' ? t : null);
  $('#r-x').textContent = p ? String(Math.round(p.x)).padStart(3, '0') : '---';
  $('#r-y').textContent = p ? String(Math.round(p.y)).padStart(3, '0') : '---';
  $('#r-size').textContent = p ? `${Math.round(p.w)}×${Math.round(p.h)}` : '---';
  $('#r-psr').textContent = t.state === 'locked' && t.psr != null ? Number(t.psr).toFixed(1) : '---';
  const now = performance.now();
  ui.posTimes = ui.posTimes.filter((x) => now - x < 1000);
  ui.videoTimes = ui.videoTimes.filter((x) => now - x < 1000);
  $('#r-rate').textContent = ui.link ? String(ui.posTimes.length) : '--';
  $('#r-video').textContent = ui.link ? String(ui.videoTimes.length) : '--';
}

// ---- board messages ------------------------------------------------------------------------
function onTrackData(t) {
  if (!t) return;
  if (t.state === 'locked') ui.posTimes.push(performance.now());
  if (ui.lost && t.state === 'off') return;            // keep showing TARGET LOST
  ui.track = t;
}

function onFrame(d) {
  if (d.resolution) [ui.frameW, ui.frameH] = d.resolution;
  if (d.track) onTrackData(d.track);
  else if (ui.track.state === 'locked') ui.track = { state: 'off' };
  if (d.bitmap) {                                       // demo board
    ui.bitmap?.close?.();
    ui.bitmap = d.bitmap;
    ui.videoTimes.push(performance.now());
  } else if (d.image && !ui.decoding) {                 // a JPEG; drop it if one is still decoding
    ui.decoding = true;
    createImageBitmap(new Blob([b64bytes(d.image)], { type: 'image/jpeg' }))
      .then((bm) => { ui.bitmap?.close?.(); ui.bitmap = bm; ui.videoTimes.push(performance.now()); })
      .catch(() => {})
      .finally(() => { ui.decoding = false; });
  }
}

function onTrack(e) {
  const t = e.detail;
  if (t.state === 'lost') {
    // The board stops tracking when it loses the target (and says so once): alarm until the
    // next lock-on or Release.
    ui.lost = { x: t.x, y: t.y, w: t.w, h: t.h };
    ui.track = { state: 'off' };
  } else if (t.state === 'locked' || t.state === 'armed') {
    ui.lost = null;
    ui.track = t;
  } else if (t.state === 'off' && !ui.lost) {
    ui.track = { state: 'off' };
  }
  renderButtons();
}

function attach(link) {
  link.addEventListener('frame', (e) => onFrame(e.detail));
  link.addEventListener('trackpos', (e) => onTrackData(e.detail.track));
  link.addEventListener('track', onTrack);
}

// ---- connect -------------------------------------------------------------------------------
async function connect() {
  if (!('serial' in navigator)) {
    msg('This browser has no Web Serial. Use Chrome or Edge on a desktop computer, or try the demo (link below).', true);
    return;
  }
  let port;
  try {
    port = await navigator.serial.requestPort({ filters: [USB_FILTER] });
  } catch (err) {
    if (err.name === 'NotFoundError') { msg('No board chosen. Plug the Grove Vision AI V2 into this computer, then Connect.'); return; }
    throw err;
  }
  ui.connecting = true;
  renderButtons();
  try {
    await port.open({ baudRate: 921600, bufferSize: 1 << 20 });
  } catch (err) {
    ui.connecting = false;
    renderButtons();
    msg(`Could not open the port (${err.message}). Is another program using it (the web UI server, a serial monitor)?`, true);
    return;
  }
  ui.port = port;
  ui.writer = port.writable.getWriter();
  const link = new GroveLink((bytes) => ui.writer.write(bytes));
  attach(link);
  readLoop(port, link);
  setLink('CONNECTING');
  msg('The board restarts when the port opens…');
  await link.waitReady(8000);
  try {
    const ver = await link.command('VER?');
    if (ver.firmware !== 'grove-sd' || (ver.demo ?? 1) < 2) {
      throw new Error(`This board runs "${ver.software ?? ver.firmware ?? 'other firmware'}". Flash firmware/prebuilt/grove-lockon-tracker.img first.`);
    }
    $('#fw').textContent = (ver.software || '').toUpperCase();
    ui.link = link;
    await startStream();
    setLink('LINK', 'on');
    msg('Aim the box at the target, then LOCK (space).');
  } catch (err) {
    msg(err.message, true);
    await disconnect();
  } finally {
    ui.connecting = false;
    renderButtons();
  }
}

async function readLoop(port, link) {
  while (port.readable) {
    const reader = port.readable.getReader();
    ui.reader = reader;
    try {
      for (;;) {
        const { value, done } = await reader.read();
        if (done) break;
        if (value) link.feed(value);
      }
    } catch (err) {
      if (ui.port === port) msg(`Serial error: ${err.message}`, true);   // e.g. a buffer overrun: keep reading
    } finally {
      reader.releaseLock();
    }
    if (ui.port !== port) break;
  }
}

async function startStream() {
  const p = await ui.link.command(`PREVIEW=1,320,1,${prefs.cam}`);      // grayscale QVGA, fast camera mode
  ui.frameW = p.width ?? 320;
  ui.frameH = p.height ?? 240;
  await ui.link.command(`VIDEO=${prefs.video}`);
  await ui.link.command('INVOKE=-1,0,0', 5000);
}

async function disconnect() {
  const link = ui.link, port = ui.port;
  ui.link = null;
  ui.track = { state: 'off' };
  ui.lost = null;
  if (link) {
    try { await link.command('TRACK=0', 800); } catch { /* fine */ }
    try { await link.command('BREAK', 800); } catch { /* fine */ }
    link.stop?.();
  }
  ui.port = null;
  if (port) {
    try { await ui.reader?.cancel(); } catch { /* fine */ }
    try { ui.writer?.releaseLock(); } catch { /* fine */ }
    try { await port.close(); } catch { /* fine */ }
  }
  ui.writer = ui.reader = null;
  ui.bitmap = null;
  $('#fw').textContent = '';
  setLink('OFFLINE');
  renderButtons();
}

// ---- actions ------------------------------------------------------------------------------
async function lock() {
  if (!ui.link) return;
  const bw = Math.round(ui.frameW / prefs.box), bh = Math.round((bw * 3) / 4);
  ui.lost = null;
  try {
    await ui.link.command(`TRACK=1,${bw},${bh}`);
    ui.track = { state: 'armed', x: ui.frameW / 2, y: ui.frameH / 2, w: bw, h: bh };
  } catch (err) { msg(err.message, true); }
  renderButtons();
}

async function release() {
  ui.lost = null;
  if (ui.link) {
    try { await ui.link.command('TRACK=0'); } catch (err) { msg(err.message, true); }
  }
  ui.track = { state: 'off' };
  renderButtons();
}

async function setCam(fps) {
  prefs.cam = fps; savePrefs(); renderButtons();
  if (ui.link) try { await ui.link.command(`PREVIEW=1,320,1,${fps}`); } catch (err) { msg(err.message, true); }
}

async function setVideo(v) {
  prefs.video = v; savePrefs(); renderButtons();
  if (!v) ui.bitmap = null;
  if (ui.link) try { await ui.link.command(`VIDEO=${v}`); } catch (err) { msg(err.message, true); }
}

$('#b-connect').addEventListener('click', () => (ui.link ? disconnect() : connect()).catch((e) => msg(e.message, true)));
$('#b-lock').addEventListener('click', lock);
$('#b-release').addEventListener('click', release);
for (const b of document.querySelectorAll('#g-box button')) b.addEventListener('click', () => { prefs.box = Number(b.dataset.box); savePrefs(); renderButtons(); });
for (const b of document.querySelectorAll('#g-cam button')) b.addEventListener('click', () => setCam(Number(b.dataset.cam)));
for (const b of document.querySelectorAll('#g-video button')) b.addEventListener('click', () => setVideo(Number(b.dataset.video)));
document.addEventListener('keydown', (e) => {
  if (e.target.closest?.('input, textarea')) return;
  if (e.code === 'Space') { e.preventDefault(); if (ui.track.state === 'locked' || ui.track.state === 'armed') release(); else lock(); }
  if (e.code === 'Escape') release();
});
navigator.serial?.addEventListener?.('disconnect', (e) => {
  if (ui.port && e.target === ui.port) { msg('The board was unplugged.', true); disconnect(); }
});

// ---- HUD ------------------------------------------------------------------------------------
const GREEN = '61,255,138', AMBER = '255,176,0', RED = '255,43,43', WHITE = '220,240,230';

function brackets(x, y, w, h, color, lw, len) {
  const l = Math.min(len, w / 2.5, h / 2.5);
  ctx.strokeStyle = color;
  ctx.lineWidth = lw;
  ctx.beginPath();
  for (const [cx, cy, dx, dy] of [[x, y, 1, 1], [x + w, y, -1, 1], [x, y + h, 1, -1], [x + w, y + h, -1, -1]]) {
    ctx.moveTo(cx + dx * l, cy); ctx.lineTo(cx, cy); ctx.lineTo(cx, cy + dy * l);
  }
  ctx.stroke();
}

function label(text, x, y, color, size, align = 'left') {
  ctx.font = `600 ${size}px ui-monospace, Menlo, Consolas, monospace`;
  ctx.textAlign = align;
  ctx.textBaseline = 'top';
  ctx.fillStyle = color;
  ctx.fillText(text, x, y);
}

function draw() {
  requestAnimationFrame(draw);
  const dpr = window.devicePixelRatio || 1;
  const cw = Math.max(1, Math.round(canvas.clientWidth * dpr)), ch = Math.max(1, Math.round(canvas.clientHeight * dpr));
  if (canvas.width !== cw || canvas.height !== ch) { canvas.width = cw; canvas.height = ch; }
  const sx = cw / ui.frameW, sy = ch / ui.frameH, u = Math.max(1, cw / 640);   // u: HUD unit
  const now = performance.now();

  // picture (or a dark grid with no video)
  ctx.fillStyle = '#020403';
  ctx.fillRect(0, 0, cw, ch);
  if (ui.bitmap && prefs.video) {
    ctx.filter = 'contrast(1.1) brightness(0.95)';
    ctx.drawImage(ui.bitmap, 0, 0, cw, ch);
    ctx.filter = 'none';
  } else {
    ctx.strokeStyle = `rgba(${GREEN},0.07)`;
    ctx.lineWidth = 1;
    ctx.beginPath();
    for (let i = 1; i < 8; i++) { ctx.moveTo((cw * i) / 8, 0); ctx.lineTo((cw * i) / 8, ch); }
    for (let i = 1; i < 6; i++) { ctx.moveTo(0, (ch * i) / 6); ctx.lineTo(cw, (ch * i) / 6); }
    ctx.stroke();
    if (ui.link && !prefs.video) label('VIDEO OFF · TRACKING DATA ONLY', cw / 2, ch - 30 * u, `rgba(${GREEN},0.45)`, 11 * u, 'center');
  }
  // scanlines
  ctx.fillStyle = 'rgba(0,0,0,0.12)';
  for (let y = 0; y < ch; y += 3 * u) ctx.fillRect(0, y, cw, u);

  // fixed reticle: centre cross and edge ticks
  const mx = cw / 2, my = ch / 2;
  ctx.strokeStyle = `rgba(${WHITE},0.35)`;
  ctx.lineWidth = 1.2 * u;
  ctx.beginPath();
  ctx.moveTo(mx - 18 * u, my); ctx.lineTo(mx - 6 * u, my); ctx.moveTo(mx + 6 * u, my); ctx.lineTo(mx + 18 * u, my);
  ctx.moveTo(mx, my - 18 * u); ctx.lineTo(mx, my - 6 * u); ctx.moveTo(mx, my + 6 * u); ctx.lineTo(mx, my + 18 * u);
  for (let i = 1; i < 10; i++) {
    const t = (i % 5 === 0 ? 12 : 6) * u;
    ctx.moveTo((cw * i) / 10, 0); ctx.lineTo((cw * i) / 10, t);
    ctx.moveTo((cw * i) / 10, ch); ctx.lineTo((cw * i) / 10, ch - t);
    ctx.moveTo(0, (ch * i) / 10); ctx.lineTo(t, (ch * i) / 10);
    ctx.moveTo(cw, (ch * i) / 10); ctx.lineTo(cw - t, (ch * i) / 10);
  }
  ctx.stroke();

  const t = ui.track;
  const pad = 14 * u, fs = 12 * u;
  if (ui.lost) {
    // TARGET LOST: last position in red, and the flashing alarm
    const L = ui.lost;
    ctx.setLineDash([5 * u, 5 * u]);
    brackets((L.x - L.w / 2) * sx, (L.y - L.h / 2) * sy, L.w * sx, L.h * sy, `rgba(${RED},0.8)`, 2 * u, 16 * u);
    ctx.setLineDash([]);
    if (Math.floor(now / 350) % 2 === 0) {
      ctx.save();
      ctx.shadowColor = `rgba(${RED},0.9)`;
      ctx.shadowBlur = 18 * u;
      const big = 44 * u;
      ctx.strokeStyle = `rgb(${RED})`;
      ctx.lineWidth = 2.5 * u;
      ctx.strokeRect(mx - 190 * u, my - 46 * u, 380 * u, 92 * u);
      label('TARGET LOST', mx, my - 30 * u, `rgb(${RED})`, big, 'center');
      ctx.restore();
    }
    label(`LAST  X ${Math.round(L.x)}  Y ${Math.round(L.y)}`, mx, my + 56 * u, `rgba(${RED},0.9)`, fs, 'center');
    label('SPACE TO RE-LOCK', mx, my + 74 * u, `rgba(${WHITE},0.6)`, fs * 0.9, 'center');
  } else if (t.state === 'locked') {
    const x = (t.x - t.w / 2) * sx, y = (t.y - t.h / 2) * sy, w = t.w * sx, h = t.h * sy;
    const cx = t.x * sx, cy = t.y * sy;
    // tracking lines to the edges
    ctx.strokeStyle = `rgba(${GREEN},0.35)`;
    ctx.lineWidth = 1 * u;
    ctx.setLineDash([4 * u, 6 * u]);
    ctx.beginPath();
    ctx.moveTo(0, cy); ctx.lineTo(x, cy); ctx.moveTo(x + w, cy); ctx.lineTo(cw, cy);
    ctx.moveTo(cx, 0); ctx.lineTo(cx, y); ctx.moveTo(cx, y + h); ctx.lineTo(cx, ch);
    ctx.stroke();
    ctx.setLineDash([]);
    brackets(x, y, w, h, `rgb(${GREEN})`, 2.5 * u, 18 * u);
    ctx.strokeStyle = `rgb(${GREEN})`;
    ctx.lineWidth = 2 * u;
    ctx.beginPath();
    ctx.moveTo(cx, cy - 7 * u); ctx.lineTo(cx + 7 * u, cy); ctx.lineTo(cx, cy + 7 * u); ctx.lineTo(cx - 7 * u, cy); ctx.closePath();
    ctx.stroke();
    label(`TGT ${Math.round(t.x)},${Math.round(t.y)}`, x, Math.max(2 * u, y - 18 * u), `rgb(${GREEN})`, fs);
    label('LOCKED', pad, pad, `rgb(${GREEN})`, 16 * u);
    // confidence bar
    const psr = Math.max(0, Math.min(1, ((t.psr ?? 0) - 4) / 20));
    ctx.strokeStyle = `rgba(${GREEN},0.6)`;
    ctx.strokeRect(pad, pad + 24 * u, 120 * u, 6 * u);
    ctx.fillStyle = psr > 0.25 ? `rgb(${GREEN})` : `rgb(${AMBER})`;
    ctx.fillRect(pad, pad + 24 * u, 120 * u * psr, 6 * u);
  } else {
    // standby / acquiring: the lock-on box
    const bw = t.state === 'armed' ? t.w : Math.round(ui.frameW / prefs.box);
    const bh = t.state === 'armed' ? t.h : Math.round((bw * 3) / 4);
    const armed = t.state === 'armed';
    const show = !armed || Math.floor(now / 120) % 2 === 0;
    if (show) brackets(mx - (bw * sx) / 2, my - (bh * sy) / 2, bw * sx, bh * sy, armed ? `rgb(${AMBER})` : `rgba(${WHITE},0.75)`, 2 * u, 14 * u);
    label(armed ? 'ACQUIRING' : (ui.link ? 'STANDBY' : 'NO LINK'), pad, pad, armed ? `rgb(${AMBER})` : `rgba(${WHITE},0.7)`, 16 * u);
    if (ui.link && !armed) label('SPACE TO LOCK', mx, my + (bh * sy) / 2 + 10 * u, `rgba(${WHITE},0.55)`, fs * 0.9, 'center');
  }
  label(`${ui.frameW}×${ui.frameH}  CAM ${prefs.cam >= 70 ? 'MAX' : prefs.cam}`, cw - pad, pad, `rgba(${WHITE},0.5)`, fs * 0.9, 'right');
}

// ---- demo board (?demo): a target that wanders, and now and then disappears ---------------
class DemoBoard extends EventTarget {
  constructor() {
    super();
    this.n = 0;
    this.t0 = performance.now();
    this.track = null;
    this.video = prefs.video;
    this.bad = 0;
    this.cv = typeof OffscreenCanvas !== 'undefined' ? new OffscreenCanvas(320, 240) : Object.assign(document.createElement('canvas'), { width: 320, height: 240 });
    this.g = this.cv.getContext('2d');
    this.bg = this.makeBackground();
    this.timer = setInterval(() => this.tick(), 1000 / 60);
  }

  makeBackground() {
    const c = typeof OffscreenCanvas !== 'undefined' ? new OffscreenCanvas(320, 240) : Object.assign(document.createElement('canvas'), { width: 320, height: 240 });
    const g = c.getContext('2d');
    const grd = g.createLinearGradient(0, 0, 0, 240);
    grd.addColorStop(0, '#6d7a7c'); grd.addColorStop(0.55, '#4b5650'); grd.addColorStop(1, '#2c332d');
    g.fillStyle = grd; g.fillRect(0, 0, 320, 240);
    let seed = 7;
    const rnd = () => ((seed = (seed * 16807) % 2147483647) / 2147483647);
    for (let i = 0; i < 90; i++) {
      const v = 40 + Math.floor(rnd() * 90);
      g.fillStyle = `rgb(${v},${v + 5},${v})`;
      g.fillRect(rnd() * 320, 120 + rnd() * 120, 4 + rnd() * 30, 2 + rnd() * 10);
    }
    return c;
  }

  // the target: a Lissajous path; hidden for 2 s of every 16 s (to show TARGET LOST)
  target(ms) {
    const s = ms / 1000;
    const hidden = s % 16 > 12 && s % 16 < 14;
    return { x: 160 + 95 * Math.sin(s * 0.7), y: 118 + 55 * Math.sin(s * 1.13 + 0.6), r: 15 + 4 * Math.sin(s * 0.5), a: s * 1.4, hidden };
  }

  tick() {
    const now = performance.now() - this.t0;
    const T = this.target(now);
    this.n++;
    if (this.track?.state === 'locked') {
      const near = !T.hidden && Math.hypot(T.x - this.track.x, T.y - this.track.y) < this.track.w;
      if (near) {
        this.bad = 0;
        this.track.x += (T.x - this.track.x) * 0.85;
        this.track.y += (T.y - this.track.y) * 0.85;
        this.track.w += (T.r * 2.6 - this.track.w) * 0.1;
        this.track.h = this.track.w * 0.75;
        this.track.psr = 30 + 10 * Math.sin(now / 300);
      } else if (++this.bad >= 8) {
        this.emit('track', { state: 'lost', x: Math.round(this.track.x), y: Math.round(this.track.y), w: Math.round(this.track.w), h: Math.round(this.track.h) });
        this.track = null;
      }
    } else if (this.track?.state === 'armed') {
      this.track.state = 'locked';
      this.bad = 0;
      this.emit('track', { ...this.track });
    }
    const tr = this.track ? { ...this.track, x: Math.round(this.track.x), y: Math.round(this.track.y), w: Math.round(this.track.w), h: Math.round(this.track.h), ms: 14 } : undefined;
    const videoDue = this.video > 0 && this.n % Math.max(1, Math.round(60 / this.video)) === 0;
    if (videoDue) {
      const g = this.g;
      g.drawImage(this.bg, 0, 0);
      if (!T.hidden) {
        g.save(); g.translate(T.x, T.y); g.rotate(T.a);
        g.fillStyle = '#d8dcd4'; g.fillRect(-T.r, -T.r * 0.55, T.r * 2, T.r * 1.1);
        g.fillStyle = '#26302a'; g.fillRect(-T.r * 0.3, -T.r * 0.9, T.r * 0.6, T.r * 1.8);
        g.restore();
      }
      createImageBitmap(this.cv).then((bitmap) => this.emit('frame', { n: this.n, resolution: [320, 240], track: tr, bitmap }));
    } else if (tr) {
      this.emit('trackpos', { n: this.n, track: tr });
    }
  }

  emit(type, detail) { this.dispatchEvent(new CustomEvent(type, { detail })); }

  async command(cmd) {
    const [name, args = ''] = cmd.split('=');
    const a = args.split(',').map(Number);
    if (name === 'TRACK') {
      if (a[0] === 1) this.track = { state: 'armed', x: 160, y: 120, w: a[1] || 80, h: a[2] || 60 };
      else this.track = null;
      return { state: this.track ? 'armed' : 'off' };
    }
    if (name === 'VIDEO') { this.video = a[0]; return { video_fps: this.video }; }
    if (name === 'PREVIEW') return { gray: 1, width: 320, height: 240, fast: 1, fps: a[3] || 60 };
    return {};
  }

  stop() { clearInterval(this.timer); }
}

if (new URLSearchParams(location.search).has('demo')) {
  const demo = new DemoBoard();
  attach(demo);
  ui.link = demo;
  setLink('DEMO', 'demo');
  $('#fw').textContent = 'SIMULATED BOARD';
  msg('Demo: a simulated board. The target wanders, and hides now and then. Wait until it passes the centre, then LOCK (space).');
  $('#b-connect').addEventListener('click', () => { location.search = ''; }, { capture: true, once: true });
}

renderButtons();
setInterval(renderReadout, 100);
requestAnimationFrame(draw);
})();
