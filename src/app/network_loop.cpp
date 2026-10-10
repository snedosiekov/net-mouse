#include "app/network_loop.h"

#include <chrono>
#include <vector>

#include "core/protocol.h"

namespace nm {

uint64_t steadyNowMs() {
  using namespace std::chrono;
  return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

void runNetworkLoop(UdpMulticast& net, MasterEngine* master, SlaveEngine* slave, const std::atomic<bool>& running) {
  std::vector<uint8_t> buffer(2048);
  uint64_t lastTick = 0;
  uint64_t lastInterfaceRefresh = steadyNowMs();
  int silentRefreshes = 0;
  while (running) {
    uint32_t source = 0;
    const int n = net.receive(buffer.data(), buffer.size(), source, 20);
    const uint64_t now = steadyNowMs();
    if (n > 0) {
      if (auto packet = proto::decode(buffer.data(), static_cast<size_t>(n))) {
        if (master) master->onPacket(*packet, source, now);
        if (slave) slave->onPacket(*packet, source, now);
      }
    } else if (n < 0) {
      break;
    }
    if (now - lastTick >= 50) {
      lastTick = now;
      if (master) master->tick(now);
      if (slave) slave->tick(now);
    }
    if (now - lastInterfaceRefresh >= 10'000) {
      lastInterfaceRefresh = now;
      net.refreshInterfaces();
      // A slave that hears no master may have been forgotten by a switch or Wi-Fi access point
      // (e.g. after sleep): announce its group membership again, every 30 s while it lasts.
      if (slave && !slave->snapshot().masterOnline) {
        if (++silentRefreshes >= 3) {
          silentRefreshes = 0;
          net.renewMemberships();
        }
      } else {
        silentRefreshes = 0;
      }
    }
  }
}

}  // namespace nm
