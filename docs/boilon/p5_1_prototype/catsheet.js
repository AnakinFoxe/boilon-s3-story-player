// Quick look at the cat alone at 4x on both backgrounds.
const fs = require('fs'), vm = require('vm'), zlib = require('zlib');
const html = fs.readFileSync(__dirname + '/out/boilon-pixel-ui.html', 'utf8');
const fontData = html.match(/const FONT_DATA = (\{[\s\S]*?\});\n/)[1];
let js = fs.readFileSync(__dirname + '/proto.js', 'utf8');
js = js.slice(0, js.indexOf('// ---------- page wiring ----------'));
const ctx = {}; vm.createContext(ctx);
vm.runInContext('const FONT_DATA = ' + fontData + ';\n' + js + '\nthis.catGrid = catGrid; this.CAT_COLORS = CAT_COLORS;', ctx);
const faces = [{}, { eyes: 'happy' }, { eyes: 'wide', mouth: 'o' }, { eyes: 'sad', mouth: 'sad', droop: true }, { eyes: 'sleep' }, { eyes: 'up', mouth: 'flat' }, { eyes: 'x', mouth: 'flat' }, { mouth: 'open' }];
const S = 5, bgs = ['#efdcb9', '#2c2e86'];
const grids = faces.map((f) => ctx.catGrid(f));
const cw = 41 * S + 20, chh = 46 * S + 20, W = cw * faces.length, H = chh * 2;
const img = Buffer.alloc(W * H * 4);
const hex = (h) => [parseInt(h.slice(1, 3), 16), parseInt(h.slice(3, 5), 16), parseInt(h.slice(5, 7), 16)];
for (let row = 0; row < 2; row++) for (let i = 0; i < faces.length; i++) {
  const [br, bg, bb] = hex(bgs[row]);
  for (let y = 0; y < chh; y++) for (let x = 0; x < cw; x++) { const o = ((row * chh + y) * W + i * cw + x) * 4; img[o] = br; img[o + 1] = bg; img[o + 2] = bb; img[o + 3] = 255; }
  grids[i].forEach((r, gy) => [...r].forEach((k, gx) => {
    if (k === '.') return; const [r1, g1, b1] = hex(ctx.CAT_COLORS[k]);
    for (let dy = 0; dy < S; dy++) for (let dx = 0; dx < S; dx++) { const o = ((row * chh + 10 + gy * S + dy) * W + i * cw + 10 + gx * S + dx) * 4; img[o] = r1; img[o + 1] = g1; img[o + 2] = b1; }
  }));
}
const raw = Buffer.alloc((W * 4 + 1) * H);
for (let y = 0; y < H; y++) img.copy(raw, y * (W * 4 + 1) + 1, y * W * 4, (y + 1) * W * 4);
const chunk = (t, d) => { const l = Buffer.alloc(4); l.writeUInt32BE(d.length); const td = Buffer.concat([Buffer.from(t), d]); const c = Buffer.alloc(4); c.writeUInt32BE(zlib.crc32(td) >>> 0); return Buffer.concat([l, td, c]); };
const ih = Buffer.alloc(13); ih.writeUInt32BE(W, 0); ih.writeUInt32BE(H, 4); ih[8] = 8; ih[9] = 6;
fs.writeFileSync(__dirname + '/out/cats.png', Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ih), chunk('IDAT', zlib.deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]));
console.log('cats.png', W, H);
