#!/usr/bin/env python3
"""Bench test with an external master, the 2N7000 on GP10; captures LIN on Saleae channel 0.
Usage: uv run --with logic2-automation --with mpremote python firmware/mastertest.py <outdir>"""
import glob, os, pathlib, statistics, subprocess, sys, tempfile, csv
from saleae import automation
here = pathlib.Path(__file__).parent
sys.path.insert(0, str(here.parent / "decoder"))
import lin
out = os.path.abspath(sys.argv[1]); os.makedirs(out, exist_ok=True)
PLAN = [(19200, 30), (18816, 29), (19584, 29), (18432, 29), (19968, 29)]
src = (here / "master_rp.py").read_text().replace("@@PLAN@@", repr(PLAN))
with automation.Manager.connect(port=10430) as m:
    dev = m.get_devices(include_simulation_devices=False)[0]
    cfg = automation.LogicDeviceConfiguration(enabled_digital_channels=[0], digital_sample_rate=6250000, digital_threshold_volts=3.3)
    with m.start_capture(device_id=dev.device_id, device_configuration=cfg, capture_configuration=automation.CaptureConfiguration(capture_mode=automation.ManualCaptureMode())) as cap, tempfile.NamedTemporaryFile("w", suffix=".py") as f:
        f.write(src); f.flush()
        r = subprocess.run(["mpremote", "connect", glob.glob("/dev/cu.usbmodem*")[0], "run", f.name], capture_output=True, text=True, timeout=240)
        cap.stop()
        cap.export_raw_data_csv(directory=out, digital_channels=[0])
pathlib.Path(out, "debug.log").write_text(r.stdout + r.stderr)
print("\n".join(l[:165] for l in r.stdout.splitlines() if "=== " in l or " hdr=" in l or "status" in l or "cut" in l or "Trace" in l or "rror" in l))
if r.stderr.strip(): print(r.stderr[-400:])
t, v = lin.load(os.path.join(out, "digital.csv"))
frames = lin.decode(t, v)
print("segment at nominal baud only:")
first = [f for f in frames if f["t"] < frames[0]["t"] + 11]
rows = [f for f in first if len(f["bytes"]) >= 2 and f["bytes"][0] == 0x55]
print(" frames", len(first), "valid sync", len(rows), "framing errors", sum(f["errs"] for f in rows))
g, sq = [], []
for f in rows:
    p, d = f["bytes"][1], f["bytes"][2:]
    if p in (0x99, 0x7D):
        print(" " * 0, end="")
        if d: g.append((f["times"][2] - (f["times"][1] + 10 * lin.BIT)) * 1e6)
    if p == 0x7D: print(" 3D at %.3f:" % f["t"], " ".join("%02X" % x for x in d) or "silent")
    if p == 0x99:
        ok = len(d) == 3 and lin.ck(d[:2], 0x99) == d[2]
        sq.append((round(f["t"], 2), tuple(d), ok))
print(" ID19 frames", len(sq), "all valid", all(x[2] for x in sq), "gap us min/median/max", [round(x) for x in (min(g), statistics.median(g), max(g))] if g else None)
last = None
for ts, d, ok in sq:
    if d != last: print("  t=%.2f %s" % (ts, " ".join("%02X" % x for x in d) or "silent")); last = d
