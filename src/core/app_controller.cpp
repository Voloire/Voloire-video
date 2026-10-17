#include "app_controller.h"
#include <algorithm>
#include <filesystem>
#include "devices.h"
#include "telemetry.h"

namespace vv {

namespace {
std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
  return s;
}
json err(const std::string& m) { return {{"ok", false}, {"error", m}}; }
}  // namespace

AppController::AppController() { cfg_.outDir = defaultOutDir(); }

RecordConfig AppController::config() const {
  std::lock_guard lk(mu_);
  return cfg_;
}

void AppController::setConfig(const RecordConfig& c) {
  std::lock_guard lk(mu_);
  cfg_ = c;
}

std::string AppController::resolveTarget(RecordConfig& c, const json& hints) const {
  if (c.videoSource != "window") {
    return {};
  }
  auto wins = listWindows();
  const WindowInfo* pick = nullptr;
  std::string title = hints.value("windowTitle", std::string());
  std::string exe = hints.value("windowExe", std::string());
  if (hints.value("foreground", false)) {
    for (auto& w : wins) if (w.foreground) pick = &w;
  }
  if (!pick && c.hwnd) {
    for (auto& w : wins) if (w.hwnd == c.hwnd) pick = &w;
  }
  if (!pick && (!title.empty() || !exe.empty())) {
    for (auto& w : wins) {
      bool t = title.empty() || lower(w.title).find(lower(title)) != std::string::npos;
      bool e = exe.empty() || lower(w.exe) == lower(exe);
      if (t && e) { pick = &w; break; }
    }
    if (!pick) return "no window matches the given title/exe";
  }
  if (pick) {
    c.hwnd = pick->hwnd;
    if (c.pid == 0) c.pid = pick->pid;
  }
  return {};
}

json AppController::handle(const json& req) {
  const std::string cmd = req.value("cmd", std::string());
  if (cmd == "ping") return {{"ok", true}, {"version", VV_VERSION}, {"state", stateName(rec_.state())}};
  if (cmd == "list_windows") {
    json a = json::array();
    for (auto& w : listWindows()) a.push_back(w.toJson());
    return {{"ok", true}, {"windows", a}};
  }
  if (cmd == "list_monitors") {
    json a = json::array();
    for (auto& m : listMonitors()) a.push_back(m.toJson());
    return {{"ok", true}, {"monitors", a}};
  }
  if (cmd == "list_devices") {
    json a = json::array();
    for (auto& d : listAudioDevices()) a.push_back(d.toJson());
    return {{"ok", true}, {"devices", a}};
  }
  if (cmd == "status") {
    json s = rec_.status();
    s["ok"] = true;
    s["share"] = shareStatus();
    return s;
  }
  if (cmd == "metrics") return {{"ok", true}, {"metrics", Telemetry::get().snapshot()}};
  if (cmd == "logs") return {{"ok", true}, {"logs", Telemetry::get().tailLogs(req.value("n", 100))}};
  if (cmd == "get_config") return {{"ok", true}, {"config", config().toJson()}};
  if (cmd == "set_config") {
    json merged = config().toJson();
    merged.update(req.value("config", json::object()), true);
    RecordConfig c = RecordConfig::fromJson(merged);
    setConfig(c);
    return {{"ok", true}, {"config", c.toJson()}};
  }
  if (cmd == "start") {
    json merged = config().toJson();
    merged.update(req.value("config", json::object()), true);
    RecordConfig c = RecordConfig::fromJson(merged);
    std::string e = resolveTarget(c, req);
    if (!e.empty()) return err(e);
    setConfig(c);
    if (!rec_.start(c, e)) return err(e);
    return {{"ok", true}, {"state", stateName(rec_.state())}, {"config", c.toJson()}};
  }
  if (cmd == "pause") return rec_.pause() ? json{{"ok", true}, {"state", "paused"}} : err("not recording");
  if (cmd == "resume") return rec_.resume() ? json{{"ok", true}, {"state", "recording"}} : err("not paused");
  if (cmd == "stop") {
    if (rec_.state() == RecState::Idle) return err("not recording");
    RecResult r = rec_.stop();
    json j = r.toJson();
    j["ok"] = r.error.empty();
    j["metrics"] = Telemetry::get().snapshot();
    return j;
  }
  if (cmd == "share") return shareStart(req.value("path", std::string()), req.value("targetMB", 0));
  if (cmd == "share_status") return {{"ok", true}, {"share", shareStatus()}};
  if (cmd == "shutdown") {
    if (onShutdown) onShutdown();
    return {{"ok", true}};
  }
  if (extra) {
    json r = extra(req);
    if (!r.is_null()) return r;
  }
  return err("unknown command: " + cmd);
}

AppController::~AppController() {
  shareCancel_ = true;
  if (shareThread_.joinable()) shareThread_.join();
}

json AppController::shareStatus() {
  std::lock_guard lk(shareMu_);
  return {{"state", shareState_}, {"progress", shareProgress_}, {"path", sharePath_},
          {"bytes", shareBytes_},   {"error", shareError_}};
}

json AppController::shareStart(const std::string& path, int targetMB) {
  if (path.empty() || targetMB <= 0) return err("share needs a path and a positive targetMB");
  {
    std::lock_guard lk(shareMu_);
    if (shareState_ == "running") return err("a share copy is already running");
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return err("no such file: " + path);
    shareState_ = "running";
    shareProgress_ = 0;
    sharePath_.clear();
    shareError_.clear();
    shareBytes_ = 0;
  }
  if (shareThread_.joinable()) shareThread_.join();
  shareCancel_ = false;
  shareThread_ = std::thread([this, path, targetMB] {
    ShareResult r = makeShareCopy(
        path, targetMB,
        [this](double p) {
          std::lock_guard lk(shareMu_);
          shareProgress_ = p;
        },
        &shareCancel_);
    std::lock_guard lk(shareMu_);
    if (r.error.empty()) {
      shareState_ = "done";
      sharePath_ = r.path;
      shareBytes_ = r.bytes;
      shareProgress_ = 1;
    } else {
      shareState_ = "error";
      shareError_ = r.error;
    }
  });
  return {{"ok", true}, {"share", shareStatus()}};
}

}  // namespace vv
