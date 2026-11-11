// Runs lib/suggest_worker.py (OpenCV GrabCut) as a long-lived helper: given a live frame and the
// lock-on box, it guesses the object the user means and returns a tight box and its outline.
// Uses the project's .venv Python when there is one (that's where OpenCV is installed).
import { spawn } from 'node:child_process';
import { existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

const WORKER = fileURLToPath(new URL('./suggest_worker.py', import.meta.url));
const VENV_PY = fileURLToPath(new URL('../.venv/bin/python', import.meta.url));

export class Suggester {
  constructor({ log = () => {} } = {}) {
    this.log = log;
    this.proc = null;
    this.pending = new Map();
    this.nextId = 1;
    this.buf = '';
    this.disabledUntil = 0;
  }

  _start() {
    const python = existsSync(VENV_PY) ? VENV_PY : 'python3';
    const proc = spawn(python, ['-u', WORKER], { stdio: ['pipe', 'pipe', 'pipe'] });
    this.proc = proc;
    this.buf = '';
    proc.stdout.on('data', (chunk) => {
      this.buf += chunk.toString();
      let nl;
      while ((nl = this.buf.indexOf('\n')) >= 0) {
        const line = this.buf.slice(0, nl);
        this.buf = this.buf.slice(nl + 1);
        let msg;
        try { msg = JSON.parse(line); } catch { continue; }
        const p = this.pending.get(msg.id);
        if (!p) continue;
        this.pending.delete(msg.id);
        clearTimeout(p.timer);
        if (msg.ok) p.resolve(msg); else p.reject(new Error(msg.error || 'suggestion failed'));
      }
    });
    let err = '';
    proc.stderr.on('data', (c) => { err = (err + c.toString()).slice(-2000); });
    proc.on('exit', (code) => {
      if (this.proc === proc) this.proc = null;
      for (const p of this.pending.values()) { clearTimeout(p.timer); p.reject(new Error('the suggestion helper stopped')); }
      this.pending.clear();
      if (code) {
        this.log(`Object suggestion helper exited (${code}): ${err.trim().split('\n').pop() || 'no message'}`);
        this.disabledUntil = Date.now() + 30000;          // don't respawn in a tight loop
      }
    });
  }

  // jpeg: base64 JPEG of the frame; box: [cx, cy, w, h] in frame pixels.
  suggest(jpeg, box, timeoutMs = 3000) {
    if (Date.now() < this.disabledUntil) return Promise.reject(new Error('the suggestion helper is unavailable'));
    if (!this.proc) this._start();
    const id = this.nextId++;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => { this.pending.delete(id); reject(new Error('suggestion timed out')); }, timeoutMs);
      this.pending.set(id, { resolve, reject, timer });
      this.proc.stdin.write(`${JSON.stringify({ id, jpeg, box })}\n`);
    });
  }

  stop() {
    if (this.proc) this.proc.kill();
    this.proc = null;
  }
}
