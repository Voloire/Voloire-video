#include "redact.h"
#include <algorithm>

namespace vv {

namespace {

bool isSecretKey(const std::string& k) {
  std::string l = k;
  std::transform(l.begin(), l.end(), l.begin(), [](unsigned char c) { return (char)std::tolower(c); });
  return l == "key" || l == "streamkey";
}

json maskKeys(const json& j) {
  if (j.is_object()) {
    json o = json::object();
    for (auto it = j.begin(); it != j.end(); ++it)
      o[it.key()] = isSecretKey(it.key()) ? json("***") : maskKeys(it.value());
    return o;
  }
  if (j.is_array()) {
    json a = json::array();
    for (auto& v : j) a.push_back(maskKeys(v));
    return a;
  }
  return j;
}

void maskUrls(json& j) {
  if (j.is_object()) {
    for (auto it = j.begin(); it != j.end(); ++it) maskUrls(it.value());
  } else if (j.is_array()) {
    for (auto& v : j) maskUrls(v);
  } else if (j.is_string()) {
    std::string s = j.get<std::string>();
    std::string r = redactUrl(s);
    if (r != s) j = r;
  }
}

}  // namespace

json redactJson(const json& j) { return maskKeys(j); }

std::string redactUrl(const std::string& url) {
  bool rtmp = url.rfind("rtmp://", 0) == 0;
  bool rtmps = url.rfind("rtmps://", 0) == 0;
  if (!rtmp && !rtmps) return url;
  size_t host = url.find("://") + 3;
  size_t app = url.find('/', host);        // start of the app segment
  if (app == std::string::npos) return url;
  size_t secret = url.find('/', app + 1);  // everything from here on is the stream key
  if (secret == std::string::npos) return url;
  return url.substr(0, secret + 1) + "***";
}

void redactUrls(json& j) { maskUrls(j); }

}  // namespace vv
