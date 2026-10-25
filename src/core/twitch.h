#pragma once
#include <string>
#include <vector>
#include "common.h"
#include "http_client.h"

namespace vv {

struct TwitchBases {
  std::string api = "https://api.twitch.tv/helix";
  std::string id = "https://id.twitch.tv/oauth2";
  std::string ingest = "https://ingest.twitch.tv";
  static TwitchBases fromEnv();  // VV_TWITCH_API_BASE, VV_TWITCH_ID_BASE, VV_TWITCH_INGEST_BASE
};

constexpr const char* kTwitchScopes = "channel:manage:broadcast channel:read:stream_key";
constexpr const char* kTwitchAccessSecret = "twitch_access_token";
constexpr const char* kTwitchRefreshSecret = "twitch_refresh_token";

struct TwitchDeviceCode { std::string deviceCode, userCode, verificationUri; int expiresIn = 0, interval = 5; };
struct TwitchUser { std::string id, login, displayName; };
struct TwitchChannelInfo { std::string title, categoryId, categoryName, language; std::vector<std::string> tags; };
struct TwitchCategory { std::string id, name; };
struct TwitchIngest { std::string name, url; };  // url without "/{stream_key}"
struct TwitchVod { std::string id, url, streamId, createdAt; };

// Twitch Helix over WinHTTP. Tokens live in DPAPI secrets; nothing secret is ever logged.
// Every method returns false with a human-readable `err` on failure.
class TwitchClient {
 public:
  explicit TwitchClient(std::string clientId, TwitchBases b = TwitchBases::fromEnv());

  bool startDeviceAuth(TwitchDeviceCode& out, std::string& err);
  std::string pollDeviceToken(const std::string& deviceCode, std::string& err);  // "ok" | "pending" | "error"
  bool signedIn() const;  // access token secret present
  void signOut();         // deletes both token secrets

  bool me(TwitchUser& out, std::string& err);
  bool streamKey(const std::string& userId, std::string& key, std::string& err);
  bool channelInfo(const std::string& userId, TwitchChannelInfo& out, std::string& err);
  bool modifyChannel(const std::string& userId, const TwitchChannelInfo& in, std::string& err);  // validates first
  bool searchCategories(const std::string& query, std::vector<TwitchCategory>& out, std::string& err);
  bool ingests(std::vector<TwitchIngest>& out, std::string& err);
  bool currentStreamId(const std::string& userId, std::string& streamId, std::string& err);  // "" when offline
  bool videos(const std::string& userId, std::vector<TwitchVod>& out, std::string& err);     // type=archive, first=5

 private:
  HttpResponse helix(const std::string& method, const std::string& path, const std::string& body, std::string& err);
  bool refresh(std::string& err);
  std::string clientId_;
  TwitchBases b_;
};

// "rtmp://x/app/{stream_key}" -> "rtmp://x/app"; a URL without the placeholder is returned as is.
std::string ingestBaseFromTemplate(const std::string& urlTemplate);
// Empty when the broadcast info can be sent to Twitch, else the reason.
std::string validateBroadcastInfo(const TwitchChannelInfo& in);
// The exact PATCH /channels body: title, game_id and language only when set, tags always.
json modifyChannelBody(const TwitchChannelInfo& in);

}  // namespace vv
