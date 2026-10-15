"""Extract 1-bpp glyph bitmaps from the device CBIN font and LVGL unscii-8."""
import re, struct, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

class Cbin:
    def __init__(self, data):
        b = self.b = data
        self.line_height, self.base_line = struct.unpack_from('<ii', b, 12)
        dsc = struct.unpack_from('<I', b, 24)[0]
        self.base = dsc
        gb, gd, cm, kd = struct.unpack_from('<IIII', b, dsc)
        kern_scale, bits = struct.unpack_from('<HH', b, dsc + 16)
        self.cmap_num = bits & 0x1ff
        self.bpp = (bits >> 9) & 0xf
        self.fmt = (bits >> 14) & 3
        self.stride = b[dsc + 20]
        self.gb, self.gd = dsc + gb, dsc + gd
        cur = dsc + cm
        self.cmaps_addr = cur
        self.cmaps = []
        for _ in range(self.cmap_num):
            rs, rl, gs, ul, ol, ll, ty = struct.unpack_from('<IHHIIHB', b, cur)
            self.cmaps.append((rs, rl, gs, ul, ol, ll, ty))
            cur += 20

    def glyph_id(self, cp):
        b = self.b
        for rs, rl, gs, ul, ol, ll, ty in self.cmaps:
            rcp = cp - rs
            if rcp < 0 or rcp >= rl:
                continue
            if ty == 2:  # format0 tiny
                return gs + rcp
            if ty == 0:  # format0 full
                return gs + b[self.cmaps_addr + ol + rcp]
            ulist = [struct.unpack_from('<H', b, self.cmaps_addr + ul + 2 * i)[0] for i in range(ll)]
            if rcp in ulist:
                i = ulist.index(rcp)
                if ty == 3:
                    return gs + i
                return gs + struct.unpack_from('<H', b, self.cmaps_addr + ol + 2 * i)[0]
        return None

    def glyph(self, cp):
        gid = self.glyph_id(cp)
        if gid is None:
            return None
        # CONFIG_LV_FONT_FMT_TXT_LARGE=y: 16-byte glyph descriptors.
        index, adv, bw, bh, ox, oy = struct.unpack_from('<IIHHhh', self.b, self.gd + 16 * gid)
        bits = []
        for i in range(bw * bh):
            byte = self.b[self.gb + index + i // 8]
            bits.append(1 if byte & (0x80 >> (i % 8)) else 0)
        return dict(adv=(adv + 8) // 16, w=bw, h=bh, x=ox, y=oy,
                    rows=[''.join(str(bits[r * bw + c]) for c in range(bw)) for r in range(bh)])

def unscii8():
    """unscii-8 from the board header the firmware uses (see gen_unscii.py)."""
    src = (ROOT / 'main/boards/boilon-s3-gen2/p5_1_ui_font.h').read_text()
    boxes = [tuple(int(v) for v in m) for m in re.findall(
        r'\{(\d+), (\d+), (\d+), (-?\d+), (-?\d+), (\d+)\}', src)]
    bits_src = src[src.index('kBits[] = {'):]
    data = [int(x, 16) for x in re.findall(r'0x[0-9a-fA-F]+', bits_src[:bits_src.index('};')])]
    out = {}
    for cp in range(32, 127):
        adv, bw, bh, ox, oy, offset = boxes[cp - 32]
        bits = []
        for i in range(bw * bh):
            bits.append(1 if data[offset + i // 8] & (0x80 >> (i % 8)) else 0)
        out[chr(cp)] = dict(adv=adv, w=bw, h=bh, x=ox, y=oy,
                            rows=[''.join(str(bits[r * bw + c]) for c in range(bw)) for r in range(bh)])
    return out


def common_font_bytes():
    """The device's Noto CBIN, from the board's mechanical xxd copy."""
    src = (ROOT / 'main/boards/boilon-s3-gen2/m2_library_common_font.inc').read_text()
    return bytes(int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]{2})', src))


if __name__ == '__main__':
    f = Cbin(common_font_bytes())
    print('line', f.line_height, 'base', f.base_line, 'bpp', f.bpp, 'fmt', f.fmt, 'stride', f.stride, 'cmaps', f.cmap_num, file=sys.stderr)
    for ch in sys.argv[1:]:
        for c in ch:
            g = f.glyph(ord(c))
            print(c, g and {k: v for k, v in g.items() if k != 'rows'}, file=sys.stderr)
            if g:
                for r in g['rows']:
                    print('   ' + r.replace('0', '.').replace('1', '#'), file=sys.stderr)
