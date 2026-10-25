#!/usr/bin/env python3
"""Measurement routines over the serial diagnostics (requires pyserial).

Talks to a running board through the commands in the README's "Serial
diagnostics" table; does not flash anything. On macOS (and sometimes
elsewhere) opening the port resets the board even with DTR/RTS off, so the
script waits --boot seconds and drops the boot log before the first command.

    python tools/serial_tests.py PORT send 'TDSTAT?' 'TDFOCUS?@3'
    python tools/serial_tests.py PORT rates          # captures/s, listening per page
    python tools/serial_tests.py PORT rate           # TDRATE 80/40/16 MS/s at several LOs
    python tools/serial_tests.py PORT focus          # TDFOCUS? on every FOCUS page
    python tools/serial_tests.py PORT spinoise 100   # TDSPINOISE, survey mode
    python tools/serial_tests.py PORT span 1 2437 38 20
    python tools/serial_tests.py PORT probe 3300 3900 20
    python tools/serial_tests.py PORT screen shot.png  # screenshot (needs Pillow)
    python tools/serial_tests.py PORT screen focus.png --cmd 'TDSNPAGE 4' --wait 10
    python tools/serial_tests.py PORT gif waterfall.gif --secs 20 --wait 10
    python tools/serial_tests.py PORT --baud 1000000 gif ble.gif --secs 60 --fps 10 \
        --colors 32 --cmd 'TDSNPAGE 4' --cmd 'TDFOCUS 14' --wait 10   # time-lapse

PORT: JC3248 /dev/cu.usbmodem* or /dev/ttyACM* (USB serial/JTAG), T-Display
/dev/cu.usbserial-* or /dev/ttyUSB* (CP2104, 2 Mbaud).
"""
import argparse
import os
import re
import sys
import time

import serial

SN_PAGES = ['FINDER', 'DETECTOR', 'BEARING', 'FOCUS WIFI', 'FOCUS BLE', 'FOCUS ZIGBEE', 'BLE LIST SDR', 'BLE LIST']
FOCUS_PAGES = {3: 'WIFI', 4: 'BLE', 5: 'ZIGBEE'}


class Board:
    def __init__(self, port, boot, baud=2000000):
        s = serial.Serial()
        s.port, s.baudrate, s.timeout = port, 2000000, 0.1
        s.dtr = s.rts = False
        s.open()
        self.s = s
        time.sleep(boot)
        # The JC3248's USB serial hands out the held-back boot log only after
        # the host has written something: one empty line, then drop it all.
        s.write(b'\n')
        self.read(1.0)
        s.reset_input_buffer()
        self.dropped = 0
        if baud != s.baudrate:  # UART only (T-Display); the board forgets it on reset
            if not self.line(f'BAUD {baud}', 'OK BAUD'):
                sys.exit(f'board did not switch to {baud} baud')
            s.baudrate = baud
            time.sleep(0.1)
            s.reset_input_buffer()

    def read(self, secs, until=None):
        """Lines arriving within secs (sooner once a line contains until).
        The T-Display's 115200 boot log leaves zero bytes at 2 Mbaud."""
        buf, end = b'', time.time() + secs
        while time.time() < end:
            buf += self.s.read(4096)
            if until and until.encode() in buf:
                time.sleep(0.1)
                buf += self.s.read(4096)
                break
        return buf.replace(b'\0', b'').decode(errors='replace').splitlines()

    def cmd(self, line, secs=1.0, until=None):
        self.s.reset_input_buffer()
        self.s.write((line + '\n').encode())
        return self.read(secs, until)

    def line(self, line, prefix, secs=2.0):
        """The first reply line starting with prefix, or ''."""
        for ln in self.cmd(line, secs, until=prefix):
            if ln.startswith(prefix):
                return ln
        return ''


def field(ln, name, cast=float):
    """Value after 'name ' in a reply line, or None."""
    m = re.search(r'\b' + re.escape(name) + r' (-?[\d.]+)', ln)
    return cast(m.group(1)) if m else None


def cap_n(board, port):
    if board != 'auto':
        return 16128 if board == 'jc3248' else 8192
    return 16128 if 'usbmodem' in port or 'ACM' in port else 8192


def caps_per_s(b, secs):
    """Captures per second from the global capture counter (TDSTAT?)."""
    t0, c0 = time.time(), field(b.line('TDSTAT?', 'TDSTAT'), 'caps', int)
    time.sleep(secs)
    t1, c1 = time.time(), field(b.line('TDSTAT?', 'TDSTAT'), 'caps', int)
    if c0 is None or c1 is None:
        return None
    return (c1 - c0) / (t1 - t0)


def do_send(b, a):
    for arg in a.cmds:
        cmd, _, secs = arg.partition('@')
        t = float(secs) if secs else a.wait
        if cmd:
            print(f'>>> {cmd}', flush=True)
            b.s.reset_input_buffer()
            b.s.write((cmd + '\n').encode())
        for ln in b.read(t):
            print(ln, flush=True)


def do_rates(b, a):
    n = cap_n(a.board, a.port)
    print(f'capture {n} samples; {a.secs:.0f} s per page after {a.settle:.0f} s settling')
    b.cmd('TDSNIFF 0')
    time.sleep(a.settle)
    r = caps_per_s(b, a.secs)
    print(f'survey        {r:5.1f} caps/s  listen {r * n / 80e3:5.1f} ms/s' if r else 'survey        no reply')
    for p, name in enumerate(SN_PAGES):
        b.cmd(f'TDSNPAGE {p}')
        time.sleep(a.settle)
        if p in FOCUS_PAGES:
            ln = b.line('TDFOCUS?', 'TDFOCUS')
            time.sleep(a.secs)
            ln = b.line('TDFOCUS?', 'TDFOCUS') or ln
            print(f'{name:13s} {field(ln, "caps/s") or 0:5.0f} caps/s  listen {field(ln, "listen") or 0:5.1f} ms/s'
                  f'  ({field(ln, "msps", int)} MS/s)')
        else:
            r = caps_per_s(b, a.secs)
            print(f'{name:13s} {r:5.1f} caps/s' if r is not None else f'{name:13s} no reply')
    b.cmd('TDSNIFF 0')


def do_rate(b, a):
    """Each LO a.n times: one capture can catch live traffic instead of the
    harmonic; ok k/n counts the captures that found it within one bin."""
    print('TDRATE: strongest line vs the 40 MHz harmonic expected (JC3248: a 2448 MHz carrier may win)')
    for d, msps in ((0, 80), (1, 40), (6, 16)):
        for lo in a.lo:
            hits, other, exp = 0, [], None
            for _ in range(a.n):
                ln = b.line(f'TDRATE {d} {lo}', 'TDRATE', 3)
                m = re.search(r'strongest ([+-][\d.]+) MHz \(\+([\d.]+) dB.*expected ([+-][\d.]+)', ln)
                if not m:
                    continue
                got, exp = float(m.group(1)), float(m.group(3))
                if abs(got - exp) <= msps / 256:
                    hits += 1
                else:
                    other.append(f'{lo + got:.1f}')
            if exp is None:
                print(f'{msps:2d} MS/s LO {lo}: no reply')
                continue
            print(f'{msps:2d} MS/s LO {lo}: expected {exp:+.3f} MHz  ok {hits}/{a.n}'
                  + (f'  other lines at {" ".join(other)} MHz' if other else ''))


def do_focus(b, a):
    for p, name in FOCUS_PAGES.items():
        b.cmd(f'TDSNPAGE {p}')
        time.sleep(a.settle)
        for _ in range(a.n):
            print(b.line('TDFOCUS?', 'TDFOCUS'))
            time.sleep(1)
    b.cmd('TDSNIFF 0')


def do_spinoise(b, a):
    b.cmd('TDSNIFF 0')
    time.sleep(2)
    for ln in b.cmd(f'TDSPINOISE {a.n}', secs=10 + a.n * 0.5):
        print(ln)


def do_span(b, a):
    print(b.line(f'TDSPAN {a.d} {a.lo} {a.bw} {a.n}', 'TDSPAN', 10 + a.n))


def do_probe(b, a):
    steps = (a.f1 - a.f0) // a.step + 1
    for ln in b.cmd(f'TDPROBE {a.f0} {a.f1} {a.step} {a.n}', secs=10 + steps * a.n * 0.1, until='TDPROBE END'):
        if ln.startswith('TDPROBE'):
            print(ln)


def grab(b, half=False):
    """One TDSCREEN frame as a PIL image, or None (half: 2x2 averaged).
    A frame that comes up short or fails its CRC is dropped (b.dropped):
    the T-Display's CP2104 now and then loses a few bytes at 2 Mbaud."""
    import zlib
    import numpy as np
    from PIL import Image
    s = b.s
    s.reset_input_buffer()
    s.write(b'TDSCREEN 2\n' if half else b'TDSCREEN\n')
    end, line = time.time() + 5, b''
    while time.time() < end and not line.startswith(b'TDSCREEN '):
        line = s.readline().replace(b'\0', b'')
    m = re.match(rb'TDSCREEN (\d+) (\d+)', line)
    if not m:
        return None
    w, h = int(m.group(1)), int(m.group(2))
    # Lost bytes would leave the read waiting for the rest: give up after
    # 0.3 s without data. The END line then lands in data, so the CRC fails.
    data, quiet = bytearray(), time.time() + 1
    while len(data) < w * h * 2 and time.time() < quiet:
        got = s.read(w * h * 2 - len(data))
        if got:
            data += got
            quiet = time.time() + 0.3
    tail = s.readline().decode(errors='replace').strip()
    if len(data) < w * h * 2 or not tail.endswith(f'{zlib.crc32(bytes(data)) & 0xffffffff:08x}'):
        b.dropped += 1
        return None
    v = np.frombuffer(bytes(data), dtype='>u2').reshape(h, w).astype(np.uint32)
    r, g, bl = v >> 11, (v >> 5) & 63, v & 31
    rgb = np.dstack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (bl << 3) | (bl >> 2)]).astype(np.uint8)
    return Image.fromarray(rgb, 'RGB')


def grab_one(b, tries=5):
    """A screenshot, retried when a frame is dropped."""
    for _ in range(tries):
        img = grab(b)
        if img is not None:
            return img
    return None


def setup(b, a):
    for c in a.cmd:  # opening the port may have reset the board: set it up here
        b.cmd(c, 1)
    # Keep reading while waiting: a JC3248 restarted (BLE LIST in/out) while
    # the host does not read leaves its USB port silent.
    b.read(a.wait)


def do_screen(b, a):
    """TDSCREEN: header line, W*H RGB565 pixels high byte first, END line
    with the CRC-32 of the pixel bytes."""
    setup(b, a)
    img = grab_one(b)
    if img is None:
        sys.exit('no screenshot')
    img.save(a.out)
    print(f'{a.out}: {img.width}x{img.height}')


def save_gif(frames, durations, out, colors=256, dither=False):
    """One palette for all frames (taken from a few of them), so the GIF
    has no per-frame palettes and changes between frames stay small."""
    from PIL import Image
    pick = frames[::max(1, len(frames) // 6)]
    sheet = Image.new('RGB', (pick[0].width, pick[0].height * len(pick)))
    for i, f in enumerate(pick):
        sheet.paste(f, (0, i * f.height))
    # The UI's colours (wifi_ui.c C_*, as RGB565 shows them) stay exact where
    # they appear, text colours with a half tone for their smoothed edges;
    # the rest of the palette goes to the spectrum and waterfall shades.
    # Otherwise grey text turns brown.
    import numpy as np
    q = lambda c: tuple((v >> 3 << 3 | v >> 5) if i != 1 else (v >> 2 << 2 | v >> 6) for i, v in enumerate(c))
    ui = [(0, 0, 0), (255, 255, 255), (220, 220, 220), (110, 110, 110), (24, 24, 48), (255, 220, 0),
          (0, 200, 230), (40, 220, 40), (230, 60, 230), (255, 140, 0), (255, 40, 40)]
    text = {(220, 220, 220), (110, 110, 110), (255, 220, 0), (0, 200, 230)}
    px = np.asarray(sheet).reshape(-1, 3).astype(int)
    fixed = []
    for c in ui:
        n = int((np.abs(px - q(c)).max(axis=1) <= 4).sum())
        if n > px.shape[0] // 2000 or c == (0, 0, 0):
            fixed.append(q(c))
            if c in text:
                fixed.append(tuple(v // 2 for v in q(c)))
    fixed = fixed[:colors // 2]
    rest = sheet.quantize(colors=colors - len(fixed), method=Image.Quantize.MEDIANCUT).getpalette()
    rest = [tuple(rest[3 * i:3 * i + 3]) for i in range(colors - len(fixed))]
    flat = [v for c in fixed + rest for v in c]
    pal = Image.new('P', (1, 1))
    pal.putpalette(flat + [0] * (768 - len(flat)))
    d = Image.Dither.FLOYDSTEINBERG if dither else Image.Dither.NONE
    q = [f.quantize(palette=pal, dither=d) for f in frames]
    q[0].save(out, save_all=True, append_images=q[1:], duration=durations, loop=0, optimize=True)


def do_gif(b, a):
    """Screenshots back to back as an animated GIF, each frame shown for the
    time it really took (so it plays at the board's speed), or with --fps at
    a fixed rate: a time-lapse when the board sends fewer frames per second
    (a full JC3248 frame takes about 0.4 s over USB)."""
    setup(b, a)
    frames, times = [], []
    end = time.time() + a.secs
    while time.time() < end:
        img = grab(b, a.half)
        if img is not None:
            frames.append(img)
            times.append(time.time())
    if len(frames) < 2:
        sys.exit('too few frames')
    durations = [int((t1 - t0) * 1000) for t0, t1 in zip(times, times[1:])]
    durations.append(durations[-1])
    if a.fps:
        durations = [int(round(1000 / a.fps))] * len(frames)
    if a.scale != 1:
        frames = [f.resize((f.width * a.scale, f.height * a.scale)) for f in frames]
    if a.raw:  # the frames as PNGs plus their durations, to try palettes later
        os.makedirs(a.raw, exist_ok=True)
        for i, f in enumerate(frames):
            f.save(os.path.join(a.raw, f'{i:04d}.png'))
        with open(os.path.join(a.raw, 'durations.txt'), 'w') as fh:
            fh.write(' '.join(map(str, durations)))
    save_gif(frames, durations, a.out, a.colors, a.dither)
    print(f'{a.out}: {len(frames)} frames, {len(frames) / (times[-1] - times[0]):.1f} frames/s, '
          f'{b.dropped} dropped')


# README pages: file, commands, seconds to settle, boxes to black out
# (x0, y0, x1, y1, in JC3248 pixels, 480x320; scaled to the T-Display's
# 240x135, whose layout is the same at half size) where network names and
# addresses are shown.
README_PAGES = [
    ('page-waterfall.png', ['TDSNIFF 0', 'TDPAGE 0'], 15, []),
    ('page-networks.png', ['TDSNIFF 0', 'TDPAGE 1'], 15, [(28, 20, 284, 320)]),
    ('page-channels.png', ['TDSNIFF 0', 'TDPAGE 2'], 15, []),
    ('page-zigbee.png', ['TDSNIFF 0', 'TDPAGE 3'], 15, []),
    ('page-coverage.png', ['TDSNIFF 0', 'TDPAGE 4'], 25, [(0, 22, 480, 66)]),
    ('page-detector.png', ['TDSNPAGE 1'], 25, []),
    ('page-focus.png', ['TDSNPAGE 3', 'TDFOCUS 5'], 15, []),
    ('page-ble-list-sdr.png', ['TDSNPAGE 6'], 40, [(108, 20, 446, 320)]),
    ('page-ble-list.png', ['TDSNPAGE 7'], 40, [(108, 20, 446, 320)]),
]


def redact(img, box, line):
    """Each text line inside box becomes the same flat grey bar over the full
    width: nothing of the names or addresses is left, not even their
    length."""
    import numpy as np
    from PIL import Image
    a = np.asarray(img).copy()
    x0, y0, x1, y1 = box
    a[y0:y1, x0:x1] = 0
    for y in range(y0, y1, line):  # one bar per line, all the same colour
        a[y + line // 5:min(y + line - line // 5, y1), x0:x1] = (48, 48, 56)
    img.paste(Image.fromarray(a))


def do_readme(b, a):
    """Screenshots of the README pages with names and addresses blacked out,
    for `tools/readme_images.py --shots DIR`. Check them before committing."""
    os.makedirs(a.dir, exist_ok=True)
    for name, cmds, wait, boxes in README_PAGES:
        if a.only and name not in a.only:
            continue
        for c in cmds:
            b.cmd(c, 1)
        b.read(wait)
        if name == 'page-ble-list.png' and cap_n(a.board, a.port) == 8192:
            continue  # no Bluetooth stack on the T-Display
        img = grab_one(b)
        if img is None:
            print(f'{name}: no screenshot')
            continue
        k = img.width / 480
        for box in boxes:
            x0, y0, x1, y1 = (int(round(v * k)) for v in box)
            redact(img, (x0, y0, x1, min(y1, img.height)), int(round(20 * k)))
        img.save(os.path.join(a.dir, name))
        print(f'{name}: ok')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('port')
    ap.add_argument('--boot', type=float, default=8, help='seconds to wait after opening the port (default 8)')
    ap.add_argument('--board', choices=['auto', 'jc3248', 'tdisplay'], default='auto',
                    help='for the capture length; auto: from the port name')
    ap.add_argument('--baud', type=int, choices=[1000000, 2000000], default=2000000,
                    help='T-Display only: switch the UART after connecting (default 2000000)')
    sub = ap.add_subparsers(dest='what', required=True)
    p = sub.add_parser('send', help='send commands; CMD@SECS waits SECS, a bare @SECS just listens')
    p.add_argument('cmds', nargs='+')
    p.add_argument('--wait', type=float, default=2)
    p = sub.add_parser('rates', help='captures/s and listening time, survey and every sniffer page')
    p.add_argument('--secs', type=float, default=5)
    p.add_argument('--settle', type=float, default=3)
    p = sub.add_parser('rate', help='TDRATE at 80/40/16 MS/s')
    p.add_argument('--lo', type=int, nargs='+', default=[2438, 2442, 2446, 2450])
    p.add_argument('-n', type=int, default=3, help='captures per LO (default 3)')
    p = sub.add_parser('focus', help='TDFOCUS? on each FOCUS page')
    p.add_argument('-n', type=int, default=3)
    p.add_argument('--settle', type=float, default=3)
    p = sub.add_parser('spinoise', help='TDSPINOISE n')
    p.add_argument('n', type=int, nargs='?', default=40)
    p = sub.add_parser('span', help='TDSPAN d lo bw n')
    for k in ('d', 'lo', 'bw', 'n'):
        p.add_argument(k, type=int)
    p = sub.add_parser('probe', help='TDPROBE f0 f1 step [n]')
    for k in ('f0', 'f1', 'step'):
        p.add_argument(k, type=int)
    p.add_argument('n', type=int, nargs='?', default=20)
    p = sub.add_parser('screen', help='TDSCREEN: save a screenshot as PNG')
    p.add_argument('out')
    p.add_argument('--cmd', action='append', default=[], help='command to send first (repeatable)')
    p.add_argument('--wait', type=float, default=0, help='seconds to wait before the screenshot')
    p = sub.add_parser('readme', help='README page screenshots, names and addresses blacked out')
    p.add_argument('dir')
    p.add_argument('--only', nargs='*', help='only these files (page-networks.png ...)')
    p = sub.add_parser('gif', help='screenshots back to back as an animated GIF')
    p.add_argument('out')
    p.add_argument('--secs', type=float, default=15)
    p.add_argument('--scale', type=int, default=1)
    p.add_argument('--fps', type=float, help='play back at this rate instead of the recorded one')
    p.add_argument('--half', action='store_true', help='half resolution from the board (faster)')
    p.add_argument('--colors', type=int, default=256, help='palette size (one palette for all frames)')
    p.add_argument('--dither', action='store_true')
    p.add_argument('--raw', help='also save the frames as PNGs into this folder')
    p.add_argument('--cmd', action='append', default=[], help='command to send first (repeatable)')
    p.add_argument('--wait', type=float, default=0, help='seconds to wait before recording')
    a = ap.parse_args()
    b = Board(a.port, a.boot, a.baud)
    try:
        globals()['do_' + a.what](b, a)
    finally:
        b.s.close()


if __name__ == '__main__':
    sys.exit(main())
