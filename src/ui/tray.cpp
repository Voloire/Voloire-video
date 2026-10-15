#include "tray.h"
#include <QActionGroup>
#include <QApplication>
#include <QDesktopServices>
#include <QTimer>
#include <QUrl>
#include "panel.h"

namespace vv::ui {

namespace {

QString clean(QString s) { return s.remove('&'); }

vv::json dumpMenu(QMenu* m) {
  vv::json arr = vv::json::array();
  for (QAction* a : m->actions()) {
    if (a->isSeparator()) { arr.push_back({{"separator", true}}); continue; }
    vv::json j = {{"text", clean(a->text()).toStdString()}, {"enabled", a->isEnabled()}};
    if (a->isCheckable()) j["checked"] = a->isChecked();
    if (a->menu()) j["menu"] = dumpMenu(a->menu());
    arr.push_back(j);
  }
  return arr;
}

QAction* findAction(QMenu* m, const QString& text) {
  for (QAction* a : m->actions())
    if (!a->isSeparator() && clean(a->text()) == text) return a;
  return nullptr;
}

}  // namespace

TrayController::TrayController(MainPanel* panel, QObject* parent) : QObject(parent), panel_(panel) {
  tray_.setContextMenu(&menu_);
  connect(&menu_, &QMenu::aboutToShow, this, [this] { rebuildMenu(); });
  connect(&tray_, &QSystemTrayIcon::activated, this, &TrayController::onActivated);
  connect(panel_, &MainPanel::changed, this, &TrayController::refresh);
  tick_ = new QTimer(this);
  connect(tick_, &QTimer::timeout, this, [this] {
    pulseHigh_ = !pulseHigh_;
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
  updateIcon();
  if (menu_.isVisible()) return;  // never rebuild under the user's cursor
  rebuildMenu();
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

  QAction* startStop = menu_.addAction(st == RS::Idle ? "Start recording" : "Stop recording");
  startStop->setEnabled(!panel_->starting());
  connect(startStop, &QAction::triggered, this, [this] {
    if (panel_->recState() == vv::RecState::Idle) panel_->startRecording(); else panel_->stopRecording();
  });
  QAction* pause = menu_.addAction(st == RS::Paused ? "Resume" : "Pause");
  pause->setEnabled(st == RS::Recording || st == RS::Paused);
  connect(pause, &QAction::triggered, this, [this] { panel_->togglePause(); });
  menu_.addSeparator();

  const bool idle = st == RS::Idle && !panel_->starting();

  // Source
  QMenu* src = menu_.addMenu("Source");
  src->setEnabled(idle);
  {
    auto* grp = new QActionGroup(src);
    auto add = [&](const QString& text, bool checked, std::function<void()> fn) {
      QAction* a = src->addAction(text);
      a->setCheckable(true);
      a->setChecked(checked);
      grp->addAction(a);
      connect(a, &QAction::triggered, this, [fn] { fn(); });
    };
    const bool winMode = panel_->videoKind() == "window";
    add("Active window at record time", winMode && panel_->selectedHwnd() == 0, [this] { panel_->selectWindow(0); });
    for (auto& w : panel_->windows())
      add(QString::fromStdString(w.title + "  [" + w.exe + "]").left(70), winMode && panel_->selectedHwnd() == w.hwnd,
          [this, h = w.hwnd] { panel_->selectWindow(h); });
    src->addSeparator();
    for (auto& m : panel_->monitors())
      add(QString("Monitor %1 (%2x%3)").arg(m.index + 1).arg(m.w).arg(m.h),
          panel_->videoKind() == "monitor" && panel_->monitorIndex() == m.index,
          [this, i = m.index] { panel_->setVideoSource("monitor", i); });
    for (auto& m : panel_->monitors())
      add(QString("Monitor %1, exclusive fullscreen mode").arg(m.index + 1),
          panel_->videoKind() == "dxgi" && panel_->monitorIndex() == m.index,
          [this, i = m.index] { panel_->setVideoSource("dxgi", i); });
    src->addSeparator();
    QAction* r = src->addAction("Refresh lists");
    connect(r, &QAction::triggered, this, [this] { panel_->refreshSources(); });
  }

  // Format
  QMenu* fmt = menu_.addMenu("Format");
  fmt->setEnabled(idle);
  {
    auto radio = [&](QMenu* sub, const QString& text, bool checked, std::function<void()> fn) {
      QAction* a = sub->addAction(text);
      a->setCheckable(true);
      a->setChecked(checked);
      if (!sub->property("grp").isValid()) sub->setProperty("grp", true);
      connect(a, &QAction::triggered, this, [fn] { fn(); });
      return a;
    };
    vv::RecordConfig c = panel_->currentConfig();
    QMenu* cont = fmt->addMenu("Container");
    auto* g1 = new QActionGroup(cont);
    for (auto tv : {std::pair{"MKV (crash safe)", "mkv"}, std::pair{"MP4", "mp4"}}) {
      QString v = tv.second;
      g1->addAction(radio(cont, tv.first, c.container == tv.second, [this, v] { panel_->setContainer(v); }));
    }
    QMenu* codec = fmt->addMenu("Codec");
    auto* g2 = new QActionGroup(codec);
    for (auto tv : {std::pair{"H.264", "h264"}, std::pair{"HEVC (H.265)", "hevc"}, std::pair{"AV1", "av1"}}) {
      QString v = tv.second;
      g2->addAction(radio(codec, tv.first, c.codec == tv.second, [this, v] { panel_->setCodec(v); }));
    }
    QMenu* res = fmt->addMenu("Resolution");
    auto* g3 = new QActionGroup(res);
    g3->addAction(radio(res, "Source size", c.outHeight == 0, [this] { panel_->setHeight(0); }));
    for (int h : {2160, 1440, 1080, 720, 480})
      g3->addAction(radio(res, QString("%1p").arg(h), c.outHeight == h, [this, h] { panel_->setHeight(h); }));
    QMenu* fps = fmt->addMenu("Frame rate");
    auto* g4 = new QActionGroup(fps);
    for (int f : {24, 30, 60, 120})
      g4->addAction(radio(fps, QString("%1 fps").arg(f), c.fps == f, [this, f] { panel_->setFps(f); }));
  }

  // Audio devices
  auto audioMenu = [&](const QString& title, bool sys) {
    QMenu* m = menu_.addMenu(title);
    m->setEnabled(idle);
    auto* grp = new QActionGroup(m);
    QString key = sys ? panel_->sysKey() : panel_->micKey();
    auto add = [&](const QString& text, const QString& mode, const QString& id) {
      QAction* a = m->addAction(text);
      a->setCheckable(true);
      a->setChecked(key == mode + "|" + id);
      grp->addAction(a);
      connect(a, &QAction::triggered, this, [this, sys, mode, id] { if (sys) panel_->setSys(mode, id); else panel_->setMic(mode, id); });
    };
    if (sys) add("App audio of the captured window", "app", "");
    else add("Default microphone", "default", "");
    for (auto& d : panel_->devices())
      if (d.capture != sys)
        add(QString::fromStdString(sys ? "System output: " + d.name : d.name), sys ? "system" : "device", QString::fromStdString(d.id));
    add("Off", "none", "");
  };
  audioMenu("MIC", false);
  audioMenu("SYS", true);

  menu_.addSeparator();
  connect(menu_.addAction("Open panel"), &QAction::triggered, this, [this] { showPanel(); });
  connect(menu_.addAction("Open recordings folder"), &QAction::triggered, this, [this] {
    QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(panel_->currentConfig().outDir)));
  });
  menu_.addSeparator();
  connect(menu_.addAction("Quit"), &QAction::triggered, this, [this] {
    if (panel_->recState() != vv::RecState::Idle) panel_->stopRecording();
    QTimer::singleShot(200, [this] {
      if (panel_->recState() != vv::RecState::Idle) { QTimer::singleShot(300, qApp, &QApplication::quit); return; }
      qApp->quit();
    });
  });
}

vv::json TrayController::state() {
  rebuildMenu();
  static const char* names[] = {"idle", "recording", "paused", "error"};
  return {{"icon", names[(int)iconState_]},
          {"tooltip", tooltip().toStdString()},
          {"trayVisible", tray_.isVisible()},
          {"panelVisible", panel_->isVisible()},
          {"state", vv::stateName(panel_->recState())},
          {"timer", panel_->timerText().toStdString()},
          {"error", panel_->errorText().toStdString()},
          {"menu", dumpMenu(&menu_)}};
}

bool TrayController::trigger(const std::vector<std::string>& path) {
  rebuildMenu();
  QMenu* m = &menu_;
  QAction* a = nullptr;
  for (auto& p : path) {
    a = findAction(m, QString::fromStdString(p));
    if (!a) return false;
    if (a->menu()) m = a->menu();
  }
  if (!a || !a->isEnabled()) return false;
  a->trigger();
  return true;
}

}  // namespace vv::ui
