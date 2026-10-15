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
          {"outHeight", outHeight},     {"fps", fps},       {"bitrateKbps", bitrateKbps},
          {"outDir", outDir},           {"name", name},     {"synthSysHz", synthSysHz},
          {"synthMicHz", synthMicHz},   {"synthFps", synthFps},
          {"telemetryDir", telemetryDir}};
}

RecordConfig RecordConfig::fromJson(const json& j) {
  RecordConfig c;
  get(j, "videoSource", c.videoSource); get(j, "hwnd", c.hwnd);   get(j, "pid", c.pid);
  get(j, "monitor", c.monitor);         get(j, "sys", c.sys);     get(j, "sysDevice", c.sysDevice);
  get(j, "mic", c.mic);                 get(j, "micDevice", c.micDevice);
  get(j, "container", c.container);     get(j, "codec", c.codec); get(j, "encoder", c.encoder);
  get(j, "outHeight", c.outHeight);     get(j, "fps", c.fps);     get(j, "bitrateKbps", c.bitrateKbps);
  get(j, "outDir", c.outDir);           get(j, "name", c.name);   get(j, "synthSysHz", c.synthSysHz);
  get(j, "synthMicHz", c.synthMicHz);   get(j, "synthFps", c.synthFps);
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
  if (fps < 1 || fps > 240) return "fps out of range";
  if (bitrateKbps < 100 || bitrateKbps > 200000) return "bitrate out of range";
  if (outHeight < 0 || outHeight > 4320) return "outHeight out of range";
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

}  // namespace vv
