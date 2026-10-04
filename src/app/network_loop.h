#pragma once

#include <atomic>
#include <cstdint>

#include "app/master_engine.h"
#include "app/slave_engine.h"
#include "core/udp.h"

namespace nm {

uint64_t steadyNowMs();

// Receives datagrams and drives the engine (exactly one of master/slave is non-null) until
// `running` becomes false or the socket is closed.
void runNetworkLoop(UdpMulticast& net, MasterEngine* master, SlaveEngine* slave, const std::atomic<bool>& running);

}  // namespace nm
