#!/usr/bin/env python3
# Stand-in for APRSdroid on a computer: opens the radio's USB serial port,
# sends KISS data frames as APRSdroid does, and prints every frame the radio
# sends back, decoded. No dependency (termios), macOS and Linux.
#
#   python3 kiss_host.py                     # first /dev/cu.usbmodem* or /dev/ttyACM*
#   python3 kiss_host.py /dev/cu.usbmodemXXXX --call F4WAT-7 --count 3
#
# With the KISS TNC app (step 0) running and echo on, each frame sent comes
# back unchanged; key 2 on the radio sends its test frame, printed here too.
import argparse, glob, os, select, sys, termios, time

FEND, FESC, TFEND, TFESC = 0xC0, 0xDB, 0xDC, 0xDD

def addr(call, last):
    name, _, ssid = call.partition('-')
    c = name.upper().ljust(6)[:6].encode()
    return bytes(x << 1 for x in c) + bytes([0x60 | (int(ssid or 0) << 1) | last])

def frame(src, info):
    """AX.25 UI frame as APRSdroid builds it (javAPRSlib toAX25Frame), no FCS."""
    return addr('APDR16', 0) + addr(src, 0) + addr('WIDE1-1', 1) + b'\x03\xf0' + info

def kiss(f):
    e = f.replace(b'\xdb', b'\xdb\xdd').replace(b'\xc0', b'\xdb\xdc')
    return bytes([FEND, 0]) + e + bytes([FEND])

def decode(f):
    calls, i = [], 0
    while i + 7 <= len(f):
        c = bytes(x >> 1 for x in f[i:i + 6]).decode('ascii', 'replace').strip()
        ssid = (f[i + 6] >> 1) & 15
        calls.append(c + (f'-{ssid}' if ssid else ''))
        i += 7
        if f[i - 1] & 1:
            break
    if len(calls) < 2:
        return f'?? {f.hex()}'
    info = f[i + 2:].decode('ascii', 'replace')
    path = ',' + ','.join(calls[2:]) if calls[2:] else ''
    return f'{calls[1]}>{calls[0]}{path}:{info}'

def open_port(path):
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)   # opening raises DTR
    a = termios.tcgetattr(fd)
    a[0] = a[1] = a[3] = 0                                        # raw: no iflag/oflag/lflag
    a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    termios.tcsetattr(fd, termios.TCSANOW, a)
    return fd

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('port', nargs='?')
    ap.add_argument('--call', default='N0CALL')
    ap.add_argument('--count', type=int, default=1, help='frames to send, 1 s apart')
    ap.add_argument('--listen', type=float, default=10, help='seconds to listen after the last frame')
    a = ap.parse_args()
    port = a.port or (sorted(glob.glob('/dev/cu.usbmodem*') + glob.glob('/dev/ttyACM*')) or [None])[0]
    if not port:
        sys.exit('no USB serial port found: plug the radio, then pass the port')
    fd = open_port(port)
    print(f'opened {port}')
    buf, inframe, esc = bytearray(), False, False
    sent, deadline, next_tx = 0, None, time.time() + 0.5
    while deadline is None or time.time() < deadline:
        if sent < a.count and time.time() >= next_tx:
            sent += 1
            f = frame(a.call, f'>kiss_host.py test {sent}/{a.count}'.encode())
            os.write(fd, kiss(f))
            print(f'TX  {decode(f)}')
            next_tx = time.time() + 1
            if sent == a.count:
                deadline = time.time() + a.listen
        if not select.select([fd], [], [], 0.05)[0]:
            continue
        for b in os.read(fd, 512):
            if b == FEND:
                if inframe and len(buf) > 1 and buf[0] & 0x0F == 0:
                    print(f'RX  {decode(bytes(buf[1:]))}')
                buf.clear(); inframe, esc = True, False
            elif not inframe:
                continue
            elif esc:
                buf.append(FEND if b == TFEND else FESC if b == TFESC else b); esc = False
            elif b == FESC:
                esc = True
            else:
                buf.append(b)
    os.close(fd)

if __name__ == '__main__':
    main()
