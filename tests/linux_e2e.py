#!/usr/bin/env python3
"""End-to-end test of the Linux daemon with real kernel devices (run as root on Linux / WSL2).

Creates a fake physical mouse through uinput, starts a master and a slave daemon on a private UDP
port, moves the fake mouse across the master's right edge, clicks and scrolls, and checks what the
slave's virtual pointer emitted.

    sudo python3 tests/linux_e2e.py path/to/net-mouse-daemon
"""

import fcntl
import json
import os
import re
import struct
import subprocess
import sys
import tempfile
import time

EV_SYN, EV_KEY, EV_REL, EV_ABS = 0, 1, 2, 3
REL_X, REL_Y, REL_WHEEL, REL_WHEEL_HI_RES = 0, 1, 8, 11
ABS_X = 0
BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, BTN_SIDE = 0x110, 0x111, 0x112, 0x113

UI_SET_EVBIT, UI_SET_KEYBIT, UI_SET_RELBIT = 0x40045564, 0x40045565, 0x40045566
UI_DEV_SETUP, UI_DEV_CREATE, UI_DEV_DESTROY = 0x405C5503, 0x5501, 0x5502
EVENT = struct.Struct("llHHi")
PORT = "47899"  # keep away from real installations on the network


class FakeMouse:
    def __init__(self, name="Test Physical Mouse"):
        self.fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
        fcntl.ioctl(self.fd, UI_SET_EVBIT, EV_KEY)
        fcntl.ioctl(self.fd, UI_SET_EVBIT, EV_REL)
        for code in (BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, BTN_SIDE):
            fcntl.ioctl(self.fd, UI_SET_KEYBIT, code)
        for code in (REL_X, REL_Y, REL_WHEEL):
            fcntl.ioctl(self.fd, UI_SET_RELBIT, code)
        setup = struct.pack("HHHH80sI", 0x03, 0x1234, 0x5678, 1, name.encode(), 0)
        fcntl.ioctl(self.fd, UI_DEV_SETUP, setup)
        fcntl.ioctl(self.fd, UI_DEV_CREATE)

    def emit(self, *events):
        for type_, code, value in events:
            os.write(self.fd, EVENT.pack(0, 0, type_, code, value))
        os.write(self.fd, EVENT.pack(0, 0, EV_SYN, 0, 0))

    def move(self, dx, dy=0):
        self.emit((EV_REL, REL_X, dx), (EV_REL, REL_Y, dy))

    def close(self):
        fcntl.ioctl(self.fd, UI_DEV_DESTROY)
        os.close(self.fd)


KEY_LEFTCTRL, KEY_A, KEY_LEFTALT, KEY_HOME = 29, 30, 56, 102


class FakeKeyboard(FakeMouse):
    def __init__(self, name="Test Physical Keyboard"):
        self.fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
        fcntl.ioctl(self.fd, UI_SET_EVBIT, EV_KEY)
        for code in range(1, 128):  # a full main block including A, Z, Ctrl, Alt, Home
            fcntl.ioctl(self.fd, UI_SET_KEYBIT, code)
        setup = struct.pack("HHHH80sI", 0x03, 0x1234, 0x5679, 1, name.encode(), 0)
        fcntl.ioctl(self.fd, UI_DEV_SETUP, setup)
        fcntl.ioctl(self.fd, UI_DEV_CREATE)

    def tap(self, code):
        self.emit((EV_KEY, code, 1))
        self.emit((EV_KEY, code, 0))


def input_devices():
    """{name: (event node, handlers)} from /proc/bus/input/devices."""
    result = {}
    for block in open("/proc/bus/input/devices").read().split("\n\n"):
        name = re.search(r'N: Name="(.*)"', block)
        handlers = re.search(r"H: Handlers=(.*)", block)
        if name and handlers:
            event = re.search(r"event\d+", handlers.group(1))
            result[name.group(1)] = ("/dev/input/" + event.group(0) if event else None, handlers.group(1))
    return result


class Reader:
    def __init__(self, path):
        self.fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)

    def drain(self):
        events = []
        while True:
            try:
                data = os.read(self.fd, EVENT.size * 64)
            except BlockingIOError:
                return events
            for i in range(0, len(data), EVENT.size):
                _, _, type_, code, value = EVENT.unpack_from(data, i)
                if type_ != EV_SYN:
                    events.append((type_, code, value))


def wait_for(predicate, timeout=10.0):
    end = time.time() + timeout
    while time.time() < end:
        if predicate():
            return True
        time.sleep(0.1)
    return False


def main():
    daemon = sys.argv[1] if len(sys.argv) > 1 else "build-linux/net-mouse-daemon"
    tmp = tempfile.mkdtemp(prefix="nm-e2e-")
    os.chmod(tmp, 0o755)  # the unprivileged-client check connects as "nobody"
    for role in ("master", "slave"):
        with open(f"{tmp}/{role}.ini", "w") as f:
            f.write(f"role={role}\nport={PORT}\nmonitors=1920x1080+0+0\nname=e2e-{role}\n")

    failures = []

    def check(cond, what):
        print(("ok   " if cond else "FAIL ") + what)
        if not cond:
            failures.append(what)

    mouse = FakeMouse()
    keyboard = FakeKeyboard()
    time.sleep(0.3)
    logs = {r: open(f"{tmp}/{r}.log", "w+") for r in ("master", "slave")}
    procs = {r: subprocess.Popen([daemon, "--config", f"{tmp}/{r}.ini", "--control", f"{tmp}/{r}.sock"],
                                 stdout=logs[r], stderr=subprocess.STDOUT)
             for r in ("slave", "master")}
    try:
        log = lambda r: open(f"{tmp}/{r}.log").read()
        check(wait_for(lambda: "Configuration from master applied" in log("slave")), "slave configured by master")
        check(wait_for(lambda: "Using mouse 'Test Physical Mouse'" in log("master")), "master grabbed the fake mouse")
        check(wait_for(lambda: "Using keyboard 'Test Physical Keyboard'" in log("master")),
              "master grabbed the fake keyboard")
        devices = input_devices()
        check(all("js" not in h for n, (_, h) in devices.items() if n.startswith("net-mouse")),
              "virtual devices are not exposed as joysticks")
        master_ptr = Reader(devices["net-mouse master pointer"][0])
        slave_ptr = Reader(devices["net-mouse slave pointer"][0])
        master_keys = Reader(devices["net-mouse master pointer (buttons)"][0])
        slave_keys = Reader(devices["net-mouse slave pointer (buttons)"][0])
        keys_of = lambda ev: [(c, v) for t, c, v in ev if t == EV_KEY]
        time.sleep(0.5)  # let the slave's next hello confirm the configuration

        keyboard.tap(KEY_A)
        time.sleep(0.2)
        check(keys_of(master_keys.drain()) == [(KEY_A, 1), (KEY_A, 0)] and not keys_of(slave_keys.drain()),
              "keyboard: keys stay on the master while the pointer is there")

        for _ in range(9):  # 960 -> 1860: stays on the master
            mouse.move(100)
            time.sleep(0.02)
        time.sleep(0.2)
        ev = master_ptr.drain()
        xs = [v for t, c, v in ev if t == EV_ABS and c == ABS_X]
        check(bool(xs) and xs[-1] == 1860, f"local motion re-injected on the master (last x={xs[-1] if xs else None})")
        check(not slave_ptr.drain(), "slave idle while the pointer is on the master")

        for _ in range(6):  # crosses the right edge at 1919 and continues 500 px into the slave
            mouse.move(100)
            time.sleep(0.02)
        time.sleep(0.3)
        ev = slave_ptr.drain()
        # Track the device's X state (a new device starts at 0; the kernel drops unchanged values).
        x, first_frame_x = 0, None
        for t, c, v in ev:
            if t == EV_ABS:
                if c == ABS_X:
                    x = v
                if first_frame_x is None and c == 1:  # ABS_Y closes the first position frame
                    first_frame_x = x
        check(first_frame_x == 0, f"pointer entered the slave at its left edge (x={first_frame_x})")
        check(x == 500, f"pointer moved on the slave (last x={x})")
        ev = master_ptr.drain()
        check(all(not (t == EV_ABS and c == ABS_X and v != 1919) for t, c, v in ev),
              "master cursor stays at the edge while remote")

        mouse.emit((EV_KEY, BTN_LEFT, 1))
        mouse.emit((EV_KEY, BTN_LEFT, 0))
        mouse.emit((EV_REL, REL_WHEEL, -1))
        time.sleep(0.3)
        ev = slave_ptr.drain()
        check((EV_KEY, BTN_LEFT, 1) in ev and (EV_KEY, BTN_LEFT, 0) in ev, "click delivered to the slave")
        check((EV_REL, REL_WHEEL, -1) in ev, "wheel delivered to the slave")
        check(not any(t == EV_KEY for t, _, _ in master_ptr.drain()), "click not delivered to the master")

        keyboard.tap(KEY_A)
        time.sleep(0.3)
        check(keys_of(slave_keys.drain()) == [(KEY_A, 1), (KEY_A, 0)], "keyboard: keys go to the slave with the pointer")
        check(not keys_of(master_keys.drain()), "keyboard: the master does not see them")

        for _ in range(7):  # back across the edge: 2420 -> 1720
            mouse.move(-100)
            time.sleep(0.02)
        time.sleep(0.3)
        ev = master_ptr.drain()
        xs = [v for t, c, v in ev if t == EV_ABS and c == ABS_X]
        check(bool(xs) and xs[-1] == 1720, f"pointer returned to the master (last x={xs[-1] if xs else None})")

        # Emergency hotkey while the keyboard goes to the slave.
        for _ in range(3):  # 1720 -> back across the edge onto the slave
            mouse.move(100)
            time.sleep(0.02)
        time.sleep(0.2)
        slave_keys.drain()
        for code in (KEY_LEFTCTRL, KEY_LEFTALT, KEY_HOME):
            keyboard.emit((EV_KEY, code, 1))
        time.sleep(0.3)
        for code in (KEY_HOME, KEY_LEFTALT, KEY_LEFTCTRL):
            keyboard.emit((EV_KEY, code, 0))
        time.sleep(0.3)
        check("Emergency hotkey" in log("master"), "Ctrl+Alt+Home returns the pointer to the master")
        received = keys_of(slave_keys.drain())
        check((KEY_HOME, 1) not in received and (KEY_LEFTCTRL, 0) in received and (KEY_LEFTALT, 0) in received,
              f"slave released Ctrl/Alt and never saw Home ({received})")

        # Control socket (used by net-mouse-gui / net-mouse-ctl).
        query = lambda r, req: subprocess.run([daemon, "--control", f"{tmp}/{r}.sock", "--query", req],
                                              capture_output=True, text=True).stdout.strip()
        status = json.loads(query("master", "status"))
        slaves = status.get("slaves", [])
        check(status.get("role") == "master" and len(slaves) == 1 and slaves[0]["online"],
              "control socket: master status lists the online slave")
        check(query("master", f"set-offset {slaves[0]['id']} 0 0").startswith("error"),
              "control socket: overlapping position is refused")
        check(query("master", f"set-offset {slaves[0]['id']} -1920 0") == "ok",
              "control socket: slave moved to the left of the master")
        check(wait_for(lambda: "desktop origin at (-1920, 0)" in log("slave")), "slave received the new position")
        check(json.loads(query("slave", "status")).get("offset") == {"x": -1920, "y": 0},
              "control socket: slave status shows the new origin")
        unprivileged = subprocess.run(["setpriv", "--reuid=65534", "--regid=65534", "--clear-groups", daemon,
                                       "--control", f"{tmp}/master.sock", "--query",
                                       f"set-offset {slaves[0]['id']} 1920 0"], capture_output=True, text=True)
        check("permission denied" in unprivileged.stdout,
              f"control socket: changes refused for unprivileged users ({(unprivileged.stdout + unprivileged.stderr).strip()})")
    finally:
        for p in procs.values():
            p.terminate()
        for p in procs.values():
            p.wait(timeout=5)
        mouse.close()
        keyboard.close()
        for r in ("master", "slave"):
            print(f"--- {r} log")
            print(open(f"{tmp}/{r}.log").read().rstrip())

    print("PASSED" if not failures else f"FAILED: {len(failures)} check(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
