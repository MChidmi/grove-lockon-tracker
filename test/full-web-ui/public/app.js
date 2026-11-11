// Grove Vision AI V2 web UI: the live feed and device state arrive over a
// WebSocket, and settings changes go to the REST API.
const $ = (sel) => document.querySelector(sel);

const PALETTE = ['#4f8cff', '#ff5a5f', '#22c55e', '#f5a524', '#a970ff', '#14b8c4', '#f062a8', '#8bc34a'];
const ALGO_NAMES = { 1: 'FOMO', 2: 'PFLD', 3: 'YOLO', 4: 'Image classification', 5: 'YOLO pose' };
const CATEGORY_NAMES = { 1: 'Object detection', 2: 'Keypoints', 3: 'Classification' };
const SENSOR_TYPES = { 1: 'Camera' };
// Frames normally arrive every 70-160 ms; flag the picture as frozen after this long.
const FROZEN_AFTER_S = 2;
// The server ticks every 5 s; after this much silence, assume the connection is dead and reconnect.
const LINK_TIMEOUT_MS = 12000;
// The rule SenseCraft writes, and the one the LED builder produces.
const ACTION_RE = /^\(\(max_score\(target,(\d+)\)>=(\d+)\)&&led\(1\)\)\|\|led\(0\)$/;

const store = {
  get(key, fallback) {
    try {
      const v = localStorage.getItem(`grove.${key}`);
      return v === null ? fallback : JSON.parse(v);
    } catch {
      return fallback;
    }
  },
  set(key, value) {
    try { localStorage.setItem(`grove.${key}`, JSON.stringify(value)); } catch { /* private mode */ }
  },
};

const ui = {
  link: false,
  status: null,
  info: {},
  frame: null,     // last decoded camera image
  pending: null,   // image being decoded (newest wins)
  last: null,      // last result drawn
  times: [],       // arrival times for the frame-rate readout
  lastFrameAt: 0,  // Date.now() of the last result
  lastMsgAt: 0,    // Date.now() of the last WebSocket message
  ws: null,
  minScore: store.get('minScore', 0),
  labels: store.get('labels', true),
  dirty: new Set(), // settings forms the user is editing; incoming state won't overwrite them
  history: [],
  historyPos: 0,
};

const canvas = $('#view');
const ctx = canvas.getContext('2d');

// ---- helpers ---------------------------------------------------------------

const classes = () => ui.info?.modelInfo?.classes ?? [];
const className = (t) => classes()[t] ?? `Class ${t}`;
const color = (t) => PALETTE[(Number(t) || 0) % PALETTE.length];

function el(tag, props = {}, ...children) {
  const node = Object.assign(document.createElement(tag), props);
  node.append(...children);
  return node;
}

function fillDl(dl, rows) {
  dl.replaceChildren(...rows
    .filter(([, v]) => v !== undefined)
    .flatMap(([k, v]) => [el('dt', { textContent: k }), el('dd', { textContent: v ?? '–' })]));
}

let toastTimer;
function toast(message, error = false) {
  const t = $('#toast');
  t.textContent = message;
  t.className = error ? 'toast error' : 'toast';
  t.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { t.hidden = true; }, error ? 7000 : 3000);
}

async function api(path, body = {}, method = 'POST') {
  const res = await fetch(`/api/${path}`, method === 'GET' || method === 'DELETE' ? { method } : {
    method,
    headers: { 'content-type': 'application/json' },
    body: JSON.stringify(body),
  });
  const json = await res.json().catch(() => ({ ok: false, error: `Server error (HTTP ${res.status})` }));
  if (!json.ok) throw new Error(json.error);
  return json.result;
}

const apiGet = (path) => api(path, undefined, 'GET');

async function run(button, fn, done) {
  button.disabled = true;
  try {
    const result = await fn();
    if (done) toast(done);
    return result;
  } catch (err) {
    toast(err.message, true);
    return undefined;
  } finally {
    button.disabled = false;
  }
}

// ---- tabs --------------------------------------------------------------------

function showTab(name) {
  ui.tab = name;
  for (const b of document.querySelectorAll('[data-tab]')) b.setAttribute('aria-selected', String(b.dataset.tab === name));
  for (const p of document.querySelectorAll('.tab-panel')) p.hidden = p.id !== `tab-${name}`;
  if (name === 'files') openFiles();
}
for (const b of document.querySelectorAll('[data-tab]')) {
  b.addEventListener('click', () => {
    history.replaceState(null, '', b.dataset.tab === 'preview' ? location.pathname : `#${b.dataset.tab}`);
    showTab(b.dataset.tab);
  });
}

// ---- status -----------------------------------------------------------------

function fps() {
  const t = ui.times;
  return t.length > 1 ? ((t.length - 1) * 1000) / (t.at(-1) - t[0]) : 0;
}

// Seconds since the last frame while frames are expected, or 0.
function frozenFor() {
  const s = ui.status;
  if (!ui.link || s?.state !== 'ready' || !s.stream.want || !ui.lastFrameAt) return 0;
  if (s.firmware?.streams === false) return 0;
  const gap = (Date.now() - ui.lastFrameAt) / 1000;
  return gap >= FROZEN_AFTER_S ? Math.floor(gap) : 0;
}

function renderStatus() {
  const s = ui.status;
  const wd = s?.watchdog;
  const frozen = frozenFor();
  let tone = 'bad';
  let label = 'Server offline';
  if (ui.link && s) {
    if (s.state === 'ready') {
      if (s.firmware?.streams === false) { tone = 'good'; label = 'Connected · SD demo 1'; }
      else if (wd?.stalled) { tone = 'warn'; label = 'Recovering…'; }
      else if (frozen) { tone = 'warn'; label = `No frames for ${frozen} s`; }
      else if (s.stream.running) { tone = 'good'; label = fps() ? `Live · ${fps().toFixed(1)} fps` : 'Live'; }
      else if (s.stream.want) { tone = 'warn'; label = 'Not streaming'; }
      else { tone = 'idle'; label = 'Paused'; }
    } else if (s.state === 'connecting') { tone = 'warn'; label = wd?.stalled ? 'Recovering…' : 'Connecting…'; }
    else if (s.state === 'released') { tone = 'idle'; label = 'Port released'; }
    else { label = 'Board disconnected'; }
  }
  const pill = $('#status');
  pill.dataset.tone = tone;
  $('#status-text').textContent = label;

  let banner = '';
  if (!ui.link) banner = 'Lost contact with the web server. Retrying…';
  else if (s?.state === 'released') banner = 'The serial port is released. Press Reconnect under Settings → Connection when the other program is done.';
  else if (s?.error && s.state !== 'ready') banner = s.error;
  $('#banner').textContent = banner;
  $('#banner').hidden = !banner;

  const stream = s?.stream;
  const streams = s?.firmware?.streams !== false;
  const btn = $('#btn-stream');
  btn.textContent = stream?.want === false ? 'Resume' : 'Pause';
  btn.disabled = !ui.link || s?.state !== 'ready' || !streams;
  $('#opt-image').checked = stream?.image ?? true;
  $('#opt-gray').checked = stream?.gray ?? true;
  renderMotion(s?.motion);
  $('#opt-live-size').value = String(stream?.size ?? 320);
  $('#opt-cam-fps').value = String(stream?.fps ?? 60);
  $('#opt-video').value = String(stream?.video ?? 30);
  if (ui.flavor === 'grove-sd') $('#live-model').textContent = liveLine(ui.info, {});

  let stage = '';
  if (ui.link && s?.state === 'ready' && !streams) {
    stage = 'This firmware (SD demo 1) has no camera.\nThe SD card is on the Files tab. Flash demo 2 for the preview and capture.';
  } else if (wd?.stalled && wd.last && s?.state !== 'released') {
    const gap = ui.lastFrameAt ? ` for ${Math.floor((Date.now() - ui.lastFrameAt) / 1000)} s` : '';
    stage = `No frames from the board${gap}.\n${wd.last.label}… (step ${wd.level})`;
  } else if (!ui.link || s?.state !== 'ready') stage = label;
  else if (frozen) stage = `No new frames for ${frozen} s. The watchdog steps in after ${Math.round(wd?.stallMs / 1000) || 4} s.`;
  else if (!stream.running) stage = stream.want ? 'Inference is not running. The watchdog will retry.' : 'Paused';
  else if (!ui.last) stage = 'Waiting for the first frame…';
  $('#stage-msg').textContent = stage;
  $('#stage-msg').hidden = !stage;

  const last = wd?.last;
  fillDl($('#conn-dl'), [
    ['Port', s?.path],
    ['State', s?.state],
    ['Last error', s?.error ?? 'none'],
    ['Watchdog', wd ? (wd.stalled ? 'recovering' : `watching (acts after ${Math.round(wd.stallMs / 1000)} s without frames)`) : null],
    ['Freezes fixed', wd ? `${wd.fixes} of ${wd.attempts} attempts` : null],
    ['Last action', last
      ? `${new Date(last.at).toLocaleTimeString()} · ${last.label} · ${last.ok === true ? 'fixed it' : last.ok === false ? 'failed' : 'in progress'}`
      : 'none yet'],
  ]);
  $('#btn-release').disabled = s?.state === 'released';
  $('#btn-connect').disabled = s?.state === 'ready' || s?.state === 'connecting';
}

// ---- live preview -------------------------------------------------------------

// ---- object tracking (grove-sd firmware) ----------------------------------------------------

function renderTrack(t) {
  const state = t?.state ?? 'off';
  ui.trackShown = state !== 'off';
  const badge = $('#track-state');
  badge.textContent = state === 'off' ? '' : state;
  badge.className = `badge ${state === 'locked' ? 'ok' : state === 'lost' ? 'bad' : ''}`;
  $('#btn-track').hidden = state !== 'off';
  $('#btn-track-stop').hidden = state === 'off';
  fillDl($('#track-dl'), state === 'off' ? [] : [
    ['Centre', state === 'armed' ? 'waiting for the next frame' : `x ${t.x}, y ${t.y}`],
    ['Size', `${t.w} × ${t.h}`],
    ['Confidence', t.psr != null ? `${t.psr} (PSR; above ~6 is a lock)` : undefined],
    ['Tracker time', t.ms != null ? `${t.ms} ms per frame` : undefined],
  ]);
}

$('#btn-track').addEventListener('click', (e) => run(e.currentTarget, async () => {
  const div = Number($('#track-size').value) || 4;
  const w = Math.round((ui.last?.resolution?.[0] ?? canvas.width) / div);
  renderTrack(await api('track', { on: true, w, h: Math.round((w * 3) / 4), snap: $('#track-snap').checked }));
}));
$('#track-size').addEventListener('change', (e) => run(e.currentTarget, () => api('track/prefs', { div: Number(e.currentTarget.value) })));
$('#track-snap').addEventListener('change', (e) => run(e.currentTarget, () => api('track/prefs', { snap: e.currentTarget.checked })));
apiGet('track/prefs').then((p) => {
  $('#track-size').value = String(p.div);
  $('#track-snap').checked = p.snap;
}).catch(() => {});

function renderSuggest(sg) {
  const line = $('#track-suggest');
  if (!sg) { line.textContent = '–'; return; }
  if (!sg.confident) { line.textContent = `No clear object in the box: ${sg.reason ?? 'unsure'}`; return; }
  const [x, y, w, h] = sg.box.map((v) => Math.round(v));
  line.textContent = `${sg.level === 'likely' ? 'Likely' : 'Maybe'} an object of ${w} × ${h} at ${x}, ${y} (score ${sg.score})`;
}
$('#btn-track-stop').addEventListener('click', (e) => run(e.currentTarget, async () => { renderTrack(await api('track', { on: false })); }));
$('#track-pin').addEventListener('change', (e) => run(e.currentTarget, () => api('track/button', { pin: e.currentTarget.value || null }),
  e.currentTarget.value ? `Button on ${e.currentTarget.value}` : 'Button off'));

// ---- motion detection (grove-sd firmware) -------------------------------------------------

function renderMotion(m) {
  if (!m) return;
  ui.motion = m;
  const set = (sel, prop, value) => { const n = $(sel); if (document.activeElement !== n) n[prop] = value; };
  set('#motion-on', 'checked', m.on);
  set('#motion-action', 'value', m.action);
  set('#motion-pin', 'value', m.pin);
  set('#motion-ms', 'value', String(m.pulseMs));
  set('#motion-sens', 'value', String(m.sensitivity));
  $('#motion-sens-out').textContent = $('#motion-sens').value;
  $('#motion-pulse-opts').hidden = $('#motion-action').value !== 'pulse';
  $('#motion-badge').textContent = m.events ? String(m.events) : '';
  if (!m.on) $('#motion-live').textContent = 'Off';
  else if (m.last) $('#motion-live').textContent = motionText(m.last);
}

function motionText(e) {
  const when = new Date(e.at).toLocaleTimeString();
  const what = e.error ? `failed: ${e.error}` : e.file ? `saved ${e.file}` : e.action === 'pulse' ? 'pulse sent' : 'seen';
  return `Last motion ${when}: ${what}`;
}

function sendMotion(changes) {
  return api('motion', changes).then(renderMotion);
}

for (const [sel, key, read] of [
  ['#motion-on', 'on', (n) => n.checked],
  ['#motion-action', 'action', (n) => n.value],
  ['#motion-pin', 'pin', (n) => n.value],
  ['#motion-ms', 'pulseMs', (n) => Number(n.value)],
  ['#motion-sens', 'sensitivity', (n) => Number(n.value)],
]) {
  $(sel).addEventListener('change', (e) => run(e.currentTarget, () => sendMotion({ [key]: read(e.currentTarget) })));
}
$('#motion-sens').addEventListener('input', (e) => { $('#motion-sens-out').textContent = e.currentTarget.value; });
$('#motion-action').addEventListener('input', (e) => { $('#motion-pulse-opts').hidden = e.currentTarget.value !== 'pulse'; });
$('#btn-motion-test').addEventListener('click', (e) => run(e.currentTarget, () => api('motion/test'),
  ui.motion?.action === 'save' ? 'Test photo taken' : ui.motion?.action === 'pulse' ? 'Pulse sent' : 'Test pulse sent'));

function onResult(d) {
  ui.lastFrameAt = Date.now();
  countProcessed(d.n);
  if (d.track || ui.trackShown) renderTrack(d.track);
  if (typeof d.motion === 'number' && d.motion >= 0 && ui.motion?.on) {
    const pct = Math.round((d.motion / 300) * 100);
    const last = ui.motion.last ? ` · ${motionText(ui.motion.last)}` : '';
    $('#motion-live').textContent = `Movement: ${pct}% of the view${last}`;
  }
  const now = performance.now();
  ui.times.push(now);
  while (ui.times.length > 2 && now - ui.times[0] > 3000) ui.times.shift();

  if (d.image) {
    const img = new Image();
    ui.pending = img;
    img.onload = () => {
      if (ui.pending !== img) return;
      ui.pending = null;
      ui.frame = img;
      ui.last = d;
      draw();
      renderStatus();
    };
    img.src = `data:image/jpeg;base64,${d.image}`;
  } else {
    ui.last = d;
    draw();
    renderStatus();
  }
  renderResults(d);
  const [pre, infer] = d.perf ?? [];
  $('#stat-fps').textContent = fps() ? fps().toFixed(1) : '–';
  $('#stat-infer').textContent = infer ?? '–';
  $('#stat-pre').textContent = pre ?? '–';
}

function label(x, y, t, text, scale, above) {
  const pad = 4 * scale;
  const height = Math.round(15 * scale) + pad * 2;
  const width = ctx.measureText(text).width + pad * 2;
  let top = above ? y - height : y;
  if (top < 0) top = y;
  const left = Math.max(0, Math.min(x, canvas.width - width));
  ctx.fillStyle = color(t);
  ctx.fillRect(left, top, width, height);
  ctx.fillStyle = '#fff';
  ctx.fillText(text, left + pad, top + pad + 1);
}

function box(cx, cy, w, h, score, t, scale) {
  // SSCMA boxes are centre x/y plus width/height, in frame pixels.
  const x = cx - w / 2;
  const y = cy - h / 2;
  ctx.strokeStyle = color(t);
  ctx.strokeRect(x, y, w, h);
  if (ui.labels) label(x - ctx.lineWidth / 2, y - ctx.lineWidth / 2, t, `${className(t)} ${score}%`, scale, true);
}

// The tracker's overlay. Off: a dashed centre box showing what a lock-on would take. Armed or
// tracking: the box and a crosshair on the centre, with the coordinates.
function drawTrack(d, w, h, scale) {
  const t = d.track;
  const state = t?.state ?? 'off';
  ctx.save();
  ctx.lineWidth = 2 * scale;
  if (state === 'off' && ui.targetLost && Math.floor(Date.now() / 400) % 2 === 0) {
    ctx.save();
    ctx.font = `bold ${Math.round(28 * scale)}px monospace`;
    ctx.textAlign = 'center';
    ctx.fillStyle = '#ff2b2b';
    ctx.fillText('TARGET LOST', w / 2, h * 0.18);
    ctx.restore();
  }
  if (state === 'off' || state === 'armed') {
    const div = Number($('#track-size').value) || 4;
    const bw = state === 'armed' ? t.w : Math.round(w / div), bh = state === 'armed' ? t.h : Math.round((bw * 3) / 4);
    const bx = state === 'armed' ? t.x : w / 2, by = state === 'armed' ? t.y : h / 2;
    ctx.setLineDash([6 * scale, 4 * scale]);
    ctx.strokeStyle = state === 'armed' ? '#ffd166' : 'rgba(255,255,255,0.7)';
    ctx.strokeRect(bx - bw / 2, by - bh / 2, bw, bh);
    const sg = ui.suggest;
    if (state === 'off' && sg?.confident && sg.frame?.[0] === w && Date.now() - sg.receivedAt < 2000) {
      // the guessed object: filled outline and its tight box
      const col = sg.level === 'likely' ? '61,220,132' : '255,209,102';
      ctx.setLineDash([]);
      if (sg.contour?.length > 2) {
        ctx.beginPath();
        sg.contour.forEach(([x, y], i) => (i ? ctx.lineTo(x, y) : ctx.moveTo(x, y)));
        ctx.closePath();
        ctx.fillStyle = `rgba(${col},0.25)`;
        ctx.fill();
        ctx.lineWidth = 1.5 * scale;
        ctx.strokeStyle = `rgba(${col},0.9)`;
        ctx.stroke();
      }
      const [cx, cy, sw, sh] = sg.box;
      ctx.lineWidth = 2 * scale;
      ctx.strokeStyle = `rgb(${col})`;
      ctx.strokeRect(cx - sw / 2, cy - sh / 2, sw, sh);
    }
  } else {
    ctx.setLineDash([]);
    ctx.strokeStyle = state === 'locked' ? '#3ddc84' : '#ff5c5c';
    ctx.strokeRect(t.x - t.w / 2, t.y - t.h / 2, t.w, t.h);
    ctx.beginPath();
    ctx.moveTo(t.x - 8 * scale, t.y); ctx.lineTo(t.x + 8 * scale, t.y);
    ctx.moveTo(t.x, t.y - 8 * scale); ctx.lineTo(t.x, t.y + 8 * scale);
    ctx.stroke();
    if (ui.labels) label(t.x - t.w / 2 - ctx.lineWidth / 2, t.y - t.h / 2 - ctx.lineWidth / 2, 0,
      `${state === 'locked' ? '' : 'lost · '}${t.x}, ${t.y}`, scale, true);
  }
  ctx.restore();
}

function dot(x, y, t, scale) {
  ctx.beginPath();
  ctx.arc(x, y, 4 * scale, 0, Math.PI * 2);
  ctx.fillStyle = color(t);
  ctx.fill();
  ctx.lineWidth = 1.5 * scale;
  ctx.strokeStyle = '#fff';
  ctx.stroke();
}

function draw() {
  const d = ui.last;
  if (!d) return;
  const [w, h] = d.resolution ?? [canvas.width, canvas.height];
  if (canvas.width !== w || canvas.height !== h) {
    canvas.width = w;
    canvas.height = h;
  }
  const scale = Math.max(w, h) / 640;
  if (ui.status?.stream?.image !== false && ui.frame) {
    ctx.drawImage(ui.frame, 0, 0, w, h);
  } else {
    ctx.fillStyle = '#14171c';
    ctx.fillRect(0, 0, w, h);
  }
  ctx.font = `600 ${Math.round(15 * scale)}px system-ui, sans-serif`;
  ctx.textBaseline = 'top';
  const lw = Math.max(2, 3 * scale);
  const min = ui.minScore;

  if (ui.flavor === 'grove-sd') drawTrack(d, w, h, scale);
  for (const [x, y, bw, bh, score, t] of d.boxes ?? []) {
    if (ui.flavor === 'grove-sd') break;                // drawn by drawTrack
    if (score < min) continue;
    ctx.lineWidth = lw;
    box(x, y, bw, bh, score, t, scale);
  }
  for (const [b, pts] of d.keypoints ?? []) {
    const [x, y, bw, bh, score, t] = b ?? [];
    if (score < min) continue;
    ctx.lineWidth = lw;
    box(x, y, bw, bh, score, t, scale);
    for (const [px, py] of pts ?? []) dot(px, py, t, scale);
  }
  for (const [x, y, score, t] of d.points ?? []) {
    if (score >= min) dot(x, y, t, scale);
  }
  if (ui.labels) {
    const cls = (d.classes ?? []).filter(([score]) => score >= min).sort((a, b) => b[0] - a[0]);
    cls.forEach(([score, t], i) => label(10 * scale, 10 * scale + i * 28 * scale, t, `${className(t)} ${score}%`, scale, false));
  }
}

function renderResults(d) {
  const items = [];
  for (const b of d?.boxes ?? []) items.push([b[4], b[5]]);
  for (const [b] of d?.keypoints ?? []) if (b) items.push([b[4], b[5]]);
  for (const p of d?.points ?? []) items.push([p[2], p[3]]);
  for (const c of d?.classes ?? []) items.push([c[0], c[1]]);
  const shown = items.filter(([score]) => score >= ui.minScore).sort((a, b) => b[0] - a[0]);

  $('#det-count').textContent = shown.length ? String(shown.length) : '';
  const list = $('#det-list');
  if (!shown.length) {
    list.replaceChildren(el('li', { className: 'empty', textContent: 'Nothing detected' }));
    return;
  }
  list.replaceChildren(...shown.map(([score, t]) => {
    const li = el('li', {},
      el('span', { className: 'swatch' }),
      el('span', { className: 'name', textContent: className(t) }),
      el('span', { className: 'score', textContent: `${score}%` }));
    li.style.setProperty('--c', color(t));
    li.style.setProperty('--w', `${Math.max(0, Math.min(100, score))}%`);
    return li;
  }));
}

// ---- settings -------------------------------------------------------------------

function renderInfo() {
  const i = ui.info ?? {};
  const mi = i.modelInfo ?? {};
  const v = i.version ?? {};

  // Seeed's firmware gets the detection controls; the SD demo firmware gets the Files tab.
  // Which firmware: remember the last one seen, so a disconnect (empty info) doesn't bring
  // back the other firmware's cards; until a firmware has been seen, show neither set.
  if (i.flavor) ui.flavor = i.flavor;
  const sdFw = ui.flavor === 'grove-sd';
  for (const node of document.querySelectorAll('[data-flavor]')) node.hidden = !ui.flavor || node.dataset.flavor !== ui.flavor;
  $('#files-needs-fw').hidden = sdFw || !i.flavor;
  $('#files-body').hidden = !sdFw;
  $('#files-fw-name').textContent = v.software ? `${i.name ?? 'firmware'} ${v.software}` : 'another firmware';
  $('#capture-card').hidden = !sdFw || (i.demo ?? 1) < 2;
  if (sdFw && i.sd) renderSd(i.sd);
  if (sdFw && ui.tab === 'files' && !ui.files.loaded) loadDir(ui.files.path);

  fillDl($('#device-dl'), [
    ['Name', i.name],
    ['Device ID', i.id],
    ['Firmware', v.software],
    ['Hardware rev', v.hardware],
    ['AT API', v.at_api],
    ['Boot count', i.stat?.boot_count],
    ['Ready', i.stat ? (i.stat.is_ready ? 'Yes' : 'No') : null],
  ]);

  renderCamera();
  renderModel();

  const a = i.algorithm;
  const cfg = a?.config;
  fillDl($('#algo-dl'), a?.type ? [
    ['Algorithm', ALGO_NAMES[a.type] ?? `Type ${a.type}`],
    ['Task', CATEGORY_NAMES[a.categroy] ?? `Category ${a.categroy}`],
    ['Min score', cfg?.tscore !== undefined ? `${cfg.tscore}%` : undefined],
    ['Box overlap (IoU)', cfg?.tiou !== undefined ? `${cfg.tiou}%` : undefined],
  ] : [['Status', 'Start the preview to read these']]);
  $('#tscore-hint').textContent = cfg?.tscore !== undefined
    ? `The board only reports results scoring ${cfg.tscore}% or more.` : '';

  renderAction();
  $('#live-model').textContent = liveLine(i, mi);
  if (ui.last) {
    draw();
    renderResults(ui.last);
  }
}

function renderCamera() {
  const i = ui.info;
  const sel = $('#sensor-select');
  if (!ui.dirty.has('camera')) {
    sel.replaceChildren(...(i.sensors ?? []).map((s) => el('option', {
      value: String(s.id),
      textContent: `${SENSOR_TYPES[s.type] ?? `Sensor type ${s.type}`} #${s.id}${s.id === i.sensor?.id && s.state === 1 ? ' (active)' : ''}`,
    })));
    if (i.sensor?.id) sel.value = String(i.sensor.id);
  }
  renderResolutions();
}

function renderResolutions() {
  const i = ui.info;
  const sensor = (i.sensors ?? []).find((s) => String(s.id) === $('#sensor-select').value);
  const sel = $('#opt-select');
  const keep = ui.dirty.has('camera') ? sel.value : null;
  const opts = Object.entries(sensor?.opts ?? {}).sort((a, b) => Number(a[0]) - Number(b[0]));
  sel.replaceChildren(...opts.map(([k, name]) => el('option', { value: k, textContent: name })));
  const current = sensor?.id === i.sensor?.id ? i.sensor?.opt_id : sensor?.opt_id;
  sel.value = keep ?? String(current ?? opts[0]?.[0] ?? '');
  $('#btn-sensor').disabled = !sensor;
}

function renderModel() {
  const i = ui.info;
  const mi = i.modelInfo ?? {};
  const args = mi.arguments ?? {};
  const sel = $('#model-select');
  if (!ui.dirty.has('model')) {
    sel.replaceChildren(...(i.models ?? []).map((m) => el('option', {
      value: String(m.id),
      textContent: `Slot ${m.id}${m.id === i.model?.id && mi.model_name ? ` · ${mi.model_name}` : ''}`,
    })));
    if (i.model?.id) sel.value = String(i.model.id);
  }
  $('#btn-model').disabled = !(i.models ?? []).length;
  fillDl($('#model-dl'), i.modelInfo ? [
    ['Name', mi.model_name ?? (mi.raw ? 'Unknown (not a SenseCraft model card)' : undefined)],
    ['SenseCraft ID', mi.model_id],
    ['Version', mi.version],
    ['Author', mi.author],
    ['Task', args.task],
    ['Format', mi.model_format],
  ] : [['Name', 'No model information stored']]);
  $('#model-classes').replaceChildren(...classes().map((c, idx) => {
    const chip = el('span', { className: 'chip', textContent: c });
    chip.style.setProperty('--c', color(idx));
    return chip;
  }));
}

function clampScore(v) {
  return Math.round(Math.min(100, Math.max(0, Number(v) || 0)));
}

function builtAction() {
  return `((max_score(target,${Number($('#act-class').value) || 0})>=${clampScore($('#act-score').value)})&&led(1))||led(0)`;
}

function describeAction(expr) {
  const m = ACTION_RE.exec(expr ?? '');
  if (m) return `LED on when ${className(Number(m[1]))} scores at least ${m[2]}%`;
  return expr ? 'Custom rule' : 'No rule set';
}

function renderAction() {
  const current = ui.info.action ?? '';
  if (!ui.dirty.has('action')) {
    const cls = classes();
    const sel = $('#act-class');
    sel.replaceChildren(...(cls.length ? cls : ['Class 0']).map((c, idx) => el('option', { value: String(idx), textContent: c })));
    const m = ACTION_RE.exec(current);
    if (m) {
      sel.value = m[1];
      $('#act-score').value = m[2];
    }
  }
  if (!ui.dirty.has('raw')) $('#act-raw').value = current;
  $('#act-preview').textContent = builtAction();
  $('#act-summary').textContent = `${describeAction(current)}:`;
  $('#act-current').textContent = current || '(empty)';
}

// ---- log & console ------------------------------------------------------------------

function appendLog(text, kind = '') {
  const log = $('#log');
  const atBottom = log.scrollHeight - log.scrollTop - log.clientHeight < 24;
  log.append(el('span', { className: kind, textContent: `${text}\n` }));
  while (log.childNodes.length > 400) log.firstChild.remove();
  if (atBottom) log.scrollTop = log.scrollHeight;
}

$('#console-form').addEventListener('submit', async (e) => {
  e.preventDefault();
  const input = $('#console-input');
  const cmd = input.value.trim().replace(/^AT\+/i, '');
  if (!cmd) return;
  ui.history = [cmd, ...ui.history.filter((h) => h !== cmd)].slice(0, 30);
  ui.historyPos = -1;
  appendLog(`> AT+${cmd}`, 'cmd');
  try {
    const r = await api('console', { command: cmd });
    if (r.reply) appendLog(JSON.stringify(r.reply.data ?? r.reply, null, 2), r.reply.code === 0 ? 'reply' : 'error');
    else if (!r.logs.length) appendLog('(no reply)', 'muted');
  } catch (err) {
    appendLog(err.message, 'error');
  }
  input.value = '';
});

$('#console-input').addEventListener('keydown', (e) => {
  if (e.key !== 'ArrowUp' && e.key !== 'ArrowDown') return;
  e.preventDefault();
  ui.historyPos = Math.max(-1, Math.min(ui.history.length - 1, ui.historyPos + (e.key === 'ArrowUp' ? 1 : -1)));
  e.target.value = ui.historyPos < 0 ? '' : ui.history[ui.historyPos];
});

// ---- controls -------------------------------------------------------------------------

$('#btn-stream').addEventListener('click', (e) => run(e.currentTarget, () => api('stream', { running: !ui.status?.stream?.want })));
$('#opt-image').addEventListener('change', (e) => run(e.currentTarget, () => api('stream', { image: e.currentTarget.checked })));
$('#opt-gray').addEventListener('change', (e) => run(e.currentTarget, () => api('stream', { gray: e.currentTarget.checked })));
$('#opt-live-size').addEventListener('change', (e) => run(e.currentTarget, () => api('stream', { size: Number(e.currentTarget.value) })));
$('#opt-cam-fps').addEventListener('change', (e) => run(e.currentTarget, () => api('stream', { fps: Number(e.currentTarget.value) })));
$('#opt-video').addEventListener('change', (e) => run(e.currentTarget, () => api('stream', { video: Number(e.currentTarget.value) })));

// Frames the board processed (the camera's rate), from the "n" counter in frames and TRACKPOS.
function countProcessed(n) {
  if (typeof n !== 'number') return;
  const now = performance.now();
  ui.proc = (ui.proc ?? []).filter((p) => now - p[0] < 2000);
  ui.proc.push([now, n]);
  if (ui.proc.length >= 2) {
    const [t0, n0] = ui.proc[0], [t1, n1] = ui.proc[ui.proc.length - 1];
    if (t1 > t0 && n1 >= n0) $('#stat-proc').textContent = ((n1 - n0) * 1000 / (t1 - t0)).toFixed(0);
  }
}

$('#opt-labels').checked = ui.labels;
$('#opt-labels').addEventListener('change', (e) => {
  ui.labels = e.currentTarget.checked;
  store.set('labels', ui.labels);
  draw();
});

$('#min-score').value = String(ui.minScore);
$('#min-score-out').textContent = String(ui.minScore);
$('#min-score').addEventListener('input', (e) => {
  ui.minScore = Number(e.currentTarget.value);
  $('#min-score-out').textContent = String(ui.minScore);
  store.set('minScore', ui.minScore);
  draw();
  renderResults(ui.last);
});

$('#btn-snapshot').addEventListener('click', () => {
  if (!ui.last) {
    toast('No frame to save yet', true);
    return;
  }
  canvas.toBlob((blob) => {
    const a = el('a', { href: URL.createObjectURL(blob), download: `grove-${new Date().toISOString().replace(/[:.]/g, '-')}.png` });
    document.body.append(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(a.href), 1000);
  }, 'image/png');
});

$('#btn-refresh').addEventListener('click', (e) => run(e.currentTarget, () => api('refresh'), 'Device info refreshed'));
$('#btn-reboot').addEventListener('click', (e) => {
  if (!confirm('Reboot the board? The preview stops for a few seconds.')) return;
  run(e.currentTarget, () => api('reboot'), 'Board rebooted');
});

$('#sensor-select').addEventListener('change', () => {
  ui.dirty.add('camera');
  renderResolutions();
});
$('#opt-select').addEventListener('change', () => ui.dirty.add('camera'));
$('#btn-sensor').addEventListener('click', async (e) => {
  const body = { id: Number($('#sensor-select').value), opt: Number($('#opt-select').value) };
  if (await run(e.currentTarget, () => api('sensor', body).then(() => true), 'Camera updated')) {
    ui.dirty.delete('camera');
    renderCamera();
  }
});

$('#model-select').addEventListener('change', () => ui.dirty.add('model'));
$('#btn-model').addEventListener('click', async (e) => {
  const id = Number($('#model-select').value);
  if (await run(e.currentTarget, () => api('model', { id }).then(() => true), `Model slot ${id} loaded`)) {
    ui.dirty.delete('model');
    renderModel();
  }
});

for (const id of ['#act-class', '#act-score']) {
  $(id).addEventListener('input', () => {
    ui.dirty.add('action');
    $('#act-preview').textContent = builtAction();
  });
}
$('#act-raw').addEventListener('input', () => ui.dirty.add('raw'));

async function saveAction(button, expression, which) {
  if (await run(button, () => api('action', { expression }).then(() => true), 'LED rule saved')) {
    ui.dirty.delete(which);
    renderAction();
  }
}
$('#btn-action').addEventListener('click', (e) => saveAction(e.currentTarget, builtAction(), 'action'));
$('#btn-action-raw').addEventListener('click', (e) => {
  const expression = $('#act-raw').value.trim();
  if (!expression) {
    toast('The expression is empty', true);
    return;
  }
  saveAction(e.currentTarget, expression, 'raw');
});

for (const b of document.querySelectorAll('[data-led]')) {
  b.addEventListener('click', (e) => run(e.currentTarget, () => api('led', { on: b.dataset.led === '1' }), `LED ${b.dataset.led === '1' ? 'on' : 'off'} sent`));
}

$('#btn-release').addEventListener('click', (e) => run(e.currentTarget, () => api('release'), 'Serial port released'));
$('#btn-connect').addEventListener('click', (e) => run(e.currentTarget, () => api('connect')));

// ---- files (SD demo firmware) ------------------------------------------------------

ui.files = { path: '/', entries: [], loaded: false };
ui.transfers = new Map();

const ICONS = {
  dir: '<path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"/>',
  file: '<path d="M14 3H7a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h10a2 2 0 0 0 2-2V8z"/><path d="M14 3v5h5"/>',
  image: '<rect x="3" y="4" width="18" height="16" rx="2"/><circle cx="9" cy="10" r="2"/><path d="m21 17-5-5-9 8"/>',
};

function icon(kind) {
  const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
  svg.setAttribute('viewBox', '0 0 24 24');
  svg.setAttribute('class', `icon ${kind}`);
  svg.setAttribute('aria-hidden', 'true');
  svg.innerHTML = ICONS[kind];
  return svg;
}

function fmtSize(n) {
  if (n < 1024) return `${n} B`;
  if (n < 1048576) return `${(n / 1024).toFixed(1)} KB`;
  if (n < 1073741824) return `${(n / 1048576).toFixed(1)} MB`;
  return `${(n / 1073741824).toFixed(1)} GB`;
}
const joinPath = (dir, name) => `${dir.replace(/\/$/, '')}/${name}`;
const isImage = (name) => /\.(jpe?g|png|bmp|gif)$/i.test(name);
const isText = (name) => /\.(txt|json|csv|log|md)$/i.test(name);
const fileUrl = (path, inline) => `/api/sd/file?path=${encodeURIComponent(path)}${inline ? '&inline=1' : ''}`;
const emptyRow = (text) => el('tr', {}, el('td', { colSpan: 4, className: 'empty', textContent: text }));

function openFiles() {
  if (ui.flavor !== 'grove-sd') return;
  if (!ui.files.loaded) loadDir(ui.files.path);
  if (!ui.capShown) showLastCapture();
}

function renderSd(sd) {
  fillDl($('#sd-dl'), sd.mounted ? [
    ['File system', sd.fs],
    ['Label', sd.label || '(none)'],
    ['Size', sd.total_kb != null ? fmtSize(sd.total_kb * 1024) : undefined],
    ['Free', sd.free_kb != null ? fmtSize(sd.free_kb * 1024) : undefined],
  ] : [['Status', sd.error ?? 'No card']]);
  const meter = $('#sd-meter');
  meter.hidden = !(sd.mounted && sd.total_kb);
  if (!meter.hidden) meter.firstElementChild.style.width = `${((sd.total_kb - sd.free_kb) / sd.total_kb) * 100}%`;
}

async function refreshSd() {
  try {
    renderSd(await apiGet('sd'));
  } catch (err) {
    toast(err.message, true);
  }
}

async function loadDir(path) {
  const rows = $('#file-rows');
  if (!ui.files.loaded || ui.files.path !== path) rows.replaceChildren(emptyRow('Loading…'));
  try {
    const r = await apiGet(`sd/list?path=${encodeURIComponent(path)}`);
    ui.files = { path: r.path, entries: r.entries, loaded: true };
    renderFiles();
  } catch (err) {
    rows.replaceChildren(emptyRow(err.message));
  }
}

function renderCrumbs() {
  const parts = ui.files.path.split('/').filter(Boolean);
  const items = [];
  const add = (text, path, current) => {
    if (items.length) items.push(el('span', { className: 'sep', textContent: '/' }));
    const b = el('button', { type: 'button', textContent: text });
    if (current) b.setAttribute('aria-current', 'page');
    else b.addEventListener('click', () => loadDir(path));
    items.push(b);
  };
  add('SD card', '/', parts.length === 0);
  parts.forEach((p, i) => add(p, `/${parts.slice(0, i + 1).join('/')}`, i === parts.length - 1));
  $('#crumbs').replaceChildren(...items);
  $('#btn-up').disabled = parts.length === 0;
}

function renderFiles() {
  renderCrumbs();
  const { entries, path } = ui.files;
  const rows = $('#file-rows');
  if (!entries.length) {
    rows.replaceChildren(emptyRow('This folder is empty'));
    return;
  }
  rows.replaceChildren(...entries.map((f) => {
    const full = joinPath(path, f.name);
    const name = el('div', { className: 'fname' }, icon(f.dir ? 'dir' : isImage(f.name) ? 'image' : 'file'));
    if (f.dir) {
      const open = el('button', { type: 'button', textContent: f.name, title: f.name });
      open.addEventListener('click', () => loadDir(full));
      name.append(open);
    } else {
      name.append(el('span', { className: 'text', textContent: f.name, title: f.name }));
    }
    const actions = el('td', { className: 'actions' });
    if (!f.dir) {
      if (isImage(f.name) || isText(f.name)) {
        actions.append(el('a', { href: fileUrl(full, true), target: '_blank', rel: 'noopener', textContent: 'View' }));
      }
      actions.append(el('a', { href: fileUrl(full, false), textContent: 'Download' }));
    }
    const rename = el('button', { type: 'button', textContent: 'Rename' });
    rename.addEventListener('click', () => renameEntry(f));
    const del = el('button', { type: 'button', textContent: 'Delete', className: 'del' });
    del.addEventListener('click', () => deleteEntry(f));
    actions.append(rename, del);
    return el('tr', {},
      el('td', {}, name),
      el('td', { className: 'num', textContent: f.dir ? '' : fmtSize(f.size) }),
      el('td', { className: 'when', textContent: f.mtime ? new Date(f.mtime).toLocaleString() : '' }),
      actions);
  }));
}

async function renameEntry(f) {
  const name = prompt(`New name for ${f.name}`, f.name);
  if (!name || name === f.name) return;
  if (name.includes('/')) {
    toast('Names cannot contain /', true);
    return;
  }
  try {
    await api('sd/move', { from: joinPath(ui.files.path, f.name), to: joinPath(ui.files.path, name) });
    toast(`Renamed to ${name}`);
    loadDir(ui.files.path);
  } catch (err) {
    toast(err.message, true);
  }
}

async function deleteEntry(f) {
  const what = f.dir ? `the folder "${f.name}" and everything in it` : `"${f.name}"`;
  if (!confirm(`Delete ${what} from the SD card? This cannot be undone.`)) return;
  try {
    await api(`sd/file?path=${encodeURIComponent(joinPath(ui.files.path, f.name))}${f.dir ? '&recursive=1' : ''}`, undefined, 'DELETE');
    toast(`Deleted ${f.name}`);
    loadDir(ui.files.path);
    refreshSd();
  } catch (err) {
    toast(err.message, true);
  }
}

// One row per running transfer, fed by the server's progress messages.
function renderTransfer(p) {
  let row = ui.transfers.get(p.id);
  if (!row) {
    if (p.finished) return;
    row = el('div', { className: 'transfer' },
      el('span', { className: 'what' }), el('span', { className: 'pct' }), el('div', { className: 'bar' }, el('span')));
    ui.transfers.set(p.id, row);
    $('#transfers').append(row);
  }
  const tiles = /tiles/i.test(p.stage ?? '');
  row.querySelector('.what').textContent = p.stage ? `${p.label} · ${p.stage}` : p.label;
  row.querySelector('.pct').textContent = !p.total ? '' : tiles ? `${p.done} of ${p.total}` : `${fmtSize(p.done)} of ${fmtSize(p.total)}`;
  row.querySelector('.bar span').style.width = `${p.total ? Math.min(100, (p.done / p.total) * 100) : 0}%`;
  if (p.finished) {
    ui.transfers.delete(p.id);
    setTimeout(() => row.remove(), 1200);
  }
}

// The Live card's second line: the model on Seeed's firmware, the stream format on ours.
function liveLine(i, mi) {
  if (i.flavor === 'grove-sd') {
    const size = ui.status?.stream?.size === 640 ? '640×480' : '320×240';
    return `${size} · ${ui.status?.stream?.gray === false ? 'colour' : 'grayscale'}`;
  }
  return [mi.model_name, i.sensor?.opt_detail].filter(Boolean).join(' · ') || '–';
}

function showCapture(r) {
  ui.capShown = true;
  const url = r.localUrl ?? fileUrl(r.sdPath, true);
  $('#cap-result').hidden = false;
  $('#cap-img').src = r.localUrl ? `${r.localUrl}?t=${Date.now()}` : url;
  $('#cap-link').href = url;
  const total = r.ms ? Object.values(r.ms).reduce((a, b) => a + b, 0) : null;
  fillDl($('#cap-dl'), [
    ['Image', r.width ? `${r.width}×${r.height} · ${fmtSize(r.size)}` : `${r.name} · ${fmtSize(r.size)}`],
    ['On the card', r.sdPath],
    ['Made', r.joined === 'direct' ? `saved directly by the board${r.view ? ` · ${r.view}` : ''}`
      : r.tiles ? `${r.tiles} tiles, ${r.joined === 'lossless' ? 'joined without re-compressing' : 'joined and scaled'}${r.view ? ` · ${r.view}` : ''}` : undefined],
    ['Took', total ? `${(total / 1000).toFixed(1)} s (camera ${(r.ms.capture / 1000).toFixed(1)} s)` : undefined],
  ]);
}

async function showLastCapture() {
  ui.capShown = true;
  try {
    const [last] = await apiGet('captures');
    if (last) showCapture({ ...last, localUrl: last.url, sdPath: `/DCIM/${last.name.replace(/\.jpe?g$/i, '')}/full.jpg` });
  } catch { /* no local captures yet */ }
}

$('#btn-up').addEventListener('click', () => loadDir(ui.files.path.replace(/\/[^/]+$/, '') || '/'));
$('#btn-ls').addEventListener('click', () => {
  loadDir(ui.files.path);
  refreshSd();
});
$('#btn-sd-refresh').addEventListener('click', (e) => run(e.currentTarget, async () => renderSd(await apiGet('sd'))));
$('#btn-sd-mount').addEventListener('click', (e) => run(e.currentTarget, async () => {
  renderSd(await api('sd/mount'));
  loadDir('/');
}, 'Card mounted'));

$('#btn-sd-format').addEventListener('click', (e) => {
  const answer = prompt('Formatting ERASES EVERYTHING on the SD card and makes a fresh FAT32 file system.\n\nType ERASE to go ahead.');
  if (answer !== 'ERASE') return;
  run(e.currentTarget, async () => {
    toast('Formatting… this takes a little while');
    renderSd(await api('sd/format', { format: 'FAT32', confirm: 'ERASE' }));
    loadDir('/');
  }, 'Card formatted');
});

$('#btn-newdir').addEventListener('click', async () => {
  const name = prompt('Folder name');
  if (!name) return;
  if (name.includes('/')) {
    toast('Names cannot contain /', true);
    return;
  }
  try {
    await api('sd/mkdir', { path: joinPath(ui.files.path, name) });
    loadDir(ui.files.path);
  } catch (err) {
    toast(err.message, true);
  }
});

$('#upload-input').addEventListener('change', async (e) => {
  const files = [...e.currentTarget.files];
  e.currentTarget.value = '';
  const dir = ui.files.path;
  for (const file of files) {
    if (ui.files.entries.some((f) => f.name === file.name) && !confirm(`"${file.name}" already exists. Replace it?`)) continue;
    try {
      const res = await fetch(`/api/sd/file?path=${encodeURIComponent(joinPath(dir, file.name))}`, {
        method: 'PUT', headers: { 'content-type': 'application/octet-stream' }, body: file,
      });
      if (res.status === 413) throw new Error('File too large (the limit is 32 MB)');
      const json = await res.json().catch(() => ({ ok: false, error: `Upload failed (HTTP ${res.status})` }));
      if (!json.ok) throw new Error(json.error);
      toast(`Uploaded ${file.name}`);
    } catch (err) {
      toast(`${file.name}: ${err.message}`, true);
    }
  }
  if (ui.files.path === dir) loadDir(dir);
  refreshSd();
});

$('#btn-capture').addEventListener('click', (e) => run(e.currentTarget, async () => {
  const [width, height] = $('#cap-size').value.split('x').map(Number);
  const r = await api('capture', { width, height });
  showCapture(r);
  if (ui.files.path === '/' || r.dir.startsWith(ui.files.path)) loadDir(ui.files.path);
  refreshSd();
}, 'Captured and saved to the SD card'));

// ---- WebSocket ------------------------------------------------------------------------

const handlers = {
  status({ status }) {
    const prev = ui.status;
    ui.status = status;
    // Start the frozen-picture clock whenever frames become expected, but not on the
    // state changes the watchdog itself causes while it recovers.
    const expectStart = !prev
      || (status.stream.want && !prev.stream.want)
      || (status.state === 'ready' && prev.state !== 'ready' && !status.watchdog?.stalled);
    if (expectStart) ui.lastFrameAt = Date.now();
    if (!status.stream.running) {
      ui.times = [];
      $('#stat-fps').textContent = '–';
    }
    renderStatus();
  },
  info({ info }) {
    ui.info = info ?? {};
    renderInfo();
  },
  result({ data }) {
    onResult(data);
  },
  log({ line, source }) {
    appendLog(line, source === 'server' ? 'server' : 'muted');
  },
  suggest({ data }) {
    ui.suggest = data ? { ...data, receivedAt: Date.now() } : null;
    renderSuggest(data);
  },
  trackpos({ data }) {
    // a frame that wasn't sent: move the tracking box anyway
    countProcessed(data.n);
    ui.lastFrameAt = Date.now();                       // video may be off: the board is alive
    if (ui.last && data.track) {
      ui.last = { ...ui.last, track: data.track };
      draw();
      renderTrack(data.track);
    }
  },
  track({ data }) {
    renderTrack(data.state === 'lost' ? { state: 'off' } : data);
    if (data.state === 'lost') ui.targetLost = true;           // flashes until the next lock-on
    if (data.state === 'locked' || data.state === 'armed') ui.targetLost = false;
  },
  motion({ data }) {
    if (ui.motion) ui.motion = { ...ui.motion, last: data, events: (ui.motion.events ?? 0) + 1 };
    $('#motion-badge').textContent = String(ui.motion?.events ?? '');
    $('#motion-live').textContent = motionText(data);
    const stage = document.querySelector('.stage');
    stage.classList.remove('motion-flash');
    void stage.offsetWidth;                            // restart the animation
    stage.classList.add('motion-flash');
    if (data.error) toast(`Motion: ${data.error}`, true);
  },
  progress(p) {
    renderTransfer(p);
  },
  tick() {},
};

function connect() {
  const ws = new WebSocket(`${location.protocol === 'https:' ? 'wss' : 'ws'}://${location.host}/ws`);
  ui.ws = ws;
  ui.lastMsgAt = Date.now();
  ws.onopen = () => {
    ui.link = true;
    renderStatus();
  };
  ws.onmessage = (e) => {
    ui.lastMsgAt = Date.now();
    const msg = JSON.parse(e.data);
    handlers[msg.type]?.(msg);
  };
  ws.onclose = () => {
    if (ui.ws !== ws) return;
    ui.link = false;
    renderStatus();
    setTimeout(connect, 1500);
  };
}

// Keep the frozen-picture counter ticking, and reconnect if the server has gone silent
// (a half-open connection never fires onclose by itself).
setInterval(() => {
  if (ui.link && Date.now() - ui.lastMsgAt > LINK_TIMEOUT_MS) {
    appendLog('No word from the server, reconnecting…', 'server');
    const stale = ui.ws;
    ui.ws = null; // its onclose (which may come much later) is now ignored
    ui.link = false;
    stale?.close();
    connect();
  }
  if (ui.status?.stream?.want || ui.status?.watchdog?.stalled) renderStatus();
}, 500);

const firstTab = location.hash.slice(1);
showTab(['files', 'settings'].includes(firstTab) ? firstTab : 'preview');
renderStatus();
connect();
