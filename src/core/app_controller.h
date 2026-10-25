#pragma once
#include <atomic>
#include <functional>
#include <mutex>
#include <thread>
#include "config.h"
#include "links.h"
#include "recorder.h"
#include "share_copy.h"
#include "twitch.h"

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
  std::function<void()> onPrefsChanged;                   // ui.json was rewritten under the UI; may run on any thread

 private:
  json handleTwitch(const std::string& cmd, const json& req);
  std::string twitchClientId() const;
  StreamChannel twitchProfile(const std::string& channelId = {}) const;
  void upsertTwitchProfile(const TwitchUser& u);
  bool resolveTwitch(TwitchClient& t, const std::string& channelId, StreamChannel& out, std::string& err);
  void twitchAuthLoop(TwitchDeviceCode code);
  json shareStatus();
  json shareStart(const std::string& path, int targetMB);

  LinkTracker links_;  // declared before rec_: the recorder reports its last state change while it is destroyed
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

  // Twitch sign-in: the device-code poll runs on its own thread, its state is read by twitch_status.
  std::mutex twitchMu_;
  std::mutex twitchAuthOpMu_;  // serialize start/cancel/sign-out and ownership of twitchThread_
  json twitchAuth_ = {{"state", "idle"}};
  std::atomic<bool> twitchCancel_{false};
  std::thread twitchThread_;
};

}  // namespace vv
