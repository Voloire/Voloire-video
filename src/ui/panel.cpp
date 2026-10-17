#include "panel.h"
#include <QApplication>
#include <QComboBox>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QRadioButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStyle>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWindow>
#include <windows.h>
#include "telemetry.h"
#include "theme.h"

namespace vv::ui {

namespace {

QFrame* makeCard(const QString& title, QLayout* inner, QWidget* parent) {
  auto* f = new QFrame(parent);
  f->setObjectName("card");
  auto* v = new QVBoxLayout(f);
  v->setContentsMargins(14, 10, 14, 12);
  v->setSpacing(8);
  auto* t = new QLabel(title, f);
  t->setObjectName("section");
  v->addWidget(t);
  v->addLayout(inner);
  return f;
}

QString fmtDuration(int64_t us) {
  int64_t s = us / 1000000;
  return QString("%1:%2:%3").arg(s / 3600, 2, 10, QChar('0')).arg((s / 60) % 60, 2, 10, QChar('0')).arg(s % 60, 2, 10, QChar('0'));
}

}  // namespace

MainPanel::MainPanel(vv::AppController& app, QWidget* parent) : QWidget(parent), app_(app) {
  setObjectName("root");
  setAttribute(Qt::WA_StyledBackground, true);
  setWindowFlags(Qt::FramelessWindowHint | Qt::Window);
  setWindowTitle("Voloire-video");
  setWindowIcon(trayIcon(IconState::Recording));
  resize(600, 920);
  build();
  refreshSources();

  pollTimer_ = new QTimer(this);
  connect(pollTimer_, &QTimer::timeout, this, &MainPanel::poll);
  pollTimer_->start(100);
  fgTimer_ = new QTimer(this);
  connect(fgTimer_, &QTimer::timeout, this, &MainPanel::trackForeground);
  fgTimer_->start(400);
}

MainPanel::~MainPanel() {
  if (startTh_.joinable()) startTh_.join();
}

void MainPanel::build() {
  loading_ = true;
  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(1, 1, 1, 1);
  root->setSpacing(0);

  // Title bar
  titleBar_ = new QWidget(this);
  auto* th = new QHBoxLayout(titleBar_);
  th->setContentsMargins(18, 14, 10, 8);
  auto* tcol = new QVBoxLayout();
  auto* title = new QLabel("VOLOIRE//VIDEO", titleBar_);
  title->setObjectName("title");
  auto* sub = new QLabel("WINDOW + APP AUDIO RECORDER", titleBar_);
  sub->setObjectName("sub");
  tcol->addWidget(title);
  tcol->addWidget(sub);
  th->addLayout(tcol, 1);
  auto* minBtn = new QPushButton(QString::fromUtf8("–"), titleBar_);
  minBtn->setObjectName("flat");
  connect(minBtn, &QPushButton::clicked, this, &QWidget::hide);
  auto* closeBtn = new QPushButton(QString::fromUtf8("✕"), titleBar_);
  closeBtn->setObjectName("flat");
  connect(closeBtn, &QPushButton::clicked, this, &QWidget::hide);
  th->addWidget(minBtn);
  th->addWidget(closeBtn);
  titleBar_->installEventFilter(this);
  root->addWidget(titleBar_);

  auto* body = new QVBoxLayout();
  body->setContentsMargins(16, 6, 16, 16);
  body->setSpacing(12);
  root->addLayout(body, 1);

  // Timer and controls
  {
    auto* box = new QVBoxLayout();
    timerLabel_ = new QLabel("00:00:00", this);
    timerLabel_->setAlignment(Qt::AlignCenter);
    QFont f(monoFamily());
    f.setPixelSize(58);
    f.setBold(true);
    timerLabel_->setFont(f);
    timerLabel_->setStyleSheet(QString("color:%1; font-size:58px; font-weight:700; font-family:'%2';").arg(col::dim.name(), monoFamily()));
    stateLabel_ = new QLabel("IDLE", this);
    stateLabel_->setObjectName("status");
    stateLabel_->setAlignment(Qt::AlignCenter);
    infoLabel_ = new QLabel("", this);
    infoLabel_->setObjectName("sub");
    infoLabel_->setAlignment(Qt::AlignCenter);
    errLabel_ = new QLabel("", this);
    errLabel_->setObjectName("err");
    errLabel_->setAlignment(Qt::AlignCenter);
    errLabel_->setWordWrap(true);
    errLabel_->hide();
    auto* btns = new QHBoxLayout();
    recBtn_ = new QPushButton("REC", this);
    recBtn_->setObjectName("rec");
    pauseBtn_ = new QPushButton("PAUSE", this);
    btns->addWidget(recBtn_, 2);
    btns->addWidget(pauseBtn_, 1);
    box->addWidget(timerLabel_);
    box->addWidget(stateLabel_);
    box->addWidget(infoLabel_);
    box->addWidget(errLabel_);
    box->addLayout(btns);
    body->addWidget(makeCard("RECORDER", box, this));
    connect(recBtn_, &QPushButton::clicked, this, [this] {
      if (recState() == RecState::Idle) startRecording(); else stopRecording();
    });
    connect(pauseBtn_, &QPushButton::clicked, this, &MainPanel::togglePause);
  }

  // Source
  {
    auto* g = new QGridLayout();
    g->setHorizontalSpacing(10);
    rbWindow_ = new QRadioButton("Window", this);
    rbMonitor_ = new QRadioButton("Monitor", this);
    rbDxgi_ = new QRadioButton("Monitor (exclusive fullscreen)", this);
    rbWindow_->setChecked(true);
    winCombo_ = new QComboBox(this);
    monCombo_ = new QComboBox(this);
    auto* refresh = new QPushButton("REFRESH", this);
    auto* active = new QPushButton("ACTIVE", this);
    active->setToolTip("Capture the window that was active before this panel");
    g->addWidget(rbWindow_, 0, 0);
    g->addWidget(winCombo_, 0, 1);
    g->addWidget(active, 0, 2);
    g->addWidget(refresh, 0, 3);
    g->addWidget(rbMonitor_, 1, 0);
    g->addWidget(monCombo_, 1, 1, 1, 3);
    g->addWidget(rbDxgi_, 2, 0, 1, 4);
    g->setColumnStretch(1, 1);
    body->addWidget(makeCard("VIDEO SOURCE", g, this));
    connect(refresh, &QPushButton::clicked, this, &MainPanel::refreshSources);
    connect(active, &QPushButton::clicked, this, &MainPanel::useActiveNow);
    for (auto* rb : {rbWindow_, rbMonitor_, rbDxgi_}) connect(rb, &QRadioButton::toggled, this, [this] { persist(); });
    connect(winCombo_, &QComboBox::currentIndexChanged, this, [this] { persist(); });
    connect(monCombo_, &QComboBox::currentIndexChanged, this, [this] { persist(); });
  }

  // Audio
  {
    auto* g = new QGridLayout();
    g->setHorizontalSpacing(10);
    sysCombo_ = new QComboBox(this);
    micCombo_ = new QComboBox(this);
    sysMeter_ = new LevelMeter(this);
    micMeter_ = new LevelMeter(this);
    auto* l1 = new QLabel("SYS", this);
    auto* l2 = new QLabel("MIC", this);
    l1->setObjectName("status");
    l2->setObjectName("status");
    g->addWidget(l1, 0, 0);
    g->addWidget(sysCombo_, 0, 1);
    g->addWidget(sysMeter_, 1, 1);
    g->addWidget(l2, 2, 0);
    g->addWidget(micCombo_, 2, 1);
    g->addWidget(micMeter_, 3, 1);
    g->setColumnStretch(1, 1);
    body->addWidget(makeCard("AUDIO CHANNELS", g, this));
    connect(sysCombo_, &QComboBox::currentIndexChanged, this, [this] { persist(); });
    connect(micCombo_, &QComboBox::currentIndexChanged, this, [this] { persist(); });
  }

  // Output
  {
    auto* g = new QGridLayout();
    g->setHorizontalSpacing(10);
    g->setVerticalSpacing(8);
    containerCombo_ = new QComboBox(this);
    containerCombo_->addItem("MKV (crash safe)", "mkv");
    containerCombo_->addItem("MP4", "mp4");
    codecCombo_ = new QComboBox(this);
    codecCombo_->addItem("H.264", "h264");
    codecCombo_->addItem("HEVC (H.265)", "hevc");
    codecCombo_->addItem("AV1", "av1");
    encoderCombo_ = new QComboBox(this);
    encoderCombo_->addItem("Auto", "auto");
    encoderCombo_->addItem("NVIDIA NVENC", "nvenc");
    encoderCombo_->addItem("AMD AMF", "amf");
    encoderCombo_->addItem("Intel QSV", "qsv");
    encoderCombo_->addItem("Media Foundation", "mf");
    resCombo_ = new QComboBox(this);
    resCombo_->addItem("Source size", 0);
    for (int h : {2160, 1440, 1080, 720, 480}) resCombo_->addItem(QString("%1p").arg(h), h);
    fpsCombo_ = new QComboBox(this);
    for (int f : {24, 30, 60, 120}) fpsCombo_->addItem(QString("%1 fps").arg(f), f);
    fpsCombo_->setCurrentIndex(2);
    bitrate_ = new QSpinBox(this);
    bitrate_->setRange(500, 200000);
    bitrate_->setSingleStep(1000);
    bitrate_->setValue(12000);
    bitrate_->setSuffix(" kbps");
    qualityCombo_ = new QComboBox(this);
    qualityCombo_->addItem("High", "high");
    qualityCombo_->addItem("Standard", "standard");
    qualityCombo_->addItem("Light", "light");
    qualityCombo_->addItem("Minimal", "minimal");
    qualityCombo_->addItem("Custom", "custom");
    shareCombo_ = new QComboBox(this);
    shareCombo_->addItem("Off", 0);
    for (int mb : {10, 25, 50, 100}) shareCombo_->addItem(QString("%1 MB").arg(mb), mb);
    outputCombo_ = new QComboBox(this);
    outputCombo_->addItem("Record to file", "file");
    outputCombo_->addItem("Stream to YouTube", "stream");
    dirEdit_ = new QLineEdit(QString::fromStdString(app_.config().outDir), this);
    auto* browse = new QPushButton("...", this);
    auto* open = new QPushButton("OPEN", this);
    auto lab = [&](const char* t) { auto* l = new QLabel(t, this); l->setObjectName("sub"); return l; };
    g->addWidget(lab("CONTAINER"), 0, 0);
    g->addWidget(containerCombo_, 1, 0);
    g->addWidget(lab("CODEC"), 0, 1);
    g->addWidget(codecCombo_, 1, 1);
    g->addWidget(lab("ENCODER"), 0, 2);
    g->addWidget(encoderCombo_, 1, 2);
    g->addWidget(lab("RESOLUTION"), 2, 0);
    g->addWidget(resCombo_, 3, 0);
    g->addWidget(lab("FRAME RATE"), 2, 1);
    g->addWidget(fpsCombo_, 3, 1);
    g->addWidget(lab("BITRATE"), 2, 2);
    g->addWidget(bitrate_, 3, 2);
    g->addWidget(lab("QUALITY"), 4, 0);
    g->addWidget(qualityCombo_, 5, 0);
    g->addWidget(lab("SHARE COPY"), 4, 1);
    g->addWidget(shareCombo_, 5, 1);
    g->addWidget(lab("OUTPUT"), 4, 2);
    g->addWidget(outputCombo_, 5, 2);
    auto* dir = new QHBoxLayout();
    dir->addWidget(dirEdit_, 1);
    dir->addWidget(browse);
    dir->addWidget(open);
    g->addWidget(lab("OUTPUT FOLDER"), 6, 0, 1, 3);
    g->addLayout(dir, 7, 0, 1, 3);
    body->addWidget(makeCard("OUTPUT", g, this));
    for (auto* c : {containerCombo_, codecCombo_, encoderCombo_, resCombo_, fpsCombo_, qualityCombo_})
      connect(c, &QComboBox::currentIndexChanged, this, [this] { persist(); });
    connect(outputCombo_, &QComboBox::currentIndexChanged, this, [this] {
      streamCard_->setVisible(outputKind() == "stream");
      persist();
    });
    connect(shareCombo_, &QComboBox::currentIndexChanged, this, [this] { setShareTarget(shareCombo_->currentData().toInt()); });
    connect(bitrate_, &QSpinBox::valueChanged, this, [this] {
      if (loading_) return;
      int i = qualityCombo_->findData("custom");  // a hand-typed bitrate means the preset no longer applies
      if (i >= 0 && qualityCombo_->currentIndex() != i) {
        QSignalBlocker b(qualityCombo_);
        qualityCombo_->setCurrentIndex(i);
      }
      persist();
    });
    connect(dirEdit_, &QLineEdit::editingFinished, this, [this] { persist(); });
    connect(browse, &QPushButton::clicked, this, [this] {
      QString d = QFileDialog::getExistingDirectory(this, "Output folder", dirEdit_->text());
      if (!d.isEmpty()) { dirEdit_->setText(d); persist(); }
    });
    connect(open, &QPushButton::clicked, this, [this] { QDesktopServices::openUrl(QUrl::fromLocalFile(dirEdit_->text())); });
  }

  // Stream (YouTube): shown when the output is a stream.
  {
    auto* g = new QGridLayout();
    g->setHorizontalSpacing(10);
    g->setVerticalSpacing(8);
    keyEdit_ = new QLineEdit(this);
    keyEdit_->setEchoMode(QLineEdit::Password);
    keyEdit_->setPlaceholderText("stream key (stored encrypted)");
    keySave_ = new QPushButton("SAVE", this);
    urlEdit_ = new QLineEdit(this);
    urlEdit_->setPlaceholderText("rtmp://a.rtmp.youtube.com/live2");
    streamKbps_ = new QSpinBox(this);
    streamKbps_->setRange(1000, 51000);
    streamKbps_->setSingleStep(1000);
    streamKbps_->setSuffix(" kbps");
    auto lab = [&](const char* t) { auto* l = new QLabel(t, this); l->setObjectName("sub"); return l; };
    auto* keyRow = new QHBoxLayout();
    keyRow->addWidget(keyEdit_, 1);
    keyRow->addWidget(keySave_);
    g->addWidget(lab("STREAM KEY"), 0, 0, 1, 2);
    g->addLayout(keyRow, 1, 0, 1, 2);
    g->addWidget(lab("INGEST URL"), 2, 0);
    g->addWidget(lab("STREAM BITRATE"), 2, 1);
    g->addWidget(urlEdit_, 3, 0);
    g->addWidget(streamKbps_, 3, 1);
    g->setColumnStretch(0, 2);
    g->setColumnStretch(1, 1);
    streamCard_ = makeCard("STREAM", g, this);
    streamCard_->setVisible(false);
    body->addWidget(streamCard_);
    connect(urlEdit_, &QLineEdit::editingFinished, this, [this] { persist(); });
    connect(streamKbps_, &QSpinBox::valueChanged, this, [this] { if (!loading_) persist(); });
    connect(keySave_, &QPushButton::clicked, this, [this] {
      if (keyEdit_->text().isEmpty()) return;
      if (setStreamKey(keyEdit_->text().trimmed().toStdString())) keyEdit_->clear();  // nothing to show back
    });
  }
  body->addStretch(1);
  loading_ = false;

  // Restore saved settings into the controls.
  vv::RecordConfig s = vv::loadSettings();
  loading_ = true;
  auto pick = [](QComboBox* c, const QVariant& v) { int i = c->findData(v); if (i >= 0) c->setCurrentIndex(i); };
  pick(containerCombo_, QString::fromStdString(s.container));
  pick(codecCombo_, QString::fromStdString(s.codec));
  pick(encoderCombo_, QString::fromStdString(s.encoder));
  pick(resCombo_, s.outHeight);
  pick(fpsCombo_, s.fps);
  pick(qualityCombo_, QString::fromStdString(s.quality));
  bitrate_->setValue(s.bitrateKbps);
  pick(outputCombo_, QString::fromStdString(s.output));
  urlEdit_->setText(QString::fromStdString(s.streamUrl));
  streamKbps_->setValue(s.streamBitrateKbps);
  streamCard_->setVisible(s.output == "stream");
  dirEdit_->setText(QString::fromStdString(s.outDir));
  if (s.videoSource == "monitor") rbMonitor_->setChecked(true);
  else if (s.videoSource == "dxgi") rbDxgi_->setChecked(true);
  loading_ = false;
}

void MainPanel::refreshSources() {
  loading_ = true;
  qint64 keepWin = selectedHwnd();
  QString keepSys = sysKey(), keepMic = micKey();
  windows_ = vv::listWindows();
  devices_ = vv::listAudioDevices();
  monitors_ = vv::listMonitors();

  winCombo_->clear();
  winCombo_->addItem("[ active window at record time ]", (qlonglong)0);
  for (auto& w : windows_)
    winCombo_->addItem(QString::fromStdString(w.title + "   [" + w.exe + "]").left(90), (qlonglong)w.hwnd);
  int wi = winCombo_->findData((qlonglong)keepWin);
  winCombo_->setCurrentIndex(wi >= 0 ? wi : 0);

  monCombo_->clear();
  for (auto& m : monitors_)
    monCombo_->addItem(QString("Monitor %1  %2x%3%4").arg(m.index + 1).arg(m.w).arg(m.h).arg(m.primary ? "  (primary)" : ""), m.index);

  sysCombo_->clear();
  sysCombo_->addItem("App audio of the captured window", "app|");
  for (auto& d : devices_)
    if (!d.capture) sysCombo_->addItem(QString::fromStdString("System output: " + d.name + (d.isDefault ? " (default)" : "")), QString::fromStdString("system|" + d.id));
  sysCombo_->addItem("Off", "none|");
  int si = sysCombo_->findData(keepSys.isEmpty() ? "app|" : keepSys);
  sysCombo_->setCurrentIndex(si >= 0 ? si : 0);

  micCombo_->clear();
  micCombo_->addItem("Default microphone", "default|");
  for (auto& d : devices_)
    if (d.capture) micCombo_->addItem(QString::fromStdString(d.name + (d.isDefault ? " (default)" : "")), QString::fromStdString("device|" + d.id));
  micCombo_->addItem("Off", "none|");
  int mi = micCombo_->findData(keepMic.isEmpty() ? "default|" : keepMic);
  micCombo_->setCurrentIndex(mi >= 0 ? mi : 0);
  loading_ = false;
  emit changed();
}

vv::WindowInfo MainPanel::findWindow(qint64 hwnd) const {
  for (auto& w : windows_) if (w.hwnd == hwnd) return w;
  return {};
}

qint64 MainPanel::selectedHwnd() const { return winCombo_ ? winCombo_->currentData().toLongLong() : 0; }
QString MainPanel::sysKey() const { return sysCombo_ ? sysCombo_->currentData().toString() : QString(); }
QString MainPanel::micKey() const { return micCombo_ ? micCombo_->currentData().toString() : QString(); }
QString MainPanel::videoKind() const { return rbMonitor_->isChecked() ? "monitor" : rbDxgi_->isChecked() ? "dxgi" : "window"; }
int MainPanel::monitorIndex() const { return std::max(0, monCombo_->currentData().toInt()); }

vv::RecordConfig MainPanel::currentConfig() const {
  vv::RecordConfig c = app_.config();
  c.videoSource = videoKind().toStdString();
  c.monitor = monitorIndex();
  c.hwnd = 0;
  c.pid = 0;
  if (c.videoSource == "window") {
    vv::WindowInfo w;
    qint64 h = selectedHwnd();
    if (h) w = findWindow(h);
    if (!w.hwnd) w = lastForeground_;  // "active window at record time"
    c.hwnd = w.hwnd;
    c.pid = w.pid;
  }
  QStringList sys = sysKey().split('|');
  c.sys = sys.value(0, "app").toStdString();
  c.sysDevice = sys.value(1).toStdString();
  if (c.sys == "app" && c.pid == 0) c.sys = "system";  // monitor capture has no single app
  QStringList mic = micKey().split('|');
  c.mic = mic.value(0, "default").toStdString();
  c.micDevice = mic.value(1).toStdString();
  c.container = containerCombo_->currentData().toString().toStdString();
  c.codec = codecCombo_->currentData().toString().toStdString();
  c.encoder = encoderCombo_->currentData().toString().toStdString();
  c.outHeight = resCombo_->currentData().toInt();
  c.fps = fpsCombo_->currentData().toInt();
  c.quality = qualityCombo_->currentData().toString().toStdString();
  c.bitrateKbps = bitrate_->value();
  c.output = outputCombo_->currentData().toString().toStdString();
  c.streamUrl = urlEdit_->text().toStdString();
  c.streamBitrateKbps = streamKbps_->value();
  c.outDir = dirEdit_->text().toStdString();
  if (!override_.empty()) {
    vv::json j = c.toJson();
    j.update(override_, true);
    c = vv::RecordConfig::fromJson(j);
  }
  return c;
}

void MainPanel::persist() {
  if (loading_) return;
  vv::RecordConfig c = currentConfig();
  app_.setConfig(c);
  vv::saveSettings(c);
  emit changed();
}

void MainPanel::selectWindow(qint64 hwnd) {
  rbWindow_->setChecked(true);
  int i = winCombo_->findData((qlonglong)hwnd);
  winCombo_->setCurrentIndex(i >= 0 ? i : 0);
}

void MainPanel::setVideoSource(const QString& kind, int monitor) {
  if (kind == "monitor") rbMonitor_->setChecked(true);
  else if (kind == "dxgi") rbDxgi_->setChecked(true);
  else rbWindow_->setChecked(true);
  int i = monCombo_->findData(monitor);
  if (i >= 0) monCombo_->setCurrentIndex(i);
}

void MainPanel::setContainer(const QString& v) { int i = containerCombo_->findData(v); if (i >= 0) containerCombo_->setCurrentIndex(i); }
void MainPanel::setCodec(const QString& v) { int i = codecCombo_->findData(v); if (i >= 0) codecCombo_->setCurrentIndex(i); }
void MainPanel::setHeight(int h) { int i = resCombo_->findData(h); if (i >= 0) resCombo_->setCurrentIndex(i); }
void MainPanel::setFps(int f) { int i = fpsCombo_->findData(f); if (i >= 0) fpsCombo_->setCurrentIndex(i); }
void MainPanel::setMic(const QString& mode, const QString& id) { int i = micCombo_->findData(mode + "|" + id); if (i >= 0) micCombo_->setCurrentIndex(i); }
void MainPanel::setSys(const QString& mode, const QString& id) { int i = sysCombo_->findData(mode + "|" + id); if (i >= 0) sysCombo_->setCurrentIndex(i); }
void MainPanel::setQuality(const QString& v) { int i = qualityCombo_->findData(v); if (i >= 0) qualityCombo_->setCurrentIndex(i); }
void MainPanel::setOutput(const QString& v) { int i = outputCombo_->findData(v); if (i >= 0) outputCombo_->setCurrentIndex(i); }
QString MainPanel::outputKind() const { return outputCombo_ ? outputCombo_->currentData().toString() : QString("file"); }

// The stream key lives in the controller (DPAPI blob under the settings dir): the panel, the
// menu and the HUD only ever learn whether one is set.
bool MainPanel::streamKeySet() {
  vv::json r = app_.handle({{"cmd", "get_config"}});
  return r.value("config", vv::json::object()).value("streamKeySet", false);
}

bool MainPanel::setStreamKey(const std::string& key) {
  vv::json r = app_.handle({{"cmd", "set_stream_key"}, {"key", key}});
  if (!r.value("ok", false)) showError(QString::fromStdString(r.value("error", std::string("cannot store the stream key"))));
  else showError({});
  emit changed();
  return r.value("ok", false);
}

void MainPanel::clearStreamKey() { setStreamKey(""); }

void MainPanel::promptStreamKey() {
  bool ok = false;
  QString k = QInputDialog::getText(this, "Stream key", "Paste the stream key from YouTube Studio > Go live:",
                                    QLineEdit::Password, "", &ok);
  if (ok && !k.trimmed().isEmpty()) setStreamKey(k.trimmed().toStdString());
}

vv::json MainPanel::streamStatus() const { return app_.recorder().status().value("stream", vv::json::object()); }

void MainPanel::setPrefs(vv::UiPrefs* p) {
  prefs_ = p;
  if (shareCombo_ && prefs_) {
    int i = shareCombo_->findData(prefs_->shareTargetMB);
    if (i >= 0 && shareCombo_->currentIndex() != i) {
      QSignalBlocker b(shareCombo_);
      shareCombo_->setCurrentIndex(i);
    }
  }
}

void MainPanel::setShareTarget(int mb) {
  if (prefs_) {
    prefs_->shareTargetMB = mb;
    vv::saveUiPrefs(*prefs_);
  }
  if (shareCombo_) {
    int i = shareCombo_->findData(mb);
    if (i >= 0 && shareCombo_->currentIndex() != i) {
      QSignalBlocker b(shareCombo_);
      shareCombo_->setCurrentIndex(i);
    }
  }
  emit changed();
}

void MainPanel::setShareToClipboard(bool on) {
  if (prefs_) {
    prefs_->shareToClipboard = on;
    vv::saveUiPrefs(*prefs_);
  }
  emit changed();
}

void MainPanel::shareLastRecording() {
  if (lastRecording_.isEmpty()) return;
  startShare(lastRecording_, shareTargetMB() > 0 ? shareTargetMB() : 25);
}

void MainPanel::startShare(const QString& path, int mb) {
  vv::json r = app_.handle({{"cmd", "share"}, {"path", path.toStdString()}, {"targetMB", mb}});
  if (!r.value("ok", false)) showError(QString::fromStdString(r.value("error", std::string("share copy did not start"))));
  emit changed();
}

vv::json MainPanel::shareStatus() {
  vv::json r = app_.handle({{"cmd", "share_status"}});
  return r.contains("share") ? r["share"] : vv::json::object();
}

void MainPanel::useActiveNow() {
  if (lastForeground_.hwnd) {
    refreshSources();
    selectWindow(lastForeground_.hwnd);
  }
}

void MainPanel::trackForeground() {
  vv::WindowInfo w = vv::foregroundWindowInfo();
  if (!w.hwnd || w.pid == GetCurrentProcessId()) return;
  if (w.exe == "explorer.exe" && (w.title == "Program Manager" || w.title.empty())) return;
  lastForeground_ = w;
}

vv::RecState MainPanel::recState() const { return app_.recorder().state(); }

QString MainPanel::timerText() const { return fmtDuration(app_.recorder().elapsedUs()); }

QString MainPanel::infoText() const {
  auto m = vv::Telemetry::get().snapshot();
  auto num = [&](const char* k) { return m.contains(k) ? m[k].get<double>() : 0.0; };
  if (recState() == RecState::Idle) return starting_ ? "STARTING..." : QString();
  return QString("%1 FPS  |  %2 MB  |  DROPPED %3  |  REPEATED %4")
      .arg(num("video.fps_actual"), 0, 'f', 1)
      .arg(num("rec.size_bytes") / 1048576.0, 0, 'f', 1)
      .arg((int)num("video.dropped_ticks"))
      .arg((int)num("video.repeated_frames"));
}

double MainPanel::meterRms(bool sys) const { return (sys ? sysMeter_ : micMeter_)->rmsDb(); }

void MainPanel::showError(const QString& m) {
  error_ = m;
  errLabel_->setText(m);
  errLabel_->setVisible(!m.isEmpty());
  emit changed();
}

void MainPanel::startRecording() {
  if (recState() != RecState::Idle || starting_) return;
  if (startTh_.joinable()) startTh_.join();
  vv::RecordConfig c = currentConfig();
  if (c.videoSource == "window" && !c.hwnd) {
    showError("No window to capture: select one, or activate the target window first.");
    return;
  }
  showError({});
  starting_ = true;
  emit changed();
  startTh_ = std::thread([this, c] {
    std::string err;
    bool ok = app_.recorder().start(c, err);
    QMetaObject::invokeMethod(this, [this, ok, err] {
      starting_ = false;
      if (!ok) showError(QString::fromStdString(err));
      emit changed();
    }, Qt::QueuedConnection);
  });
}

void MainPanel::stopRecording() {
  if (recState() == RecState::Idle) return;
  std::thread([this] {
    vv::RecResult r = app_.recorder().stop();
    QMetaObject::invokeMethod(this, [this, r] {
      if (!r.error.empty()) showError(QString::fromStdString(r.error));
      else if (!r.path.empty()) {
        lastRecording_ = QString::fromStdString(r.path);
        if (shareTargetMB() > 0) startShare(lastRecording_, shareTargetMB());  // Share copy > N MB
      }
      emit changed();
    }, Qt::QueuedConnection);
  }).detach();
}

void MainPanel::togglePause() {
  if (recState() == RecState::Recording) app_.recorder().pause();
  else if (recState() == RecState::Paused) app_.recorder().resume();
  emit changed();
}

void MainPanel::poll() {
  static RecState last = RecState::Idle;
  static bool lastStarting = false;
  RecState s = recState();
  timerLabel_->setText(timerText());
  QString st = s == RecState::Idle ? (starting_ ? "STARTING" : "IDLE") : QString(vv::stateName(s)).toUpper();
  stateLabel_->setText(st);
  infoLabel_->setText(infoText());
  recBtn_->setText(s == RecState::Idle ? "REC" : "STOP");
  recBtn_->setProperty("active", s != RecState::Idle);
  recBtn_->style()->unpolish(recBtn_);
  recBtn_->style()->polish(recBtn_);
  pauseBtn_->setEnabled(s == RecState::Recording || s == RecState::Paused);
  pauseBtn_->setText(s == RecState::Paused ? "RESUME" : "PAUSE");
  timerLabel_->setStyleSheet(QString("color:%1; font-size:58px; font-weight:700; font-family:'%2';")
                                 .arg(s == RecState::Paused ? col::paused.name() : s == RecState::Idle ? col::dim.name() : col::text.name(), monoFamily()));

  auto m = vv::Telemetry::get().snapshot();
  auto num = [&](const char* k, double d) { return m.contains(k) ? m[k].get<double>() : d; };
  bool live = s == RecState::Recording;
  sysMeter_->setLevels(live ? num("audio.sys.rms_db", -120) : -120, live ? num("audio.sys.peak_db", -120) : -120);
  micMeter_->setLevels(live ? num("audio.mic.rms_db", -120) : -120, live ? num("audio.mic.peak_db", -120) : -120);

  if (s != last || starting_ != lastStarting) {
    last = s;
    lastStarting = starting_;
    if (s == RecState::Idle && !app_.recorder().lastError().empty() && error_.isEmpty())
      showError(QString::fromStdString(app_.recorder().lastError()));
    emit changed();
  }
  for (QWidget* w : std::initializer_list<QWidget*>{sysCombo_, micCombo_, containerCombo_, codecCombo_, encoderCombo_, resCombo_, fpsCombo_, qualityCombo_, shareCombo_, winCombo_, monCombo_, outputCombo_, streamKbps_, urlEdit_, keyEdit_, keySave_})
    w->setEnabled(s == RecState::Idle && !starting_);
}

bool MainPanel::eventFilter(QObject* o, QEvent* e) {
  if (o == titleBar_ && e->type() == QEvent::MouseButtonPress) {
    if (auto* w = windowHandle()) w->startSystemMove();
    return true;
  }
  return QWidget::eventFilter(o, e);
}

}  // namespace vv::ui
