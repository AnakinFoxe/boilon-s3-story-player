"""Embed the glyphs used by proto.js into one self-contained HTML page."""
import json
import sys
from pathlib import Path

from fontx import Cbin, common_font_bytes, unscii8

HERE = Path(__file__).resolve().parent


def pack(g):
    bits = ''.join(g['rows'])
    hexs = format(int(bits, 2), 'x').zfill((len(bits) + 3) // 4) if bits else ''
    return [g['adv'], g['w'], g['h'], g['x'], g['y'], hexs]


def main():
    js = (HERE / 'proto.js').read_text()
    page = (HERE / 'page.html').read_text()
    noto = Cbin(common_font_bytes())
    chars = sorted(set(js) | set(page) | {chr(c) for c in range(32, 127)} | {'?', '…'})
    glyphs, missing = {}, []
    for ch in chars:
        if ch in '\n\r\t':
            continue
        g = noto.glyph(ord(ch))
        if g is None:
            missing.append(ch)
            continue
        glyphs[ch] = pack(g)
    px = {ch: pack(g) for ch, g in unscii8().items()}
    data = {
        'noto': {'lh': noto.line_height, 'base': noto.base_line, 'g': glyphs},
        'px': {'lh': 9, 'base': 0, 'g': px},
    }
    out = page.replace('/*FONT_DATA*/', 'const FONT_DATA = ' + json.dumps(data, ensure_ascii=False,
                                                                         separators=(',', ':')) + ';')
    out = out.replace('/*PROTO_JS*/', js)
    target = HERE / 'out' / 'boilon-pixel-ui.html'
    target.parent.mkdir(exist_ok=True)
    target.write_text(out)
    cjk_missing = [c for c in missing if ord(c) > 0x2e80]
    print(f'{len(glyphs)} noto glyphs, {len(px)} px glyphs, {len(out)} bytes -> {target}')
    print('missing (drawn as empty boxes):', ''.join(cjk_missing))


if __name__ == '__main__':
    sys.exit(main())
