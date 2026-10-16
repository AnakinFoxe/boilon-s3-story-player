// Render cases.json with the prototype and write reference.json: the SHA-256 of
// each 240x296 frame as little-endian RGB565, which test_p5_1_ui compares with
// the firmware's drawing code. `node reference.js --png <dir>` writes every
// frame as a PNG instead; `--gallery <dir>` writes the captioned frames at 2x
// with the screen's rounded corners, and their index README.md.
const crypto = require('crypto'), fs = require('fs'), path = require('path'), vm = require('vm'),
  zlib = require('zlib');
const html = fs.readFileSync(path.join(__dirname, 'out/boilon-pixel-ui.html'), 'utf8');
const fontData = html.match(/const FONT_DATA = (\{[\s\S]*?\});\n/)[1];
let js = fs.readFileSync(path.join(__dirname, 'proto.js'), 'utf8');
js = js.slice(0, js.indexOf('// ---------- page wiring ----------'));
const P = {};
vm.createContext(P);
vm.runInContext('const FONT_DATA = ' + fontData + ';\n' + js +
  '\nObject.assign(this, { Px, THEMES, LAYOUTS, volumeOverlay, seekOverlay, toast, badge, pxt, text, dirPicker,' +
  ' centerPage, aiHint, hotspot, activation, aiScreen, sprite, battery });', P);
const canvas = () => ({ getContext: () => ({ createImageData: (w, h) => ({ data: new Uint8ClampedArray(w * h * 4) }), putImageData() {} }) });

function draw(c) {
  const px = new P.Px(canvas()), th = P.THEMES[c.palette];
  const d = { state: c.play, title: c.title, collection: c.collection, index: c.index, count: c.count, pos: c.pos,
    dur: c.dur, vol: c.vol, bat: c.bat, ai: c.ai_ready ? 'ready' : 'offline', mode: c.mode };
  const center = (icon, color, title, lines) => P.centerPage(px, th.bg, icon, color, 4, title, lines, th);
  switch (c.page) {
    case 'player':
      P.LAYOUTS[c.layout === 'cat' ? 'cat' : 'book'](px, th, d, c.t);
      if (c.overlay === 'volume') P.volumeOverlay(px, th, c.vol);
      if (c.overlay === 'seek') P.seekOverlay(px, th, c.seek, c.seek_target);
      if (c.toast === 'ai_offline') {
        P.toast(px, th, null, null, '喵喵暂时不在线');
        P.badge(px, 28, 183);
        P.pxt(px, 'z', 50, 175, 1, th.sub);
      }
      if (c.toast === 'activated') P.toast(px, th, 'done', th.green, '已激活', '双击顶部键就能聊天');
      if (c.toast === 'refused') P.toast(px, th, 'wifi', th.sub, '现在不能配网');
      if (c.toast === 'low_battery') P.toast(px, th, 'warn', th.red, '电量低，请充电', '没电前会自动保存');
      break;
    case 'directory': P.dirPicker(px, th, c.selected, 0, c.directories, c.directory_left_ms / 5000); break;
    case 'checking':
      center('sd', th.blue, '正在检查', ['SD 卡内容…']);
      if (c.version) P.text(px, c.version, 120, 260, { color: th.sub, align: 'center' });
      break;
    case 'nocard':
      center('sd', th.sub, '没有 SD 卡', ['请插入或检查 SD 卡']);
      P.battery(px, th, c.bat, 196, 21);
      if (c.ai_ready) P.aiHint(px, th, 206);
      P.text(px, '长按顶部键关机', 120, 260, { color: th.sub, align: 'center' });
      break;
    case 'empty':
      center('box', th.sub, '没有故事', ['卡里没有能播的 MP3', '放在根目录或一级目录']);
      P.battery(px, th, c.bat, 196, 21);
      if (c.ai_ready) P.aiHint(px, th, 214);
      break;
    case 'error':
      center('warn', th.red, '读卡出错', ['请检查 SD 卡内容']);
      P.battery(px, th, c.bat, 196, 21);
      if (c.ai_ready) P.aiHint(px, th, 214);
      break;
    case 'goodbye':
      center('moon', th.amber, '晚安', ['正在保存进度…']);
      for (const [sx, sy] of [[150, 46], [164, 84], [82, 60]]) P.sprite(px, ['.#.', '###', '.#.'], { '#': th.amber }, sx, sy, 2);
      break;
    case 'lowbattery': center('warn', th.red, '电量低', ['请充电', '正在保存进度…']); break;
    case 'updating':
      if (c.progress < 0) center('warn', th.red, '升级没有成功', ['仍用原来的版本']);
      else center('sd', th.blue, '正在升级', [c.title + '  ' + c.progress + '%', '请不要关机']);
      break;
    case 'updated': center('done', th.green, '升级完成', ['现在是 ' + c.title]); break;
    case 'hotspot': P.hotspot(px, th, 0, c.hotspot, c.hotspot_left_s); break;
    case 'activation': P.activation(px, th, c.code); break;
    case 'ai': {
      const mode = { connecting: 'connecting', listening: 'listening', thinking: 'thinking', speaking: 'speaking',
        returning: 'exiting' }[c.ai];
      P.aiScreen(px, c.palette, mode, c.t, { interrupted: c.interrupted, bat: c.bat, noStory: c.no_story, face: c.face,
        failed: c.failed, subs: c.subs, heard: c.heard, said: c.said });
      break;
    }
    default: throw new Error('unknown page ' + c.page);
  }
  return px.v;
}

// RGBA, each pixel scale x scale; with corners, pixels outside the estimated
// 40 px corner radius are transparent.
function png(v, file, scale = 1, corners = false) {
  const W = 240, H = 296, R = 40, SW = W * scale, SH = H * scale, raw = Buffer.alloc((SW * 4 + 1) * SH);
  for (let y = 0; y < H; y++)
    for (let x = 0; x < W; x++) {
      const dx = x < R ? R - x - 0.5 : x >= W - R ? x - (W - R) + 0.5 : 0;
      const dy = y < R ? R - y - 0.5 : y >= H - R ? y - (H - R) + 0.5 : 0;
      const p = v[y * W + x], alpha = corners && dx * dx + dy * dy > R * R ? 0 : 255;
      const rgba = [Math.round((p >> 11) * 255 / 31), Math.round(((p >> 5) & 63) * 255 / 63), Math.round((p & 31) * 255 / 31), alpha];
      for (let sy = 0; sy < scale; sy++)
        for (let sx = 0; sx < scale; sx++) {
          const o = (y * scale + sy) * (SW * 4 + 1) + 1 + (x * scale + sx) * 4;
          raw[o] = rgba[0]; raw[o + 1] = rgba[1]; raw[o + 2] = rgba[2]; raw[o + 3] = rgba[3];
        }
    }
  const chunk = (type, data) => {
    const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
    const td = Buffer.concat([Buffer.from(type), data]);
    const crc = Buffer.alloc(4); crc.writeUInt32BE(zlib.crc32(td) >>> 0);
    return Buffer.concat([len, td, crc]);
  };
  const ihdr = Buffer.alloc(13); ihdr.writeUInt32BE(SW, 0); ihdr.writeUInt32BE(SH, 4); ihdr[8] = 8; ihdr[9] = 6;
  fs.writeFileSync(file, Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ihdr),
    chunk('IDAT', zlib.deflateSync(raw, { level: 9 })), chunk('IEND', Buffer.alloc(0))]));
}

// The gallery's sections in order; a case joins one with "gallery" and "caption".
const GALLERY = [
  ['player-card', '播放页 · A 故事卡（默认）', '标题优先。状态栏、25 段进度条和时间行是 A、C 共用的；AI 就绪时右上角有喵喵头像；不是顺序播放时状态栏有播放模式图标。'],
  ['title', '播放页 · A 的长标题', '标题先用 28 px（原来的 2 倍字），放不下就 1 px 一档往下缩，用能完整放下的最大字号；最小 18 px，还放不下就在最后一行末尾加「…」。21 px 起笔画 2 格粗，以下 1 格细。'],
  ['player-cat', '播放页 · C 喵喵陪听', '喵喵戴着耳机陪着听，暂停、定位和播完各有样子。'],
  ['overlay', '覆盖提示与选择目录', '调音量、跳转和短提示叠在播放页上，过一会儿自动消失。'],
  ['system', '系统页', '只有本地故事和 AI 都用不了时才用整页。'],
  ['ai', 'AI 对话', '紫蓝底，喵喵 3 倍。只有真的打断了正在播的故事，底部才写「故事已暂停」。'],
  ['ai-frames', 'AI 动画帧', '同一画面的其他帧：眨眼、右耳抖、思考泡依次亮起、嘴开合。'],
  ['faces', '说话时的表情', '服务器发来的情绪词归到这几种脸，只在喵喵说话时用；嘴照常开合。'],
  ['subtitles', '字幕（打开时）', '对喵喵说「打开字幕」：想一想时显示孩子说的话，说话时显示正在说的那一句。'],
  ['night', '夜灯配色', '对喵喵说「换成夜灯」，所有页面换成深蓝底。'],
];
function gallery(dir) {
  fs.mkdirSync(dir, { recursive: true });
  const lines = ['# P5-1 设计图：产品当前形态', '',
    '<!-- 由 `node docs/boilon/p5_1_prototype/reference.js --gallery docs/boilon/p5_1_gallery` 生成，不要手改。 -->', '',
    '设备每个画面的样子：240×296，按 2 倍保存，四角按估计的 40 px 圆角裁掉，示例数据是假的。图由原型画出；',
    '`test_p5_1_ui` 证明固件画出的每一张都与原型逐像素一致。屏幕上的实际颜色以真机为准。默认组合是暖纸配色、',
    'A 故事卡、字幕关；设计说明见 [P5_1_PIXEL_UI.md](../P5_1_PIXEL_UI.md)。', ''];
  for (const [key, title, note] of GALLERY) {
    const shots = cases.filter((c) => c.gallery === key);
    if (!shots.length) throw new Error('empty gallery section ' + key);
    lines.push('## ' + title, '', note, '', '<table>');
    for (let i = 0; i < shots.length; i += 3) {
      lines.push('<tr>');
      for (const c of shots.slice(i, i + 3)) {
        png(draw(c), path.join(dir, c.id + '.png'), 2, true);
        lines.push(`<td align="center"><img src="${c.id}.png" width="240" alt="${c.caption}"><br>${c.caption}</td>`);
      }
      lines.push('</tr>');
    }
    lines.push('</table>', '');
  }
  const unknown = cases.filter((c) => c.gallery && !GALLERY.some(([key]) => key === c.gallery));
  if (unknown.length) throw new Error('unknown gallery section in ' + unknown.map((c) => c.id));
  fs.writeFileSync(path.join(dir, 'README.md'), lines.join('\n'));
  console.log(cases.filter((c) => c.gallery).length + ' gallery frames -> ' + dir);
}

const cases = JSON.parse(fs.readFileSync(path.join(__dirname, 'cases.json'), 'utf8'));
const pngDir = process.argv[2] === '--png' ? process.argv[3] : null;
if (process.argv[2] === '--gallery') {
  gallery(process.argv[3]);
} else if (pngDir) {
  fs.mkdirSync(pngDir, { recursive: true });
  for (const c of cases) png(draw(c), path.join(pngDir, c.id + '.png'));
} else {
  const reference = {};
  for (const c of cases) {
    const v = draw(c);
    reference[c.id] = crypto.createHash('sha256').update(Buffer.from(v.buffer, v.byteOffset, v.byteLength)).digest('hex');
  }
  fs.writeFileSync(path.join(__dirname, 'reference.json'), JSON.stringify(reference, null, 1) + '\n');
  console.log(Object.keys(reference).length + ' reference frames');
}
