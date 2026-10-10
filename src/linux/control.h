#pragma once

// Local control socket of the daemon (Unix domain, line based): the GUI and net-mouse-ctl send one
// request line and get one reply line back (JSON for "status", "ok ..." / "error ..." otherwise).
// Anyone may read the status; changes need root or membership in sudo/wheel/admin/netmouse.

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace nm {

constexpr char kDefaultControlSocket[] = "/run/net-mouse/control.sock";

class ControlServer {
 public:
  using Handler = std::function<std::string(const std::string& request, bool privileged)>;

  ~ControlServer() { stop(); }
  bool start(const std::string& path, Handler handler, std::string& error);
  void stop();

 private:
  struct Client {
    int fd = -1;
    bool privileged = false;
    std::string buffer;
  };

  void run();

  std::string path_;
  Handler handler_;
  int listenFd_ = -1;
  int wake_[2] = {-1, -1};
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::vector<Client> clients_;
};

// Client side: sends one request line and returns the reply line (empty + error on failure).
std::string controlRequest(const std::string& path, const std::string& request, std::string& error);


}  // namespace nm
