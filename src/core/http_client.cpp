#include "http_client.h"
#include <windows.h>
#include <winhttp.h>
#include <cstdlib>
#include "common.h"

namespace vv {

namespace {
struct Handle {
  HINTERNET h = nullptr;
  explicit Handle(HINTERNET v = nullptr) : h(v) {}
  ~Handle() { if (h) WinHttpCloseHandle(h); }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
};

std::string failure(const char* fn) { return std::string("http: ") + fn + " failed (" + std::to_string(GetLastError()) + ")"; }
}  // namespace

std::string urlEncode(const std::string& s) {
  static const char* hex = "0123456789ABCDEF";
  std::string o;
  for (unsigned char c : s) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      o += (char)c;
    } else {
      o += '%';
      o += hex[c >> 4];
      o += hex[c & 15];
    }
  }
  return o;
}

std::string envOr(const wchar_t* name, const std::string& fallback) {
  const wchar_t* v = _wgetenv(name);
  if (!v || !*v) return fallback;
  return utf8(v);
}

HttpResponse httpRequest(const std::string& method, const std::string& url, const HttpHeaders& headers,
                         const std::string& body, int timeoutMs) {
  HttpResponse r;
  std::wstring wurl = wide(url);
  URL_COMPONENTS uc{};
  uc.dwStructSize = sizeof uc;
  uc.dwSchemeLength = uc.dwHostNameLength = uc.dwUrlPathLength = uc.dwExtraInfoLength = (DWORD)-1;
  if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc) || !uc.lpszHostName || uc.dwHostNameLength == 0) {
    r.error = "http: invalid URL";
    return r;
  }
  std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
  std::wstring path(uc.lpszUrlPath ? uc.lpszUrlPath : L"", uc.dwUrlPathLength);
  if (uc.lpszExtraInfo) path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
  if (path.empty()) path = L"/";
  const bool secure = uc.nScheme == INTERNET_SCHEME_HTTPS;

  Handle session(WinHttpOpen(wide(std::string("Voloire-video/") + VV_VERSION).c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                             WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
  if (!session.h) { r.error = failure("WinHttpOpen"); return r; }
  WinHttpSetTimeouts(session.h, timeoutMs, timeoutMs, timeoutMs, timeoutMs);
  Handle conn(WinHttpConnect(session.h, host.c_str(), uc.nPort, 0));
  if (!conn.h) { r.error = failure("WinHttpConnect"); return r; }
  Handle req(WinHttpOpenRequest(conn.h, wide(method).c_str(), path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0));
  if (!req.h) { r.error = failure("WinHttpOpenRequest"); return r; }

  std::wstring hdr;
  for (const auto& [k, v] : headers) hdr += wide(k) + L": " + wide(v) + L"\r\n";
  if (!hdr.empty() && !WinHttpAddRequestHeaders(req.h, hdr.c_str(), (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE)) {
    r.error = failure("WinHttpAddRequestHeaders");
    return r;
  }
  LPVOID data = body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data();
  if (!WinHttpSendRequest(req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, data, (DWORD)body.size(), (DWORD)body.size(), 0)) {
    r.error = failure("WinHttpSendRequest");
    return r;
  }
  if (!WinHttpReceiveResponse(req.h, nullptr)) { r.error = failure("WinHttpReceiveResponse"); return r; }

  DWORD status = 0, sz = sizeof status;
  if (WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz,
                          WINHTTP_NO_HEADER_INDEX))
    r.status = (int)status;
  for (;;) {
    DWORD avail = 0;
    if (!WinHttpQueryDataAvailable(req.h, &avail)) { r.error = failure("WinHttpQueryDataAvailable"); return r; }
    if (avail == 0) break;
    size_t at = r.body.size();
    r.body.resize(at + avail);
    DWORD got = 0;
    if (!WinHttpReadData(req.h, &r.body[at], avail, &got)) { r.error = failure("WinHttpReadData"); r.body.resize(at); return r; }
    r.body.resize(at + got);
  }
  wchar_t fin[2048];
  DWORD finLen = sizeof fin;
  if (WinHttpQueryOption(req.h, WINHTTP_OPTION_URL, fin, &finLen)) r.finalUrl = utf8(fin);
  return r;
}

}  // namespace vv
