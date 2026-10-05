// Minimal self-contained tests: protocol round trips, layout geometry and an end-to-end
// master <-> slave session driven through fake platform objects.

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "app/master_engine.h"
#include "app/slave_engine.h"
#include "core/config.h"
#include "core/layout.h"
#include "core/protocol.h"

using namespace nm;

static int g_failures = 0;
#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
      ++g_failures;                                                    \
    }                                                                  \
  } while (0)

namespace {

struct FakeTransport : ITransport {
  std::vector<std::vector<uint8_t>> sent;
  void send(const std::vector<uint8_t>& d, uint32_t) override { sent.push_back(d); }
};

struct FakeDisplays : IDisplays {
  std::vector<MonitorInfo> list;
  std::vector<MonitorInfo> monitors() override { return list; }
};

struct FakeCursor : ICursor {
  Point pos;
  bool hidden = false;
  uint8_t buttons = 0;
  void setPos(Point p) override { pos = p; }
  void hide() override { hidden = true; }
  void show() override { hidden = false; }
  uint8_t pressedButtons() override { return buttons; }
};

struct FakeInjector : IInjector {
  std::vector<Point> moves;
  std::vector<std::pair<uint8_t, bool>> buttons;
  std::vector<int> wheels;
  void moveTo(Point p) override { moves.push_back(p); }
  void button(uint8_t b, bool down) override { buttons.push_back({b, down}); }
  void wheel(int d, bool) override { wheels.push_back(d); }
};

template <typename Engine>
void deliver(FakeTransport& from, Engine& to, uint64_t now) {
  auto packets = std::move(from.sent);
  from.sent.clear();
  for (const auto& bytes : packets) {
    auto p = proto::decode(bytes.data(), bytes.size());
    CHECK(p.has_value());
    if (p) to.onPacket(*p, 0x0A000002, now);
  }
}

void testProtocol() {
  proto::Packet p;
  p.sender = 0x1122334455667788ull;
  p.seq = 0xDEADBEEF;
  p.body = proto::SlaveHello{"pc-2", {{0, 0, 1920, 1080}, {-1280, 0, 0, 1024}}, 42, 7};
  auto bytes = proto::encode(p);
  auto d = proto::decode(bytes.data(), bytes.size());
  CHECK(d && d->sender == p.sender && d->seq == p.seq);
  const auto* h = d ? std::get_if<proto::SlaveHello>(&d->body) : nullptr;
  CHECK(h && h->name == "pc-2" && h->monitors.size() == 2 && h->monitors[1].left == -1280 &&
        h->masterId == 42 && h->configVersion == 7);

  p.body = proto::Pointer{9, {-5, 70000}, proto::kButtonLeft | proto::kButtonX2, -120, 240};
  bytes = proto::encode(p);
  d = proto::decode(bytes.data(), bytes.size());
  const auto* ptr = d ? std::get_if<proto::Pointer>(&d->body) : nullptr;
  CHECK(ptr && ptr->target == 9 && ptr->pos.x == -5 && ptr->pos.y == 70000 && ptr->buttons == 0x11 &&
        ptr->wheelV == -120 && ptr->wheelH == 240);

  // The hello's main address is an appended field: a hello from an older slave (without it) still decodes.
  p.body = proto::SlaveHello{"pc-2", {{0, 0, 1920, 1080}}, 42, 7, 0x0A0A4597};
  auto withAddress = proto::encode(p);
  auto d2 = proto::decode(withAddress.data(), withAddress.size());
  CHECK(d2 && std::get<proto::SlaveHello>(d2->body).address == 0x0A0A4597);
  auto d3 = proto::decode(withAddress.data(), withAddress.size() - 4);
  CHECK(d3 && std::get<proto::SlaveHello>(d3->body).address == 0 &&
        std::get<proto::SlaveHello>(d3->body).monitors.size() == 1);

  // Truncated or foreign datagrams are rejected.
  CHECK(!proto::decode(bytes.data(), bytes.size() - 1));
  bytes[0] ^= 0xFF;
  CHECK(!proto::decode(bytes.data(), bytes.size()));
}

void testLayout() {
  using layout::Screen;
  std::vector<Screen> screens = {
      {{0, 0, 1920, 1080}, layout::kMasterOwner},
      {{1920, 0, 4480, 1440}, 7},  // taller slave to the right
  };
  CHECK(layout::findEdgeCrossing(screens, {1919, 500}) == Point(1920, 500));
  CHECK(!layout::findEdgeCrossing(screens, {1918, 500}));
  CHECK(!layout::findEdgeCrossing(screens, {0, 500}));

  // Crossing back and sliding along the edge where the master is shorter.
  CHECK(layout::movePointer(screens, {1930, 500}, -20, 0) == Point(1910, 500));
  CHECK(layout::movePointer(screens, {2000, 1300}, -200, 0) == Point(1920, 1300));
  CHECK(layout::movePointer(screens, {2000, 1430}, 0, 50) == Point(2000, 1439));

  const Point off = layout::autoPlace({{0, 0, 1920, 1080}}, {0, 0, 1920, 1080}, {-100, 0, 1820, 1080});
  CHECK(off == Point(2020, 0));
  CHECK(layout::overlapsAny({{0, 0, 10, 10}}, {{9, 9, 20, 20}}));
  CHECK(!layout::overlapsAny({{0, 0, 10, 10}}, {{10, 0, 20, 10}}));
}

void testConfig() {
  auto path = std::filesystem::temp_directory_path() / "nm_test_config.ini";
  Config c;
  c.load(path);
  c.set("role", "master");
  c.setHex("id", 0xABCDEF0123456789ull);
  c.setInt("neg", -42);
  CHECK(c.save());
  Config d;
  CHECK(d.load(path));
  CHECK(d.get("role") == "master" && d.getHex("id") == 0xABCDEF0123456789ull && d.getInt("neg") == -42);
  std::filesystem::remove(path);
}

void testSession() {
  auto dir = std::filesystem::temp_directory_path();
  Config masterCfg, slaveCfg;
  masterCfg.load(dir / "nm_test_master.ini");
  slaveCfg.load(dir / "nm_test_slave.ini");

  FakeTransport masterNet, slaveNet;
  FakeDisplays masterDisplays, slaveDisplays;
  masterDisplays.list = {{{0, 0, 1920, 1080}, true, "M1"}};
  slaveDisplays.list = {{{0, 0, 2560, 1440}, true, "S1"}};
  FakeCursor cursor;
  FakeInjector injector;

  MasterEngine master({1, "master", true}, masterCfg, masterNet, masterDisplays, cursor);
  SlaveEngine slave({2, "slave"}, slaveCfg, slaveNet, slaveDisplays, injector);

  uint64_t now = 10'000;
  // Discovery and configuration handshake.
  slave.tick(now);
  deliver(slaveNet, master, now);  // hello -> master auto-places the slave and sends config
  deliver(masterNet, slave, now);  // config -> slave applies it and acknowledges
  deliver(slaveNet, master, now);  // ack
  auto snap = master.snapshot();
  CHECK(snap.slaves.size() == 1 && snap.slaves[0].online && snap.slaves[0].synced);
  CHECK(snap.slaves[0].offset == Point(1920, 0));
  CHECK(slave.snapshot().offset == Point(1920, 0));

  // Interior move stays local.
  CHECK(!master.onMouseMove({1000, 500}));
  // Hitting the right edge hands the pointer to the slave.
  CHECK(master.onMouseMove({1919, 500}));
  CHECK(master.snapshot().remote && cursor.hidden && cursor.pos == Point(960, 540));
  deliver(masterNet, slave, now);
  CHECK(!injector.moves.empty() && injector.moves.back() == Point(0, 500));

  // Relative motion from the park point moves the slave cursor.
  CHECK(master.onMouseMove({1060, 590}));
  deliver(masterNet, slave, now);
  CHECK(injector.moves.back() == Point(100, 550));

  // Click and wheel are forwarded (duplicates dropped by sequence number).
  CHECK(master.onMouseButton(proto::kButtonLeft, true));
  CHECK(master.onMouseButton(proto::kButtonLeft, false));
  CHECK(master.onMouseWheel(-120, false));
  deliver(masterNet, slave, now);
  CHECK(injector.buttons.size() == 2 && injector.buttons[0].second && !injector.buttons[1].second);
  CHECK(injector.wheels.size() == 1 && injector.wheels[0] == -120);

  // A held button keeps the pointer on the slave even when moving back far.
  CHECK(master.onMouseButton(proto::kButtonRight, true));
  CHECK(master.onMouseMove({960 - 500, 540}));
  CHECK(master.snapshot().remote);
  CHECK(master.onMouseButton(proto::kButtonRight, false));
  deliver(masterNet, slave, now);

  // Moving back across the edge returns to the master and releases the slave.
  CHECK(master.onMouseMove({960 - 30, 540}));
  CHECK(!master.snapshot().remote && !cursor.hidden && cursor.pos == Point(1890, 550));
  deliver(masterNet, slave, now);
  CHECK(!slave.snapshot().active);

  // Edge crossing is refused while a button is held locally (e.g. dragging a window).
  cursor.buttons = proto::kButtonLeft;
  CHECK(!master.onMouseMove({1919, 500}));
  cursor.buttons = 0;

  // The slave disappearing while controlled returns the pointer to the master.
  CHECK(master.onMouseMove({1919, 300}));
  now += 5000;
  master.tick(now);
  CHECK(!master.snapshot().remote && cursor.pos == Point(1919, 300));

  // Rearranging: overlapping the master is refused, a free position is accepted.
  slave.tick(now);
  deliver(slaveNet, master, now);
  CHECK(!master.setSlaveOffset(2, {1000, 0}));
  CHECK(master.setSlaveOffset(2, {-2560, 0}));
  master.tick(now + 1);
  deliver(masterNet, slave, now);
  CHECK(slave.snapshot().offset == Point(-2560, 0));
  CHECK(master.onMouseMove({0, 100}));  // left edge now leads to the slave

  std::filesystem::remove(dir / "nm_test_master.ini");
  std::filesystem::remove(dir / "nm_test_slave.ini");
}

// A slave with two network cards announces itself from both; the master must keep the address that
// is on one of its own subnets.
void testMultiHomedSlave() {
  struct SubnetTransport : FakeTransport {
    bool isLocalSubnet(uint32_t ip) const override { return (ip >> 24) == 10; }
  };
  Config cfg;
  cfg.load(std::filesystem::temp_directory_path() / "nm_test_multihomed.ini");
  SubnetTransport net;
  FakeDisplays displays;
  displays.list = {{{0, 0, 1920, 1080}, true, "M1"}};
  FakeCursor cursor;
  MasterEngine master({1, "master", true}, cfg, net, displays, cursor);

  proto::Packet hello;
  hello.sender = 7;
  hello.body = proto::SlaveHello{"kie", {{0, 0, 1920, 1080}}, 0, 0};
  const uint32_t reachable = 0x0A0A4597;    // 10.10.69.151
  const uint32_t unreachable = 0xC0A80D03;  // 192.168.13.3
  master.onPacket(hello, reachable, 1000);
  master.onPacket(hello, unreachable, 1001);
  CHECK(master.snapshot().slaves[0].ip == reachable);
  master.onPacket(hello, reachable, 1002);
  CHECK(master.snapshot().slaves[0].ip == reachable);

  // First seen only on an unreachable address, later on a reachable one: switch to it.
  hello.sender = 8;
  master.onPacket(hello, unreachable, 1003);
  master.onPacket(hello, reachable, 1004);
  CHECK(master.snapshot().slaves[1].ip == reachable);

  // Two reachable addresses: no flipping, and the slave's own main address wins when it reports one.
  const uint32_t other = 0x0A0A0102;  // 10.10.1.2, also reachable
  hello.sender = 9;
  master.onPacket(hello, other, 1005);
  master.onPacket(hello, reachable, 1006);
  CHECK(master.snapshot().slaves[2].ip == other);
  std::get<proto::SlaveHello>(hello.body).address = reachable;
  master.onPacket(hello, other, 1007);
  CHECK(master.snapshot().slaves[2].ip == reachable);
  std::filesystem::remove(std::filesystem::temp_directory_path() / "nm_test_multihomed.ini");
}

}  // namespace

int main() {
  testProtocol();
  testLayout();
  testConfig();
  testSession();
  testMultiHomedSlave();
  if (g_failures == 0) std::printf("All tests passed\n");
  return g_failures == 0 ? 0 : 1;
}
