#!/usr/bin/env python3
# ToneCall assets: the mutable preset block, edited by ToneCall Edit through
# the configuration proxy (see ../tonecall_presets.h). It ships empty: no code
# is built in, the user defines every preset.
#
#   ./gen_assets.py tonecall_assets.bin tonecall_assets.h
import os, struct, sys
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from app_assets import Assets, cdefine

MAGIC = cdefine("tonecall_presets.h", "TC_PRESETS_MAGIC")
COUNT = cdefine("tonecall_presets.h", "TC_PRESET_COUNT")
REC_LEN = cdefine("tonecall_presets.h", "TC_REC_LEN")
LEN = cdefine("tonecall_presets.h", "TC_PRESETS_LEN")

empty = b" " * 8 + b"\x00" + b"\x00" * 4          # no name, no digits
block = struct.pack("<I", MAGIC) + empty * COUNT
if len(empty) != REC_LEN or len(block) != LEN:
    sys.exit("preset block layout changed")

a = Assets("TONECALL")
a.prefix("F_PRESETS", block)
a.main()
