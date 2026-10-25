#include "app_controller.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include "devices.h"
#include "http_client.h"
#include "secrets.h"
#include "telemetry.h"
#include "twitch.h"

namespace vv {

namespace {
std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
  return s;
}
json err(const std::string& m) { return {{"ok", false}, {"error", m}}; }
std::string streamSecretName(const std::string& ch) {
  return (ch.empty() || ch == "default") ? "stream_key" : ("stream_key_" + ch);
}
}  // namespace

AppController::AppController() {
  cfg_.outDir = defaultOutDir();
  // The share links follow the stream: begin when it is live, finish when the recorder is idle again.
  rec_.onState = [this](RecState s) {
    if (s == RecState::Recording) {
      RecordConfig c = config();
      if (c.output != "stream") return;
      StreamChannel ch;
      for (const auto& x : loadUiPrefs().channels)
        if (x.id == c.streamChannel) ch = x;
      links_.begin(ch, twitchClientId());
    } else if (s == RecState::Idle) {
      links_.streamEnded();
    }
  };
}

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
    s["links"] = links_.status().toJson();
    return s;
  }
  if (cmd == "metrics") return {{"ok", true}, {"metrics", Telemetry::get().snapshot()}};
  if (cmd == "logs") return {{"ok", true}, {"logs", Telemetry::get().tailLogs(req.value("n", 100))}};
  if (cmd == "get_config") {
    json c = config().toJson();
    std::string ch = req.value("channel", c.value("streamChannel", std::string("default")));
    c["streamKeySet"] = !loadSecret(streamSecretName(ch)).empty();  // the key itself never leaves the machine
    return {{"ok", true}, {"config", c}};
  }
  if (cmd == "set_stream_key") {
    std::string k = req.value("key", std::string());
    std::string ch = req.value("channel", config().streamChannel);
    std::string sec = streamSecretName(ch);
    if (k.empty()) deleteSecret(sec);
    else if (!saveSecret(sec, k)) return err("cannot store the stream key");
    return {{"ok", true}, {"streamKeySet", !k.empty()}, {"channel", ch}};
  }
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
  if (cmd == "links") return {{"ok", true}, {"links", links_.status().toJson()}};
  if (cmd.rfind("twitch_", 0) == 0) return handleTwitch(cmd, req);
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
  rec_.onState = nullptr;
  shareCancel_ = true;
  twitchCancel_ = true;
  if (shareThread_.joinable()) shareThread_.join();
  if (twitchThread_.joinable()) twitchThread_.join();
}

// ---------------------------------------------------------------- Twitch commands

std::string AppController::twitchClientId() const { return loadUiPrefs().twitchClientId; }

StreamChannel AppController::twitchProfile(const std::string& channelId) const {
  UiPrefs p = loadUiPrefs();
  for (const auto& c : p.channels)
    if (c.platform == "twitch" && (channelId.empty() || c.id == channelId)) return c;
  return StreamChannel{};  // platform "youtube": no Twitch profile
}

void AppController::upsertTwitchProfile(const TwitchUser& u) {
  UiPrefs p = loadUiPrefs();
  const std::string id = "tw_" + u.id;
  StreamChannel* ch = nullptr;
  for (auto& c : p.channels) if (c.id == id) ch = &c;
  if (!ch) {
    p.channels.push_back(StreamChannel{});
    ch = &p.channels.back();
    ch->id = id;
    ch->platform = "twitch";
    ch->streamUrl = "rtmp://live.twitch.tv/app";
  }
  ch->label = u.displayName;
  ch->twitchLogin = u.login;
  ch->twitchUserId = u.id;
  ch->twitchDisplayName = u.displayName;
  ch->studioUrl = "https://dashboard.twitch.tv/u/" + u.login + "/stream-manager";
  saveUiPrefs(p);
  if (onPrefsChanged) onPrefsChanged();
}

// The Twitch profile a command acts on; when none exists yet but a session does, it is created.
bool AppController::resolveTwitch(TwitchClient& t, const std::string& channelId, StreamChannel& out, std::string& err) {
  out = twitchProfile(channelId);
  if (out.platform == "twitch" && !out.twitchUserId.empty()) return true;
  if (!t.signedIn()) { err = "Twitch: not connected, sign in from the Twitch tab"; return false; }
  TwitchUser u;
  if (!t.me(u, err)) return false;
  upsertTwitchProfile(u);
  out = twitchProfile("tw_" + u.id);
  return true;
}

void AppController::twitchAuthLoop(TwitchDeviceCode code) {
  TwitchClient t(twitchClientId());
  const std::string pollMs = envOr(L"VV_LINK_POLL_MS", "");
  int interval = std::max(1, code.interval);
  if (!pollMs.empty() && std::atoi(pollMs.c_str()) < 5000) interval = 1;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(std::max(30, code.expiresIn));
  auto finish = [this](const char* state, const std::string& error, const std::string& login = {}) {
    std::lock_guard lk(twitchMu_);
    twitchAuth_ = {{"state", state}, {"error", error}, {"login", login}};
  };
  while (!twitchCancel_) {
    for (int i = 0; i < interval * 10 && !twitchCancel_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (twitchCancel_) return;
    if (std::chrono::steady_clock::now() > deadline) { finish("error", "Twitch: the code expired, start the sign-in again"); return; }
    std::string err;
    std::string r = t.pollDeviceToken(code.deviceCode, err);
    if (r == "pending") continue;
    if (r == "error") { finish("error", err); return; }
    TwitchUser u;
    if (!t.me(u, err)) { t.signOut(); finish("error", err); return; }
    upsertTwitchProfile(u);
    finish("ok", {}, u.login);
    return;
  }
}

json AppController::handleTwitch(const std::string& cmd, const json& req) {
  TwitchClient t(twitchClientId());
  std::string e;
  if (cmd == "twitch_status") {
    StreamChannel p = twitchProfile();
    json auth;
    {
      std::lock_guard lk(twitchMu_);
      auth = twitchAuth_;
    }
    return {{"ok", true},
            {"clientIdSet", !twitchClientId().empty()},
            {"signedIn", t.signedIn()},
            {"login", p.twitchLogin},
            {"userId", p.twitchUserId},
            {"displayName", p.twitchDisplayName},
            {"channel", p.platform == "twitch" ? p.id : std::string()},
            {"auth", auth}};
  }
  if (cmd == "twitch_auth_start") {
    std::lock_guard authOp(twitchAuthOpMu_);
    std::string cid = req.value("clientId", std::string());
    if (!cid.empty()) {
      UiPrefs p = loadUiPrefs();
      p.twitchClientId = cid;
      saveUiPrefs(p);
      if (onPrefsChanged) onPrefsChanged();
    }
    TwitchClient t2(twitchClientId());
    TwitchDeviceCode code;
    if (!t2.startDeviceAuth(code, e)) return err(e);
    twitchCancel_ = true;
    if (twitchThread_.joinable()) twitchThread_.join();
    twitchCancel_ = false;
    {
      std::lock_guard lk(twitchMu_);
      twitchAuth_ = {{"state", "pending"}, {"userCode", code.userCode}, {"verificationUri", code.verificationUri}, {"error", ""}};
    }
    twitchThread_ = std::thread([this, code] { twitchAuthLoop(code); });
    return {{"ok", true}, {"userCode", code.userCode}, {"verificationUri", code.verificationUri}, {"expiresIn", code.expiresIn}};
  }
  if (cmd == "twitch_auth_cancel") {
    std::lock_guard authOp(twitchAuthOpMu_);
    twitchCancel_ = true;
    if (twitchThread_.joinable()) twitchThread_.join();
    std::lock_guard lk(twitchMu_);
    twitchAuth_ = {{"state", "idle"}};
    return {{"ok", true}};
  }
  if (cmd == "twitch_sign_out") {
    std::lock_guard authOp(twitchAuthOpMu_);
    // Stop a pending device-code flow before deleting tokens, including an in-flight response.
    twitchCancel_ = true;
    if (twitchThread_.joinable()) twitchThread_.join();
    t.signOut();
    std::lock_guard lk(twitchMu_);
    twitchAuth_ = {{"state", "idle"}};
    return {{"ok", true}};
  }
  if (cmd == "twitch_ingests") {
    std::vector<TwitchIngest> v;
    if (!t.ingests(v, e)) return err(e);
    json a = json::array();
    for (const auto& i : v) a.push_back({{"name", i.name}, {"url", i.url}});
    return {{"ok", true}, {"ingests", a}};
  }
  if (cmd == "twitch_search_categories") {
    std::vector<TwitchCategory> v;
    if (!t.searchCategories(req.value("query", std::string()), v, e)) return err(e);
    json a = json::array();
    for (const auto& c : v) a.push_back({{"id", c.id}, {"name", c.name}});
    return {{"ok", true}, {"categories", a}};
  }

  StreamChannel p;
  if (!resolveTwitch(t, req.value("channel", std::string()), p, e)) return err(e);
  if (cmd == "twitch_fetch_key") {
    std::string key;
    if (!t.streamKey(p.twitchUserId, key, e)) return err(e);
    if (!saveSecret(streamSecretName(p.id), key)) return err("cannot store the stream key");
    return {{"ok", true}, {"streamKeySet", true}, {"channel", p.id}};
  }
  if (cmd == "twitch_get_info") {
    TwitchChannelInfo i;
    if (!t.channelInfo(p.twitchUserId, i, e)) return err(e);
    return {{"ok", true}, {"title", i.title}, {"categoryId", i.categoryId}, {"categoryName", i.categoryName},
            {"language", i.language}, {"tags", i.tags}};
  }
  if (cmd == "twitch_set_info") {
    TwitchChannelInfo i;
    i.title = req.value("title", std::string());
    i.categoryId = req.value("categoryId", std::string());
    i.categoryName = req.value("categoryName", std::string());
    i.language = req.value("language", std::string("it"));
    if (req.contains("tags") && req["tags"].is_array())
      for (const auto& x : req["tags"]) if (x.is_string()) i.tags.push_back(x.get<std::string>());
    if (!t.modifyChannel(p.twitchUserId, i, e)) return err(e);
    UiPrefs prefs = loadUiPrefs();
    for (auto& c : prefs.channels) {
      if (c.id != p.id) continue;
      c.title = i.title;
      if (c.categoryId != i.categoryId || !i.categoryName.empty()) c.categoryName = i.categoryName;
      c.categoryId = i.categoryId;
      c.language = i.language;
      c.tags = i.tags;
    }
    saveUiPrefs(prefs);
    if (onPrefsChanged) onPrefsChanged();
    return {{"ok", true}};
  }
  return err("unknown command: " + cmd);
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
