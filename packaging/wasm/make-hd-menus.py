#!/usr/bin/env python3
# Makes the HD menu pictures (Setup > Display > Advanced > HD Menus and Backgrounds), which the
# game fetches from beside the page when the box is ticked, from the "Jedi Knight Hi-Res Backgrounds" zip
# (provided by JKLE_Cougar):
# its "4 to 3" and "2.39 to 1" folders, each with 11 PNGs named like the game's ui/bm/bk*.bm (no
# bkDialog). Needs Python 3 with Pillow (WebP) and numpy.
#
#   python3 -I packaging/wasm/make-hd-menus.py <zip> <repo root>
#
# Writes:
# - packaging/wasm/rooms/hdmenu-43-<name>.<8 hex>.webp and hdmenu-wide-<name>.<8 hex>.webp: WebP,
#   quality 90, named by their content (the first 8 hex digits of its SHA-256) like the room
#   pictures, so the site keeps them a year and a kit copies them (the page names them). The 4:3
#   ones taller than 1200 are made 1600x1200 (the 4:3 screens they're for are at most that tall).
#   Earlier hdmenu-* files there are removed.
# - HD_MENU_PICTURES in packaging/wasm/index.html: those names, in the engine's picture order.
# - src/Gui/jkGUIHd_data.h: for each picture, the sizes of both and, for the wide one, the part
#   of it that lies under the game's 640x480 menu (x, y, w, h in its pixels); and each 4:3
#   picture as 16x12 average colours, which the engine compares with the game's own picture so a
#   mod's (or another game's) menu art isn't covered. The wide pictures were made from the 4:3
#   ones and some are a little larger or offset, so the part under the menu is found by matching
#   each one's edges against its 4:3 picture: scales 0.97 to 1.08 by 0.005, positions within
#   12 px (sideways) and 6 px (up and down) of the centre, at 640x480.
import hashlib, io, os, re, sys, zipfile
import numpy as np
from PIL import Image

NAMES = ['bkMain', 'bkSingle', 'bkMulti', 'bkSetup', 'bkEsc', 'bkLoading', 'bkFieldlog', 'bkForce', 'bkTally',
         'bkBuildMulti', 'bkBuildload']
STD_MAX_H = 1200
QUALITY = 90


def load(z, folder, name):
    for i in z.infolist():
        if i.filename.lower() == f'{folder}/{name}.png'.lower():
            return Image.open(io.BytesIO(z.read(i)))
    sys.exit(f'{folder}/{name}.png is not in the zip')


def webp(im):
    b = io.BytesIO()
    im.save(b, 'WEBP', quality=QUALITY, method=6)
    return b.getvalue()


def save(out, prefix, name, data):
    fname = f'hdmenu-{prefix}-{name.lower()}.{hashlib.sha256(data).hexdigest()[:8]}.webp'
    with open(os.path.join(out, fname), 'wb') as f:
        f.write(data)
    return 'rooms/' + fname


def edges(a):
    gx = np.zeros_like(a)
    gy = np.zeros_like(a)
    gx[:, 1:] = a[:, 1:] - a[:, :-1]
    gy[1:, :] = a[1:, :] - a[:-1, :]
    return np.abs(gx) + np.abs(gy)


def menu_part(std, wide):
    W, H = 640, 480
    ga = edges(np.asarray(std.convert('L').resize((W, H), Image.LANCZOS), dtype=np.float32))
    g = wide.convert('L')
    best = None
    for s in np.arange(0.97, 1.081, 0.005):
        h = int(round(H * s))
        w = int(round(g.width * h / g.height))
        gb = edges(np.asarray(g.resize((w, h), Image.BILINEAR), dtype=np.float32))
        cx, cy = (w - W) // 2, (h - H) // 2
        for oy in range(max(0, cy - 6), min(h - H, cy + 6) + 1):
            for ox in range(max(0, cx - 12), min(w - W, cx + 12) + 1):
                d = float(np.mean(np.abs(gb[oy:oy + H, ox:ox + W] - ga)))
                if best is None or d < best[0]:
                    best = (d, ox, oy, h)
    d, ox, oy, h = best
    k = g.height / h
    return [round(ox * k, 1), round(oy * k, 1), round(W * k, 1), round(H * k, 1)]


def main():
    zpath, root = sys.argv[1], sys.argv[2]
    out = os.path.join(root, 'packaging', 'wasm', 'rooms')
    for f in os.listdir(out):
        if f.startswith('hdmenu-'):
            os.remove(os.path.join(out, f))
    z = zipfile.ZipFile(zpath)
    rows = []
    names = {'43': [], 'wide': []}
    for n in NAMES:
        std = load(z, '4 to 3', n).convert('RGB')
        wide = load(z, '2.39 to 1', n).convert('RGB')
        t = np.asarray(std.resize((640, 480), Image.BOX), dtype=np.float64).reshape(12, 40, 16, 40, 3).mean(axis=(1, 3))
        thumb = [int(round(v)) for v in t.flatten()]
        if std.height > STD_MAX_H:
            std = std.resize((STD_MAX_H * 4 // 3, STD_MAX_H), Image.LANCZOS)
        part = menu_part(std, wide)
        names['43'].append(save(out, '43', n, webp(std)))
        names['wide'].append(save(out, 'wide', n, webp(wide)))
        rows.append((n.lower(), std.size, wide.size, part, thumb))
        print(n, std.size, wide.size, part, file=sys.stderr)

    with open(os.path.join(root, 'src', 'Gui', 'jkGUIHd_data.h'), 'w') as f:
        f.write('// Made by packaging/wasm/make-hd-menus.py with the HD pictures (packaging/wasm/rooms/hdmenu-*).\n')
        f.write('// Each picture: its name, the 4:3 one\'s width and height, the wide one\'s width and height\n'
                '// and the part of it under the 640x480 menu (x, y, w, h in its pixels).\n')
        f.write('static const jkGuiHdPictureInfo jkGuiHd_aPictureInfo[JKGUIHD_NUM_PICTURES] = {\n')
        for name, ss, ws, part, _ in rows:
            f.write(f'    {{ "{name}", {ss[0]}, {ss[1]}, {ws[0]}, {ws[1]}, {{ {part[0]}f, {part[1]}f, {part[2]}f, {part[3]}f }} }},\n')
        f.write('};\n\n')
        f.write('// Each 4:3 picture as 16x12 average colours (RGB, rows top to bottom), compared with the\n'
                '// game\'s own picture (jkGUIHd.c).\n')
        f.write('static const uint8_t jkGuiHd_aThumbs[JKGUIHD_NUM_PICTURES][16*12*3] = {\n')
        for name, _, _, _, v in rows:
            f.write('    { // ' + name + '\n')
            for r in range(12):
                f.write('        ' + ','.join(str(x) for x in v[r * 48:(r + 1) * 48]) + ',\n')
            f.write('    },\n')
        f.write('};\n')

    page = os.path.join(root, 'packaging', 'wasm', 'index.html')
    html = open(page, encoding='utf-8').read()
    m = re.search(r"( *)var HD_MENU_PICTURES = \{.*?\n *\};\n", html, re.S)
    if not m:
        sys.exit('index.html has no HD_MENU_PICTURES')
    ind = m.group(1)
    block = ind + 'var HD_MENU_PICTURES = {\n' + ',\n'.join(
        ind + f"  '{k}': [\n" + ',\n'.join(ind + f"    '{v}'" for v in names[k]) + '\n' + ind + '  ]'
        for k in ('43', 'wide')) + '\n' + ind + '};\n'
    with open(page, 'w', encoding='utf-8', newline='') as f:
        f.write(html[:m.start()] + block + html[m.end():])


main()
