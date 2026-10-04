// net-mouse-daemon: Linux service for both roles. Runs below the display server (evdev + uinput),
// so it works on X11, Wayland and the console alike. Needs access to /dev/uinput and
// /dev/input/event* (it runs as root under systemd).
//
//   net-mouse-daemon [--config FILE] [--role master|slave] [--name NAME] [--monitors SPEC] [--dry-run]
//   net-mouse-daemon --list-devices

#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <thread>

#include "app/master_engine.h"
#include "app/network_loop.h"
#include "app/slave_engine.h"
#include "core/config.h"
#include "core/log.h"
#include "core/udp.h"
#include "platform/linux/linux_platform.h"
#include "platform/linux/uinput_device.h"

using namespace nm;

namespace {

constexpr char kDefaultConfig[] = "/etc/net-mouse/netmouse.ini";
constexpr char kDefaultGroup[] = "239.255.77.77";
constexpr int kDefaultPort = 47800;

struct Args {
  std::string config = kDefaultConfig;
  std::string role;
  std::string name;
  std::string monitors;
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
      "  --dry-run          slave: log received input instead of injecting it\n"
      "  --list-devices     show detected input devices and monitors, then exit\n",
      kDefaultConfig);
}

bool parseArgs(int argc, char** argv, Args& a) {
  for (int i = 1; i < argc; ++i) {
    const std::string s = argv[i];
    const bool hasValue = i + 1 < argc;
    if (s == "--config" && hasValue) {
      a.config = argv[++i];
    } else if (s == "--role" && hasValue) {
      a.role = argv[++i];
    } else if (s == "--name" && hasValue) {
      a.name = argv[++i];
    } else if (s == "--monitors" && hasValue) {
      a.monitors = argv[++i];
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

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!parseArgs(argc, argv, args)) return 2;
  log::setEcho(true);

  Config config;
  config.load(args.config);
  const std::string monitorSpec = !args.monitors.empty() ? args.monitors : config.get("monitors");
  LinuxDisplays displays(monitorSpec);
  if (args.listDevices) return listDevices(displays);

  const std::string role = !args.role.empty() ? args.role : config.get("role");
  if (role != "master" && role != "slave") {
    std::fprintf(stderr, "No role configured: set role=master or role=slave in %s, or pass --role.\n",
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

  // Handle SIGINT/SIGTERM synchronously in main; worker threads inherit the blocked mask.
  sigset_t signals;
  sigemptyset(&signals);
  sigaddset(&signals, SIGINT);
  sigaddset(&signals, SIGTERM);
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

  std::atomic<bool> running{true};
  std::thread netThread([&] { runNetworkLoop(net, master.get(), slave.get(), running); });
  log::info("Running; Ctrl+Alt+Home returns the pointer to the master");

  int sig = 0;
  sigwait(&signals, &sig);
  log::info(log::fmt("Received %s, stopping", sig == SIGINT ? "SIGINT" : "SIGTERM"));

  if (capture) capture->stop();  // releases the mouse grabs
  if (master) master->onPanic();
  running = false;
  netThread.join();
  net.close();
  device.close();
  log::info("Stopped");
  return 0;
}
