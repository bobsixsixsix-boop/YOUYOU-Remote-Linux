"""PC side of one dedicated-desktop measurement (Windows, run on the computer being
streamed). Starts tools/stream-run.sh on the Y700, waits for the virtual display
GameViewer adds for the session, shows y700's motion.html on it, ends the session after
the tablet's measurement and copies the tablet's results to tools/results/runs/<label>.

The computer's physical monitors stay on: the motion page opens on the virtual display
only. Turning them off in software is not an option anyway: GameViewer keeps them awake
during a session, a software display-off stops the stream, and it puts them back within a
second if they are taken out of the layout (README, root causes). host-display-power.txt
logs Windows' console display state for reference.

Needs y700's linux/streaming (host-run.py, motion.html) in Y700_STREAMING and the build
in /root/yybuild on the tablet (ML_TABLET, default as in host-run.py).
Usage: python tools/desktop-run.py <pc> <label> <WxH@fps> <mbps> [settle_s] [measure_s]"""
import ctypes, importlib.util, os, subprocess, sys, time, uuid
from pathlib import Path

streaming = Path(os.environ.get("Y700_STREAMING", ""))
if not (streaming / "host-run.py").exists():
    sys.exit("set Y700_STREAMING to y700's linux/streaming directory")
spec = importlib.util.spec_from_file_location("hr", streaming / "host-run.py")
hr = importlib.util.module_from_spec(spec); spec.loader.exec_module(hr)  # also makes us per-monitor DPI aware
HERE = Path(__file__).resolve().parent
pc, label, stream, mbps = sys.argv[1:5]
settle, secs = (int(x) for x in (sys.argv[5:7] + ["20", "30"][len(sys.argv[5:7]):]))
out = HERE / "results" / "runs" / label
out.mkdir(parents=True, exist_ok=True)
T0 = time.time()
plog = open(out / "host-display-power.txt", "w")
def log(text):
    line = f"[{time.time() - T0:5.1f}] {text}"
    print(line, flush=True); plog.write(line + "\n"); plog.flush()

# GUID_CONSOLE_DISPLAY_STATE: 0 off, 1 on, 2 dimmed; the callback fires once on subscribe
CB = ctypes.WINFUNCTYPE(ctypes.c_ulong, ctypes.c_void_p, ctypes.c_ulong, ctypes.c_void_p)
def on_power(ctx, kind, setting):
    if kind == 0x8013:  # PBT_POWERSETTINGCHANGE
        state = ctypes.c_ulong.from_address(setting + 20).value
        log(f"display {['off', 'on', 'dimmed'][state] if state < 3 else state}")
    return 0
callback = CB(on_power)
class Params(ctypes.Structure):
    _fields_ = [("cb", CB), ("ctx", ctypes.c_void_p)]
params, handle = Params(callback, None), ctypes.c_void_p()
guid = (ctypes.c_ubyte * 16).from_buffer_copy(uuid.UUID("6fe69556-704a-47a0-8f24-c28d936fda47").bytes_le)
assert ctypes.windll.powrprof.PowerSettingRegisterNotification(guid, 2, ctypes.byref(params), ctypes.byref(handle)) == 0

before = set(hr.monitors())
pl = hr.PageLog(out / "host-page.csv"); pl.start()
try:
    subprocess.run(hr.SCP + [str(HERE / "stream-run.sh"), f"{hr.TABLET}:/tmp/stream-run.sh"], check=True)
    env = f"YY_STREAM={stream} YY_MBPS={mbps} YY_DESKTOP=1 {os.environ.get('YY_EXTRA', '')}".strip()
    hr.ssh(f"systemd-run --unit=yy-stream --collect '--setenv=YY_ENV={env}' /bin/bash /tmp/stream-run.sh "
           f"{pc} {label} {settle} {secs} 30")
    log(f"tablet started: {env}")
    vd = None
    while vd is None and time.time() - T0 < 40:
        vd = next((m for m in hr.monitors() if m not in before), None)
        time.sleep(0.1)
    log(f"virtual display: {vd}")
    if vd:
        # the player asks the host for the stream size as the desktop size
        want = tuple(int(v) for v in stream.split("@")[0].split("x"))
        for _ in range(50):
            if any(m[2:] == want for m in hr.monitors() if m not in before):
                break
            time.sleep(0.1)
        vd = next((m for m in hr.monitors() if m not in before), vd)
        log(f"virtual display now: {vd}")
        hr.kill_edge()
        subprocess.Popen([hr.EDGE, f"--user-data-dir={hr.EDGE_PROFILE}", "--inprivate", "--no-first-run",
                          "--no-default-browser-check", "--disable-sync", "--disable-extensions",
                          "--disable-features=Translate,msTranslate,msUndersideButton",
                          f"--window-position={vd[0]},{vd[1]}", f"--window-size={vd[2]},{vd[3]}",
                          "--kiosk", "--edge-kiosk-type=fullscreen",
                          f"file:///{(streaming / 'motion.html').as_posix()}?mode=dynamic"])
    time.sleep(max(0, T0 + 2 + settle + secs + 2 - time.time()))
    log(f"displays at the end: {sorted(hr.monitors())}")
finally:
    hr.kill_edge()
    hr.ssh("pkill -TERM -f '^/tmp/yyplay/youyou connect'", check=False)
for _ in range(30):
    if hr.ssh(f"cat /tmp/yyrun/{label}/done 2>/dev/null", check=False).stdout.strip():
        break
    time.sleep(1)
pl.stop()
subprocess.run(hr.SCP + ["-r", f"{hr.TABLET}:/tmp/yyrun/{label}/*", str(out)], check=False)
for _ in range(20):
    if set(hr.monitors()) == before:
        break
    time.sleep(0.5)
log(f"done; virtual display removed: {set(hr.monitors()) == before}")
plog.close()
