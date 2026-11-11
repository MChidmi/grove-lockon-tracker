"""Tests lib/suggest_worker.py's object suggestion on generated scenes with a known answer.

Each scene: a real photo as background (from captures/, or any folder given), scaled to the live
frame size, with an object cut from a *different* photo pasted near the centre box: irregular
blob outline, 25-80% of the box, offset up to a quarter of the box, sometimes sticking out of it.
Negative scenes have no object: the suggestion must not be confident there.

    .venv/bin/python test/suggest_test.py [photo folder] [--gray] [--n 60] [--size 320] [--show out.png]

Prints, for object scenes: how often the suggestion is confident and how well its box matches
the object (IoU); for empty scenes: how often it is wrongly confident.
"""
import argparse
import glob
import os
import sys
import time

import cv2
import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'lib'))
import suggest_worker  # noqa: E402


def blob_mask(w, h, rng):
    """An irregular blob filling most of a w x h box."""
    m = np.zeros((h, w), np.uint8)
    n = rng.integers(7, 13)
    ang = np.sort(rng.uniform(0, 2 * np.pi, n))
    r = rng.uniform(0.62, 1.0, n)
    pts = np.stack([w / 2 + np.cos(ang) * r * w / 2, h / 2 + np.sin(ang) * r * h / 2], 1).astype(np.int32)
    cv2.fillPoly(m, [pts], 1)
    m = cv2.GaussianBlur(m.astype(np.float32), (0, 0), 1.0)
    return m  # soft edge, 0..1


def iou(a, b):
    ax0, ay0, ax1, ay1 = a[0] - a[2] / 2, a[1] - a[3] / 2, a[0] + a[2] / 2, a[1] + a[3] / 2
    bx0, by0, bx1, by1 = b[0] - b[2] / 2, b[1] - b[3] / 2, b[0] + b[2] / 2, b[1] + b[3] / 2
    iw, ih = max(0, min(ax1, bx1) - max(ax0, bx0)), max(0, min(ay1, by1) - max(ay0, by0))
    inter = iw * ih
    return inter / (a[2] * a[3] + b[2] * b[3] - inter + 1e-9)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('photos', nargs='?', default=os.path.join(os.path.dirname(__file__), '..', 'captures'))
    ap.add_argument('--gray', action='store_true', help='grayscale frames, like the default live view')
    ap.add_argument('--n', type=int, default=60)
    ap.add_argument('--size', type=int, default=320, help='frame width (4:3)')
    ap.add_argument('--show', help='save a contact sheet of the first 12 scenes')
    ap.add_argument('--method', default='suggest', help='function in suggest_worker to test')
    ap.add_argument('--dump', help='write every scene\'s scores to this JSON file')
    a = ap.parse_args()
    rng = np.random.default_rng(7)
    files = sorted(glob.glob(os.path.join(a.photos, '*.jpg')))
    photos = [cv2.imread(f) for f in files]
    photos = [p for p in photos if p is not None and p.std() > 15 and p.shape[1] >= 640]
    if len(photos) < 2:
        sys.exit('need at least two photos')
    W, H = a.size, a.size * 3 // 4
    bw, bh = W // 4, W * 3 // 16
    cx, cy = W // 2, H // 2

    pos_conf, ious, neg_conf, times, sheet = 0, [], 0, [], []
    bins = {'clear': [], 'moderate': [], 'faint': []}   # per object scene: (confident, IoU)
    records = []
    for i in range(a.n):
        bg_i, ob_i = rng.choice(len(photos), 2, replace=False)
        bg = photos[bg_i]
        # a random 4:3 crop of the background photo, scaled to the frame size
        s = rng.uniform(0.35, 1.0)
        cw = int(min(bg.shape[1], bg.shape[0] * 4 / 3) * s); ch = cw * 3 // 4
        ox, oy = rng.integers(0, bg.shape[1] - cw + 1), rng.integers(0, bg.shape[0] - ch + 1)
        frame = cv2.resize(bg[oy:oy + ch, ox:ox + cw], (W, H), interpolation=cv2.INTER_AREA).astype(np.float32)
        negative = i % 4 == 3
        truth = None
        if not negative:
            ow = int(bw * rng.uniform(0.5, 1.1)); oh = int(bh * rng.uniform(0.5, 1.1))
            src = photos[ob_i]
            sx, sy = rng.integers(0, src.shape[1] - ow * 3), rng.integers(0, src.shape[0] - oh * 3)
            patch = cv2.resize(src[sy:sy + oh * 3, sx:sx + ow * 3], (ow, oh), interpolation=cv2.INTER_AREA).astype(np.float32)
            m = blob_mask(ow, oh, rng)[..., None]
            px = int(cx + rng.uniform(-0.25, 0.25) * bw - ow / 2)
            py = int(cy + rng.uniform(-0.25, 0.25) * bh - oh / 2)
            region = frame[py:py + oh, px:px + ow].copy()
            frame[py:py + oh, px:px + ow] = region * (1 - m) + patch * m
            # visibility: colour difference between the object and the background it covers,
            # in units of that background's spread (Lab, so it's close to what a person sees)
            lab = lambda im: cv2.cvtColor(np.clip(im, 0, 255).astype(np.uint8), cv2.COLOR_BGR2LAB).astype(np.float32)
            on = m[..., 0] > 0.5
            if a.gray:
                lo, lb = lab(patch)[..., :1][on], lab(region)[..., :1][on]
            else:
                lo, lb = lab(patch)[on], lab(region)[on]
            vis_score = float(np.linalg.norm(lo.mean(0) - lb.mean(0)) / (lb.std(0).mean() + 3.0))
            ys, xs = np.nonzero(m[..., 0] > 0.5)
            truth = [px + (xs.min() + xs.max() + 1) / 2, py + (ys.min() + ys.max() + 1) / 2,
                     float(xs.max() - xs.min() + 1), float(ys.max() - ys.min() + 1)]
        frame += rng.normal(0, 2.0, frame.shape)
        frame = np.clip(frame, 0, 255).astype(np.uint8)
        if a.gray:
            frame = cv2.cvtColor(cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY), cv2.COLOR_GRAY2BGR)
        # through JPEG, like the live view
        frame = cv2.imdecode(cv2.imencode('.jpg', frame, [cv2.IMWRITE_JPEG_QUALITY, 60])[1], cv2.IMREAD_COLOR)

        t0 = time.time()
        r = getattr(suggest_worker, a.method)(frame, [cx, cy, bw, bh])
        times.append((time.time() - t0) * 1000)
        rec = {k: r.get(k) for k in ('confident', 'reason', 'cover', 'sep', 'edges', 'area')}
        rec.update(negative=negative, iou=iou(r['box'], truth) if (truth and r.get('box')) else None,
                   visibility=None if negative else vis_score)
        records.append(rec)
        if negative:
            neg_conf += bool(r.get('confident'))
        else:
            pos_conf += bool(r.get('confident'))
            if r.get('confident'):
                ious.append(iou(r['box'], truth))
            k = 'clear' if vis_score >= 1.5 else 'moderate' if vis_score >= 0.7 else 'faint'
            bins[k].append((bool(r.get('confident')), iou(r['box'], truth) if r.get('confident') else 0.0))
        if a.show and len(sheet) < 12:
            vis = frame.copy()
            cv2.rectangle(vis, (cx - bw // 2, cy - bh // 2), (cx + bw // 2, cy + bh // 2), (255, 255, 255), 1)
            if truth:
                t = truth
                cv2.rectangle(vis, (int(t[0] - t[2] / 2), int(t[1] - t[3] / 2)), (int(t[0] + t[2] / 2), int(t[1] + t[3] / 2)), (255, 0, 0), 1)
            if r.get('contour'):
                ov = vis.copy(); cv2.fillPoly(ov, [np.array(r['contour'], np.int32)], (0, 200, 255)); vis = cv2.addWeighted(ov, 0.35, vis, 0.65, 0)
            if r.get('box'):
                b = r['box']; col = (0, 220, 0) if r.get('confident') else (0, 0, 255)
                cv2.rectangle(vis, (int(b[0] - b[2] / 2), int(b[1] - b[3] / 2)), (int(b[0] + b[2] / 2), int(b[1] + b[3] / 2)), col, 1)
            sheet.append(vis)

    n_pos = a.n - a.n // 4
    n_neg = a.n // 4
    ious = np.array(ious) if ious else np.array([0.0])
    print(f"{'gray' if a.gray else 'colour'} {W}x{H}: objects: confident {pos_conf}/{n_pos}, box IoU median {np.median(ious):.2f}, "
          f"IoU >= 0.5 in {int((ious >= 0.5).sum())}/{len(ious)}; empty boxes: wrongly confident {neg_conf}/{n_neg}; "
          f"{np.median(times):.0f} ms median")
    if a.dump:
        import json
        json.dump(records, open(a.dump, 'w'))
    for k, v in bins.items():
        if not v:
            continue
        conf = [x for x in v if x[0]]
        good = sum(1 for c, j in v if c and j >= 0.5)
        print(f"  {k:8s} objects: {len(v):3d}  confident {len(conf):3d}  right (confident, IoU >= 0.5) {good:3d} = {100 * good / len(v):3.0f}%"
              + (f"  median IoU {np.median([j for _, j in conf]):.2f}" if conf else ''))
    if a.show and sheet:
        rows = [np.hstack(sheet[k:k + 4]) for k in range(0, len(sheet) - len(sheet) % 4 or 4, 4) if len(sheet[k:k + 4]) == 4]
        if rows:
            cv2.imwrite(a.show, np.vstack(rows))


if __name__ == '__main__':
    main()
