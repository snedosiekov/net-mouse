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
from tkinter import font as tkfont
from tkinter import messagebox

SOCKET_PATH = os.environ.get("NETMOUSE_CONTROL", "/run/net-mouse/control.sock")
POLL_MS = 400
SNAP_PX = 12

MASTER_COLORS = ("#447fdb", "#3a71cc")  # (upper, lower) half of a screen: a subtle sheen
SLAVE_COLORS = ("#33a873", "#2b9866")
SLAVE_DRAG_COLORS = ("#62cf97", "#55c18a")
CONNECTING_COLORS = ("#9aa3b1", "#8c95a3")
AVAILABLE_COLORS = ("#8492a8", "#768399")
WARNING_COLORS = ("#e2605a", "#cf4d47")
STRIP_HEIGHT = 104


def in_layout(v):
    """Slaves shown in the arrangement: connected (or being connected) with a trusted key."""
    return v.get("paired") and not v.get("key_mismatch")


def is_available(v):
    """Slaves shown as cards in the 'Available' strip."""
    return v["online"] and not in_layout(v)
BEZEL = "#22262e"
SHADOW = "#c9cfd9"
GRID_DOT = "#d6dbe4"
ACTIVE = "#ff8c00"
CANVAS_BG = "#eef1f6"


def round_rect(canvas, x0, y0, x1, y1, r, **kw):
    """Rounded rectangle as a smoothed polygon."""
    points = [x0 + r, y0, x1 - r, y0, x1, y0, x1, y0 + r, x1, y1 - r, x1, y1, x1 - r, y1, x0 + r, y1,
              x0, y1, x0, y1 - r, x0, y0 + r, x0, y0]
    return canvas.create_polygon(points, smooth=True, **kw)


def elide(text, font, width):
    """Shortens text with an ellipsis until it fits into width pixels."""
    if font.measure(text) <= width:
        return text
    while text and font.measure(text + "…") > width:
        text = text[:-1]
    return text + "…" if text else ""


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
        self.title_font = tkfont.nametofont("TkDefaultFont").copy()
        self.title_font.configure(weight="bold", size=self.title_font.cget("size") + 1)
        self.body_font = tkfont.nametofont("TkDefaultFont").copy()
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
        self.forget_button = tk.Button(this_pc, text="Forget master", command=self.forget_master)
        self.forget_button.grid(row=1, column=3, padx=2, pady=(6, 0))
        this_pc.columnconfigure(1, weight=1)

        self.canvas.bind("<ButtonPress-1>", self.on_press)
        self.canvas.bind("<B1-Motion>", self.on_motion)
        self.canvas.bind("<ButtonRelease-1>", self.on_release)
        self.canvas.bind("<ButtonPress-3>", self.on_right_click)
        self.cards = []  # (x0, y0, x1, y1, slave) of the Available strip
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

    def forget_master(self):
        if messagebox.askokcancel("Net Mouse", "Forget the master this PC is paired with?\n\nIt will no longer "
                                               "control this PC until a master connects it again."):
            self.send("forget-master")

    def connect_slave(self, v):
        if v.get("other_master"):
            messagebox.showinfo("Net Mouse", f"\"{v['name']}\" is paired with another master.\n\nOn that PC choose "
                                             "\"Forget master\" (or run: net-mouse-ctl forget-master), then connect it here.")
            return
        warning = ("\n\nWARNING: this PC now has a different key than the one connected before. Only continue if it "
                   "was reinstalled.") if v.get("key_mismatch") else ""
        if messagebox.askokcancel("Net Mouse", f"Connect \"{v['name']}\" ({v['ip']})?\n\nFingerprint: "
                                               f"{v['fingerprint']}\n\nCheck that Net Mouse on that PC shows the same "
                                               f"fingerprint. Once connected, this master controls it over an "
                                               f"encrypted channel.{warning}"):
            if v.get("key_mismatch"):
                self.send(f"unpair {v['id']}")
            self.send(f"pair {v['id']}")

    def on_right_click(self, event):
        v = self.slave_at(event.x, event.y)
        if v and messagebox.askokcancel("Net Mouse", f"Disconnect \"{v['name']}\"?\n\nThis master stops controlling "
                                                     "it; it goes back to Available."):
            self.send(f"unpair {v['id']}")

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
        self.header.config(text=f"{role.upper()} \u2022 {s.get('name', '')} \u2022 fingerprint {s.get('fingerprint', '')}")
        self.forget_button.config(state="normal" if role == "slave" and s.get("paired") else "disabled")
        lines = []
        if role == "master":
            slaves = s.get("slaves", [])
            if s.get("remote"):
                active = next((v["name"] for v in slaves if v["id"] == s.get("active")), "?")
                lines.append(f"Pointer and keyboard are on \"{active}\"  (Ctrl+Alt+Home brings them back)")
            else:
                lines.append("Pointer is on this PC")
            if not slaves:
                lines.append("No other PCs found yet. Install Net Mouse on another PC as slave.")
            for v in slaves:
                if not v["online"]:
                    state = "connected, offline" if v["paired"] else "offline"
                elif v["key_mismatch"]:
                    state = "KEY CHANGED \u2014 not trusted"
                elif not v["paired"]:
                    state = "paired with another master" if v["other_master"] else "available"
                elif not v["connected"]:
                    state = "connecting\u2026"
                else:
                    state = "connected (encrypted)" if v["synced"] else "connected, syncing"
                lines.append(f"  \u2022 {v['name']}  {v['ip']}  {state}  \u2022  fingerprint {v['fingerprint']}")
            lines.append("Click an available PC to connect it, drag connected screens to where they are; "
                         "right-click one to disconnect.")
        else:
            m = s.get("master", {})
            if not s.get("paired"):
                lines.append(f"Not connected. On the master \"{m.get('name')}\", click this PC (fingerprint "
                             f"{s.get('fingerprint')}) under Available." if m.get("online")
                             else "Not connected. Looking for a master on the local network\u2026")
            else:
                state = "offline" if not m.get("online") else (
                    "connected (encrypted)" if s.get("connected") else "connecting\u2026")
                lines.append(f"Paired with master \"{m.get('name')}\" (fingerprint {m.get('fingerprint')}) "
                             f"\u2022 {state}")
                if s.get("configured"):
                    lines.append(f"This desktop's origin is at ({s['offset']['x']}, {s['offset']['y']}) in the "
                                 "master layout")
            lines.append("The master's pointer and keyboard are on this PC" if s.get("active") else "Idle")
        self.info.config(text="\n".join(lines))

    # ------------------------------------------------------------------ canvas

    def screens(self):
        """[(rect(x, y, w, h), kind, slave or None, label)] in layout coordinates."""
        s = self.status or {}
        out = []
        for i, m in enumerate(s.get("monitors", [])):
            title = "MASTER" if s.get("role") == "master" else "THIS PC"
            label = f"{title} {i + 1}\n{m['w']} × {m['h']}" + ("\nprimary" if m["primary"] else "")
            out.append(((m["x"], m["y"], m["w"], m["h"]), "master", None, label))
        for v in s.get("slaves", []):
            if not in_layout(v) or (not v["online"] and not (self.drag and self.drag["id"] == v["id"])):
                continue
            ox, oy = self.offset_of(v)
            for k, r in enumerate(v["monitors"]):
                label = v["name"] + (f" #{k + 1}" if len(v["monitors"]) > 1 else "")
                label += f"\n{v['ip']}" if v.get("ip") else ""
                label += f"\n{r['w']} × {r['h']}"
                label += "\nconnecting\u2026" if not v["connected"] else ("" if v["synced"] else "\nsyncing\u2026")
                out.append(((r["x"] + ox, r["y"] + oy, r["w"], r["h"]), "slave", v, label))
        return out

    def offset_of(self, v):
        if self.drag and self.drag["id"] == v["id"]:
            return self.drag["offset"]
        return v["offset"]["x"], v["offset"]["y"]

    def compute_transform(self, rects, strip=0):
        cw, ch = max(self.canvas.winfo_width(), 1), max(self.canvas.winfo_height() - strip, 1)
        if not rects:
            self.scale, self.origin = 0.1, (0.0, 0.0)
            return
        left = min(r[0] for r in rects)
        top = min(r[1] for r in rects)
        right = max(r[0] + r[2] for r in rects)
        bottom = max(r[1] + r[3] for r in rects)
        margin = 36  # room for shadows and for dragging a slave past the outer edges
        self.scale = min((cw - 2 * margin) / (right - left), (ch - 2 * margin) / (bottom - top))
        self.scale = max(0.01, min(self.scale, 0.25))  # a single small screen should not fill the view
        self.origin = ((left + right) / 2 - cw / 2 / self.scale, (top + bottom) / 2 - ch / 2 / self.scale)

    def to_canvas(self, x, y):
        return (x - self.origin[0]) * self.scale, (y - self.origin[1]) * self.scale

    def redraw(self):
        c = self.canvas
        c.delete("all")
        screens = self.screens()
        s = self.status or {}
        available = [v for v in s.get("slaves", []) if is_available(v)] if s.get("role") == "master" else []
        strip = STRIP_HEIGHT if available else 0
        if not self.drag:
            self.compute_transform([r for r, _, _, _ in screens], strip)
        w_c, h_c = c.winfo_width(), c.winfo_height()
        for gy in range(11, h_c, 22):  # faint dot grid
            for gx in range(11, w_c, 22):
                c.create_oval(gx - 1, gy - 1, gx + 1, gy + 1, fill=GRID_DOT, outline="")
        for (x, y, w, h), kind, v, label in screens:
            x0, y0 = self.to_canvas(x, y)
            x1, y1 = self.to_canvas(x + w, y + h)
            active = v is not None and s.get("remote") and s.get("active") == v["id"]
            if kind == "master":
                colors = MASTER_COLORS
            elif self.drag and self.drag["id"] == v["id"]:
                colors = SLAVE_DRAG_COLORS
            else:
                colors = SLAVE_COLORS if v["connected"] else CONNECTING_COLORS
            self.draw_monitor(x0, y0, x1, y1, colors, active, label.split("\n"))
        self.cards = []
        if available:  # "Available" strip: one card per discovered PC that is not connected
            top = h_c - strip
            c.create_line(12, top, w_c - 12, top, fill="#c3cad6")
            c.create_text(16, top + 12, anchor="w", fill="#46505f", font=self.body_font,
                          text="Available \u2014 click a PC to connect it")
            x = 16
            for v in available:
                if x + 230 > w_c:
                    break
                lines = [v["name"], v["ip"], v["fingerprint"]]
                colors = AVAILABLE_COLORS
                if v["key_mismatch"]:
                    lines.append("key changed \u2014 click to trust it")
                    colors = WARNING_COLORS
                elif v["other_master"]:
                    lines.append("paired with another master")
                    colors = CONNECTING_COLORS
                self.draw_monitor(x, top + 28, x + 230, top + 94, colors, False, lines)
                self.cards.append((x, top + 28, x + 230, top + 94, v))
                x += 242
        if s.get("role") == "master" and s.get("remote"):
            px, py = self.to_canvas(s["pointer"]["x"], s["pointer"]["y"])
            c.create_oval(px - 8, py - 8, px + 8, py + 8, fill="white", outline="")
            c.create_oval(px - 6, py - 6, px + 6, py + 6, fill=ACTIVE, outline="")
        if s.get("role") == "master" and not s.get("slaves"):
            c.create_text(c.winfo_width() / 2, c.winfo_height() - 20, fill="#5a5a5a",
                          text="Waiting for slaves… Install Net Mouse on another PC and choose \"slave\".")

    def draw_monitor(self, x0, y0, x1, y1, colors, active, lines):
        """Shadow, dark bezel, two-tone 'screen' and text (name bold; lines that do not fit are dropped)."""
        c = self.canvas
        x0, y0, x1, y1 = x0 + 1.5, y0 + 1.5, x1 - 1.5, y1 - 1.5  # gap between touching monitors
        if x1 - x0 < 6 or y1 - y0 < 6:
            return
        r = max(3.0, min(10.0, min(x1 - x0, y1 - y0) * 0.08))
        round_rect(c, x0 + 1, y0 + 4, x1 + 2, y1 + 4, r, fill=SHADOW, outline="")
        if active:
            round_rect(c, x0 - 4, y0 - 4, x1 + 4, y1 + 4, r + 3, fill="", outline=ACTIVE, width=3)
        round_rect(c, x0, y0, x1, y1, r, fill=BEZEL, outline="")
        b = 3
        light, dark = colors
        round_rect(c, x0 + b, y0 + b, x1 - b, y1 - b, max(1.0, r - 2), fill=dark, outline="")
        round_rect(c, x0 + b, y0 + b, x1 - b, y0 + b + (y1 - y0 - 2 * b) * 0.5, max(1.0, r - 2), fill=light,
                   outline="")
        c.create_rectangle(x0 + b, y0 + b + (y1 - y0 - 2 * b) * 0.25, x1 - b, y0 + b + (y1 - y0 - 2 * b) * 0.5,
                           fill=light, outline="")
        width = x1 - x0 - 2 * b - 6
        heights = [self.title_font.metrics("linespace")] + [self.body_font.metrics("linespace")] * (len(lines) - 1)
        count, needed = 0, 0
        for h in heights:
            if needed + h > y1 - y0 - 2 * b and count:
                break
            needed, count = needed + h, count + 1
        y = (y0 + y1) / 2 - needed / 2
        for i in range(count):
            font = self.title_font if i == 0 else self.body_font
            text = elide(lines[i], font, width)
            c.create_text((x0 + x1) / 2, y + heights[i] / 2, text=text, font=font,
                          fill="white" if i == 0 else "#e6f0f5")
            y += heights[i]

    def slave_at(self, x, y):
        """The connected slave whose screen is at canvas position (x, y)."""
        if not self.status or self.status.get("role") != "master":
            return None
        for v in reversed(self.status.get("slaves", [])):
            if not v["online"] or not in_layout(v):
                continue
            ox, oy = v["offset"]["x"], v["offset"]["y"]
            for r in v["monitors"]:
                x0, y0 = self.to_canvas(r["x"] + ox, r["y"] + oy)
                x1, y1 = self.to_canvas(r["x"] + ox + r["w"], r["y"] + oy + r["h"])
                if x0 <= x <= x1 and y0 <= y <= y1:
                    return v
        return None

    def on_press(self, event):
        if not self.status or self.status.get("role") != "master":
            return
        for x0, y0, x1, y1, v in self.cards:
            if x0 <= event.x <= x1 and y0 <= event.y <= y1:
                self.connect_slave(v)
                return
        for v in reversed(self.status.get("slaves", [])):
            if not v["online"] or not in_layout(v):
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
            if v["id"] == moving["id"] or not v["online"] or not in_layout(v):
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
