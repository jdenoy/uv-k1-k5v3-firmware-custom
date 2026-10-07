#!/usr/bin/env python3
# Host test of the KISS TNC app (step 0): builds kisstnc_app.c against a mock
# API (host_kiss.c), feeds it KISS traffic shaped as APRSdroid sends it, in odd
# USB packet sizes, and checks the echo, the test frame and the counters.
#
#   python3 test_kiss.py        (needs a host C compiler: cc / clang)
import os, re, shutil, subprocess, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
tmp = tempfile.mkdtemp()
src = open(os.path.join(HERE, '..', 'kisstnc_app.c')).read()
src = src.replace('__attribute__((section(".text.entry"), used))', '')
src = src.replace('#include "../app_api.h"', '#include "%s"' % os.path.join(HERE, '..', '..', 'app_api.h'))
open(os.path.join(tmp, 'app.c'), 'w').write(src)
shutil.copy(os.path.join(HERE, 'host_kiss.c'), tmp)
h = open(os.path.join(tmp, 'host_kiss.c')).read().replace('"../../app_api.h"', '"%s"' % os.path.join(HERE, '..', '..', 'app_api.h'))
open(os.path.join(tmp, 'host_kiss.c'), 'w').write(h)
cc = shutil.which('clang') or shutil.which('gcc')
subprocess.run([cc, '-O1', '-Wall', '-Wno-unused-function', '-o', os.path.join(tmp, 'h'), os.path.join(tmp, 'host_kiss.c')], check=True)
os.chdir(tmp)
FEND, FESC = 0xC0, 0xDB
def esc(b): return b.replace(b'\xdb', b'\xdb\xdd').replace(b'\xc0', b'\xdb\xdc')
def addr(call, ssid, last):
    c = call.ljust(6)[:6].encode(); return bytes(x << 1 for x in c) + bytes([0x60 | (ssid << 1) | last])
def frame(src, ssid, info):  # as APRSdroid / javAPRSlib toAX25Frame: dest, src, path, 03 F0, info
    return addr('APDR16', 0, 0) + addr(src, ssid, 0) + addr('WIDE1', 1, 1) + b'\x03\xf0' + info
f1 = frame('F4WAT', 7, b'=4916.27N/00046.92E>APRSdroid test \xc0\xdb escape')   # bytes to escape
f2 = frame('F4WAT', 0, b'>' + b'x' * 200)                                          # > 64 B: several USB writes
stream = (b'\x00\x11' + bytes([FEND, 0]) + esc(f1) + bytes([FEND])                 # noise before the first FEND
          + bytes([FEND, FEND, 0]) + esc(f2) + bytes([FEND])                       # back-to-back FENDs
          + bytes([FEND, 0x06, 0x01, FEND])                                        # SETHW command: ignored
          + bytes([FEND, 0, 0x41, FESC, 0x41, FEND]))                              # bad escape: one error
open('in.bin', 'wb').write(stream)
out = subprocess.run(['./h', 'in.bin', '.' * 40 + '2' + '.' * 5 + 'x'], capture_output=True, text=True).stdout
writes = [bytes(int(x, 16) for x in l.split()[1:]) for l in out.splitlines() if l.startswith('OUT')]
blob = b''.join(writes)
frames = [x for x in blob.split(b'\xc0') if x]
def unesc(b): return b.replace(b'\xdb\xdc', b'\xc0').replace(b'\xdb\xdd', b'\xdb')
ok = True
def check(name, cond): 
    global ok; ok &= cond; print(('PASS ' if cond else 'FAIL ') + name)
check('two data frames echoed, command and SETHW/bad frames not', len(frames) == 3)
check('echo 1 == frame 1 (escapes round-trip)', frames[0][0] == 0 and unesc(frames[0][1:]) == f1)
check('echo 2 == frame 2 (multi-write)', unesc(frames[1][1:]) == f2 and max(len(w) for w in writes) <= 64)
t = unesc(frames[2][1:])
dst = bytes(x >> 1 for x in t[0:6]).decode(); src = bytes(x >> 1 for x in t[7:13]).decode()
check('test frame: APZK1 <- F4WAT, last-address bit, UI 03 F0', dst.strip() == 'APZK1' and src.strip() == 'F4WAT'
      and t[13] & 1 == 1 and t[6] & 1 == 0 and t[14:16] == b'\x03\xf0' and t[16:] == b'>UV-K1 KISS TNC test')
last = [l for l in out.splitlines() if l.startswith('LINE')][-7:]
print('\n'.join(last))
check('screen: 2 frames in, 3 out, 1 error, last source F4WAT', any('Frames 2 / 3' in l for l in last)
      and any('KISS errors 1' in l for l in last) and any('Last F4WAT ' in l for l in last))
sys.exit(0 if ok else 1)
