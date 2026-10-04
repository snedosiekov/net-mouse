# net-mouse

Use the mouse of one PC (the **master**) to control that PC and any number of other PCs (**slaves**) on
the same local network. Slave screens are arranged around the master's monitors, the same way as in the
Windows display settings. When the pointer leaves the master's monitors through an edge that touches a
slave screen, it continues on that slave.

Windows 10/11 is fully supported, with a tray app, an arrangement editor and an installer. Linux is a
**prototype**: a systemd service for both roles, working below the display server (see [Linux](#linux-prototype)).

## Build

Requirements: Visual Studio 2022 (C++ workload) and CMake 3.20 or newer.

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
build\Release\nm_tests.exe
```

The result is a single `build\Release\net-mouse.exe` (static runtime). Copy it to every PC.

### Installer

With [Inno Setup 6](https://jrsoftware.org/isinfo.php) installed:

```
cmake --build build --config Release --target installer
```

This produces `build\installer\net-mouse-setup-<version>.exe` from [installer/net-mouse.iss](installer/net-mouse.iss).
The setup:

* installs to `Program Files\Net Mouse` (requires administrator rights);
* asks for the role of the PC: master, slave, or ask on first start. The role becomes
  `--default-role`, so a role changed later from the tray menu is kept;
* adds a Windows Firewall rule for the program;
* optionally registers the scheduled task **Net Mouse**. It starts the app at logon for any user, with
  highest privileges, so a slave can also control administrator windows;
* on upgrade or uninstall asks running instances to quit (`net-mouse.exe --quit`, which restores the
  cursor), then removes the task and the firewall rule. Per-user settings in `%APPDATA%\NetMouse` are kept.

Unattended install, e.g. for a slave deployed over SSH:

```
net-mouse-setup-0.1.0.exe /VERYSILENT /SUPPRESSMSGBOXES /ROLE=slave /TASKS=autostart
```

## Usage

1. Start `net-mouse.exe` on every PC. On first start choose the role: **Master** on the PC whose mouse
   you use, **Slave** on the others. The choice is remembered. Change it later from the tray menu
   (*Change role…*).
2. Each slave announces itself on the network. The master detects it, places it to the right of
   its screens, and sends the slave its configuration.
3. On the master, open *Arrange displays…* from the tray icon. Drag the green slave screens to where
   the PCs physically stand. Edges snap together, and the pointer crosses only where screens touch.
4. Net Mouse runs minimized in the notification area (system tray). The mouse icon is blue when idle
   and turns orange while the pointer is on a slave (on a slave: while it is being controlled). Its
   tooltip shows the current state. Click the icon to open the window. Minimizing or closing the window
   hides it to the tray again, and starting the app a second time opens the running instance. On
   Windows 11 new tray icons go to the overflow area; drag the icon onto the taskbar to keep it visible.
5. Move the pointer over the edge. While it is on a slave, the master's cursor is hidden and the
   master's clicks and wheel are sent to the slave.

**Emergency return:** **Ctrl+Alt+Home** or the tray item *Return pointer to this PC* brings the pointer
back to the master at once. A slave that disappears from the network also returns the pointer
automatically.

On first start, Windows Firewall asks whether to allow network access. Allow it on **private**
networks. To allow it ahead of time:

```
netsh advfirewall firewall add rule name="net-mouse" dir=in action=allow protocol=UDP localport=47800
```

### Command line

| Option | Meaning |
|---|---|
| `--config <file>` | settings file (default `%APPDATA%\NetMouse\netmouse.ini`) |
| `--role master\|slave` | set the role without the dialog |
| `--name <name>` | name shown to other PCs (default: computer name) |
| `--default-role master\|slave` | role to use if none has been chosen yet (set by the installer) |
| `--dry-run` | slave only: log received input instead of moving the cursor |
| `--quit` | ask running instances in this session to exit cleanly |
| `--show` | open the window at start (by default the app starts minimized to the tray) |

Testing on a single PC: run a master and a `--dry-run` slave side by side, each with its own `--config`.

### Settings (`netmouse.ini`)

| Key | Default | |
|---|---|---|
| `role` | — | `master` or `slave` |
| `id` | random | persistent identity of this PC |
| `name` | computer name | |
| `group`, `port` | `239.255.77.77`, `47800` | multicast group and UDP port (must match on all PCs) |
| `interface` | empty | local IPv4 address to pin traffic to; empty = all interfaces |
| `hide_cursor` | `1` | master: hide the local cursor while the pointer is on a slave |
| `slave.<id>.x/.y` | | master: saved position of each slave |
| `master_id`, `offset_x/_y`, `config_version` | | slave: last configuration received |

Logs: `netmouse-master.log` / `netmouse-slave.log` next to the settings file.

## How it works

```
 MASTER                                                SLAVE
 WH_MOUSE_LL hook ──► MasterEngine ──UDP multicast──► SlaveEngine ──► SendInput
 (swallows input       (layout space,                 (layout -> local coords,
  while remote)          edge crossing)                 button reconciliation)
```

* **Layout space** = the master's virtual-desktop coordinates in physical pixels. Every slave's monitors
  are placed in it at an *offset* chosen in the arrangement dialog. The slave receives that offset and
  converts with `local = layout_position − offset`.
* **Capture (master):** a low-level mouse hook watches the cursor. When it touches an edge pixel and
  the neighbouring layout pixel belongs to a slave, the master switches to *remote* mode. The real cursor
  is parked at the centre of the primary monitor and hidden. Each hooked move is converted into a
  delta from the park point and applied to a virtual pointer. All mouse events are swallowed so the
  master doesn't react to them. When the virtual pointer moves back onto a master monitor, the real
  cursor is placed there and normal operation continues.
* **Injection (slave):** `SendInput` with absolute virtual-desktop coordinates, button and wheel events.
* Edge crossing is blocked while a button is held, so a drag never starts on one PC and ends on another.

No kernel driver is needed for this. User-mode hooks and `SendInput` cover normal desktop use (see
[Limitations](#limitations) for the cases where a driver or service would help).

### Protocol

UDP multicast `239.255.77.77:47800`, TTL 1. Every datagram starts with a header: magic `NMSE`,
version, type, 64-bit sender id and a sequence number. All integers are little-endian. See
[src/core/protocol.h](src/core/protocol.h).

| Message | Direction | When |
|---|---|---|
| `MasterAnnounce` | master → all | every second |
| `SlaveHello` (name, monitors, applied master id + config version) | slave → all | every second, and as an acknowledgement |
| `SlaveConfig` (target, version, offset) | master → slave | until the slave's hello acknowledges that version |
| `Pointer` (target, layout position, button mask, wheel) | master → slave | every mouse event, plus a 100 ms heartbeat |
| `Leave` (target) | master → slave | pointer left that slave |

Loss handling: each `Pointer` carries the *complete* button state, so a lost press or release is
repaired by the next packet. Button and wheel packets are sent twice and the slave drops
duplicates by sequence number. A slave releases all buttons if the pointer stream stops for 1.5 s.
A master treats a slave as gone after 3.5 s without a hello.

## Linux (prototype)

`net-mouse-daemon` is one service for both roles. It works at the kernel input layer, so it behaves the
same on X11, any Wayland compositor (GNOME, KDE, sway, Hyprland, ...) and the text console:

* **Slave:** creates a *uinput* virtual **absolute** pointer, like a VM's USB tablet. The display server
  maps its range onto the whole desktop, so the master's position lands exactly where it should.
* **Master:** grabs the physical mice exclusively through *evdev* (`EVIOCGRAB`) and owns the pointer
  position. Local movement is re-injected through its own virtual pointer. When the pointer crosses an
  edge that touches a slave, the movement goes to the slave instead and the real cursor waits at the
  edge. Keyboards are read without grabbing, only for the **Ctrl+Alt+Home** emergency hotkey. If the
  daemon stops, the kernel releases the grab and the mice work normally again.
* The **monitor layout** comes from `monitors=` in the settings (recommended), or is detected from the
  DRM connectors in `/sys/class/drm` (preferred mode, placed left to right).

Build and install (needs `cmake` 3.20+ and `g++` 10+; no other dependencies):

```
sudo packaging/linux/install.sh slave                       # or: master
sudo packaging/linux/install.sh slave --monitors 2560x1440+0+0,1920x1080+2560+0
journalctl -u net-mouse -f                                  # logs
sudo net-mouse-daemon --list-devices                        # detected monitors and mice
sudo packaging/linux/install.sh --uninstall
```

The script builds into `build-linux/`, installs `/usr/local/bin/net-mouse-daemon`, writes
`/etc/net-mouse/netmouse.ini`, loads `uinput`, opens UDP 47800 in ufw/firewalld and enables
`net-mouse.service`.

Settings (`/etc/net-mouse/netmouse.ini`), in addition to the common ones:

| Key | Default | |
|---|---|---|
| `monitors` | detected | layout as `WxH+X+Y,...`; first entry is the primary monitor. Use the compositor's *logical* size when display scaling is on (a 3840×2160 screen at 200 % is `1920x1080+0+0`) |
| `pointer_speed` | `1.0` | master: speed multiplier for local movement |
| `pointer_accel` | `0.0` | master: simple speed-dependent acceleration (0 = flat) |

There is no tray UI on Linux yet. A Linux master places new slaves to its right; to move one, edit
`slave.<id>.x/.y` in the settings and restart the service.

Testing without hardware: `sudo python3 tests/linux_e2e.py build-linux/net-mouse-daemon` creates a
fake mouse with uinput, runs a master and a slave on a private port, and checks edge crossing,
clicks, wheel and the return to the master.

Prototype limitations:

* **Master:** touchpads are not grabbed. On a laptop master, using the touchpad moves the real cursor
  behind the daemon's back. Use a mouse, or run the laptop as a slave.
* **Master:** pointer acceleration is the daemon's own, not the desktop's setting. Tune it with
  `pointer_speed` / `pointer_accel`.
* **Multi-monitor slaves with mixed scaling:** how the compositor maps the absolute device can differ
  between compositors. Check that the pointer lands correctly at the edges.
* Displays are read at start-up; restart the service after changing the monitor layout.

## Source layout

| Path | |
|---|---|
| `src/core/` | portable: geometry, protocol, layout math, config, log, UDP multicast (Winsock + BSD sockets) |
| `src/app/` | portable role engines: `MasterEngine`, `SlaveEngine` |
| `src/platform/platform.h` | OS interfaces: displays, cursor, injector, mouse sink |
| `src/platform/win/` | Windows: monitor enumeration, LL hook, `SendInput`, cursor hiding |
| `src/ui/win/` | Windows UI: role dialog, tray icon, status window, display-arrangement editor |
| `src/platform/linux/` | Linux: uinput virtual pointer, evdev capture, DRM/config monitor layout |
| `src/linux/` | Linux service entry point (`net-mouse-daemon`) |
| `installer/`, `packaging/linux/` | Inno Setup script; systemd unit and install script |
| `tests/` | protocol/layout/config tests, a full master↔slave session over fake platform objects, Linux end-to-end test |

## Limitations

* **Mouse only.** Keyboard input stays on the master; keyboard sharing is the obvious next step.
* **Elevated windows on a slave:** Windows blocks `SendInput` into windows of a higher integrity
  level (UIPI). Run the slave as administrator to control admin windows.
* **Secure desktop** (UAC prompts, Ctrl+Alt+Del, the lock and logon screens) can't be controlled from
  a user-session app. That needs a Windows service running in the `SYSTEM` session (or a filter
  driver such as Interception), which is a candidate for a later iteration.
* Pointer speed is 1:1 in physical pixels. Screens with very different DPI feel different in size.
* Several masters on one network see each other (a warning is logged). A slave follows the most recent
  master that configured it.

## Roadmap

* **Linux, next steps:** a session helper (tray icon, arrangement editor) that also reports the exact
  monitor layout from Wayland (`wl_output` / `xdg-output`) or X11 (XRandR) to the service over D-Bus;
  touchpad support on the master; .deb/.rpm packages.
* Keyboard sharing, clipboard sharing, optional encryption and authentication of the protocol.
