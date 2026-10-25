#include "twitch.h"
#include <mutex>
#include "secrets.h"
#include "telemetry.h"

namespace vv {

namespace {
std::mutex gRefreshMu;  // refresh tokens are single use: one refresh at a time, process wide

json parseJson(const std::string& s) { return json::parse(s, nullptr, false); }

std::string str(const json& j, const char* k) {
  auto it = j.find(k);
  return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

std::string form(const std::vector<std::pair<std::string, std::string>>& kv) {
  std::string o;
  for (const auto& [k, v] : kv) {
    if (!o.empty()) o += '&';
    o += urlEncode(k) + "=" + urlEncode(v);
  }
  return o;
}

size_t codePoints(const std::string& s) {
  size_t n = 0;
  for (unsigned char c : s) if ((c & 0xC0) != 0x80) ++n;
  return n;
}

std::string bodyMessage(const std::string& body) {
  json j = parseJson(body);
  if (j.is_object()) return str(j, "message");
  return {};
}

std::string pathOnly(const std::string& path) { return path.substr(0, path.find('?')); }

std::string httpErr(int status, const std::string& body) {
  std::string m = bodyMessage(body);
  return "Twitch: HTTP " + std::to_string(status) + (m.empty() ? "" : " " + m);
}

const json* firstData(const json& j) {
  if (j.is_object() && j.contains("data") && j["data"].is_array() && !j["data"].empty()) return &j["data"][0];
  return nullptr;
}
}  // namespace

TwitchBases TwitchBases::fromEnv() {
  TwitchBases b;
  b.api = envOr(L"VV_TWITCH_API_BASE", b.api);
  b.id = envOr(L"VV_TWITCH_ID_BASE", b.id);
  b.ingest = envOr(L"VV_TWITCH_INGEST_BASE", b.ingest);
  return b;
}

std::string ingestBaseFromTemplate(const std::string& t) {
  const std::string tag = "/{stream_key}";
  auto p = t.find(tag);
  return p == std::string::npos ? t : t.substr(0, p);
}

std::string validateBroadcastInfo(const TwitchChannelInfo& in) {
  size_t n = codePoints(in.title);
  if (n < 1 || n > 140) return "title must be 1..140 characters";
  if (in.tags.size() > 10) return "at most 10 tags";
  for (const auto& t : in.tags) {
    bool ok = codePoints(t) >= 1 && codePoints(t) <= 25;
    for (unsigned char c : t) {
      bool alnum = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c >= 0x80;
      if (!alnum) ok = false;
    }
    if (!ok) return "tag '" + t + "' must be 1..25 letters or digits";
  }
  bool lang = in.language == "other" ||
              (in.language.size() == 2 && in.language[0] >= 'a' && in.language[0] <= 'z' && in.language[1] >= 'a' && in.language[1] <= 'z');
  if (!lang) return "language must be an ISO 639-1 code or 'other'";
  return {};
}

json modifyChannelBody(const TwitchChannelInfo& in) {
  json b = json::object();
  if (!in.title.empty()) b["title"] = in.title;
  if (!in.categoryId.empty()) b["game_id"] = in.categoryId;
  if (!in.language.empty()) b["broadcaster_language"] = in.language;
  b["tags"] = in.tags;
  return b;
}

TwitchClient::TwitchClient(std::string clientId, TwitchBases b) : clientId_(std::move(clientId)), b_(std::move(b)) {}

bool TwitchClient::signedIn() const { return !loadSecret(kTwitchAccessSecret).empty(); }

void TwitchClient::signOut() {
  deleteSecret(kTwitchAccessSecret);
  deleteSecret(kTwitchRefreshSecret);
}

bool TwitchClient::startDeviceAuth(TwitchDeviceCode& out, std::string& err) {
  if (clientId_.empty()) { err = "Twitch: paste the Client ID of your Twitch app first"; return false; }
  HttpResponse r = httpRequest("POST", b_.id + "/device", {{"Content-Type", "application/x-www-form-urlencoded"}},
                               form({{"client_id", clientId_}, {"scopes", kTwitchScopes}}));
  VV_LOG(Info, "twitch", "device code request", {{"status", r.status}});
  if (!r.error.empty()) { err = "Twitch: " + r.error; return false; }
  if (!r.ok()) { err = httpErr(r.status, r.body); return false; }
  json j = parseJson(r.body);
  if (!j.is_object() || str(j, "device_code").empty()) { err = "Twitch: unexpected device code answer"; return false; }
  out.deviceCode = str(j, "device_code");
  out.userCode = str(j, "user_code");
  out.verificationUri = str(j, "verification_uri");
  out.expiresIn = j.value("expires_in", 1800);
  out.interval = j.value("interval", 5);
  return true;
}

std::string TwitchClient::pollDeviceToken(const std::string& deviceCode, std::string& err) {
  HttpResponse r = httpRequest("POST", b_.id + "/token", {{"Content-Type", "application/x-www-form-urlencoded"}},
                               form({{"client_id", clientId_},
                                     {"scopes", kTwitchScopes},
                                     {"device_code", deviceCode},
                                     {"grant_type", "urn:ietf:params:oauth:grant-type:device_code"}}));
  if (!r.error.empty()) { err = "Twitch: " + r.error; return "error"; }
  if (r.ok()) {
    json j = parseJson(r.body);
    std::string a = j.is_object() ? str(j, "access_token") : std::string();
    std::string rt = j.is_object() ? str(j, "refresh_token") : std::string();
    if (a.empty() || !saveSecret(kTwitchAccessSecret, a) || (!rt.empty() && !saveSecret(kTwitchRefreshSecret, rt))) {
      err = "Twitch: cannot store the tokens";
      return "error";
    }
    VV_LOG(Info, "twitch", "signed in");
    return "ok";
  }
  std::string m = bodyMessage(r.body);
  if (r.status == 400 && (m == "authorization_pending" || m == "slow_down")) return "pending";
  err = httpErr(r.status, r.body);
  return "error";
}

bool TwitchClient::refresh(std::string& err) {
  std::string rt = loadSecret(kTwitchRefreshSecret);
  if (rt.empty()) { err = "no refresh token"; return false; }
  HttpResponse r = httpRequest("POST", b_.id + "/token", {{"Content-Type", "application/x-www-form-urlencoded"}},
                               form({{"client_id", clientId_}, {"grant_type", "refresh_token"}, {"refresh_token", rt}}));
  VV_LOG(Info, "twitch", "token refresh", {{"status", r.status}});
  if (!r.ok()) { err = r.error.empty() ? "refresh refused" : r.error; return false; }
  json j = parseJson(r.body);
  std::string a = j.is_object() ? str(j, "access_token") : std::string();
  std::string nrt = j.is_object() ? str(j, "refresh_token") : std::string();
  if (a.empty() || !saveSecret(kTwitchAccessSecret, a)) { err = "cannot store the refreshed token"; return false; }
  if (!nrt.empty()) saveSecret(kTwitchRefreshSecret, nrt);
  return true;
}

HttpResponse TwitchClient::helix(const std::string& method, const std::string& path, const std::string& body, std::string& err) {
  HttpResponse r;
  std::string token = loadSecret(kTwitchAccessSecret);
  if (token.empty()) {
    err = "Twitch: not connected, sign in from the Twitch tab";
    r.error = err;
    return r;
  }
  auto send = [&](const std::string& tok) {
    return httpRequest(method, b_.api + path,
                       {{"Authorization", "Bearer " + tok}, {"Client-Id", clientId_}, {"Content-Type", "application/json"}}, body);
  };
  r = send(token);
  if (r.status == 401) {
    std::string why;
    {
      std::lock_guard lk(gRefreshMu);
      std::string cur = loadSecret(kTwitchAccessSecret);
      if (!cur.empty() && cur != token) token = cur;  // another thread refreshed already
      else if (!refresh(why)) token.clear();
      else token = loadSecret(kTwitchAccessSecret);
    }
    if (token.empty()) {
      signOut();
      err = "Twitch: session expired, sign in again";
      r.error = err;
      return r;
    }
    r = send(token);
  }
  VV_LOG(Info, "twitch", method + " " + pathOnly(path), {{"status", r.status}});
  if (!r.error.empty()) { err = "Twitch: " + r.error; return r; }
  if (!r.ok()) err = httpErr(r.status, r.body);
  return r;
}

bool TwitchClient::me(TwitchUser& out, std::string& err) {
  HttpResponse r = helix("GET", "/users", {}, err);
  if (!r.ok()) return false;
  json j = parseJson(r.body);
  const json* d = firstData(j);
  if (!d) { err = "Twitch: no user in the answer"; return false; }
  out.id = str(*d, "id");
  out.login = str(*d, "login");
  out.displayName = str(*d, "display_name");
  if (out.displayName.empty()) out.displayName = out.login;
  return !out.id.empty();
}

bool TwitchClient::streamKey(const std::string& userId, std::string& key, std::string& err) {
  HttpResponse r = helix("GET", "/streams/key?broadcaster_id=" + urlEncode(userId), {}, err);
  if (!r.ok()) return false;
  json j = parseJson(r.body);
  const json* d = firstData(j);
  if (!d) { err = "Twitch: no stream key in the answer"; return false; }
  key = str(*d, "stream_key");
  if (key.empty()) { err = "Twitch: empty stream key"; return false; }
  return true;
}

bool TwitchClient::channelInfo(const std::string& userId, TwitchChannelInfo& out, std::string& err) {
  HttpResponse r = helix("GET", "/channels?broadcaster_id=" + urlEncode(userId), {}, err);
  if (!r.ok()) return false;
  json j = parseJson(r.body);
  const json* d = firstData(j);
  if (!d) { err = "Twitch: no channel in the answer"; return false; }
  out.title = str(*d, "title");
  out.categoryId = str(*d, "game_id");
  out.categoryName = str(*d, "game_name");
  out.language = str(*d, "broadcaster_language");
  out.tags.clear();
  if (d->contains("tags") && (*d)["tags"].is_array())
    for (const auto& t : (*d)["tags"]) if (t.is_string()) out.tags.push_back(t.get<std::string>());
  return true;
}

bool TwitchClient::modifyChannel(const std::string& userId, const TwitchChannelInfo& in, std::string& err) {
  err = validateBroadcastInfo(in);
  if (!err.empty()) { err = "Twitch: " + err; return false; }
  HttpResponse r = helix("PATCH", "/channels?broadcaster_id=" + urlEncode(userId), modifyChannelBody(in).dump(), err);
  return r.ok();
}

bool TwitchClient::searchCategories(const std::string& query, std::vector<TwitchCategory>& out, std::string& err) {
  HttpResponse r = helix("GET", "/search/categories?first=10&query=" + urlEncode(query), {}, err);
  if (!r.ok()) return false;
  json j = parseJson(r.body);
  out.clear();
  if (j.is_object() && j.contains("data") && j["data"].is_array())
    for (const auto& d : j["data"]) out.push_back({str(d, "id"), str(d, "name")});
  return true;
}

bool TwitchClient::ingests(std::vector<TwitchIngest>& out, std::string& err) {
  HttpResponse r = httpRequest("GET", b_.ingest + "/ingests");  // public, no token
  if (!r.error.empty()) { err = "Twitch: " + r.error; return false; }
  if (!r.ok()) { err = "Twitch: HTTP " + std::to_string(r.status) + " from the ingest list"; return false; }
  json j = parseJson(r.body);
  out.clear();
  if (j.is_object() && j.contains("ingests") && j["ingests"].is_array())
    for (const auto& d : j["ingests"]) out.push_back({str(d, "name"), ingestBaseFromTemplate(str(d, "url_template"))});
  return true;
}

bool TwitchClient::currentStreamId(const std::string& userId, std::string& streamId, std::string& err) {
  HttpResponse r = helix("GET", "/streams?user_id=" + urlEncode(userId), {}, err);
  if (!r.ok()) return false;
  json j = parseJson(r.body);
  streamId.clear();
  if (const json* d = firstData(j)) streamId = str(*d, "id");
  return true;
}

bool TwitchClient::videos(const std::string& userId, std::vector<TwitchVod>& out, std::string& err) {
  HttpResponse r = helix("GET", "/videos?type=archive&first=5&user_id=" + urlEncode(userId), {}, err);
  if (!r.ok()) return false;
  json j = parseJson(r.body);
  out.clear();
  if (j.is_object() && j.contains("data") && j["data"].is_array())
    for (const auto& d : j["data"]) out.push_back({str(d, "id"), str(d, "url"), str(d, "stream_id"), str(d, "created_at")});
  return true;
}

}  // namespace vv
