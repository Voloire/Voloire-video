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
#include "panel.h"
#include "telemetry.h"
#include "theme.h"
#include "tray.h"

using namespace vv;
using namespace vv::ui;

namespace {

json screenshots(MainPanel& panel, TrayController& tray, const QString& dir) {
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
  tray.rebuildMenu();
  tray.menu()->adjustSize();
  save(tray.menu()->grab(), "menu.png");
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
  app.setStyleSheet(styleSheet());

  AppController ctl;
  ctl.setConfig(loadSettings());
  MainPanel panel(ctl);
  TrayController tray(&panel);

  if (!shotDir.isEmpty()) {
    json r = screenshots(panel, tray, shotDir);
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
        result["ok"] = true;
      } else if (cmd == "ui_trigger") {
        std::vector<std::string> path = req.value("path", std::vector<std::string>{});
        bool ok = tray.trigger(path);
        result = {{"ok", ok}};
        if (!ok) result["error"] = "menu entry not found or disabled";
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
        result = {{"ok", true}, {"files", screenshots(panel, tray, QString::fromStdString(req.value("dir", std::string("shots"))))}};
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
