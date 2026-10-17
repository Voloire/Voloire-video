#pragma once
#include <string>
#include "common.h"

namespace vv {

struct RecordConfig {
  // Video source: window | monitor | dxgi | synthetic
  std::string videoSource = "window";
  int64_t hwnd = 0;
  uint32_t pid = 0;   // process whose audio is captured in "app" mode
  int monitor = 0;

  // SYS channel: app | system | none | synthetic
  std::string sys = "app";
  std::string sysDevice;  // render endpoint id for "system"; empty = default output
  // MIC channel: default | device | none | synthetic
  std::string mic = "default";
  std::string micDevice;

  // Output
  std::string container = "mkv";  // mkv | mp4
  std::string codec = "h264";     // h264 | hevc | av1
  std::string encoder = "auto";   // auto | nvenc | amf | qsv | mf
  std::string quality = "standard";  // high | standard | light | minimal | custom
  int outHeight = 0;              // 0 keeps the source size
  int fps = 60;
  int bitrateKbps = 12000;
  std::string outDir;
  std::string name = "recording";
  std::string telemetryDir;  // empty: <outDir>/telemetry/<base name>

  // Output destination: a local file, or an RTMP stream (the key is NOT part of the config).
  std::string output = "file";   // file | stream
  std::string streamChannel = "default";  // selected YouTube channel profile id
  std::string streamUrl = "rtmp://a.rtmp.youtube.com/live2";  // ingest base, YouTube by default
  int streamBitrateKbps = 6000;  // 1000..51000
  int streamHeight = 1080;       // 0 keeps the source size
  bool recordWhileStreaming = false;  // save a local .mkv copy alongside the RTMP stream

  // Audio mix & mute (applied to both recording and streaming)
  int sysVolumePct = 100;  // 0..200%
  int micVolumePct = 100;  // 0..200%
  bool sysMuted = false;
  bool micMuted = false;

  // Synthetic sources (test harness)
  double synthSysHz = 440.0;
  double synthMicHz = 1000.0;
  int synthFps = 60;

  json toJson() const;
  static RecordConfig fromJson(const json& j);
  std::string validate() const;  // empty string when valid
};

std::string defaultOutDir();
std::string settingsPath();
RecordConfig loadSettings();
void saveSettings(const RecordConfig& c);

struct StreamChannel {
  std::string id = "default";
  std::string label = "Main Channel";
  std::string studioUrl = "https://studio.youtube.com/";
  std::string streamUrl = "rtmp://a.rtmp.youtube.com/live2";
  json toJson() const;
  static StreamChannel fromJson(const json& j);
};

// UI-only preferences, kept in ui.json beside config.json.
struct UiPrefs {
  bool hud = true;
  int hudX = -1, hudY = -1;   // -1: never placed, use the default position
  int shareTargetMB = 0;      // 0 = off (Task 5)
  bool shareToClipboard = true;
  bool darkTheme = false;     // false = light sage, true = dark charcoal/sage
  std::vector<StreamChannel> channels;
  json toJson() const;
  static UiPrefs fromJson(const json& j);
};
UiPrefs loadUiPrefs();
void saveUiPrefs(const UiPrefs& p);

}  // namespace vv
