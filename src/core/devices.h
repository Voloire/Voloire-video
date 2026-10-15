#pragma once
#include <string>
#include <vector>
#include "common.h"

namespace vv {

struct WindowInfo {
  int64_t hwnd = 0;
  uint32_t pid = 0;
  std::string title, exe;
  int w = 0, h = 0;
  bool fullscreen = false;  // covers its whole monitor
  bool foreground = false;
  json toJson() const;
};

struct AudioDevice {
  std::string id, name;
  bool capture = false;  // true: input (microphone), false: output
  bool isDefault = false;
  json toJson() const;
};

struct MonitorInfo {
  int index = 0;
  std::string name;
  int w = 0, h = 0;
  bool primary = false;
  json toJson() const;
};

std::vector<WindowInfo> listWindows();
WindowInfo foregroundWindowInfo();
std::vector<AudioDevice> listAudioDevices();
std::vector<MonitorInfo> listMonitors();

}  // namespace vv
