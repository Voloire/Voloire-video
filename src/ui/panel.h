#pragma once
#include <QPointer>
#include <QWidget>
#include <functional>
#include <thread>
#include "app_controller.h"
#include "devices.h"

class QCheckBox;
class QFrame;
class QGridLayout;
class QMessageBox;
class QUrl;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;
class QSlider;
class QSpinBox;
class QTabWidget;
class QTimer;

namespace vv::ui {

class LevelMeter;

// A titled card of the panel (shared by the tab builders).
QFrame* makeCard(const QString& title, QLayout* inner, QWidget* parent);

// The configuration panel. It is also the model the tray menu edits: every setting lives in
// one of its controls, and RecordConfig is derived from them on demand.
class MainPanel : public QWidget {
  Q_OBJECT
 public:
  explicit MainPanel(vv::AppController& app, QWidget* parent = nullptr);
  ~MainPanel() override;

  vv::RecordConfig currentConfig() const;
  void setOverride(const vv::json& j) { override_ = j; emit changed(); }  // test hook: force config fields
  void setPrefs(vv::UiPrefs* p);  // shared with the status bar: one source of truth for ui.json
  void setDarkMode(bool dark);
  bool darkMode() const;
  void setActiveTab(int index);

  void refreshSources();
  void selectWindow(qint64 hwnd);   // 0 = follow the active window
  void setVideoSource(const QString& kind, int monitor = 0);  // window | monitor | dxgi
  void setContainer(const QString& v);
  void setCodec(const QString& v);
  void setHeight(int h);
  void setFps(int fps);
  void setQuality(const QString& v);  // high | standard | light | minimal | custom
  void setMic(const QString& mode, const QString& id = {});   // default | device | none
  void setSys(const QString& mode, const QString& id = {});   // app | system | none
  void setOutput(const QString& v);   // file | stream
  QString outputKind() const;

  // Streaming: the key is held by the controller (DPAPI, never in ui.json).
  bool streamKeySet();
  bool setStreamKey(const std::string& key);  // empty clears it
  void clearStreamKey();
  void promptStreamKey();                     // masked dialog, from the YouTube menu
  vv::json streamStatus() const;

  // Multi-channel YouTube destinations
  std::vector<vv::StreamChannel> channels() const;
  QString activeChannelId() const;
  QString activeChannelLabel() const;
  void selectChannel(const QString& id);
  void promptAddChannel();
  void deleteActiveChannel();
  void openStudio(const QString& platform = {});  // YouTube Studio or the Twitch stream manager; empty = the active channel's platform
  QString activeChannelPlatform() const;  // youtube | twitch
  bool setStreamKeyFor(const QString& channelId, const std::string& key);

  // Twitch (panel_twitch.cpp)
  vv::json twitchStatus();                       // twitch_status: sign-in state and profile, never a token
  void twitchConnect();                          // browser sign-in: code dialog, verification page, poll until done
  void twitchSignOut();
  bool twitchKeySet();                           // the Twitch profile has a stored stream key
  bool twitchApplyOnLive() const;
  void setTwitchApplyOnLive(bool on);
  void twitchFetchKey();                         // the stream key of the account, stored encrypted
  void twitchApplyInfo(std::function<void(bool)> done = nullptr);  // title, category, tags, language -> Twitch
  void twitchPromptTitle();
  void twitchPromptCategory();
  void openVideoProducer();
  void onPrefsReloaded();                        // ui.json was rewritten by the controller
  vv::json twitchUiState() const;                // what the Twitch tab shows, for the tests
  void uiTwitchConnect(const QString& clientId);
  void uiTwitchSet(const vv::json& fields);

  // Share links of the current stream and the Share link menu entries
  vv::json shareLinks() const;                   // links: platform, liveUrl, vodUrl, state, error
  QString liveLink() const;                      // of the active channel: the tracker's, else computed from its profile
  QString vodLink() const;
  void copyLiveLink();
  void copyVodLink();
  void openLiveLink();
  void openVodLink();
  static void openExternal(const QUrl& u);       // QDesktopServices::openUrl unless VV_NO_BROWSER is set

  // Audio mix & mute
  bool sysMuted() const;
  bool micMuted() const;
  void setSysMuted(bool on);
  void setMicMuted(bool on);

  // Share copy of the last recording: the target size and the clipboard live in ui.json.
  void setShareTarget(int mb);   // 0 = off
  void setShareToClipboard(bool on);
  void shareLastRecording();
  int shareTargetMB() const { return prefs_ ? prefs_->shareTargetMB : 0; }
  bool shareToClipboard() const { return prefs_ && prefs_->shareToClipboard; }
  QString lastRecording() const { return lastRecording_; }
  vv::json shareStatus();

  void startRecording();
  void stopRecording();
  void togglePause();

  vv::RecState recState() const;
  bool starting() const { return starting_; }
  QString timerText() const;
  QString infoText() const;
  QString errorText() const { return error_; }
  const std::vector<vv::WindowInfo>& windows() const { return windows_; }
  const std::vector<vv::AudioDevice>& devices() const { return devices_; }
  const std::vector<vv::MonitorInfo>& monitors() const { return monitors_; }
  qint64 selectedHwnd() const;
  QString videoKind() const;
  int monitorIndex() const;
  QString sysKey() const;
  QString micKey() const;
  double meterRms(bool sys) const;

 signals:
  void changed();  // state or configuration changed: tray refreshes

 protected:
  bool eventFilter(QObject* o, QEvent* e) override;
  void mousePressEvent(QMouseEvent* e) override;
  void mouseMoveEvent(QMouseEvent* e) override;

 private:
  void build();
  void poll();
  void trackForeground();
  void persist();
  void useActiveNow();
  void showError(const QString& m);
  void startShare(const QString& path, int mb);
  void buildTwitchTab();
  struct ShareWidgets {
    QLabel *live = nullptr, *vod = nullptr;
    QPushButton *copyLive = nullptr, *openLive = nullptr, *copyVod = nullptr, *openVod = nullptr;
  };
  QGridLayout* buildShareGrid(ShareWidgets& w, const QString& platform);
  QString liveLinkFor(const QString& platform) const;
  QString vodLinkFor(const QString& platform) const;
  void refreshShareLinks();
  void copyText(const QString& t);
  void syncTwitchUi();
  void applyTwitchEnabled(bool canEdit);
  void loadTwitchFields();
  void saveTwitchFields();
  void setTwitchIngest(const QString& url);
  const vv::StreamChannel* twitchProfile() const;
  vv::StreamChannel* twitchProfile();
  QString twitchChannelId() const;
  void runAsync(std::function<vv::json()> work, std::function<void(const vv::json&)> done);
  void syncKeyUi();
  void refreshChannelCombos();
  void applyAudioMix();
  void stopIdleAudio();
  void pollIdleAudio();
  vv::WindowInfo findWindow(qint64 hwnd) const;

  vv::AppController& app_;
  std::vector<vv::WindowInfo> windows_;
  std::vector<vv::AudioDevice> devices_;
  std::vector<vv::MonitorInfo> monitors_;
  vv::WindowInfo lastForeground_;
  QString error_;
  QString lastRecording_;      // path of the last finished recording, for "Make share copy"
  vv::json override_ = vv::json::object();
  vv::UiPrefs* prefs_ = nullptr;  // owned by main: tray and status bar read the same ui.json
  bool starting_ = false;
  bool loading_ = false;
  std::thread startTh_;

  std::unique_ptr<vv::IAudioSource> idleSys_, idleMic_;
  QString idleSysKey_, idleMicKey_;

  QTabWidget* tabs_ = nullptr;
  QPushButton* themeBtn_ = nullptr;
  QRadioButton *rbWindow_, *rbMonitor_, *rbDxgi_;
  QComboBox *winCombo_, *monCombo_, *sysCombo_, *micCombo_, *containerCombo_, *codecCombo_, *encoderCombo_, *resCombo_,
      *fpsCombo_, *qualityCombo_, *shareCombo_, *outputCombo_;
  QComboBox *channelCombo_ = nullptr, *ytChannelCombo_ = nullptr;
  QComboBox *ytOutputCombo_ = nullptr, *streamServerCombo_ = nullptr, *streamPresetCombo_ = nullptr,
            *streamResCombo_ = nullptr, *ytFpsCombo_ = nullptr, *ytEncoderCombo_ = nullptr,
            *ytSysCombo_ = nullptr, *ytMicCombo_ = nullptr;
  QSpinBox *bitrate_, *streamKbps_, *ytKbps_ = nullptr;
  QLineEdit *dirEdit_, *urlEdit_, *keyEdit_, *ytUrlEdit_ = nullptr, *ytKeyEdit_ = nullptr;
  QLineEdit *ytChannelLabelEdit_ = nullptr, *ytStudioUrlEdit_ = nullptr;
  QPushButton *keySave_, *keyClear_ = nullptr, *studioBtn_ = nullptr,
              *ytKeySave_ = nullptr, *ytKeyClear_ = nullptr, *ytStudioBtn_ = nullptr,
              *addChBtn_ = nullptr, *ytAddChBtn_ = nullptr, *ytDelChBtn_ = nullptr;
  QCheckBox *localCopyCheck_ = nullptr, *ytLocalCopyCheck_ = nullptr;
  QPushButton *sysMuteBtn_ = nullptr, *micMuteBtn_ = nullptr, *ytSysMuteBtn_ = nullptr, *ytMicMuteBtn_ = nullptr;
  QSlider *sysVolSlider_ = nullptr, *micVolSlider_ = nullptr, *ytSysVolSlider_ = nullptr, *ytMicVolSlider_ = nullptr;
  QLabel *sysVolLabel_ = nullptr, *micVolLabel_ = nullptr, *ytSysVolLabel_ = nullptr, *ytMicVolLabel_ = nullptr;
  QLabel *keyStateLabel_ = nullptr, *ytKeyStateLabel_ = nullptr;
  QWidget* streamCard_;  // the Stream group in the Recording tab (always visible)

  // TWITCH STREAM tab
  QLineEdit *twClientIdEdit_ = nullptr, *twUrlEdit_ = nullptr, *twKeyEdit_ = nullptr, *twTitleEdit_ = nullptr,
            *twCategoryEdit_ = nullptr, *twTagsEdit_ = nullptr;
  QPushButton *twConnectBtn_ = nullptr, *twSignOutBtn_ = nullptr, *twIngestRefreshBtn_ = nullptr, *twFetchKeyBtn_ = nullptr,
              *twKeySave_ = nullptr, *twKeyClear_ = nullptr, *twDashboardBtn_ = nullptr, *twCategorySearchBtn_ = nullptr,
              *twLoadInfoBtn_ = nullptr, *twApplyBtn_ = nullptr, *twGoLiveBtn_ = nullptr, *twVideoProducerBtn_ = nullptr,
              *twOpenFolderBtn_ = nullptr;
  QComboBox *twIngestCombo_ = nullptr, *twCategoryCombo_ = nullptr, *twLangCombo_ = nullptr;
  QLabel *twStatusLabel_ = nullptr, *twKeyStateLabel_ = nullptr, *twTitleCount_ = nullptr, *twInfoStatusLabel_ = nullptr;
  QCheckBox *twApplyOnLiveCheck_ = nullptr, *twLocalCopyCheck_ = nullptr;
  ShareWidgets twShare_;
  QTimer* twAuthTimer_ = nullptr;
  QPointer<QMessageBox> twAuthBox_;
  bool twSigned_ = false, twHasProfile_ = false;
  QString twLoadedId_;
  qint64 lastTwSyncMs_ = 0, lastLinksMs_ = 0;
  LevelMeter *sysMeter_, *micMeter_, *ytSysMeter_ = nullptr, *ytMicMeter_ = nullptr;
  QLabel *timerLabel_, *stateLabel_, *infoLabel_, *errLabel_;
  QPushButton *recBtn_, *pauseBtn_;
  QTimer *pollTimer_, *fgTimer_;
  QWidget* titleBar_;
  QPoint dragOrigin_;
};

}  // namespace vv::ui
