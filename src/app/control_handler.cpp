#include "app/control_handler.h"

#include <sstream>

#include "core/config.h"
#include "core/json.h"
#include "core/log.h"

namespace nm {
namespace {

const char* b(bool v) { return v ? "true" : "false"; }

std::string pointJson(Point p) { return log::fmt("{\"x\":%d,\"y\":%d}", p.x, p.y); }

std::string rectJson(const Rect& r) {
  return log::fmt("{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d}", r.left, r.top, r.width(), r.height());
}

std::string monitorsJson(const std::vector<MonitorInfo>& monitors) {
  std::string out = "[";
  for (size_t i = 0; i < monitors.size(); ++i) {
    const MonitorInfo& m = monitors[i];
    if (i) out += ",";
    out += log::fmt("{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"primary\":%s,\"name\":\"%s\"}", m.rect.left, m.rect.top,
                    m.rect.width(), m.rect.height(), b(m.primary), json::escape(m.name).c_str());
  }
  return out + "]";
}

std::string interfacesJson(const UdpMulticast& net) {
  std::string out = "[";
  for (const UdpMulticast::Choice& c : net.choices()) {
    if (out.size() > 1) out += ",";
    out += log::fmt("{\"name\":\"%s\",\"ip\":\"%s\",\"up\":%s,\"selected\":%s}", json::escape(c.iface.name).c_str(),
                    c.iface.addr ? ipToString(c.iface.addr).c_str() : "", b(c.iface.addr != 0), b(c.selected));
  }
  return out + "]";
}

std::string logJson() {
  std::string out = "[";
  for (const std::string& line : log::recent(8)) {
    if (out.size() > 1) out += ",";
    out += "\"" + json::escape(line) + "\"";
  }
  return out + "]";
}

std::string saveSetting(const ControlContext& ctx, const std::string& key, const std::string& value) {
  Config fresh;
  fresh.load(ctx.configPath);
  if (value.empty()) {
    fresh.erase(key);
  } else {
    fresh.set(key, value);
  }
  if (!fresh.save()) return "error cannot write " + ctx.configPath;
  if (ctx.requestRestart) ctx.requestRestart();
  return "ok restarting";
}

bool parseId(const std::string& text, uint64_t& id) {
  try {
    id = std::stoull(text, nullptr, 16);
    return true;
  } catch (...) {
    return false;
  }
}

}  // namespace

std::string controlStatusJson(const ControlContext& ctx) {
  std::string out = log::fmt(
      "{\"role\":\"%s\",\"name\":\"%s\",\"id\":\"%s\",\"config\":\"%s\",\"group\":\"%s\",\"port\":%d,"
      "\"monitor_spec\":\"%s\",\"interface_filter\":\"%s\",\"interfaces\":%s,\"log\":%s",
      ctx.role.c_str(), json::escape(ctx.name).c_str(), toHex(ctx.id).c_str(), json::escape(ctx.configPath).c_str(),
      ctx.group.c_str(), ctx.port, json::escape(ctx.monitorSpec).c_str(),
      ctx.net ? json::escape(joinInterfaceList(ctx.net->filter())).c_str() : "",
      ctx.net ? interfacesJson(*ctx.net).c_str() : "[]", logJson().c_str());
  if (ctx.master) {
    const MasterSnapshot s = ctx.master->snapshot();
    out += log::fmt(",\"fingerprint\":\"%s\",\"remote\":%s,\"active\":\"%s\",\"pointer\":%s,\"monitors\":%s,\"slaves\":[",
                    s.fingerprint.c_str(), b(s.remote), s.remote ? toHex(s.activeSlave).c_str() : "",
                    pointJson(s.virtualPos).c_str(), monitorsJson(s.masterMonitors).c_str());
    for (size_t i = 0; i < s.slaves.size(); ++i) {
      const SlaveView& v = s.slaves[i];
      if (i) out += ",";
      out += log::fmt(
          "{\"id\":\"%s\",\"name\":\"%s\",\"ip\":\"%s\",\"online\":%s,\"paired\":%s,\"connected\":%s,"
          "\"synced\":%s,\"key_mismatch\":%s,\"other_master\":%s,\"fingerprint\":\"%s\",\"offset\":%s,\"monitors\":[",
          toHex(v.id).c_str(), json::escape(v.name).c_str(), ipToString(v.ip).c_str(), b(v.online), b(v.paired),
          b(v.connected), b(v.synced), b(v.keyMismatch), b(v.otherMaster), v.fingerprint.c_str(),
          pointJson(v.offset).c_str());
      for (size_t k = 0; k < v.monitors.size(); ++k) out += (k ? "," : "") + rectJson(v.monitors[k]);
      out += "]}";
    }
    out += "]";
  } else if (ctx.slave) {
    const SlaveSnapshot s = ctx.slave->snapshot();
    out += log::fmt(
        ",\"fingerprint\":\"%s\",\"paired\":%s,\"connected\":%s,\"configured\":%s,\"active\":%s,\"offset\":%s,"
        "\"monitors\":%s,\"master\":{\"id\":\"%s\",\"name\":\"%s\",\"ip\":\"%s\",\"online\":%s,\"fingerprint\":\"%s\"}",
        s.fingerprint.c_str(), b(s.paired), b(s.connected), b(s.configured), b(s.active), pointJson(s.offset).c_str(),
        monitorsJson(s.monitors).c_str(), toHex(s.masterId).c_str(), json::escape(s.masterName).c_str(),
        s.masterIp ? ipToString(s.masterIp).c_str() : "", b(s.masterOnline), s.masterFingerprint.c_str());
  }
  return out + "}";
}

std::string handleControlRequest(const ControlContext& ctx, const std::string& request, bool privileged) {
  std::istringstream in(request);
  std::string cmd;
  in >> cmd;
  if (cmd == "status") return controlStatusJson(ctx);
  if (!privileged) return "error permission denied: needs root or membership in sudo/wheel/admin/netmouse";

  if (cmd == "pair" || cmd == "unpair" || cmd == "set-offset") {
    if (!ctx.master) return "error only a master manages PCs";
    std::string idText;
    uint64_t id = 0;
    if (!(in >> idText) || !parseId(idText, id)) return "error usage: " + cmd + " SLAVE_ID" + (cmd == "set-offset" ? " X Y" : "");
    if (cmd == "pair") {
      return ctx.master->pairSlave(id) ? "ok" : "error that PC is not available (offline, or paired with another master)";
    }
    if (cmd == "unpair") return ctx.master->unpairSlave(id) ? "ok" : "error that PC is not connected";
    int x = 0, y = 0;
    if (!(in >> x >> y)) return "error usage: set-offset SLAVE_ID X Y";
    return ctx.master->setSlaveOffset(id, {x, y}) ? "ok" : "error that position overlaps another screen";
  }
  if (cmd == "return") {
    if (!ctx.master) return "error only a master has a pointer to return";
    ctx.master->onPanic();
    return "ok";
  }
  if (cmd == "forget-master") {
    if (!ctx.slave) return "error only a slave has a master to forget";
    ctx.slave->forgetMaster();
    return "ok";
  }
  if (cmd == "set-interfaces") {
    if (!ctx.net) return "error no network yet (choose a role first)";
    std::string rest;
    std::getline(in, rest);
    std::vector<std::string> list = parseInterfaceList(rest);
    if (list.size() == 1 && list[0] == "all") list.clear();
    if (list.empty() && rest.find_first_not_of(" \t") == std::string::npos) {
      return "error usage: set-interfaces all|NAME[,NAME...]";
    }
    Config fresh;
    fresh.load(ctx.configPath);
    fresh.set("interfaces", joinInterfaceList(list));
    fresh.erase("interface");
    if (!fresh.save()) return "error cannot write " + ctx.configPath;
    ctx.net->setFilter(list);
    return "ok";
  }
  if (cmd == "set-role") {
    std::string role;
    in >> role;
    if (role != "master" && role != "slave") return "error usage: set-role master|slave";
    log::info("Role changed to " + role + " via the control interface");
    return saveSetting(ctx, "role", role);
  }
  if (cmd == "set-monitors") {
    if (!ctx.validMonitorSpec) return "error monitors are detected automatically on this platform";
    std::string spec;
    in >> spec;
    if (spec == "auto") spec.clear();
    if (!spec.empty() && !ctx.validMonitorSpec(spec)) return "error bad monitor layout (expected WxH+X+Y,...)";
    log::info("Monitor layout changed to '" + (spec.empty() ? std::string("auto") : spec) + "' via the control interface");
    return saveSetting(ctx, "monitors", spec);
  }
  if (cmd == "restart") {
    if (ctx.requestRestart) ctx.requestRestart();
    return "ok restarting";
  }
  return "error unknown request '" + cmd +
         "' (status, pair, unpair, set-offset, return, forget-master, set-interfaces, set-role, set-monitors, restart)";
}

}  // namespace nm
