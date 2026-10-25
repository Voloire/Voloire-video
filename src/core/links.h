#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "common.h"
#include "config.h"

namespace vv {

// "UC..." (24 chars) found as is, or right after "/channel/"; empty otherwise.
std::string youtubeChannelId(const std::string& studioUrlOrId);
// <VV_YOUTUBE_BASE or https://www.youtube.com>/channel/<id>/live; empty when the id is empty.
std::string youtubeLiveUrl(const std::string& channelId);
// The video id in the <link rel="canonical" href=".../watch?v=<id>"> of a page; empty when absent.
std::string extractCanonicalVideoId(const std::string& html);
std::string twitchLiveUrl(const std::string& login);  // https://www.twitch.tv/<login>

struct LinkStatus {
  std::string platform, liveUrl, vodUrl, state = "idle", error;  // state: idle | watching | found | ended
  json toJson() const;
};

// The shareable links of the current stream: the live page at once, the VOD / watch page as soon
// as the platform publishes it. One worker thread polls; begin() and streamEnded() never block.
//   VV_LINK_POLL_MS  polling period, default 20000
//   VV_LINK_GRACE_MS how long a Twitch VOD is still looked for after the stream ended, default 600000
class LinkTracker {
 public:
  LinkTracker() = default;
  ~LinkTracker();
  LinkTracker(const LinkTracker&) = delete;
  LinkTracker& operator=(const LinkTracker&) = delete;

  void begin(const StreamChannel& ch, const std::string& twitchClientId);
  void streamEnded();
  LinkStatus status() const;

 private:
  struct Session {
    std::atomic<bool> stop{false}, ended{false}, done{false};
    std::atomic<int64_t> endedAtMs{0};
  };
  void work(std::shared_ptr<Session> s, StreamChannel ch, std::string clientId, std::string beginIso);
  void update(const std::shared_ptr<Session>& s, const std::function<void(LinkStatus&)>& f);

  mutable std::mutex mu_;
  LinkStatus st_;
  std::shared_ptr<Session> cur_;
  std::thread th_;
  std::vector<std::pair<std::shared_ptr<Session>, std::thread>> retired_;
};

}  // namespace vv
