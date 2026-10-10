// The NetMouse service (LocalSystem). A service runs in session 0 and can neither see nor drive a
// user's desktop, so it only supervises the agent: one net-mouse.exe --agent, started as SYSTEM in
// the session at the console (also the logon screen), restarted when it ends or when the console
// session changes (fast user switching, remote desktop to the console, logoff).

#include <windows.h>
#include <wtsapi32.h>

#include <algorithm>
#include <filesystem>
#include <string>

#include "core/log.h"
#include "platform/win/win_common.h"
#include "win/entry.h"

namespace nm::win {
namespace {

SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
SERVICE_STATUS g_status{};
HANDLE g_stopEvent = nullptr;
HANDLE g_sessionEvent = nullptr;

void reportStatus(DWORD state, DWORD waitHint = 0) {
  g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  g_status.dwCurrentState = state;
  g_status.dwControlsAccepted =
      state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_SESSIONCHANGE : 0;
  g_status.dwWaitHint = waitHint;
  SetServiceStatus(g_statusHandle, &g_status);
}

DWORD WINAPI controlHandler(DWORD control, DWORD /*eventType*/, LPVOID /*eventData*/, LPVOID /*context*/) {
  switch (control) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
      reportStatus(SERVICE_STOP_PENDING, 8000);
      SetEvent(g_stopEvent);
      return NO_ERROR;
    case SERVICE_CONTROL_SESSIONCHANGE:
      SetEvent(g_sessionEvent);
      return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:
      return NO_ERROR;
    default:
      return ERROR_CALL_NOT_IMPLEMENTED;
  }
}

struct Agent {
  HANDLE process = nullptr;
  DWORD session = 0;
  ULONGLONG startedAt = 0;
};

// Starts the agent as SYSTEM in `session`: our own token, with the session id changed.
bool launchAgent(DWORD session, HANDLE quitEvent, Agent& agent) {
  ResetEvent(quitEvent);
  HANDLE own = nullptr;
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(),
                        TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY | TOKEN_QUERY | TOKEN_ADJUST_DEFAULT |
                            TOKEN_ADJUST_SESSIONID,
                        &own)) {
    log::info(log::fmt("OpenProcessToken failed: %lu", GetLastError()));
    return false;
  }
  const bool ok = DuplicateTokenEx(own, MAXIMUM_ALLOWED, nullptr, SecurityIdentification, TokenPrimary, &token) &&
                  SetTokenInformation(token, TokenSessionId, &session, sizeof(session));
  CloseHandle(own);
  if (!ok) {
    log::info(log::fmt("Preparing the agent token failed: %lu", GetLastError()));
    if (token) CloseHandle(token);
    return false;
  }
  const std::wstring exe = exePath().wstring();
  std::wstring cmd = L"\"" + exe + L"\" --agent --quit-event " + kAgentQuitEvent;
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.lpDesktop = const_cast<wchar_t*>(L"winsta0\\default");
  PROCESS_INFORMATION pi{};
  const BOOL created = CreateProcessAsUserW(token, exe.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                                            CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr, &si, &pi);
  CloseHandle(token);
  if (!created) {
    log::info(log::fmt("Starting the agent in session %lu failed: %lu", session, GetLastError()));
    return false;
  }
  CloseHandle(pi.hThread);
  agent = {pi.hProcess, session, GetTickCount64()};
  log::info(log::fmt("Agent started in session %lu (pid %lu)", session, pi.dwProcessId));
  return true;
}

void stopAgent(Agent& agent, HANDLE quitEvent) {
  if (!agent.process) return;
  SetEvent(quitEvent);
  if (WaitForSingleObject(agent.process, 5000) != WAIT_OBJECT_0) {
    log::info("Agent did not stop in time; terminating it");
    TerminateProcess(agent.process, 1);
    WaitForSingleObject(agent.process, 2000);
  }
  CloseHandle(agent.process);
  agent = {};
}

void serviceLoop() {
  // The agent opens this by name; created by SYSTEM, only SYSTEM and Administrators can set it.
  HANDLE quitEvent = CreateEventW(nullptr, TRUE, FALSE, kAgentQuitEvent);
  Agent agent;
  ULONGLONG nextLaunch = 0;
  ULONGLONG backoff = 1000;
  while (true) {
    const DWORD console = WTSGetActiveConsoleSessionId();
    if (agent.process && agent.session != console) {
      log::info(log::fmt("Console session changed (%lu -> %lu); moving the agent", agent.session, console));
      stopAgent(agent, quitEvent);
      nextLaunch = 0;
    }
    if (!agent.process && console != 0xFFFFFFFF && GetTickCount64() >= nextLaunch) {
      if (!launchAgent(console, quitEvent, agent)) nextLaunch = GetTickCount64() + 5000;
    }

    HANDLE waits[3] = {g_stopEvent, g_sessionEvent, agent.process};
    const DWORD r = WaitForMultipleObjects(agent.process ? 3 : 2, waits, FALSE, 2000);
    if (r == WAIT_OBJECT_0) break;  // service stop
    if (r == WAIT_OBJECT_0 + 2) {   // the agent ended
      DWORD code = 0;
      GetExitCodeProcess(agent.process, &code);
      const ULONGLONG ran = GetTickCount64() - agent.startedAt;
      CloseHandle(agent.process);
      agent = {};
      if (code == static_cast<DWORD>(kAgentExitRestart)) {
        nextLaunch = 0;  // settings changed: start again at once
        backoff = 1000;
      } else {
        // Crashing right after start: back off up to 30 s; after a long run start again quickly.
        backoff = ran < 10000 ? std::min<ULONGLONG>(backoff * 2, 30000) : 1000;
        nextLaunch = GetTickCount64() + backoff;
        log::info(log::fmt("Agent exited with code %lu after %llu s; starting it again in %llu s", code, ran / 1000,
                           backoff / 1000));
      }
    }
  }
  stopAgent(agent, quitEvent);
  CloseHandle(quitEvent);
}

void WINAPI serviceMain(DWORD /*argc*/, LPWSTR* /*argv*/) {
  g_statusHandle = RegisterServiceCtrlHandlerExW(kServiceName, controlHandler, nullptr);
  if (!g_statusHandle) return;
  g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  g_sessionEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  reportStatus(SERVICE_START_PENDING, 3000);
  std::error_code ec;
  std::filesystem::create_directories(dataDir(), ec);
  log::init(dataDir() / "netmouse-service.log");
  log::info("Service started");
  reportStatus(SERVICE_RUNNING);
  serviceLoop();
  log::info("Service stopped");
  reportStatus(SERVICE_STOPPED);
}

}  // namespace

int runService() {
  SERVICE_TABLE_ENTRYW table[] = {{const_cast<wchar_t*>(kServiceName), serviceMain}, {nullptr, nullptr}};
  return StartServiceCtrlDispatcherW(table) ? 0 : 1;
}

}  // namespace nm::win
