#include "config.h"
#include <windows.h>
#include <shlobj.h>
#include <filesystem>
#include <fstream>

namespace vv {

namespace {
template <class T>
void get(const json& j, const char* key, T& out) {
  auto it = j.find(key);
  if (it != j.end() && !it->is_null()) {
    try { out = it->get<T>(); } catch (...) {}
  }
}

std::string knownFolder(REFKNOWNFOLDERID id) {
  PWSTR p = nullptr;
  std::string r;
  if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) r = utf8(p);
  CoTaskMemFree(p);
  return r;
}
}  // namespace

json RecordConfig::toJson() const {
  return {{"videoSource", videoSource}, {"hwnd", hwnd},     {"pid", pid},
          {"monitor", monitor},         {"sys", sys},       {"sysDevice", sysDevice},
          {"mic", mic},                 {"micDevice", micDevice},
          {"container", container},     {"codec", codec},   {"encoder", encoder},
          {"quality", quality},
          {"outHeight", outHeight},     {"fps", fps},       {"bitrateKbps", bitrateKbps},
          {"outDir", outDir},           {"name", name},     {"synthSysHz", synthSysHz},
          {"synthMicHz", synthMicHz},   {"synthFps", synthFps},
          {"output", output},           {"streamChannel", streamChannel},
          {"streamUrl", streamUrl},     {"streamBitrateKbps", streamBitrateKbps},
          {"streamHeight", streamHeight}, {"recordWhileStreaming", recordWhileStreaming},
          {"sysVolumePct", sysVolumePct}, {"micVolumePct", micVolumePct},
          {"sysMuted", sysMuted},       {"micMuted", micMuted},
          {"telemetryDir", telemetryDir}};
}

RecordConfig RecordConfig::fromJson(const json& j) {
  RecordConfig c;
  get(j, "videoSource", c.videoSource); get(j, "hwnd", c.hwnd);   get(j, "pid", c.pid);
  get(j, "monitor", c.monitor);         get(j, "sys", c.sys);     get(j, "sysDevice", c.sysDevice);
  get(j, "mic", c.mic);                 get(j, "micDevice", c.micDevice);
  get(j, "container", c.container);     get(j, "codec", c.codec); get(j, "encoder", c.encoder);
  get(j, "quality", c.quality);
  get(j, "outHeight", c.outHeight);     get(j, "fps", c.fps);     get(j, "bitrateKbps", c.bitrateKbps);
  get(j, "outDir", c.outDir);           get(j, "name", c.name);   get(j, "synthSysHz", c.synthSysHz);
  get(j, "synthMicHz", c.synthMicHz);   get(j, "synthFps", c.synthFps);
  get(j, "output", c.output);           get(j, "streamChannel", c.streamChannel);
  get(j, "streamUrl", c.streamUrl);
  get(j, "streamBitrateKbps", c.streamBitrateKbps); get(j, "streamHeight", c.streamHeight);
  get(j, "recordWhileStreaming", c.recordWhileStreaming);
  get(j, "sysVolumePct", c.sysVolumePct); get(j, "micVolumePct", c.micVolumePct);
  get(j, "sysMuted", c.sysMuted);       get(j, "micMuted", c.micMuted);
  get(j, "telemetryDir", c.telemetryDir);
  return c;
}

std::string RecordConfig::validate() const {
  auto in = [](const std::string& v, std::initializer_list<const char*> opts) {
    for (auto o : opts) if (v == o) return true;
    return false;
  };
  if (!in(videoSource, {"window", "monitor", "dxgi", "synthetic"})) return "videoSource must be window|monitor|dxgi|synthetic";
  if (videoSource == "window" && hwnd == 0) return "no window selected";
  if (!in(sys, {"app", "system", "none", "synthetic"})) return "sys must be app|system|none|synthetic";
  if (sys == "app" && pid == 0) return "SYS app mode needs a target process";
  if (!in(mic, {"default", "device", "none", "synthetic"})) return "mic must be default|device|none|synthetic";
  if (!in(container, {"mkv", "mp4"})) return "container must be mkv|mp4";
  if (!in(codec, {"h264", "hevc", "av1"})) return "codec must be h264|hevc|av1";
  if (!in(encoder, {"auto", "nvenc", "amf", "qsv", "mf"})) return "encoder must be auto|nvenc|amf|qsv|mf";
  if (!in(quality, {"high", "standard", "light", "minimal", "custom"})) return "quality must be high|standard|light|minimal|custom";
  if (fps < 1 || fps > 240) return "fps out of range";
  if (bitrateKbps < 100 || bitrateKbps > 200000) return "bitrate out of range";
  if (outHeight < 0 || outHeight > 4320) return "outHeight out of range";
  if (!in(output, {"file", "stream"})) return "output must be file|stream";
  if (output == "stream") {
    if (streamUrl.rfind("rtmp://", 0) != 0 && streamUrl.rfind("rtmps://", 0) != 0)
      return "stream URL must start with rtmp:// or rtmps://";
    if (streamBitrateKbps < 1000 || streamBitrateKbps > 51000) return "stream bitrate out of range (1000..51000)";
    if (streamHeight < 0 || streamHeight > 4320) return "stream height out of range";
  }
  return {};
}

std::string defaultOutDir() {
  std::string v = knownFolder(FOLDERID_Videos);
  if (v.empty()) v = ".";
  return strOf(pathOf(v) / "Voloire-video");
}

std::string settingsPath() {
  if (const wchar_t* o = _wgetenv(L"VV_SETTINGS_DIR"))  // tests point this at a scratch folder
    return strOf(pathOf(utf8(o)) / "config.json");
  std::string a = knownFolder(FOLDERID_RoamingAppData);
  return strOf(pathOf(a) / "Voloire-video" / "config.json");
}

RecordConfig loadSettings() {
  RecordConfig c;
  try {
    std::ifstream f(pathOf(settingsPath()));
    if (f) c = RecordConfig::fromJson(json::parse(f));
  } catch (...) {}
  // Window, process and synthetic choices never survive a restart.
  c.hwnd = 0;
  c.pid = 0;
  if (c.videoSource == "synthetic") c.videoSource = "window";
  if (c.sys == "synthetic") c.sys = "app";
  if (c.mic == "synthetic") c.mic = "default";
  if (c.outDir.empty()) c.outDir = defaultOutDir();
  return c;
}

void saveSettings(const RecordConfig& c) {
  try {
    auto p = pathOf(settingsPath());
    std::filesystem::create_directories(p.parent_path());
    std::ofstream(p) << c.toJson().dump(2);
  } catch (...) {}
}

namespace {
std::string uiPrefsPath() { return strOf(pathOf(settingsPath()).parent_path() / "ui.json"); }
}  // namespace

json StreamChannel::toJson() const {
  return {{"id", id},
          {"label", label},
          {"platform", platform},
          {"studioUrl", studioUrl},
          {"streamUrl", streamUrl},
          {"twitchLogin", twitchLogin},
          {"twitchUserId", twitchUserId},
          {"twitchDisplayName", twitchDisplayName},
          {"title", title},
          {"categoryId", categoryId},
          {"categoryName", categoryName},
          {"language", language},
          {"tags", tags},
          {"applyInfoOnGoLive", applyInfoOnGoLive}};
}

StreamChannel StreamChannel::fromJson(const json& j) {
  StreamChannel c;
  get(j, "id", c.id);
  get(j, "label", c.label);
  get(j, "platform", c.platform);
  if (c.platform != "twitch") c.platform = "youtube";
  get(j, "studioUrl", c.studioUrl);
  get(j, "streamUrl", c.streamUrl);
  get(j, "twitchLogin", c.twitchLogin);
  get(j, "twitchUserId", c.twitchUserId);
  get(j, "twitchDisplayName", c.twitchDisplayName);
  get(j, "title", c.title);
  get(j, "categoryId", c.categoryId);
  get(j, "categoryName", c.categoryName);
  get(j, "language", c.language);
  get(j, "tags", c.tags);
  get(j, "applyInfoOnGoLive", c.applyInfoOnGoLive);
  if (c.id.empty()) c.id = "default";
  if (c.label.empty()) c.label = "Main Channel";
  return c;
}

json UiPrefs::toJson() const {
  json ch = json::array();
  for (const auto& c : channels) ch.push_back(c.toJson());
  return {{"hud", hud}, {"hudX", hudX}, {"hudY", hudY},
          {"shareTargetMB", shareTargetMB}, {"shareToClipboard", shareToClipboard},
          {"darkTheme", darkTheme}, {"channels", ch}, {"twitchClientId", twitchClientId}};
}

UiPrefs UiPrefs::fromJson(const json& j) {
  UiPrefs p;
  get(j, "hud", p.hud);
  get(j, "hudX", p.hudX);
  get(j, "hudY", p.hudY);
  get(j, "shareTargetMB", p.shareTargetMB);
  get(j, "shareToClipboard", p.shareToClipboard);
  get(j, "darkTheme", p.darkTheme);
  get(j, "twitchClientId", p.twitchClientId);
  auto it = j.find("channels");
  if (it != j.end() && it->is_array()) {
    for (const auto& item : *it) p.channels.push_back(StreamChannel::fromJson(item));
  }
  if (p.channels.empty()) p.channels.push_back(StreamChannel{});
  return p;
}

UiPrefs loadUiPrefs() {
  UiPrefs p;
  try {
    std::ifstream f(pathOf(uiPrefsPath()));
    if (f) p = UiPrefs::fromJson(json::parse(f));
  } catch (...) {}
  if (p.channels.empty()) p.channels.push_back(StreamChannel{});
  return p;
}

void saveUiPrefs(const UiPrefs& p) {
  try {
    auto path = pathOf(uiPrefsPath());
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path) << p.toJson().dump(2);
  } catch (...) {}
}

}  // namespace vv
