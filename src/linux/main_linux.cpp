// net-mouse-daemon: Linux service for both roles. Runs below the display server (evdev + uinput),
// so it works on X11, Wayland and the console alike. Needs access to /dev/uinput and
// /dev/input/event* (it runs as root under systemd).
//
//   net-mouse-daemon [--config FILE] [--role master|slave] [--name NAME] [--monitors SPEC]
//                    [--control SOCKET] [--dry-run]
//   net-mouse-daemon --list-devices
//   net-mouse-daemon --query "status"        (client: talk to the running daemon)

#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <thread>

#include "app/master_engine.h"
#include "app/network_loop.h"
#include "app/slave_engine.h"
#include "core/config.h"
#include "core/log.h"
#include "core/udp.h"
#include "linux/control.h"
#include "platform/linux/linux_platform.h"
#include "platform/linux/uinput_device.h"

using namespace nm;

namespace {

constexpr char kDefaultConfig[] = "/etc/net-mouse/netmouse.ini";
constexpr char kDefaultGroup[] = "239.255.77.77";
constexpr int kDefaultPort = 47800;

struct Args {
  std::string config = kDefaultConfig;
  std::string control = kDefaultControlSocket;
  std::string role;
  std::string name;
  std::string monitors;
  std::string query;
  bool dryRun = false;
  bool listDevices = false;
};

void usage() {
  std::printf(
      "Usage: net-mouse-daemon [options]\n"
      "  --config FILE      settings file (default %s)\n"
      "  --role ROLE        master or slave (overrides role= in the settings)\n"
      "  --name NAME        name shown to other PCs (default: host name)\n"
      "  --monitors SPEC    monitor layout, e.g. 1920x1080+0+0,2560x1440+1920+0\n"
      "                     (default: monitors= in the settings, else detected from DRM)\n"
      "  --control SOCKET   control socket (default %s)\n"
      "  --dry-run          slave: log received input instead of injecting it\n"
      "  --list-devices     show detected input devices and monitors, then exit\n"
      "  --query REQUEST    send REQUEST (e.g. status) to the running daemon and print the reply\n",
      kDefaultConfig, kDefaultControlSocket);
}

bool parseArgs(int argc, char** argv, Args& a) {
  for (int i = 1; i < argc; ++i) {
    const std::string s = argv[i];
    const bool hasValue = i + 1 < argc;
    if (s == "--config" && hasValue) {
      a.config = argv[++i];
    } else if (s == "--control" && hasValue) {
      a.control = argv[++i];
    } else if (s == "--role" && hasValue) {
      a.role = argv[++i];
    } else if (s == "--name" && hasValue) {
      a.name = argv[++i];
    } else if (s == "--monitors" && hasValue) {
      a.monitors = argv[++i];
    } else if (s == "--query" && hasValue) {
      a.query = argv[++i];
    } else if (s == "--dry-run") {
      a.dryRun = true;
    } else if (s == "--list-devices") {
      a.listDevices = true;
    } else {
      usage();
      return false;
    }
  }
  return true;
}

uint64_t randomId() {
  std::random_device rd;
  uint64_t id = 0;
  while (id == 0) id = (static_cast<uint64_t>(rd()) << 32) | rd();
  return id;
}

std::string hostName() {
  char buf[256] = {};
  return gethostname(buf, sizeof(buf) - 1) == 0 && buf[0] ? buf : "linux";
}

int listDevices(LinuxDisplays& displays) {
  std::printf("Monitors:\n");
  for (const MonitorInfo& m : displays.monitors()) {
    std::printf("  %-12s %dx%d at (%d, %d)%s\n", m.name.c_str(), m.rect.width(), m.rect.height(), m.rect.left,
                m.rect.top, m.primary ? " primary" : "");
  }
  std::printf("Input devices:\n");
  for (const InputDeviceInfo& d : listInputDevices()) {
    std::printf("  %-20s %-10s %s\n", d.path.c_str(), d.mouse ? "mouse" : (d.keyboard ? "keyboard" : "-"),
                d.name.c_str());
  }
  return 0;
}

// ---------------------------------------------------------------------------------------------
// Control requests

std::string pointJson(Point p) { return log::fmt("{\"x\":%d,\"y\":%d}", p.x, p.y); }

std::string rectJson(const Rect& r) {
  return log::fmt("{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d}", r.left, r.top, r.width(), r.height());
}

std::string monitorsJson(const std::vector<MonitorInfo>& monitors) {
  std::string out = "[";
  for (size_t i = 0; i < monitors.size(); ++i) {
    const MonitorInfo& m = monitors[i];
    if (i) out += ",";
    out += log::fmt("{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"primary\":%s,\"name\":\"%s\"}", m.rect.left, m.rect.top,
                    m.rect.width(), m.rect.height(), m.primary ? "true" : "false", jsonEscape(m.name).c_str());
  }
  return out + "]";
}

struct DaemonState {
  std::string role;
  std::string name;
  uint64_t id = 0;
  std::string configPath;
  std::string monitorSpec;
  MasterEngine* master = nullptr;
  SlaveEngine* slave = nullptr;
};

std::string statusJson(const DaemonState& st) {
  std::string out = log::fmt("{\"role\":\"%s\",\"name\":\"%s\",\"id\":\"%s\",\"config\":\"%s\",\"monitor_spec\":\"%s\",",
                             st.role.c_str(), jsonEscape(st.name).c_str(), toHex(st.id).c_str(),
                             jsonEscape(st.configPath).c_str(), jsonEscape(st.monitorSpec).c_str());
  if (st.master) {
    const MasterSnapshot s = st.master->snapshot();
    out += log::fmt("\"remote\":%s,\"active\":\"%s\",\"pointer\":%s,\"monitors\":%s,\"slaves\":[",
                    s.remote ? "true" : "false", s.remote ? toHex(s.activeSlave).c_str() : "",
                    pointJson(s.virtualPos).c_str(), monitorsJson(s.masterMonitors).c_str());
    for (size_t i = 0; i < s.slaves.size(); ++i) {
      const SlaveView& v = s.slaves[i];
      if (i) out += ",";
      out += log::fmt("{\"id\":\"%s\",\"name\":\"%s\",\"ip\":\"%s\",\"online\":%s,\"synced\":%s,\"offset\":%s,\"monitors\":[",
                      toHex(v.id).c_str(), jsonEscape(v.name).c_str(), ipToString(v.ip).c_str(),
                      v.online ? "true" : "false", v.synced ? "true" : "false", pointJson(v.offset).c_str());
      for (size_t k = 0; k < v.monitors.size(); ++k) out += (k ? "," : "") + rectJson(v.monitors[k]);
      out += "]}";
    }
    out += "]}";
  } else {
    const SlaveSnapshot s = st.slave->snapshot();
    out += log::fmt(
        "\"configured\":%s,\"active\":%s,\"offset\":%s,\"monitors\":%s,"
        "\"master\":{\"id\":\"%s\",\"name\":\"%s\",\"ip\":\"%s\",\"online\":%s}}",
        s.configured ? "true" : "false", s.active ? "true" : "false", pointJson(s.offset).c_str(),
        monitorsJson(s.monitors).c_str(), toHex(s.masterId).c_str(), jsonEscape(s.masterName).c_str(),
        s.masterIp ? ipToString(s.masterIp).c_str() : "", s.masterOnline ? "true" : "false");
  }
  return out;
}

// Role and monitor changes are written to the settings and applied by restarting: the daemon exits
// and systemd (Restart=always) starts it again with the new settings.
void requestRestart() {
  std::thread([] {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    kill(getpid(), SIGUSR1);
  }).detach();
}

std::string saveSetting(const std::string& configPath, const std::string& key, const std::string& value) {
  Config fresh;
  fresh.load(configPath);
  if (value.empty()) {
    fresh.erase(key);
  } else {
    fresh.set(key, value);
  }
  if (!fresh.save()) return "error cannot write " + configPath;
  requestRestart();
  return "ok restarting";
}

std::string handleRequest(const DaemonState& st, const std::string& request, bool privileged) {
  std::istringstream in(request);
  std::string cmd;
  in >> cmd;
  if (cmd == "status") return statusJson(st);
  if (cmd == "set-offset" || cmd == "set-role" || cmd == "set-monitors") {
    if (!privileged) return "error permission denied: needs root or membership in sudo/wheel/admin/netmouse";
  }
  if (cmd == "set-offset") {
    std::string id;
    int x = 0, y = 0;
    if (!st.master) return "error only a master arranges slaves";
    if (!(in >> id >> x >> y)) return "error usage: set-offset SLAVE_ID X Y";
    uint64_t slaveId = 0;
    try {
      slaveId = std::stoull(id, nullptr, 16);
    } catch (...) {
      return "error bad slave id";
    }
    return st.master->setSlaveOffset(slaveId, {x, y}) ? "ok" : "error that position overlaps another screen";
  }
  if (cmd == "set-role") {
    std::string role;
    in >> role;
    if (role != "master" && role != "slave") return "error usage: set-role master|slave";
    log::info("Role changed to " + role + " via the control socket");
    return saveSetting(st.configPath, "role", role);
  }
  if (cmd == "set-monitors") {
    std::string spec;
    in >> spec;
    if (spec == "auto") spec.clear();
    if (!spec.empty() && parseMonitorSpec(spec).empty()) return "error bad monitor layout (expected WxH+X+Y,...)";
    log::info("Monitor layout changed to '" + (spec.empty() ? std::string("auto") : spec) + "' via the control socket");
    return saveSetting(st.configPath, "monitors", spec);
  }
  return "error unknown request '" + cmd + "' (status, set-offset, set-role, set-monitors)";
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!parseArgs(argc, argv, args)) return 2;

  if (!args.query.empty()) {
    std::string error;
    const std::string reply = controlRequest(args.control, args.query, error);
    if (reply.empty()) {
      std::fprintf(stderr, "%s\n", error.c_str());
      return 1;
    }
    std::printf("%s\n", reply.c_str());
    return reply.rfind("error", 0) == 0 ? 1 : 0;
  }

  log::setEcho(true);
  Config config;
  config.load(args.config);
  const std::string monitorSpec = !args.monitors.empty() ? args.monitors : config.get("monitors");
  LinuxDisplays displays(monitorSpec);
  if (args.listDevices) return listDevices(displays);

  const std::string role = !args.role.empty() ? args.role : config.get("role");
  if (role != "master" && role != "slave") {
    std::fprintf(stderr, "No role configured: run 'sudo net-mouse-ctl role master|slave' (or set role= in %s).\n",
                 args.config.c_str());
    return 2;
  }
  uint64_t id = config.getHex("id");
  if (id == 0) {
    id = randomId();
    config.setHex("id", id);
  }
  if (!args.name.empty()) config.set("name", args.name);
  const std::string name = config.get("name", hostName());
  const std::string group = config.get("group", kDefaultGroup);
  const int port = static_cast<int>(config.getInt("port", kDefaultPort));
  config.set("role", role);
  if (!config.has("group")) config.set("group", group);
  if (!config.has("port")) config.setInt("port", port);
  if (!config.save()) log::info("Warning: cannot write " + args.config + "; settings will not persist");

  log::info(log::fmt("Starting as %s '%s' (id %s), config %s", role.c_str(), name.c_str(), toHex(id).c_str(),
                     args.config.c_str()));
  for (const MonitorInfo& m : displays.monitors()) {
    log::info(log::fmt("Monitor %s: %dx%d at (%d, %d)%s", m.name.c_str(), m.rect.width(), m.rect.height(), m.rect.left,
                       m.rect.top, m.primary ? " primary" : ""));
  }

  // Handle signals synchronously in main; worker threads inherit the blocked mask.
  // SIGUSR1 = restart to apply new settings (requested through the control socket).
  sigset_t signals;
  sigemptyset(&signals);
  sigaddset(&signals, SIGINT);
  sigaddset(&signals, SIGTERM);
  sigaddset(&signals, SIGUSR1);
  pthread_sigmask(SIG_BLOCK, &signals, nullptr);

  UdpMulticast net;
  std::string error;
  if (!net.open(group, static_cast<uint16_t>(port), config.get("interface"), error)) {
    log::info("Network setup failed: " + error);
    return 1;
  }

  // The virtual pointer covers the bounding box of all monitors; the display server maps the
  // device's absolute range onto its whole desktop.
  std::vector<Rect> rects;
  for (const MonitorInfo& m : displays.monitors()) rects.push_back(m.rect);
  UinputDevice device;
  const bool needDevice = role == "master" || !args.dryRun;
  if (needDevice && !device.open("net-mouse " + role + " pointer", boundingRect(rects), role == "master", error)) {
    log::info("Cannot create the virtual pointer: " + error);
    return 1;
  }

  std::unique_ptr<MasterEngine> master;
  std::unique_ptr<SlaveEngine> slave;
  std::unique_ptr<LinuxCursor> cursor;
  std::unique_ptr<LinuxInjector> injector;
  std::unique_ptr<EvdevCapture> capture;
  if (role == "master") {
    cursor = std::make_unique<LinuxCursor>(device);
    master = std::make_unique<MasterEngine>(MasterEngine::Options{id, name, true}, config, net, displays, *cursor);
    // We own the cursor position from now on: start in the middle of the primary monitor.
    cursor->setPos(displays.monitors().front().rect.center());
    EvdevCapture::Options opt;
    opt.speed = std::stod(config.get("pointer_speed", "1.0"));
    opt.accel = std::stod(config.get("pointer_accel", "0.0"));
    capture = std::make_unique<EvdevCapture>(opt, displays, *cursor, device);
    if (!capture->start(master.get(), error)) {
      log::info("Input capture failed: " + error);
      return 1;
    }
  } else {
    injector = std::make_unique<LinuxInjector>(device, args.dryRun);
    slave = std::make_unique<SlaveEngine>(SlaveEngine::Options{id, name}, config, net, displays, *injector);
  }

  DaemonState state{role, name, id, args.config, monitorSpec, master.get(), slave.get()};
  ControlServer control;
  if (!control.start(
          args.control, [&state](const std::string& req, bool privileged) { return handleRequest(state, req, privileged); },
          error)) {
    log::info("Warning: " + error + " (the GUI and net-mouse-ctl status will not work)");
  }

  std::atomic<bool> running{true};
  std::thread netThread([&] { runNetworkLoop(net, master.get(), slave.get(), running); });
  log::info("Running; Ctrl+Alt+Home returns the pointer to the master");

  int sig = 0;
  sigwait(&signals, &sig);
  log::info(sig == SIGUSR1 ? "Restarting to apply new settings"
                           : log::fmt("Received %s, stopping", sig == SIGINT ? "SIGINT" : "SIGTERM"));

  control.stop();
  if (capture) capture->stop();  // releases the mouse grabs
  if (master) master->onPanic();
  running = false;
  netThread.join();
  net.close();
  device.close();
  log::info("Stopped");
  return 0;
}
