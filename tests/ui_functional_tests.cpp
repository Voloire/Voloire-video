// Functional widget checks only. No capture, encoding, audio/video devices or browser.
#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest.h"
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QEventLoop>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QTabWidget>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTimer>
#include <QWheelEvent>
#include "control_menu.h"
#include "destination.h"
#include "hud.h"
#include "panel.h"
#include "theme.h"

using namespace vv::ui;

namespace {
void settle() {
  QEventLoop loop;
  QTimer::singleShot(130, &loop, &QEventLoop::quit);
  loop.exec();
}

void saveProof(QWidget& widget, const QString& name) {
  const QString dir = qEnvironmentVariable("VV_UI_PROOF_DIR");
  if (dir.isEmpty()) return;
  QDir().mkpath(dir);
  CHECK(widget.grab().save(dir + "/" + name + ".png"));
}
}

TEST_CASE("destination names and actions distinguish services before, during and after live") {
  vv::StreamChannel yt, tw;
  yt.id = "yt_gaming"; yt.label = "Gaming channel";
  tw.id = "tw_voloire"; tw.platform = "twitch"; tw.label = "Custom label"; tw.twitchLogin = "voloire";
  std::vector<vv::StreamChannel> channels{yt, tw};
  auto local = destinationFor("file", tw.id, channels);
  CHECK(local.caption() == "LOCAL RECORDING");
  CHECK_FALSE(local.streaming());
  CHECK(local.actionText(false) == "RECORD LOCALLY");
  for (auto id : {yt.id, tw.id}) {
    auto dest = destinationFor("stream", id, channels);
    const QString name = id == yt.id ? "YOUTUBE" : "TWITCH";
    CHECK(dest.caption().startsWith(name + " / "));
    CHECK(dest.actionText(false) == "GO LIVE ON " + name);
    CHECK(dest.actionText(true) == "END " + name + " STREAM");
    CHECK_FALSE(destinationIcon(dest.platform).isNull());
    CHECK(destinationPixmap(dest.platform, 24, 2).devicePixelRatio() == 2);
  }
  CHECK(destinationFor("stream", tw.id, channels).caption() == "TWITCH / voloire");
  CHECK(destinationFor("stream", "missing", channels).platform == "unknown");
}

TEST_CASE("the destination selector controls actual routing and stays consistent across tabs and HUD") {
  QTemporaryDir settings;
  REQUIRE(settings.isValid());
  qputenv("VV_SETTINGS_DIR", settings.path().toUtf8());
  vv::UiPrefs prefs;
  vv::StreamChannel yt, tw;
  yt.id = "yt_gaming"; yt.label = "Gaming channel";
  tw.id = "tw_voloire"; tw.platform = "twitch"; tw.label = "voloire"; tw.twitchLogin = "voloire";
  tw.streamUrl = "rtmp://live.twitch.tv/app";
  prefs.channels = {yt, tw};
  prefs.darkTheme = true;
  vv::saveUiPrefs(prefs);
  vv::AppController ctl;
  MainPanel panel(ctl, nullptr, true);
  panel.setPrefs(&prefs);
  HudBar hud(&panel, &prefs);
  auto* selector = panel.findChild<QComboBox*>("destination");
  REQUIRE(selector != nullptr);
  panel.show();

  for (bool dark : {true, false}) {
    panel.setDarkMode(dark);
    for (const QString id : {QString("yt_gaming"), QString("tw_voloire"), QString()}) {
      const int index = selector->findData(id);
      REQUIRE(index >= 0);
      selector->setCurrentIndex(index);
      settle();
      const auto d = panel.destination();
      const bool stream = !id.isEmpty();
      CHECK(selector->currentText() == d.caption());
      CHECK(selector->currentData().toString() == id);
      CHECK(panel.currentConfig().output == (stream ? "stream" : "file"));
      if (stream) {
        CHECK(panel.currentConfig().streamChannel == id.toStdString());
        CHECK(panel.currentConfig().streamUrl == (id == "tw_voloire" ? tw.streamUrl : yt.streamUrl));
      }
      CHECK(panel.destinationState()["button"] == d.actionText(false).toStdString());
      CHECK(hud.state()["platform"] == d.platform.toStdString());
      CHECK(hud.state()["icon"] == d.platform.toStdString());
      CHECK(hud.state()["text"].get<std::string>().find(d.caption().toStdString()) != std::string::npos);
      CHECK(hud.accessibleName().contains(d.caption()));
      for (int tab : {0, 1, 2}) {
        panel.setActiveTab(tab);
        CHECK(panel.destination().caption() == d.caption());
        CHECK(selector->isVisible());
      }
      QMenu menu;
      buildControlMenu(menu, panel, {});
      CHECK(menu.actions()[0]->text() == "Destination: " + d.caption());
      CHECK_FALSE(menu.actions()[0]->icon().isNull());
      QAction* start = nullptr;
      for (auto* a : menu.actions()) if (a->text() == d.actionText(false, true)) start = a;
      REQUIRE(start != nullptr);
      CHECK(start->isEnabled() == !stream);  // no key in isolated settings
      const QString proof = d.platform + (dark ? "_dark" : "_light");
      panel.resize(620, 715);
      saveProof(panel, proof + "_panel");
      saveProof(hud, proof + "_hud");
      panel.resize(420, 715);
      settle();
      CHECK(panel.width() == 420);
      CHECK(selector->width() >= 100);
      saveProof(panel, proof + "_narrow");
    }
  }
  // Even an accidental start in this test mode cannot open devices or an ingest.
  panel.startRecording();
  CHECK(panel.recState() == vv::RecState::Idle);
  CHECK(panel.errorText().contains("disabled in functional UI tests"));
  qunsetenv("VV_SETTINGS_DIR");
}

TEST_CASE("HUD resolution edits route to streaming and Twitch controls mirror the real settings") {
  QTemporaryDir settings;
  REQUIRE(settings.isValid());
  qputenv("VV_SETTINGS_DIR", settings.path().toUtf8());
  vv::RecordConfig c;
  c.output = "stream"; c.streamChannel = "tw_voloire";
  c.outHeight = 720; c.streamHeight = 0; c.streamBitrateKbps = 16000;
  vv::saveSettings(c);
  vv::UiPrefs prefs;
  vv::StreamChannel yt, tw;
  yt.id = "default";
  tw.id = "tw_voloire"; tw.platform = "twitch"; tw.label = "voloire"; tw.twitchLogin = "voloire";
  prefs.channels = {yt, tw};
  prefs.darkTheme = true;
  vv::saveUiPrefs(prefs);
  vv::AppController ctl;
  ctl.setConfig(c);
  MainPanel panel(ctl, nullptr, true);
  panel.setPrefs(&prefs);
  auto* twRes = panel.findChild<QComboBox*>("twitchStreamResolution");
  auto* twFps = panel.findChild<QComboBox*>("twitchStreamFps");
  auto* twKbps = panel.findChild<QSpinBox*>("twitchStreamBitrate");
  auto* streamKbps = panel.findChild<QSpinBox*>("streamBitrate");
  REQUIRE(twRes); REQUIRE(twFps); REQUIRE(twKbps); REQUIRE(streamKbps);
  CHECK(panel.activeChannelPlatform() == "twitch");
  CHECK(panel.currentConfig().streamHeight == 0);
  CHECK(panel.currentConfig().streamBitrateKbps == 6000);
  QMenu menu;
  buildControlMenu(menu, panel, {});
  REQUIRE(triggerMenu(menu, {"Format", "Resolution", "1080p"}));
  CHECK(panel.currentConfig().streamHeight == 1080);
  CHECK(panel.currentConfig().outHeight == 720);
  CHECK(vv::loadSettings().streamHeight == 1080);
  CHECK(twRes->currentData().toInt() == 1080);
  CHECK(panel.encodingSummary().contains("1920x1080"));
  CHECK(panel.encodingSummary().contains("6000 kbps"));
  HudBar hud(&panel, &prefs);
  CHECK(hud.state()["text"].get<std::string>().find("1920x1080") != std::string::npos);
  twRes->setCurrentIndex(twRes->findData(720));
  twFps->setCurrentIndex(twFps->findData(30));
  twKbps->setValue(4500);
  CHECK(panel.currentConfig().streamHeight == 720);
  CHECK(panel.currentConfig().fps == 30);
  CHECK(panel.currentConfig().streamBitrateKbps == 4500);
  panel.setOutput("file");
  panel.setHeight(480);
  CHECK(panel.currentConfig().outHeight == 480);
  CHECK(panel.currentConfig().streamHeight == 720);
  panel.setOutput("stream");
  panel.setHeight(0);
  CHECK(panel.currentConfig().streamHeight == 0);
  CHECK(twRes->currentData().toInt() == 0);
  CHECK(panel.encodingSummary().contains("NATIVE SIZE"));
  panel.selectChannel("default");
  streamKbps->setValue(16000);
  CHECK(panel.currentConfig().streamBitrateKbps == 16000);
  panel.selectChannel("tw_voloire");
  CHECK(panel.currentConfig().streamBitrateKbps == 6000);
  panel.setHeight(1080);
  panel.setFps(60);
  panel.setActiveTab(2);
  panel.show();
  settle();
  for (bool dark : {true, false}) {
    panel.setDarkMode(dark);
    saveProof(panel, dark ? "stream_settings_dark" : "stream_settings_light");
    saveProof(hud, dark ? "stream_settings_dark_hud" : "stream_settings_light_hud");
  }
  qunsetenv("VV_SETTINGS_DIR");
}

TEST_CASE("panel stays inside the work area and every tab scrolls to its last control") {
  QTemporaryDir settings;
  REQUIRE(settings.isValid());
  qputenv("VV_SETTINGS_DIR", settings.path().toUtf8());
  vv::RecordConfig config;
  config.output = "stream"; config.streamChannel = "tw_voloire";
  config.streamHeight = 1080; config.streamBitrateKbps = 6000; config.fps = 120;
  vv::saveSettings(config);
  vv::UiPrefs prefs;
  vv::StreamChannel twitch;
  twitch.id = config.streamChannel; twitch.platform = "twitch"; twitch.twitchLogin = "voloire";
  twitch.title = "Mortal Shell II - Chill Playthrough (ITA) - NoCommentary";
  twitch.categoryName = "Mortal Shell II";
  prefs.channels = {twitch};
  vv::saveUiPrefs(prefs);
  vv::AppController ctl;
  ctl.setConfig(config);
  MainPanel panel(ctl, nullptr, true);
  panel.setPrefs(&prefs);
  panel.show();
  settle();
  auto* tabs = panel.findChild<QTabWidget*>();
  REQUIRE(tabs);
  const QRect work = panel.screen()->availableGeometry();
  for (bool dark : {true, false}) {
    panel.setDarkMode(dark);
    for (QSize requested : {QSize(620, 500), QSize(420, 380), QSize(1200, 1600)}) {
      panel.resize(requested);
      settle();
      CHECK(work.contains(panel.frameGeometry()));
      for (int tab = 0; tab < tabs->count(); ++tab) {
        panel.setActiveTab(tab);
        settle();
        auto* scroll = qobject_cast<QScrollArea*>(tabs->widget(tab));
        REQUIRE(scroll);
        auto* horizontal = scroll->horizontalScrollBar();
        horizontal->setValue(horizontal->maximum());
        auto* bar = scroll->verticalScrollBar();
        bar->setValue(bar->minimum());
        const QPointF center(scroll->viewport()->rect().center());
        // Each wheel event scrolls at most one page, even with a large delta.
        for (int page = 0; page < 40 && bar->value() < bar->maximum(); ++page) {
          QWheelEvent wheel(center, scroll->viewport()->mapToGlobal(center.toPoint()),
                            QPoint(), QPoint(0, -120000), Qt::NoButton, Qt::NoModifier,
                            Qt::NoScrollPhase, false);
          QApplication::sendEvent(scroll->viewport(), &wheel);
        }
        settle();
        CHECK(bar->value() == bar->maximum());
        CHECK(panel.rect().contains(scroll->mapTo(&panel, scroll->rect().bottomRight())));
        int lastBottom = -1;
        int lastRight = -1;
        for (auto* button : scroll->widget()->findChildren<QPushButton*>()) {
          if (!button->isVisible()) continue;
          const int bottom = button->mapTo(scroll->viewport(), button->rect().bottomRight()).y();
          lastBottom = std::max(lastBottom, bottom);
          lastRight = std::max(lastRight, button->mapTo(scroll->viewport(), button->rect().bottomRight()).x());
        }
        INFO("tab=", tab, " requested=", requested.width(), "x", requested.height(),
             " window=", panel.width(), "x", panel.height(), " bottom=", lastBottom,
             " viewport=", scroll->viewport()->height(), " max=", bar->maximum());
        CHECK(lastBottom >= 0);
        CHECK(lastBottom < scroll->viewport()->height());
        CHECK(lastRight < scroll->viewport()->width());
        if (horizontal->maximum() > 0) CHECK(horizontal->isVisible());
        if (requested == QSize(620, 500))
          saveProof(panel, QString("scroll_bottom_%1_%2").arg(tab).arg(dark ? "dark" : "light"));
      }
    }
  }
  panel.hide();
  panel.move(work.right() + 100, work.bottom() + 100);
  panel.show();
  settle();
  CHECK(work.contains(panel.frameGeometry()));
  qunsetenv("VV_SETTINGS_DIR");
}

int main(int argc, char** argv) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  qputenv("QT_QPA_PLATFORM_PLUGIN_PATH", VV_QT_PLUGIN_PATH);
  QApplication app(argc, argv);
  loadFonts(QCoreApplication::applicationDirPath() + "/fonts");
  return doctest::Context(argc, argv).run();
}
