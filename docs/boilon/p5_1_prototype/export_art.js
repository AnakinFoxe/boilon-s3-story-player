// Print the prototype's generated pixel art as C++ string rows for
// main/boards/boilon-s3-gen2/p5_1_ui_art.h: the cat without eyes (the
// firmware draws the eyes and mouths), its moving ears and the status-bar badge.
const fs = require('fs'), vm = require('vm');
const html = fs.readFileSync(__dirname + '/out/boilon-pixel-ui.html', 'utf8');
const fontData = html.match(/const FONT_DATA = (\{[\s\S]*?\});\n/)[1];
let js = fs.readFileSync(__dirname + '/proto.js', 'utf8');
js = js.slice(0, js.indexOf('// ---------- page wiring ----------'));
const ctx = {};
vm.createContext(ctx);
vm.runInContext('const FONT_DATA = ' + fontData + ';\n' + js + '\nthis.catGrid = catGrid; this.BADGE = BADGE; this.THEMES = THEMES; this.AIPAL = AIPAL; this.CAT_COLORS = CAT_COLORS; this.ICONS = ICONS;', ctx);
const rows = (name, list) => `constexpr const char* ${name}[] = {\n` + list.map((r) => `    "${r}",`).join('\n') + '\n};';
console.log(rows('kCatRows', ctx.catGrid({ eyes: 'none', mouth: 'w' })));
console.log(rows('kBadgeRows', ctx.BADGE));
// The ears that move (right ear twitching, both drooping) as patches over
// kCatRows: '.' keeps the pixel, '_' becomes the background.
for (const [name, o] of [['kTwitchRows', { twitch: true }], ['kDroopRows', { droop: true }]]) {
  const base = ctx.catGrid({ eyes: 'none', mouth: 'w' }), ears = ctx.catGrid({ eyes: 'none', mouth: 'w', ...o });
  let x0 = 99, x1 = -1, y0 = 99, y1 = -1;
  base.forEach((r, y) => [...r].forEach((k, x) => {
    if (k !== ears[y][x]) { x0 = Math.min(x0, x); x1 = Math.max(x1, x); y0 = Math.min(y0, y); y1 = Math.max(y1, y); }
  }));
  const patch = [];
  for (let y = y0; y <= y1; y++) {
    let line = '';
    for (let x = x0; x <= x1; x++) line += base[y][x] === ears[y][x] ? '.' : ears[y][x] === '.' ? '_' : ears[y][x];
    patch.push(line);
  }
  console.log(`// at grid (${x0}, ${y0})`);
  console.log(rows(name, patch));
}
// Palettes in the order of enum Color in p5_1_ui_art.h.
const order = [['th', 'bg'], ['th', 'panel'], ['th', 'line'], ['th', 'shadow'], ['th', 'text'], ['th', 'sub'],
  ['th', 'faint'], ['th', 'amber'], ['th', 'green'], ['th', 'red'], ['th', 'blue'],
  ['ai', 'bg'], ['ai', 'panel'], ['ai', 'line'], ['ai', 'shadow'], ['ai', 'text'], ['ai', 'sub'], ['ai', 'glow'],
  ['ai', 'amber'], ['ai', 'green'], ['ai', 'red'], ['ai', 'faint'],
  ...'kwompcehb'.split('').map((k) => ['cat', k])];
for (const [name, key] of [['kPaperRgb', 'paper'], ['kNightRgb', 'night']]) {
  const src = { th: ctx.THEMES[key], ai: ctx.AIPAL[key], cat: ctx.CAT_COLORS };
  const values = order.map(([set, k]) => '0x' + src[set][k].slice(1));
  console.log(`constexpr uint32_t ${name}[kColorCount] = {`);
  for (let i = 0; i < values.length; i += 8) console.log('    ' + values.slice(i, i + 8).join(', ') + ',');
  console.log('};');
}
for (const name of ['play', 'pause', 'hourglass', 'done', 'warn', 'folder', 'speaker', 'mute', 'wifi', 'sd', 'box', 'moon', 'ff', 'rew', 'note'])
  console.log(`${name}: ${JSON.stringify(ctx.ICONS[name])}`);
