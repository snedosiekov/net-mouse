#pragma once

#include <cstdint>
#include <vector>

namespace nm {

// Peer address hints for ITransport::send (IPv4, host byte order).
constexpr uint32_t kRouteDefault = 0;                  // let the OS pick the outgoing interface
constexpr uint32_t kRouteAllInterfaces = 0xFFFFFFFFu;  // send a copy out of every interface

class ITransport {
 public:
  virtual ~ITransport() = default;
  // Sends a datagram to the multicast group. peerIp selects the outgoing interfaces: those whose
  // subnet contains peerIp (every interface if none does), every interface (kRouteAllInterfaces)
  // or the default one (kRouteDefault).
  virtual void send(const std::vector<uint8_t>& datagram, uint32_t peerIp) = 0;
  // True if ip lies in the subnet of one of our interfaces (multi-homed peers announce themselves
  // from every interface; this tells which of their addresses we can actually reach).
  virtual bool isLocalSubnet(uint32_t /*ip*/) const { return true; }
};

}  // namespace nm
