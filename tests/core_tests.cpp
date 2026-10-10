// Minimal self-contained tests: protocol and crypto round trips, layout geometry, and master <-> slave
// sessions (pairing, pointer, keyboard, attacks) driven through fake platform objects.

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "app/control_handler.h"
#include "app/master_engine.h"
#include "app/slave_engine.h"
#include "core/config.h"
#include "core/crypto.h"
#include "core/json.h"
#include "core/keymap.h"
#include "core/layout.h"
#include "core/protocol.h"
#include "core/udp.h"

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

namespace fs = std::filesystem;

struct FakeTransport : ITransport {
  std::vector<std::vector<uint8_t>> sent;
  std::vector<uint32_t> peers;  // the peerIp of each sent datagram
  void send(const std::vector<uint8_t>& d, uint32_t peer) override {
    sent.push_back(d);
    peers.push_back(peer);
  }
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
  std::vector<std::pair<uint16_t, int>> keys;
  void moveTo(Point p) override { moves.push_back(p); }
  void button(uint8_t b, bool down) override { buttons.push_back({b, down}); }
  void wheel(int d, bool) override { wheels.push_back(d); }
  void key(uint16_t code, int value) override { keys.push_back({code, value}); }
};

using KeyEvents = std::vector<std::pair<uint16_t, int>>;

// Delivers everything `from` sent to `to` (like the multicast group would) and clears it.
template <typename Engine>
void deliver(FakeTransport& from, Engine& to, uint64_t now) {
  auto packets = std::move(from.sent);
  from.sent.clear();
  from.peers.clear();
  for (const auto& bytes : packets) {
    auto p = proto::decode(bytes.data(), bytes.size());
    CHECK(p.has_value());
    if (p) to.onPacket(*p, 0x0A000002, now);
  }
}

// Like deliver(), but only what was addressed to `peer` (a network that lost the group); returns the count.
template <typename Engine>
int deliverAddressed(FakeTransport& from, Engine& to, uint32_t peer, uint64_t now) {
  auto packets = std::move(from.sent);
  auto peers = std::move(from.peers);
  from.sent.clear();
  from.peers.clear();
  int count = 0;
  for (size_t i = 0; i < packets.size(); ++i) {
    if (peers[i] != peer) continue;
    auto p = proto::decode(packets[i].data(), packets[i].size());
    CHECK(p.has_value());
    if (p) to.onPacket(*p, 0x0A000002, now);
    ++count;
  }
  return count;
}

fs::path tempFile(const std::string& name) { return fs::temp_directory_path() / name; }

// A master (id 1) and a slave (id 2) with fresh identities and settings.
struct Rig {
  explicit Rig(const std::string& tag, Rect slaveScreen = {0, 0, 2560, 1440}) {
    masterPath = tempFile("nm_test_" + tag + "_master.ini");
    slavePath = tempFile("nm_test_" + tag + "_slave.ini");
    fs::remove(masterPath);
    fs::remove(slavePath);
    masterCfg.load(masterPath);
    slaveCfg.load(slavePath);
    masterDisplays.list = {{{0, 0, 1920, 1080}, true, "M1"}};
    slaveDisplays.list = {{slaveScreen, true, "S1"}};
    masterIdentity = crypto::generateIdentity();
    master = std::make_unique<MasterEngine>(MasterEngine::Options{1, "master", true, masterIdentity}, masterCfg,
                                            masterNet, masterDisplays, cursor);
    slave = std::make_unique<SlaveEngine>(SlaveEngine::Options{2, "slave", crypto::generateIdentity()}, slaveCfg,
                                          slaveNet, slaveDisplays, injector);
  }
  ~Rig() {
    master.reset();
    slave.reset();
    fs::remove(masterPath);
    fs::remove(slavePath);
  }

  // Announce + hello: the slave becomes "available" on the master.
  void discover() {
    master->tick(now);
    slave->tick(now);
    deliver(masterNet, *slave, now);
    deliver(slaveNet, *master, now);
  }
  // Session handshake and configuration, after pairSlave() or a restart.
  void handshake() {
    for (int i = 0; i < 3; ++i) {
      deliver(masterNet, *slave, now);  // SessionInit, then config
      deliver(slaveNet, *master, now);  // hello with the session, then the configuration ack
    }
  }
  void connect() {
    discover();
    CHECK(master->pairSlave(2));
    handshake();
  }

  fs::path masterPath, slavePath;
  Config masterCfg, slaveCfg;
  FakeTransport masterNet, slaveNet;
  FakeDisplays masterDisplays, slaveDisplays;
  FakeCursor cursor;
  FakeInjector injector;
  crypto::Identity masterIdentity;
  std::unique_ptr<MasterEngine> master;
  std::unique_ptr<SlaveEngine> slave;
  uint64_t now = 10'000;
};

// ---------------------------------------------------------------------------------------------

void testCrypto() {
  const auto a = crypto::generateIdentity();
  const auto b = crypto::generateIdentity();
  // Both sides derive the same pair key, and nobody else does.
  CHECK(crypto::pairKey(a, b.publicKey, true) == crypto::pairKey(b, a.publicKey, false));
  CHECK(crypto::pairKey(a, b.publicKey, true) != crypto::pairKey(crypto::generateIdentity(), b.publicKey, true));
  const auto k = crypto::pairKey(a, b.publicKey, true);
  const auto n1 = crypto::randomArray<crypto::Nonce16>();
  const auto n2 = crypto::randomArray<crypto::Nonce16>();
  CHECK(crypto::sessionKey(k, n1, n2) == crypto::sessionKey(k, n1, n2));
  CHECK(crypto::sessionKey(k, n1, n2) != crypto::sessionKey(k, n2, n1));
  const std::string fp = crypto::fingerprint(a.publicKey);
  CHECK(fp.size() == 19 && fp[4] == '-' && fp[9] == '-' && fp[14] == '-');

  // Identity files: created once, then loaded unchanged.
  const fs::path keyFile = tempFile("nm_test_identity.key");
  fs::remove(keyFile);
  crypto::Identity created, loaded;
  std::string error;
  CHECK(crypto::loadOrCreateIdentity(keyFile, created, error));
  CHECK(crypto::loadOrCreateIdentity(keyFile, loaded, error));
  CHECK(created.secret == loaded.secret && created.publicKey == loaded.publicKey);
  fs::remove(keyFile);
}

void testProtocol() {
  proto::Packet p;
  p.sender = 0x1122334455667788ull;
  p.seq = 0xDEADBEEF;
  proto::SlaveHello hello{"pc-2", {{0, 0, 1920, 1080}, {-1280, 0, 0, 1024}}, 42, 7, 0x0A0A4597};
  hello.publicKey = crypto::generateIdentity().publicKey;
  hello.sessionNonce = crypto::randomArray<crypto::Nonce16>();
  hello.sessionId = 99;
  p.body = hello;
  auto bytes = proto::encode(p);
  auto d = proto::decode(bytes.data(), bytes.size());
  CHECK(d && d->sender == p.sender && d->seq == p.seq);
  const auto* h = d ? std::get_if<proto::SlaveHello>(&d->body) : nullptr;
  CHECK(h && h->name == "pc-2" && h->monitors.size() == 2 && h->monitors[1].left == -1280 && h->masterId == 42 &&
        h->configVersion == 7 && h->address == 0x0A0A4597 && h->publicKey == hello.publicKey &&
        h->sessionNonce == hello.sessionNonce && h->sessionId == 99);
  CHECK(!proto::decode(bytes.data(), bytes.size() - 1));  // truncated

  // Sealed round trip.
  const auto key = crypto::randomArray<crypto::Key>();
  p.body = proto::Pointer{9, {-5, 70000}, proto::kButtonLeft | proto::kButtonX2, -120, 240, {}};
  bytes = proto::seal(p, 9, proto::KeyKind::Session, key);
  d = proto::decode(bytes.data(), bytes.size());
  const auto* sealed = d ? std::get_if<proto::Sealed>(&d->body) : nullptr;
  CHECK(sealed && sealed->target == 9 && sealed->kind == proto::KeyKind::Session);
  auto opened = d ? proto::open(*d, key) : std::nullopt;
  const auto* ptr = opened ? std::get_if<proto::Pointer>(&opened->body) : nullptr;
  CHECK(ptr && ptr->target == 9 && ptr->pos.x == -5 && ptr->pos.y == 70000 && ptr->buttons == 0x11 &&
        ptr->wheelV == -120 && ptr->wheelH == 240 && opened->seq == p.seq);

  // Wrong key, altered ciphertext, altered (readable) header: all rejected.
  CHECK(d && !proto::open(*d, crypto::randomArray<crypto::Key>()));
  for (size_t pos : {bytes.size() - 20, size_t{14}, size_t{20}}) {  // ciphertext, seq, target
    auto tampered = bytes;
    tampered[pos] ^= 0x01;
    auto t = proto::decode(tampered.data(), tampered.size());
    CHECK(!t || !proto::open(*t, key));
  }

  // Control messages are never accepted unencrypted.
  p.body = proto::Pointer{9, {1, 2}, 0, 0, 0, {}};
  bytes = proto::encode(p);
  CHECK(!proto::decode(bytes.data(), bytes.size()));
  p.body = proto::SlaveConfig{9, 1, {0, 0}};
  bytes = proto::encode(p);
  CHECK(!proto::decode(bytes.data(), bytes.size()));

  // Foreign datagrams are rejected.
  bytes[0] ^= 0xFF;
  CHECK(!proto::decode(bytes.data(), bytes.size()));
}

void testKeyProtocol() {
  proto::Packet p;
  p.sender = 5;
  p.seq = 9;
  proto::KeySet held;
  held.set(29, true);
  held.set(30, true);
  p.body = proto::Key{77, 30, 1, held};
  const auto key = crypto::randomArray<crypto::Key>();
  auto bytes = proto::seal(p, 77, proto::KeyKind::Session, key);
  auto d = proto::decode(bytes.data(), bytes.size());
  auto opened = d ? proto::open(*d, key) : std::nullopt;
  const auto* k = opened ? std::get_if<proto::Key>(&opened->body) : nullptr;
  CHECK(k && k->target == 77 && k->code == 30 && k->value == 1 && k->held.has(29) && k->held.has(30) &&
        !k->held.has(31));
  // The readable target must match the sealed one.
  bytes = proto::seal(p, 78, proto::KeyKind::Session, key);
  d = proto::decode(bytes.data(), bytes.size());
  CHECK(d && !proto::open(*d, key));
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
  auto path = tempFile("nm_test_config.ini");
  Config c;
  c.load(path);
  c.set("role", "master");
  c.setHex("id", 0xABCDEF0123456789ull);
  c.setInt("neg", -42);
  CHECK(c.save());
  Config d;
  CHECK(d.load(path));
  CHECK(d.get("role") == "master" && d.getHex("id") == 0xABCDEF0123456789ull && d.getInt("neg") == -42);
  fs::remove(path);
}

void testInterfaceList() {
  CHECK((parseInterfaceList(" Ethernet 5 , eno1,,10.10.69.150 ") ==
         std::vector<std::string>{"Ethernet 5", "eno1", "10.10.69.150"}));
  CHECK(parseInterfaceList("").empty());
  CHECK(joinInterfaceList({"Ethernet 5", "eno1"}) == "Ethernet 5, eno1");
  NetInterface eth{0x0A0A4596, 0xFFFFFF00, "Ethernet 5"};
  CHECK(interfaceMatches(eth, "ethernet 5"));  // names are case-insensitive
  CHECK(interfaceMatches(eth, "10.10.69.150"));
  CHECK(!interfaceMatches(eth, "Ethernet"));
  CHECK(!interfaceMatches(eth, "10.10.69.151"));
}

void testJson() {
  auto v = json::parse(R"({"a":1.5,"b":[true,false,null],"s":"x\"y\\z\né","o":{"k":"v"},"n":-3})");
  CHECK(v.has_value());
  if (v) {
    CHECK((*v)["a"].asNumber() == 1.5 && (*v)["n"].asInt() == -3);
    CHECK((*v)["b"].size() == 3 && (*v)["b"][0].asBool() && !(*v)["b"][1].asBool(true) &&
          (*v)["b"][2].type == json::Value::Type::Null);
    CHECK((*v)["s"].asString() == "x\"y\\z\n\xC3\xA9");
    CHECK((*v)["o"]["k"].asString() == "v" && (*v)["missing"]["deeper"].asString("d") == "d");
  }
  CHECK(!json::parse("{\"a\":}") && !json::parse("[1,2") && !json::parse("{} trailing"));
  // Escaping round-trips.
  const std::string tricky = "quote\" backslash\\ newline\n tab\t ctrl\x01";
  auto back = json::parse("\"" + json::escape(tricky) + "\"");
  CHECK(back && back->asString() == tricky);
}

// The control protocol (Linux socket / Windows pipe) on a connected master and slave.
void testControlHandler() {
  Rig r("control", {0, 0, 1920, 1080});
  r.discover();
  bool restarted = false;
  ControlContext mc;
  mc.role = "master";
  mc.name = "master";
  mc.id = 1;
  mc.configPath = r.masterPath.string();
  mc.master = r.master.get();
  mc.requestRestart = [&] { restarted = true; };
  auto status = json::parse(handleControlRequest(mc, "status", true));
  CHECK(status && (*status)["role"].asString() == "master" && (*status)["slaves"].size() == 1);
  const std::string slaveId = status ? (*status)["slaves"][0]["id"].asString() : "";
  CHECK(status && !(*status)["slaves"][0]["paired"].asBool(true));

  CHECK(handleControlRequest(mc, "pair " + slaveId, false).rfind("error permission denied", 0) == 0);
  CHECK(handleControlRequest(mc, "pair " + slaveId, true) == "ok");
  r.handshake();
  status = json::parse(handleControlRequest(mc, "status", true));
  CHECK(status && (*status)["slaves"][0]["connected"].asBool());
  CHECK(handleControlRequest(mc, "set-offset " + slaveId + " 0 0", true).rfind("error", 0) == 0);  // overlap
  CHECK(handleControlRequest(mc, "set-offset " + slaveId + " -1920 0", true) == "ok");
  CHECK(handleControlRequest(mc, "forget-master", true).rfind("error", 0) == 0);  // master has none
  CHECK(handleControlRequest(mc, "set-monitors 1x1+0+0", true).rfind("error", 0) == 0);  // not on this platform
  CHECK(handleControlRequest(mc, "set-role slave", true) == "ok restarting" && restarted);
  Config saved;
  saved.load(r.masterPath);
  CHECK(saved.get("role") == "slave");
  CHECK(handleControlRequest(mc, "bogus", true).rfind("error unknown request", 0) == 0);

  ControlContext sc;
  sc.role = "slave";
  sc.slave = r.slave.get();
  status = json::parse(handleControlRequest(sc, "status", true));
  CHECK(status && (*status)["paired"].asBool() && (*status)["master"]["fingerprint"].asString().size() == 19);
  CHECK(handleControlRequest(sc, "forget-master", true) == "ok" && !r.slave->snapshot().paired);
}

void testKeymap() {
  using namespace keymap;
  CHECK(fromWindows(0x41, 0x1E, false) == 30);   // A
  CHECK(fromWindows(0x7B, 0x58, false) == 88);   // F12
  CHECK(fromWindows(0xA3, 0x1D, true) == 97);    // Right Ctrl
  CHECK(fromWindows(0x26, 0x48, true) == 103);   // Up
  CHECK(fromWindows(0x13, 0x45, false) == 119);  // Pause
  CHECK(fromWindows(0x90, 0x45, true) == 69);    // NumLock (reported as extended)
  CHECK(fromWindows(0xA2, 0x21D, false) == 0);   // fake Left Ctrl sent with AltGr
  CHECK(fromWindows(0xAF, 0, false) == 115);     // Volume Up without a scan code
  WinKey wk;
  CHECK(toWindows(30, wk) && wk.scan == 0x1E && !wk.extended && wk.vk == 0);
  CHECK(toWindows(103, wk) && wk.scan == 0x48 && wk.extended);
  CHECK(toWindows(119, wk) && wk.vk == 0x13);
  CHECK(toWindows(69, wk) && wk.vk == 0x90 && wk.extended);
  // Every key Windows can report survives the round trip to an injectable key.
  for (uint32_t scan = 1; scan <= 0x58; ++scan) {
    const uint16_t code = fromWindows(0, scan, false);
    if (code) CHECK(toWindows(code, wk));
  }
}

// ---------------------------------------------------------------------------------------------

void testPairing() {
  Rig r("pairing", {0, 0, 1920, 1080});
  r.discover();
  // Discovered but not connected: listed as available, the pointer cannot go there.
  auto snap = r.master->snapshot();
  CHECK(snap.slaves.size() == 1 && snap.slaves[0].online && !snap.slaves[0].paired && !snap.slaves[0].connected);
  CHECK(snap.slaves[0].fingerprint == r.slave->snapshot().fingerprint);
  CHECK(!r.master->onMouseMove({1919, 500}));
  CHECK(!r.slave->snapshot().paired);

  CHECK(r.master->pairSlave(2));
  r.handshake();
  snap = r.master->snapshot();
  CHECK(snap.slaves[0].paired && snap.slaves[0].connected && snap.slaves[0].synced);
  const auto ss = r.slave->snapshot();
  CHECK(ss.paired && ss.connected && ss.configured && ss.masterId == 1 && ss.masterFingerprint == snap.fingerprint);
  CHECK(r.master->onMouseMove({1919, 500}));

  // Disconnect: the pointer comes back and cannot go there again.
  CHECK(r.master->unpairSlave(2));
  CHECK(!r.master->snapshot().remote && !r.master->snapshot().slaves[0].paired);
  CHECK(!r.master->onMouseMove({1919, 500}));
}

void testSession() {
  Rig r("session");
  r.connect();
  MasterEngine& master = *r.master;
  SlaveEngine& slave = *r.slave;
  FakeInjector& injector = r.injector;
  FakeCursor& cursor = r.cursor;
  uint64_t& now = r.now;
  auto snap = master.snapshot();
  CHECK(snap.slaves.size() == 1 && snap.slaves[0].connected && snap.slaves[0].synced);
  CHECK(snap.slaves[0].offset == Point(1920, 0));
  CHECK(slave.snapshot().offset == Point(1920, 0));

  // Interior move stays local.
  CHECK(!master.onMouseMove({1000, 500}));
  // Hitting the right edge hands the pointer to the slave.
  CHECK(master.onMouseMove({1919, 500}));
  CHECK(master.snapshot().remote && cursor.hidden && cursor.pos == Point(960, 540));
  deliver(r.masterNet, slave, now);
  CHECK(!injector.moves.empty() && injector.moves.back() == Point(0, 500));

  // Relative motion from the park point moves the slave cursor.
  CHECK(master.onMouseMove({1060, 590}));
  deliver(r.masterNet, slave, now);
  CHECK(injector.moves.back() == Point(100, 550));

  // Click and wheel are forwarded (duplicates dropped by sequence number).
  CHECK(master.onMouseButton(proto::kButtonLeft, true));
  CHECK(master.onMouseButton(proto::kButtonLeft, false));
  CHECK(master.onMouseWheel(-120, false));
  deliver(r.masterNet, slave, now);
  CHECK(injector.buttons.size() == 2 && injector.buttons[0].second && !injector.buttons[1].second);
  CHECK(injector.wheels.size() == 1 && injector.wheels[0] == -120);

  // A held button keeps the pointer on the slave even when moving back far.
  CHECK(master.onMouseButton(proto::kButtonRight, true));
  CHECK(master.onMouseMove({960 - 500, 540}));
  CHECK(master.snapshot().remote);
  CHECK(master.onMouseButton(proto::kButtonRight, false));
  deliver(r.masterNet, slave, now);

  // Moving back across the edge returns to the master and releases the slave.
  CHECK(master.onMouseMove({960 - 30, 540}));
  CHECK(!master.snapshot().remote && !cursor.hidden && cursor.pos == Point(1890, 550));
  deliver(r.masterNet, slave, now);
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

  // It comes back with the same session; rearranging: overlap refused, free position accepted.
  slave.tick(now);
  deliver(r.slaveNet, master, now);
  CHECK(master.snapshot().slaves[0].connected);
  CHECK(!master.setSlaveOffset(2, {1000, 0}));
  CHECK(master.setSlaveOffset(2, {-2560, 0}));
  master.tick(now + 1);
  deliver(r.masterNet, slave, now);
  CHECK(slave.snapshot().offset == Point(-2560, 0));
  CHECK(master.onMouseMove({0, 100}));  // left edge now leads to the slave
}

// A slave whose network stopped delivering the multicast group (e.g. Wi-Fi after sleep) still
// finds its master and gets input: announcements and session traffic are also addressed to it.
void testGroupLostAfterSleep() {
  Rig r("grouplost");
  r.connect();
  const uint32_t slaveIp = 0x0A000002;
  r.now += 60'000;  // asleep
  r.slave->tick(r.now);
  CHECK(!r.slave->snapshot().masterOnline);

  deliver(r.slaveNet, *r.master, r.now);  // its hellos still reach the master
  r.master->tick(r.now + 2000);
  CHECK(deliverAddressed(r.masterNet, *r.slave, slaveIp, r.now) > 0);  // the announcement
  CHECK(r.slave->snapshot().masterOnline);

  r.now += 60'000;
  r.slave->tick(r.now);
  CHECK(!r.slave->snapshot().masterOnline);
  deliver(r.slaveNet, *r.master, r.now);
  CHECK(r.master->onMouseMove({1919, 500}));
  CHECK(deliverAddressed(r.masterNet, *r.slave, slaveIp, r.now) > 0);  // session traffic alone
  CHECK(r.slave->snapshot().masterOnline && r.slave->snapshot().active);
  CHECK(!r.injector.moves.empty() && r.injector.moves.back() == Point(0, 500));
}

// A monitor added to the master where a slave was placed: the slave moves out of the way.
void testOverlapAfterMonitorChange() {
  Rig r("overlap", {0, 0, 1920, 1080});
  r.connect();
  CHECK(r.master->snapshot().slaves[0].offset == Point(1920, 0));
  r.masterDisplays.list.push_back({{1920, 0, 3840, 1200}, false, "M2"});  // right where the slave was
  r.now += 1500;
  r.master->tick(r.now);  // notices the new monitor
  const auto view = r.master->snapshot().slaves[0];
  CHECK(view.offset == Point(3840, 0) && !view.synced);
  deliver(r.masterNet, *r.slave, r.now);  // the new position reaches the slave
  CHECK(r.slave->snapshot().offset == Point(3840, 0));
  CHECK(!r.master->onMouseMove({1919, 500}));  // that edge now leads to the master's own monitor
  CHECK(r.master->onMouseMove({3839, 500}));   // and the new one to the slave
}

void testKeyboardSession() {
  Rig r("keyboard", {0, 0, 1920, 1080});
  r.connect();
  MasterEngine& master = *r.master;
  SlaveEngine& slave = *r.slave;
  FakeInjector& injector = r.injector;
  uint64_t& now = r.now;

  constexpr uint16_t A = 30, S = 31, SHIFT = 42, CTRL = 29, ALT = 56, HOME = 102;
  // Pointer on the master: keys stay local.
  CHECK(!master.onKey(A, 1) && !master.onKey(A, 0));

  // Shift pressed locally, then the pointer crosses: the Shift release must stay local.
  CHECK(!master.onKey(SHIFT, 1));
  CHECK(master.onMouseMove({1919, 500}));
  CHECK(master.onKey(A, 1) && master.onKey(A, 2) && master.onKey(A, 0));
  CHECK(!master.onKey(SHIFT, 0));
  deliver(r.masterNet, slave, now);
  CHECK((injector.keys == KeyEvents{{A, 1}, {A, 2}, {A, 0}}));

  // Ctrl pressed on the slave, pointer comes back: the slave releases it, the late release still
  // goes to the slave (where it is ignored), never to the master.
  injector.keys.clear();
  CHECK(master.onKey(CTRL, 1));
  CHECK(master.onMouseMove({960 - 100, 540}));  // back to the master
  CHECK(!master.snapshot().remote);
  CHECK(master.onKey(CTRL, 0));
  deliver(r.masterNet, slave, now);
  CHECK((injector.keys == KeyEvents{{CTRL, 1}, {CTRL, 0}}));

  // A lost release is repaired by the next heartbeat's key set.
  injector.keys.clear();
  CHECK(master.onMouseMove({1919, 500}));
  CHECK(master.onKey(S, 1));
  deliver(r.masterNet, slave, now);
  CHECK(master.onKey(S, 0));
  r.masterNet.sent.clear();  // the release never arrives
  now += 200;
  master.tick(now);  // heartbeat
  deliver(r.masterNet, slave, now);
  CHECK((injector.keys == KeyEvents{{S, 1}, {S, 0}}));

  // Ctrl+Alt+Home while on the slave returns the pointer; Home itself is not sent.
  injector.keys.clear();
  CHECK(master.onKey(CTRL, 1) && master.onKey(ALT, 1));
  CHECK(master.onKey(HOME, 1));
  CHECK(!master.snapshot().remote);
  deliver(r.masterNet, slave, now);
  for (const auto& [code, value] : injector.keys) CHECK(code != HOME);
  CHECK(!injector.keys.empty() && injector.keys.back().second == 0);  // Leave released Ctrl/Alt
}

// Attacks: another master, replayed traffic after a restart, a slave impersonated with another key.
void testSecurity() {
  Rig r("security", {0, 0, 1920, 1080});
  r.connect();
  uint64_t& now = r.now;

  // Record a session's traffic (a pointer move onto the slave), then restart the master.
  CHECK(r.master->onMouseMove({1919, 500}));
  CHECK(r.master->onKey(30, 1));
  const auto recorded = r.masterNet.sent;
  deliver(r.masterNet, *r.slave, now);
  CHECK(r.master->onKey(30, 0));
  CHECK(r.master->onMouseMove({960 - 100, 540}));
  deliver(r.masterNet, *r.slave, now);

  r.master = std::make_unique<MasterEngine>(MasterEngine::Options{1, "master", true, r.masterIdentity}, r.masterCfg,
                                            r.masterNet, r.masterDisplays, r.cursor);
  now += 1000;  // the slave's next hello
  r.discover();
  r.handshake();
  CHECK(r.master->snapshot().slaves[0].connected);  // remembered across the restart, new session

  // Replaying the recorded packets does nothing: they belong to the old session.
  r.injector = FakeInjector{};
  for (const auto& bytes : recorded) {
    auto p = proto::decode(bytes.data(), bytes.size());
    if (p) r.slave->onPacket(*p, 0x0A000002, now);
  }
  CHECK(r.injector.moves.empty() && r.injector.keys.empty());

  // Another master cannot take the slave over.
  Config rogueCfg;
  rogueCfg.load(tempFile("nm_test_rogue.ini"));
  FakeTransport rogueNet;
  FakeCursor rogueCursor;
  MasterEngine rogue({3, "rogue", true, crypto::generateIdentity()}, rogueCfg, rogueNet, r.masterDisplays,
                     rogueCursor);
  rogue.tick(now);
  deliver(rogueNet, *r.slave, now);  // its announcement
  r.slave->tick(now + 1000);
  deliver(r.slaveNet, rogue, now);
  CHECK(!rogue.pairSlave(2));  // the hello says the slave belongs to another master
  CHECK(rogue.snapshot().slaves[0].otherMaster);

  // Forgetting the master on the slave allows another one to connect it.
  r.slave->forgetMaster();
  deliver(r.slaveNet, rogue, now);
  CHECK(rogue.pairSlave(2));
  for (int i = 0; i < 3; ++i) {
    deliver(rogueNet, *r.slave, now);
    deliver(r.slaveNet, rogue, now);
  }
  CHECK(r.slave->snapshot().masterId == 3 && rogue.snapshot().slaves[0].connected);
  fs::remove(tempFile("nm_test_rogue.ini"));

  // A PC announcing itself as a paired slave but with another key is not trusted.
  proto::Packet fake;
  fake.sender = 2;
  proto::SlaveHello h{"slave", {{0, 0, 1920, 1080}}, 1, 0, 0};
  h.publicKey = crypto::generateIdentity().publicKey;
  fake.body = h;
  r.master->onPacket(fake, 0x0A000009, now + 2000);
  const auto view = r.master->snapshot().slaves[0];
  CHECK(view.keyMismatch && !view.connected);
}

// A slave with two network cards announces itself from both; the master must keep the address that
// is on one of its own subnets.
void testMultiHomedSlave() {
  struct SubnetTransport : FakeTransport {
    bool isLocalSubnet(uint32_t ip) const override { return (ip >> 24) == 10; }
  };
  Config cfg;
  cfg.load(tempFile("nm_test_multihomed.ini"));
  SubnetTransport net;
  FakeDisplays displays;
  displays.list = {{{0, 0, 1920, 1080}, true, "M1"}};
  FakeCursor cursor;
  MasterEngine master({1, "master", true, crypto::generateIdentity()}, cfg, net, displays, cursor);

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
  fs::remove(tempFile("nm_test_multihomed.ini"));
}

}  // namespace

int main() {
  testCrypto();
  testProtocol();
  testKeyProtocol();
  testLayout();
  testConfig();
  testKeymap();
  testInterfaceList();
  testJson();
  testControlHandler();
  testPairing();
  testSession();
  testGroupLostAfterSleep();
  testKeyboardSession();
  testOverlapAfterMonitorChange();
  testSecurity();
  testMultiHomedSlave();
  if (g_failures == 0) std::printf("All tests passed\n");
  return g_failures == 0 ? 0 : 1;
}
