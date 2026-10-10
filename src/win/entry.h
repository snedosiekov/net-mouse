#pragma once

// The three faces of net-mouse.exe on Windows:
//   net-mouse.exe --service   the NetMouse Windows service (LocalSystem): keeps an agent running in the
//                             session at the console
//   net-mouse.exe --agent     the engine (network, encryption, hooks / input injection), no window;
//                             started by the service as SYSTEM in the console session
//   net-mouse.exe             the control app: tray icon and windows, talks to the agent over a pipe

#include <string>

namespace nm::win {

struct AgentOptions {
  std::wstring config;     // default: %ProgramData%\NetMouse\netmouse.ini
  std::wstring pipe;       // default: kDefaultPipe
  std::wstring quitEvent;  // named event the service sets to stop the agent
  std::string role;        // overrides the settings (testing)
  bool dryRun = false;     // slave: log input instead of injecting it
};

int runAgent(const AgentOptions& options);
int runService();

}  // namespace nm::win
