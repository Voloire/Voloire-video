#pragma once
#include <functional>
#include <mutex>
#include "config.h"
#include "recorder.h"

namespace vv {

// The one command surface of the app. The tray UI, the headless CLI and the named pipe all
// drive the recorder through handle(), so what a test exercises is what a user gets.
class AppController {
 public:
  AppController();
  json handle(const json& req);

  Recorder& recorder() { return rec_; }
  RecordConfig config() const;
  void setConfig(const RecordConfig& c);

  // Fills hwnd / pid from "windowTitle" / "windowExe" / "foreground" hints; returns an error or "".
  std::string resolveTarget(RecordConfig& c, const json& hints) const;

  std::function<void()> onShutdown;                       // "shutdown" command
  std::function<json(const json&)> extra;                 // UI-only commands; return null json if unhandled

 private:
  Recorder rec_;
  mutable std::mutex mu_;
  RecordConfig cfg_;
};

}  // namespace vv
