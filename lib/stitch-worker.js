// Joins the board's 640x480 JPEG tiles into one JPEG, off the main thread.
// workerData: { width, height, tiles: [{ x, y, jpeg }], outW?, outH? }. width x height is the
// board's image and x/y the tiles' places in it. outW x outH is the wanted size (default the
// board's image).
//
// - Same size (the board's image is at most 15 pixels bigger, which is cropped from the right
//   and bottom): the tiles' compressed data is joined as it is (lib/jpeg-codec.js). There's
//   no second compression, so the pixels are exactly the board's, and the file is smaller
//   than the tiles together.
// - Smaller size: the tiles are decoded and joined, centre-cropped to the wanted aspect
//   ratio, shrunk with area averaging, and encoded once with the tiles' own quantization
//   tables (4:2:0), so the quality matches the board's.
import { parentPort, workerData } from 'node:worker_threads';
import jpeg from 'jpeg-js';
import { encodeRgba, joinTilesLossless, parseJpeg } from './jpeg-codec.js';

// How grainy a tile is at the pixel level: the mean difference between horizontally
// neighbouring green values. A frame whose Bayer colour pattern came through shifted by a
// pixel (pink cast, fine checker grain) scores several times more than its neighbours;
// normal tiles score about 1-3.
function checkerGrain(img) {
  const { width: w, height: h, data: d } = img;
  let sum = 0, n = 0;
  for (let y = 0; y < h; y += 2) {
    let i = y * w * 4 + 1;
    for (let x = 0; x < w - 1; x++, i += 4) {
      const diff = d[i + 4] - d[i];
      sum += diff < 0 ? -diff : diff;
      n++;
    }
  }
  return n ? sum / n : 0;
}

// Area-averaging resize of an RGBA region (sx, sy, sw, sh) of src (width srcW) to dw x dh.
// Each output pixel is the weighted mean of the source pixels it covers. For downscaling.
function resizeArea(src, srcW, sx, sy, sw, sh, dw, dh) {
  const weights = (sn, dn) => {
    const scale = sn / dn;
    const out = [];
    for (let d = 0; d < dn; d++) {
      const a = d * scale, b = Math.min(sn, (d + 1) * scale);
      const taps = [];
      for (let s = Math.floor(a); s < Math.ceil(b); s++) {
        const w = Math.min(b, s + 1) - Math.max(a, s);
        if (w > 0) taps.push([s, w / (b - a)]);
      }
      out.push(taps);
    }
    return out;
  };
  const wx = weights(sw, dw), wy = weights(sh, dh);
  const tmp = new Float32Array(dw * sh * 4);         // horizontal pass
  for (let y = 0; y < sh; y++) {
    const row = ((sy + y) * srcW + sx) * 4;
    for (let x = 0; x < dw; x++) {
      let r = 0, g = 0, b = 0;
      for (const [s, w] of wx[x]) {
        const i = row + s * 4;
        r += src[i] * w; g += src[i + 1] * w; b += src[i + 2] * w;
      }
      const o = (y * dw + x) * 4;
      tmp[o] = r; tmp[o + 1] = g; tmp[o + 2] = b;
    }
  }
  const out = new Uint8Array(dw * dh * 4);            // vertical pass
  for (let y = 0; y < dh; y++) {
    for (let x = 0; x < dw; x++) {
      let r = 0, g = 0, b = 0;
      for (const [s, w] of wy[y]) {
        const i = (s * dw + x) * 4;
        r += tmp[i] * w; g += tmp[i + 1] * w; b += tmp[i + 2] * w;
      }
      const o = (y * dw + x) * 4;
      out[o] = Math.round(r); out[o + 1] = Math.round(g); out[o + 2] = Math.round(b); out[o + 3] = 255;
    }
  }
  return out;
}

// Copies the region (sx, sy, w, h) of an RGBA image.
function cropRgba(src, srcW, sx, sy, w, h) {
  const out = new Uint8Array(w * h * 4);
  for (let y = 0; y < h; y++) out.set(src.subarray(((sy + y) * srcW + sx) * 4, ((sy + y) * srcW + sx + w) * 4), y * w * 4);
  return out;
}

try {
  const { width, height, tiles, outW = width, outH = height } = workerData;

  // Decode every tile: for the grain check, and for joining in pixels if that's needed.
  const out = new Uint8Array(width * height * 4);
  const grain = [];
  for (const t of tiles) {
    const img = jpeg.decode(t.jpeg, { useTArray: true, formatAsRGBA: true, maxMemoryUsageInMB: 256 });
    grain.push(checkerGrain(img));
    const x0 = Math.max(0, t.x);
    const x1 = Math.min(width, t.x + img.width);
    if (x1 <= x0) continue;
    for (let row = 0; row < img.height; row++) {
      const y = t.y + row;
      if (y < 0 || y >= height) continue;
      const src = (row * img.width + (x0 - t.x)) * 4;
      out.set(img.data.subarray(src, src + (x1 - x0) * 4), (y * width + x0) * 4);
    }
  }

  const sameSize = outW <= width && outH <= height && width - outW < 16 && height - outH < 16;
  let encoded = null, method = 're-encoded', note = null;
  if (sameSize) {
    try {
      encoded = joinTilesLossless(tiles, outW, outH);
      method = 'lossless';
    } catch (err) {
      note = err.message;                              // fall back to encoding the pixels
    }
  }
  if (!encoded) {
    let data, w = outW, h = outH;
    if (sameSize) {
      data = cropRgba(out, width, 0, 0, outW, outH);
    } else {
      // Centre crop to the target's aspect ratio, then scale down to the target size.
      let cw = width, ch = height;
      if (width * outH > height * outW) cw = Math.round((height * outW) / outH);
      else ch = Math.round((width * outH) / outW);
      if (cw < outW || ch < outH) throw new Error(`Cannot scale ${cw}x${ch} up to ${outW}x${outH}`);
      data = resizeArea(out, width, (width - cw) >> 1, (height - ch) >> 1, cw, ch, outW, outH);
    }
    const { qt } = parseJpeg(tiles[0].jpeg, { coefficients: false });
    encoded = encodeRgba(data, w, h, { 0: qt[0], 1: qt[1] ?? qt[0] });
  }
  parentPort.postMessage({ jpeg: encoded, grain, method, note });
} catch (err) {
  parentPort.postMessage({ error: err.message });
}
