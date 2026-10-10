#pragma once

// Local control protocol, shared by the Linux daemon (Unix socket) and the Windows agent (named
// pipe): one request line in, one reply line out ("status" -> JSON, otherwise "ok ..." / "error ...").
//
//   status                      everything the settings windows show (JSON)
//   pair ID | unpair ID         master: connect / disconnect a PC
//   set-offset ID X Y           master: move a connected slave
//   return                      master: bring the pointer back (emergency)
//   forget-master               slave: drop the paired master
//   set-interfaces all|LIST     network interfaces to use (applied immediately)
//   set-role master|slave       restarts the engine with the new role
//   set-monitors SPEC|auto      Linux: monitor layout (restarts)
//   restart                     restart the engine (re-reads the settings)

#include <cstdint>
#include <functional>
#include <string>

#include "app/master_engine.h"
#include "app/slave_engine.h"
#include "core/udp.h"

namespace nm {

struct ControlContext {
  std::string role;  // "" while no role has been chosen yet (Windows: the control app asks)
  std::string name;
  uint64_t id = 0;
  std::string configPath;
  std::string group;
  int port = 0;
  std::string monitorSpec;
  MasterEngine* master = nullptr;
  SlaveEngine* slave = nullptr;
  UdpMulticast* net = nullptr;
  // Applies role/monitor changes: the engine process exits and its supervisor (systemd, the
  // Windows service) starts it again with the new settings.
  std::function<void()> requestRestart;
  // Checks a monitor layout for set-monitors; empty when the platform detects monitors itself.
  std::function<bool(const std::string&)> validMonitorSpec;
};

std::string controlStatusJson(const ControlContext& ctx);
std::string handleControlRequest(const ControlContext& ctx, const std::string& request, bool privileged);

}  // namespace nm
