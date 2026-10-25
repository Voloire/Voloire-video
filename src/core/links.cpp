#include "links.h"
#include <chrono>
#include <ctime>
#include "http_client.h"
#include "telemetry.h"
#include "twitch.h"

namespace vv {

namespace {
bool channelIdChars(const std::string& s) {
  if (s.size() != 24 || s[0] != 'U' || s[1] != 'C') return false;
  for (char c : s)
    if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == '-')) return false;
  return true;
}

int64_t steadyMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string isoUtcNow(int offsetSec) {
  std::time_t t = std::time(nullptr) + offsetSec;
  std::tm tm{};
  gmtime_s(&tm, &t);
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

long long envMs(const wchar_t* name, long long fallback) {
  std::string v = envOr(name, "");
  if (v.empty()) return fallback;
  try { return std::stoll(v); } catch (...) { return fallback; }
}
}  // namespace

std::string youtubeChannelId(const std::string& s) {
  if (channelIdChars(s)) return s;
  auto p = s.find("/channel/");
  if (p != std::string::npos) {
    std::string id = s.substr(p + 9, 24);
    if (channelIdChars(id)) return id;
  }
  return {};
}

std::string youtubeLiveUrl(const std::string& channelId) {
  if (channelId.empty()) return {};
  return envOr(L"VV_YOUTUBE_BASE", "https://www.youtube.com") + "/channel/" + channelId + "/live";
}

std::string extractCanonicalVideoId(const std::string& html) {
  auto rel = html.find("rel=\"canonical\"");
  if (rel == std::string::npos) return {};
  auto start = html.rfind('<', rel);
  auto end = html.find('>', rel);
  if (start == std::string::npos || end == std::string::npos) return {};
  std::string tag = html.substr(start, end - start);
  auto h = tag.find("href=\"");
  if (h == std::string::npos) return {};
  auto q = tag.find('"', h + 6);
  std::string href = tag.substr(h + 6, q == std::string::npos ? std::string::npos : q - h - 6);
  auto w = href.find("watch?v=");
  if (w == std::string::npos) return {};
  std::string id;
  for (size_t i = w + 8; i < href.size(); ++i) {
    char c = href[i];
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == '-') id += c;
    else break;
  }
  return id;
}

std::string twitchLiveUrl(const std::string& login) { return login.empty() ? std::string() : "https://www.twitch.tv/" + login; }

json LinkStatus::toJson() const {
  return {{"platform", platform}, {"liveUrl", liveUrl}, {"vodUrl", vodUrl}, {"state", state}, {"error", error}};
}

LinkTracker::~LinkTracker() {
  std::vector<std::thread> all;
  {
    std::lock_guard lk(mu_);
    if (cur_) cur_->stop = true;
    if (th_.joinable()) all.push_back(std::move(th_));
    for (auto& r : retired_) {
      r.first->stop = true;
      if (r.second.joinable()) all.push_back(std::move(r.second));
    }
    retired_.clear();
  }
  for (auto& t : all) t.join();
}

void LinkTracker::begin(const StreamChannel& ch, const std::string& clientId) {
  auto s = std::make_shared<Session>();
  LinkStatus st;
  st.platform = ch.platform;
  if (ch.platform == "twitch") {
    st.liveUrl = twitchLiveUrl(ch.twitchLogin);
    if (st.liveUrl.empty()) st.error = "Sign in to Twitch to get a share link";
  } else {
    st.liveUrl = youtubeLiveUrl(youtubeChannelId(ch.studioUrl));
    if (st.liveUrl.empty()) st.error = "Set the channel ID (UC...) in the YouTube tab to get a share link";
  }
  st.state = st.liveUrl.empty() ? "ended" : "watching";

  std::lock_guard lk(mu_);
  if (cur_) cur_->stop = true;
  if (th_.joinable()) retired_.emplace_back(cur_, std::move(th_));
  for (auto it = retired_.begin(); it != retired_.end();) {  // reap the workers that already finished
    if (it->first->done) {
      if (it->second.joinable()) it->second.join();
      it = retired_.erase(it);
    } else {
      ++it;
    }
  }
  cur_ = s;
  st_ = st;
  if (st.state == "watching") th_ = std::thread([this, s, ch, clientId, iso = isoUtcNow(-60)] { work(s, ch, clientId, iso); });
}

void LinkTracker::streamEnded() {
  std::lock_guard lk(mu_);
  if (!cur_) return;
  cur_->ended = true;
  cur_->endedAtMs = steadyMs();
  if (st_.state == "found") st_.state = "ended";
}

LinkStatus LinkTracker::status() const {
  std::lock_guard lk(mu_);
  return st_;
}

void LinkTracker::update(const std::shared_ptr<Session>& s, const std::function<void(LinkStatus&)>& f) {
  std::lock_guard lk(mu_);
  if (cur_ == s) f(st_);  // a stale worker never writes over a newer session
}

void LinkTracker::work(std::shared_ptr<Session> s, StreamChannel ch, std::string clientId, std::string beginIso) {
  const long long pollMs = std::max(100LL, envMs(L"VV_LINK_POLL_MS", 20000));
  const long long graceMs = envMs(L"VV_LINK_GRACE_MS", 600000);
  const bool twitch = ch.platform == "twitch";
  TwitchClient tw(clientId);
  std::string liveUrl = twitch ? twitchLiveUrl(ch.twitchLogin) : youtubeLiveUrl(youtubeChannelId(ch.studioUrl));
  std::string streamId;
  bool sawEnd = false;

  while (!s->stop) {
    std::string vod, err;
    if (twitch) {
      if (streamId.empty()) {
        std::string sid;
        if (tw.currentStreamId(ch.twitchUserId, sid, err) && !sid.empty()) streamId = sid;
      }
      std::vector<TwitchVod> v;
      if (tw.videos(ch.twitchUserId, v, err)) {
        err.clear();
        for (const auto& d : v) {
          bool match = !streamId.empty() ? d.streamId == streamId : d.createdAt >= beginIso;
          if (match) { vod = d.url; break; }
        }
      }
    } else {
      HttpResponse r = httpRequest("GET", liveUrl, {{"Accept-Language", "en"}}, {}, 8000);
      if (r.ok()) {
        std::string id = extractCanonicalVideoId(r.body);
        if (!id.empty()) vod = "https://youtu.be/" + id;
      } else {
        err = r.error.empty() ? "YouTube: HTTP " + std::to_string(r.status) : "YouTube: " + r.error;
      }
    }

    if (!vod.empty()) {
      VV_LOG(Info, "links", "video link found", {{"platform", ch.platform}});
      update(s, [&](LinkStatus& st) { st.vodUrl = vod; st.error.clear(); st.state = s->ended ? "ended" : "found"; });
      break;
    }
    update(s, [&](LinkStatus& st) { st.error = err; });
    if (s->ended && (!twitch || steadyMs() - s->endedAtMs > graceMs)) {
      update(s, [&](LinkStatus& st) { st.state = "ended"; });
      break;
    }
    sawEnd = s->ended;
    for (long long i = 0; i < pollMs / 100 && !s->stop && (sawEnd || !s->ended); ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  s->done = true;
}

}  // namespace vv
