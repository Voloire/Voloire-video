// The TWITCH STREAM tab, the share links and the Twitch sign-in of the panel.
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QDateTime>
#include "links.h"
#include "panel.h"
#include "telemetry.h"
#include "video_layout.h"

namespace vv::ui {

namespace {
QString& lastOpenedUrl() {
  static QString u;
  return u;
}

QString s2q(const std::string& s) { return QString::fromStdString(s); }

std::vector<std::string> parseTags(const QString& text) {
  std::vector<std::string> out;
  for (const QString& t : text.split(',', Qt::SkipEmptyParts)) {
    QString v = t.trimmed();
    if (!v.isEmpty()) out.push_back(v.toStdString());
  }
  return out;
}

QString joinTags(const std::vector<std::string>& tags) {
  QStringList l;
  for (const auto& t : tags) l << s2q(t);
  return l.join(", ");
}
}  // namespace

void MainPanel::openExternal(const QUrl& u) {
  lastOpenedUrl() = u.toString();
  if (qEnvironmentVariableIsSet("VV_NO_BROWSER")) return;  // the harness never opens a browser
  QDesktopServices::openUrl(u);
}

void MainPanel::runAsync(std::function<vv::json()> work, std::function<void(const vv::json&)> done) {
  for (auto it = asyncTasks_.begin(); it != asyncTasks_.end();) {
    if (it->wait_for(std::chrono::seconds(0)) == std::future_status::ready) it = asyncTasks_.erase(it);
    else ++it;
  }
  QPointer<MainPanel> self(this);
  asyncTasks_.push_back(std::async(std::launch::async, [self, work = std::move(work), done = std::move(done)] {
    vv::json r;
    try { r = work(); }
    catch (const std::exception&) { r = {{"ok", false}, {"error", "Twitch: unexpected response, try again"}}; }
    QMetaObject::invokeMethod(qApp, [self, done, r] { if (self) done(r); }, Qt::QueuedConnection);
  }));
}

// ---------------------------------------------------------------- profile helpers

const vv::StreamChannel* MainPanel::twitchProfile() const {
  if (!prefs_) return nullptr;
  for (const auto& c : prefs_->channels)
    if (c.platform == "twitch") return &c;
  return nullptr;
}

vv::StreamChannel* MainPanel::twitchProfile() {
  return const_cast<vv::StreamChannel*>(static_cast<const MainPanel*>(this)->twitchProfile());
}

QString MainPanel::twitchChannelId() const {
  const auto* p = twitchProfile();
  return p ? s2q(p->id) : QString();
}

QString MainPanel::activeChannelPlatform() const {
  std::string cid = activeChannelId().toStdString();
  for (const auto& c : channels())
    if (c.id == cid) return s2q(c.platform);
  return "youtube";
}

vv::json MainPanel::twitchStatus() { return app_.handle({{"cmd", "twitch_status"}}); }

// ---------------------------------------------------------------- share links

vv::json MainPanel::shareLinks() const { return app_.handle({{"cmd", "links"}}).value("links", vv::json::object()); }

QString MainPanel::liveLinkFor(const QString& platform) const {
  vv::json l = shareLinks();
  if (s2q(l.value("platform", std::string())) == platform) {
    QString u = s2q(l.value("liveUrl", std::string()));
    if (!u.isEmpty()) return u;
  }
  if (platform == "twitch") {
    const auto* p = twitchProfile();
    return p ? s2q(vv::twitchLiveUrl(p->twitchLogin)) : QString();
  }
  // YouTube: the active channel when it is a YouTube one, else the first YouTube profile.
  std::string cid = activeChannelId().toStdString();
  const vv::StreamChannel* pick = nullptr;
  auto chs = channels();
  for (const auto& c : chs) if (c.platform != "twitch" && c.id == cid) pick = &c;
  if (!pick) for (const auto& c : chs) if (c.platform != "twitch") { pick = &c; break; }
  return pick ? s2q(vv::youtubeLiveUrl(vv::youtubeChannelId(pick->studioUrl))) : QString();
}

QString MainPanel::vodLinkFor(const QString& platform) const {
  vv::json l = shareLinks();
  return s2q(l.value("platform", std::string())) == platform ? s2q(l.value("vodUrl", std::string())) : QString();
}

QString MainPanel::liveLink() const { return liveLinkFor(activeChannelPlatform()); }

QString MainPanel::vodLink() const { return s2q(shareLinks().value("vodUrl", std::string())); }

void MainPanel::copyText(const QString& t) {
  if (!t.isEmpty()) QApplication::clipboard()->setText(t);
}
void MainPanel::copyLiveLink() { copyText(liveLink()); }
void MainPanel::copyVodLink() { copyText(vodLink()); }
void MainPanel::openLiveLink() { QString u = liveLink(); if (!u.isEmpty()) openExternal(QUrl(u)); }
void MainPanel::openVodLink() { QString u = vodLink(); if (!u.isEmpty()) openExternal(QUrl(u)); }

QGridLayout* MainPanel::buildShareGrid(ShareWidgets& w, const QString& platform) {
  auto* g = new QGridLayout();
  g->setHorizontalSpacing(8);
  g->setVerticalSpacing(3);
  auto lab = [&](const char* t) { auto* l = new QLabel(t, this); l->setObjectName("sub"); return l; };
  w.live = new QLabel("not available", this);
  w.vod = new QLabel("not published yet", this);
  for (QLabel* l : {w.live, w.vod}) {
    l->setTextInteractionFlags(Qt::TextSelectableByMouse);
    l->setWordWrap(true);
  }
  w.copyLive = new QPushButton("COPY", this);
  w.openLive = new QPushButton("OPEN", this);
  w.copyVod = new QPushButton("COPY", this);
  w.openVod = new QPushButton("OPEN", this);
  g->addWidget(lab("LIVE LINK (SHARE IT AS SOON AS YOU ARE LIVE)"), 0, 0, 1, 3);
  g->addWidget(w.live, 1, 0);
  g->addWidget(w.copyLive, 1, 1);
  g->addWidget(w.openLive, 1, 2);
  g->addWidget(lab("VIDEO LINK (WHEN THE PLATFORM PUBLISHES IT)"), 2, 0, 1, 3);
  g->addWidget(w.vod, 3, 0);
  g->addWidget(w.copyVod, 3, 1);
  g->addWidget(w.openVod, 3, 2);
  g->setColumnStretch(0, 1);
  connect(w.copyLive, &QPushButton::clicked, this, [this, platform] { copyText(liveLinkFor(platform)); });
  connect(w.openLive, &QPushButton::clicked, this, [this, platform] {
    QString u = liveLinkFor(platform);
    if (!u.isEmpty()) openExternal(QUrl(u));
  });
  connect(w.copyVod, &QPushButton::clicked, this, [this, platform] { copyText(vodLinkFor(platform)); });
  connect(w.openVod, &QPushButton::clicked, this, [this, platform] {
    QString u = vodLinkFor(platform);
    if (!u.isEmpty()) openExternal(QUrl(u));
  });
  return g;
}

void MainPanel::refreshShareLinks() {
  vv::json l = shareLinks();
  const QString trackerPlatform = s2q(l.value("platform", std::string()));
  const QString error = s2q(l.value("error", std::string()));
  auto fill = [&](ShareWidgets& w, const QString& platform) {
    if (!w.live) return;
    QString live = liveLinkFor(platform);
    QString vod = trackerPlatform == platform ? s2q(l.value("vodUrl", std::string())) : QString();
    QString liveText = live;
    if (live.isEmpty()) {
      liveText = (trackerPlatform == platform && !error.isEmpty()) ? error
                 : platform == "youtube" ? QString("Set the channel ID (UC...) in the YouTube tab to get a share link")
                                         : QString("not available: sign in to Twitch");
    }
    if (w.live->text() != liveText) w.live->setText(liveText);
    QString vodText = vod.isEmpty() ? QString("not published yet") : vod;
    if (w.vod->text() != vodText) w.vod->setText(vodText);
    w.copyLive->setEnabled(!live.isEmpty());
    w.openLive->setEnabled(!live.isEmpty());
    w.copyVod->setEnabled(!vod.isEmpty());
    w.openVod->setEnabled(!vod.isEmpty());
  };
  fill(twShare_, "twitch");
  fill(ytShare_, "youtube");
}

// ---------------------------------------------------------------- the tab

void MainPanel::buildTwitchTab() {
  auto* scroll = new QScrollArea(tabs_);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  auto* widget = new QWidget(scroll);
  widget->setObjectName("scrollBody");
  auto* body = new QVBoxLayout(widget);
  body->setSizeConstraint(QLayout::SetMinAndMaxSize);
  body->setContentsMargins(0, 4, 0, 4);
  body->setSpacing(6);
  scroll->setWidget(widget);
  tabs_->addTab(scroll, "TWITCH STREAM");
  auto lab = [&](const char* t) { auto* l = new QLabel(t, this); l->setObjectName("sub"); return l; };

  // Card 1: account, ingest and stream key
  {
    auto* g = new QGridLayout();
    g->setHorizontalSpacing(8);
    g->setVerticalSpacing(4);
    twClientIdEdit_ = new QLineEdit(this);
    twClientIdEdit_->setPlaceholderText("Client ID of your Twitch app (dev.twitch.tv/console/apps, type Public)");
    twConnectBtn_ = new QPushButton("CONNECT", this);
    twConnectBtn_->setToolTip("Sign in with Twitch in the browser");
    twSignOutBtn_ = new QPushButton("SIGN OUT", this);
    twStatusLabel_ = new QLabel("NOT CONNECTED", this);
    twStatusLabel_->setObjectName("status");
    twIngestCombo_ = new QComboBox(this);
    twIngestCombo_->addItem("Default (rtmp://live.twitch.tv/app)", "rtmp://live.twitch.tv/app");
    twIngestCombo_->addItem("Custom", "custom");
    twIngestRefreshBtn_ = new QPushButton("REFRESH LIST", this);
    twIngestRefreshBtn_->setToolTip("Load the Twitch ingest servers");
    twUrlEdit_ = new QLineEdit(this);
    twUrlEdit_->setPlaceholderText("rtmp://live.twitch.tv/app");
    twKeyStateLabel_ = new QLabel("Stream key: NOT SET", this);
    twKeyStateLabel_->setObjectName("status");
    twFetchKeyBtn_ = new QPushButton("FETCH KEY FROM TWITCH", this);
    twDashboardBtn_ = new QPushButton("DASHBOARD", this);
    twDashboardBtn_->setToolTip("Open the Twitch stream manager");
    twKeyEdit_ = new QLineEdit(this);
    twKeyEdit_->setEchoMode(QLineEdit::Password);
    twKeyEdit_->setPlaceholderText("or paste the stream key by hand (stored encrypted)");
    twKeySave_ = new QPushButton("SAVE", this);
    twKeyClear_ = new QPushButton("CLEAR", this);
    auto* hint = lab("Set the actual stream resolution and bitrate in STREAM ENCODING below. Audio is shared with Recording.");
    hint->setWordWrap(true);

    g->addWidget(lab("TWITCH APP CLIENT ID"), 0, 0, 1, 3);
    g->addWidget(twClientIdEdit_, 1, 0);
    g->addWidget(twConnectBtn_, 1, 1);
    g->addWidget(twSignOutBtn_, 1, 2);
    g->addWidget(twStatusLabel_, 2, 0, 1, 3);
    g->addWidget(lab("INGEST SERVER"), 3, 0, 1, 3);
    g->addWidget(twIngestCombo_, 4, 0, 1, 2);
    g->addWidget(twIngestRefreshBtn_, 4, 2);
    g->addWidget(lab("INGEST URL"), 5, 0, 1, 3);
    g->addWidget(twUrlEdit_, 6, 0, 1, 3);
    g->addWidget(lab("STREAM KEY"), 7, 0);
    g->addWidget(twKeyStateLabel_, 7, 1, 1, 2);
    auto* keyRow = new QHBoxLayout();
    keyRow->addWidget(twKeyEdit_, 1);
    keyRow->addWidget(twKeySave_);
    keyRow->addWidget(twKeyClear_);
    g->addLayout(keyRow, 8, 0, 1, 3);
    auto* fetchRow = new QHBoxLayout();
    fetchRow->addWidget(twFetchKeyBtn_, 1);
    fetchRow->addWidget(twDashboardBtn_);
    g->addLayout(fetchRow, 9, 0, 1, 3);
    g->addWidget(hint, 10, 0, 1, 3);
    g->setColumnStretch(0, 1);
    body->addWidget(makeCard("TWITCH ACCOUNT", g, this));

    twAuthTimer_ = new QTimer(this);
    connect(twAuthTimer_, &QTimer::timeout, this, [this] {
      vv::json st = twitchStatus();
      std::string state = st.value("auth", vv::json::object()).value("state", std::string("idle"));
      if (state == "pending") return;
      twAuthTimer_->stop();
      if (twAuthBox_) {
        QObject::disconnect(twAuthBox_, nullptr, this, nullptr);
        twAuthBox_->close();
      }
      if (state == "ok") {
        if (prefs_) *prefs_ = vv::loadUiPrefs();
        refreshChannelCombos();
        selectChannel(s2q(st.value("channel", std::string())));
        twLoadedId_.clear();
        syncKeyUi();
        showError({});
      } else if (state == "error") {
        showError(s2q(st.value("auth", vv::json::object()).value("error", std::string("Twitch sign-in failed"))));
      }
      emit changed();
    });

    connect(twClientIdEdit_, &QLineEdit::editingFinished, this, [this] {
      if (loading_ || !prefs_) return;
      prefs_->twitchClientId = twClientIdEdit_->text().trimmed().toStdString();
      vv::saveUiPrefs(*prefs_);
    });
    connect(twConnectBtn_, &QPushButton::clicked, this, &MainPanel::twitchConnect);
    connect(twSignOutBtn_, &QPushButton::clicked, this, &MainPanel::twitchSignOut);
    connect(twIngestCombo_, &QComboBox::currentIndexChanged, this, [this] {
      if (loading_) return;
      QString u = twIngestCombo_->currentData().toString();
      if (u != "custom") setTwitchIngest(u);
    });
    connect(twUrlEdit_, &QLineEdit::editingFinished, this, [this] {
      if (!loading_) setTwitchIngest(twUrlEdit_->text().trimmed());
    });
    connect(twIngestRefreshBtn_, &QPushButton::clicked, this, [this] {
      runAsync([this] { return app_.handle({{"cmd", "twitch_ingests"}}); }, [this](const vv::json& r) {
        if (!r.value("ok", false)) { showError(s2q(r.value("error", std::string("Twitch: cannot load the ingest list")))); return; }
        QString cur = twUrlEdit_->text().trimmed();
        QSignalBlocker b(twIngestCombo_);
        twIngestCombo_->clear();
        twIngestCombo_->addItem("Default (rtmp://live.twitch.tv/app)", "rtmp://live.twitch.tv/app");
        for (const auto& i : r.value("ingests", vv::json::array()))
          twIngestCombo_->addItem(QString("%1 (%2)").arg(s2q(i.value("name", std::string())), s2q(i.value("url", std::string()))),
                                  s2q(i.value("url", std::string())));
        twIngestCombo_->addItem("Custom", "custom");
        int idx = twIngestCombo_->findData(cur);
        twIngestCombo_->setCurrentIndex(idx >= 0 ? idx : twIngestCombo_->findData("custom"));
        showError({});
      });
    });
    connect(twFetchKeyBtn_, &QPushButton::clicked, this, &MainPanel::twitchFetchKey);
    connect(twKeySave_, &QPushButton::clicked, this, [this] {
      if (twKeyEdit_->text().trimmed().isEmpty() || twitchChannelId().isEmpty()) return;
      if (setStreamKeyFor(twitchChannelId(), twKeyEdit_->text().trimmed().toStdString())) twKeyEdit_->clear();
    });
    connect(twKeyClear_, &QPushButton::clicked, this, [this] {
      if (!twitchChannelId().isEmpty()) setStreamKeyFor(twitchChannelId(), "");
    });
    connect(twDashboardBtn_, &QPushButton::clicked, this, [this] {
      if (const auto* p = twitchProfile()) openExternal(QUrl(s2q(p->studioUrl)));
    });
  }

  // Real stream controls, mirrored with the shared streaming configuration and HUD menu.
  {
    auto* g = new QGridLayout();
    auto lab = [&](const char* text) { auto* l = new QLabel(text, this); l->setObjectName("sub"); return l; };
    twStreamResCombo_ = new QComboBox(this);
    twStreamResCombo_->setObjectName("twitchStreamResolution");
    twStreamResCombo_->addItem("Source size (native)", 0);
    for (int h : {2160, 1440, 1080, 720, 480}) {
      auto size = vv::streamCanvas(0, 0, h);
      twStreamResCombo_->addItem(QString("%1p (%2 x %3)").arg(h).arg(size.width).arg(size.height), h);
    }
    twFpsCombo_ = new QComboBox(this);
    twFpsCombo_->setObjectName("twitchStreamFps");
    for (int fps : {24, 30, 60, 120}) twFpsCombo_->addItem(QString("%1 fps").arg(fps), fps);
    twBitrate_ = new QSpinBox(this);
    twBitrate_->setObjectName("twitchStreamBitrate");
    twBitrate_->setRange(1000, 6000);
    twBitrate_->setSingleStep(500);
    twBitrate_->setSuffix(" kbps");
    g->addWidget(lab("STREAM RESOLUTION"), 0, 0);
    g->addWidget(lab("FRAME RATE"), 0, 1);
    g->addWidget(lab("BITRATE (CBR)"), 0, 2);
    g->addWidget(twStreamResCombo_, 1, 0);
    g->addWidget(twFpsCombo_, 1, 1);
    g->addWidget(twBitrate_, 1, 2);
    auto* hint = lab("Named resolutions use a 16:9 canvas with black bars for ultrawide sources. Twitch H.264 uses up to 6000 kbps.");
    hint->setWordWrap(true);
    g->addWidget(hint, 2, 0, 1, 3);
    body->addWidget(makeCard("STREAM ENCODING (H.264 CBR + 2s GOP)", g, this));
    connect(twStreamResCombo_, &QComboBox::currentIndexChanged, this, [this] {
      if (!loading_) setStreamHeight(twStreamResCombo_->currentData().toInt());
    });
    connect(twFpsCombo_, &QComboBox::currentIndexChanged, this, [this] {
      if (!loading_) setFps(twFpsCombo_->currentData().toInt());
    });
    connect(twBitrate_, &QSpinBox::valueChanged, this, [this](int value) {
      if (!loading_) { streamKbps_->setValue(value); persist(); }
    });
  }

  // Card 2: the broadcast info, set before going live
  {
    auto* g = new QGridLayout();
    g->setHorizontalSpacing(8);
    g->setVerticalSpacing(4);
    twTitleEdit_ = new QLineEdit(this);
    twTitleEdit_->setMaxLength(140);
    twTitleEdit_->setPlaceholderText("Stream title");
    twTitleCount_ = new QLabel("0/140", this);
    twTitleCount_->setObjectName("sub");
    twTitleCount_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    twCategoryEdit_ = new QLineEdit(this);
    twCategoryEdit_->setPlaceholderText("Search a category or game");
    twCategorySearchBtn_ = new QPushButton("SEARCH", this);
    twCategoryCombo_ = new QComboBox(this);
    twTagsEdit_ = new QLineEdit(this);
    twTagsEdit_->setPlaceholderText("gaming, chill (letters and digits, up to 10)");
    twLangCombo_ = new QComboBox(this);
    for (const char* l : {"it", "en", "es", "fr", "de", "pt", "other"}) twLangCombo_->addItem(QString(l).toUpper(), l);
    twLoadInfoBtn_ = new QPushButton("LOAD FROM TWITCH", this);
    twApplyBtn_ = new QPushButton("APPLY", this);
    twGoLiveBtn_ = new QPushButton("GO LIVE ON TWITCH", this);
    twGoLiveBtn_->setIcon(destinationIcon("twitch"));
    twGoLiveBtn_->setToolTip("Apply broadcast info, select Twitch and go live on Twitch");
    twApplyOnLiveCheck_ = new QCheckBox("Apply this info when going live", this);
    twApplyOnLiveCheck_->setChecked(true);
    twInfoStatusLabel_ = new QLabel("", this);
    twInfoStatusLabel_->setObjectName("status");
    twInfoStatusLabel_->setWordWrap(true);

    g->addWidget(lab("TITLE"), 0, 0);
    g->addWidget(twTitleCount_, 0, 1);
    g->addWidget(twTitleEdit_, 1, 0, 1, 2);
    g->addWidget(lab("CATEGORY / GAME"), 2, 0, 1, 2);
    g->addWidget(twCategoryEdit_, 3, 0);
    g->addWidget(twCategorySearchBtn_, 3, 1);
    g->addWidget(twCategoryCombo_, 4, 0, 1, 2);
    g->addWidget(lab("TAGS (COMMA SEPARATED, MAX 10)"), 5, 0);
    g->addWidget(lab("LANGUAGE"), 5, 1);
    g->addWidget(twTagsEdit_, 6, 0);
    g->addWidget(twLangCombo_, 6, 1);
    auto* btnRow = new QHBoxLayout();
    btnRow->addWidget(twLoadInfoBtn_);
    btnRow->addWidget(twApplyBtn_);
    btnRow->addWidget(twGoLiveBtn_);
    g->addLayout(btnRow, 7, 0, 1, 2);
    g->addWidget(twApplyOnLiveCheck_, 8, 0, 1, 2);
    g->addWidget(twInfoStatusLabel_, 9, 0, 1, 2);
    g->setColumnStretch(0, 3);
    g->setColumnStretch(1, 1);
    body->addWidget(makeCard("BROADCAST INFO (SET BEFORE GOING LIVE)", g, this));

    connect(twTitleEdit_, &QLineEdit::textChanged, this,
            [this](const QString& t) { twTitleCount_->setText(QString("%1/140").arg(t.size())); });
    connect(twTitleEdit_, &QLineEdit::editingFinished, this, &MainPanel::saveTwitchFields);
    connect(twTagsEdit_, &QLineEdit::editingFinished, this, &MainPanel::saveTwitchFields);
    connect(twLangCombo_, &QComboBox::currentIndexChanged, this, [this] { saveTwitchFields(); });
    connect(twCategoryCombo_, &QComboBox::currentIndexChanged, this, [this] { saveTwitchFields(); });
    connect(twApplyOnLiveCheck_, &QCheckBox::toggled, this, [this] { saveTwitchFields(); });
    auto search = [this] {
      QString q = twCategoryEdit_->text().trimmed();
      if (q.isEmpty()) return;
      runAsync([this, q] { return app_.handle({{"cmd", "twitch_search_categories"}, {"query", q.toStdString()}}); },
               [this, q](const vv::json& r) {
                 if (!r.value("ok", false)) { showError(s2q(r.value("error", std::string("Twitch: category search failed")))); return; }
                 const vv::json cats = r.value("categories", vv::json::array());
                 if (cats.empty()) { showError("Twitch: no category matches '" + q + "'"); return; }
                 {
                   QSignalBlocker b(twCategoryCombo_);
                   twCategoryCombo_->clear();
                   for (const auto& c : cats)
                     twCategoryCombo_->addItem(s2q(c.value("name", std::string())), s2q(c.value("id", std::string())));
                   twCategoryCombo_->setCurrentIndex(0);
                 }
                 showError({});
                 saveTwitchFields();
               });
    };
    connect(twCategorySearchBtn_, &QPushButton::clicked, this, search);
    connect(twCategoryEdit_, &QLineEdit::returnPressed, this, search);
    connect(twLoadInfoBtn_, &QPushButton::clicked, this, [this] {
      runAsync([this] { return app_.handle({{"cmd", "twitch_get_info"}}); }, [this](const vv::json& r) {
        if (!r.value("ok", false)) {
          showError(s2q(r.value("error", std::string("Twitch: cannot load the channel info"))));
          return;
        }
        if (auto* p = twitchProfile()) {
          p->title = r.value("title", std::string());
          p->categoryId = r.value("categoryId", std::string());
          p->categoryName = r.value("categoryName", std::string());
          std::string lang = r.value("language", std::string());
          if (!lang.empty()) p->language = lang;
          p->tags = r.value("tags", std::vector<std::string>{});
          vv::saveUiPrefs(*prefs_);
        }
        loadTwitchFields();
        twInfoStatusLabel_->setText("LOADED FROM TWITCH");
        showError({});
      });
    });
    connect(twApplyBtn_, &QPushButton::clicked, this, [this] { twitchApplyInfo(); });
    connect(twGoLiveBtn_, &QPushButton::clicked, this, [this] {
      if (recState() != RecState::Idle || starting_ || !twitchProfile()) return;
      saveTwitchFields();
      selectChannel(twitchChannelId());
      setOutput("stream");
      if (twitchProfile()->applyInfoOnGoLive) startRecording();  // startRecording applies the info first
      else twitchApplyInfo([this](bool ok) { if (ok) startRecording(); });
    });
  }

  // Card 3: share links, the VOD and the local copy
  {
    QGridLayout* g = buildShareGrid(twShare_, "twitch");
    twVideoProducerBtn_ = new QPushButton("DOWNLOAD VOD (VIDEO PRODUCER)", this);
    twVideoProducerBtn_->setToolTip("Twitch has no download API: the Video Producer page has the download");
    twLocalCopyCheck_ = new QCheckBox("Save local .mkv copy while live (the file to upload to YouTube)", this);
    twOpenFolderBtn_ = new QPushButton("OPEN FOLDER", this);
    g->addWidget(twVideoProducerBtn_, 4, 0, 1, 3);
    g->addWidget(twLocalCopyCheck_, 5, 0, 1, 2);
    g->addWidget(twOpenFolderBtn_, 5, 2);
    body->addWidget(makeCard("SHARE", g, this));
    connect(twVideoProducerBtn_, &QPushButton::clicked, this, &MainPanel::openVideoProducer);
    connect(twLocalCopyCheck_, &QCheckBox::toggled, this, [this](bool on) {
      if (localCopyCheck_ && localCopyCheck_->isChecked() != on) localCopyCheck_->setChecked(on);
    });
    connect(localCopyCheck_, &QCheckBox::toggled, this, [this](bool on) {
      if (twLocalCopyCheck_ && twLocalCopyCheck_->isChecked() != on) {
        QSignalBlocker b(twLocalCopyCheck_);
        twLocalCopyCheck_->setChecked(on);
      }
    });
    connect(twOpenFolderBtn_, &QPushButton::clicked, this,
            [this] { openExternal(QUrl::fromLocalFile(dirEdit_->text())); });
  }
  body->addStretch(1);
}

// ---------------------------------------------------------------- state of the tab

void MainPanel::loadTwitchFields() {
  if (!twTitleEdit_) return;
  const auto* p = twitchProfile();
  const bool was = loading_;
  loading_ = true;
  QSignalBlocker b1(twTitleEdit_), b2(twTagsEdit_), b3(twLangCombo_), b4(twCategoryCombo_), b5(twApplyOnLiveCheck_),
      b6(twIngestCombo_), b7(twUrlEdit_);
  twLoadedId_ = p ? s2q(p->id) : QString();
  twTitleEdit_->setText(p ? s2q(p->title) : QString());
  twTitleCount_->setText(QString("%1/140").arg(twTitleEdit_->text().size()));
  twTagsEdit_->setText(p ? joinTags(p->tags) : QString());
  int li = twLangCombo_->findData(p ? s2q(p->language) : QString("it"));
  twLangCombo_->setCurrentIndex(li >= 0 ? li : 0);
  twCategoryCombo_->clear();
  if (p && !p->categoryId.empty()) {
    twCategoryCombo_->addItem(p->categoryName.empty() ? s2q(p->categoryId) : s2q(p->categoryName), s2q(p->categoryId));
    twCategoryCombo_->setCurrentIndex(0);
  }
  twApplyOnLiveCheck_->setChecked(p ? p->applyInfoOnGoLive : true);
  QString url = p && !p->streamUrl.empty() ? s2q(p->streamUrl) : QString("rtmp://live.twitch.tv/app");
  twUrlEdit_->setText(url);
  int ii = twIngestCombo_->findData(url);
  twIngestCombo_->setCurrentIndex(ii >= 0 ? ii : twIngestCombo_->findData("custom"));
  loading_ = was;
}

void MainPanel::saveTwitchFields() {
  if (loading_ || !prefs_ || !twTitleEdit_) return;
  auto* p = twitchProfile();
  if (!p) return;
  p->title = twTitleEdit_->text().trimmed().toStdString();
  p->tags = parseTags(twTagsEdit_->text());
  p->language = twLangCombo_->currentData().toString().toStdString();
  p->applyInfoOnGoLive = twApplyOnLiveCheck_->isChecked();
  if (twCategoryCombo_->currentIndex() >= 0) {
    p->categoryId = twCategoryCombo_->currentData().toString().toStdString();
    p->categoryName = twCategoryCombo_->currentText().toStdString();
  } else {
    p->categoryId.clear();
    p->categoryName.clear();
  }
  vv::saveUiPrefs(*prefs_);
}

void MainPanel::setTwitchIngest(const QString& url) {
  auto* p = twitchProfile();
  if (!p || url.isEmpty()) return;
  p->streamUrl = url.toStdString();
  vv::saveUiPrefs(*prefs_);
  loadTwitchFields();
  if (activeChannelId() == s2q(p->id)) {
    refreshChannelCombos();
    persist();
  }
}

void MainPanel::syncTwitchUi() {
  if (!twStatusLabel_) return;
  vv::json st = twitchStatus();
  twSigned_ = st.value("signedIn", false);
  QString login = s2q(st.value("login", std::string()));
  twHasProfile_ = !st.value("channel", std::string()).empty();
  twStatusLabel_->setText(twSigned_ ? "SIGNED IN AS " + login : (twHasProfile_ ? "SIGNED OUT (" + login + ")" : QString("NOT CONNECTED")));
  bool keySet = false;
  if (twHasProfile_)
    keySet = app_.handle({{"cmd", "get_config"}, {"channel", twitchChannelId().toStdString()}})
                 .value("config", vv::json::object()).value("streamKeySet", false);
  twKeyStateLabel_->setText(keySet ? "Stream key: SET" : "Stream key: NOT SET");
  if (twitchChannelId() != twLoadedId_) loadTwitchFields();
  applyTwitchEnabled(recState() == RecState::Idle && !starting_);
}

void MainPanel::applyTwitchEnabled(bool canEdit) {
  if (!twConnectBtn_) return;
  const bool prof = canEdit && twHasProfile_;
  const bool net = prof && twSigned_;
  twClientIdEdit_->setEnabled(canEdit);
  twConnectBtn_->setEnabled(canEdit && !twSigned_);
  twSignOutBtn_->setEnabled(canEdit && twSigned_);
  for (QWidget* w : std::initializer_list<QWidget*>{twIngestCombo_, twUrlEdit_, twKeyEdit_, twKeySave_, twKeyClear_, twTitleEdit_,
                                                    twTagsEdit_, twLangCombo_, twCategoryCombo_, twApplyOnLiveCheck_})
    w->setEnabled(prof);
  for (QWidget* w : std::initializer_list<QWidget*>{twIngestRefreshBtn_, twFetchKeyBtn_, twCategoryEdit_, twCategorySearchBtn_,
                                                    twLoadInfoBtn_, twApplyBtn_})
    w->setEnabled(net);
  twGoLiveBtn_->setEnabled(prof);
  twDashboardBtn_->setEnabled(twHasProfile_);
  twVideoProducerBtn_->setEnabled(twHasProfile_);
  twLocalCopyCheck_->setEnabled(canEdit);
}

void MainPanel::onPrefsReloaded() {
  refreshChannelCombos();
  syncKeyUi();
  QWidget* f = focusWidget();
  const bool typing = f && (f == twTitleEdit_ || f == twTagsEdit_ || f == twCategoryEdit_ || f == twUrlEdit_);
  if (!typing) twLoadedId_.clear();  // reload the fields from the new ui.json unless the user is typing in them
  syncTwitchUi();
}

// ---------------------------------------------------------------- Twitch actions

void MainPanel::twitchConnect() {
  const QString cid = twClientIdEdit_->text().trimmed();
  if (prefs_) {
    prefs_->twitchClientId = cid.toStdString();
    vv::saveUiPrefs(*prefs_);
  }
  showError({});
  runAsync([this, cid] { return app_.handle({{"cmd", "twitch_auth_start"}, {"clientId", cid.toStdString()}}); },
           [this](const vv::json& r) {
             if (!r.value("ok", false)) { showError(s2q(r.value("error", std::string("Twitch sign-in failed")))); return; }
             const QString code = s2q(r.value("userCode", std::string()));
             const QString uri = s2q(r.value("verificationUri", std::string()));
             if (twAuthBox_) {
               QObject::disconnect(twAuthBox_, nullptr, this, nullptr);
               twAuthBox_->close();
             }
             auto* mb = new QMessageBox(QMessageBox::Information, "Twitch sign-in",
                                        QString("Confirm the code %1 in the browser.\nIf the page did not open, go to %2").arg(code, uri),
                                        QMessageBox::Cancel, this);
             mb->setAttribute(Qt::WA_DeleteOnClose);
             mb->setWindowModality(Qt::NonModal);
             connect(mb, &QDialog::rejected, this, [this] {
               twAuthTimer_->stop();
               runAsync([this] { return app_.handle({{"cmd", "twitch_auth_cancel"}}); }, [](const vv::json&) {});
             });
             twAuthBox_ = mb;
             mb->show();
             openExternal(QUrl(uri));
             twAuthTimer_->start(1000);
           });
}

void MainPanel::twitchSignOut() {
  runAsync([this] { return app_.handle({{"cmd", "twitch_sign_out"}}); }, [this](const vv::json&) {
    syncKeyUi();
    syncTwitchUi();
    emit changed();
  });
}

void MainPanel::twitchFetchKey() {
  const QString id = twitchChannelId();
  if (id.isEmpty()) { showError("Twitch: sign in first"); return; }
  runAsync([this, id] { return app_.handle({{"cmd", "twitch_fetch_key"}, {"channel", id.toStdString()}}); },
           [this](const vv::json& r) {
             if (!r.value("ok", false)) { showError(s2q(r.value("error", std::string("Twitch: cannot fetch the stream key")))); return; }
             showError({});
             refreshChannelCombos();
             syncKeyUi();
             emit changed();
           });
}

void MainPanel::twitchApplyInfo(std::function<void(bool)> done) {
  saveTwitchFields();
  const auto* p = twitchProfile();
  if (!p) { showError("Twitch: sign in first"); if (done) done(false); return; }
  vv::json req = {{"cmd", "twitch_set_info"}, {"channel", p->id},           {"title", p->title},
                  {"categoryId", p->categoryId}, {"categoryName", p->categoryName}, {"language", p->language},
                  {"tags", p->tags}};
  twInfoStatusLabel_->setText("APPLYING...");
  runAsync([this, req] { return app_.handle(req); }, [this, done](const vv::json& r) {
    const bool ok = r.value("ok", false);
    const QString err = s2q(r.value("error", std::string("Twitch: the info was refused")));
    twInfoStatusLabel_->setText(ok ? QString("APPLIED TO TWITCH") : "NOT APPLIED: " + err);
    showError(ok ? QString() : err);
    if (done) done(ok);
  });
}

void MainPanel::twitchPromptTitle() {
  auto* p = twitchProfile();
  if (!p) return;
  bool ok = false;
  QString t = QInputDialog::getText(this, "Twitch title", "Stream title (up to 140 characters):", QLineEdit::Normal,
                                    s2q(p->title), &ok).trimmed();
  if (!ok || t.isEmpty()) return;
  p->title = t.toStdString();
  vv::saveUiPrefs(*prefs_);
  loadTwitchFields();
  twitchApplyInfo();
}

void MainPanel::twitchPromptCategory() {
  if (!twitchProfile()) return;
  bool ok = false;
  QString q = QInputDialog::getText(this, "Twitch category", "Search a category or game:", QLineEdit::Normal, "", &ok).trimmed();
  if (!ok || q.isEmpty()) return;
  runAsync([this, q] { return app_.handle({{"cmd", "twitch_search_categories"}, {"query", q.toStdString()}}); },
           [this, q](const vv::json& r) {
             const vv::json cats = r.value("categories", vv::json::array());
             if (!r.value("ok", false) || cats.empty()) {
               showError(r.value("ok", false) ? "Twitch: no category matches '" + q + "'"
                                              : s2q(r.value("error", std::string("Twitch: category search failed"))));
               return;
             }
             if (auto* p = twitchProfile()) {
               p->categoryId = cats[0].value("id", std::string());
               p->categoryName = cats[0].value("name", std::string());
               vv::saveUiPrefs(*prefs_);
             }
             loadTwitchFields();
             twitchApplyInfo();
           });
}

bool MainPanel::twitchKeySet() {
  if (twitchChannelId().isEmpty()) return false;
  return app_.handle({{"cmd", "get_config"}, {"channel", twitchChannelId().toStdString()}})
      .value("config", vv::json::object()).value("streamKeySet", false);
}

bool MainPanel::twitchApplyOnLive() const {
  const auto* p = twitchProfile();
  return p ? p->applyInfoOnGoLive : true;
}

void MainPanel::setTwitchApplyOnLive(bool on) {
  auto* p = twitchProfile();
  if (!p) return;
  p->applyInfoOnGoLive = on;
  vv::saveUiPrefs(*prefs_);
  if (twApplyOnLiveCheck_ && twApplyOnLiveCheck_->isChecked() != on) {
    QSignalBlocker b(twApplyOnLiveCheck_);
    twApplyOnLiveCheck_->setChecked(on);
  }
}

void MainPanel::openVideoProducer() {
  if (const auto* p = twitchProfile())
    openExternal(QUrl("https://dashboard.twitch.tv/u/" + s2q(p->twitchLogin) + "/content/video-producer"));
}

// ---------------------------------------------------------------- test hooks

vv::json MainPanel::twitchUiState() const {
  return {{"statusText", twStatusLabel_->text().toStdString()},
          {"keyText", twKeyStateLabel_->text().toStdString()},
          {"title", twTitleEdit_->text().toStdString()},
          {"tags", twTagsEdit_->text().toStdString()},
          {"language", twLangCombo_->currentData().toString().toStdString()},
          {"category", twCategoryCombo_->currentText().toStdString()},
          {"clientId", twClientIdEdit_->text().toStdString()},
          {"connectEnabled", twConnectBtn_->isEnabled()},
          {"signOutEnabled", twSignOutBtn_->isEnabled()},
          {"applyEnabled", twApplyBtn_->isEnabled()},
          {"infoStatus", twInfoStatusLabel_->text().toStdString()},
          {"liveLink", twShare_.live->text().toStdString()},
          {"vodLink", twShare_.vod->text().toStdString()},
          {"authDialog", twAuthBox_ ? twAuthBox_->text().toStdString() : std::string()},
          {"openedUrl", lastOpenedUrl().toStdString()}};
}

vv::json MainPanel::youtubeUiState() const {
  return {{"liveLink", ytShare_.live->text().toStdString()}, {"vodLink", ytShare_.vod->text().toStdString()}};
}

void MainPanel::uiTwitchConnect(const QString& clientId) {
  if (!clientId.isEmpty()) twClientIdEdit_->setText(clientId);
  twitchConnect();
}

void MainPanel::uiTwitchSet(const vv::json& f) {
  if (f.contains("title")) twTitleEdit_->setText(s2q(f.value("title", std::string())));
  if (f.contains("tags")) twTagsEdit_->setText(s2q(f.value("tags", std::string())));
  if (f.contains("language")) {
    int i = twLangCombo_->findData(s2q(f.value("language", std::string())));
    if (i >= 0) twLangCombo_->setCurrentIndex(i);
  }
  if (f.contains("categoryId")) {
    QSignalBlocker b(twCategoryCombo_);
    twCategoryCombo_->clear();
    twCategoryCombo_->addItem(s2q(f.value("categoryName", f.value("categoryId", std::string()))), s2q(f.value("categoryId", std::string())));
    twCategoryCombo_->setCurrentIndex(0);
  }
  saveTwitchFields();
}

}  // namespace vv::ui
