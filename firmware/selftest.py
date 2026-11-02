#!/usr/bin/env python3
"""Runs the self-test on the bench with a Saleae capture of LIN on channel 0 and checks the bus.
Usage: uv run --with logic2-automation --with mpremote python firmware/selftest.py [outdir]"""
import glob, os, pathlib, statistics, subprocess, sys
from saleae import automation
here = pathlib.Path(__file__).parent
sys.path.insert(0, str(here.parent / "decoder"))
import lin
out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "selftest_out")
os.makedirs(out, exist_ok=True)
with automation.Manager.connect(port=10430) as m:
    dev = m.get_devices(include_simulation_devices=False)[0]
    cfg = automation.LogicDeviceConfiguration(enabled_digital_channels=[0], digital_sample_rate=6250000, digital_threshold_volts=3.3)
    with m.start_capture(device_id=dev.device_id, device_configuration=cfg, capture_configuration=automation.CaptureConfiguration(capture_mode=automation.ManualCaptureMode())) as cap:
        r = subprocess.run(["mpremote", "connect", glob.glob("/dev/cu.usbmodem*")[0], "run", str(here / "selftest_rp.py")], capture_output=True, text=True, timeout=240)
        cap.stop()
        cap.export_raw_data_csv(directory=out, digital_channels=[0])
pathlib.Path(out, "debug.log").write_text(r.stdout + r.stderr)
print("\n".join(l for l in r.stdout.splitlines() if not l.split(">")[0].strip().replace(".", "").isdigit() or ">" not in l))
t, v = lin.load(os.path.join(out, "digital.csv"))
frames = lin.decode(t, v)
fails = []
rows = []
for f in frames:
    b = f["bytes"]
    if len(b) < 2 or b[0] != 0x55:
        fails.append(f"bad frame at {f['t']:.3f}: {b}")
        continue
    rows.append((f["t"], b[1], b[2:], f))
print(len(frames), "frames, framing errors", sum(f["errs"] for f in frames), "pid errors", sum(not lin.pid_ok(x[1]) for x in rows))
gaps, seq = [], []
for ts, pid, d, f in rows:
    if pid in (0x99, 0x7D) and d:
        gaps.append((f["times"][2] - (f["times"][1] + 10 * lin.BIT)) * 1e6)
    if pid == 0x99:
        ok = not d or (len(d) == 3 and lin.ck(d[:2], 0x99) == d[2])
        seq.append((round(ts, 2), d[:2], ok))
        if not ok: fails.append(f"ID19 at {ts:.3f}: {d}")
    if pid == 0x7D:
        print("3D at %.3f: %s" % (ts, " ".join("%02X" % x for x in d) or "silent"))
print("ID19 headers:", len(seq), "; response gap us min/median/max", [round(x) for x in (min(gaps), statistics.median(gaps), max(gaps))])
last = None
for ts, d, ok in seq:
    if d != last: print("  t=%.2f %s" % (ts, "current=%d status=%02X" % (d[0], d[1]) if d else "silent")); last = d
print("FAILS:", fails or "none")
