#pragma once
#include <QMenu>
#include <QPoint>
#include <QWidget>
#include "common.h"
#include "config.h"

class QTimer;

namespace vv::ui {

class MainPanel;

// The always-on-top status pill: dot, state label and timer, with the shared control menu on
// right click. Position and visibility live in ui.json.
class HudBar : public QWidget {
  Q_OBJECT
 public:
  explicit HudBar(MainPanel* panel, vv::UiPrefs* prefs);
  ~HudBar() override;

  vv::json state() const;  // test hook: physical px geometry, label, dot, capture exclusion, menu
  QMenu* menu() { return &menu_; }
  bool trigger(const std::vector<std::string>& path);  // test hook: click a menu entry by text path
  void setShown(bool on);                              // remember the choice, then show or hide
  void moveTo(int x, int y);                           // move, remembering the spot
  void setTestPattern(bool on);                        // fills the pill with pure #FF00FF
  void setExcludeFromCapture(bool on);                 // WDA_EXCLUDEFROMCAPTURE on every show
  std::string dotName() const;

 protected:
  void paintEvent(QPaintEvent*) override;
  void showEvent(QShowEvent*) override;
  void mousePressEvent(QMouseEvent*) override;
  void mouseMoveEvent(QMouseEvent*) override;
  void mouseReleaseEvent(QMouseEvent*) override;

 private:
  void refreshContent();
  void rebuildMenu();
  void applyExclusion();
  void restorePosition();

  MainPanel* panel_;
  vv::UiPrefs* prefs_;
  QMenu menu_;
  QTimer* tick_;
  bool pulse_ = true;
  bool testPattern_ = false;
  bool exclude_ = true;
  bool excluded_ = false;  // last SetWindowDisplayAffinity result
  bool dragging_ = false, moved_ = false;
  QPoint dragFrom_;
  QString label_;
};

}  // namespace vv::ui
