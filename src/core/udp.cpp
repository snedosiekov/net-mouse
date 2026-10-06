#include "core/udp.h"

#include "core/log.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
using SocketHandle = SOCKET;
using SockLen = int;
static void closeSocket(SocketHandle s) { closesocket(s); }
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle = int;
using SockLen = socklen_t;
static void closeSocket(SocketHandle s) { ::close(s); }
#endif

#include <cctype>
#include <cstring>

namespace nm {
namespace {

SocketHandle handle(intptr_t s) { return static_cast<SocketHandle>(s); }

in_addr toInAddr(uint32_t hostOrder) {
  in_addr a{};
  a.s_addr = htonl(hostOrder);
  return a;
}

#ifdef _WIN32
std::string narrow(const wchar_t* w) {
  if (!w) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) return {};
  std::string s(static_cast<size_t>(n - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  return s;
}
#endif

}  // namespace

std::string ipToString(uint32_t hostOrderIp) {
  char buf[INET_ADDRSTRLEN] = {};
  const in_addr a = toInAddr(hostOrderIp);
  inet_ntop(AF_INET, &a, buf, sizeof(buf));
  return buf;
}

bool parseIp(const std::string& text, uint32_t& hostOrderIp) {
  in_addr a{};
  if (inet_pton(AF_INET, text.c_str(), &a) != 1) return false;
  hostOrderIp = ntohl(a.s_addr);
  return true;
}

std::vector<NetInterface> listInterfaces() {
  std::vector<NetInterface> result;
#ifdef _WIN32
  ULONG size = 16 * 1024;
  std::vector<uint8_t> buf(size);
  const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
  auto* list = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
  ULONG rc = GetAdaptersAddresses(AF_INET, flags, nullptr, list, &size);
  if (rc == ERROR_BUFFER_OVERFLOW) {
    buf.resize(size);
    list = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    rc = GetAdaptersAddresses(AF_INET, flags, nullptr, list, &size);
  }
  if (rc != NO_ERROR) return result;
  for (auto* a = list; a; a = a->Next) {
    if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
    if (a->Flags & IP_ADAPTER_NO_MULTICAST) continue;
    for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
      if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
      const auto* sin = reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr);
      NetInterface ni;
      ni.addr = ntohl(sin->sin_addr.s_addr);
      const unsigned prefix = u->OnLinkPrefixLength;
      ni.mask = prefix == 0 ? 0 : (prefix >= 32 ? 0xFFFFFFFFu : 0xFFFFFFFFu << (32 - prefix));
      ni.name = narrow(a->FriendlyName);
      result.push_back(std::move(ni));
    }
  }
#else
  ifaddrs* list = nullptr;
  if (getifaddrs(&list) != 0) return result;
  for (ifaddrs* a = list; a; a = a->ifa_next) {
    if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET) continue;
    if (!(a->ifa_flags & IFF_UP) || (a->ifa_flags & IFF_LOOPBACK)) continue;
    NetInterface ni;
    ni.addr = ntohl(reinterpret_cast<sockaddr_in*>(a->ifa_addr)->sin_addr.s_addr);
    if (a->ifa_netmask) ni.mask = ntohl(reinterpret_cast<sockaddr_in*>(a->ifa_netmask)->sin_addr.s_addr);
    ni.name = a->ifa_name ? a->ifa_name : "";
    result.push_back(std::move(ni));
  }
  freeifaddrs(list);
#endif
  return result;
}

std::vector<std::string> parseInterfaceList(const std::string& text) {
  std::vector<std::string> list;
  size_t start = 0;
  while (start <= text.size()) {
    size_t comma = text.find(',', start);
    if (comma == std::string::npos) comma = text.size();
    std::string item = text.substr(start, comma - start);
    const size_t b = item.find_first_not_of(" \t");
    const size_t e = item.find_last_not_of(" \t");
    if (b != std::string::npos) list.push_back(item.substr(b, e - b + 1));
    start = comma + 1;
  }
  return list;
}

std::string joinInterfaceList(const std::vector<std::string>& list) {
  std::string out;
  for (const std::string& item : list) out += (out.empty() ? "" : ", ") + item;
  return out;
}

bool interfaceMatches(const NetInterface& ni, const std::string& entry) {
  if (ni.addr != 0 && entry == ipToString(ni.addr)) return true;
  if (entry.size() != ni.name.size()) return false;
  for (size_t i = 0; i < entry.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(entry[i])) != std::tolower(static_cast<unsigned char>(ni.name[i]))) {
      return false;
    }
  }
  return true;
}

UdpMulticast::~UdpMulticast() { close(); }

bool UdpMulticast::open(const std::string& group, uint16_t port, const std::vector<std::string>& filter,
                        std::string& error) {
#ifdef _WIN32
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    error = "WSAStartup failed";
    return false;
  }
  wsaStarted_ = true;
#endif
  if (!parseIp(group, group_) || (group_ >> 28) != 0xE) {
    error = "invalid multicast group: " + group;
    return false;
  }
  port_ = port;
  {
    std::lock_guard lock(mutex_);
    filter_ = filter;
  }

  SocketHandle s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#ifdef _WIN32
  if (s == INVALID_SOCKET) {
#else
  if (s < 0) {
#endif
    error = "socket() failed";
    return false;
  }
  sock_ = static_cast<intptr_t>(s);

  // Several processes on one PC (e.g. a master and a test slave) may share the port.
  int one = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));

  sockaddr_in local{};
  local.sin_family = AF_INET;
  local.sin_port = htons(port_);
  local.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(s, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
    error = "bind() to UDP port " + std::to_string(port_) + " failed";
    close();
    return false;
  }

  int ttl = 1;  // never leave the local network
  setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char*>(&ttl), sizeof(ttl));
  int loop = 1;  // lets a master and a slave on the same PC see each other
  setsockopt(s, IPPROTO_IP, IP_MULTICAST_LOOP, reinterpret_cast<const char*>(&loop), sizeof(loop));
#ifdef __linux__
#ifndef IP_MULTICAST_ALL
#define IP_MULTICAST_ALL 49
#endif
  // Linux would otherwise deliver the group from every interface any socket joined it on.
  int all = 0;
  setsockopt(s, IPPROTO_IP, IP_MULTICAST_ALL, &all, sizeof(all));
#endif

  refreshInterfaces();
  std::lock_guard lock(mutex_);
  if (joined_.empty()) {
    if (filter_.empty()) {
      error = "could not join multicast group " + group;
      close();
      return false;
    }
    log::info("None of the selected network interfaces (" + joinInterfaceList(filter_) +
              ") is up yet; waiting for them");
  }
  return true;
}

void UdpMulticast::close() {
  if (sock_ != -1) {
    closeSocket(handle(sock_));
    sock_ = -1;
  }
#ifdef _WIN32
  if (wsaStarted_) {
    WSACleanup();
    wsaStarted_ = false;
  }
#endif
}

void UdpMulticast::setFilter(const std::vector<std::string>& filter) {
  {
    std::lock_guard lock(mutex_);
    filter_ = filter;
  }
  log::info(filter.empty() ? std::string("Network: using every interface")
                           : "Network: using only " + joinInterfaceList(filter));
  refreshInterfaces();
}

std::vector<std::string> UdpMulticast::filter() const {
  std::lock_guard lock(mutex_);
  return filter_;
}

std::vector<UdpMulticast::Choice> UdpMulticast::choices() const {
  std::lock_guard lock(mutex_);
  std::vector<Choice> out;
  for (const NetInterface& ni : all_) {
    bool selected = filter_.empty();
    for (const std::string& entry : filter_) selected = selected || interfaceMatches(ni, entry);
    out.push_back({ni, selected});
  }
  for (const std::string& entry : filter_) {  // selected but not up right now (e.g. a VPN that is off)
    bool present = false;
    for (const NetInterface& ni : all_) present = present || interfaceMatches(ni, entry);
    if (!present) out.push_back({NetInterface{0, 0, entry}, true});
  }
  return out;
}

bool UdpMulticast::membershipLocked(uint32_t ifaceAddr, bool join) {
  ip_mreq mreq{};
  mreq.imr_multiaddr = toInAddr(group_);
  mreq.imr_interface = toInAddr(ifaceAddr);
  return setsockopt(handle(sock_), IPPROTO_IP, join ? IP_ADD_MEMBERSHIP : IP_DROP_MEMBERSHIP,
                    reinterpret_cast<const char*>(&mreq), sizeof(mreq)) == 0;
}

void UdpMulticast::refreshInterfaces() {
  if (sock_ == -1) return;
  std::vector<NetInterface> found = listInterfaces();
  const uint32_t defaultRoute = defaultRouteAddress();
  std::lock_guard lock(mutex_);
  all_ = found;
  interfaces_.clear();
  for (const NetInterface& ni : found) {
    bool use = filter_.empty();
    for (const std::string& entry : filter_) use = use || interfaceMatches(ni, entry);
    if (use) interfaces_.push_back(ni);
  }
  // Main address: the default-route one if it is in use, else the first interface in use.
  primary_ = interfaces_.empty() ? 0 : interfaces_.front().addr;
  for (const NetInterface& ni : interfaces_) {
    if (ni.addr == defaultRoute) primary_ = defaultRoute;
  }

  std::set<uint32_t> wanted;
  for (const NetInterface& ni : interfaces_) wanted.insert(ni.addr);
  for (auto it = joined_.begin(); it != joined_.end();) {  // leave interfaces no longer in use
    if (!wanted.count(*it) && !(*it == 0 && filter_.empty() && interfaces_.empty())) {
      membershipLocked(*it, false);
      log::info("Left multicast group " + ipToString(group_) + " on " + (*it ? ipToString(*it) : "the default interface"));
      it = joined_.erase(it);
    } else {
      ++it;
    }
  }
  for (uint32_t addr : wanted) {
    if (joined_.count(addr)) continue;
    if (membershipLocked(addr, true)) {
      joined_.insert(addr);
      log::info("Joined multicast group " + ipToString(group_) + " on " + ipToString(addr));
    }
  }
  if (joined_.empty() && filter_.empty() && membershipLocked(0, true)) {
    // No usable interface yet: let the OS choose one.
    joined_.insert(0);
    log::info("Joined multicast group " + ipToString(group_) + " on the default interface");
  }
}

int UdpMulticast::receive(uint8_t* buffer, size_t capacity, uint32_t& sourceIp, int timeoutMs) {
  if (sock_ == -1) return -1;
  SocketHandle s = handle(sock_);
  fd_set fds;
  FD_ZERO(&fds);
  FD_SET(s, &fds);
  timeval tv{};
  tv.tv_sec = timeoutMs / 1000;
  tv.tv_usec = (timeoutMs % 1000) * 1000;
  const int ready = select(static_cast<int>(s) + 1, &fds, nullptr, nullptr, &tv);
  if (ready <= 0) return 0;

  sockaddr_in from{};
  SockLen fromLen = sizeof(from);
  const int n = static_cast<int>(recvfrom(s, reinterpret_cast<char*>(buffer), static_cast<int>(capacity), 0,
                                          reinterpret_cast<sockaddr*>(&from), &fromLen));
  if (n <= 0) return 0;  // e.g. WSAECONNRESET after an ICMP "port unreachable"
  sourceIp = ntohl(from.sin_addr.s_addr);
  // No check of the sender's address against the selected subnets: a network card with several
  // addresses sends from its first one, whatever interface address was chosen. Interfaces that are not
  // selected are kept out by the group membership (joined only on selected interfaces; on Linux
  // with IP_MULTICAST_ALL off).
  return n;
}

void UdpMulticast::setOutgoingLocked(uint32_t ifaceAddr) {
  if (ifaceAddr == currentOutgoing_) return;
  const in_addr a = toInAddr(ifaceAddr);
  setsockopt(handle(sock_), IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<const char*>(&a), sizeof(a));
  currentOutgoing_ = ifaceAddr;
}

void UdpMulticast::sendToGroupLocked(const std::vector<uint8_t>& datagram) {
  sockaddr_in to{};
  to.sin_family = AF_INET;
  to.sin_port = htons(port_);
  to.sin_addr = toInAddr(group_);
  sendto(handle(sock_), reinterpret_cast<const char*>(datagram.data()), static_cast<int>(datagram.size()), 0,
         reinterpret_cast<const sockaddr*>(&to), sizeof(to));
}

void UdpMulticast::sendOnAllActiveLocked(const std::vector<uint8_t>& datagram) {
  if (interfaces_.empty()) {
    if (filter_.empty()) {  // nothing detected: let the OS pick; with a selection, send nowhere else
      setOutgoingLocked(0);
      sendToGroupLocked(datagram);
    }
    return;
  }
  for (const NetInterface& ni : interfaces_) {
    setOutgoingLocked(ni.addr);
    sendToGroupLocked(datagram);
  }
}

uint32_t defaultRouteAddress() {
  // connect() on a UDP socket only selects the route; nothing is sent.
  SocketHandle s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#ifdef _WIN32
  if (s == INVALID_SOCKET) return 0;
#else
  if (s < 0) return 0;
#endif
  sockaddr_in remote{};
  remote.sin_family = AF_INET;
  remote.sin_port = htons(53);
  remote.sin_addr = toInAddr(0x08080808);  // any public address works
  uint32_t result = 0;
  if (connect(s, reinterpret_cast<sockaddr*>(&remote), sizeof(remote)) == 0) {
    sockaddr_in local{};
    SockLen len = sizeof(local);
    if (getsockname(s, reinterpret_cast<sockaddr*>(&local), &len) == 0) result = ntohl(local.sin_addr.s_addr);
  }
  closeSocket(s);
  return result;
}

uint32_t UdpMulticast::primaryAddress() const {
  std::lock_guard lock(mutex_);
  return primary_;
}

bool UdpMulticast::isLocalSubnetLocked(uint32_t ip) const {
  for (const NetInterface& ni : interfaces_) {
    if (ni.mask && (ni.addr & ni.mask) == (ip & ni.mask)) return true;
  }
  return false;
}

bool UdpMulticast::isLocalSubnet(uint32_t ip) const {
  std::lock_guard lock(mutex_);
  return isLocalSubnetLocked(ip);
}

void UdpMulticast::send(const std::vector<uint8_t>& datagram, uint32_t peerIp) {
  if (sock_ == -1) return;
  std::lock_guard lock(mutex_);
  if (peerIp == kRouteDefault || peerIp == kRouteAllInterfaces || !isLocalSubnetLocked(peerIp)) {
    sendOnAllActiveLocked(datagram);  // the right interface is unknown
    return;
  }
  // Several local interfaces can share the peer's subnet (e.g. Ethernet plus a bridged VPN adapter)
  // and only one of them may actually reach it, so send through every match; the receiver drops
  // duplicates by sequence number.
  for (const NetInterface& ni : interfaces_) {
    if (ni.mask && (ni.addr & ni.mask) == (peerIp & ni.mask)) {
      setOutgoingLocked(ni.addr);
      sendToGroupLocked(datagram);
    }
  }
}

}  // namespace nm
