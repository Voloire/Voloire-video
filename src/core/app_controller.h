#pragma once
#include <atomic>
#include <functional>
#include <mutex>
#include <thread>
#include "config.h"
#include "recorder.h"
#include "share_copy.h"

namespace vv {

// The one command surface of the app. The tray UI, the headless CLI and the named pipe all
// drive the recorder through handle(), so what a test exercises is what a user gets.
class AppController {
 public:
  AppController();
  ~AppController();
  json handle(const json& req);

  Recorder& recorder() { return rec_; }
  RecordConfig config() const;
  void setConfig(const RecordConfig& c);

  // Fills hwnd / pid from "windowTitle" / "windowExe" / "foreground" hints; returns an error or "".
  std::string resolveTarget(RecordConfig& c, const json& hints) const;

  std::function<void()> onShutdown;                       // "shutdown" command
  std::function<json(const json&)> extra;                 // UI-only commands; return null json if unhandled

 private:
  json shareStatus();
  json shareStart(const std::string& path, int targetMB);

  Recorder rec_;
  mutable std::mutex mu_;
  RecordConfig cfg_;

  // One share-copy job at a time, driven from its own thread.
  std::mutex shareMu_;
  std::string shareState_ = "idle";  // idle | running | done | error
  std::string sharePath_, shareError_;
  double shareProgress_ = 0;
  uint64_t shareBytes_ = 0;
  std::atomic<bool> shareCancel_{false};
  std::thread shareThread_;
};

}  // namespace vv
