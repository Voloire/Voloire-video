// Functional widget checks only. No capture, encoding, audio/video devices or browser.
#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest.h"
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QEventLoop>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>
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
      CHECK(menu.actions()[1]->text() == d.actionText(false, true));
      CHECK(menu.actions()[1]->isEnabled() == !stream);  // no key in isolated settings
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

int main(int argc, char** argv) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  qputenv("QT_QPA_PLATFORM_PLUGIN_PATH", VV_QT_PLUGIN_PATH);
  QApplication app(argc, argv);
  loadFonts(QCoreApplication::applicationDirPath() + "/fonts");
  return doctest::Context(argc, argv).run();
}
