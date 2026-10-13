#!/usr/bin/env python3
"""Host checks: ToneCall Edit's proxy descriptor points at ToneCall's preset
block, and the block ToneCall ships matches ../tonecall_presets.h."""
import os, re, struct, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
APPS = os.path.join(HERE, "..", "..")


def assets(app):
    with tempfile.TemporaryDirectory() as d:
        b, h = os.path.join(d, "a.bin"), os.path.join(d, "a.h")
        subprocess.run([sys.executable, os.path.join(APPS, app, "gen_assets.py"), b, h], check=True)
        defs = {k: int(v) for k, v in re.findall(r"#define (\w+) (\d+)u", open(h).read())}
        return open(b, "rb").read(), defs


def define(name):
    text = open(os.path.join(APPS, "tonecall_presets.h")).read()
    return int(re.search(r"#define " + name + r"\s+(0x[0-9A-Fa-f]+|\d+)", text).group(1), 0)


def build_name(app):
    text = open(os.path.join(APPS, app, "build.sh")).read()
    return re.search(r'APP_NAME="([^"]+)"', text).group(1)


def main():
    magic, length = define("TC_PRESETS_MAGIC"), define("TC_PRESETS_LEN")
    count, rec = define("TC_PRESET_COUNT"), define("TC_REC_LEN")
    edit, edefs = assets("tonecalledit")
    tc, tdefs = assets("tonecall")
    pmagic, target, offset, plen, reserved, tmagic = struct.unpack("<I16sHBBI", edit[:28])
    block = tc[tdefs["F_PRESETS"]:tdefs["F_PRESETS"] + length]
    checks = {
        "proxy descriptor at asset offset 0": edefs["CFG_PROXY"] == 0,
        "proxy magic CXP1": pmagic == 0x31505843,
        "proxy target is ToneCall's packed name": target.rstrip(b"\0").decode() == build_name("tonecall"),
        "proxy offset = F_PRESETS": offset == tdefs["F_PRESETS"],
        "proxy length = TC_PRESETS_LEN": plen == length,
        "proxy length within APP_CONFIG_PROXY_MAX": plen <= 117,
        "proxy reserved byte 0": reserved == 0,
        "proxy expects the block magic": tmagic == magic,
        "block starts with its magic": struct.unpack("<I", block[:4])[0] == magic,
        "block ships every preset empty": all(block[4 + i * rec + 8] == 0 for i in range(count)),
        "block names are blank": all(block[4 + i * rec:4 + i * rec + 8] == b" " * 8 for i in range(count)),
        "app names fit 16 bytes": len(build_name("tonecall")) < 16 and len(build_name("tonecalledit")) < 16,
    }
    for name, ok in checks.items():
        print(("ok   " if ok else "FAIL ") + name)
    failed = sum(not ok for ok in checks.values())
    print(f"{'FAILED' if failed else 'PASSED'}: {failed} failure(s)")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
