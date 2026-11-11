"""Guesses which object the user means inside the lock-on box, and separates it from the
background. Runs as a long-lived helper of server.js (lib/suggest.js).

Protocol: one JSON object per line on stdin
    {"id": 1, "jpeg": "<base64>", "box": [cx, cy, w, h]}
and one per line on stdout
    {"id": 1, "ok": true, "box": [cx, cy, w, h], "contour": [[x, y], ...], "cover": 0.42,
     "score": 74, "level": "likely", "confident": true, "reason": null, "ms": 31}
Coordinates are frame pixels. "box" is the suggested tight box around the object; when nothing
clearly stands out ("confident": false), it is the input box and "reason" says why.

Method: learn what the background looks like from a ring around the box (brightness, colour
when there is any, and local texture, so it also works on grayscale frames), with a few k-means
clusters. Pixels in and just around the box that match none of them are the object; after a
cleanup, the piece that best covers the box centre is kept. Confidence comes from how much the
object differs from the ring, compared with how much the ring differs from itself.
"""
import base64
import json
import sys
import time

import cv2
import numpy as np

EXPAND = 1.9          # search area = box x EXPAND (the ring is what's outside the box in it)
ZONE = 1.25           # the object may stick out of the box by this much
MIN_COVER = 0.04      # object must fill at least 4% of the box
MAX_WIDTH = 400       # larger frames are halved first (speed; the result is scaled back)
SHOW_AT = 40          # score from which a suggestion is shown ("maybe")
LIKELY_AT = 70        # ... and called "likely"


def _fill_holes(m):
    h, w = m.shape
    flood = m.copy()
    ff = np.zeros((h + 2, w + 2), np.uint8)
    cv2.floodFill(flood, ff, (0, 0), 1)
    return m | (1 - flood)


def _quantize(roi, colour):
    """Per-pixel bin index for the histograms: brightness x colour, or brightness x texture."""
    lab = cv2.cvtColor(roi, cv2.COLOR_BGR2LAB)
    L = (lab[..., 0].astype(np.int32) * 16) >> 8                      # 16 levels
    if colour:
        a = np.clip((lab[..., 1].astype(np.int32) - 128 + 32) * 8 // 64, 0, 7)
        b = np.clip((lab[..., 2].astype(np.int32) - 128 + 32) * 8 // 64, 0, 7)
        return L * 64 + a * 8 + b, 16 * 64
    g = lab[..., 0].astype(np.float32)
    gx = cv2.Sobel(g, cv2.CV_32F, 1, 0, ksize=3); gy = cv2.Sobel(g, cv2.CV_32F, 0, 1, ksize=3)
    tex = cv2.GaussianBlur(np.sqrt(gx * gx + gy * gy), (0, 0), 1.5)
    t = np.clip((np.log1p(tex) * 2.0).astype(np.int32), 0, 7)       # 8 texture levels
    return L * 8 + t, 16 * 8


def suggest_core(img, box):
    """Likelihood ratio box-vs-ring on colour/texture histograms, refined with GrabCut."""
    H, W = img.shape[:2]
    cx, cy, bw, bh = box
    bw = max(8, min(int(bw), W)); bh = max(8, min(int(bh), H))
    rw, rh = int(bw * EXPAND), int(bh * EXPAND)
    x0 = int(max(0, cx - rw / 2)); y0 = int(max(0, cy - rh / 2))
    x1 = int(min(W, cx + rw / 2)); y1 = int(min(H, cy + rh / 2))
    roi = np.ascontiguousarray(img[y0:y1, x0:x1])
    if roi.shape[0] < 16 or roi.shape[1] < 16:
        return dict(reason='box too small')
    ccx, ccy = cx - x0, cy - y0
    yy, xx = np.mgrid[0:roi.shape[0], 0:roi.shape[1]]
    ux, uy = (xx - ccx) / (bw / 2), (yy - ccy) / (bh / 2)            # box = [-1, 1]^2
    in_box = (np.abs(ux) <= 1) & (np.abs(uy) <= 1)
    in_zone = (np.abs(ux) <= ZONE) & (np.abs(uy) <= ZONE)
    ring = ~((np.abs(ux) <= 1.15) & (np.abs(uy) <= 1.15))
    if ring.sum() < 200:
        return dict(reason='the box leaves too little background around it')

    lab = cv2.cvtColor(roi, cv2.COLOR_BGR2LAB)
    colour = float(lab[..., 1].std() + lab[..., 2].std()) > 2.0
    q, nb = _quantize(roi, colour)
    # inside: weighted towards the box centre (the user aims at the object)
    wk = np.clip(1.0 - (ux * ux + uy * uy) / 2.0, 0, 1) * in_box
    h_in = np.bincount(q.ravel(), weights=wk.ravel(), minlength=nb) + 1e-3
    h_out = np.bincount(q[ring].ravel(), minlength=nb).astype(np.float64) + 1e-3
    h_in /= h_in.sum(); h_out /= h_out.sum()
    P = (h_in / (h_in + h_out))[q].astype(np.float32)               # 0..1, > 0.5: more like the box
    P = cv2.GaussianBlur(P, (0, 0), 1.2)

    # GrabCut seeds from P
    gc = np.full(roi.shape[:2], cv2.GC_PR_BGD, np.uint8)
    gc[in_zone & (P > 0.5)] = cv2.GC_PR_FGD
    core = (np.abs(ux) <= 0.6) & (np.abs(uy) <= 0.6) & (P > 0.75)
    if core.sum() >= 12:
        gc[core] = cv2.GC_FGD
    gc[ring & (P < 0.45)] = cv2.GC_BGD
    edge = 2
    gc[:edge] = gc[-edge:] = cv2.GC_BGD; gc[:, :edge] = gc[:, -edge:] = cv2.GC_BGD
    if not ((gc == cv2.GC_FGD) | (gc == cv2.GC_PR_FGD)).any():
        return dict(reason='nothing stands out from the background')
    bgd = np.zeros((1, 65), np.float64); fgd = np.zeros((1, 65), np.float64)
    try:
        cv2.grabCut(roi, gc, None, bgd, fgd, 3, cv2.GC_INIT_WITH_MASK)
    except cv2.error:
        pass                                                         # keep the seeds as they are
    m = (((gc == cv2.GC_FGD) | (gc == cv2.GC_PR_FGD)) & in_zone).astype(np.uint8)
    m = cv2.morphologyEx(m, cv2.MORPH_OPEN, np.ones((3, 3), np.uint8))
    m = cv2.morphologyEx(m, cv2.MORPH_CLOSE, np.ones((5, 5), np.uint8))
    m = _fill_holes(m)

    n, labels, stats, cents = cv2.connectedComponentsWithStats(m, connectivity=8)
    best, best_score = 0, 0.0
    for i in range(1, n):
        comp = labels == i
        inside = float((comp & in_box).sum())
        if inside < 6:
            continue
        dist = np.hypot((cents[i][0] - ccx) / bw, (cents[i][1] - ccy) / bh)
        score = inside * (1.5 - min(dist, 1.0))
        if score > best_score:
            best, best_score = i, score
    if best == 0:
        return dict(reason='nothing stands out from the background')
    obj = (labels == best).astype(np.uint8)
    ob = obj.astype(bool)
    cover = float((ob & in_box).sum()) / float(in_box.sum())
    # separation: how much more box-like the object is than the background around it
    around = (cv2.dilate(obj, np.ones((11, 11), np.uint8)).astype(bool)) & ~ob
    sep = float(P[ob].mean() - (P[around].mean() if around.any() else 0.5))
    # edges: real objects have their outline on image edges
    g = cv2.cvtColor(roi, cv2.COLOR_BGR2GRAY).astype(np.float32)
    gx = cv2.Sobel(g, cv2.CV_32F, 1, 0, ksize=3); gy = cv2.Sobel(g, cv2.CV_32F, 0, 1, ksize=3)
    grad = np.sqrt(gx * gx + gy * gy)
    outline = (obj - cv2.erode(obj, np.ones((3, 3), np.uint8))).astype(bool)
    edge_ratio = float(grad[outline].mean() / (grad[in_zone].mean() + 1e-3)) if outline.any() else 0.0

    x, y, w, h = cv2.boundingRect(obj)
    zx0, zx1 = ccx - bw * ZONE / 2, ccx + bw * ZONE / 2
    zy0, zy1 = ccy - bh * ZONE / 2, ccy + bh * ZONE / 2
    touches = (x <= zx0 + 1) + (y <= zy0 + 1) + (x + w >= zx1 - 1) + (y + h >= zy1 - 1)
    contours, _ = cv2.findContours(obj, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
    c = max(contours, key=cv2.contourArea)
    c = cv2.approxPolyDP(c, 1.2, True).reshape(-1, 2)
    out = dict(
        box=[x0 + x + w / 2, y0 + y + h / 2, float(w), float(h)],
        contour=[[int(px + x0), int(py + y0)] for px, py in c],
        cover=round(cover, 3), sep=round(sep, 2), edges=round(edge_ratio, 2), area=int(obj.sum()),
    )
    if cover < MIN_COVER:
        out.update(reason='the object is tiny in the box')
    elif touches >= 3:
        out.update(reason='the object fills the box: make the box bigger, or step back')
    else:
        out.update(reason=None)
    return out


def _score(r):
    """0..100: how sure the suggestion is. Separation from the surroundings (60%) and an outline on
    real image edges (40%). Calibrated on test/suggest_test.py: at 40, the right object for about
    70% of clearly visible objects; at 70, rarely an outline in an empty box."""
    if r.get('sep') is None or r.get('edges') is None or r.get('reason') == 'the object is tiny in the box':
        return 0
    s = min(max((r['sep'] - 0.05) / 0.30, 0), 1) * 0.6 + min(max((r['edges'] - 0.8) / 0.8, 0), 1) * 0.4
    if r.get('reason') and 'fills the box' in r['reason']:
        s *= 0.5
    return int(round(100 * s))


def suggest(img, box):
    """The guess for the object in box [cx, cy, w, h]: box, contour, score 0-100, level
    ('likely' / 'maybe' / None), confident (score >= SHOW_AT) and a reason when it isn't."""
    scale = 1.0
    if img.shape[1] > MAX_WIDTH:
        scale = 0.5
        img = cv2.resize(img, (img.shape[1] // 2, img.shape[0] // 2), interpolation=cv2.INTER_AREA)
        box = [v * scale for v in box]
    r = suggest_core(img, box)
    r['score'] = _score(r)
    r['level'] = 'likely' if r['score'] >= LIKELY_AT else 'maybe' if r['score'] >= SHOW_AT else None
    r['confident'] = r['score'] >= SHOW_AT
    if not r['confident'] and not r.get('reason'):
        r['reason'] = 'the object barely stands out from the background'
    if scale != 1.0:
        if r.get('box'):
            r['box'] = [v / scale for v in r['box']]
        if r.get('contour'):
            r['contour'] = [[int(x / scale), int(y / scale)] for x, y in r['contour']]
        if r.get('area'):
            r['area'] = int(r['area'] / (scale * scale))
    return r


def main():
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        t0 = time.time()
        rid = None
        try:
            req = json.loads(line)
            rid = req.get('id')
            data = np.frombuffer(base64.b64decode(req['jpeg']), np.uint8)
            img = cv2.imdecode(data, cv2.IMREAD_COLOR)
            if img is None:
                raise ValueError('could not decode the frame')
            res = suggest(img, req['box'])
            if not res.get('box'):
                res['box'] = [float(v) for v in req['box']]
            res.update(id=rid, ok=True, frame=[img.shape[1], img.shape[0]])
        except Exception as e:  # report, keep running
            res = dict(id=rid, ok=False, error=str(e))
        res['ms'] = int((time.time() - t0) * 1000)
        sys.stdout.write(json.dumps(res) + '\n')
        sys.stdout.flush()


if __name__ == '__main__':
    main()
