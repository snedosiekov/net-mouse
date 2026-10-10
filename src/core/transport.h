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
  // Sends a datagram. kRouteAllInterfaces / kRouteDefault: to the multicast group through every
  // interface / the default one. A peer address: to that peer (unicast; to the group when the peer
  // is on this PC, and additionally when several interfaces share its subnet).
  virtual void send(const std::vector<uint8_t>& datagram, uint32_t peerIp) = 0;
  // True if ip lies in the subnet of one of our interfaces (multi-homed peers announce themselves
  // from every interface; this tells which of their addresses we can actually reach).
  virtual bool isLocalSubnet(uint32_t /*ip*/) const { return true; }
  // This host's main IPv4 address (the one on its default route), 0 if unknown.
  virtual uint32_t primaryAddress() const { return 0; }
};

}  // namespace nm
