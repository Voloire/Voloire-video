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
  int outHeight = 0;              // 0 keeps the source size
  int fps = 60;
  int bitrateKbps = 12000;
  std::string outDir;
  std::string name = "recording";
  std::string telemetryDir;  // empty: <outDir>/telemetry/<base name>

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

}  // namespace vv
