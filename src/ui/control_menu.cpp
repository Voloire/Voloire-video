#include "control_menu.h"
#include <QActionGroup>
#include <QApplication>
#include <QDesktopServices>
#include <QTimer>
#include <QUrl>
#include "panel.h"
#include "theme.h"

namespace vv::ui {

namespace {

QString clean(QString s) { return s.remove('&'); }

QAction* findAction(QMenu* m, const QString& text) {
  for (QAction* a : m->actions())
    if (!a->isSeparator() && clean(a->text()) == text) return a;
  return nullptr;
}

}  // namespace

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

bool triggerMenu(QMenu& menu, const std::vector<std::string>& path) {
  QMenu* m = &menu;
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

void buildControlMenu(QMenu& menu, MainPanel& panel, ControlMenuHooks hooks) {
  using RS = RecState;
  RS st = panel.recState();
  MainPanel* p = &panel;

  QAction* startStop = menu.addAction(st == RS::Idle ? "Start recording" : "Stop recording");
  startStop->setEnabled(!panel.starting());
  QObject::connect(startStop, &QAction::triggered, &menu, [p] {
    if (p->recState() == RS::Idle) p->startRecording(); else p->stopRecording();
  });
  QAction* pause = menu.addAction(st == RS::Paused ? "Resume" : "Pause");
  pause->setEnabled(st == RS::Recording || st == RS::Paused);
  QObject::connect(pause, &QAction::triggered, &menu, [p] { p->togglePause(); });
  menu.addSeparator();

  const bool idle = st == RS::Idle && !panel.starting();

  // Source
  QMenu* src = menu.addMenu("Source");
  src->setEnabled(idle);
  {
    auto* grp = new QActionGroup(src);
    auto add = [&](const QString& text, bool checked, std::function<void()> fn) {
      QAction* a = src->addAction(text);
      a->setCheckable(true);
      a->setChecked(checked);
      grp->addAction(a);
      QObject::connect(a, &QAction::triggered, &menu, [fn] { fn(); });
    };
    const bool winMode = panel.videoKind() == "window";
    add("Active window at record time", winMode && panel.selectedHwnd() == 0, [p] { p->selectWindow(0); });
    for (auto& w : panel.windows())
      add(QString::fromStdString(w.title + "  [" + w.exe + "]").left(70), winMode && panel.selectedHwnd() == w.hwnd,
          [p, h = w.hwnd] { p->selectWindow(h); });
    src->addSeparator();
    for (auto& m : panel.monitors())
      add(QString("Monitor %1 (%2x%3)").arg(m.index + 1).arg(m.w).arg(m.h),
          panel.videoKind() == "monitor" && panel.monitorIndex() == m.index,
          [p, i = m.index] { p->setVideoSource("monitor", i); });
    for (auto& m : panel.monitors())
      add(QString("Monitor %1, exclusive fullscreen mode").arg(m.index + 1),
          panel.videoKind() == "dxgi" && panel.monitorIndex() == m.index,
          [p, i = m.index] { p->setVideoSource("dxgi", i); });
    src->addSeparator();
    QAction* r = src->addAction("Refresh lists");
    QObject::connect(r, &QAction::triggered, &menu, [p] { p->refreshSources(); });
  }

  // Format
  QMenu* fmt = menu.addMenu("Format");
  fmt->setEnabled(idle);
  {
    auto radio = [&](QMenu* sub, const QString& text, bool checked, std::function<void()> fn) {
      QAction* a = sub->addAction(text);
      a->setCheckable(true);
      a->setChecked(checked);
      if (!sub->property("grp").isValid()) sub->setProperty("grp", true);
      QObject::connect(a, &QAction::triggered, &menu, [fn] { fn(); });
      return a;
    };
    vv::RecordConfig c = panel.currentConfig();
    QMenu* cont = fmt->addMenu("Container");
    auto* g1 = new QActionGroup(cont);
    for (auto tv : {std::pair{"MKV (crash safe)", "mkv"}, std::pair{"MP4", "mp4"}}) {
      QString v = tv.second;
      g1->addAction(radio(cont, tv.first, c.container == tv.second, [p, v] { p->setContainer(v); }));
    }
    QMenu* codec = fmt->addMenu("Codec");
    auto* g2 = new QActionGroup(codec);
    for (auto tv : {std::pair{"H.264", "h264"}, std::pair{"HEVC (H.265)", "hevc"}, std::pair{"AV1", "av1"}}) {
      QString v = tv.second;
      g2->addAction(radio(codec, tv.first, c.codec == tv.second, [p, v] { p->setCodec(v); }));
    }
    QMenu* res = fmt->addMenu("Resolution");
    auto* g3 = new QActionGroup(res);
    g3->addAction(radio(res, "Source size", c.outHeight == 0, [p] { p->setHeight(0); }));
    for (int h : {2160, 1440, 1080, 720, 480})
      g3->addAction(radio(res, QString("%1p").arg(h), c.outHeight == h, [p, h] { p->setHeight(h); }));
    QMenu* fps = fmt->addMenu("Frame rate");
    auto* g4 = new QActionGroup(fps);
    for (int f : {24, 30, 60, 120})
      g4->addAction(radio(fps, QString("%1 fps").arg(f), c.fps == f, [p, f] { p->setFps(f); }));
  }

  // Audio devices
  auto audioMenu = [&](const QString& title, bool sys) {
    QMenu* m = menu.addMenu(title);
    m->setEnabled(idle);
    auto* grp = new QActionGroup(m);
    QString key = sys ? panel.sysKey() : panel.micKey();
    auto add = [&](const QString& text, const QString& mode, const QString& id) {
      QAction* a = m->addAction(text);
      a->setCheckable(true);
      a->setChecked(key == mode + "|" + id);
      grp->addAction(a);
      QObject::connect(a, &QAction::triggered, &menu, [p, sys, mode, id] { if (sys) p->setSys(mode, id); else p->setMic(mode, id); });
    };
    if (sys) add("App audio of the captured window", "app", "");
    else add("Default microphone", "default", "");
    for (auto& d : panel.devices())
      if (d.capture != sys)
        add(QString::fromStdString(sys ? "System output: " + d.name : d.name), sys ? "system" : "device", QString::fromStdString(d.id));
    add("Off", "none", "");
  };
  audioMenu("MIC", false);
  audioMenu("SYS", true);

  menu.addSeparator();
  QAction* hud = menu.addAction("Show status bar");
  hud->setCheckable(true);
  hud->setChecked(hooks.hudVisible && hooks.hudVisible());
  QObject::connect(hud, &QAction::triggered, &menu, [set = hooks.setHudVisible](bool on) { if (set) set(on); });
  QObject::connect(menu.addAction("Open panel"), &QAction::triggered, &menu, [s = hooks.showPanel] { if (s) s(); });
  QObject::connect(menu.addAction("Open recordings folder"), &QAction::triggered, &menu, [p] {
    QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(p->currentConfig().outDir)));
  });
  menu.addSeparator();
  QObject::connect(menu.addAction("Quit"), &QAction::triggered, &menu, [p] {
    if (p->recState() != RecState::Idle) p->stopRecording();
    QTimer::singleShot(200, [p] {
      if (p->recState() != RecState::Idle) { QTimer::singleShot(300, qApp, &QApplication::quit); return; }
      qApp->quit();
    });
  });

  // The shared menus (and their submenus) speak the status-bar palette.
  const QString style = hudMenuStyleSheet();
  for (QMenu* m : menu.findChildren<QMenu*>()) m->setStyleSheet(style);
  menu.setStyleSheet(style);
}

}  // namespace vv::ui
