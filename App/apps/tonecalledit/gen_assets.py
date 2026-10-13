#!/usr/bin/env python3
# ToneCall Edit assets: the configuration-proxy descriptor that points the
# app's cfg_load/cfg_save at ToneCall's preset block (../tonecall_presets.h),
# then the T9 key map used to type preset names.
#
#   ./gen_assets.py tonecalledit_assets.bin tonecalledit_assets.h
import os, struct, sys
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from app_assets import Assets, cdefine

PROXY_MAGIC = cdefine("app_config_proxy.h", "APP_CONFIG_PROXY_MAGIC")
PRESETS_MAGIC = cdefine("tonecall_presets.h", "TC_PRESETS_MAGIC")
PRESETS_LEN = cdefine("tonecall_presets.h", "TC_PRESETS_LEN")
TARGET = b"ToneCall"

# app_config_proxy_t: magic, padded target name, target asset offset/length,
# reserved byte, then the marker expected at the target offset.
proxy = struct.pack("<I16sHBBI", PROXY_MAGIC, TARGET.ljust(16, b"\0"),
                    0, PRESETS_LEN, 0, PRESETS_MAGIC)
if len(proxy) != 28:
    sys.exit("configuration proxy descriptor layout changed")

a = Assets("TONECALLEDIT")
a.prefix("CFG_PROXY", proxy)
KEYS = [" ", ".-/", "ABC", "DEF", "GHI", "JKL", "MNO", "PQRS", "TUV", "WXYZ"]
a.raw("KEYMAP", b"".join(k.encode("ascii").ljust(5, b"\0") for k in KEYS))
a.const("KEYMAP_STRIDE", 5)
a.main()
