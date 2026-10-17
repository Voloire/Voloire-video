#include "tray.h"
#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFileInfo>
#include <QMimeData>
#include <QTimer>
#include <QUrl>
#include <cmath>
#include "panel.h"

namespace vv::ui {

TrayController::TrayController(MainPanel* panel, QObject* parent) : QObject(parent), panel_(panel) {
  hooks_.showPanel = [this] { showPanel(); };
  tray_.setContextMenu(&menu_);
  connect(&menu_, &QMenu::aboutToShow, this, [this] { rebuildMenu(); });
  connect(&tray_, &QSystemTrayIcon::activated, this, &TrayController::onActivated);
  connect(panel_, &MainPanel::changed, this, &TrayController::refresh);
  tick_ = new QTimer(this);
  connect(tick_, &QTimer::timeout, this, [this] {
    pulseHigh_ = !pulseHigh_;
    checkShare();
    updateIcon();
    if (header_) header_->setText(QString("%1   %2").arg(QString(vv::stateName(panel_->recState())).toUpper(), panel_->timerText()));
  });
  tick_->start(500);
  rebuildMenu();
  updateIcon();
  tray_.show();
}

QString TrayController::tooltip() const {
  vv::RecordConfig c = panel_->currentConfig();
  QString state = QString(vv::stateName(panel_->recState())).toUpper();
  if (panel_->recState() == vv::RecState::Idle && panel_->starting()) state = "STARTING";
  QString res = c.outHeight ? QString("%1p").arg(c.outHeight) : QString("source size");
  QString t = QString("Voloire-video | %1 %2 | %3 %4 fps %5 | SYS %6 | MIC %7")
                  .arg(state, panel_->timerText(), res)
                  .arg(c.fps)
                  .arg(QString::fromStdString(c.codec))
                  .arg(QString::fromStdString(c.sys), QString::fromStdString(c.mic));
  if (!panel_->errorText().isEmpty()) t += " | ERROR: " + panel_->errorText();
  vv::json sh = panel_->shareStatus();
  if (sh.value("state", std::string("idle")) == "running")
    t += QString(" | SHARE %1%").arg((int)std::lround(sh.value("progress", 0.0) * 100));
  return t;
}

void TrayController::updateIcon() {
  IconState s = IconState::Idle;
  switch (panel_->recState()) {
    case vv::RecState::Recording: s = IconState::Recording; break;
    case vv::RecState::Paused: s = IconState::Paused; break;
    default: s = panel_->errorText().isEmpty() ? IconState::Idle : IconState::Error; break;
  }
  iconState_ = s;
  tray_.setIcon(trayIcon(s, pulseHigh_ ? 1.0 : 0.0));
  tray_.setToolTip(tooltip());
}

void TrayController::refresh() {
  checkShare();
  updateIcon();
  if (menu_.isVisible()) return;  // never rebuild under the user's cursor
  rebuildMenu();
}

// Balloon when the copy is ready, error balloon when it is not, and the file on the clipboard
// so a single Ctrl+V pastes it into a chat window or Explorer.
void TrayController::checkShare() {
  vv::json sh = panel_->shareStatus();
  std::string st = sh.value("state", std::string("idle"));
  if (st == shareSeen_) return;
  shareSeen_ = st;
  if (st == "done") {
    QString path = QString::fromStdString(sh.value("path", std::string()));
    double mb = sh.value("bytes", 0.0) / 1048576.0;
    if (panel_->shareToClipboard() && !path.isEmpty()) {
      auto* md = new QMimeData();
      md->setUrls({QUrl::fromLocalFile(path)});
      QApplication::clipboard()->setMimeData(md);
    }
    tray_.showMessage("Voloire-video",
                      QString("Share copy ready: %1 (%2 MB)").arg(QFileInfo(path).fileName()).arg(mb, 0, 'f', 1));
  } else if (st == "error") {
    tray_.showMessage("Voloire-video", QString::fromStdString(sh.value("error", std::string())));
  }
}

void TrayController::showPanel() {
  panel_->show();
  panel_->raise();
  panel_->activateWindow();
}

void TrayController::onActivated(QSystemTrayIcon::ActivationReason r) {
  if (r == QSystemTrayIcon::Trigger || r == QSystemTrayIcon::DoubleClick) {
    if (panel_->isVisible()) panel_->hide(); else showPanel();
  }
}

void TrayController::rebuildMenu() {
  menu_.clear();
  using RS = vv::RecState;
  RS st = panel_->recState();

  header_ = menu_.addAction(QString("%1   %2").arg(QString(vv::stateName(st)).toUpper(), panel_->timerText()));
  header_->setEnabled(false);
  menu_.addSeparator();

  buildControlMenu(menu_, *panel_, hooks_);
}

vv::json TrayController::state() {
  rebuildMenu();
  static const char* names[] = {"idle", "recording", "paused", "error"};
  vv::json clip = vv::json::array();
  if (const QMimeData* md = QApplication::clipboard()->mimeData())
    if (md->hasUrls())
      for (const QUrl& u : md->urls())
        if (u.isLocalFile()) clip.push_back(QDir::toNativeSeparators(u.toLocalFile()).toStdString());
  return {{"icon", names[(int)iconState_]},
          {"tooltip", tooltip().toStdString()},
          {"trayVisible", tray_.isVisible()},
          {"panelVisible", panel_->isVisible()},
          {"state", vv::stateName(panel_->recState())},
          {"timer", panel_->timerText().toStdString()},
          {"error", panel_->errorText().toStdString()},
          {"share", panel_->shareStatus()},
          {"clipboardFiles", clip},
          {"menu", dumpMenu(&menu_)}};
}

bool TrayController::trigger(const std::vector<std::string>& path) {
  rebuildMenu();
  return triggerMenu(menu_, path);
}

}  // namespace vv::ui
