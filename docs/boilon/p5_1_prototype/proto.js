'use strict';
// Host prototype for P5-1. Every screen is drawn pixel by pixel into a 240x296
// buffer with the device's own 14px 1-bit Noto CJK glyphs and LVGL's unscii-8.
// All colors are quantized to RGB565 so the preview matches what the panel can show.
const W = 240, H = 296;

// ---------- color ----------
const colorCache = new Map(), wireCache = new Map();
// The RGB565 value the panel receives.
function v565(hex) {
  let v = wireCache.get(hex);
  if (v !== undefined) return v;
  const n = parseInt(hex.slice(1), 16);
  v = (Math.round(((n >> 16) & 255) * 31 / 255) << 11) | (Math.round(((n >> 8) & 255) * 63 / 255) << 5) |
    Math.round((n & 255) * 31 / 255);
  wireCache.set(hex, v);
  return v;
}
function c565(hex) {
  let c = colorCache.get(hex);
  if (c !== undefined) return c;
  const n = parseInt(hex.slice(1), 16);
  const r5 = Math.round(((n >> 16) & 255) * 31 / 255);
  const g6 = Math.round(((n >> 8) & 255) * 63 / 255);
  const b5 = Math.round((n & 255) * 31 / 255);
  const r = Math.round(r5 * 255 / 31), g = Math.round(g6 * 255 / 63), b = Math.round(b5 * 255 / 31);
  c = ((255 << 24) | (b << 16) | (g << 8) | r) >>> 0;
  colorCache.set(hex, c);
  return c;
}

// ---------- fonts ----------
function decodeFont(f) {
  const g = {};
  for (const [ch, [adv, w, h, x, y, hex]] of Object.entries(f.g)) {
    const n = w * h, bits = new Uint8Array(n);
    let bin = '';
    for (const d of hex) bin += parseInt(d, 16).toString(2).padStart(4, '0');
    bin = bin.slice(bin.length - n);
    for (let i = 0; i < n; i++) bits[i] = bin.charCodeAt(i) === 49 ? 1 : 0;
    g[ch] = { adv, w, h, x, y, bits };
  }
  return { lh: f.lh, base: f.base, g };
}
const NOTO = decodeFont(FONT_DATA.noto);
const PXF = decodeFont(FONT_DATA.px);

// ---------- pixel buffer ----------
class Px {
  constructor(canvas) {
    canvas.width = W;
    canvas.height = H;
    this.ctx = canvas.getContext('2d');
    this.img = this.ctx.createImageData(W, H);
    this.u = new Uint32Array(this.img.data.buffer);
    this.v = new Uint16Array(W * H);
    this.clip = null;
  }
  flush() { this.ctx.putImageData(this.img, 0, 0); }
  clear(hex) { this.u.fill(c565(hex)); this.v.fill(v565(hex)); }
  dot(x, y, hex) {
    x |= 0; y |= 0;
    if (x < 0 || y < 0 || x >= W || y >= H) return;
    const k = this.clip;
    if (k && (x < k[0] || y < k[1] || x >= k[2] || y >= k[3])) return;
    this.u[y * W + x] = c565(hex);
    this.v[y * W + x] = v565(hex);
  }
  rect(x, y, w, h, hex) {
    const c = c565(hex), v = v565(hex), k = this.clip || [0, 0, W, H];
    const x0 = Math.max(k[0], x | 0), y0 = Math.max(k[1], y | 0);
    const x1 = Math.min(k[2], (x + w) | 0), y1 = Math.min(k[3], (y + h) | 0);
    if (x1 <= x0) return;
    for (let yy = y0; yy < y1; yy++) { this.u.fill(c, yy * W + x0, yy * W + x1); this.v.fill(v, yy * W + x0, yy * W + x1); }
  }
  frame(x, y, w, h, hex, t = 2) {
    this.rect(x, y, w, t, hex); this.rect(x, y + h - t, w, t, hex);
    this.rect(x, y, t, h, hex); this.rect(x + w - t, y, t, h, hex);
  }
  // Rectangle with its r x r corners left untouched: a one-step pixel rounding.
  round(x, y, w, h, hex, r = 2) {
    this.rect(x + r, y, w - 2 * r, h, hex);
    this.rect(x, y + r, r, h - 2 * r, hex);
    this.rect(x + w - r, y + r, r, h - 2 * r, hex);
  }
  panel(x, y, w, h, o) {
    const t = o.t ?? 2, s = o.s ?? 2;
    if (o.shadow) this.round(x + s, y + s, w, h, o.shadow, t);
    this.round(x, y, w, h, o.line, t);
    this.rect(x + t, y + t, w - 2 * t, h - 2 * t, o.fill);
  }
  // Dotted 2px rule, the cheapest pixel divider.
  dash(x, y, w, hex, on = 2, off = 2) {
    for (let i = 0; i < w; i += on + off) this.rect(x + i, y, Math.min(on, w - i), 2, hex);
  }
}

// ---------- text ----------
// Characters outside the installed font draw as an empty box (tofu), not '?'.
const TOFU = (() => {
  const w = 10, h = 12, bits = new Uint8Array(w * h);
  for (let r = 0; r < h; r++) for (let c = 0; c < w; c++) bits[r * w + c] = r === 0 || r === h - 1 || c === 0 || c === w - 1 ? 1 : 0;
  return { adv: 14, w, h, x: 2, y: 0, bits };
})();
function glyph(font, ch) { return font.g[ch] || (font === NOTO ? TOFU : font.g['?']); }
function measure(str, o = {}) {
  const font = o.font || NOTO, s = o.s || 1;
  let w = 0;
  for (const ch of str) w += glyph(font, ch).adv * s;
  return w;
}
function layout(str, o) {
  const font = o.font || NOTO, s = o.s || 1, maxW = o.maxW ?? 1e9, lines = o.lines ?? 1;
  const out = [];
  let cur = [], w = 0;
  for (const ch of str) {
    const a = glyph(font, ch).adv * s;
    if (cur.length && w + a > maxW) { out.push(cur); cur = []; w = 0; }
    cur.push(ch);
    w += a;
  }
  if (cur.length) out.push(cur);
  if (out.length > lines) {
    out.length = lines;
    const last = out[lines - 1];
    const ew = glyph(font, '…').adv * s;
    while (last.length && measure(last.join(''), o) + ew > maxW) last.pop();
    last.push('…');
  }
  return out.map((l) => l.join(''));
}
// Draws like the device: glyph rows start at top + (line_height - base_line - box_h - ofs_y).
function text(px, str, x, y, o = {}) {
  const font = o.font || NOTO, s = o.s || 1;
  const lh = o.lh || (font === NOTO ? (s === 1 ? 20 : 30) : 9 * s + 4);
  const lines = layout(String(str), o);
  lines.forEach((line, li) => {
    const lw = measure(line, o);
    let cx = o.align === 'center' ? x - Math.floor(lw / 2) : o.align === 'right' ? x - lw : x;
    const top = y + li * lh;
    for (const ch of line) {
      const g = glyph(font, ch);
      const oy = top + (font.lh - font.base - g.h - g.y) * s;
      for (let r = 0; r < g.h; r++)
        for (let c = 0; c < g.w; c++)
          if (g.bits[r * g.w + c]) {
            if (s === 1) px.dot(cx + g.x + c, oy + r, o.color);
            else px.rect(cx + (g.x + c) * s, oy + r * s, s, s, o.color);
          }
      cx += g.adv * s;
    }
  });
  return lines.length;
}
const pxt = (px, str, x, y, s, color, align) => text(px, str, x, y, { font: PXF, s, color, align });

// ---------- sprites and icons ----------
function sprite(px, rows, map, x, y, s = 1) {
  rows.forEach((row, r) => {
    for (let c = 0; c < row.length; c++) {
      const k = row[c];
      if (k === '.' || k === ' ') continue;
      const col = map[k];
      if (col) px.rect(x + c * s, y + r * s, s, s, col);
    }
  });
}
const ICONS = {
  play: ['#......', '###....', '#####..', '#######', '#####..', '###....', '#......'],
  pause: ['##..##', '##..##', '##..##', '##..##', '##..##', '##..##', '##..##'],
  stop: ['######', '######', '######', '######', '######', '######'],
  hourglass: ['#######', '#.....#', '.#...#.', '..#.#..', '.#.#.#.', '#.###.#', '#######'],
  done: ['......#', '.....##', '#...##.', '##.##..', '.###...', '..#....'],
  warn: ['....#....', '...#.#...', '...#.#...', '..#.#.#..', '..#.#.#..', '.#.....#.', '.#..#..#.', '#########'],
  folder: ['###......', '#########', '#.......#', '#.......#', '#.......#', '#########'],
  speaker: ['...#.....', '..##...#.', '####.#..#', '####.#..#', '####.#..#', '..##...#.', '...#.....'],
  mute: ['...#.....', '..##.....', '####.#.#.', '####..#..', '####.#.#.', '..##.....', '...#.....'],
  cat: ['#.......#', '##.....##', '#########', '#########', '##.###.##', '#########', '.#######.'],
  wifi: ['.#######.', '#.......#', '..#####..', '.#.....#.', '...###...', '.........', '....#....'],
  sd: ['...######', '..#.#.#.#', '.#..#.#.#', '#...#.#.#', '#.......#', '#.......#', '#.......#', '#.......#', '#.......#', '#########'],
  box: ['#.......#', '##.....##', '#.#####.#', '#.......#', '#.......#', '#.......#', '#########'],
  moon: ['...####..', '.###.....', '.##......', '##.......', '##.......', '##.......', '.##......', '.###.....', '...####..'],
  ff: ['#....#.....', '##...##....', '###..###...', '####.####..', '###..###...', '##...##....', '#....#.....'],
  rew: ['.....#....#', '....##...##', '...###..###', '..####.####', '...###..###', '....##...##', '.....#....#'],
  batt: ['#'],
  note: ['..###', '..#.#', '..#..', '..#..', '###..', '###..'],
  key: ['.###.', '#...#', '#.#.#', '#...#', '.###.'],
  // Play modes (2026-10-15), in the status bar when not in order.
  loop: ['......#..', '########.', '#.....#..', '#.......#', '..#.....#', '.########', '..#......'],
  single: ['......#....', '########..#', '#.....#..##', '#.......#.#', '..#.....#.#', '.########.#', '..#........'],
  shuffle: ['.......#.', '##...####', '..#.#..#.', '...#.....', '..#.#..#.', '##...####', '.......#.'],
};
function icon(px, name, x, y, s, color) { sprite(px, ICONS[name], { '#': color }, x, y, s); }
function outlined(rows) {
  const h = rows.length, w = rows[0].length, out = [];
  const on = (x, y) => y >= 0 && y < h && x >= 0 && x < w && rows[y][x] !== '.';
  for (let y = -1; y <= h; y++) {
    let line = '';
    for (let x = -1; x <= w; x++)
      line += on(x, y) ? rows[y][x] : on(x - 1, y) || on(x + 1, y) || on(x, y - 1) || on(x, y + 1) ? 'k' : '.';
    out.push(line);
  }
  return out;
}
const BADGE = outlined(['w.......w', 'ww.....ww', 'wwwwwwwww', 'wweoooeww', 'wwwwpwwww', '.wwwwwww.'].map((r) => r.replace(/o/g, 'w')));
function badge(px, x, y, s = 2) { sprite(px, BADGE, CAT_COLORS, x, y, s); }
function iconSize(name, s) { return [ICONS[name][0].length * s, ICONS[name].length * s]; }

// 2px-stroke battery; four bars, the last one red when low.
function battery(px, th, pct, x, y) {
  const low = pct >= 0 && pct <= 15;
  const line = low ? th.red : th.text;
  px.round(x, y, 22, 12, line, 1);
  px.rect(x + 2, y + 2, 18, 8, th.bg);
  px.rect(x + 22, y + 3, 2, 6, line);
  const bars = pct < 0 ? 0 : Math.max(pct > 0 ? 1 : 0, Math.ceil(pct / 25));
  for (let i = 0; i < 4; i++)
    px.rect(x + 3 + i * 4, y + 3, 3, 6, i < bars ? (low ? th.red : th.green) : th.faint);
  const label = pct < 0 ? '--%' : pct + '%';
  text(px, label, x - 4, y - 3, { color: th.sub, align: 'right' });
  return x - 4 - measure(label);
}

// ---------- the cat ----------
// 喵喵: an original orange American Shorthair ("大橘") sitting politely, tail wrapped
// around its paws. Parametric on a 41x46 grid; cx = 20 keeps it symmetric.
const CAT_COLORS = {
  k: '#4a2a1a', w: '#f7a84e', o: '#d8702a', m: '#ffe9cb', p: '#f49aa7', c: '#ff8f86', e: '#3a2014', h: '#ffffff',
  q: '#2f78c2', b: '#79c3f5',
};
const CAT_W = 41, CAT_H = 46, CAT_HEAD_H = 36;
function catGrid(o = {}) {
  const CW = CAT_W, CH = o.noBody ? CAT_HEAD_H : CAT_H;
  const g = Array.from({ length: CH }, () => Array(CW).fill('.'));
  const set = (x, y, v) => { if (x >= 0 && y >= 0 && x < CW && y < CH) g[y][x] = v; };
  const sup = (x, y, cx, cy, rx, ry, n = 2) => Math.abs((x - cx) / rx) ** n + Math.abs((y - cy) / ry) ** n <= 1;
  const sign = (p, a, b) => (p[0] - b[0]) * (a[1] - b[1]) - (a[0] - b[0]) * (p[1] - b[1]);
  const inT = (x, y, t) => {
    const p = [x, y], d1 = sign(p, t[0], t[1]), d2 = sign(p, t[1], t[2]), d3 = sign(p, t[2], t[0]);
    return !((d1 < 0 || d2 < 0 || d3 < 0) && (d1 > 0 || d2 > 0 || d3 > 0));
  };
  const shrink = (t, k) => {
    const cx = (t[0][0] + t[1][0] + t[2][0]) / 3, cy = (t[0][1] + t[1][1] + t[2][1]) / 3;
    return t.map(([x, y]) => [cx + (x - cx) * k, cy + (y - cy) * k + 0.9]);
  };
  // Shorter, wide-set ears with soft tips; a broad round head with full cheeks.
  const earL = [[4.8, 16.5], [8.4, 3.6], [16.4, 9.8]], earR = [[35.2, 16.5], [31.6, 3.6], [23.6, 9.8]];
  if (o.twitch) earR[1] = [33.6, 6];
  if (o.droop) { earL[1] = [2.2, 11]; earR[1] = [37.8, 11]; }
  const head = (x, y) => sup(x, y, 20, 21.5, 16.2, 13, 2.25);
  const tailPts = [[30, 42.4], [34.5, 42.6], [37.4, 40.6], [38.3, 37.2], [37.2, 34.2]];
  const nearTail = (x, y) => {
    for (let i = 0; i + 1 < tailPts.length; i++) {
      const [ax, ay] = tailPts[i], [bx, by] = tailPts[i + 1];
      const dx = bx - ax, dy = by - ay, t = Math.max(0, Math.min(1, ((x - ax) * dx + (y - ay) * dy) / (dx * dx + dy * dy)));
      if ((x - ax - t * dx) ** 2 + (y - ay - t * dy) ** 2 <= 1.7 ** 2) return true;
    }
    return false;
  };
  const paw = (x, y) => sup(x, y, 15.5, 42.6, 3.6, 2.1) || sup(x, y, 24.5, 42.6, 3.6, 2.1);
  const bodyCore = (x, y) => sup(x, y, 20, 37.6, 12.4, 7.4, 2.2);
  const body = (x, y) => !o.noBody && (bodyCore(x, y) || paw(x, y) || nearTail(x, y));
  const ear = (x, y) => (inT(x, y, earL) || inT(x, y, earR)) && !(y < 6.2 && Math.abs(x - 20) > 10.6 && Math.abs(x - 20) < 12.9 && false);
  const inner = [shrink(earL, 0.5), shrink(earR, 0.5)];
  const S = (x, y) => head(x, y) || body(x, y) || ear(x, y);
  for (let y = 0; y < CH; y++)
    for (let x = 0; x < CW; x++) {
      if (!S(x, y)) continue;
      g[y][x] = 'w';
      if (!head(x, y) && (inT(x, y, inner[0]) || inT(x, y, inner[1]))) g[y][x] = 'p';
    }
  // soften the ear tips: drop the single apex pixel
  for (const t of [earL, earR]) { const ax = Math.round(t[1][0]), ay = Math.round(t[1][1]); if (g[ay] && g[ay][ax] === 'w' && !S(ax, ay - 1)) g[ay][ax] = '.'; }
  const filled = (x, y) => x >= 0 && y >= 0 && x < CW && y < CH && g[y][x] !== '.' && g[y][x] !== 'k';
  const snap = g.map((r) => r.slice());
  const was = (x, y) => x >= 0 && y >= 0 && x < CW && y < CH && snap[y][x] !== '.';
  for (let y = 0; y < CH; y++)
    for (let x = 0; x < CW; x++) {
      if (was(x, y)) {
        if (!head(x, y) && bodyCore(x, y) && (head(x, y - 1) || head(x - 1, y) || head(x + 1, y))) g[y][x] = 'k'; // chin line
        continue;
      }
      if (was(x - 1, y) || was(x + 1, y) || was(x, y - 1) || was(x, y + 1)) g[y][x] = 'k';
    }
  // where the tail leaves the body, keep a short seam so it reads as a separate shape
  if (!o.noBody) for (let y = 36; y < CH; y++) for (let x = 30; x < CW; x++)
    if (g[y][x] === 'w' && bodyCore(x, y) && !nearTail(x, y) && (nearTail(x + 1, y) || nearTail(x, y - 1))) g[y][x] = 'k';
  if (!o.noBody) {
    // cream bib, paw toes, tail stripes, side stripes
    for (let y = 30; y < CH; y++) for (let x = 0; x < CW; x++)
      if (g[y][x] === 'w' && sup(x, y, 20, 35.2, 5.2, 4.2) && !head(x, y)) g[y][x] = 'm';
    for (const x of [14, 16, 23, 25]) set(x, 43, 'k');
    set(20, 41, 'k'); set(20, 42, 'k'); set(20, 43, 'k');
    for (const [x, y] of [[33, 42], [33, 43], [36, 41], [37, 41], [37, 37], [38, 37]]) if (g[y] && g[y][x] === 'w') g[y][x] = 'o';
    for (const [x, y] of [[8, 34], [9, 35], [8, 37], [9, 38], [31, 34], [32, 35], [31, 37], [30, 38]]) if (g[y] && g[y][x] === 'w') g[y][x] = 'o';
  }
  // tabby "M" on the forehead and stripes on the cheeks
  for (const [x, y0, y1] of [[20, 10, 12], [17, 11, 12], [23, 11, 12]]) for (let y = y0; y <= y1; y++) set(x, y, 'o');
  for (const [x, y] of [[5, 20], [6, 20], [7, 20]]) { set(x, y, 'o'); set(40 - x, y, 'o'); }
  // cream muzzle and chin
  for (let y = 22; y < 33; y++) for (let x = 12; x < 29; x++)
    if (g[y][x] === 'w' && sup(x, y, 20, 27.4, 5.6, 3.6)) g[y][x] = 'm';
  const E = (pts) => pts.forEach(([x, y, v]) => set(x, y, v || 'e'));
  const pair = (pts) => { E(pts); E(pts.map(([x, y, v]) => [40 - x, y, v])); };
  const eyes = o.eyes || 'open';
  const round = (dx, dy, extra) => {
    const pts = [[13, 17], [14, 17], [12, 18], [13, 18], [14, 18], [15, 18], [12, 19], [13, 19], [14, 19], [15, 19],
      [12, 20], [13, 20], [14, 20], [15, 20], [13, 21], [14, 21]];
    if (extra) pts.push([12, 17], [15, 17], [12, 21], [15, 21]);
    pair(pts.map(([x, y]) => [x + dx, y + dy]));
    set(13 + dx, 18 + dy, 'h'); set(26 + dx, 18 + dy, 'h');
    set(14 + dx, 18 + dy, 'h'); set(27 + dx, 18 + dy, 'h');
  };
  if (eyes === 'open') round(0, 0, false);
  if (eyes === 'wide') { round(0, 0, false); set(15, 20, 'h'); set(28, 20, 'h'); }
  if (eyes === 'up') { const pts = [[13, 16], [14, 16], [12, 17], [13, 17], [14, 17], [15, 17], [13, 18], [14, 18]];
    E(pts.map(([x, y]) => [x + 1, y])); E(pts.map(([x, y]) => [x + 14, y])); set(14, 17, 'h'); set(27, 17, 'h'); }
  if (eyes === 'blink') pair([[12, 20], [13, 20], [14, 20], [15, 20]]);
  if (eyes === 'happy') pair([[12, 20], [13, 19], [14, 19], [15, 20]]);
  if (eyes === 'sleep') pair([[12, 19], [13, 20], [14, 20], [15, 19]]);
  if (eyes === 'x') pair([[12, 18], [15, 18], [13, 19], [14, 19], [13, 20], [14, 20], [12, 21], [15, 21]]);
  if (eyes === 'sad') { pair([[13, 19], [14, 19], [12, 20], [13, 20], [14, 20], [15, 20], [13, 21], [14, 21], [11, 17, 'o'], [12, 16, 'o']]); set(13, 19, 'h'); set(26, 19, 'h'); set(13, 23, 'b'); set(13, 24, 'b'); }
  // nose and mouth on the muzzle
  E([[19, 24, 'p'], [20, 24, 'p'], [21, 24, 'p'], [20, 25, 'p']]);
  const mouth = o.mouth || 'w';
  if (mouth === 'w') E([[20, 26, 'k'], [17, 26, 'k'], [23, 26, 'k'], [18, 27, 'k'], [19, 27, 'k'], [21, 27, 'k'], [22, 27, 'k']]);
  if (mouth === 'open') E([[18, 26, 'k'], [19, 26, 'k'], [20, 26, 'k'], [21, 26, 'k'], [22, 26, 'k'], [18, 27, 'k'], [19, 27, 'p'], [20, 27, 'p'], [21, 27, 'p'], [22, 27, 'k'], [19, 28, 'k'], [20, 28, 'k'], [21, 28, 'k']]);
  if (mouth === 'o') E([[19, 26, 'k'], [20, 26, 'k'], [21, 26, 'k'], [19, 27, 'k'], [20, 27, 'p'], [21, 27, 'k'], [19, 28, 'k'], [20, 28, 'k'], [21, 28, 'k']]);
  if (mouth === 'flat') E([[18, 27, 'k'], [19, 27, 'k'], [20, 27, 'k'], [21, 27, 'k'], [22, 27, 'k']]);
  if (mouth === 'sad') E([[18, 28, 'k'], [19, 27, 'k'], [20, 27, 'k'], [21, 27, 'k'], [22, 28, 'k']]);
  if (!o.noCheeks) E([[9, 23, 'c'], [10, 23, 'c'], [30, 23, 'c'], [31, 23, 'c']]);
  for (const [x, y] of [[0, 23], [1, 23], [2, 23], [1, 26], [2, 26], [3, 25]]) { set(x, y, 'k'); set(40 - x, y, 'k'); }
  return g.map((r) => r.join(''));
}
function cat(px, x, y, s, o) { sprite(px, catGrid(o), CAT_COLORS, x, y, s); }

// Headphones sit behind the ears (band) and on the cheeks (cups).
function headphones(px, x, y, s, color) {
  const band = [];
  for (let gy = 0; gy < 22; gy++)
    for (let gx = 0; gx < 41; gx++) {
      const d = ((gx - 20) / 18.2) ** 2 + ((gy - 22) / 18.6) ** 2;
      if (d <= 1 && d >= 0.84) band.push([gx, gy]);
    }
  band.forEach(([gx, gy]) => px.rect(x + gx * s, y + gy * s, s, s, color));
  for (const gx of [0, 37]) {
    px.rect(x + gx * s, y + 17 * s, 4 * s, 9 * s, CAT_COLORS.k);
    px.rect(x + (gx + 1) * s, y + 18 * s, 2 * s, 7 * s, color);
  }
}

// ---------- palettes ----------
const THEMES = {
  night: {
    bg: '#171a38', panel: '#23285a', line: '#0b0d22', shadow: '#0b0d22', text: '#f7e8c9', sub: '#a3a2cb',
    faint: '#363c74', amber: '#f4b23e', green: '#83d491', red: '#f2705f', blue: '#74b8f2', ai: '#b4adff',
  },
  paper: {
    bg: '#efdcb9', panel: '#fcf4e1', line: '#1f2244', shadow: '#1f2244', text: '#1f2244', sub: '#7c6a4f',
    faint: '#d9c39b', amber: '#d98c16', green: '#33925a', red: '#d24332', blue: '#2f78c2', ai: '#5a55c8',
  },
};
const AIPAL = {
  night: { bg: '#2c2e86', panel: '#212364', line: '#11123c', shadow: '#11123c', text: '#fff0d4', sub: '#bdb9f4', glow: '#9c97ff', amber: '#f4b23e', green: '#83d491', red: '#f2705f', faint: '#4a4db0' },
  paper: { bg: '#4b4cb8', panel: '#3b3c9a', line: '#1c1d5a', shadow: '#1c1d5a', text: '#fff0d4', sub: '#d3d0ff', glow: '#c2beff', amber: '#ffc04d', green: '#8fe09d', red: '#ff806f', faint: '#6d6ed0' },
};

// ---------- demo data ----------
const DEMO = {
  collection: '恐龙探险队', title: '第2集：小猫寻找月亮', index: 2, count: 24,
  pos: 402, dur: 1085, vol: 60, bat: 87,
  longCollection: '恐龙探险队：森林里很长很长的奇妙旅程',
  longTitle: '第10集：穿过星光闪烁的森林寻找失踪的朋友',
  // Fitted titles (2026-10-16): longer and longer names, an English one, one past 18 px.
  fitTitles: ['第23回：孙悟空三打白骨精，唐僧误会把他赶走了', '十万个为什么：为什么天空是蓝色的，而傍晚的晚霞却是红色的呢',
    '第8集：小兔子和小松鼠一起去森林深处寻找会发光的蘑菇，路上遇到了一只迷路的小乌龟'],
  enTitle: 'The Little Red Hen S01E05 - Who Will Bake the Bread',
  longestTitle: '【睡前故事】小熊阿布和他的好朋友们：大森林里的春天、夏天、秋天和冬天，一年四季都有好玩的事情发生，小猪、小鹿和小狐狸都来了，大家一起过了快乐的一年',
  dirs: [['恐龙探险队', 24], ['小猫系列', 12], ['睡前故事', 31], ['西游记', 108], ['未分类', 5], ['十万个为什么', 40]],
  // A song collection for the play-mode pictures (2026-10-15).
  song: { collection: '儿歌', title: '小星星', index: 5, count: 12, pos: 41, dur: 128 },
  // Subtitles: what the child said, and the sentence 喵喵 is speaking.
  heard: '喵喵，月亮为什么会跟着我走？',
  said: '因为月亮离我们特别远，你走路的时候，它看起来就像一直在原地陪着你。',
  longSaid: '月亮离我们大约有三十八万公里那么远，所以不管你走到哪里，抬头看它的角度几乎都不会变，看起来就好像它一直跟着你一起走，其实是你在走，它一直待在天上。',
};
const mmss = (s) => `${Math.floor(s / 60)}:${String(Math.floor(s % 60)).padStart(2, '0')}`;
const STATUS = {
  playing: { icon: 'play', label: '正在播放', color: 'green' },
  paused: { icon: 'pause', label: '已暂停', color: 'amber' },
  locating: { icon: 'hourglass', label: '正在定位', color: 'blue' },
  switching: { icon: 'hourglass', label: '正在切换', color: 'blue' },
  finished: { icon: 'done', label: '已播完', color: 'sub' },
  error: { icon: 'warn', label: '播放错误', color: 'red' },
};

// ---------- shared player parts ----------
function topBar(px, th, d) {
  const st = STATUS[d.state];
  const [iw] = iconSize(st.icon, 2);
  icon(px, st.icon, 20 + (14 - iw) / 2, 20, 2, th[st.color]);
  text(px, st.label, 42, 18, { color: th[st.color], maxW: 110 });
  // After the widest label, clear of the AI badge even at 100%.
  if (d.mode && d.mode !== 'order') icon(px, d.mode, 102, 20, 2, th.sub);
  const left = battery(px, th, d.bat ?? DEMO.bat, 196, 21);
  if (d.ai === 'ready') badge(px, left - 28, 18);
}
function progress(px, th, x, y, w, p, n = 25) {
  const step = Math.floor(w / n), on = Math.round(p * n);
  for (let i = 0; i < n; i++) px.rect(x + i * step, y, step - 2, 10, i < on ? th.amber : th.faint);
}
function timeRow(px, th, d, y) {
  const el = d.state === 'locating' ? '--:--' : mmss(d.pos);
  pxt(px, el, 20, y, 2, th.text);
  text(px, '/ ' + (d.dur ? mmss(d.dur) : '--:--'), 24 + measure(el, { font: PXF, s: 2 }), y + 1, { color: th.sub });
  const vw = measure(String(d.vol), { font: PXF, s: 2 });
  pxt(px, String(d.vol), 220, y, 2, th.text, 'right');
  icon(px, d.vol ? 'speaker' : 'mute', 220 - vw - 22, y + 1, 2, th.sub);
}
function displayTitle(name) { return name.replace(/\.(mp3|wav)$/i, ''); }

// ---------- fitted title (2026-10-16) ----------
// Layout A's title uses the biggest size from 28 px (the old 2x) down to 18 px that
// shows it whole, else 18 px with an ellipsis. A size is the advance of a Chinese
// character; every 14 px measure scales by A/14, rounded.
const TITLE = { w: 182, h: 108, max: 28, min: 18, bold: 21 };
const titleScale = (v, A) => Math.floor((v * A + 7) / 14);
const titlePitch = (A) => Math.max(A + 4, 20);
function titleLines(A) { let n = 1; while (n * titlePitch(A) + A + 2 <= TITLE.h) n++; return n; }
const titleAdv = (ch, A) => titleScale(glyph(NOTO, ch).adv, A);
function titleWidth(chars, A) { let w = 0; for (const ch of chars) w += titleAdv(ch, A); return w; }
// No line starts with a closing mark or ends with an opening one, and ASCII letters
// and digits stay together; spaces where a line breaks are dropped.
const NO_START = new Set([...'，。、；：！？）》」』】〉〕”’…·,.;:!?)]}%']);
const NO_END = new Set([...'（《「『【〈〔“‘([{']);
const TRIM = new Set([...' ，、。；：,.;:', ...NO_END]);
const isWord = (ch) => /^[A-Za-z0-9'_]$/.test(ch);
const canBreak = (a, b) => !NO_START.has(b) && !NO_END.has(a) && !(isWord(a) && isWord(b));
function trimLine(line) { while (line.length > 1 && line[line.length - 1] === ' ') line.pop(); return line; }
// A character that does not fit starts a new line, with whatever must stay with it.
// Past `lines` lines the last one ends with an ellipsis; a last line of one character
// takes the previous line's last character or word.
function titleWrap(str, A, lines) {
  const out = [];
  let cur = [], w = 0;
  for (const ch of str) {
    const a = titleAdv(ch, A);
    if (cur.length && w + a > TITLE.w) {
      let at = cur.length;
      if (!canBreak(cur[at - 1], ch)) {
        at--;
        while (at > 0 && !canBreak(cur[at - 1], cur[at])) at--;
        if (at === 0 || titleWidth(cur.slice(at), A) + a > TITLE.w) at = cur.length;
      }
      const rest = cur.splice(at);
      out.push(trimLine(cur));
      while (rest.length && rest[0] === ' ') rest.shift();
      cur = rest;
      w = titleWidth(cur, A);
      if (!cur.length && ch === ' ') continue;
    }
    cur.push(ch);
    w += a;
  }
  if (cur.length) out.push(cur);
  if (out.length > lines) {
    out.length = lines;
    const last = out[lines - 1];
    while (last.length && (titleWidth(last, A) + titleAdv('…', A) > TITLE.w || TRIM.has(last[last.length - 1]))) last.pop();
    last.push('…');
    return { lines: out, cut: true };
  }
  const n = out.length;
  if (n > 1 && out[n - 1].length === 1) {
    const prev = out[n - 2];
    let k = prev.length - 1;
    while (k > 0 && !canBreak(prev[k - 1], prev[k])) k--;
    const moved = prev.slice(k).concat(out[n - 1]);
    while (moved.length && moved[0] === ' ') moved.shift();
    if (k > 0 && titleWidth(moved, A) <= TITLE.w) { out[n - 2] = trimLine(prev.slice(0, k)); out[n - 1] = moved; }
  }
  return { lines: out, cut: false };
}
function titleFit(str) {
  for (let A = TITLE.max; ; A--) {
    const t = titleWrap(str, A, titleLines(A));
    if (!t.cut || A === TITLE.min) return { A, lines: t.lines };
  }
}
// Which of n source pixels draw 2 px wide so that together they cover m (n <= m <= 2n):
// the cheapest set, every boundary less than 1 px from its exact place and boundary
// `pin` at its nearest. Returns where each source pixel starts (n + 1 values).
function titleStretch(cost, n, m, pin) {
  const d = m - n, INF = 1e9, pick = [];
  let best = new Array(d + 1).fill(INF);
  best[0] = 0;
  for (let c = 0; c < n; c++) {
    const next = new Array(d + 1).fill(INF), took = new Array(d + 1).fill(0);
    for (let k = 0; k <= d; k++)
      for (let two = 0; two < 2 && best[k] < INF && k + two <= d; two++) {
        const off = Math.abs(n * (c + 1 + k + two) - (c + 1) * m);
        if (c + 1 === pin ? 2 * off > n : off >= n) continue;
        const v = best[k] + two * cost[c] * 1000 + off;
        if (v < next[k + two]) { next[k + two] = v; took[k + two] = two; }
      }
    pick.push(took);
    best = next;
  }
  const start = new Array(n + 1);
  start[n] = m;
  for (let c = n - 1, k = d; c >= 0; c--) { start[c] = start[c + 1] - 1 - pick[c][k]; k -= pick[c][k]; }
  return start;
}
// A 14 px glyph at size A: some columns and rows draw 2 px wide. From TITLE.bold up the
// ones a stroke runs along go first (2 px strokes, like the old 2x); below it the empty
// ones (1 px strokes, like 1x). The baseline row stays where the line puts it.
function titleGlyph(px, g, x, top, A, color) {
  const y0 = NOTO.lh - NOTO.base - g.h - g.y;
  const ink = (c, r) => { c -= g.x; r -= y0; return c >= 0 && c < g.w && r >= 0 && r < g.h && g.bits[r * g.w + c] === 1; };
  const c0 = Math.min(0, g.x), c1 = Math.max(g.adv, g.x + g.w), r0 = Math.min(0, y0), r1 = Math.max(NOTO.lh, y0 + g.h);
  const costX = [], costY = [];
  for (let c = c0; c < c1; c++) {
    let k = 0;
    for (let r = r0; r < r1; r++) if (ink(c, r) && !ink(c - 1, r) && !ink(c + 1, r)) k++;
    costX.push(k);
  }
  for (let r = r0; r < r1; r++) {
    let k = 0;
    for (let c = c0; c < c1; c++) if (ink(c, r) && !ink(c, r - 1) && !ink(c, r + 1)) k++;
    costY.push(k);
  }
  if (A >= TITLE.bold)
    for (const cost of [costX, costY]) { const most = Math.max(...cost); cost.forEach((v, i) => { cost[i] = most - v; }); }
  const sx = titleStretch(costX, c1 - c0, titleScale(c1, A) - titleScale(c0, A), -1);
  const sy = titleStretch(costY, r1 - r0, titleScale(r1, A) - titleScale(r0, A), NOTO.lh - NOTO.base - r0);
  const ox = x + titleScale(c0, A), oy = top + titleScale(r0, A);
  for (let r = r0; r < r1; r++)
    for (let c = c0; c < c1; c++)
      if (ink(c, r)) px.rect(ox + sx[c - c0], oy + sy[r - r0], sx[c - c0 + 1] - sx[c - c0], sy[r - r0 + 1] - sy[r - r0], color);
}

// Layout A: the story card, text first.
function playerBook(px, th, d, t) {
  px.clear(th.bg);
  topBar(px, th, d);
  const cx = 16, cy = 44, cw = 206, ch = 156;
  px.panel(cx, cy, cw, ch, { fill: th.panel, line: th.line, shadow: th.shadow });
  icon(px, 'folder', cx + 12, cy + 14, 2, th.sub);
  const idx = `${d.index}/${d.count}`;
  text(px, idx, cx + cw - 12, cy + 10, { color: th.sub, align: 'right' });
  text(px, d.collection, cx + 36, cy + 10, { color: th.sub, maxW: cw - 60 - measure(idx) });
  px.dash(cx + 12, cy + 36, cw - 24, th.faint);
  const fit = titleFit(displayTitle(d.title)), pitch = titlePitch(fit.A);
  const block = (fit.lines.length - 1) * pitch + fit.A + 2, top = cy + 40 + Math.max(0, (ch - 48 - block) >> 1);
  fit.lines.forEach((line, i) => {
    let x = cx + 12;
    for (const c of line) { titleGlyph(px, glyph(NOTO, c), x, top + i * pitch, fit.A, th.text); x += titleAdv(c, fit.A); }
  });
  progress(px, th, 20, 216, 200, d.dur ? d.pos / d.dur : 0);
  timeRow(px, th, d, 236);
}

// Layout C: the cat listens along; the same cat becomes the AI.
function playerCat(px, th, d, t) {
  px.clear(th.bg);
  topBar(px, th, d);
  const playing = d.state === 'playing';
  const bob = playing && Math.floor(t * 2) % 2 ? 2 : 0;
  const x = 79, y = 40 + bob;
  if (playing) headphones(px, x, y, 2, th.blue);
  const eyes = playing ? 'happy' : d.state === 'finished' ? 'sleep' : d.state === 'locating' ? 'up' : (Math.floor(t * 8) % 24 === 0 ? 'blink' : 'open');
  cat(px, x, y, 2, { eyes, mouth: 'w' });
  if (playing) {
    const f = Math.floor(t * 3) % 3;
    for (let i = 0; i < 2; i++) {
      const k = (f + i * 2) % 3;
      icon(px, 'note', 172 + i * 14, 92 - k * 10 - i * 6, 2, k === 2 ? th.faint : th.sub);
    }
  }
  if (d.state === 'paused') icon(px, 'pause', 172, 60, 2, th.amber);
  if (d.state === 'finished') { pxt(px, 'z', 168, 64, 2, th.sub); pxt(px, 'Z', 182, 50, 2, th.sub); }
  if (d.state === 'locating') icon(px, 'hourglass', 172, 60, 2, th.blue);
  icon(px, 'folder', 20, 144, 2, th.sub);
  const idx = `${d.index}/${d.count}`;
  text(px, idx, 220, 140, { color: th.sub, align: 'right' });
  text(px, d.collection, 44, 140, { color: th.sub, maxW: 170 - measure(idx) });
  text(px, displayTitle(d.title), 20, 166, { maxW: 200, lines: 2, lh: 20, color: th.text });
  progress(px, th, 20, 216, 200, d.dur ? d.pos / d.dur : 0);
  timeRow(px, th, d, 236);
}
const LAYOUTS = { book: playerBook, cat: playerCat };

// ---------- overlays ----------
function overlayPanel(px, th, x, y, w, h) { px.panel(x, y, w, h, { fill: th.panel, line: th.line, shadow: th.shadow }); }
function volumeOverlay(px, th, vol) {
  overlayPanel(px, th, 30, 92, 180, 88);
  icon(px, vol ? 'speaker' : 'mute', 46, 108, 3, th.text);
  pxt(px, String(vol), 194, 106, 3, th.text, 'right');
  for (let i = 0; i < 20; i++) px.rect(50 + i * 7, 152, 5, 12, i < vol / 5 ? th.amber : th.faint);
}
function seekOverlay(px, th, delta, target) {
  overlayPanel(px, th, 30, 92, 180, 88);
  icon(px, delta < 0 ? 'rew' : 'ff', 46, 110, 2, th.text);
  text(px, `${delta < 0 ? '后退' : '前进'} ${Math.abs(delta)} 秒`, 76, 104, { color: th.text });
  pxt(px, mmss(target), 120, 140, 2, th.amber, 'center');
}
function toast(px, th, iconName, iconColor, line1, line2) {
  overlayPanel(px, th, 18, 168, 204, line2 ? 64 : 46);
  if (iconName) icon(px, iconName, 32, 184, 2, iconColor);
  text(px, line1, 62, line2 ? 178 : 181, { color: th.text, maxW: 150 });
  if (line2) text(px, line2, 62, 200, { color: th.sub, maxW: 150 });
}
function dirPicker(px, th, sel, t, dirs = DEMO.dirs, left = 1 - ((t % 5) / 5)) {
  px.clear(th.bg);
  icon(px, 'folder', 20, 22, 2, th.amber);
  text(px, '选择目录', 46, 18, { color: th.text });
  text(px, `${sel + 1}/${dirs.length}`, 220, 18, { color: th.sub, align: 'right' });
  const top = 46, rowH = 36, first = Math.max(0, Math.min(sel - 2, dirs.length - 5));
  px.panel(16, top, 206, rowH * 5 + 8, { fill: th.panel, line: th.line, shadow: th.shadow });
  for (let i = 0; i < 5 && first + i < dirs.length; i++) {
    const di = first + i, [name, n] = dirs[di], y = top + 4 + i * rowH, hi = di === sel;
    if (hi) {
      px.rect(20, y, 198, rowH, th.amber);
      icon(px, 'play', 26, y + 11, 2, th.line);
    } else if (i) px.dash(28, y, 180, th.faint);
    text(px, name, 46, y + 8, { color: hi ? th.line : th.text, maxW: 128 });
    text(px, `${n}个`, 210, y + 8, { color: hi ? th.line : th.sub, align: 'right' });
  }
  text(px, '中键/底键 选择　顶部键 确认', 120, 246, { color: th.sub, align: 'center' });
  px.rect(20, 270, Math.round(200 * left), 2, th.faint);
}

// ---------- AI ----------
// The faces the server's emotion words map to (P5_1_PIXEL_UI.md), shown only
// while speaking; the firmware maps the words, the prototype takes the face.
const FACES = {
  neutral: { eyes: 'open', mouth: 'w' }, happy: { eyes: 'happy', mouth: 'w' }, surprised: { eyes: 'wide', mouth: 'o' },
  sad: { eyes: 'sad', mouth: 'sad', droop: true }, sleepy: { eyes: 'sleep', mouth: 'w' }, thinking: { eyes: 'up', mouth: 'flat' },
};
function aiState(mode, t, face = 'neutral', failed = false) {
  const f8 = Math.floor(t * 8);
  const blink = f8 % 26 === 0;
  switch (mode) {
    case 'connecting': return { eyes: blink ? 'blink' : 'open', mouth: 'w', label: '连接中' };
    case 'listening': return { eyes: blink ? 'blink' : 'open', mouth: 'w', twitch: f8 % 16 < 2, label: '我在听' };
    case 'thinking': return { eyes: 'up', mouth: 'flat', label: '想一想' };
    case 'speaking': {
      // The emotion's eyes (and ears); the mouth opens over the emotion's own
      // shape. Only the default round eyes blink.
      const f = FACES[face];
      return { eyes: f.eyes === 'open' && blink ? 'blink' : f.eyes, mouth: Math.floor(t * 5) % 2 ? 'open' : f.mouth,
        droop: f.droop, label: '我在说' };
    }
    // The error face when the chat ended because of the network or a server error.
    case 'exiting': return { eyes: failed ? 'x' : 'happy', mouth: failed ? 'flat' : 'w', label: '回到故事' };
    default: return { eyes: 'open', mouth: 'w', label: '' };
  }
}
// Sound waves beside the ears while listening; drawn in the cat's own pixel scale.
function arcs(px, A, cx, cy, t, side, s) {
  const n = Math.floor(t * 3) % 4;
  const shapes = [[1, 0, 0, 1], [2, 1, 0, 0, 0, 1, 2], [3, 2, 1, 1, 0, 0, 0, 1, 1, 2, 3]];
  shapes.forEach((sh, i) => {
    if (i >= n) return;
    sh.forEach((off, r) => {
      const gx = side < 0 ? -(i * 3 + 2) + off : i * 3 + 2 - off;
      px.rect(cx + gx * s - (side < 0 ? s : 0), cy + (r - (sh.length >> 1)) * s, s, s, A.glow);
    });
  });
}
// o: interrupted (the chat paused a playing story), noStory (no card), subs,
// bat, face (while speaking), failed (the error face while returning), heard
// (the child's words) and said (the sentence being spoken).
function aiScreen(px, themeKey, mode, t, o = {}) {
  const A = AIPAL[themeKey], th = THEMES[themeKey];
  px.clear(A.bg);
  const st = aiState(mode, t, o.face, o.failed);
  const subs = o.subs && (mode === 'thinking' || mode === 'speaking');
  badge(px, 18, 18);
  text(px, subs ? st.label : '和喵喵聊天', 46, 18, { color: subs ? A.text : A.sub });
  battery(px, { ...th, bg: A.bg, text: A.text, sub: A.sub, faint: A.faint, green: A.green, red: A.red }, o.bat ?? DEMO.bat, 196, 21);
  const s = subs ? 2 : 3, cw = CAT_W * s, x = (W - cw) >> 1, y = 40;
  cat(px, x, y, s, st);
  const earY = y + 17 * s;
  if (mode === 'listening') { arcs(px, A, x + 2 * s, earY, t, -1, s); arcs(px, A, x + cw - 2 * s, earY, t, 1, s); }
  if (mode === 'thinking') {
    const n = Math.floor(t * 3) % 4;
    // three thought bubbles rising from the right ear, filled one by one
    const spots = s === 3 ? [[170, 78, 6], [182, 64, 8], [198, 48, 10]] : [[156, 66, 4], [165, 56, 6], [177, 44, 8]];
    spots.forEach(([bx, by, r], i) => px.round(bx, by, r, r, i < n ? A.text : A.faint, r >= 8 ? 2 : 1));
  }
  if (mode === 'speaking' && Math.floor(t * 5) % 2) {
    for (const [dx, dy, w] of [[1, -4, 3], [2, 0, 4], [1, 4, 3]]) px.rect(x + cw + dx * s, y + 26 * s + dy * s, w * s, s, A.glow);
  }
  if (mode === 'connecting') {
    const n = Math.floor(t * 3) % 4;
    const keep = [[6], [4, 5, 6], [2, 3, 4, 5, 6], [0, 1, 2, 3, 4, 5, 6]][n];
    const rows = ICONS.wifi.map((r, i) => (keep.includes(i) ? r : '.'.repeat(r.length)));
    sprite(px, rows, { '#': A.text }, x + cw - 2 * s, y - 2, s);
  }
  if (subs) {
    const bx = 18, by = 140, bw = 204, bh = 100;
    px.panel(bx, by, bw, bh, { fill: A.panel, line: A.line, shadow: A.shadow });
    for (let i = 0; i < 4; i++) px.rect(112 + i * 2, by - 2 - i * 2, 16 - i * 4, 2, A.line);
    // Thinking shows the child's words; speaking the sentence being spoken, or
    // the child's words until the first sentence arrives.
    const said = mode === 'speaking' && o.said ? o.said : '';
    const words = said || (o.heard ? '你说：' + o.heard : '');
    text(px, words, bx + 12, by + 10, { color: said ? A.text : A.sub, maxW: bw - 24, lines: 4, lh: 20 });
  } else {
    const label = o.noStory && mode === 'exiting' ? '再见' : st.label;
    text(px, label, 120, y + CAT_H * s + 2, { s: 2, color: A.text, align: 'center' });
  }
  // footer: only claim "story paused" when the chat really interrupted playback
  const fy = 252;
  if (o.noStory) {
    // No card: there is no story to return to.
    const ending = mode === 'exiting';
    text(px, ending ? '对话已结束' : '按顶部键结束对话', 20, fy, { color: ending ? A.text : A.sub });
  } else if (mode === 'exiting') {
    icon(px, 'play', 20, fy + 2, 2, A.green);
    text(px, '正在回到故事…', 42, fy, { color: A.text });
  } else if (o.interrupted) {
    icon(px, 'pause', 21, fy + 2, 2, A.amber);
    text(px, '故事已暂停', 42, fy, { color: A.amber });
    text(px, '顶部键 继续听', 220, fy, { color: A.sub, align: 'right' });
  } else {
    icon(px, 'play', 20, fy + 2, 2, A.sub);
    text(px, '按顶部键回到故事', 42, fy, { color: A.sub });
  }
}
function expressionSheet(px, themeKey, t) {
  const A = AIPAL[themeKey];
  px.clear(A.bg);
  const list = [['开心', { eyes: 'happy', mouth: 'w' }], ['惊讶', { eyes: 'wide', mouth: 'o' }], ['难过', { eyes: 'sad', mouth: 'sad', droop: true }],
    ['困了', { eyes: 'sleep', mouth: 'w' }], ['出错', { eyes: 'x', mouth: 'flat' }], ['思考', { eyes: 'up', mouth: 'flat' }]];
  list.forEach(([name, o], i) => {
    const col = i % 2, row = Math.floor(i / 2);
    const x = 24 + col * 110, y = 14 + row * 92;
    cat(px, x, y, 2, { ...o, noBody: true });
    text(px, name, x + 41, y + 72, { color: A.text, align: 'center' });
  });
}

// ---------- system pages ----------
function centerPage(px, bg, iconName, iconColor, scale, title, lines, o = {}) {
  px.clear(bg);
  const [iw, ih] = iconSize(iconName, scale);
  icon(px, iconName, (W - iw) >> 1, 70 - (ih >> 1) + 10, scale, iconColor);
  text(px, title, 120, 118, { s: 2, color: o.text, align: 'center' });
  (lines || []).forEach((l, i) => text(px, l, 120, 160 + i * 20, { color: o.sub, align: 'center', maxW: 200 }));
}
function aiHint(px, th, y) {
  px.panel(18, y, 204, 40, { fill: th.panel, line: th.line, shadow: th.shadow });
  badge(px, 28, y + 11);
  text(px, '双击顶部键 和喵喵聊天', 56, y + 11, { color: th.text });
}
function hotspot(px, th, t, name = 'Boilon-7F3A', left = 600 - Math.floor(t) % 600) {
  px.clear(th.bg);
  icon(px, 'wifi', 20, 22, 2, th.blue);
  text(px, '配网', 46, 18, { color: th.text });
  text(px, `剩 ${mmss(left)}`, 220, 18, { color: th.sub, align: 'right' });
  px.panel(16, 48, 206, 196, { fill: th.panel, line: th.line, shadow: th.shadow });
  text(px, '① 手机连接 Wi-Fi', 30, 60, { color: th.sub });
  pxt(px, name, 30, 86, 2, th.text);
  px.dash(30, 116, 178, th.faint);
  text(px, '② 浏览器打开', 30, 128, { color: th.sub });
  pxt(px, '192.168.4.1', 30, 154, 2, th.text);
  px.dash(30, 184, 178, th.faint);
  text(px, '③ 选家里的 Wi-Fi，填密码', 30, 196, { color: th.sub, maxW: 184 });
  text(px, '10 分钟后自动关闭', 120, 254, { color: th.sub, align: 'center' });
}
function activation(px, th, code = '482913') {
  px.clear(th.bg);
  badge(px, 18, 18);
  text(px, '激活喵喵', 46, 18, { color: th.text });
  px.panel(16, 52, 206, 96, { fill: th.panel, line: th.line, shadow: th.shadow });
  text(px, '激活码', 120, 62, { color: th.sub, align: 'center' });
  pxt(px, code.slice(0, 3) + ' ' + code.slice(3), 120, 94, 3, th.text, 'center');
  text(px, '1. 打开 xiaozhi.me 添加设备', 22, 172, { color: th.text });
  text(px, '2. 输入上面的 6 位数字', 22, 196, { color: th.text });
  text(px, '激活后故事照常播放', 120, 246, { color: th.sub, align: 'center' });
}

// ---------- current device screen, for comparison ----------
function currentScreen(px) {
  px.clear('#080808');
  const clip = [20, 24, 220, 272];
  const T = (s, x, y, w, lines, scale = 1) => {
    // LibraryLcd::Text: wraps at the box edge, 22px line pitch, no ellipsis.
    let cx = x, line = 0;
    for (const ch of s) {
      const g = glyph(NOTO, ch);
      if (cx + g.adv * scale > x + w) { if (++line >= lines) break; cx = x; }
      const oy = y + line * 22 + (16 - 2 - g.h - g.y) * scale;
      for (let r = 0; r < g.h; r++) for (let c = 0; c < g.w; c++) if (g.bits[r * g.w + c])
        for (let dy = 0; dy < scale; dy++) for (let dx = 0; dx < scale; dx++) {
          const X = cx + (c + g.x) * scale + dx, Y = oy + r * scale + dy;
          if (X >= clip[0] && X < clip[2] && Y >= clip[1] && Y < clip[3]) px.dot(X, Y, '#ffffff');
        }
      cx += g.adv * scale;
    }
  };
  T('正在播放', 20, 30, 132, 1, 2);
  T('87%', 183, 28, 37, 1);
  px.frame(160, 29, 18, 12, '#ffffff', 1);
  px.rect(178, 33, 3, 4, '#ffffff');
  px.rect(162, 31, Math.floor(87 * 14 / 100), 8, '#ffffff');
  T('恐龙探险队', 20, 78, 200, 1);
  T('第2集：小猫寻找月亮.mp3', 20, 112, 200, 3);
  T('6:42 / 18:05', 20, 220, 134, 1);
  const vol = 'VOL 60';
  T(vol, 220 - Math.min(56, measure(vol)), 220, 56, 1);
  const prog = Math.floor(402 / 1085 * 200);
  px.rect(20, 246, 200, 10, '#202020');
  px.rect(20, 246, prog, 10, '#00ffff');
}

// ---------- screen catalog ----------
const P = (over) => ({ ...DEMO, ai: 'ready', ...over });
const SCREENS = [
  { sec: 'player', id: 'play', name: '播放中', note: '默认页。标题先用 28 px，放不下就 1 px 一档往下缩，最小 18 px。', anim: true,
    draw: (px, S, t) => LAYOUTS[S.layout](px, THEMES[S.theme], P({ state: 'playing' }), t) },
  { sec: 'player', id: 'pause', name: '已暂停', note: '状态行变琥珀色；卡带停转。', anim: true,
    draw: (px, S, t) => LAYOUTS[S.layout](px, THEMES[S.theme], P({ state: 'paused' }), t) },
  { sec: 'player', id: 'locate', name: '开机续播定位中', note: '对应 F01：有实际输出之前如实显示「正在定位」。', anim: true,
    draw: (px, S, t) => LAYOUTS[S.layout](px, THEMES[S.theme], P({ state: 'locating' }), t) },
  { sec: 'player', id: 'long', name: '长目录名和长标题', note: '目录名单行省略；21 个字的标题缩到 26 px。', anim: true,
    draw: (px, S, t) => LAYOUTS[S.layout](px, THEMES[S.theme], P({ state: 'playing', collection: DEMO.longCollection, title: DEMO.longTitle + '.mp3', index: 10, pos: 95, dur: 1312 }), t) },
  { sec: 'player', id: 'longer', name: '更长的标题', note: '40 个字缩到 18 px；21 px 以下笔画 1 格细。', anim: true,
    draw: (px, S, t) => LAYOUTS[S.layout](px, THEMES[S.theme], P({ state: 'playing', collection: '睡前故事', title: DEMO.fitTitles[2] + '.mp3', index: 8, count: 120 }), t) },
  { sec: 'player', id: 'longest', name: '超长标题', note: '18 px 也放不下：最后一行末尾加「…」。', anim: true,
    draw: (px, S, t) => LAYOUTS[S.layout](px, THEMES[S.theme], P({ state: 'playing', collection: '睡前故事', title: DEMO.longestTitle + '.mp3', index: 3, count: 120 }), t) },
  { sec: 'player', id: 'english', name: '英文标题', note: '英文单词和数字不从中间断开。', anim: true,
    draw: (px, S, t) => LAYOUTS[S.layout](px, THEMES[S.theme], P({ state: 'playing', collection: 'English Stories', title: DEMO.enTitle + '.mp3', index: 5, count: 26 }), t) },
  { sec: 'player', id: 'end', name: '目录播完', note: '停在最后一集，不自动循环。', anim: true,
    draw: (px, S, t) => LAYOUTS[S.layout](px, THEMES[S.theme], P({ state: 'finished', index: 24, pos: 1085, dur: 1085 }), t) },
  { sec: 'player', id: 'offline', name: 'AI 不在线时', note: '右上角不显示喵喵，故事照常。', anim: true,
    draw: (px, S, t) => LAYOUTS[S.layout](px, THEMES[S.theme], P({ state: 'playing', ai: 'offline', bat: 12 }), t) },
  { sec: 'player', id: 'now', name: '现状（对照）', note: '设备上现在的样子，按现有代码的坐标重画。', fixed: true,
    draw: (px) => currentScreen(px) },

  { sec: 'overlay', id: 'vol', name: '调音量', note: '每档 5，正好 20 格；约 1.5 秒后消失。', anim: true,
    draw: (px, S, t) => { LAYOUTS[S.layout](px, THEMES[S.theme], P({ state: 'playing' }), t); volumeOverlay(px, THEMES[S.theme], 60); } },
  { sec: 'overlay', id: 'seek', name: '后退 30 秒', note: '连点累计，显示跳到的时间。', anim: true,
    draw: (px, S, t) => { LAYOUTS[S.layout](px, THEMES[S.theme], P({ state: 'playing' }), t); seekOverlay(px, THEMES[S.theme], -60, 342); } },
  { sec: 'overlay', id: 'dirs', name: '选择目录', note: '中/底浏览，顶部确认；底部细线是 5 秒自动取消。', anim: true,
    draw: (px, S, t) => dirPicker(px, THEMES[S.theme], 3, t) },
  { sec: 'overlay', id: 'aino', name: 'AI 暂不可用', note: '双击但 AI 没准备好；故事不停。第一批不写原因。', anim: true,
    draw: (px, S, t) => { const th = THEMES[S.theme]; LAYOUTS[S.layout](px, th, P({ state: 'playing', ai: 'offline' }), t); toast(px, th, null, null, '喵喵暂时不在线'); badge(px, 28, 183); pxt(px, 'z', 50, 175, 1, th.sub); } },
  { sec: 'overlay', id: 'lowbat', name: '电量低', note: '降到 10% 时提示一次，约 5 秒，不出声（P5-4）。', anim: true,
    draw: (px, S, t) => { const th = THEMES[S.theme]; LAYOUTS[S.layout](px, th, P({ state: 'playing', bat: 9 }), t); toast(px, th, 'warn', th.red, '电量低，请充电', '没电前会自动保存'); } },

  { sec: 'ai', id: 'conn', name: '连接中', note: '双击后到真正开始听之前。', anim: true,
    draw: (px, S, t) => aiScreen(px, S.theme, 'connecting', t, { interrupted: true }) },
  { sec: 'ai', id: 'listen', name: '在听（打断了故事）', note: '只有真的打断了正在播的故事，才写「故事已暂停」。', anim: true,
    draw: (px, S, t) => aiScreen(px, S.theme, 'listening', t, { interrupted: true }) },
  { sec: 'ai', id: 'listen2', name: '在听（故事原本暂停）', note: '进入前本来就暂停：不说「已暂停」。', anim: true,
    draw: (px, S, t) => aiScreen(px, S.theme, 'listening', t, { interrupted: false }) },
  { sec: 'ai', id: 'think', name: '想一想', note: '收到孩子说的话、回答还没开始。需要板级接上 STT 消息。', anim: true,
    draw: (px, S, t) => aiScreen(px, S.theme, 'thinking', t, { interrupted: true, subs: S.subs, heard: DEMO.heard }) },
  { sec: 'ai', id: 'speak', name: '在说', note: '嘴两帧开合；字幕开关在页面顶部。', anim: true,
    draw: (px, S, t) => aiScreen(px, S.theme, 'speaking', t, { interrupted: true, subs: S.subs, heard: DEMO.heard, said: DEMO.said }) },
  { sec: 'ai', id: 'mood', name: '说话时的表情', note: '开心、惊讶、难过、困了、思考轮流显示：眼睛按情绪，嘴照常开合。', anim: true,
    draw: (px, S, t) => aiScreen(px, S.theme, 'speaking', t, { interrupted: true, subs: S.subs, heard: DEMO.heard, said: DEMO.said,
      face: ['happy', 'surprised', 'sad', 'sleepy', 'thinking'][Math.floor(t / 2) % 5] }) },
  { sec: 'ai', id: 'exit', name: '回到故事', note: '结束后约 1–2 秒内接着播。', anim: true,
    draw: (px, S, t) => aiScreen(px, S.theme, 'exiting', t, { interrupted: true }) },
  { sec: 'ai', id: 'fail', name: '出错后回到故事', note: '断网、服务出错或对话没能开始时用「出错」脸。', anim: false,
    draw: (px, S, t) => aiScreen(px, S.theme, 'exiting', t, { interrupted: true, failed: true }) },
  { sec: 'ai', id: 'faces', name: '表情', note: '服务器发来的情绪词归到这 6 种脸上。', anim: false,
    draw: (px, S, t) => expressionSheet(px, S.theme, t) },

  { sec: 'system', id: 'check', name: '开机检查卡', note: '同步挂载 SD 卡时的第一帧。', anim: false,
    draw: (px, S) => { const th = THEMES[S.theme]; centerPage(px, th.bg, 'sd', th.blue, 4, '正在检查', ['SD 卡内容…'], th); text(px, 'v1.1.2', 120, 260, { color: th.sub, align: 'center' }); } },
  { sec: 'system', id: 'nocard', name: '没有 SD 卡', note: '第四批起没卡也能和喵喵聊天。', anim: false,
    draw: (px, S) => { const th = THEMES[S.theme]; centerPage(px, th.bg, 'sd', th.sub, 4, '没有 SD 卡', ['请插入或检查 SD 卡'], th); battery(px, th, DEMO.bat, 196, 21); aiHint(px, th, 206); text(px, '长按顶部键关机', 120, 260, { color: th.sub, align: 'center' }); } },
  { sec: 'system', id: 'empty', name: '卡里没有故事', note: '认 MP3 和 WAV（F08）。', anim: false,
    draw: (px, S) => { const th = THEMES[S.theme]; centerPage(px, th.bg, 'box', th.sub, 4, '没有故事', ['卡里没有能播的 MP3', '放在根目录或一级目录'], th); battery(px, th, DEMO.bat, 196, 21); aiHint(px, th, 214); } },
  { sec: 'system', id: 'err', name: '播放出错', note: '读卡或扫描出错。', anim: false,
    draw: (px, S) => { const th = THEMES[S.theme]; centerPage(px, th.bg, 'warn', th.red, 4, '读卡出错', ['请检查 SD 卡内容'], th); battery(px, th, DEMO.bat, 196, 21); aiHint(px, th, 214); } },
  { sec: 'system', id: 'off', name: '正在关机', note: '保存断点，最多约 15 秒。', anim: false,
    draw: (px, S) => { const th = THEMES[S.theme]; centerPage(px, th.bg, 'moon', th.amber, 4, '晚安', ['正在保存进度…'], th);
      for (const [sx, sy] of [[150, 46], [164, 84], [82, 60]]) sprite(px, ['.#.', '###', '.#.'], { '#': th.amber }, sx, sy, 2); } },
  { sec: 'system', id: 'lowoff', name: '电量低，关机', note: '连续约 15 秒不高于 3%：像长按一样保存进度再关机，至少显示约 3 秒（P5-4）。', anim: false,
    draw: (px, S) => { const th = THEMES[S.theme]; centerPage(px, th.bg, 'warn', th.red, 4, '电量低', ['请充电', '正在保存进度…'], th); } },
  { sec: 'system', id: 'updating', name: '正在升级', note: '卡的根目录有更新的版本：写进另一个槽，校验后切换、重启（M7）。', anim: false,
    draw: (px, S) => { const th = THEMES[S.theme]; centerPage(px, th.bg, 'sd', th.blue, 4, '正在升级', ['v1.1.1  42%', '请不要关机'], th); } },
  { sec: 'system', id: 'updatefail', name: '升级没有成功', note: '写入或校验没通过：约 3 秒后照常开机，仍用原来的版本（M7）。', anim: false,
    draw: (px, S) => { const th = THEMES[S.theme]; centerPage(px, th.bg, 'warn', th.red, 4, '升级没有成功', ['仍用原来的版本'], th); } },
  { sec: 'system', id: 'updated', name: '升级完成', note: '升级后第一次开机，约 3 秒，说明现在运行的版本；之后照常开机（M7）。', anim: false,
    draw: (px, S) => { const th = THEMES[S.theme]; centerPage(px, th.bg, 'done', th.green, 4, '升级完成', ['现在是 v1.1.2'], th); } },
  { sec: 'system', id: 'hotspot', name: '配网热点', note: '同时长按中/底约 5 秒打开，10 分钟后关闭。', anim: true,
    draw: (px, S, t) => hotspot(px, THEMES[S.theme], t) },
  { sec: 'system', id: 'code', name: '激活码', note: '现在挤在状态行里，这里单独成页。', anim: false,
    draw: (px, S) => activation(px, THEMES[S.theme]) },
  { sec: 'system', id: 'ok', name: '已激活', note: '短提示，叠在播放页上。', anim: true,
    draw: (px, S, t) => { const th = THEMES[S.theme]; LAYOUTS[S.layout](px, th, P({ state: 'playing' }), t); toast(px, th, 'done', th.green, '已激活', '双击顶部键就能聊天'); } },

  { sec: 'font', id: 'rare', name: '缺字显示方框', note: '「饕餮」「貔貅」不在 6845 字的常用字库里，改为显示方框。', anim: true,
    draw: (px, S, t) => LAYOUTS[S.layout](px, THEMES[S.theme], P({ state: 'playing', collection: '山海经', title: '第7集：饕餮和貔貅的故事.mp3', index: 7, count: 40 }), t) },
];

// ---------- page wiring ----------
const STATE = { theme: 'paper', layout: 'book', scale: 2, safe: false, anim: true, subs: false };
let saved = null;
try { saved = JSON.parse(localStorage.getItem('boilon-p51-v2') || 'null'); } catch (e) { saved = null; }
if (saved && typeof saved === 'object') Object.assign(STATE, saved);
if (!LAYOUTS[STATE.layout]) STATE.layout = 'book';
if (!THEMES[STATE.theme]) STATE.theme = 'paper';
if (window.matchMedia && matchMedia('(prefers-reduced-motion: reduce)').matches) STATE.anim = false;
// Integer scaling only, and never wider than the page.
function fitScale() {
  const room = Math.max(1, Math.floor((document.documentElement.clientWidth - 48) / 256));
  document.documentElement.style.setProperty('--s', Math.min(STATE.scale, room));
}
window.addEventListener('resize', fitScale);

const shots = [];
function build() {
  for (const sc of SCREENS) {
    const host = document.querySelector(`[data-sec="${sc.sec}"]`);
    const fig = document.createElement('figure');
    fig.className = 'shot';
    fig.innerHTML = `<div class="screen"><canvas aria-label="${sc.name}"></canvas><div class="safe" aria-hidden="true"></div></div>
      <figcaption><b>${sc.name}</b><span>${sc.note}</span></figcaption>`;
    host.appendChild(fig);
    const px = new Px(fig.querySelector('canvas'));
    shots.push({ sc, px, fig });
  }
}
function render(t) {
  for (const s of shots) {
    if (t !== 0 && !s.sc.anim) continue;
    s.sc.draw(s.px, STATE, t);
    s.px.flush();
  }
}
function applyControls() {
  fitScale();
  document.body.classList.toggle('show-safe', STATE.safe);
  document.querySelectorAll('[data-key]').forEach((b) => {
    const k = b.dataset.key, v = b.dataset.val;
    const on = v === undefined ? !!STATE[k] : String(STATE[k]) === v;
    b.setAttribute('aria-pressed', on ? 'true' : 'false');
  });
  try { localStorage.setItem('boilon-p51-v2', JSON.stringify(STATE)); } catch (e) { /* ignore */ }
  render(0);
}
document.addEventListener('click', (e) => {
  const b = e.target.closest('[data-key]');
  if (!b) return;
  const k = b.dataset.key, v = b.dataset.val;
  if (v === undefined) STATE[k] = !STATE[k];
  else STATE[k] = k === 'scale' ? Number(v) : v;
  applyControls();
});
build();
applyControls();
let start = performance.now(), last = -1;
function tick(now) {
  const t = (now - start) / 1000;
  const frame = Math.floor(t * 8);
  if (STATE.anim && frame !== last) { last = frame; render(t || 0.001); }
  requestAnimationFrame(tick);
}
requestAnimationFrame(tick);
