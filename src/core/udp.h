#pragma once

// UDP multicast transport (Winsock on Windows, BSD sockets elsewhere).

#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "core/transport.h"

namespace nm {

struct NetInterface {
  uint32_t addr = 0;  // IPv4, host byte order
  uint32_t mask = 0;
  std::string name;
};

// Up, non-loopback IPv4 interfaces.
std::vector<NetInterface> listInterfaces();
std::string ipToString(uint32_t hostOrderIp);
// Local address the OS would use for the default route (0 if there is none).
uint32_t defaultRouteAddress();
bool parseIp(const std::string& text, uint32_t& hostOrderIp);

class UdpMulticast : public ITransport {
 public:
  UdpMulticast() = default;
  ~UdpMulticast() override;
  UdpMulticast(const UdpMulticast&) = delete;
  UdpMulticast& operator=(const UdpMulticast&) = delete;

  // iface: optional local IPv4 address to pin all traffic to; empty = use every interface.
  bool open(const std::string& group, uint16_t port, const std::string& iface, std::string& error);
  void close();

  // Joins the group on interfaces that appeared since the last call (DHCP, VPN, cable plugged in).
  void refreshInterfaces();

  // Waits up to timeoutMs. Returns the datagram size, 0 on timeout/transient error, -1 if closed.
  int receive(uint8_t* buffer, size_t capacity, uint32_t& sourceIp, int timeoutMs);

  void send(const std::vector<uint8_t>& datagram, uint32_t peerIp) override;
  bool isLocalSubnet(uint32_t ip) const override;
  uint32_t primaryAddress() const override;

 private:
  bool isLocalSubnetLocked(uint32_t ip) const;
  void setOutgoingLocked(uint32_t ifaceAddr);
  void sendToGroupLocked(const std::vector<uint8_t>& datagram);

  intptr_t sock_ = -1;
  uint32_t group_ = 0;
  uint16_t port_ = 0;
  uint32_t fixedIface_ = 0;
  bool wsaStarted_ = false;

  mutable std::mutex mutex_;  // guards everything below plus the socket's outgoing interface option
  std::vector<NetInterface> interfaces_;
  std::set<uint32_t> joined_;
  uint32_t currentOutgoing_ = 0xFFFFFFFFu;
  uint32_t primary_ = 0;
};

}  // namespace nm
