#include "platform/win/win_common.h"

#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>

#include <random>

namespace nm::win {
namespace {

// SYSTEM, Administrators and interactively logged-on users; remote clients are rejected anyway.
constexpr wchar_t kPipeSddl[] = L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;OW)(A;;GRGW;;;IU)";
constexpr DWORD kIoTimeoutMs = 1000;

// Waits for an overlapped operation; false on timeout or when `stop` is signalled.
bool waitIo(HANDLE pipe, OVERLAPPED& ov, HANDLE stop, DWORD& transferred) {
  HANDLE waits[2] = {ov.hEvent, stop};
  const DWORD r = WaitForMultipleObjects(stop ? 2 : 1, waits, FALSE, kIoTimeoutMs);
  if (r != WAIT_OBJECT_0) {
    CancelIoEx(pipe, &ov);
    GetOverlappedResult(pipe, &ov, &transferred, TRUE);
    return false;
  }
  return GetOverlappedResult(pipe, &ov, &transferred, FALSE) != FALSE;
}

}  // namespace

std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

std::string narrow(const std::wstring& w) {
  if (w.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

std::filesystem::path dataDir() {
  PWSTR path = nullptr;
  std::filesystem::path dir;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &path))) dir = path;
  CoTaskMemFree(path);
  if (dir.empty()) dir = L"C:\\ProgramData";
  return dir / L"NetMouse";
}

std::filesystem::path exePath() {
  wchar_t buf[MAX_PATH];
  const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  return std::filesystem::path(std::wstring(buf, n));
}

std::string computerName() {
  wchar_t buf[MAX_COMPUTERNAME_LENGTH + 1];
  DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
  return GetComputerNameW(buf, &size) ? narrow(buf) : "pc";
}

uint64_t randomId() {
  std::random_device rd;
  uint64_t id = 0;
  while (id == 0) id = (static_cast<uint64_t>(rd()) << 32) | rd();
  return id;
}

bool runningAsSystem() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
  BYTE buf[256];
  DWORD len = 0;
  bool system = false;
  if (GetTokenInformation(token, TokenUser, buf, sizeof(buf), &len)) {
    system = IsWellKnownSid(reinterpret_cast<TOKEN_USER*>(buf)->User.Sid, WinLocalSystemSid) != FALSE;
  }
  CloseHandle(token);
  return system;
}

bool restrictToAdmins(const std::filesystem::path& path) {
  PSECURITY_DESCRIPTOR sd = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;FA;;;SY)(A;;FA;;;BA)", SDDL_REVISION_1, &sd,
                                                            nullptr)) {
    return false;
  }
  BOOL present = FALSE, defaulted = FALSE;
  PACL dacl = nullptr;
  GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted);
  const DWORD rc = SetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT,
                                         DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr,
                                         nullptr, dacl, nullptr);
  LocalFree(sd);
  return rc == ERROR_SUCCESS;
}

// ---------------------------------------------------------------------------------------------

bool PipeServer::start(const std::wstring& name, Handler handler, std::string& error) {
  path_ = L"\\\\.\\pipe\\" + name;
  handler_ = std::move(handler);
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(kPipeSddl, SDDL_REVISION_1, &securityDescriptor_,
                                                            nullptr)) {
    error = "cannot build the control pipe's security descriptor";
    return false;
  }
  stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  thread_ = std::thread([this] { run(); });
  return true;
}

void PipeServer::stop() {
  if (thread_.joinable()) {
    SetEvent(stopEvent_);
    thread_.join();
  }
  if (stopEvent_) CloseHandle(stopEvent_);
  stopEvent_ = nullptr;
  if (securityDescriptor_) LocalFree(securityDescriptor_);
  securityDescriptor_ = nullptr;
}

// One client at a time: requests are tiny and answered at once; a client that connects and stays
// silent is dropped after kIoTimeoutMs.
void PipeServer::run() {
  SECURITY_ATTRIBUTES sa{sizeof(sa), securityDescriptor_, FALSE};
  HANDLE ioEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  while (WaitForSingleObject(stopEvent_, 0) != WAIT_OBJECT_0) {
    HANDLE pipe = CreateNamedPipeW(path_.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                   PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                   PIPE_UNLIMITED_INSTANCES, 64 * 1024, 4096, 0, &sa);
    if (pipe == INVALID_HANDLE_VALUE) {
      WaitForSingleObject(stopEvent_, 1000);
      continue;
    }
    OVERLAPPED ov{};
    ov.hEvent = ioEvent;
    ResetEvent(ioEvent);
    bool connected = ConnectNamedPipe(pipe, &ov) != FALSE || GetLastError() == ERROR_PIPE_CONNECTED;
    if (!connected && GetLastError() == ERROR_IO_PENDING) {
      HANDLE waits[2] = {ioEvent, stopEvent_};
      if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0) {
        DWORD ignored = 0;
        connected = GetOverlappedResult(pipe, &ov, &ignored, FALSE) != FALSE;
      } else {
        CancelIoEx(pipe, &ov);
      }
    }
    if (connected) {
      std::string request;
      char buf[1024];
      while (request.find('\n') == std::string::npos && request.size() < 4096) {
        ResetEvent(ioEvent);
        DWORD n = 0;
        if (!ReadFile(pipe, buf, sizeof(buf), &n, &ov) && GetLastError() != ERROR_IO_PENDING) break;
        if (!waitIo(pipe, ov, stopEvent_, n) || n == 0) break;
        request.append(buf, n);
      }
      const size_t nl = request.find('\n');
      if (nl != std::string::npos) {
        request.resize(nl);
        if (!request.empty() && request.back() == '\r') request.pop_back();
        const std::string reply = handler_(request) + "\n";
        ResetEvent(ioEvent);
        DWORD written = 0;
        if (WriteFile(pipe, reply.data(), static_cast<DWORD>(reply.size()), &written, &ov) ||
            GetLastError() == ERROR_IO_PENDING) {
          waitIo(pipe, ov, stopEvent_, written);
        }
        FlushFileBuffers(pipe);
      }
      DisconnectNamedPipe(pipe);
    }
    CloseHandle(pipe);
  }
  CloseHandle(ioEvent);
}

std::optional<std::string> pipeRequest(const std::wstring& name, const std::string& request, DWORD timeoutMs) {
  const std::wstring path = L"\\\\.\\pipe\\" + name;
  HANDLE pipe = INVALID_HANDLE_VALUE;
  for (int attempt = 0; attempt < 2 && pipe == INVALID_HANDLE_VALUE; ++attempt) {
    pipe = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
      if (GetLastError() != ERROR_PIPE_BUSY || !WaitNamedPipeW(path.c_str(), timeoutMs)) return std::nullopt;
    }
  }
  if (pipe == INVALID_HANDLE_VALUE) return std::nullopt;
  const std::string line = request + "\n";
  DWORD n = 0;
  std::optional<std::string> reply;
  if (WriteFile(pipe, line.data(), static_cast<DWORD>(line.size()), &n, nullptr)) {
    std::string data;
    char buf[4096];
    while (ReadFile(pipe, buf, sizeof(buf), &n, nullptr) && n > 0) {
      data.append(buf, n);
      const size_t nl = data.find('\n');
      if (nl != std::string::npos) {
        reply = data.substr(0, nl);
        break;
      }
    }
  }
  CloseHandle(pipe);
  return reply;
}

}  // namespace nm::win
