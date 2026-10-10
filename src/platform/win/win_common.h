#pragma once

// Small Windows helpers shared by the service, the agent and the control app.

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <thread>

namespace nm::win {

constexpr wchar_t kServiceName[] = L"NetMouse";
constexpr wchar_t kDefaultPipe[] = L"net-mouse-control";
constexpr wchar_t kAgentQuitEvent[] = L"Global\\NetMouseAgentQuit";
constexpr int kAgentExitRestart = 3;  // agent exit code: "start me again now" (role change etc.)

std::wstring widen(const std::string& s);
std::string narrow(const std::wstring& w);

// %ProgramData%\NetMouse: settings, key and logs of the service and its agent.
std::filesystem::path dataDir();
std::filesystem::path exePath();
std::string computerName();
uint64_t randomId();
bool runningAsSystem();
// Makes a file readable and writable by SYSTEM and Administrators only (the private key).
bool restrictToAdmins(const std::filesystem::path& path);

// Local control pipe (\\.\pipe\NAME): one request line in, one reply line out. Usable by SYSTEM,
// Administrators and users logged on at this PC; never over the network.
class PipeServer {
 public:
  using Handler = std::function<std::string(const std::string& request)>;
  ~PipeServer() { stop(); }
  bool start(const std::wstring& name, Handler handler, std::string& error);
  void stop();

 private:
  void run();

  std::wstring path_;
  Handler handler_;
  HANDLE stopEvent_ = nullptr;
  std::thread thread_;
  void* securityDescriptor_ = nullptr;
};

// Client side; nullopt if the agent is not running (or does not answer within timeoutMs).
std::optional<std::string> pipeRequest(const std::wstring& name, const std::string& request, DWORD timeoutMs = 2000);

}  // namespace nm::win
