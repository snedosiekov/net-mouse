#include "linux/control.h"

#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <pwd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include "core/log.h"

namespace nm {
namespace {

constexpr size_t kMaxRequest = 4096;

bool isPrivileged(int fd) {
  ucred cred{};
  socklen_t len = sizeof(cred);
  if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) return false;
  if (cred.uid == 0) return true;
  passwd pw{};
  passwd* result = nullptr;
  char buf[4096];
  if (getpwuid_r(cred.uid, &pw, buf, sizeof(buf), &result) != 0 || !result) return false;
  int count = 64;
  std::vector<gid_t> groups(static_cast<size_t>(count));
  if (getgrouplist(pw.pw_name, pw.pw_gid, groups.data(), &count) < 0) {
    groups.resize(static_cast<size_t>(count));
    if (getgrouplist(pw.pw_name, pw.pw_gid, groups.data(), &count) < 0) return false;
  }
  groups.resize(static_cast<size_t>(count));
  for (gid_t gid : groups) {
    if (const group* g = getgrgid(gid)) {
      const std::string name = g->gr_name;
      if (name == "sudo" || name == "wheel" || name == "admin" || name == "netmouse") return true;
    }
  }
  return false;
}

void writeAll(int fd, const std::string& data) {
  size_t off = 0;
  while (off < data.size()) {
    const ssize_t n = ::send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
    if (n <= 0) {
      if (n < 0 && errno == EINTR) continue;
      return;
    }
    off += static_cast<size_t>(n);
  }
}

}  // namespace

bool ControlServer::start(const std::string& path, Handler handler, std::string& error) {
  path_ = path;
  handler_ = std::move(handler);
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
  ::unlink(path.c_str());  // stale socket from a previous run

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (path.size() >= sizeof(addr.sun_path)) {
    error = "control socket path too long";
    return false;
  }
  std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
  listenFd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (listenFd_ < 0 || ::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
      ::listen(listenFd_, 8) != 0) {
    error = "control socket " + path + ": " + std::strerror(errno);
    if (listenFd_ >= 0) ::close(listenFd_);
    listenFd_ = -1;
    return false;
  }
  ::chmod(path.c_str(), 0666);  // reading the status is open to everyone; changes are checked per peer
  if (pipe2(wake_, O_CLOEXEC | O_NONBLOCK) != 0) {
    error = std::string("pipe2: ") + std::strerror(errno);
    return false;
  }
  running_ = true;
  thread_ = std::thread([this] { run(); });
  return true;
}

void ControlServer::stop() {
  if (!thread_.joinable()) return;
  running_ = false;
  const char b = 1;
  [[maybe_unused]] const ssize_t n = ::write(wake_[1], &b, 1);
  thread_.join();
  for (Client& c : clients_) ::close(c.fd);
  clients_.clear();
  ::close(listenFd_);
  ::close(wake_[0]);
  ::close(wake_[1]);
  ::unlink(path_.c_str());
}

void ControlServer::run() {
  std::vector<pollfd> fds;
  while (running_) {
    fds.clear();
    fds.push_back({wake_[0], POLLIN, 0});
    fds.push_back({listenFd_, POLLIN, 0});
    for (const Client& c : clients_) fds.push_back({c.fd, POLLIN, 0});
    if (::poll(fds.data(), fds.size(), -1) < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (fds[0].revents) break;
    if (fds[1].revents & POLLIN) {
      const int fd = ::accept4(listenFd_, nullptr, nullptr, SOCK_CLOEXEC);
      if (fd >= 0) clients_.push_back({fd, isPrivileged(fd), {}});
    }
    for (size_t i = 2; i < fds.size(); ++i) {
      if (!fds[i].revents) continue;
      Client& c = clients_[i - 2];
      char buf[1024];
      const ssize_t n = ::recv(c.fd, buf, sizeof(buf), 0);
      if (n <= 0) {
        ::close(c.fd);
        c.fd = -1;
        continue;
      }
      c.buffer.append(buf, static_cast<size_t>(n));
      size_t nl;
      while ((nl = c.buffer.find('\n')) != std::string::npos) {
        std::string request = c.buffer.substr(0, nl);
        c.buffer.erase(0, nl + 1);
        if (!request.empty() && request.back() == '\r') request.pop_back();
        writeAll(c.fd, handler_(request, c.privileged) + "\n");
      }
      if (c.buffer.size() > kMaxRequest) {
        ::close(c.fd);
        c.fd = -1;
      }
    }
    clients_.erase(std::remove_if(clients_.begin(), clients_.end(), [](const Client& c) { return c.fd < 0; }),
                   clients_.end());
  }
}

std::string controlRequest(const std::string& path, const std::string& request, std::string& error) {
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
  const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0 || ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    error = "cannot connect to " + path + ": " + std::strerror(errno) + " (is net-mouse.service running?)";
    if (fd >= 0) ::close(fd);
    return {};
  }
  writeAll(fd, request + "\n");
  std::string reply;
  char buf[4096];
  ssize_t n;
  while ((n = ::recv(fd, buf, sizeof(buf), 0)) > 0) {
    reply.append(buf, static_cast<size_t>(n));
    if (reply.find('\n') != std::string::npos) break;
  }
  ::close(fd);
  const size_t nl = reply.find('\n');
  if (nl == std::string::npos) {
    error = "no reply from the daemon";
    return {};
  }
  return reply.substr(0, nl);
}

}  // namespace nm
