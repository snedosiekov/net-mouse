#!/usr/bin/env python3
"""Net Mouse settings for Linux.

Talks to net-mouse-daemon over its control socket (/run/net-mouse/control.sock):
  * master: drag the slave screens to where the PCs physically are (like the Windows app),
  * this PC's monitor layout (the daemon cannot see the desktop's arrangement by itself),
  * the role of this PC.

Requires python3-tk. Changing settings needs root or membership in sudo/wheel/admin/netmouse.
"""

import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tkinter as tk
from tkinter import messagebox

SOCKET_PATH = os.environ.get("NETMOUSE_CONTROL", "/run/net-mouse/control.sock")
POLL_MS = 400
SNAP_PX = 12

MASTER_FILL = "#3465a4"
SLAVE_ONLINE_FILL = "#2e8b57"
SLAVE_DRAG_FILL = "#5ab482"
BORDER = "#1e1e1e"
ACTIVE = "#ff8c00"
CANVAS_BG = "#eceef2"


class Client:
    """Persistent line-based connection to the daemon; reconnects on demand."""

    def __init__(self, path):
        self.path = path
        self.sock = None
        self.buffer = b""

    def request(self, line):
        for _ in range(2):
            try:
                if self.sock is None:
                    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                    s.settimeout(2.0)
                    s.connect(self.path)
                    self.sock, self.buffer = s, b""
                self.sock.sendall(line.encode() + b"\n")
                while b"\n" not in self.buffer:
                    chunk = self.sock.recv(65536)
                    if not chunk:
                        raise ConnectionError("closed")
                    self.buffer += chunk
                reply, self.buffer = self.buffer.split(b"\n", 1)
                return reply.decode()
            except (OSError, ConnectionError):
                self.close()
        return None

    def close(self):
        if self.sock is not None:
            try:
                self.sock.close()
            except OSError:
                pass
        self.sock = None


def detect_monitors(root):
    """This session's monitor layout as 'WxH+X+Y,...' (primary first), or None."""
    if shutil.which("xrandr"):
        try:
            out = subprocess.run(["xrandr", "--listmonitors"], capture_output=True, text=True, timeout=5).stdout
        except (OSError, subprocess.SubprocessError):
            out = ""
        found = []
        for line in out.splitlines():
            m = re.search(r"(\d+)/\d+x(\d+)/\d+\+(-?\d+)\+(-?\d+)", line)
            if m:
                w, h, x, y = (int(v) for v in m.groups())
                found.append(("*" in line.split()[1] if len(line.split()) > 1 else False, f"{w}x{h}+{x}+{y}"))
        if found:
            found.sort(key=lambda item: not item[0])  # primary first
            return ",".join(spec for _, spec in found)
    w, h = root.winfo_screenwidth(), root.winfo_screenheight()
    return f"{w}x{h}+0+0" if w > 0 and h > 0 else None


class App:
    def __init__(self, root):
        self.root = root
        self.client = Client(SOCKET_PATH)
        self.status = None
        self.drag = None  # dict(id, start=(x, y), start_offset=(x, y), offset=(x, y))
        self.scale, self.origin = 0.1, (0.0, 0.0)

        root.title("Net Mouse")
        root.geometry("900x680")
        root.minsize(560, 420)

        self.header = tk.Label(root, anchor="w", font=("TkDefaultFont", 11, "bold"), padx=10, pady=6)
        self.header.pack(fill="x")
        self.canvas = tk.Canvas(root, background=CANVAS_BG, highlightthickness=0)
        self.canvas.pack(fill="both", expand=True, padx=10)
        self.info = tk.Label(root, anchor="w", justify="left", padx=10, pady=6)
        self.info.pack(fill="x")

        this_pc = tk.LabelFrame(root, text="This PC", padx=8, pady=6)
        this_pc.pack(fill="x", padx=10, pady=(0, 10))
        tk.Label(this_pc, text="Monitors (WxH+X+Y,...; first = primary):").grid(row=0, column=0, sticky="w")
        self.monitors_var = tk.StringVar()
        tk.Entry(this_pc, textvariable=self.monitors_var, width=46).grid(row=0, column=1, sticky="we", padx=6)
        tk.Button(this_pc, text="Detect", command=self.on_detect).grid(row=0, column=2, padx=2)
        tk.Button(this_pc, text="Auto (DRM)", command=lambda: self.apply_monitors("auto")).grid(row=0, column=3, padx=2)
        tk.Button(this_pc, text="Apply", command=lambda: self.apply_monitors(self.monitors_var.get().strip())).grid(
            row=0, column=4, padx=2)
        tk.Label(this_pc, text="Role:").grid(row=1, column=0, sticky="w", pady=(6, 0))
        self.role_var = tk.StringVar()
        roles = tk.Frame(this_pc)
        roles.grid(row=1, column=1, columnspan=3, sticky="w", pady=(6, 0))
        tk.Radiobutton(roles, text="Master (this mouse controls the others)", variable=self.role_var,
                       value="master").pack(side="left")
        tk.Radiobutton(roles, text="Slave", variable=self.role_var, value="slave").pack(side="left", padx=8)
        tk.Button(this_pc, text="Apply role", command=self.apply_role).grid(row=1, column=4, padx=2, pady=(6, 0))
        this_pc.columnconfigure(1, weight=1)

        self.canvas.bind("<ButtonPress-1>", self.on_press)
        self.canvas.bind("<B1-Motion>", self.on_motion)
        self.canvas.bind("<ButtonRelease-1>", self.on_release)
        self.canvas.bind("<Configure>", lambda _e: self.redraw())
        self.monitors_loaded = False
        self.poll()

    # ------------------------------------------------------------------ daemon

    def poll(self):
        reply = self.client.request("status")
        if reply is None:
            self.status = None
            self.header.config(text="Net Mouse service is not running")
            self.info.config(text=f"Cannot reach {SOCKET_PATH}.\nStart it with: sudo systemctl start net-mouse "
                                  "(or set the role: sudo net-mouse-ctl role master|slave)")
        else:
            try:
                self.status = json.loads(reply)
            except ValueError:
                self.status = None
            if self.status:
                self.update_texts()
                if not self.monitors_loaded:
                    self.monitors_var.set(self.status.get("monitor_spec", ""))
                    self.role_var.set(self.status.get("role", ""))
                    self.monitors_loaded = True
        if not self.drag:
            self.redraw()
        self.root.after(POLL_MS, self.poll)

    def send(self, line):
        reply = self.client.request(line)
        if reply is None:
            messagebox.showerror("Net Mouse", "The service is not reachable.")
            return False
        if reply.startswith("error"):
            message = reply[len("error"):].strip()
            if "permission denied" in message:
                message += "\n\nUse an administrator account, or run e.g.:\n  sudo net-mouse-ctl role master"
            messagebox.showerror("Net Mouse", message)
            return False
        if "restarting" in reply:
            self.client.close()
            self.monitors_loaded = False
        return True

    def apply_monitors(self, spec):
        if not spec:
            messagebox.showerror("Net Mouse", "Enter a layout such as 1920x1080+0+0,2560x1440+1920+0")
            return
        self.send("set-monitors " + spec)

    def apply_role(self):
        role = self.role_var.get()
        if not role or not self.status or role == self.status.get("role"):
            return
        if messagebox.askokcancel("Net Mouse", f"Switch this PC to {role}? The service restarts."):
            self.send("set-role " + role)

    def on_detect(self):
        spec = detect_monitors(self.root)
        if spec:
            self.monitors_var.set(spec)
        else:
            messagebox.showinfo("Net Mouse", "Could not detect the monitors; enter them manually.")

    # ------------------------------------------------------------------ texts

    def update_texts(self):
        s = self.status
        role = s.get("role", "?")
        self.header.config(text=f"{role.upper()} • {s.get('name', '')} • id {s.get('id', '')}")
        lines = []
        if role == "master":
            slaves = s.get("slaves", [])
            if s.get("remote"):
                active = next((v["name"] for v in slaves if v["id"] == s.get("active")), "?")
                lines.append(f"Pointer is on slave \"{active}\"  (Ctrl+Alt+Home brings it back)")
            else:
                lines.append("Pointer is on this PC")
            if not slaves:
                lines.append("No slaves found yet. Install Net Mouse on another PC as slave.")
            for v in slaves:
                state = "online" + ("" if v["synced"] else ", syncing") if v["online"] else "offline"
                lines.append(f"  • {v['name']}  {v['ip']}  {state}  origin "
                             f"({v['offset']['x']}, {v['offset']['y']})")
            lines.append("Drag the green slave screens to where they physically are; "
                         "the pointer crosses where screens touch.")
        else:
            m = s.get("master", {})
            if not s.get("configured"):
                lines.append(f"Master \"{m.get('name')}\" found, waiting for its configuration…"
                             if m.get("online") else "Looking for a master on the local network…")
            else:
                lines.append(f"Master \"{m.get('name') or m.get('id')}\" "
                             f"{'online' if m.get('online') else 'offline'} • this desktop's origin is at "
                             f"({s['offset']['x']}, {s['offset']['y']}) in the master layout")
            lines.append("The master's pointer is on this PC" if s.get("active") else "Idle")
            lines.append("Arrange this PC's position in the master's Net Mouse window.")
        self.info.config(text="\n".join(lines))

    # ------------------------------------------------------------------ canvas

    def screens(self):
        """[(rect(x, y, w, h), kind, slave or None, label)] in layout coordinates."""
        s = self.status or {}
        out = []
        for i, m in enumerate(s.get("monitors", [])):
            title = "MASTER" if s.get("role") == "master" else "THIS PC"
            label = f"{title} {i + 1}{' (primary)' if m['primary'] else ''}\n{m['w']}×{m['h']}"
            out.append(((m["x"], m["y"], m["w"], m["h"]), "master", None, label))
        for v in s.get("slaves", []):
            if not v["online"] and not (self.drag and self.drag["id"] == v["id"]):
                continue
            ox, oy = self.offset_of(v)
            for k, r in enumerate(v["monitors"]):
                label = v["name"] + (f" #{k + 1}" if len(v["monitors"]) > 1 else "")
                label += f"\n{r['w']}×{r['h']}" + ("" if v["synced"] else "\n(syncing…)")
                out.append(((r["x"] + ox, r["y"] + oy, r["w"], r["h"]), "slave", v, label))
        return out

    def offset_of(self, v):
        if self.drag and self.drag["id"] == v["id"]:
            return self.drag["offset"]
        return v["offset"]["x"], v["offset"]["y"]

    def compute_transform(self, rects):
        cw, ch = max(self.canvas.winfo_width(), 1), max(self.canvas.winfo_height(), 1)
        if not rects:
            self.scale, self.origin = 0.1, (0.0, 0.0)
            return
        left = min(r[0] for r in rects)
        top = min(r[1] for r in rects)
        right = max(r[0] + r[2] for r in rects)
        bottom = max(r[1] + r[3] for r in rects)
        pad = max(right - left, bottom - top) * 0.35  # room to drag slaves to any side
        self.scale = min(cw / (right - left + 2 * pad), ch / (bottom - top + 2 * pad))
        self.origin = ((left + right) / 2 - cw / 2 / self.scale, (top + bottom) / 2 - ch / 2 / self.scale)

    def to_canvas(self, x, y):
        return (x - self.origin[0]) * self.scale, (y - self.origin[1]) * self.scale

    def redraw(self):
        c = self.canvas
        c.delete("all")
        screens = self.screens()
        if not self.drag:
            self.compute_transform([r for r, _, _, _ in screens])
        s = self.status or {}
        for (x, y, w, h), kind, v, label in screens:
            x0, y0 = self.to_canvas(x, y)
            x1, y1 = self.to_canvas(x + w, y + h)
            active = v is not None and s.get("remote") and s.get("active") == v["id"]
            if kind == "master":
                fill = MASTER_FILL
            else:
                fill = SLAVE_DRAG_FILL if self.drag and self.drag["id"] == v["id"] else SLAVE_ONLINE_FILL
            c.create_rectangle(x0, y0, x1, y1, fill=fill, outline=ACTIVE if active else BORDER,
                               width=4 if active else 2)
            c.create_text((x0 + x1) / 2, (y0 + y1) / 2, text=label, fill="white", justify="center",
                          width=max(x1 - x0 - 8, 10))
        if s.get("role") == "master" and s.get("remote"):
            px, py = self.to_canvas(s["pointer"]["x"], s["pointer"]["y"])
            c.create_oval(px - 5, py - 5, px + 5, py + 5, fill=ACTIVE, outline=BORDER)
        if s.get("role") == "master" and not s.get("slaves"):
            c.create_text(c.winfo_width() / 2, c.winfo_height() - 20, fill="#5a5a5a",
                          text="Waiting for slaves… Install Net Mouse on another PC and choose \"slave\".")

    def on_press(self, event):
        if not self.status or self.status.get("role") != "master":
            return
        for v in reversed(self.status.get("slaves", [])):
            if not v["online"]:
                continue
            ox, oy = v["offset"]["x"], v["offset"]["y"]
            for r in v["monitors"]:
                x0, y0 = self.to_canvas(r["x"] + ox, r["y"] + oy)
                x1, y1 = self.to_canvas(r["x"] + ox + r["w"], r["y"] + oy + r["h"])
                if x0 <= event.x <= x1 and y0 <= event.y <= y1:
                    self.drag = {"id": v["id"], "start": (event.x, event.y), "start_offset": (ox, oy),
                                 "offset": (ox, oy), "slave": v}
                    return

    def on_motion(self, event):
        if not self.drag:
            return
        sx, sy = self.drag["start_offset"]
        raw = (sx + round((event.x - self.drag["start"][0]) / self.scale),
               sy + round((event.y - self.drag["start"][1]) / self.scale))
        self.drag["offset"] = self.snap(self.drag["slave"], raw)
        self.redraw()

    def on_release(self, event):
        if not self.drag:
            return
        self.on_motion(event)
        drag, self.drag = self.drag, None
        if drag["offset"] != drag["start_offset"]:
            x, y = drag["offset"]
            if not self.send(f"set-offset {drag['id']} {x} {y}"):
                self.root.bell()
        self.redraw()

    def snap(self, moving, offset):
        """Line up the dragged slave's edges with nearby edges of other screens."""
        s = self.status
        others = [(m["x"], m["y"], m["x"] + m["w"], m["y"] + m["h"]) for m in s.get("monitors", [])]
        for v in s.get("slaves", []):
            if v["id"] == moving["id"] or not v["online"]:
                continue
            ox, oy = v["offset"]["x"], v["offset"]["y"]
            others += [(r["x"] + ox, r["y"] + oy, r["x"] + ox + r["w"], r["y"] + oy + r["h"]) for r in v["monitors"]]
        threshold = SNAP_PX / self.scale
        best_x, best_y = (threshold + 1, 0), (threshold + 1, 0)
        for r in moving["monitors"]:
            l, t = r["x"] + offset[0], r["y"] + offset[1]
            rr, b = l + r["w"], t + r["h"]
            for ol, ot, orr, ob in others:
                for d in (orr - l, ol - rr, ol - l, orr - rr):
                    if abs(d) < best_x[0]:
                        best_x = (abs(d), d)
                for d in (ob - t, ot - b, ot - t, ob - b):
                    if abs(d) < best_y[0]:
                        best_y = (abs(d), d)
        x = offset[0] + (best_x[1] if best_x[0] <= threshold else 0)
        y = offset[1] + (best_y[1] if best_y[0] <= threshold else 0)
        return x, y


def main():
    if len(sys.argv) > 1 and sys.argv[1] in ("-h", "--help"):
        print(__doc__)
        return 0
    root = tk.Tk()
    App(root)
    root.mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
