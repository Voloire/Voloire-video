#pragma once
#include <string>
#include <utility>
#include <vector>

namespace vv {

struct HttpResponse {
  int status = 0;
  std::string body, finalUrl, error;  // error: transport failure text, empty on any HTTP answer
  bool ok() const { return error.empty() && status >= 200 && status < 300; }
};
using HttpHeaders = std::vector<std::pair<std::string, std::string>>;

// Blocking, follows redirects, https when the scheme is https. Never logs headers or body.
HttpResponse httpRequest(const std::string& method, const std::string& url, const HttpHeaders& headers = {},
                         const std::string& body = {}, int timeoutMs = 15000);

// RFC 3986: unreserved characters kept, everything else (space included) as %XX over UTF-8.
std::string urlEncode(const std::string& s);

// Value of the environment variable (UTF-8), or `fallback` when it is unset or empty.
std::string envOr(const wchar_t* name, const std::string& fallback);

}  // namespace vv
