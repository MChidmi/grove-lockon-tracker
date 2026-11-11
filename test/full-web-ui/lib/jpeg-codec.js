// Small baseline-JPEG toolkit for joining the board's tiles.
//
// - joinTilesLossless(): puts tiles together without decoding them to pixels. It copies their
//   quantized DCT blocks into one image and re-does only the Huffman coding, with tables
//   optimized for this image. The pixels are exactly the board's, and the file is usually a
//   little smaller than the tiles together.
// - encodeRgba(): a plain encoder (4:2:0, optimized Huffman tables) for images that had to be
//   scaled. It takes the quantization tables to use, so scaled images keep the same quality as
//   the board's tiles.
//
// Only what the WE2's encoder produces is supported: 8-bit baseline, one interleaved scan, no
// restart intervals. Anything else throws, and the caller falls back to re-encoding.

// ZIGZAG[k] = position (row * 8 + column) of the k-th coefficient in zigzag order.
const ZIGZAG = Uint8Array.from([
  0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
  35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
]);

// ---- decoding (to quantized coefficients) -----------------------------------------------

function huffDecoder(counts, symbols) {
  const maxcode = new Int32Array(17).fill(-1);
  const valptr = new Int32Array(17);
  const mincode = new Int32Array(17);
  let code = 0, k = 0;
  for (let len = 1; len <= 16; len++) {
    valptr[len] = k;
    mincode[len] = code;
    code += counts[len - 1];
    k += counts[len - 1];
    if (counts[len - 1]) maxcode[len] = code - 1;
    code <<= 1;
  }
  return { maxcode, valptr, mincode, symbols };
}

class BitReader {
  constructor(u8, pos) {
    this.u8 = u8;
    this.pos = pos;
    this.acc = 0;
    this.n = 0;
  }

  bit() {
    if (this.n === 0) {
      let b = this.u8[this.pos];
      if (b === 0xff) {
        const next = this.u8[this.pos + 1];
        if (next === 0x00) this.pos += 2;            // stuffed 0xFF
        else b = 0;                                  // a marker: the data ended, read zeros
      } else {
        this.pos++;
      }
      this.acc = b;
      this.n = 8;
    }
    this.n--;
    return (this.acc >> this.n) & 1;
  }

  bits(count) {
    let v = 0;
    for (let i = 0; i < count; i++) v = (v << 1) | this.bit();
    return v;
  }

  symbol(t) {
    let code = 0;
    for (let len = 1; len <= 16; len++) {
      code = (code << 1) | this.bit();
      if (code <= t.maxcode[len]) return t.symbols[t.valptr[len] + code - t.mincode[len]];
    }
    throw new Error('Bad Huffman code');
  }
}

const extend = (v, t) => (v < 1 << (t - 1) ? v - (1 << t) + 1 : v);

// Reads a baseline JPEG. With coefficients = false it stops after the tables and frame header.
// Returns { width, height, qt: {id: Uint8Array(64) in zigzag order}, comps: [{ id, h, v, tq,
// bw, bh, blocks: Int16Array(bw * bh * 64) in zigzag order }] }.
export function parseJpeg(buf, { coefficients = true } = {}) {
  const u8 = buf instanceof Uint8Array ? buf : new Uint8Array(buf);
  if (u8[0] !== 0xff || u8[1] !== 0xd8) throw new Error('Not a JPEG');
  const qt = {}, dcT = {}, acT = {};
  let frame = null;
  let p = 2;
  while (p < u8.length) {
    if (u8[p] !== 0xff) throw new Error(`Bad marker at ${p}`);
    const m = u8[p + 1];
    if (m === 0xff) { p++; continue; }
    if (m === 0xd9) break;
    const len = (u8[p + 2] << 8) | u8[p + 3];
    const seg = u8.subarray(p + 4, p + 2 + len);
    if (m === 0xdb) {
      for (let i = 0; i < seg.length; i += 65) {
        if (seg[i] >> 4) throw new Error('16-bit quantization tables are not supported');
        qt[seg[i] & 15] = seg.slice(i + 1, i + 65);
      }
    } else if (m === 0xc4) {
      for (let i = 0; i < seg.length;) {
        const counts = seg.slice(i + 1, i + 17);
        const n = counts.reduce((a, b) => a + b, 0);
        (seg[i] >> 4 ? acT : dcT)[seg[i] & 15] = huffDecoder(counts, seg.slice(i + 17, i + 17 + n));
        i += 17 + n;
      }
    } else if (m === 0xc0 || m === 0xc1) {
      if (seg[0] !== 8) throw new Error('Only 8-bit JPEGs are supported');
      const comps = [];
      for (let k = 0; k < seg[5]; k++) {
        comps.push({ id: seg[6 + 3 * k], h: seg[7 + 3 * k] >> 4, v: seg[7 + 3 * k] & 15, tq: seg[8 + 3 * k] });
      }
      frame = { height: (seg[1] << 8) | seg[2], width: (seg[3] << 8) | seg[4], comps };
    } else if ((m >= 0xc2 && m <= 0xcf) && m !== 0xc4 && m !== 0xc8 && m !== 0xcc) {
      throw new Error('Only baseline JPEGs are supported');
    } else if (m === 0xdd) {
      if ((seg[0] << 8) | seg[1]) throw new Error('Restart intervals are not supported');
    } else if (m === 0xda) {
      if (!frame) throw new Error('Scan before frame header');
      const out = { width: frame.width, height: frame.height, qt, comps: frame.comps };
      if (!coefficients) return out;
      if (seg[0] !== frame.comps.length) throw new Error('Only single-scan JPEGs are supported');
      const sel = [];
      for (let k = 0; k < seg[0]; k++) {
        const c = frame.comps.find((x) => x.id === seg[1 + 2 * k]);
        sel.push({ c, dc: dcT[seg[2 + 2 * k] >> 4], ac: acT[seg[2 + 2 * k] & 15] });
      }
      decodeScan(u8, p + 2 + len, out, sel);
      return out;
    }
    p += 2 + len;
  }
  throw new Error('No image data');
}

function decodeScan(u8, pos, img, sel) {
  const hmax = Math.max(...img.comps.map((c) => c.h));
  const vmax = Math.max(...img.comps.map((c) => c.v));
  const mcux = Math.ceil(img.width / (8 * hmax));
  const mcuy = Math.ceil(img.height / (8 * vmax));
  for (const c of img.comps) {
    c.bw = mcux * c.h;
    c.bh = mcuy * c.v;
    c.blocks = new Int16Array(c.bw * c.bh * 64);
  }
  const br = new BitReader(u8, pos);
  const pred = new Int32Array(sel.length);
  for (let my = 0; my < mcuy; my++) {
    for (let mx = 0; mx < mcux; mx++) {
      for (let s = 0; s < sel.length; s++) {
        const { c, dc, ac } = sel[s];
        for (let by = 0; by < c.v; by++) {
          for (let bx = 0; bx < c.h; bx++) {
            const o = ((my * c.v + by) * c.bw + mx * c.h + bx) * 64;
            const t = br.symbol(dc);
            pred[s] += t ? extend(br.bits(t), t) : 0;
            c.blocks[o] = pred[s];
            for (let k = 1; k < 64;) {
              const rs = br.symbol(ac);
              const r = rs >> 4, sz = rs & 15;
              if (!sz) {
                if (r !== 15) break;                 // end of block
                k += 16;
                continue;
              }
              k += r;
              if (k > 63) throw new Error('Corrupt JPEG data');
              c.blocks[o + k] = extend(br.bits(sz), sz);
              k++;
            }
          }
        }
      }
    }
  }
}

// ---- encoding (from quantized coefficients) -----------------------------------------------

// Optimal Huffman code lengths for the symbol counts (JPEG spec Annex K.2, as libjpeg does it).
function optimalTable(freqIn) {
  const freq = Int32Array.from(freqIn);
  if (!freq.some((f) => f)) freq[0] = 1;            // the table must have at least one code
  freq[256] = 1;                                    // reserved, so no code is all ones
  const codesize = new Int32Array(257);
  const others = new Int32Array(257).fill(-1);
  for (;;) {
    let c1 = -1, c2 = -1, v = Infinity;
    for (let i = 0; i <= 256; i++) if (freq[i] && freq[i] <= v) { v = freq[i]; c1 = i; }
    v = Infinity;
    for (let i = 0; i <= 256; i++) if (freq[i] && freq[i] <= v && i !== c1) { v = freq[i]; c2 = i; }
    if (c2 < 0) break;
    freq[c1] += freq[c2];
    freq[c2] = 0;
    codesize[c1]++;
    while (others[c1] >= 0) { c1 = others[c1]; codesize[c1]++; }
    others[c1] = c2;
    codesize[c2]++;
    while (others[c2] >= 0) { c2 = others[c2]; codesize[c2]++; }
  }
  const bits = new Int32Array(33);
  for (let i = 0; i <= 256; i++) if (codesize[i]) bits[codesize[i]]++;
  for (let i = 32; i > 16; i--) {                   // limit code lengths to 16 bits
    while (bits[i] > 0) {
      let j = i - 2;
      while (bits[j] === 0) j--;
      bits[i] -= 2;
      bits[i - 1]++;
      bits[j + 1] += 2;
      bits[j]--;
    }
  }
  let i = 16;
  while (bits[i] === 0) i--;
  bits[i]--;                                        // drop the reserved symbol's code
  const symbols = [];
  for (let len = 1; len <= 32; len++) for (let s = 0; s < 256; s++) if (codesize[s] === len) symbols.push(s);
  const counts = Uint8Array.from(bits.slice(1, 17));
  // Canonical codes for writing.
  const code = new Uint16Array(256), size = new Uint8Array(256);
  let c = 0, k = 0;
  for (let len = 1; len <= 16; len++) {
    for (let n = 0; n < counts[len - 1]; n++) {
      code[symbols[k]] = c++;
      size[symbols[k]] = len;
      k++;
    }
    c <<= 1;
  }
  return { counts, symbols: Uint8Array.from(symbols), code, size };
}

class BitWriter {
  constructor(capacity) {
    this.buf = new Uint8Array(Math.max(capacity, 4096));
    this.len = 0;
    this.acc = 0;
    this.n = 0;
  }

  byte(b) {
    if (this.len + 2 > this.buf.length) {
      const bigger = new Uint8Array(this.buf.length * 2);
      bigger.set(this.buf);
      this.buf = bigger;
    }
    this.buf[this.len++] = b;
    if (b === 0xff) this.buf[this.len++] = 0;       // byte stuffing
  }

  put(value, count) {
    for (let i = count - 1; i >= 0; i--) {
      this.acc = (this.acc << 1) | ((value >> i) & 1);
      if (++this.n === 8) {
        this.byte(this.acc);
        this.acc = 0;
        this.n = 0;
      }
    }
  }

  flush() {
    if (this.n) this.put(0x7f, 8 - this.n);        // pad with 1 bits
    return this.buf.subarray(0, this.len);
  }
}

const category = (v) => {
  let a = v < 0 ? -v : v, n = 0;
  while (a) { n++; a >>= 1; }
  return n;
};

// Walks every block in MCU order, calling fn(compIndex, blocks, offset).
function forEachBlock(img, fn) {
  const hmax = Math.max(...img.comps.map((c) => c.h));
  const vmax = Math.max(...img.comps.map((c) => c.v));
  const mcux = Math.ceil(img.width / (8 * hmax));
  const mcuy = Math.ceil(img.height / (8 * vmax));
  for (let my = 0; my < mcuy; my++) {
    for (let mx = 0; mx < mcux; mx++) {
      img.comps.forEach((c, ci) => {
        for (let by = 0; by < c.v; by++) {
          for (let bx = 0; bx < c.h; bx++) fn(ci, c.blocks, ((my * c.v + by) * c.bw + mx * c.h + bx) * 64);
        }
      });
    }
  }
}

// Writes a baseline JPEG from quantized coefficients (zigzag order), with optimized Huffman
// tables. img: { width, height, qt, comps: [{ id, h, v, tq, bw, bh, blocks }] }. The first
// component uses Huffman tables 0, the others tables 1.
export function encodeJpeg(img) {
  const tableOf = (ci) => (ci === 0 ? 0 : 1);
  const dcFreq = [new Int32Array(257), new Int32Array(257)];
  const acFreq = [new Int32Array(257), new Int32Array(257)];
  const pred = new Int32Array(img.comps.length);
  forEachBlock(img, (ci, b, o) => {
    const t = tableOf(ci);
    dcFreq[t][category(b[o] - pred[ci])]++;
    pred[ci] = b[o];
    let run = 0;
    for (let k = 1; k < 64; k++) {
      const v = b[o + k];
      if (!v) { run++; continue; }
      while (run > 15) { acFreq[t][0xf0]++; run -= 16; }
      acFreq[t][(run << 4) | category(v)]++;
      run = 0;
    }
    if (run) acFreq[t][0x00]++;
  });
  const used = img.comps.length > 1 ? [0, 1] : [0];
  const dc = used.map((t) => optimalTable(dcFreq[t]));
  const ac = used.map((t) => optimalTable(acFreq[t]));

  const out = [];
  const seg = (marker, body) => {
    out.push(Uint8Array.of(0xff, marker, (body.length + 2) >> 8, (body.length + 2) & 0xff), body);
  };
  out.push(Uint8Array.of(0xff, 0xd8));
  seg(0xe0, Uint8Array.of(0x4a, 0x46, 0x49, 0x46, 0, 1, 1, 0, 0, 1, 0, 1, 0, 0));   // JFIF 1.01
  const tqs = [...new Set(img.comps.map((c) => c.tq))];
  seg(0xdb, Uint8Array.from(tqs.flatMap((tq) => [tq, ...img.qt[tq]])));
  seg(0xc0, Uint8Array.from([8, img.height >> 8, img.height & 0xff, img.width >> 8, img.width & 0xff,
    img.comps.length, ...img.comps.flatMap((c) => [c.id, (c.h << 4) | c.v, c.tq])]));
  const dht = [];
  used.forEach((t) => dht.push(0x00 | t, ...dc[t].counts, ...dc[t].symbols));
  used.forEach((t) => dht.push(0x10 | t, ...ac[t].counts, ...ac[t].symbols));
  seg(0xc4, Uint8Array.from(dht));
  seg(0xda, Uint8Array.from([img.comps.length, ...img.comps.flatMap((c, ci) => [c.id, (tableOf(ci) << 4) | tableOf(ci)]), 0, 63, 0]));

  const bw = new BitWriter(img.comps.reduce((a, c) => a + c.blocks.length, 0) / 4);
  pred.fill(0);
  forEachBlock(img, (ci, b, o) => {
    const D = dc[tableOf(ci)], A = ac[tableOf(ci)];
    const diff = b[o] - pred[ci];
    pred[ci] = b[o];
    const s = category(diff);
    bw.put(D.code[s], D.size[s]);
    if (s) bw.put(diff < 0 ? diff + (1 << s) - 1 : diff, s);
    let run = 0;
    for (let k = 1; k < 64; k++) {
      const v = b[o + k];
      if (!v) { run++; continue; }
      while (run > 15) { bw.put(A.code[0xf0], A.size[0xf0]); run -= 16; }
      const z = category(v), sym = (run << 4) | z;
      bw.put(A.code[sym], A.size[sym]);
      bw.put(v < 0 ? v + (1 << z) - 1 : v, z);
      run = 0;
    }
    if (run) bw.put(A.code[0], A.size[0]);
  });
  out.push(bw.flush(), Uint8Array.of(0xff, 0xd9));
  return Buffer.concat(out);
}

// ---- joining tiles without re-compressing --------------------------------------------------

// tiles: [{ x, y, jpeg }] with x/y the tile's place in the result. Every tile must sit on the
// MCU grid (multiples of 16 for 4:2:0) and share the same tables and colour sampling, and
// together they must cover outW x outH. Later tiles win where tiles overlap.
export function joinTilesLossless(tiles, outW, outH) {
  const parsed = tiles.map((t) => ({ ...t, img: parseJpeg(t.jpeg) }));
  const first = parsed[0].img;
  const hmax = Math.max(...first.comps.map((c) => c.h));
  const vmax = Math.max(...first.comps.map((c) => c.v));
  const mw = 8 * hmax, mh = 8 * vmax;
  const sig = (img) => JSON.stringify(img.comps.map((c) => [c.id, c.h, c.v, [...img.qt[c.tq]]]));
  for (const t of parsed) {
    if (sig(t.img) !== sig(first)) throw new Error('Tiles use different JPEG tables or sampling');
    if (t.x % mw || t.y % mh) throw new Error(`Tile at ${t.x},${t.y} is off the ${mw}x${mh} block grid`);
  }
  const mcux = Math.ceil(outW / mw), mcuy = Math.ceil(outH / mh);
  const comps = first.comps.map((c) => ({
    id: c.id, h: c.h, v: c.v, tq: c.tq, bw: mcux * c.h, bh: mcuy * c.v,
    blocks: new Int16Array(mcux * c.h * mcuy * c.v * 64),
    covered: new Uint8Array(mcux * c.h * mcuy * c.v),
  }));
  for (const t of parsed) {
    t.img.comps.forEach((src, ci) => {
      const dst = comps[ci];
      const ox = (t.x / mw) * src.h, oy = (t.y / mh) * src.v;
      for (let by = 0; by < src.bh; by++) {
        const dy = oy + by;
        if (dy >= dst.bh) break;
        for (let bx = 0; bx < src.bw; bx++) {
          const dx = ox + bx;
          if (dx >= dst.bw) break;
          const d = dy * dst.bw + dx;
          dst.blocks.set(src.blocks.subarray((by * src.bw + bx) * 64, (by * src.bw + bx + 1) * 64), d * 64);
          dst.covered[d] = 1;
        }
      }
    });
  }
  for (const c of comps) if (c.covered.includes(0)) throw new Error('The tiles do not cover the whole image');
  return encodeJpeg({ width: outW, height: outH, qt: first.qt, comps });
}

// ---- encoding from pixels ----------------------------------------------------------------

const COS = (() => {
  const t = new Float64Array(64);                   // COS[u * 8 + x] = C(u)/2 * cos((2x+1)u*pi/16)
  for (let u = 0; u < 8; u++) {
    for (let x = 0; x < 8; x++) t[u * 8 + x] = (u ? 0.5 : Math.SQRT1_2 / 2) * Math.cos(((2 * x + 1) * u * Math.PI) / 16);
  }
  return t;
})();

// Forward DCT of an 8x8 block (natural order, level-shifted), quantized into zigzag order.
function fdctQuantize(blk, q, out, o) {
  const tmp = new Float64Array(64);
  for (let y = 0; y < 8; y++) {
    for (let u = 0; u < 8; u++) {
      let s = 0;
      for (let x = 0; x < 8; x++) s += COS[u * 8 + x] * blk[y * 8 + x];
      tmp[y * 8 + u] = s;
    }
  }
  for (let k = 0; k < 64; k++) {
    const n = ZIGZAG[k], v = n >> 3, u = n & 7;
    let s = 0;
    for (let y = 0; y < 8; y++) s += COS[v * 8 + y] * tmp[y * 8 + u];
    let c = Math.round(s / q[k]);
    const lim = k ? 1023 : 2047;
    out[o + k] = c > lim ? lim : c < -lim ? -lim : c;
  }
}

// Encodes RGBA pixels as a 4:2:0 baseline JPEG with the given quantization tables
// (qt[0] for brightness, qt[1] for colour, zigzag order).
export function encodeRgba(rgba, width, height, qt) {
  const mcux = Math.ceil(width / 16), mcuy = Math.ceil(height / 16);
  const W = mcux * 16, H = mcuy * 16;
  const Y = new Float32Array(W * H), Cb = new Float32Array(W * H), Cr = new Float32Array(W * H);
  for (let y = 0; y < H; y++) {
    const sy = Math.min(y, height - 1);
    for (let x = 0; x < W; x++) {
      const i = (sy * width + Math.min(x, width - 1)) * 4;
      const r = rgba[i], g = rgba[i + 1], b = rgba[i + 2], o = y * W + x;
      Y[o] = 0.299 * r + 0.587 * g + 0.114 * b - 128;
      Cb[o] = -0.168736 * r - 0.331264 * g + 0.5 * b;
      Cr[o] = 0.5 * r - 0.418688 * g - 0.081312 * b;
    }
  }
  const comps = [
    { id: 1, h: 2, v: 2, tq: 0, bw: mcux * 2, bh: mcuy * 2 },
    { id: 2, h: 1, v: 1, tq: 1, bw: mcux, bh: mcuy },
    { id: 3, h: 1, v: 1, tq: 1, bw: mcux, bh: mcuy },
  ];
  for (const c of comps) c.blocks = new Int16Array(c.bw * c.bh * 64);
  const blk = new Float64Array(64);
  for (let by = 0; by < mcuy * 2; by++) {
    for (let bx = 0; bx < mcux * 2; bx++) {
      for (let y = 0; y < 8; y++) for (let x = 0; x < 8; x++) blk[y * 8 + x] = Y[(by * 8 + y) * W + bx * 8 + x];
      fdctQuantize(blk, qt[0], comps[0].blocks, (by * comps[0].bw + bx) * 64);
    }
  }
  for (const [ci, plane] of [[1, Cb], [2, Cr]]) {
    for (let by = 0; by < mcuy; by++) {
      for (let bx = 0; bx < mcux; bx++) {
        for (let y = 0; y < 8; y++) {
          for (let x = 0; x < 8; x++) {
            const o = (by * 16 + y * 2) * W + bx * 16 + x * 2;     // 2x2 average
            blk[y * 8 + x] = (plane[o] + plane[o + 1] + plane[o + W] + plane[o + W + 1]) / 4;
          }
        }
        fdctQuantize(blk, qt[1], comps[ci].blocks, (by * comps[ci].bw + bx) * 64);
      }
    }
  }
  return encodeJpeg({ width, height, qt: { 0: qt[0], 1: qt[1] }, comps });
}
