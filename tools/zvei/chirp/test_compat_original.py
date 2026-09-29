"""Cross-compatibility: an image saved by the ZVEI driver opens with the original driver.
Same setup as test_chirp_zvei.py. Run twice (two processes, CHIRP refuses two drivers with the same id):
    python test_compat_original.py <dir with chirp checkout> <zvei driver> <original driver> save
    python test_compat_original.py <dir with chirp checkout> <zvei driver> <original driver> load
"""
import sys, types, importlib.util
S, DRV, ORIG = sys.argv[1:4]
sys.path.insert(0, S + "/chirp"); sys.modules["wx"] = types.ModuleType("wx")
from chirp import memmap
def load(path, name):
    spec = importlib.util.spec_from_file_location(name, path); m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m); return m
which = sys.argv[4]
if which == "save":      # ZVEI driver writes an image with channel 1024 + ZVEI
    d = load(DRV, "z"); r = d.UVK5RadioEgzumer(None); r._mmap = memmap.MemoryMapBytes(b"\xFF" * d.MEM_SIZE); r.process_mmap()
    m = r.get_memory(1024); m.empty = False; m.freq = 145500000; m.mode = "FM"; m.power = r.get_features().valid_power_levels[0]
    m.extra["zvei_type"].value = "ZVEI-1"; m.extra["zvei_code1"].value = "12345"; r.set_memory(m)
    open("/tmp/zvei_chirp_test.img", "wb").write(r._mmap.get_packed()); print("saved", len(r._mmap))
else:                    # original driver reads it
    o = load(ORIG, "o"); r = o.UVK5RadioEgzumer(None); r._mmap = memmap.MemoryMapBytes(open("/tmp/zvei_chirp_test.img", "rb").read()); r.process_mmap()
    f = r.get_memory(1024).freq; print(("ok   " if f == 145500000 else "FAIL ") + "original driver reads a ZVEI image, ch1024 freq", f)
