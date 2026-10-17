#include <QApplication>
#include <QDir>
#include <QFont>
#include <QMessageBox>
#include <QTimer>
#include <atomic>
#include <windows.h>
#include "app_controller.h"
#include "control.h"
#include "crash_handler.h"
#include "hud.h"
#include "panel.h"
#include "telemetry.h"
#include "theme.h"
#include "tray.h"

using namespace vv;
using namespace vv::ui;

namespace {

json screenshots(MainPanel& panel, TrayController& tray, HudBar& hud, const QString& dir) {
  QDir().mkpath(dir);
  json out = json::array();
  auto save = [&](const QPixmap& pm, const QString& name) {
    QString p = QDir(dir).filePath(name);
    out.push_back({{"file", p.toStdString()}, {"ok", pm.save(p, "PNG")}, {"w", pm.width()}, {"h", pm.height()}});
  };
  panel.show();
  panel.refreshSources();
  QApplication::processEvents();
  save(panel.grab(), "panel.png");
  panel.setActiveTab(1);
  QApplication::processEvents();
  save(panel.grab(), "panel_stream.png");
  bool wasDark = panel.darkMode();
  panel.setDarkMode(true);
  QApplication::processEvents();
  save(panel.grab(), "panel_dark.png");
  panel.setDarkMode(wasDark);
  panel.setActiveTab(0);
  QApplication::processEvents();
  tray.rebuildMenu();
  tray.menu()->adjustSize();
  save(tray.menu()->grab(), "menu.png");
  save(hud.grab(), "hud.png");
  if (hud.isVisible()) save(hud.grab(), QString("hud_%1.png").arg(QString::fromStdString(hud.dotName())));
  for (auto sn : {std::pair{IconState::Idle, "tray_idle.png"}, std::pair{IconState::Recording, "tray_recording.png"},
                  std::pair{IconState::Paused, "tray_paused.png"}, std::pair{IconState::Error, "tray_error.png"}})
    save(trayPixmap(sn.first, 64), sn.second);
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  installCrashHandler();
  QApplication app(argc, argv);
  QApplication::setQuitOnLastWindowClosed(false);
  QApplication::setApplicationName("Voloire-video");
  QApplication::setApplicationVersion(VV_VERSION);

  std::string pipe = "voloire-video";
  QString shotDir;
  bool show = true;
  for (int i = 1; i < argc; ++i) {
    QString a = argv[i];
    if (a == "--version") { std::puts(VV_VERSION); return 0; }
    else if (a == "--pipe" && i + 1 < argc) pipe = argv[++i];
    else if (a == "--screenshots" && i + 1 < argc) shotDir = argv[++i];
    else if (a == "--show") show = true;
    else if (a == "--minimized") show = false;  // autostart: tray only
    else if (a == "--export-icons" && i + 1 < argc) {  // used by scripts/make-icon.py
      QString dir = argv[++i];
      QDir().mkpath(dir);
      for (int sz : {16, 24, 32, 48, 64, 128, 256}) trayPixmap(IconState::Recording, sz).save(QDir(dir).filePath(QString("icon_%1.png").arg(sz)));
      return 0;
    }
  }

  loadFonts(QCoreApplication::applicationDirPath() + "/fonts");
  QFont f("Ubuntu");
  f.setPixelSize(13);
  app.setFont(f);
  UiPrefs uiPrefs = loadUiPrefs();
  setDarkTheme(uiPrefs.darkTheme);
  app.setStyleSheet(styleSheet(uiPrefs.darkTheme));

  AppController ctl;
  ctl.setConfig(loadSettings());
  MainPanel panel(ctl);
  panel.setPrefs(&uiPrefs);  // share target and clipboard: one ui.json shared with the status bar
  HudBar hud(&panel, &uiPrefs);
  TrayController tray(&panel);
  tray.hooks().hudVisible = [&uiPrefs] { return uiPrefs.hud; };
  tray.hooks().setHudVisible = [&hud](bool on) { hud.setShown(on); };
  if (uiPrefs.hud) hud.show();

  if (!shotDir.isEmpty()) {
    json r = screenshots(panel, tray, hud, shotDir);
    std::puts(r.dump().c_str());
    return 0;
  }

  ControlServer server;
  std::string err;
  json uiOverride = json::object();
  ctl.extra = [&](const json& req) -> json {
    const std::string cmd = req.value("cmd", std::string());
    if (cmd.rfind("ui_", 0) != 0) return nullptr;
    json result;
    QMetaObject::invokeMethod(&app, [&] {
      if (cmd == "ui_state") {
        result = tray.state();
        result["hud"] = hud.state();
        result["ok"] = true;
      } else if (cmd == "ui_trigger") {
        std::vector<std::string> path = req.value("path", std::vector<std::string>{});
        bool ok = req.value("surface", std::string("tray")) == "hud" ? hud.trigger(path) : tray.trigger(path);
        result = {{"ok", ok}};
        if (!ok) result["error"] = "menu entry not found or disabled";
      } else if (cmd == "ui_hud") {
        if (req.contains("visible")) hud.setShown(req["visible"].get<bool>());
        if (req.contains("x") && req.contains("y")) hud.moveTo(req["x"].get<int>(), req["y"].get<int>());
        if (req.contains("testPattern")) hud.setTestPattern(req["testPattern"].get<bool>());
        if (req.contains("exclude")) hud.setExcludeFromCapture(req["exclude"].get<bool>());
        result = {{"ok", true}};
      } else if (cmd == "ui_show") {
        panel.show();
        result = {{"ok", true}};
      } else if (cmd == "ui_hide") {
        panel.hide();
        result = {{"ok", true}};
      } else if (cmd == "ui_override") {
        panel.setOverride(req.value("config", json::object()));
        result = {{"ok", true}};
      } else if (cmd == "ui_screenshot") {
        result = {{"ok", true}, {"files", screenshots(panel, tray, hud, QString::fromStdString(req.value("dir", std::string("shots"))))}};
      } else if (cmd == "ui_refresh") {
        panel.refreshSources();
        result = {{"ok", true}};
      } else if (cmd == "ui_select_window") {
        panel.selectWindow(req.value("hwnd", (long long)0));
        result = {{"ok", true}};
      } else {
        result = {{"ok", false}, {"error", "unknown ui command"}};
      }
    }, Qt::BlockingQueuedConnection);
    return result;
  };
  ctl.onShutdown = [&] {
    if (ctl.recorder().state() != RecState::Idle) ctl.recorder().stop();
    QMetaObject::invokeMethod(&app, &QApplication::quit, Qt::QueuedConnection);
  };

  if (!server.start(pipe, [&](const json& r) { return ctl.handle(r); }, err)) {
    // Already running: ask that instance to show its panel, then leave quietly.
    HANDLE h = CreateFileW(wide("\\\\.\\pipe\\" + pipe).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
      const char req[] = "{\"cmd\":\"ui_show\"}\n";
      DWORD n = 0;
      WriteFile(h, req, sizeof req - 1, &n, nullptr);
      char buf[256];
      ReadFile(h, buf, sizeof buf, &n, nullptr);
      CloseHandle(h);
      return 0;
    }
    QMessageBox::warning(nullptr, "Voloire-video", "The control pipe is unavailable: " + QString::fromStdString(err));
    return 1;
  }
  if (show) panel.show();
  int rc = app.exec();
  server.stop();
  return rc;
}
