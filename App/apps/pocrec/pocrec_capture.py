#!/usr/bin/env python3
"""Capture POCSAG Rec recordings from the radio's programming cable and analyse them.

38400 8N1 (termios, as rec406_capture.py). Every recording is saved as
  <out>/pocrec_<timestamp>.txt   raw text as sent by the app
  <out>/pocrec_<timestamp>.u16   9.6 kHz uint16 samples rebuilt from the 4-bit
                                 codes (code "lin48": x = c + (n - 8) * 48 + 24)
  <out>/pocrec_<timestamp>.png   waveform plot (needs matplotlib)
then decoded with the POCSAG host decoder at every AC-coupling corner and, if
installed, with multimon-ng (reference decoder, audio resampled to 22050 Hz).

  pocrec_capture.py --port /dev/cu.usbserial-XXXX [--out DIR] [--rate 1200]
  pocrec_capture.py --analyse FILE.txt [--rate 1200]   (redo the analysis)

Close any other program using the port (the app uploader) first.
"""
import argparse
import glob
import os
import select
import shutil
import subprocess
import sys
import tempfile
import termios
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
DEC = os.path.join(HERE, "..", "pocsag")
CORNERS = (0, 60, 250, 1000, 1200, 1500)


def open_port(path):
    fd = os.open(path, os.O_RDONLY | os.O_NOCTTY | os.O_NONBLOCK)
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0
    attrs[1] = 0
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    attrs[3] = 0
    attrs[4] = attrs[5] = termios.B38400
    attrs[6][termios.VMIN] = 1
    attrs[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    termios.tcflush(fd, termios.TCIFLUSH)
    return fd


def lines(fd):
    buf = b""
    while True:
        r, _, _ = select.select([fd], [], [], 1.0)
        if not r:
            continue
        buf += os.read(fd, 4096)
        while b"\n" in buf:
            ln, buf = buf.split(b"\n", 1)
            yield ln.decode("ascii", "replace").strip()


def parse_header(h):
    t = h.split()
    return {"ver": t[1], **{t[i]: t[i + 1] for i in range(2, len(t) - 1, 2)}}


def samples(header, digits):
    if header.get("code") != "lin48":
        sys.exit("unknown sample code in header: " + str(header.get("code")))
    c = int(header["c"])
    n = np.array([int(d, 16) for d in digits])
    return np.clip(c + (n - 8) * 48 + 24, 0, 4095).astype("<u2")


def decoder():
    exe = os.path.join(tempfile.gettempdir(), "host_pocsag_pocrec")
    srcs = [os.path.join(DEC, "test", "host_pocsag.c"), os.path.join(DEC, "pocsag.c")]
    if not os.path.exists(exe) or any(os.path.getmtime(s) > os.path.getmtime(exe) for s in srcs):
        if subprocess.call(["clang", "-O2", "-DPOC_STATS", "-o", exe] + srcs) != 0:
            return None
    return exe


def levels(x, c):
    """Rough picture of the FSK: the two levels, their spread, the clip count."""
    v = x.astype(float) - c
    hi, lo = v[v > 0], v[v < 0]
    return (f"above c: n {len(hi)} median {np.median(hi) if len(hi) else 0:+.0f}  "
            f"below c: n {len(lo)} median {np.median(lo) if len(lo) else 0:+.0f}  "
            f"std {v.std():.0f}")


def plot(x, c, header, path):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        return
    t = np.arange(len(x)) / 9.6 + int(header["t0"])          # ms from the trigger
    fig, ax = plt.subplots(3, 1, figsize=(14, 9))
    ax[0].plot(t, x, lw=0.5)
    ax[0].axhline(c, color="r", lw=0.5)
    ax[0].set_title(f"POCSAG Rec: whole window (c {c}, raw {header['min']}-{header['max']}, rssi {header['rssi']})")
    for a, (s, e) in zip(ax[1:], ((0, 400), (len(x) // 2, len(x) // 2 + 400))):
        a.plot(t[s:e], x[s:e], ".-", lw=0.5, ms=2)
        a.axhline(c, color="r", lw=0.5)
        a.set_xlabel("ms after the trigger")
    fig.tight_layout()
    fig.savefig(path, dpi=90)
    plt.close(fig)


def analyse(base, header, x, rate):
    c = int(header["c"])
    print(f"  levels: {levels(x, c)}")
    plot(x, c, header, base + ".png")
    exe = decoder()
    if exe:
        for hz in CORNERS:
            out = subprocess.run([exe, str(rate), base + ".u16", str(hz)],
                                 capture_output=True, text=True).stdout.strip().splitlines()
            stats = out[-1] if out else ""
            msgs = [ln.strip() for ln in out if ln.startswith("RIC") or "alpha:" in ln]
            print(f"  corner {hz:3d} Hz: {stats}")
            for m in msgs:
                print(f"      {m}")
    if shutil.which("multimon-ng"):
        v = x.astype(float) - c
        t = np.arange(0, len(v) - 1, 9600 / 22050)
        w = np.interp(t, np.arange(len(v)), v)
        w = w / (np.abs(w).max() or 1) * 16000
        raw = base + "_22k.raw"
        np.round(w).astype("<i2").tofile(raw)
        for inv in ("", "-i"):
            args = ["multimon-ng", "-q", "-t", "raw", "-a", f"POCSAG{rate}"] + ([inv] if inv else []) + [raw]
            r = subprocess.run(args, capture_output=True, text=True)
            print(f"  multimon-ng{(' ' + inv) if inv else ''}: " + (r.stdout.strip() or "(nothing)"))


def save(out, header_line, digits, rate):
    base = os.path.join(out, "pocrec_" + time.strftime("%Y%m%d_%H%M%S"))
    with open(base + ".txt", "w") as f:
        f.write(header_line + "\n")
        for i in range(0, len(digits), 100):
            f.write(digits[i:i + 100] + "\n")
        f.write("END\n")
    h = parse_header(header_line)
    x = samples(h, digits)
    x.tofile(base + ".u16")
    print(f"saved {base}.txt / .u16 / .png  ({len(digits)} samples, t0 {h['t0']} ms, "
          f"c {h['c']}, raw {h['min']}-{h['max']}, rssi {h['rssi']}, clip {h['clip']})")
    analyse(base, h, x, rate)


def load(txt):
    with open(txt) as f:
        rows = [r.strip() for r in f if r.strip()]
    h = parse_header(rows[0])
    digits = "".join(r[:-3] if r.endswith("END") else r for r in rows[1:] if r != "END")
    return h, digits


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--out", default=".")
    ap.add_argument("--rate", type=int, default=1200, choices=(512, 1200, 2400))
    ap.add_argument("--count", type=int, default=0, help="stop after N recordings (0 = until Ctrl-C)")
    ap.add_argument("--analyse")
    ap.add_argument("--debug", action="store_true", help="print every received line (first 60 chars)")
    a = ap.parse_args()
    if a.analyse:
        h, digits = load(a.analyse)
        base = os.path.splitext(a.analyse)[0]
        x = samples(h, digits)
        x.tofile(base + ".u16")
        analyse(base, h, x, a.rate)
        return
    if not a.port:
        ports = glob.glob("/dev/cu.usbserial-*")
        if len(ports) != 1:
            sys.exit("choose the programming cable with --port, one of: " + ", ".join(ports or ["(none found)"]))
        a.port = ports[0]
    os.makedirs(a.out, exist_ok=True)
    fd = open_port(a.port)
    print(f"listening on {a.port} (38400 8N1), Ctrl-C to stop")
    n, header, digits = 0, None, ""
    try:
        for ln in lines(fd):
            if a.debug:
                print(f"[rx] {ln[:60]!r}", flush=True)
            if ln.startswith("POCREC"):
                header, digits = ln, ""
            elif header and ln.endswith("END"):
                digits += ln[:-3]
                save(a.out, header, digits, a.rate)
                header, n = None, n + 1
                if a.count and n >= a.count:
                    break
            elif header:
                digits += ln
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
