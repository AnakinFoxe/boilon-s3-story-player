// Headless contact sheet: runs proto.js drawing code and writes a PNG.
const fs = require('fs'), zlib = require('zlib'), vm = require('vm');
const html = fs.readFileSync(__dirname + '/out/boilon-pixel-ui.html', 'utf8');
const fontData = html.match(/const FONT_DATA = (\{[\s\S]*?\});\n/)[1];
let js = fs.readFileSync(__dirname + '/proto.js', 'utf8');
js = js.slice(0, js.indexOf('// ---------- page wiring ----------'));
const ctx = { console };
vm.createContext(ctx);
vm.runInContext('const FONT_DATA = ' + fontData + ';\n' + js + '\nthis.SCREENS = SCREENS; this.Px = Px;', ctx);
const { SCREENS, Px } = ctx;
const fakeCanvas = () => ({ getContext: () => ({ createImageData: (w, h) => ({ data: new Uint8ClampedArray(w * h * 4) }), putImageData() {} }) });

function sheet(ids, state, t, scale, cols, out) {
  const W = 240, H = 296, gap = 12 * scale, R = 40;
  const list = ids.map((id) => SCREENS.find((s) => s.id === id));
  const rows = Math.ceil(list.length / cols);
  const SW = cols * W * scale + (cols + 1) * gap, SH = rows * H * scale + (rows + 1) * gap;
  const img = Buffer.alloc(SW * SH * 4);
  for (let i = 0; i < SW * SH; i++) img.writeUInt32BE(0x2a2c38ff, i * 4);
  list.forEach((sc, i) => {
    const px = new Px(fakeCanvas());
    sc.draw(px, state, t);
    const ox = gap + (i % cols) * (W * scale + gap), oy = gap + Math.floor(i / cols) * (H * scale + gap);
    for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
      const dx = x < R ? R - x - 0.5 : x >= W - R ? x - (W - R) + 0.5 : 0;
      const dy = y < R ? R - y - 0.5 : y >= H - R ? y - (H - R) + 0.5 : 0;
      const inside = dx * dx + dy * dy <= R * R;
      const v = px.u[y * W + x];
      const r = v & 255, g = (v >> 8) & 255, b = (v >> 16) & 255;
      for (let sy = 0; sy < scale; sy++) for (let sx = 0; sx < scale; sx++) {
        const o = ((oy + y * scale + sy) * SW + ox + x * scale + sx) * 4;
        if (inside) { img[o] = r; img[o + 1] = g; img[o + 2] = b; } else { img[o] = 8; img[o + 1] = 9; img[o + 2] = 14; }
        img[o + 3] = 255;
      }
    }
  });
  const raw = Buffer.alloc((SW * 4 + 1) * SH);
  for (let y = 0; y < SH; y++) { raw[y * (SW * 4 + 1)] = 0; img.copy(raw, y * (SW * 4 + 1) + 1, y * SW * 4, (y + 1) * SW * 4); }
  const chunk = (type, data) => {
    const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
    const td = Buffer.concat([Buffer.from(type), data]);
    const crc = Buffer.alloc(4); crc.writeUInt32BE(zlib.crc32(td) >>> 0);
    return Buffer.concat([len, td, crc]);
  };
  const ihdr = Buffer.alloc(13); ihdr.writeUInt32BE(SW, 0); ihdr.writeUInt32BE(SH, 4); ihdr[8] = 8; ihdr[9] = 6;
  fs.writeFileSync(out, Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ihdr),
    chunk('IDAT', zlib.deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]));
  console.log(out, SW + 'x' + SH);
}
const [,, idsArg, theme = 'night', layout = 'book', subs = '0', t = '0.4', scale = '2', cols = '4', out = 'sheet.png'] = process.argv;
sheet(idsArg.split(','), { theme, layout, scale: 2, safe: false, anim: true, subs: subs === '1' }, Number(t), Number(scale), Number(cols), __dirname + '/out/' + out);
