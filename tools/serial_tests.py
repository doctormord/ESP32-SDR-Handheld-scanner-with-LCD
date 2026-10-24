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

PORT: JC3248 /dev/cu.usbmodem* or /dev/ttyACM* (USB serial/JTAG), T-Display
/dev/cu.usbserial-* or /dev/ttyUSB* (CP2104, 2 Mbaud).
"""
import argparse
import re
import sys
import time

import serial

SN_PAGES = ['FINDER', 'DETECTOR', 'BEARING', 'FOCUS WIFI', 'FOCUS BLE', 'FOCUS ZIGBEE', 'BLE LIST']
FOCUS_PAGES = {3: 'WIFI', 4: 'BLE', 5: 'ZIGBEE'}


class Board:
    def __init__(self, port, boot):
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


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('port')
    ap.add_argument('--boot', type=float, default=8, help='seconds to wait after opening the port (default 8)')
    ap.add_argument('--board', choices=['auto', 'jc3248', 'tdisplay'], default='auto',
                    help='for the capture length; auto: from the port name')
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
    a = ap.parse_args()
    b = Board(a.port, a.boot)
    try:
        globals()['do_' + a.what](b, a)
    finally:
        b.s.close()


if __name__ == '__main__':
    sys.exit(main())
