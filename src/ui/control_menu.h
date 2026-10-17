#pragma once
#include <QMenu>
#include <functional>
#include "common.h"

namespace vv::ui {

class MainPanel;

// What the shared menu needs from its owner: both the tray icon and the status bar build the
// very same menu from here and only differ in these callbacks.
struct ControlMenuHooks {
  std::function<void()> showPanel;
  std::function<void(bool)> setHudVisible;
  std::function<bool()> hudVisible;
};

// Appends the recording controls and every setting entry to `menu` (callers clear first).
void buildControlMenu(QMenu& menu, MainPanel& panel, ControlMenuHooks hooks);

// Test hooks shared by the tray icon and the status bar.
vv::json dumpMenu(QMenu* m);
bool triggerMenu(QMenu& menu, const std::vector<std::string>& path);

}  // namespace vv::ui
