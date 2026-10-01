#!/usr/bin/env python3
"""Check the decoder's demodulated bits on the real K1 512 bps captures.

The 385 ms windows hold no sync word, so the decoder cannot frame them. Instead
the bits it produces (traced from pocsag.c) are compared with ground truth:
the level after each bit edge is the sign of the edge's pulse (the K1 audio is
close to a differentiator). Ground truth is checked against the address and
message words rpitx sends for "1234:test". Prints OK and the error counts, or
FAIL. Usage: k1_bits512.py <dir of pocsag.c>
"""
import os
import subprocess
import sys
import tempfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..")
T = 9600 / 512.4                       # measured bit period on the captures

TRACE_MAIN = r"""
#include <stdio.h>
#include <stdlib.h>
#include "pocsag.h"
int main(int c, char **v) { static poc_t d; poc_init(&d, 0, atoi(v[2]));
  FILE *f = fopen(v[1], "rb"); unsigned char b[2];
  while (fread(b, 1, 2, f) == 2) poc_push(&d, b[0] | b[1] << 8); return 0; }
"""


def build(tmp):
    src = open(os.path.join(SRC, "pocsag.c")).read()
    src = src.replace('#include "pocsag.h"', '#include "pocsag.h"\n#include <stdio.h>')
    key = "    uint8_t b = d->acc < 0;"
    i = src.index(key)
    j = src.index("\n", i)
    src = src[:j + 1] + '    printf("%d\\n", b);\n' + src[j + 1:]
    open(os.path.join(tmp, "trace.c"), "w").write(src)
    open(os.path.join(tmp, "main.c"), "w").write(TRACE_MAIN)
    exe = os.path.join(tmp, "trace")
    subprocess.check_call(["clang", "-O1", "-I", SRC, "-o", exe,
                           os.path.join(tmp, "main.c"), os.path.join(tmp, "trace.c")])
    return exe


def truth(x):
    idx = [i for i in range(1, len(x) - 1)
           if abs(x[i]) > 250 and abs(x[i]) >= abs(x[i - 1]) and abs(x[i]) > abs(x[i + 1])]
    e = []
    for i in idx:
        if not e or i - e[-1] > 3:
            e.append(i)
    ph = np.angle(np.exp(2j * np.pi * np.array(e) / T).mean()) * T / (2 * np.pi)
    out, j, cur = [], 0, None
    for k in range(int((len(x) - ph) / T) - 1):
        c = ph + (k + 0.5) * T
        while j < len(e) and e[j] < c:
            cur = x[e[j]] > 0
            j += 1
        out.append("?" if cur is None else str(int(not cur)))
    return "".join(out)


def main():
    with tempfile.TemporaryDirectory() as tmp:
        exe = build(tmp)
        res, ok = [], True
        for name in ("k1_1234_test_512_a.u16", "k1_1234_test_512_b.u16"):
            f = os.path.join(HERE, "k1", name)
            x = np.fromfile(f, dtype="<u2").astype(float)
            x -= np.median(x)
            ts = truth(x)
            my = "".join(subprocess.run([exe, f, "5"], capture_output=True, text=True).stdout.split())
            best = min(
                sum(1 for i in range(40, min(len(ts) - max(0, o), len(my) - max(0, -o)))
                    if ts[i + max(0, o)] != "?" and ts[i + max(0, o)] != str(int(my[i + max(0, -o)]) ^ inv))
                for o in range(-5, 6) for inv in (0, 1))
            res.append(f"{name[-7:-4]} {best} bit errors")
            ok &= best <= 2
        print(("OK " if ok else "FAIL ") + ", ".join(res))


if __name__ == "__main__":
    main()
