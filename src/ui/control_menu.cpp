#include "control_menu.h"
#include <QActionGroup>
#include <QApplication>
#include <QDesktopServices>
#include <QMouseEvent>
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

class MenuStayOpenFilter : public QObject {
 public:
  MenuStayOpenFilter(QMenu* root, std::function<void()> syncFn)
      : QObject(root), root_(root), syncFn_(std::move(syncFn)) {}

 protected:
  bool eventFilter(QObject* o, QEvent* e) override {
    auto* m = qobject_cast<QMenu*>(o);
    if (!m) return QObject::eventFilter(o, e);
    if (e->type() == QEvent::MouseButtonPress) {
      auto* me = static_cast<QMouseEvent*>(e);
      if (me->button() == Qt::RightButton) {
        root_->close();
        return true;
      }
    } else if (e->type() == QEvent::MouseButtonRelease) {
      auto* me = static_cast<QMouseEvent*>(e);
      if (me->button() == Qt::RightButton) return true;
      if (me->button() == Qt::LeftButton) {
        QAction* a = m->actionAt(me->pos());
        if (a && a->isEnabled() && !a->menu() && a->property("keepOpen").toBool()) {
          a->trigger();
          if (syncFn_) syncFn_();
          m->update();
          root_->update();
          return true;
        }
      }
    }
    return QObject::eventFilter(o, e);
  }

 private:
  QMenu* root_;
  std::function<void()> syncFn_;
};

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
  const bool stream = panel.outputKind() == "stream";

  auto* destinationInfo = menu.addAction(destinationIcon(panel.destination().platform),
                                         "Destination: " + panel.destination().caption());
  destinationInfo->setEnabled(false);
  QAction* startStop = menu.addAction(destinationIcon(panel.destination().platform),
                                      panel.destination().actionText(st != RS::Idle, true));
  // Going live needs a stored key: without one the entry stays disabled.
  startStop->setEnabled(!panel.starting() && (st != RS::Idle || !stream || panel.streamKeySet()));
  QObject::connect(startStop, &QAction::triggered, &menu, [p] {
    if (p->recState() == RS::Idle) p->startRecording(); else p->stopRecording();
  });
  QAction* pause = menu.addAction(st == RS::Paused ? "Resume" : "Pause");
  pause->setEnabled((st == RS::Recording || st == RS::Paused) && !stream);  // a stream cannot pause
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
      a->setProperty("keepOpen", true);
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
    r->setProperty("keepOpen", true);
    QObject::connect(r, &QAction::triggered, &menu, [p] { p->refreshSources(); });
  }

  vv::RecordConfig c = panel.currentConfig();

  auto radio = [&](QMenu* sub, const QString& text, bool checked, std::function<void()> fn) {
    QAction* a = sub->addAction(text);
    a->setCheckable(true);
    a->setChecked(checked);
    a->setProperty("keepOpen", true);
    QObject::connect(a, &QAction::triggered, &menu, [fn] { fn(); });
    return a;
  };

  // Format
  QMenu* fmt = menu.addMenu("Format");
  fmt->setEnabled(idle);
  {
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

  // Quality: the presets that resolve the bitrate, plus a disabled mirror of a hand-typed one.
  QMenu* qual = menu.addMenu("Quality");
  qual->setEnabled(idle);
  {
    auto* grp = new QActionGroup(qual);
    for (auto tv : {std::pair{"High", "high"}, std::pair{"Standard", "standard"}, std::pair{"Light", "light"},
                     std::pair{"Minimal", "minimal"}}) {
      QString v = tv.second;
      grp->addAction(radio(qual, tv.first, c.quality == tv.second, [p, v] { p->setQuality(v); }));
    }
    QAction* custom = qual->addAction(QString("Custom (%1 kbps)").arg(c.bitrateKbps));
    custom->setCheckable(true);
    custom->setChecked(c.quality == "custom");
    custom->setEnabled(false);
  }

  // Share copy: the size cap for the copy of the finished recording.
  QMenu* shr = menu.addMenu("Share copy");
  shr->setEnabled(idle);
  {
    auto* grp = new QActionGroup(shr);
    for (int mb : {0, 10, 25, 50, 100}) {
      QAction* a = shr->addAction(mb ? QString("%1 MB").arg(mb) : QString("Off"));
      a->setCheckable(true);
      a->setChecked(panel.shareTargetMB() == mb);
      a->setProperty("keepOpen", true);
      grp->addAction(a);
      QObject::connect(a, &QAction::triggered, &menu, [p, mb] { p->setShareTarget(mb); });
    }
    shr->addSeparator();
    QAction* clip = shr->addAction("Copy to clipboard when ready");
    clip->setCheckable(true);
    clip->setChecked(panel.shareToClipboard());
    clip->setProperty("keepOpen", true);
    QObject::connect(clip, &QAction::triggered, &menu, [p](bool on) { p->setShareToClipboard(on); });
    QAction* again = shr->addAction("Make share copy of last recording");
    again->setProperty("keepOpen", true);
    vv::json sh = panel.shareStatus();
    again->setEnabled(!panel.lastRecording().isEmpty() && sh.value("state", std::string("idle")) != "running");
    QObject::connect(again, &QAction::triggered, &menu, [p] { p->shareLastRecording(); });
  }

  // Output: a recording on disk, or a YouTube live stream.
  QMenu* out = menu.addMenu("Output");
  out->setEnabled(idle);  // locked while live
  {
    auto* grp = new QActionGroup(out);
    grp->addAction(radio(out, "Record to file", c.output == "file", [p] { p->setOutput("file"); }));
    grp->addAction(radio(out, "Stream (live)", c.output == "stream", [p] { p->setOutput("stream"); }));
  }

  // Share link: the live page as soon as the stream starts, the video page once the platform has it.
  QMenu* shl = menu.addMenu("Share link");
  QAction* shInfo = nullptr;
  QAction* shCopyLive = nullptr;
  QAction* shCopyVod = nullptr;
  QAction* shOpenLive = nullptr;
  QAction* shOpenVod = nullptr;
  {
    const QString live = panel.liveLink();
    const QString vod = panel.vodLink();
    shInfo = shl->addAction("Live: " + (live.isEmpty() ? QString("not available") : live));
    shInfo->setEnabled(false);
    shCopyLive = shl->addAction("Copy live link");
    shCopyLive->setEnabled(!live.isEmpty());
    shCopyLive->setProperty("keepOpen", true);
    QObject::connect(shCopyLive, &QAction::triggered, &menu, [p] { p->copyLiveLink(); });
    shCopyVod = shl->addAction("Copy video link");
    shCopyVod->setEnabled(!vod.isEmpty());
    shCopyVod->setProperty("keepOpen", true);
    QObject::connect(shCopyVod, &QAction::triggered, &menu, [p] { p->copyVodLink(); });
    shOpenLive = shl->addAction("Open live page");
    shOpenLive->setEnabled(!live.isEmpty());
    QObject::connect(shOpenLive, &QAction::triggered, &menu, [p] { p->openLiveLink(); });
    shOpenVod = shl->addAction("Open video page");
    shOpenVod->setEnabled(!vod.isEmpty());
    QObject::connect(shOpenVod, &QAction::triggered, &menu, [p] { p->openVodLink(); });
  }

  // YouTube: the key (its fate is the only thing shown), the studio page for the active channel,
  // and quick switching between saved YouTube channels.
  QMenu* yt = menu.addMenu("YouTube");
  QAction* ytInfo = nullptr;
  QAction* ytClear = nullptr;
  QAction* ytStudio = nullptr;
  {
    const bool keySet = panel.streamKeySet();
    ytInfo = yt->addAction(keySet ? "Stream key: set" : "Stream key: not set");
    ytInfo->setEnabled(false);
    QAction* pick = yt->addAction("Set stream key...");
    pick->setEnabled(idle);
    QObject::connect(pick, &QAction::triggered, &menu, [p] { p->promptStreamKey(); });
    ytClear = yt->addAction("Clear stream key");
    ytClear->setEnabled(idle && keySet);
    ytClear->setProperty("keepOpen", true);
    QObject::connect(ytClear, &QAction::triggered, &menu, [p] { p->clearStreamKey(); });
    yt->addSeparator();
    ytStudio = yt->addAction("Open YouTube Studio");
    ytStudio->setToolTip(QString("Open YouTube Studio for %1").arg(panel.activeChannelLabel()));
    QObject::connect(ytStudio, &QAction::triggered, &menu, [p] { p->openStudio("youtube"); });
    yt->addSeparator();
    QAction* chHdr = yt->addAction("Destination channel:");
    chHdr->setEnabled(false);
    auto* chGrp = new QActionGroup(yt);
    QString curCh = panel.activeChannelId();
    for (const auto& ch : panel.channels()) {
      if (ch.platform == "twitch") continue;  // Twitch destinations live in the Twitch menu
      QString cid = QString::fromStdString(ch.id);
      QString lbl = QString::fromStdString(ch.label);
      QAction* ca = yt->addAction(lbl);
      ca->setCheckable(true);
      ca->setChecked(cid == curCh);
      ca->setEnabled(idle);
      ca->setProperty("keepOpen", true);
      chGrp->addAction(ca);
      QObject::connect(ca, &QAction::triggered, &menu, [p, cid] { p->selectChannel(cid); });
    }
    QAction* addCh = yt->addAction("Add YouTube channel...");
    addCh->setEnabled(idle);
    QObject::connect(addCh, &QAction::triggered, &menu, [p] { p->promptAddChannel(); });
  }

  // Twitch: sign-in, key, broadcast info and the destination profile.
  QMenu* twm = menu.addMenu("Twitch");
  QAction* twKeyInfo = nullptr;
  {
    vv::json tws = panel.twitchStatus();
    const bool signedIn = tws.value("signedIn", false);
    const bool hasProfile = !tws.value("channel", std::string()).empty();
    const QString name = QString::fromStdString(tws.value("displayName", std::string()));
    twm->addAction(signedIn ? "Signed in as " + name : QString("Not connected"))->setEnabled(false);
    QAction* conn = twm->addAction("Connect Twitch account...");
    conn->setEnabled(idle && !signedIn);
    QObject::connect(conn, &QAction::triggered, &menu, [p] { p->twitchConnect(); });
    QAction* out = twm->addAction("Sign out");
    out->setEnabled(idle && signedIn);
    QObject::connect(out, &QAction::triggered, &menu, [p] { p->twitchSignOut(); });
    twm->addSeparator();
    twKeyInfo = twm->addAction(hasProfile && panel.twitchKeySet() ? "Stream key: set" : "Stream key: not set");
    twKeyInfo->setEnabled(false);
    QAction* fetch = twm->addAction("Fetch stream key");
    fetch->setEnabled(idle && signedIn);
    QObject::connect(fetch, &QAction::triggered, &menu, [p] { p->twitchFetchKey(); });
    twm->addSeparator();
    QAction* title = twm->addAction("Set title...");
    title->setEnabled(signedIn && hasProfile);
    QObject::connect(title, &QAction::triggered, &menu, [p] { p->twitchPromptTitle(); });
    QAction* cat = twm->addAction("Set category...");
    cat->setEnabled(signedIn && hasProfile);
    QObject::connect(cat, &QAction::triggered, &menu, [p] { p->twitchPromptCategory(); });
    QAction* applyLive = twm->addAction("Apply info when going live");
    applyLive->setCheckable(true);
    applyLive->setChecked(panel.twitchApplyOnLive());
    applyLive->setEnabled(hasProfile);
    applyLive->setProperty("keepOpen", true);
    QObject::connect(applyLive, &QAction::triggered, &menu, [p](bool on) { p->setTwitchApplyOnLive(on); });
    twm->addSeparator();
    QAction* mgr = twm->addAction("Open stream manager");
    mgr->setEnabled(hasProfile);
    QObject::connect(mgr, &QAction::triggered, &menu, [p] { p->openStudio("twitch"); });
    QAction* vp = twm->addAction("Open video producer");
    vp->setEnabled(hasProfile);
    QObject::connect(vp, &QAction::triggered, &menu, [p] { p->openVideoProducer(); });
    twm->addSeparator();
    twm->addAction("Destination channel:")->setEnabled(false);
    auto* twGrp = new QActionGroup(twm);
    const QString curCh = panel.activeChannelId();
    for (const auto& ch : panel.channels()) {
      if (ch.platform != "twitch") continue;
      QString cid = QString::fromStdString(ch.id);
      QAction* ca = twm->addAction(QString::fromStdString(ch.label));
      ca->setCheckable(true);
      ca->setChecked(cid == curCh);
      ca->setEnabled(idle);
      ca->setProperty("keepOpen", true);
      twGrp->addAction(ca);
      QObject::connect(ca, &QAction::triggered, &menu, [p, cid] { p->selectChannel(cid); });
    }
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
      a->setProperty("keepOpen", true);
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
  QAction* muteMic = menu.addAction("Mute microphone");
  muteMic->setCheckable(true);
  muteMic->setChecked(panel.micMuted());
  muteMic->setProperty("keepOpen", true);
  QObject::connect(muteMic, &QAction::triggered, &menu, [p](bool on) { p->setMicMuted(on); });
  QAction* muteSys = menu.addAction("Mute system audio");
  muteSys->setCheckable(true);
  muteSys->setChecked(panel.sysMuted());
  muteSys->setProperty("keepOpen", true);
  QObject::connect(muteSys, &QAction::triggered, &menu, [p](bool on) { p->setSysMuted(on); });

  menu.addSeparator();
  QAction* hud = menu.addAction("Show status bar");
  hud->setCheckable(true);
  hud->setChecked(hooks.hudVisible && hooks.hudVisible());
  hud->setProperty("keepOpen", true);
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

  auto* stayFilter = new MenuStayOpenFilter(&menu, [p, startStop, destinationInfo, pause, ytInfo, ytClear, ytStudio, muteMic, muteSys, shInfo, shCopyLive, shCopyVod, shOpenLive, shOpenVod, twKeyInfo] {
    const RS s = p->recState();
    const bool isStream = p->outputKind() == "stream";
    const bool keyOk = p->streamKeySet();
    const auto dest = p->destination();
    startStop->setText(dest.actionText(s != RS::Idle, true));
    startStop->setIcon(destinationIcon(dest.platform));
    destinationInfo->setText("Destination: " + dest.caption());
    destinationInfo->setIcon(destinationIcon(dest.platform));
    startStop->setEnabled(!p->starting() && (s != RS::Idle || !isStream || keyOk));
    pause->setEnabled((s == RS::Recording || s == RS::Paused) && !isStream);
    if (ytInfo) ytInfo->setText(keyOk ? "Stream key: set" : "Stream key: not set");
    if (ytClear) ytClear->setEnabled(s == RS::Idle && !p->starting() && keyOk);
    if (ytStudio) ytStudio->setToolTip(QString("Open YouTube Studio for %1").arg(p->activeChannelLabel()));
    const QString live = p->liveLink();
    const QString vod = p->vodLink();
    if (shInfo) shInfo->setText("Live: " + (live.isEmpty() ? QString("not available") : live));
    if (shCopyLive) shCopyLive->setEnabled(!live.isEmpty());
    if (shOpenLive) shOpenLive->setEnabled(!live.isEmpty());
    if (shCopyVod) shCopyVod->setEnabled(!vod.isEmpty());
    if (shOpenVod) shOpenVod->setEnabled(!vod.isEmpty());
    if (twKeyInfo) twKeyInfo->setText(p->twitchKeySet() ? "Stream key: set" : "Stream key: not set");
    if (muteMic) muteMic->setChecked(p->micMuted());
    if (muteSys) muteSys->setChecked(p->sysMuted());
  });
  menu.installEventFilter(stayFilter);
  for (QMenu* m : menu.findChildren<QMenu*>()) m->installEventFilter(stayFilter);

  // The shared menus (and their submenus) speak the status-bar palette.
  const QString style = hudMenuStyleSheet();
  for (QMenu* m : menu.findChildren<QMenu*>()) m->setStyleSheet(style);
  menu.setStyleSheet(style);
}

}  // namespace vv::ui
