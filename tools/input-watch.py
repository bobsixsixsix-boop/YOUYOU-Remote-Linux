"""PC side of the input check (Windows, on the computer being streamed): logs every change
of the XInput pads GameViewer creates for the tablet's controller, and Windows' console
display state (privacy mode 3 turns the monitors off). Pair with tools/vpad.py on the Y700;
tools/results/20261102-input.txt.
Usage: python tools/input-watch.py <seconds> [out.txt]"""
import ctypes, sys, time, uuid

secs = float(sys.argv[1])
out = open(sys.argv[2], "w", encoding="utf-8") if len(sys.argv) > 2 else None
T0 = time.time()
def log(text):
    line = f"[{time.time() - T0:6.2f}] {text}"
    print(line, flush=True)
    if out:
        out.write(line + "\n"); out.flush()

class Gamepad(ctypes.Structure):
    _fields_ = [("buttons", ctypes.c_ushort), ("lt", ctypes.c_ubyte), ("rt", ctypes.c_ubyte),
                ("lx", ctypes.c_short), ("ly", ctypes.c_short), ("rx", ctypes.c_short), ("ry", ctypes.c_short)]
class State(ctypes.Structure):
    _fields_ = [("packet", ctypes.c_ulong), ("pad", Gamepad)]
xinput = ctypes.windll.xinput1_4
NAMES = {0x1: "up", 0x2: "down", 0x4: "left", 0x8: "right", 0x10: "start", 0x20: "back", 0x40: "ls", 0x80: "rs",
         0x100: "lb", 0x200: "rb", 0x1000: "a", 0x2000: "b", 0x4000: "x", 0x8000: "y"}

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

seen = [None] * 4
while time.time() - T0 < secs:
    for i in range(4):
        s = State()
        now = None
        if xinput.XInputGetState(i, ctypes.byref(s)) == 0:
            g = s.pad
            now = (g.buttons, g.lt, g.rt, g.lx, g.ly, g.rx, g.ry)
        if now != seen[i]:
            if now is None:
                log(f"pad {i} gone")
            else:
                pressed = "+".join(n for bit, n in NAMES.items() if now[0] & bit) or "-"
                log(f"pad {i} buttons {pressed} lt {now[1]} rt {now[2]} l ({now[3]},{now[4]}) r ({now[5]},{now[6]})")
            seen[i] = now
    time.sleep(0.005)
