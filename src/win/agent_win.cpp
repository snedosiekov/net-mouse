// The agent: runs the master or slave engine in the console session (as SYSTEM when started by the
// service), serves the control pipe for the control app, and follows the input desktop.

#include <winsock2.h>
#include <windows.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <thread>

#include "app/control_handler.h"
#include "app/master_engine.h"
#include "app/network_loop.h"
#include "app/slave_engine.h"
#include "core/config.h"
#include "core/crypto.h"
#include "core/log.h"
#include "core/udp.h"
#include "platform/win/win_common.h"
#include "platform/win/win_platform.h"
#include "win/entry.h"

namespace fs = std::filesystem;

namespace nm::win {
namespace {

constexpr char kDefaultGroup[] = "239.255.77.77";
constexpr int kDefaultPort = 47800;

}  // namespace

int runAgent(const AgentOptions& options) {
  // Physical pixels everywhere: hook coordinates, SetCursorPos and SendInput then agree.
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  const fs::path configPath = options.config.empty() ? dataDir() / L"netmouse.ini" : fs::path(options.config);
  std::error_code ec;
  fs::create_directories(configPath.parent_path(), ec);
  log::init(configPath.parent_path() / "netmouse-agent.log");

  Config config;
  config.load(configPath);
  std::string role = !options.role.empty() ? options.role : config.get("role");
  if (role != "master" && role != "slave") role.clear();
  uint64_t id = config.getHex("id");
  if (id == 0) {
    id = randomId();
    config.setHex("id", id);
  }
  const std::string name = config.get("name", computerName());
  const std::string group = config.get("group", kDefaultGroup);
  const int port = static_cast<int>(config.getInt("port", kDefaultPort));
  if (!config.has("group")) config.set("group", group);
  if (!config.has("port")) config.setInt("port", port);
  if (!config.has("interfaces")) config.set("interfaces", "");
  if (role == "master" && !config.has("hide_cursor")) config.setInt("hide_cursor", 1);
  if (!config.save()) log::info("Warning: cannot write " + configPath.string());
  log::info(log::fmt("Agent starting as %s '%s' (id %s, %s), config %s", role.empty() ? "(no role yet)" : role.c_str(),
                     name.c_str(), toHex(id).c_str(), runningAsSystem() ? "SYSTEM" : "user", configPath.string().c_str()));

  HANDLE quit = options.quitEvent.empty() ? nullptr : OpenEventW(SYNCHRONIZE, FALSE, options.quitEvent.c_str());
  HANDLE restart = CreateEventW(nullptr, TRUE, FALSE, nullptr);

  ControlContext ctx;
  ctx.role = role;
  ctx.name = name;
  ctx.id = id;
  ctx.configPath = configPath.string();
  ctx.group = group;
  ctx.port = port;
  ctx.requestRestart = [restart] { SetEvent(restart); };

  UdpMulticast net;
  WinDisplays displays;
  WinCursor cursor;
  std::unique_ptr<WinInjector> injector;
  std::unique_ptr<MasterEngine> master;
  std::unique_ptr<SlaveEngine> slave;
  WinInputHook hook;
  std::thread netThread;
  std::atomic<bool> running{true};

  if (!role.empty()) {
    std::string error;
    std::vector<std::string> interfaces = parseInterfaceList(config.get("interfaces"));
    if (interfaces.empty() && !config.get("interface").empty()) interfaces.push_back(config.get("interface"));
    if (!net.open(group, static_cast<uint16_t>(port), interfaces, error)) {
      log::info("Network setup failed: " + error);
      return 1;
    }
    // This PC's key pair, next to the settings; only SYSTEM and Administrators may read it.
    const fs::path keyPath = fs::path(configPath).replace_extension(".key");
    crypto::Identity identity;
    if (!crypto::loadOrCreateIdentity(keyPath, identity, error)) {
      log::info("Cannot load this PC's key: " + error);
      return 1;
    }
    if (runningAsSystem() && !restrictToAdmins(keyPath)) log::info("Warning: cannot restrict access to " + keyPath.string());
    log::info("This PC's fingerprint: " + crypto::fingerprint(identity.publicKey));

    if (role == "master") {
      MasterEngine::Options opt{id, name, config.getInt("hide_cursor", 1) != 0, identity};
      master = std::make_unique<MasterEngine>(opt, config, net, displays, cursor);
    } else {
      injector = std::make_unique<WinInjector>(options.dryRun);
      slave = std::make_unique<SlaveEngine>(SlaveEngine::Options{id, name, identity}, config, net, displays, *injector);
    }
    ctx.master = master.get();
    ctx.slave = slave.get();
    ctx.net = &net;
    netThread = std::thread([&] { runNetworkLoop(net, master.get(), slave.get(), running); });
  }

  PipeServer pipe;
  std::string pipeError;
  if (!pipe.start(options.pipe.empty() ? kDefaultPipe : options.pipe,
                  [&ctx](const std::string& request) { return handleControlRequest(ctx, request, true); }, pipeError)) {
    log::info("Control pipe: " + pipeError);
  }

  // Main loop: stop/restart requests, and (master) keep the input hooks on the user's desktop.
  // While the master's own screen is locked or shows a UAC prompt, nothing is captured: a locked
  // master must not drive the other PCs.
  int exitCode = 0;
  std::wstring pausedOn;
  while (true) {
    if (master) {
      const std::wstring input = inputDesktopName();
      const std::wstring hooked = hook.desktop();
      if (input == L"Default") {
        if (hooked != L"Default") {
          if (!hooked.empty()) hook.stop();
          if (!hook.start(master.get())) log::info("Could not install the input hooks");
          if (!pausedOn.empty()) log::info("Input desktop is back; mouse and keyboard sharing resumed");
          pausedOn.clear();
        }
      } else if (input != pausedOn) {
        if (!hooked.empty()) hook.stop();
        master->onPanic();
        log::info("Input desktop is '" + narrow(input) + "' (locked or UAC prompt): sharing paused");
        pausedOn = input;
      }
    }
    HANDLE waits[2] = {restart, quit};
    const DWORD r = WaitForMultipleObjects(quit ? 2 : 1, waits, FALSE, 500);
    if (r == WAIT_OBJECT_0) {
      exitCode = kAgentExitRestart;
      log::info("Restarting to apply new settings");
      break;
    }
    if (r == WAIT_OBJECT_0 + 1) {
      log::info("Stop requested by the service");
      break;
    }
  }

  pipe.stop();
  hook.stop();
  if (master) master->onPanic();  // never leave the cursor hidden
  running = false;
  if (netThread.joinable()) netThread.join();
  net.close();
  if (quit) CloseHandle(quit);
  CloseHandle(restart);
  log::info("Agent stopped");
  return exitCode;
}

}  // namespace nm::win
