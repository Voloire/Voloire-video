#pragma once
#include <QWidget>
#include <thread>
#include "app_controller.h"
#include "devices.h"

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;
class QSpinBox;
class QTimer;

namespace vv::ui {

class LevelMeter;

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
  void paintEvent(QPaintEvent*) override;
  bool eventFilter(QObject* o, QEvent* e) override;

 private:
  void build();
  void poll();
  void trackForeground();
  void persist();
  void useActiveNow();
  void showError(const QString& m);
  void startShare(const QString& path, int mb);
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

  QRadioButton *rbWindow_, *rbMonitor_, *rbDxgi_;
  QComboBox *winCombo_, *monCombo_, *sysCombo_, *micCombo_, *containerCombo_, *codecCombo_, *encoderCombo_, *resCombo_,
      *fpsCombo_, *qualityCombo_, *shareCombo_;
  QSpinBox* bitrate_;
  QLineEdit* dirEdit_;
  LevelMeter *sysMeter_, *micMeter_;
  QLabel *timerLabel_, *stateLabel_, *infoLabel_, *errLabel_;
  QPushButton *recBtn_, *pauseBtn_;
  QTimer *pollTimer_, *fgTimer_;
  QWidget* titleBar_;
  QPoint dragOrigin_;
};

}  // namespace vv::ui
