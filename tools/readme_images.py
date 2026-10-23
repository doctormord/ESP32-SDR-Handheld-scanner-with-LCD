#!/usr/bin/env python3
"""Draws the annotated page mockups in images/ for the README.

The pages are redrawn after the firmware's layout (wifi_ui.c) at the
JC3248W535's 480x320 (the T-Display shows the same at 240x135) with made-up
but typical data; numbered markers point at the parts the README explains.
Pillow's default font is Aileron, the same font the firmware's smooth digits
come from (tools/gen_fonts.py).

    python tools/readme_images.py            # writes images/page-*.png
"""
import math
import os
import random

from PIL import Image, ImageDraw, ImageFont

W, H = 480, 320
S = 2                                   # UI_SCALE on the JC3248: P(x) = 2x
OUT = os.path.join(os.path.dirname(__file__), '..', 'images')

C_BG = (0, 0, 0)
C_BAR = (24, 24, 48)
C_TEXT = (220, 220, 220)
C_DIM = (110, 110, 110)
C_GRID = (40, 40, 40)
C_YELLOW = (255, 220, 0)
C_RED = (255, 40, 40)
C_GREEN = (40, 220, 40)
C_CYAN = (0, 200, 230)
C_ORANGE = (255, 140, 0)
C_BLE = (230, 60, 230)
C_WIFI = (0, 0, 44)
C_COL = (18, 18, 26)
C_BEST = (0, 48, 0)
C_WHITE = (255, 255, 255)
CLASSIC = [(0, 0, 30), (0, 0, 150), (0, 170, 220), (240, 230, 0), (255, 40, 0), (255, 255, 255)]

random.seed(7)


def font(px):
    return ImageFont.load_default(size=px)


TXT = font(17)
BIG = font(50)


def P(x):
    return x * S


def lut(v):
    """Classic theme: 0..1 -> colour."""
    v = max(0.0, min(1.0, v)) * (len(CLASSIC) - 1)
    i = min(int(v), len(CLASSIC) - 2)
    f = v - i
    a, b = CLASSIC[i], CLASSIC[i + 1]
    return tuple(int(a[k] + (b[k] - a[k]) * f) for k in range(3))


def air_colour(a):
    t = min(1.0, a / 0.8)
    r = int(60 + 390 * t) if t < 0.5 else 255
    g = 220 if t < 0.5 else int(220 - 360 * (t - 0.5))
    return (min(r, 255), max(g, 0), 40)


def rssi_colour(r):
    return C_GREEN if r >= -60 else C_YELLOW if r >= -75 else C_ORANGE


def text(d, x, y, s, c, f=TXT, anchor='la'):
    d.text((x, y), s, fill=c, font=f, anchor=anchor)


def header(d, title, mid, mid_c, right, best=None):
    """As the firmware's header_row(): status right after the title."""
    d.rectangle([0, 0, W, P(10) - 1], fill=C_BAR)
    text(d, P(2), 1, title, C_YELLOW)
    x = P(2) + d.textlength(title, font=TXT) + P(7)
    text(d, x, 1, mid, mid_c)
    if best:
        text(d, x + d.textlength(mid, font=TXT) + P(6), 1, best, C_GREEN)
    text(d, W - P(2), 1, right, C_DIM, anchor='ra')


def ch_x(ch, f0=2400.0, f1=2484.0):
    return (2407 + 5 * ch - f0) * W / (f1 - f0)


def mhz_x(f, f0=2400.0, f1=2484.0):
    return (f - f0) * W / (f1 - f0)


def frame(screen, marks, name):
    """Screen on a dark bezel with numbered markers: on the screen at (x, y),
    or for a whole row in the left margin at height y (x = None)."""
    ml, m = 34, 14
    img = Image.new('RGB', (W + ml + m, H + 2 * m), (30, 30, 34))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([2, 2, W + ml + m - 3, H + 2 * m - 3], 12, fill=(52, 52, 56))
    img.paste(screen, (ml, m))
    f = font(14)
    for n, (x, y) in enumerate(marks, 1):
        cx, cy = (ml // 2 if x is None else ml + x), m + y
        d.ellipse([cx - 11, cy - 11, cx + 11, cy + 11], fill=(255, 220, 0), outline=(0, 0, 0), width=2)
        d.text((cx, cy), str(n), fill=(0, 0, 0), font=f, anchor='mm')
    img.save(os.path.join(OUT, name))
    print('wrote', name)


# --------------------------------------------------------------------------- WATERFALL
def page_waterfall():
    img = Image.new('RGB', (W, H), C_BG)
    d = ImageDraw.Draw(img)
    header(d, 'WASSERFALL VOLL', '7 Netze', C_CYAN, '1/5')
    gy, gh = P(10), P(34)
    wy, ay = gy + gh + 1, H - P(10)
    aps = [(1, 0.55, 0.9), (6, 0.8, 0.95), (11, 0.15, 0.55), (3, 0.08, 0.35)]
    # Phosphor spectrum: every snapshot adds a little where its trace lies,
    # so levels that keep coming back glow hot and rare peaks stay faint.
    heat = [[0.0] * gh for _ in range(W)]
    for x in range(W):
        f = 2400 + (x + 0.5) * 84 / W
        for _ in range(60):
            lvl = 0.10 + 0.04 * random.gauss(0, 1)
            for ch, busy, strength in aps:
                c = 2407 + 5 * ch
                if abs(f - c) < 9.5 and random.random() < busy:
                    edge = 1 - (abs(f - c) / 10) ** 6
                    lvl = max(lvl, strength * edge * (0.75 + 0.2 * random.random()))
            y = int((1 - max(0, min(0.98, lvl))) * gh)
            for yy in range(y, gh):
                heat[x][yy] += 1.0 if yy == y else 0.04
    for x in range(W):
        for y in range(gh):
            v = heat[x][y] / 14
            if v > 0.02:
                img.putpixel((x, gy + y), lut(min(1, v)))
    d.line([0, wy - 1, W, wy - 1], fill=C_BAR)
    # Occupancy waterfall: each row is one ~50 ms step; a channel lights where
    # its access point was caught sending, brighter for stronger frames.
    rows = ay - wy
    for r in range(rows):
        y = wy + r
        for ch, busy, strength in aps:
            if random.random() < busy * 0.7:
                v = strength * (0.45 + 0.5 * random.random())
                for x in range(int(mhz_x(2407 + 5 * ch - 9)), int(mhz_x(2407 + 5 * ch + 9))):
                    if random.random() < 0.9:
                        img.putpixel((x, y), lut(v * (0.85 + 0.15 * random.random())))
        for f in (2402, 2426, 2480):
            if random.random() < 0.04:
                x = mhz_x(f)
                d.line([x - 2, y, x + 2, y], fill=lut(0.55))
        if random.random() < 0.3:
            x = random.randrange(W)
            d.point((x, y), fill=lut(0.2))
    # Best channel at the foot of the phosphor plot; fixed channel numbers.
    tw = d.textlength('best', font=TXT)
    d.rectangle([ch_x(11) - tw / 2 - P(2), wy - 1 - P(10), ch_x(11) + tw / 2 + P(2), wy - 2], fill=C_BG)
    text(d, ch_x(11), wy - 1 - P(10), 'best', C_GREEN, anchor='ma')
    for ch in range(1, 14):
        text(d, ch_x(ch), ay + P(1), str(ch), C_CYAN if ch in (1, 6, 11) else C_DIM, anchor='ma')
    for f in (2402, 2426, 2480):
        d.rectangle([mhz_x(f) - 2, ay, mhz_x(f) + 2, ay + 1], fill=C_BLE)
    frame(img, [(None, P(5)), (mhz_x(2424), gy + P(26)), (mhz_x(2437), wy + 60), (mhz_x(2480) - 16, wy + 110), (None, ay + P(6)), (ch_x(11) + 40, wy - P(6))],
          'page-waterfall.png')


# --------------------------------------------------------------------------- NETWORKS
def page_networks():
    img = Image.new('RGB', (W, H), C_BG)
    d = ImageDraw.Draw(img)
    header(d, 'NETZE', '7 Netze', C_CYAN, '2/5')
    nets = [(6, 'FRITZ!Box 7590 XY', -41, 0.71), (1, 'Vodafone-2G', -55, 0.42),
            (6, 'eduroam', -63, 0.71), (11, 'Telekom_FON', -68, 0.12), (3, 'DIRECT-7A-HP Laser', -71, 0.06),
            (1, '<versteckt>', -79, 0.42), (11, 'iPhone von Alex', -84, 0.12)]
    for i, (ch, name, rssi, air) in enumerate(nets):
        y = P(12) + i * P(10)
        text(d, P(12), y, str(ch), C_CYAN if ch in (1, 6, 11) else C_DIM, anchor='ra')
        text(d, P(15), y, name, C_DIM if name.startswith('<') else C_TEXT)
        text(d, P(156), y, str(rssi), rssi_colour(rssi), anchor='ra')
        text(d, P(184), y, f'{round(air * 100)}%', air_colour(air), anchor='ra')
        d.rectangle([P(188), y + P(1), P(188) + max(2, int(air * P(36))), y + P(7)], fill=air_colour(air))
    frame(img, [(16, 196), (90, 196), (300, 196), (352, 196), (400, 196)],
          'page-networks.png')


# --------------------------------------------------------------------------- CHANNELS
def page_channels():
    img = Image.new('RGB', (W, H), C_BG)
    d = ImageDraw.Draw(img)
    header(d, 'KANAELE WIFI', '7 Netze', C_CYAN, '3/5')
    cw = W // 13
    x0 = (W - cw * 13) // 2
    bar_y, bar_h = P(22), P(88)
    air = {1: 0.42, 2: 0.2, 3: 0.08, 4: 0.05, 5: 0.18, 6: 0.71, 7: 0.33, 8: 0.1, 11: 0.12, 12: 0.04}
    peak = {1: 0.68, 6: 0.93, 11: 0.31, 3: 0.2, 7: 0.45}
    aps = {1: 2, 3: 1, 6: 2, 11: 2}
    for i in range(13):
        c = i + 1
        x = x0 + i * cw
        a = air.get(c, 0.0)
        main = c in (1, 6, 11)
        d.rectangle([x + P(1), bar_y, x + cw - P(1) - 1, bar_y + bar_h - 1],
                    fill=C_BEST if c == 11 else C_WIFI if main else C_COL)
        for q in (0.25, 0.5, 0.75):
            yy = bar_y + bar_h - int(q * bar_h)
            d.line([x + P(1), yy, x + cw - P(1) - 1, yy], fill=C_GRID)
        ah = int(a * bar_h)
        if ah:
            d.rectangle([x + P(3), bar_y + bar_h - ah, x + cw - P(3) - 1, bar_y + bar_h - 1], fill=air_colour(a))
        if c in peak:
            yy = bar_y + bar_h - int(peak[c] * bar_h)
            d.rectangle([x + P(2), yy, x + cw - P(2) - 1, yy + P(2) - 1], fill=C_WHITE)
        if c in aps:
            text(d, x + cw / 2, P(12), str(aps[c]), C_TEXT, anchor='ma')
        text(d, x + cw / 2, bar_y + bar_h + P(3), str(round(a * 100)), air_colour(a), anchor='ma')
        text(d, x + cw / 2, bar_y + bar_h + P(13), str(c), C_CYAN if main else C_DIM, anchor='ma')
    xc = lambda c: x0 + (c - 1) * cw + cw / 2
    frame(img, [(None, P(16)), (xc(6), bar_y + bar_h - 50), (xc(5), bar_y + bar_h - int(0.93 * bar_h) + 2), (xc(11), bar_y + 30), (None, bar_y + bar_h + P(8)), (None, bar_y + bar_h + P(18))],
          'page-channels.png')


# --------------------------------------------------------------------------- DETEKTOR
def page_detector():
    img = Image.new('RGB', (W, H), C_BG)
    d = ImageDraw.Draw(img)
    header(d, 'DETEKTOR PEAKS', 'genullt +4dB G34 1:12', C_GREEN, 'S2/4')
    text(d, P(4), P(10) + P(5), '+18', C_YELLOW, BIG)
    text(d, P(8) + d.textlength('+18', font=BIG), P(10) + P(20), 'dB', C_DIM)
    text(d, P(110), P(10) + P(6), 'Spitze +27 dB', C_TEXT)
    text(d, P(110), P(10) + P(20), 'Treffer/s 14', C_YELLOW)
    cy = P(10) + P(40) + P(2)
    ch = H - cy - P(12)
    # Channelizer waterfall: the room faint, a nearby phone/BLE device bright.
    for r in range(ch):
        y = cy + r
        for x in range(W):
            f = 2402 + x * 80 / W
            v = 0
            if abs(f - 2437) < 9 and random.random() < 0.6:
                v = 0.12 + 0.06 * random.random()
            if abs(f - 2412) < 9 and random.random() < 0.4:
                v = 0.1
            if v:
                d.point((x, y), fill=lut(v))
        if r < ch * 0.55 and random.random() < 0.35:
            f = random.choice([2404, 2410, 2418, 2428, 2440, 2452, 2466, 2474, 2426, 2480])
            x = mhz_x(f, 2402, 2482)
            d.line([x - 3, y, x + 3, y], fill=lut(0.55 + 0.4 * random.random()))
    for f, lab, ble in ((2402, '37', True), (2412, '1', False), (2426, '38', True), (2437, '6', False),
                        (2462, '11', False), (2480, '39', True)):
        x = max(10, min(W - 12, mhz_x(f, 2402, 2482)))
        text(d, x, cy + ch + P(1), lab, C_BLE if ble else C_CYAN, anchor='ma')
    frame(img, [(None, P(5)), (P(78), P(10) + P(24)), (P(205), P(10) + P(12)), (P(205), P(10) + P(26)), (mhz_x(2458, 2402, 2482), cy + 24), (mhz_x(2437, 2402, 2482), cy + ch - 30), (None, cy + ch + P(6))],
          'page-detector.png')


# --------------------------------------------------------------------------- BLE LISTE
def page_ble():
    img = Image.new('RGB', (W, H), C_BG)
    d = ImageDraw.Draw(img)
    header(d, 'BLE LISTE', '18 aktiv, 31 gesamt', C_GREEN, 'S4/4')
    devs = [(-41, -38, 'Galaxy Watch6 (4F2A)', '0964ebab12cb', 0, 'new'), (-50, -46, '(Microsoft)', '1adf399a6d9e', 0, ''),
            (-52, -47, '?', '32f43d652743', 0, ''), (-58, -54, 'FNB58-052309', '98dab0076b3f*', 0, ''),
            (-61, -57, 'ATC_548972', 'a4c138548972*', 2, ''), (-69, -63, '(Apple)', '6f869654b01d', 0, ''),
            (-74, -70, 'PVVX_DAN', 'a4c138f23c81*', 1, ''), (-86, -79, 'LYWSD03MMC', 'a4c13826d630*', 3, ''),
            (-91, -88, '(Apple)', 'c92745189365', 21, 'stale')]
    mac_w = d.textlength('000000000000*', font=TXT)
    x_mac = W - P(2) - d.textlength('99s', font=TXT) - P(3) - mac_w
    for i, (r, mx, name, mac, age, flag) in enumerate(devs):
        y = P(12) + i * P(10)
        stale = flag == 'stale'
        col = C_DIM if stale else rssi_colour(r)
        text(d, P(20), y, str(r), col, anchor='ra')
        d.rectangle([P(23), y + P(2), P(53) - 1, y + P(7) - 1], fill=C_COL)
        bw = (r + 100) * P(30) // 70
        d.rectangle([P(23), y + P(2), P(23) + bw - 1, y + P(7) - 1], fill=col)
        mxx = min(P(30) - 1, (mx + 100) * P(30) // 70)
        d.rectangle([P(23) + mxx, y + P(1), P(23) + mxx, y + P(8) - 1], fill=C_WHITE)
        nc = C_DIM if stale else C_YELLOW if flag == 'new' else C_TEXT if not name.startswith(('(', '?')) else C_DIM
        text(d, P(56), y, name, nc)
        text(d, x_mac, y, mac, C_DIM if stale else C_CYAN)
        text(d, W - P(2), y, f'{age}s', C_DIM, anchor='ra')
    frame(img, [(None, P(5)), (30, 228), (76, 228), (170, 228), (380, 228), (462, 228)],
          'page-ble-list.png')


# --------------------------------------------------------------------------- KANAELE ZIGBEE
def page_zigbee():
    img = Image.new('RGB', (W, H), C_BG)
    d = ImageDraw.Draw(img)
    header(d, 'KANAELE ZIGBEE', '', C_CYAN, '4/5')
    cw = W // 16
    x0 = (W - cw * 16) // 2
    bar_y, bar_h = P(22), P(88)
    lvl = [6, 9, 22, 8, 31, 5, 7, 18, 12, 6, 4, 9, 7, 5, 26, 3]
    act = [0.02, 0.05, 0.38, 0.06, 0.12, 0.03, 0.04, 0.31, 0.22, 0.04, 0.02, 0.18, 0.05, 0.03, 0.09, 0.01]
    hold = [v + 6 for v in lvl]
    xc = lambda c: x0 + (c - 11) * cw + cw / 2
    for k in range(16):
        c = 11 + k
        x = x0 + k * cw
        gap = c in (15, 20, 25, 26)
        d.rectangle([x + P(1), bar_y, x + cw - P(1) - 1, bar_y + bar_h - 1], fill=C_WIFI if gap else C_COL)
        for db in (10, 20, 30):
            yy = bar_y + bar_h - int(db / 40 * bar_h)
            d.line([x + P(1), yy, x + cw - P(1) - 1, yy], fill=C_GRID)
        lh = int(lvl[k] / 40 * bar_h)
        d.rectangle([x + P(2), bar_y + bar_h - lh, x + cw - P(2) - 1, bar_y + bar_h - 1], fill=air_colour(act[k]))
        yy = bar_y + bar_h - int(hold[k] / 40 * bar_h)
        d.rectangle([x + P(1), yy, x + cw - P(1) - 1, yy + P(2) - 1], fill=C_WHITE)
        text(d, x + cw / 2, bar_y + bar_h + P(3), str(round(act[k] * 100)), air_colour(act[k]), anchor='ma')
        text(d, x + cw / 2, bar_y + bar_h + P(13), str(c), C_CYAN if gap else C_DIM, anchor='ma')
    frame(img, [(xc(15), bar_y + 20), (xc(13), bar_y + bar_h - 30), (xc(19), bar_y + bar_h - int(24 / 40 * bar_h) - 14),
                (None, bar_y + bar_h + P(8)), (None, bar_y + bar_h + P(18))], 'page-zigbee.png')


# --------------------------------------------------------------------------- PEGEL WIFI SINGLE
def page_coverage():
    img = Image.new('RGB', (W, H), C_BG)
    d = ImageDraw.Draw(img)
    header(d, 'PEGEL WIFI SINGLE', '7 Netze', C_CYAN, '5/5')
    text(d, P(4), P(13), 'FRITZ!Box 7590 XY', C_TEXT)
    text(d, P(4), P(24), 'Kanal 6  3c3786a1b2c4', C_DIM)
    r = -58
    text(d, P(8), P(34), str(r), rssi_colour(r), BIG)
    text(d, P(12) + d.textlength(str(r), font=BIG), P(51), 'dBm', C_DIM)
    text(d, W - P(6), P(38), 'Rahmen/s 23', C_TEXT, anchor='ra')
    text(d, W - P(6), P(50), 'min -71  max -44', C_DIM, anchor='ra')
    bx, by, bw, bh = P(6), P(66), W - P(12), P(10)
    cx = lambda v, x0, w: x0 + max(0, min(w - 1, (v + 100) * w // 70))
    d.rectangle([bx, by, bx + bw - 1, by + bh - 1], fill=C_COL)
    d.rectangle([bx, by, cx(r, 0, bw) + bx, by + bh - 1], fill=rssi_colour(r))
    d.rectangle([cx(-71, bx, bw), by - P(2), cx(-71, bx, bw) + P(1) - 1, by + bh + P(2)], fill=C_DIM)
    d.rectangle([cx(-44, bx, bw), by - P(2), cx(-44, bx, bw) + P(2) - 1, by + bh + P(2)], fill=C_WHITE)
    hy = P(82)
    hh = H - hy - P(12)
    for db in range(-90, -30, 10):
        yy = hy + hh - (db + 100) * hh // 70
        d.line([0, yy, W, yy], fill=C_GRID)
    n = 128
    for i in range(n):
        # Walking away from the access point and back: -45 .. -72 .. -55 dBm.
        t = i / (n - 1)
        v = -45 - 27 * math.sin(math.pi * min(t * 1.4, 1.0)) + 8 * t + random.uniform(-2, 2)
        if 0.52 < t < 0.56:
            continue                      # no frame heard for a moment
        bh2 = max(1, int((v + 100) * hh / 70))
        x, x2 = i * W // n, (i + 1) * W // n
        d.rectangle([x, hy + hh - bh2, max(x, x2 - 2), hy + hh - 1], fill=rssi_colour(int(v)))
    text(d, P(2), H - P(11), '32 s', C_DIM)
    text(d, W - P(2), H - P(11), 'lang: Netz', C_DIM, anchor='ra')
    frame(img, [(P(150), P(18)), (P(70), P(43)), (W - P(70), P(44)), (cx(-44, bx, bw), by + bh + 10),
                (W // 2, hy + 20), (W - P(48), H - P(6))], 'page-coverage.png')


if __name__ == '__main__':
    os.makedirs(OUT, exist_ok=True)
    page_waterfall()
    page_networks()
    page_channels()
    page_zigbee()
    page_coverage()
    page_detector()
    page_ble()
