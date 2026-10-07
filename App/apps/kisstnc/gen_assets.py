#!/usr/bin/env python3
# KISS TNC read-only assets: the demodulator's initial state and cosine tables,
# as APRS RX builds them (keep the two in step), and the screen texts.
#
#   ./gen_assets.py kisstnc_assets.bin kisstnc_assets.h
import math, os, sys
sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
from app_assets import Assets

FS = 9600

def cos_table(f):
    per = FS // math.gcd(FS, f)
    return [int(round(127 * math.cos(2 * math.pi * f * n / FS))) for n in range(per)]

a = Assets("KISSTNC")
a.text("T_TITLE", "KISS TNC")
a.text("T_USB", "USB")
a.text("T_NOUSB", "NO USB")
a.text("T_TX", "TX")
a.text("T_RXONLY", "RX ONLY")
a.text("T_RX", "RX ")
a.text("T_TXN", "  TX ")
a.text("T_DROP", "  lost ")
a.text("T_DBM", "dBm")
a.text("T_KEYS", "1 spk 2 test 3 RX")
# dem_t prefix: dc, eight filter/correlator words, pm/ps, r/ks, ring; then the
# cosine tables, so one read initializes the whole front end (as APRS RX).
a.u32("DEMOD_INIT", [2048 << 4] + [0] * 8 + [64, 64] + [0] * 18)
a.i8("COS1200", cos_table(1200))            # 8 entries
a.i8("COS2200", cos_table(2200))            # 48 entries
a.main()
