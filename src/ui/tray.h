#pragma once
#include <QMenu>
#include <QObject>
#include <QSystemTrayIcon>
#include "common.h"
#include "theme.h"

namespace vv::ui {

class MainPanel;

// The menu-bar icon: state at a glance (ring + dot, pulsing while recording), timer in the
// tooltip and menu header, and every setting reachable from its menu.
class TrayController : public QObject {
  Q_OBJECT
 public:
  TrayController(MainPanel* panel, QObject* parent = nullptr);

  void rebuildMenu();
  IconState iconState() const { return iconState_; }
  QString tooltip() const;
  QMenu* menu() { return &menu_; }
  bool visible() const { return tray_.isVisible(); }

  vv::json state();                                   // test hook: icon, tooltip, whole menu tree
  bool trigger(const std::vector<std::string>& path);  // test hook: click a menu entry by text path

 private slots:
  void refresh();
  void onActivated(QSystemTrayIcon::ActivationReason r);

 private:
  void updateIcon();
  void showPanel();

  MainPanel* panel_;
  QSystemTrayIcon tray_;
  QMenu menu_;
  QAction* header_ = nullptr;
  QTimer* tick_;
  IconState iconState_ = IconState::Idle;
  bool pulseHigh_ = true;
};

}  // namespace vv::ui
