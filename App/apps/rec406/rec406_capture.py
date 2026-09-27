#!/usr/bin/env python3
"""Capture 406 Rec recordings from the radio's programming cable.

Standard library only (termios), 38400 8N1. Every recording is saved as
  <out>/rec406_<timestamp>.txt   raw text as sent by the app
  <out>/rec406_<timestamp>.u16   9.6 kHz uint16 samples rebuilt from the 4-bit
                                 codes (code "comp1": x = c + LEVELS[digit])
and, if the host decoder is found, decoded straight away.

  rec406_capture.py --port /dev/cu.usbserial-XXXX [--out DIR] [--count N]
  rec406_capture.py --convert FILE.txt        (rebuild the .u16 from a saved capture)

Close any other program using the port (the app uploader) first.
"""
import argparse
import glob
import os
import select
import subprocess
import sys
import tempfile
import termios
import time

HERE = os.path.dirname(os.path.abspath(__file__))
DECODER_SRC = os.path.join(HERE, "..", "epirb406")


def open_port(path):
    # O_NONBLOCK: on macOS a blocking open/read of a tty can wait on the modem
    # lines; the first version of this script received nothing that way
    fd = os.open(path, os.O_RDONLY | os.O_NOCTTY | os.O_NONBLOCK)
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0                                        # iflag: raw
    attrs[1] = 0                                        # oflag
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    attrs[3] = 0                                        # lflag: no echo, no canonical
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
    kv = {t[i]: t[i + 1] for i in range(2, len(t) - 1, 2)}
    return {"ver": t[1], **kv}


# Reconstruction levels of the app's non-linear 4-bit code "comp1" (ADC LSB from c)
LEVELS = [-300, -200, -130, -80, -45, -22, -8, 0, 8, 22, 45, 80, 130, 200, 300, 420]


def to_u16(header, digits, path):
    import array
    if header.get("code") != "comp1":
        sys.exit("unknown sample code in header: " + str(header.get("code")))
    c = int(header["c"])
    a = array.array("H", (max(0, min(4095, c + LEVELS[int(d, 16)])) for d in digits))
    if sys.byteorder != "little":
        a.byteswap()
    with open(path, "wb") as f:
        a.tofile(f)


def decoder():
    exe = os.path.join(tempfile.gettempdir(), "host_dec406_rec406")   # built outside the repo
    srcs = [os.path.join(DECODER_SRC, "test", "host_dec406.c"), os.path.join(DECODER_SRC, "dec406.c")]
    if not os.path.exists(exe) or any(os.path.getmtime(s) > os.path.getmtime(exe) for s in srcs):
        if subprocess.call(["clang", "-O2", "-o", exe] + srcs) != 0:
            return None
    return exe


def save(out, header_line, digits, rssi_line=None):
    stamp = time.strftime("%Y%m%d_%H%M%S")
    base = os.path.join(out, "rec406_" + stamp)
    with open(base + ".txt", "w") as f:
        f.write(header_line + "\n")
        if rssi_line:
            f.write(rssi_line + "\n")
        for i in range(0, len(digits), 100):
            f.write(digits[i:i + 100] + "\n")
        f.write("END\n")
    h = parse_header(header_line)
    to_u16(h, digits, base + ".u16")
    print(f"saved {base}.txt / .u16  ({len(digits)} samples, c {h['c']}, rssi {h['rssi']}, clip {h['clip']})")
    if rssi_line:
        v = rssi_line.split()[1:]
        print("RSSI every 10 ms from 80 ms after the trigger:")
        for i in range(0, len(v), 10):
            print(f"  {80 + 10 * i:3d} ms: " + " ".join(f"{int(x):4d}" for x in v[i:i + 10]))
    exe = decoder()
    if exe:
        subprocess.call([exe, base + ".u16"])


def convert(txt):
    with open(txt) as f:
        rows = [r.strip() for r in f if r.strip()]
    h = parse_header(rows[0])
    digits = "".join(r[:-3] if r.endswith("END") else r for r in rows[1:] if not r.startswith("RSSI"))
    out = os.path.splitext(txt)[0] + ".u16"
    to_u16(h, digits, out)
    print(f"{out}: {len(digits)} samples")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--out", default=".")
    ap.add_argument("--count", type=int, default=0, help="stop after N recordings (0 = run until Ctrl-C)")
    ap.add_argument("--convert")
    ap.add_argument("--debug", action="store_true", help="print every received line (first 60 chars)")
    a = ap.parse_args()
    if a.convert:
        convert(a.convert)
        return
    if not a.port:
        ports = glob.glob("/dev/cu.usbserial-*")
        if len(ports) != 1:
            sys.exit("choose the programming cable with --port, one of: " + ", ".join(ports or ["(none found)"]))
        a.port = ports[0]
    os.makedirs(a.out, exist_ok=True)
    fd = open_port(a.port)
    print(f"listening on {a.port} (38400 8N1), Ctrl-C to stop")
    n, header, digits, rssi_line = 0, None, "", None
    try:
        nb = 0
        for ln in lines(fd):
            nb += len(ln) + 1
            if a.debug:
                print(f"[{nb:6d}] {ln[:60]!r}", flush=True)
            if ln.startswith("REC406"):
                header, digits, rssi_line = ln, "", None
            elif header and ln.startswith("RSSI"):          # 406 Rec v1.1+
                rssi_line = ln
            elif header and ln.endswith("END"):
                # 406 Rec v1.0 sends no line break after the last, partial line
                # (4260 = 42 x 100 + 60), so END arrives glued to it
                digits += ln[:-3]
                save(a.out, header, digits, rssi_line)
                header, n = None, n + 1
                if a.count and n >= a.count:
                    break
            elif header:
                digits += ln
    except KeyboardInterrupt:
        pass
    finally:
        os.close(fd)


if __name__ == "__main__":
    main()
