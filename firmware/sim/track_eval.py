#!/usr/bin/env python3
"""Tracker accuracy test on generated colour sequences: brightness only vs with colour.

Builds 320x240 test videos from real photos (a textured object cut from one photo, moving
over another, with camera noise), runs the PC build of the tracker (track_harness) on them
with and without colour, and scores the tracked centre against the true one.

    python3 sim/track_eval.py <photo folder> [--seeds 6] [--frames 300] [--weights 0.3]
                              [--keep out_dir] [--only rotate,clutter]

Needs numpy and opencv (test/full-web-ui/.venv has both). Builds sim/track_harness itself.

Scenarios:
  move     the object moves around (control: colour should not make it worse)
  rotate   it turns a full circle while moving
  deform   it stretches and squashes (x vs y) and tilts back and forth
  isolum   it is about as bright as the background, but a different colour
  clutter  two look-alikes (same brightness pattern, other colours) cross its path
  occlude  a bar passes over half of it
  light    the light changes by +-40% while it moves
  dull     like move, in a nearly colourless scene (colour must not make it worse)

Score per run: "ok" = frames where the tracker was locked and within 10 px of the true centre
(or 25% of the object's size, if larger), as a share of all frames. A "lost" ends the run
(the board stops tracking there), so every frame after it counts as missed.
"""
import argparse, glob, json, math, os, subprocess, sys, tempfile
import numpy as np
import cv2

W, H = 320, 240
HERE = os.path.dirname(os.path.abspath(__file__))
SCENARIOS = ['move', 'rotate', 'deform', 'isolum', 'clutter', 'occlude', 'light', 'dull']


def build_harness():
    exe = os.path.join(HERE, 'track_harness')
    src = [os.path.join(HERE, 'track_harness.c'), os.path.join(HERE, '..', 'grove_sd_demo', 'tracker.c')]
    if os.environ.get('TRACK_CFLAGS') or not os.path.exists(exe) or any(os.path.getmtime(s) > os.path.getmtime(exe) for s in src):
        subprocess.check_call(['gcc', '-O2', '-o', exe] + os.environ.get('TRACK_CFLAGS', '').split() + src + ['-lm'])
    return exe


def load_photos(folder):
    out = []
    for f in sorted(glob.glob(os.path.join(folder, '*.jpg'))):
        img = cv2.imread(f)
        if img is None:
            continue
        yuv = cv2.cvtColor(img, cv2.COLOR_BGR2YUV)
        if yuv[..., 1].std() + yuv[..., 2].std() < 6:          # grayscale captures: no colour
            continue
        out.append(img)
    return out


def textured_patch(rng, img, pw, ph):
    """A textured pw x ph patch (the object) from img: the most detailed of a few tries."""
    best, best_std = None, -1
    for _ in range(12):
        s = rng.uniform(0.5, 1.0) * min(img.shape[0] / ph, img.shape[1] / pw) * 0.25
        cw, ch = int(pw * s) or pw, int(ph * s) or ph
        x = rng.integers(0, img.shape[1] - cw)
        y = rng.integers(0, img.shape[0] - ch)
        p = cv2.resize(img[y:y + ch, x:x + cw], (pw, ph), interpolation=cv2.INTER_AREA)
        sd = cv2.cvtColor(p, cv2.COLOR_BGR2GRAY).std()
        if sd > best_std:
            best, best_std = p, sd
    return best


def recolour(patch_bgr, hue_shift, sat=1.0):
    hsv = cv2.cvtColor(patch_bgr, cv2.COLOR_BGR2HSV).astype(np.float32)
    hsv[..., 0] = (hsv[..., 0] + hue_shift) % 180
    hsv[..., 1] = np.clip(np.maximum(hsv[..., 1], 90) * sat, 0, 255)
    out = cv2.cvtColor(hsv.astype(np.uint8), cv2.COLOR_HSV2BGR)
    # keep the original brightness pattern exactly (only the colour changes)
    yo = cv2.cvtColor(patch_bgr, cv2.COLOR_BGR2YUV)
    yn = cv2.cvtColor(out, cv2.COLOR_BGR2YUV)
    yn[..., 0] = yo[..., 0]
    return cv2.cvtColor(yn, cv2.COLOR_YUV2BGR)


def paste(frame, patch, mask, cx, cy, angle=0.0, sx=1.0, sy=1.0):
    """Pastes the (masked) patch centred at cx,cy, rotated / stretched."""
    ph, pw = patch.shape[:2]
    m = cv2.getRotationMatrix2D((pw / 2, ph / 2), angle, 1.0)
    m[:, :2] = m[:, :2] @ np.diag([sx, sy])
    m[:, 2] = [cx, cy] - m[:, :2] @ np.array([pw / 2, ph / 2])     # patch centre -> (cx, cy)
    warped = cv2.warpAffine(patch, m, (W, H), flags=cv2.INTER_LINEAR)
    wm = cv2.warpAffine(mask, m, (W, H), flags=cv2.INTER_LINEAR)[..., None]
    return (frame * (1 - wm) + warped * wm).astype(np.float32)


def make_sequence(rng, photos, kind, frames):
    bg_src, obj_src = rng.choice(len(photos), 2, replace=False)
    big = cv2.resize(photos[bg_src], (W + 80, H + 60), interpolation=cv2.INTER_AREA).astype(np.float32)
    pw, ph = 46, 36
    patch = textured_patch(rng, photos[obj_src], pw, ph).astype(np.float32)
    mask = np.zeros((ph, pw), np.float32)
    cv2.ellipse(mask, (pw // 2, ph // 2), (pw // 2 - 1, ph // 2 - 1), 0, 0, 360, 1.0, -1)
    mask = cv2.GaussianBlur(mask, (5, 5), 1.2)
    if kind == 'isolum':
        # object: background-like brightness (+ a faint pattern of its own), strong colour
        hue = rng.uniform(0, 180)
        twin = recolour(patch.astype(np.uint8), hue, 1.6)
    if kind == 'clutter':
        dist_a = recolour(patch.astype(np.uint8), 60, 1.4).astype(np.float32)
        dist_b = recolour(patch.astype(np.uint8), 120, 1.4).astype(np.float32)
    ph0 = rng.uniform(0, 2 * math.pi)
    seq, truth = [], []
    for t in range(frames):
        u = t / frames
        ox, oy = 40 + 30 * math.sin(2 * math.pi * u * 0.7), 30 + 20 * math.sin(2 * math.pi * u * 0.5)  # camera pan
        frame = big[int(oy):int(oy) + H, int(ox):int(ox) + W].copy()
        cx = W / 2 + 95 * math.sin(2 * math.pi * u * 1.1 + ph0) * min(1, t / 20)
        cy = H / 2 + 60 * math.sin(2 * math.pi * u * 1.7 + ph0 * 1.3) * min(1, t / 20)
        angle, sx, sy = 0.0, 1.0, 1.0
        if kind == 'rotate':
            angle = 360 * u
        elif kind == 'deform':
            k = 0.35 * math.sin(2 * math.pi * u * 2)
            sx, sy, angle = 1 + k, 1 - k, 35 * math.sin(2 * math.pi * u * 1.3)
        obj = patch
        if kind == 'isolum':
            # brightness of the background where the object is, plus 30% of its own pattern
            y_bg = cv2.cvtColor(np.clip(frame, 0, 255).astype(np.uint8), cv2.COLOR_BGR2YUV)[..., 0].astype(np.float32)
            x0, y0 = int(cx - pw / 2), int(cy - ph / 2)
            local = cv2.resize(y_bg[max(0, y0):max(0, y0) + ph, max(0, x0):max(0, x0) + pw], (pw, ph))
            tw = cv2.cvtColor(twin, cv2.COLOR_BGR2YUV).astype(np.float32)
            own = tw[..., 0]
            tw[..., 0] = 0.7 * local + 0.3 * (own - own.mean() + local.mean())
            obj = cv2.cvtColor(np.clip(tw, 0, 255).astype(np.uint8), cv2.COLOR_YUV2BGR).astype(np.float32)
        if kind == 'clutter':
            for k, d in enumerate((dist_a, dist_b)):
                dx = W / 2 + 110 * math.sin(2 * math.pi * u * (0.9 + 0.5 * k) + k * 2.1)
                dy = H / 2 + 70 * math.sin(2 * math.pi * u * (1.3 - 0.3 * k) + k)
                frame = paste(frame, d, mask, dx, dy)
        frame = paste(frame, obj, mask, cx, cy, angle, sx, sy)
        if kind == 'occlude':
            bx = (t * 1.6) % (W + 120) - 60
            frame[:, int(max(0, bx)):int(max(0, bx + 26))] = frame[:, int(max(0, bx)):int(max(0, bx + 26))] * 0.3 + 40
        if kind == 'light':
            frame *= 1 + 0.4 * math.sin(2 * math.pi * u * 1.5)
        if kind == 'dull':
            g = cv2.cvtColor(np.clip(frame, 0, 255).astype(np.uint8), cv2.COLOR_BGR2GRAY).astype(np.float32)[..., None]
            frame = g + 0.08 * (frame - g)
        frame += rng.normal(0, 3.0, frame.shape)                              # sensor noise
        img = np.clip(frame, 0, 255).astype(np.uint8)
        seq.append(cv2.cvtColor(img, cv2.COLOR_BGR2YUV_I420))                  # Y, U, V planes
        truth.append((cx, cy, pw * max(sx, sy), ph * max(sx, sy)))
    return seq, truth


def run(exe, path, truth, color, weight):
    cx, cy, bw, bh = truth[0]
    cmd = [exe] + (['-c'] if color else []) + (['-W', str(weight)] if weight is not None else []) + \
          [str(W), str(H), str(int(round(cx))), str(int(round(cy))), str(int(bw)), str(int(bh)), path]
    out = subprocess.run(cmd, capture_output=True, text=True, check=True)
    ok, errs, lost_at = 0, [], None
    ms = float(out.stderr.split('mean ')[1].split()[0]) if 'mean ' in out.stderr else 0
    for line in out.stdout.splitlines()[1:]:
        p = line.split()
        n = int(p[0])
        vals = dict(kv.split('=') for kv in p[2:] if '=' in kv)
        x, y = float(vals['cx']), float(vals['cy'])
        tx, ty, tw, th = truth[n]
        e = math.hypot(x - tx, y - ty)
        if p[1] == 'LOST':
            lost_at = n
            break
        errs.append(e)
        if e <= max(10, 0.25 * max(tw, th)):
            ok += 1
    return {'ok': ok / (len(truth) - 1), 'err': float(np.median(errs)) if errs else float('nan'), 'lost': lost_at, 'ms': ms}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('photos')
    ap.add_argument('--seeds', type=int, default=6)
    ap.add_argument('--frames', type=int, default=300)
    ap.add_argument('--weights', default='0.3', help='colour weights to try, comma separated')
    ap.add_argument('--only', default='')
    ap.add_argument('--keep', default='')
    a = ap.parse_args()
    exe = build_harness()
    photos = load_photos(a.photos)
    if len(photos) < 2:
        sys.exit('need at least 2 colour photos')
    weights = [float(x) for x in a.weights.split(',')]
    configs = [('luma', False, None)] + [(f'color {w:g}', True, w) for w in weights]
    kinds = a.only.split(',') if a.only else SCENARIOS
    tmp = a.keep or tempfile.mkdtemp()
    os.makedirs(tmp, exist_ok=True)
    print(f'{len(photos)} colour photos, {a.seeds} sequences per scenario, {a.frames} frames each')
    print(f'{"scenario":9} ' + ' '.join(f'{c[0]:>22}' for c in configs))
    totals = {c[0]: [] for c in configs}
    for kind in kinds:
        res = {c[0]: [] for c in configs}
        for seed in range(a.seeds):
            rng = np.random.default_rng(1000 * SCENARIOS.index(kind) + seed)
            seq, truth = make_sequence(rng, photos, kind, a.frames)
            path = os.path.join(tmp, f'{kind}-{seed}.yuv')
            with open(path, 'wb') as f:
                for fr in seq:
                    f.write(fr.tobytes())
            with open(path[:-4] + '.json', 'w') as f:
                json.dump(truth, f)
            for name, color, wgt in configs:
                res[name].append(run(exe, path, truth, color, wgt))
        cells = []
        for name, _, _ in configs:
            r = res[name]
            okm = 100 * np.mean([x['ok'] for x in r])
            lost = sum(x['lost'] is not None for x in r)
            err = np.nanmedian([x['err'] for x in r])
            cells.append(f'{okm:5.1f}% ok {lost}/{len(r)} lost {err:4.1f}px')
            totals[name] += [x['ok'] for x in r]
        print(f'{kind:9} ' + ' '.join(f'{c:>22}' for c in cells), flush=True)
    print(f'{"all":9} ' + ' '.join(f'{100 * np.mean(totals[c[0]]):5.1f}% ok{"":13}' for c in configs))
    ms = {c[0]: [] for c in configs}
    print('PC time per frame (ms):', ', '.join(f'{name} {np.mean([run(exe, os.path.join(tmp, f"{kinds[0]}-0.yuv"), json.load(open(os.path.join(tmp, f"{kinds[0]}-0.json"))), color, wgt)["ms"] for _ in range(3)]):.3f}' for name, color, wgt in configs))


if __name__ == '__main__':
    main()
