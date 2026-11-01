#!/usr/bin/env python3
"""Virtual Xbox 360 pad + keyboard on the Y700, for testing the player's pad and key paths
without a real controller (tools/results/20261102-input.txt).

The pad copies what the kernel's xpad driver creates for a wired 360 pad (same id, keys and
axes), so SDL finds it in its controller database exactly like the real thing. Run as root;
commands come from a FIFO, one per line:
    press NAME [MS]       buttons: a b x y lb rb back start guide ls rs, held MS (default 80)
    hold NAME | release NAME
    axis NAME VALUE       lx ly rx ry (-32768..32767), lt rt (0..255), hatx haty (-1..1)
    key NAME [MS]         keyboard: f8 esc enter space a..z
    sleep MS | quit
usage: vpad.py [--fifo /run/yy-vpad.fifo]   (systemd-run --unit yy-vpad python3 vpad.py)"""
import argparse, os, time
from evdev import ecodes as e, AbsInfo, UInput

ap = argparse.ArgumentParser()
ap.add_argument("--fifo", default="/run/yy-vpad.fifo")
fifo = ap.parse_args().fifo

BUTTONS = {"a": e.BTN_A, "b": e.BTN_B, "x": e.BTN_X, "y": e.BTN_Y, "lb": e.BTN_TL, "rb": e.BTN_TR,
           "back": e.BTN_SELECT, "start": e.BTN_START, "guide": e.BTN_MODE, "ls": e.BTN_THUMBL, "rs": e.BTN_THUMBR}
stick, trigger, hat = AbsInfo(0, -32768, 32767, 16, 128, 0), AbsInfo(0, 0, 255, 0, 0, 0), AbsInfo(0, -1, 1, 0, 0, 0)
AXES = {"lx": (e.ABS_X, stick), "ly": (e.ABS_Y, stick), "rx": (e.ABS_RX, stick), "ry": (e.ABS_RY, stick),
        "lt": (e.ABS_Z, trigger), "rt": (e.ABS_RZ, trigger), "hatx": (e.ABS_HAT0X, hat), "haty": (e.ABS_HAT0Y, hat)}
KEYS = {"f8": e.KEY_F8, "esc": e.KEY_ESC, "enter": e.KEY_ENTER, "space": e.KEY_SPACE,
        **{c: getattr(e, f"KEY_{c.upper()}") for c in "abcdefghijklmnopqrstuvwxyz"}}

pad = UInput({e.EV_KEY: sorted(BUTTONS.values()), e.EV_ABS: sorted(AXES.values())},
             name="Microsoft X-Box 360 pad", vendor=0x045E, product=0x028E, version=0x0114, bustype=e.BUS_USB)
keyboard = UInput({e.EV_KEY: sorted(KEYS.values())}, name="yy-vpad keyboard")
held = set()

def button(dev, code, down):
    dev.write(e.EV_KEY, code, int(down)); dev.syn()
    (held.add if down else held.discard)((dev, code))

def run(line):
    p = line.split()
    if not p:
        return True
    cmd, args = p[0], p[1:]
    if cmd in ("press", "key"):
        dev, code = (pad, BUTTONS[args[0]]) if cmd == "press" else (keyboard, KEYS[args[0]])
        button(dev, code, True); time.sleep(int(args[1] if len(args) > 1 else 80) / 1000); button(dev, code, False)
    elif cmd in ("hold", "release"):
        button(pad, BUTTONS[args[0]], cmd == "hold")
    elif cmd == "axis":
        pad.write(e.EV_ABS, AXES[args[0]][0], int(args[1])); pad.syn()
    elif cmd == "sleep":
        time.sleep(int(args[0]) / 1000)
    elif cmd == "quit":
        return False
    else:
        print("unknown command:", line.strip(), flush=True)
    return True

if os.path.exists(fifo):
    os.unlink(fifo)
os.mkfifo(fifo, 0o600)
print(f"yy-vpad ready: {fifo}", flush=True)
try:
    alive = True
    while alive:
        with open(fifo) as f:  # blocks until a writer opens it; EOF when it closes
            for line in f:
                try:
                    alive = run(line)
                except Exception as ex:  # a bad line must never leave a button down
                    print("error:", line.strip(), ex, flush=True)
                    for dev, code in list(held):
                        button(dev, code, False)
                if not alive:
                    break
finally:
    for dev, code in list(held):
        button(dev, code, False)
    pad.close(); keyboard.close()
    os.unlink(fifo)
