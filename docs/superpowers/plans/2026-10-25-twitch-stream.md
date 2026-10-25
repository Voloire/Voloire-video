# Twitch Streaming Tab, Browser Sign-in and Share Links: Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. Copy this file to `docs/superpowers/plans/2026-10-25-twitch-stream.md` in the first commit so the plan travels with the code.

**Goal:** Add Twitch as a second live destination with its own TWITCH STREAM tab: sign in through the browser (Twitch Device Code flow, as OBS does), fetch the stream key from the account, set title, category, tags and language before going live, and show a shareable live link immediately plus the VOD link as soon as Twitch publishes it. The same share links appear for YouTube. Everything is also reachable from the right-click menu of the status bar and the tray.

**Architecture:** Twitch is a `platform` of the existing `StreamChannel` profile (`youtube` | `twitch`); the RTMP pipeline, the DPAPI key store, the Recording tab and the e2e harness are reused as they are. A small WinHTTP client (`src/core/http.*`) and a Twitch Helix client (`src/core/twitch.*`) live in core, driven by new control-pipe commands in `AppController`, so the panel, the menu and the e2e harness all go through one path. A `LinkTracker` (`src/core/links.*`) polls for the VOD link (Twitch Helix `/videos`) and resolves the YouTube watch link from the public `/channel/<id>/live` page. No new third-party library.

**Tech Stack:** C++20 / MSVC, Qt 6.5 Widgets (UI only), WinHTTP (`winhttp.lib`, system), nlohmann json (already vendored), DPAPI secrets (existing `secrets.h`), doctest (unit), Python 3 `http.server` fake + existing `RtmpIngest` (e2e).

**Spec:** this document's *Context* and *Decisions* sections (Franco's request and answers of 2026-10-25).

## Execution status (2026-10-25)

- Tasks 1-8 product changes are implemented on `feature/twitch-stream` in the dedicated worktree.
- Remote verification: `main` and `origin/main` were both `2f8f226`; the eight implementation
  commits ending at `6e5b5be` initially existed locally only, with no open pull request.
- Franco's latest instruction replaces the audio/video verification commands below with functional
  tests only. Do not run recordings, streams, encoders, audio/video playback, desktop GUI scenarios,
  or the old harness self-test on his working PC. Encoding and audio were already validated for YouTube.
- Functional validation uses `tests/functional_tests.py` and a test-only, allowlisted JSON-line driver,
  isolated settings and local fake APIs. It exercises the real controller and link tracker without
  starting the recorder, opening a browser or touching capture devices. Functional unit tests opt in through the `functional` doctest suite;
  media and device tests cannot be included by an incomplete exclusion filter.
- Regression fixes: sign-out cancels and joins pending device auth before deleting tokens; stream end
  immediately reports `ended` while still allowing a late VOD to resolve during the grace period.
- UI shutdown joins outstanding Twitch work; cancel/sign-out stay off the Qt thread. An empty
  title is validated before go-live when applying broadcast info is enabled.
- Default CI is functional-only; existing media and installer validation is retained as an explicit
  manual `workflow_dispatch` option. GUI and live media scenarios are not claimed as rerun.
- Local verification passed: Release build, 16 functional unit cases (106 assertions), and nine
  headless functional integration cases, via `ctest --test-dir build -L functional --output-on-failure`.
- Delivery follows this branch's pull request: push the implementation and validation commits,
  merge when configured checks pass and no review requests changes, then remove the feature worktree.

## Context

Voloire-video 1.3.0 streams to YouTube by RTMP with a pasted stream key (`src/ui/panel.cpp` YOUTUBE STREAM tab, `src/core/recorder.cpp:76-101`). Franco wants Twitch too, with a dedicated tab and settings, with sign-in done in the browser instead of pasting keys, with the broadcast info (title, category/game, tags, language) set from the app before going live, and with the share links (live page and VOD) available as early as possible for both Twitch and YouTube so he can hand them to friends and colleagues while live. VOD download stays manual (Twitch has no download API; the Video Producer page does it) and the existing local `.mkv` copy while live is the file to re-upload to YouTube.

Facts verified on 2026-10-25 against dev.twitch.tv:
- Device Code Grant: `POST https://id.twitch.tv/oauth2/device` (`client_id`, `scopes`) returns `device_code`, `user_code`, `verification_uri`, `expires_in`, `interval`; `POST https://id.twitch.tv/oauth2/token` with `grant_type=urn:ietf:params:oauth:grant-type:device_code`, `client_id`, `scopes`, `device_code` returns `access_token`, `refresh_token`, `expires_in`, HTTP 400 with `message: authorization_pending` while the user has not confirmed. Public clients need no secret. Access tokens last 4 h; refresh tokens are single use and expire after 30 days of inactivity.
- Refresh: `POST https://id.twitch.tv/oauth2/token` with `grant_type=refresh_token`, `refresh_token`, `client_id` (no secret for public clients). Response carries a new `access_token` and `refresh_token`.
- Helix needs headers `Authorization: Bearer <token>` and `Client-Id: <client id>`. `GET /users` (no params: the token's user) gives `id`, `login`, `display_name`. `GET /streams/key?broadcaster_id=` (scope `channel:read:stream_key`) gives `data[0].stream_key`. `GET /channels?broadcaster_id=` gives `title`, `game_id`, `game_name`, `broadcaster_language`, `tags`. `PATCH /channels?broadcaster_id=` (scope `channel:manage:broadcast`, JSON body, 204) accepts `title` (max 140, not empty), `game_id`, `broadcaster_language`, `tags` (max 10, each max 25 chars, no spaces or special characters). `GET /search/categories?query=` gives `id`, `name`. `GET /streams?user_id=` gives `data[0].id` while live, empty when offline. `GET /videos?user_id=&type=archive&first=5` gives `id`, `url`, `stream_id`, `created_at`.
- Ingests: `GET https://ingest.twitch.tv/ingests` (no auth) returns `ingests[]` with `name` and `url_template` like `rtmp://mil.contribute.live-video.net/app/{stream_key}`; `rtmp://live.twitch.tv/app` is the auto-routing default.
- Twitch has no description field. VODs exist only when "Store past broadcasts" is on in the channel's stream settings; they appear minutes after the stream starts and are kept 7 days (14 for Affiliates).
- YouTube: `https://www.youtube.com/channel/<UC...>/live` is public and redirects to the current live broadcast; the watch page carries `<link rel="canonical" href="https://www.youtube.com/watch?v=<id>">`. No Google sign-in is needed for it.

## Decisions (Franco's answers, binding)

- Sign-in to Twitch happens in the browser, like OBS. Device Code flow with a **Public** app registered by Franco at https://dev.twitch.tv/console/apps (category Application Integration); the Client ID is pasted once into a field of the Twitch tab and stored in `ui.json` (it is not a secret). Tokens are DPAPI secrets.
- VOD download is manual: a button opens the Video Producer page. No yt-dlp, no automatic download, no remux. The existing "Save local .mkv copy while live" is the archive to upload to YouTube (YouTube accepts `.mkv`).
- Encoding, resolution, frame rate and audio are already tested and tuned for YouTube and are shared: the Twitch tab does not duplicate those cards. It only shows a one-line hint about Twitch's 6000 kbps ceiling.
- Share links for both platforms: the live link is shown the moment the stream starts (Twitch channel page; YouTube `/channel/<id>/live`), the VOD/watch link as soon as it can be resolved. Copy and Open buttons in the tabs, the same entries in the right-click menu of the status bar and the tray.
- No new audio or video tests. The harness must prove the new UI/UX and the real flows: sign-in, key fetch, broadcast info, go live to a local ingest with a Twitch profile, links, menu entries, no token leak.
- Record and stream stay mutually exclusive; one destination per session.

## Global Constraints

- Work on branch `feature/twitch-stream` in its own `git worktree`; the main checkout stays on `main`. Merge only through a pull request. Run the full local e2e list named in Task 8 before opening it.
- Commit messages, PR text, code comments and UI copy in English; no AI/assistant attribution anywhere (no `Co-Authored-By`, no "Generated with").
- Windows PowerShell 5.1 on the dev machine: no `&&`, `rg` is not installed; build with `scripts\build.ps1`, unit tests `build\bin\vv_unit_tests.exe`, e2e `python tests\e2e\run_e2e.py --only <names>`.
- No new third-party library. New system lib: `winhttp`.
- Secrets: the Twitch access token, refresh token and stream key never appear in `config.json`, `ui.json`, `events.jsonl`, `metrics.jsonl`, `result.json`, `crash.log`, any pipe response, tooltip, menu text or log line. Only "set / not set" and the login name are shown.
- The app never opens a browser when `VV_NO_BROWSER=1` is set (the harness sets it). Every `QDesktopServices::openUrl` in new code goes through one helper that checks it.
- Tests never talk to real Twitch or YouTube: every scenario points the app at the fake server through `VV_TWITCH_API_BASE`, `VV_TWITCH_ID_BASE`, `VV_TWITCH_INGEST_BASE`, `VV_YOUTUBE_BASE`, and shortens polling with `VV_LINK_POLL_MS`.
- Network calls never run on the Qt thread: the panel runs them on a `std::thread` and posts the result with `QMetaObject::invokeMethod(..., Qt::QueuedConnection)` (pattern of `MainPanel::startRecording`, `src/ui/panel.cpp:1410`). The `ui_*` pipe handler in `src/ui/main.cpp:108` runs on the Qt thread with a blocking call: Twitch commands are therefore handled in `AppController::handle`, not there.
- UI copy, fonts, colours: as the existing tabs (`makeCard`, labels with objectName `sub` / `status`, `vv::ui::col` and `hud` tokens). Nothing heavier than the existing weights.

## Review Focus

1. **Token leak through the pipe or the files.** `twitch_status`, `get_config`, `ui_state`, `status` must never carry a token or the key. Test: `scan_for_secrets` over the settings dir, the telemetry dir and every pipe response for `[access_token, refresh_token, stream_key]` (Task 4, Task 6).
2. **Expired access token mid-session (after 4 h).** A 401 from Helix must trigger one refresh and a retry, transparently; a failed refresh must leave the UI in "Not connected, sign in again" and never loop. Test: `fake.expire_token()` then `twitch_get_info` succeeds and `fake.refreshes == 1`; `fake.break_refresh()` then the command fails with an error containing "sign in" (Task 4).
3. **Go live with Twitch info rejected** (title over 140, bad tag, Twitch 409). The stream must not start and the ingest must never see a connection; the error names Twitch and the reason. Test in Task 6.
4. **No VOD ever appears** ("Store past broadcasts" off, or the fake never adds one). The link card shows the live link and "VOD: not published yet", the tracker stops 10 min after the stream ends, nothing hangs on End stream or Quit. Test: End stream with no VOD added returns to idle in under 5 s and `links.state == "ended"` (Task 5).
5. **YouTube profile without a channel id** (only `https://studio.youtube.com/`). The live link cannot be built: the card shows "Set the channel ID (UC...) to get a share link", nothing is polled. Test in Task 5 unit test and Task 7 scenario.

---

## File Structure

| File | Responsibility |
|---|---|
| `src/core/config.{h,cpp}` (modify) | `StreamChannel.platform` and Twitch fields, `UiPrefs.twitchClientId`, JSON round trip |
| `src/core/http.{h,cpp}` (new) | `httpRequest` over WinHTTP, `urlEncode`, env-base helper |
| `src/core/twitch.{h,cpp}` (new) | `TwitchClient`: device auth, refresh, Helix calls; pure helpers `ingestBaseFromTemplate`, `validateBroadcastInfo` |
| `src/core/links.{h,cpp}` (new) | `LinkTracker` (live + VOD link per platform), `youtubeChannelId`, `youtubeLiveUrl`, `extractCanonicalVideoId`, `twitchLiveUrl` |
| `src/core/app_controller.{h,cpp}` (modify) | pipe commands `twitch_*`, `links`; starts/stops the tracker from `Recorder::onState` |
| `src/core/CMakeLists.txt` (modify) | new sources, `winhttp` |
| `src/ui/panel.{h,cpp}` (modify) | TWITCH STREAM tab, share-link cards in both stream tabs, platform-aware labels, go-live applies broadcast info |
| `src/ui/control_menu.cpp` (modify) | `Twitch` submenu, `Share link` submenu, `Output > Stream (live)` |
| `src/ui/tray.cpp` (modify) | `clipboardText` and `links` in `ui_state` |
| `src/ui/main.cpp` (modify) | `ui_tab` command |
| `tests/unit_tests.cpp` (modify) | config round trip, helpers, resolver parsing |
| `tests/e2e/fakes/twitch_api.py` (new) | `FakeTwitch`: id, helix, ingests and a fake YouTube live page |
| `tests/e2e/run_e2e.py` (modify) | scenarios `twitch_auth`, `twitch_live`, `youtube_links`; `sc_ui_stream` menu text |
| `README.md`, `CMakeLists.txt` (modify) | docs, version 1.4.0 |

---

### Task 1: Branch, version and the channel model

**Files:**
- Modify: `CMakeLists.txt:2` (version `1.4.0`)
- Modify: `src/core/config.h:62-83`, `src/core/config.cpp:132-169`
- Test: `tests/unit_tests.cpp`
- Create: `docs/superpowers/plans/2026-10-25-twitch-stream.md` (copy of this plan)

**Interfaces:**
- Produces:
  ```cpp
  struct StreamChannel {
    std::string id = "default";
    std::string label = "Main Channel";
    std::string platform = "youtube";          // youtube | twitch
    std::string studioUrl = "https://studio.youtube.com/";
    std::string streamUrl = "rtmp://a.rtmp.youtube.com/live2";
    // Twitch only (empty for YouTube profiles)
    std::string twitchLogin, twitchUserId, twitchDisplayName;
    std::string title, categoryId, categoryName, language = "it";
    std::vector<std::string> tags;
    bool applyInfoOnGoLive = true;
    json toJson() const;
    static StreamChannel fromJson(const json& j);
  };
  struct UiPrefs { ...existing...; std::string twitchClientId; };
  ```
  `fromJson` maps any `platform` other than `"twitch"` to `"youtube"`.

- [ ] **Step 1: Create the worktree and branch**

```powershell
git worktree add ..\voloire-video-twitch -b feature/twitch-stream
```
Work in `..\voloire-video-twitch` from here on. Copy this plan to `docs/superpowers/plans/2026-10-25-twitch-stream.md`.

- [ ] **Step 2: Write the failing test** in `tests/unit_tests.cpp`, after the `"stream output validates..."` case:

```cpp
TEST_CASE("stream channels carry a platform and the Twitch broadcast info") {
  StreamChannel c;
  c.id = "tw_123456"; c.label = "voloire"; c.platform = "twitch";
  c.twitchLogin = "voloire"; c.twitchUserId = "123456"; c.twitchDisplayName = "voloire";
  c.title = "Late session"; c.categoryId = "509658"; c.categoryName = "Just Chatting";
  c.language = "it"; c.tags = {"gaming", "chill"}; c.applyInfoOnGoLive = false;
  StreamChannel r = StreamChannel::fromJson(c.toJson());
  CHECK(r.platform == "twitch");
  CHECK(r.twitchUserId == "123456");
  CHECK(r.tags == std::vector<std::string>{"gaming", "chill"});
  CHECK(r.applyInfoOnGoLive == false);
  CHECK(StreamChannel::fromJson({{"id", "x"}}).platform == "youtube");   // old ui.json: no platform
  CHECK(StreamChannel::fromJson({{"id", "x"}, {"platform", "weird"}}).platform == "youtube");
  CHECK(StreamChannel::fromJson({{"id", "x"}}).applyInfoOnGoLive == true);
  UiPrefs p; p.twitchClientId = "abc123";
  CHECK(UiPrefs::fromJson(p.toJson()).twitchClientId == "abc123");
}
```

- [ ] **Step 3: Run it, expect a compile error** (`platform` is not a member): `scripts\build.ps1` then `build\bin\vv_unit_tests.exe -tc="*platform*"`.

- [ ] **Step 4: Implement** the fields in `config.h`, extend `StreamChannel::toJson/fromJson` and `UiPrefs::toJson/fromJson` in `config.cpp` (use the local `get<>` helper; `tags` through `get<std::vector<std::string>>`). Set the version to `1.4.0`.

- [ ] **Step 5: Run the unit tests, expect PASS**: `build\bin\vv_unit_tests.exe`.

- [ ] **Step 6: Commit**

```powershell
git add CMakeLists.txt src/core/config.h src/core/config.cpp tests/unit_tests.cpp docs/superpowers/plans/2026-10-25-twitch-stream.md
git commit -m "feat(config): stream channel platform and Twitch broadcast fields (1.4.0)"
```

---

### Task 2: HTTP client over WinHTTP

**Files:**
- Create: `src/core/http.h`, `src/core/http.cpp`
- Modify: `src/core/CMakeLists.txt` (add `http.cpp`; add `winhttp` to `target_link_libraries`)
- Test: `tests/unit_tests.cpp`

**Interfaces:**
- Produces:
  ```cpp
  struct HttpResponse {
    int status = 0;
    std::string body, finalUrl, error;   // error: transport failure text, empty on any HTTP answer
    bool ok() const { return error.empty() && status >= 200 && status < 300; }
  };
  using HttpHeaders = std::vector<std::pair<std::string, std::string>>;
  // Blocking. Follows redirects. https when the scheme is https. Never logs the headers or the body.
  HttpResponse httpRequest(const std::string& method, const std::string& url, const HttpHeaders& headers = {},
                           const std::string& body = {}, int timeoutMs = 15000);
  std::string urlEncode(const std::string& s);          // RFC 3986 unreserved kept, space -> %20
  std::string envOr(const wchar_t* name, const std::string& fallback);  // _wgetenv, utf8, fallback when unset/empty
  ```

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("urlEncode keeps unreserved characters and escapes the rest") {
  CHECK(urlEncode("just chatting") == "just%20chatting");
  CHECK(urlEncode("a-b_c.d~e") == "a-b_c.d~e");
  CHECK(urlEncode("tag:ok/é") == "tag%3Aok%2F%C3%A9");
}
TEST_CASE("httpRequest reports a transport error without throwing") {
  HttpResponse r = httpRequest("GET", "http://127.0.0.1:9/nothing", {}, {}, 2000);  // port 9: nothing listens
  CHECK_FALSE(r.ok());
  CHECK_FALSE(r.error.empty());
  CHECK(r.status == 0);
  HttpResponse bad = httpRequest("GET", "not a url");
  CHECK_FALSE(bad.error.empty());
}
TEST_CASE("envOr falls back when the variable is unset or empty") {
  _wputenv(L"VV_TEST_ENVOR=");
  CHECK(envOr(L"VV_TEST_ENVOR", "fb") == "fb");
  _wputenv(L"VV_TEST_ENVOR=val");
  CHECK(envOr(L"VV_TEST_ENVOR", "fb") == "val");
}
```

- [ ] **Step 2: Run, expect compile failure.**

- [ ] **Step 3: Implement `http.cpp`** with `WinHttpCrackUrl`, `WinHttpOpen` (agent `"Voloire-video/" VV_VERSION`), `WinHttpConnect`, `WinHttpOpenRequest` (flag `WINHTTP_FLAG_SECURE` for https), `WinHttpSetTimeouts(h, timeoutMs, timeoutMs, timeoutMs, timeoutMs)`, headers joined as `"Name: value\r\n"` into `WinHttpAddRequestHeaders`, `WinHttpSendRequest` with the body bytes, `WinHttpReceiveResponse`, status via `WinHttpQueryHeaders(WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER)`, body via `WinHttpReadData` loop, `finalUrl` via `WinHttpQueryOption(hRequest, WINHTTP_OPTION_URL, ...)`. On any Win32 failure fill `error` with `"http: <function> failed (<GetLastError()>)"`. Redirects follow by default (keep the default redirect policy).

- [ ] **Step 4: Run the unit tests, expect PASS.**

- [ ] **Step 5: Commit**

```powershell
git add src/core/http.h src/core/http.cpp src/core/CMakeLists.txt tests/unit_tests.cpp
git commit -m "feat(core): WinHTTP request helper"
```

---

### Task 3: Twitch client and the fake Twitch server

**Files:**
- Create: `src/core/twitch.h`, `src/core/twitch.cpp`
- Create: `tests/e2e/fakes/twitch_api.py`
- Modify: `src/core/CMakeLists.txt` (add `twitch.cpp`)
- Test: `tests/unit_tests.cpp` (pure helpers); the client itself is proven end to end in Task 4

**Interfaces:**
- Consumes: `httpRequest`, `urlEncode`, `envOr` (Task 2); `saveSecret/loadSecret/deleteSecret` (`src/core/secrets.h`).
- Produces (`twitch.h`):
  ```cpp
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
  struct TwitchIngest { std::string name, url; };          // url without "/{stream_key}"
  struct TwitchVod { std::string id, url, streamId, createdAt; };

  class TwitchClient {
   public:
    explicit TwitchClient(std::string clientId, TwitchBases b = TwitchBases::fromEnv());
    bool startDeviceAuth(TwitchDeviceCode& out, std::string& err);
    std::string pollDeviceToken(const std::string& deviceCode, std::string& err);  // "ok" | "pending" | "error"; ok saves both secrets
    bool signedIn() const;                                   // access token secret present
    void signOut();                                          // deletes both secrets
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
  std::string ingestBaseFromTemplate(const std::string& urlTemplate);   // "rtmp://x/app/{stream_key}" -> "rtmp://x/app"
  std::string validateBroadcastInfo(const TwitchChannelInfo& in);        // "" when valid
  json modifyChannelBody(const TwitchChannelInfo& in);                   // the exact PATCH body
  ```
  Rules fixed here:
  - `helix()` adds `Authorization: Bearer <access>`, `Client-Id: <clientId_>`, `Content-Type: application/json`. When `signedIn()` is false it returns `err = "Twitch: not connected, sign in from the Twitch tab"` without any request. On HTTP 401 it calls `refresh()` once and retries once; when the refresh fails it deletes both secrets and sets `err = "Twitch: session expired, sign in again"`. Any other non-2xx: `err = "Twitch: HTTP <status> <message field of the body, if any>"`.
  - `pollDeviceToken`: HTTP 400 whose body `message` is `authorization_pending` (or `slow_down`) returns `"pending"`; a 2xx saves `access_token` and `refresh_token` as secrets and returns `"ok"`; anything else returns `"error"` with `err` set.
  - `validateBroadcastInfo`: title empty or over 140 characters -> `"title must be 1..140 characters"`; more than 10 tags -> `"at most 10 tags"`; a tag over 25 characters or containing anything but letters and digits -> `"tag '<tag>' must be 1..25 letters or digits"`; language not two lowercase letters and not `"other"` -> `"language must be an ISO 639-1 code or 'other'"`.
  - `modifyChannelBody` emits only the fields that are non-empty: `title`, `game_id` (from `categoryId`), `broadcaster_language` (from `language`), `tags` (always, an empty array clears).
  - Secrets and keys are never logged. `VV_LOG(Info, "twitch", ...)` lines name the operation and the HTTP status only.

- [ ] **Step 1: Write the failing unit tests**

```cpp
TEST_CASE("Twitch helpers: ingest template, broadcast validation, patch body") {
  CHECK(ingestBaseFromTemplate("rtmp://mil.contribute.live-video.net/app/{stream_key}") == "rtmp://mil.contribute.live-video.net/app");
  CHECK(ingestBaseFromTemplate("rtmp://live.twitch.tv/app") == "rtmp://live.twitch.tv/app");
  TwitchChannelInfo i;
  i.title = "Late session"; i.categoryId = "509658"; i.language = "it"; i.tags = {"gaming", "chill"};
  CHECK(validateBroadcastInfo(i).empty());
  json b = modifyChannelBody(i);
  CHECK(b == json({{"title", "Late session"}, {"game_id", "509658"}, {"broadcaster_language", "it"}, {"tags", {"gaming", "chill"}}}));
  TwitchChannelInfo noCat = i; noCat.categoryId.clear();
  CHECK_FALSE(modifyChannelBody(noCat).contains("game_id"));
  TwitchChannelInfo longT = i; longT.title = std::string(141, 'x');
  CHECK(validateBroadcastInfo(longT).find("140") != std::string::npos);
  TwitchChannelInfo manyTags = i; manyTags.tags.assign(11, "t");
  CHECK(validateBroadcastInfo(manyTags).find("10 tags") != std::string::npos);
  TwitchChannelInfo badTag = i; badTag.tags = {"no spaces"};
  CHECK(validateBroadcastInfo(badTag).find("no spaces") != std::string::npos);
  TwitchChannelInfo badLang = i; badLang.language = "italian";
  CHECK_FALSE(validateBroadcastInfo(badLang).empty());
  _wputenv(L"VV_TWITCH_API_BASE=http://127.0.0.1:1/helix");
  CHECK(TwitchBases::fromEnv().api == "http://127.0.0.1:1/helix");
  _wputenv(L"VV_TWITCH_API_BASE=");
  CHECK(TwitchBases::fromEnv().api == "https://api.twitch.tv/helix");
}
TEST_CASE("Twitch client refuses every call while not connected, with no request") {
  auto dir = tempDir("twitch-offline");
  std::wstring env = L"VV_SETTINGS_DIR=" + wide(strOf(dir));
  _wputenv(env.c_str());
  deleteSecret(kTwitchAccessSecret); deleteSecret(kTwitchRefreshSecret);
  TwitchBases b; b.api = "http://127.0.0.1:9/helix";  // nothing listens: a request would error differently
  TwitchClient t("cid", b);
  CHECK_FALSE(t.signedIn());
  TwitchUser u; std::string err;
  CHECK_FALSE(t.me(u, err));
  CHECK(err.find("not connected") != std::string::npos);
}
```

- [ ] **Step 2: Run, expect compile failure.**

- [ ] **Step 3: Implement `twitch.cpp`.** Form bodies for `id.twitch.tv` are `application/x-www-form-urlencoded` built with `urlEncode`. Helix GET paths carry their query string; `me()` is `GET /users`; `videos()` sorts nothing (Twitch returns newest first).

- [ ] **Step 4: Write `tests/e2e/fakes/twitch_api.py`** (`http.server.ThreadingHTTPServer` on `127.0.0.1`, free port from `fakes.rtmp_ingest.free_port`, daemon thread, logging silenced):

```python
class FakeTwitch:
    client_id = "fake-client-id"
    user = {"id": "123456", "login": "voloire", "display_name": "voloire"}
    stream_key = "live_123456_fakekey9876"
    device_code, user_code = "devcode-XYZ", "ABCD-1234"
    categories = [{"id": "509658", "name": "Just Chatting"}, {"id": "33214", "name": "Fortnite"}]
    ingests = [{"name": "Default", "url_template": "rtmp://live.twitch.tv/app/{stream_key}"},
               {"name": "Europe: Milan, Italy", "url_template": "rtmp://mil.contribute.live-video.net/app/{stream_key}"}]
    def __init__(self): ...                      # tokens: access "fake-access-AAAA1", refresh "fake-refresh-BBBB1"; approved False
    base -> f"http://127.0.0.1:{port}"
    env -> {"VV_TWITCH_API_BASE": base + "/helix", "VV_TWITCH_ID_BASE": base + "/oauth2",
            "VV_TWITCH_INGEST_BASE": base, "VV_YOUTUBE_BASE": base + "/yt", "VV_LINK_POLL_MS": "500", "VV_NO_BROWSER": "1"}
    approve()                                     # the device flow now returns tokens
    expire_token()                                # current access token answers 401 until a refresh issues "fake-access-AAAA2"
    break_refresh()                               # the refresh grant answers 400 invalid refresh token
    set_live(stream_id) / set_offline()
    add_vod(vod_id, stream_id)                    # url https://www.twitch.tv/videos/<vod_id>
    reject_patch(status=409, message="title too long")   # next PATCH answers that, once
    set_youtube_video(channel_id, video_id)       # GET /yt/channel/<id>/live answers HTML with the canonical link
    patches: list of dicts (PATCH bodies received)   refreshes: int   requests: list of (method, path)
    channel: dict with title, game_id, game_name, broadcaster_language, tags (updated by PATCH)
    stop()
```
Routes: `POST /oauth2/device` (checks `client_id`, returns `device_code`, `user_code`, `verification_uri` = `base/activate`, `expires_in` 1800, `interval` 1); `POST /oauth2/token` (device grant: 400 `{"status":400,"message":"authorization_pending"}` until approved, then tokens with `expires_in` 14400; refresh grant: rotates both tokens, `refreshes += 1`, or 400 `Invalid refresh token` when broken); `GET /helix/users`, `/helix/streams/key`, `/helix/channels`, `PATCH /helix/channels` (204; records the body; applies it; or the queued rejection), `/helix/search/categories` (case-insensitive substring on `query`), `/helix/streams`, `/helix/videos` (filters `user_id`, `type=archive`), `GET /ingests`, `GET /yt/channel/<id>/live` (200 with `<link rel="canonical" href="https://www.youtube.com/watch?v=<video_id>">` when set, else 200 with a page without a canonical watch link). Every `/helix/*` route answers 401 `{"status":401,"message":"Invalid OAuth token"}` unless `Authorization == "Bearer <current access token>"` and `Client-Id == client_id`.

- [ ] **Step 5: Add the fake's self-test** to `sc_harness_selftest` in `run_e2e.py`: start `FakeTwitch`, `urllib.request` a device code, poll once (expect 400 pending), `approve()`, poll again (expect tokens), call `/helix/users` with and without the bearer (200 / 401), stop. Run: `python tests\e2e\run_e2e.py --only harness_selftest` -> all checks pass.

- [ ] **Step 6: Run the unit tests, expect PASS. Commit**

```powershell
git add src/core/twitch.h src/core/twitch.cpp src/core/CMakeLists.txt tests/unit_tests.cpp tests/e2e/fakes/twitch_api.py tests/e2e/run_e2e.py
git commit -m "feat(core): Twitch Helix client with device-code sign-in, and the fake Twitch server"
```

---

### Task 4: Twitch commands on the control pipe

**Files:**
- Modify: `src/core/app_controller.h`, `src/core/app_controller.cpp:62-140`
- Test: `tests/e2e/run_e2e.py` (new scenario `twitch_auth`)

**Interfaces:**
- Consumes: `TwitchClient` (Task 3), `loadUiPrefs/saveUiPrefs` (`config.h`), `saveSecret` (`secrets.h`), `streamSecretName` (local helper in `app_controller.cpp:16`).
- Produces: pipe commands, all handled in `AppController::handle` before the `extra` hook:

  | cmd | request | response |
  |---|---|---|
  | `twitch_status` | | `{ok, clientIdSet, signedIn, login, userId, displayName, channel, auth:{state: idle\|pending\|ok\|error, userCode, verificationUri, error}}` |
  | `twitch_auth_start` | `{clientId?}` | saves `clientId` into `ui.json` when given; starts the device flow on `twitchThread_`; returns `{ok, userCode, verificationUri, expiresIn}` or `{ok:false, error}` |
  | `twitch_auth_cancel` | | stops the poll thread, `auth.state = idle` |
  | `twitch_sign_out` | | deletes both token secrets; the Twitch profile stays |
  | `twitch_fetch_key` | `{channel}` | `GET /streams/key`, `saveSecret(streamSecretName(channel), key)`; `{ok, streamKeySet:true}`; the key is never in the response |
  | `twitch_get_info` | | `{ok, title, categoryId, categoryName, language, tags}` from `GET /channels` |
  | `twitch_set_info` | `{title, categoryId, language, tags}` | validates, `PATCH /channels`, then saves the same fields into the Twitch profile in `ui.json`; `{ok}` or `{ok:false, error}` |
  | `twitch_search_categories` | `{query}` | `{ok, categories:[{id,name}]}` |
  | `twitch_ingests` | | `{ok, ingests:[{name,url}]}`, url already stripped of `/{stream_key}` |

  Private members: `std::thread twitchThread_; std::mutex twitchMu_; json twitchAuth_ = {{"state","idle"}}; std::atomic<bool> twitchCancel_{false};` and helpers `std::string clientId() const` (from `loadUiPrefs().twitchClientId`), `StreamChannel twitchChannel() const` (the first channel with `platform == "twitch"`, or an empty one), `void upsertTwitchProfile(const TwitchUser& u)`.
  - `upsertTwitchProfile`: id `"tw_" + u.id`, `platform = "twitch"`, `label = u.displayName`, `twitchLogin/UserId/DisplayName`, `studioUrl = "https://dashboard.twitch.tv/u/" + u.login + "/stream-manager"`, `streamUrl = "rtmp://live.twitch.tv/app"` only when the profile is new; `saveUiPrefs`.
  - The auth thread: `startDeviceAuth`, publish `userCode`/`verificationUri` with `state = pending`, then loop `pollDeviceToken` every `interval` seconds (1 s when `VV_LINK_POLL_MS` is set under 5000, so the tests stay fast) until `ok`, `error`, cancel or `expiresIn` elapsed; on `ok` call `me()` and `upsertTwitchProfile`, set `state = ok` with `login`. The destructor sets `twitchCancel_` and joins.
  - `get_config` and `status` gain nothing here; `handle("get_config")` keeps working for `channel = "tw_123456"` because it only uses `streamSecretName`.

- [ ] **Step 1: Write the failing scenario** `sc_twitch_auth(out)` in `run_e2e.py` (register as `"twitch_auth"`), plus a helper reused by later scenarios:

```python
def connect_twitch(c, fake, timeout=15):
    """Signs the GUI into the fake Twitch over the pipe; returns the twitch_status when done."""
    r = c.call(cmd="twitch_auth_start", clientId=fake.client_id)
    assert r.get("ok"), r
    fake.approve()
    end = time.time() + timeout
    while time.time() < end:
        s = c.call(cmd="twitch_status")
        if s.get("signedIn"):
            return s
        time.sleep(0.2)
    return c.call(cmd="twitch_status")
```
Checks of `sc_twitch_auth`, in order (Gui started with `env_extra=fake.env`, settings under `d / "settings"`):
1. `twitch_status` before anything: `signedIn is False`, `clientIdSet is False`, `auth.state == "idle"`.
2. `twitch_auth_start` with `clientId`: `ok`, `userCode == "ABCD-1234"`, `verificationUri == fake.base + "/activate"`; `twitch_status.auth.state == "pending"`, `clientIdSet is True`.
3. `fake.approve()`; within 10 s `twitch_status.signedIn is True`, `login == "voloire"`, `userId == "123456"`, `channel == "tw_123456"`, `auth.state == "ok"`.
4. `ui.json` (`d/settings/ui.json`) has a channel with `id == "tw_123456"`, `platform == "twitch"`, `label == "voloire"`, `streamUrl == "rtmp://live.twitch.tv/app"`, `twitchClientId == fake.client_id`.
5. `twitch_fetch_key channel=tw_123456`: `ok`, `streamKeySet is True`; `d/settings/secrets/stream_key_tw_123456.bin` exists; `get_config channel=tw_123456` has `streamKeySet is True`.
6. `twitch_get_info`: `title == fake.channel["title"]`, `categoryId == fake.channel["game_id"]`.
7. `fake.expire_token()`; `twitch_get_info` still `ok`; `fake.refreshes == 1`.
8. `fake.break_refresh(); fake.expire_token()`; `twitch_get_info` -> `ok is False`, error contains `"sign in"`; `twitch_status.signedIn is False`.
9. `connect_twitch` again (new approval) -> `signedIn True`; `twitch_sign_out` -> `signedIn False`; `secrets/twitch_access_token.bin` gone; `twitch_get_info` -> error contains `"not connected"` and `len(fake.requests)` unchanged.
10. `scan_for_secrets([settings dir, telemetry dir, *all pipe responses as json strings], [every access token the fake issued, every refresh token, fake.stream_key])` -> no hits.

- [ ] **Step 2: Run `python tests\e2e\run_e2e.py --only twitch_auth`, expect the first `twitch_status` check to fail with `unknown command`.**

- [ ] **Step 3: Implement the commands in `app_controller.cpp`** as specified in Interfaces.

- [ ] **Step 4: Run the scenario, expect every check to pass.** Run `build\bin\vv_unit_tests.exe` and `--only stream_stored_key,ui_stream` to confirm nothing regressed.

- [ ] **Step 5: Commit**

```powershell
git add src/core/app_controller.h src/core/app_controller.cpp tests/e2e/run_e2e.py
git commit -m "feat(core): Twitch sign-in, key fetch and broadcast info over the control pipe"
```

---

### Task 5: Share links tracker (Twitch VOD, YouTube watch link)

**Files:**
- Create: `src/core/links.h`, `src/core/links.cpp`
- Modify: `src/core/app_controller.{h,cpp}` (`links_`, `onState` hook, `links` command, `status.links`), `src/core/CMakeLists.txt`
- Modify: `src/ui/panel.cpp:1414` (`app_.setConfig(c)` before `recorder().start`)
- Test: `tests/unit_tests.cpp`, `tests/e2e/run_e2e.py` (scenario `youtube_links`; the Twitch side is proven in Task 6)

**Interfaces:**
- Consumes: `TwitchClient::currentStreamId/videos`, `httpRequest`, `envOr`, `StreamChannel`.
- Produces (`links.h`):
  ```cpp
  std::string youtubeChannelId(const std::string& studioUrlOrId);  // "UC..." (24 chars) found as is or after "/channel/"; "" otherwise
  std::string youtubeLiveUrl(const std::string& channelId);       // envOr(VV_YOUTUBE_BASE, "https://www.youtube.com") + "/channel/" + id + "/live"; "" when id empty
  std::string extractCanonicalVideoId(const std::string& html);   // href=".../watch?v=<id>" of rel="canonical"; "" when absent
  std::string twitchLiveUrl(const std::string& login);            // "https://www.twitch.tv/" + login
  struct LinkStatus {
    std::string platform, liveUrl, vodUrl, state, error;   // state: idle | watching | found | ended
    json toJson() const;
  };
  class LinkTracker {
   public:
    ~LinkTracker();
    void begin(const StreamChannel& ch, const std::string& twitchClientId);  // from any thread; restarts the worker
    void streamEnded();   // YouTube: stop; Twitch: keep polling for the VOD up to 10 min, then "ended"
    LinkStatus status() const;
  };
  ```
  Worker rules: poll every `envOr(VV_LINK_POLL_MS, "20000")` ms. YouTube: `liveUrl` set at `begin` (or `error = "Set the channel ID (UC...) in the YouTube tab to get a share link"` and `state = ended` when no id); each tick `GET liveUrl`, `extractCanonicalVideoId(body)`; when found `vodUrl = "https://youtu.be/" + id`, `state = found`, stop polling. Twitch: `liveUrl = twitchLiveUrl(login)` at `begin`; each tick `currentStreamId` (remember it once non-empty) then `videos`; the first VOD whose `streamId` equals the remembered id (or, when the stream id is unknown yet, the first VOD with `createdAt` later than `begin` time) gives `vodUrl`, `state = found`. Errors from the client go into `error` and polling continues. `streamEnded` with `state == found` sets `state = ended` immediately.
  - `AppController`: member `LinkTracker links_;` in the constructor `rec_.onState = [this](RecState s) { ... }`: on `Recording` when `config().output == "stream"` and the tracker is not already watching this session, `links_.begin(channelById(config().streamChannel), clientId())`; on `Idle`, `links_.streamEnded()`. `channelById` reads `loadUiPrefs().channels`, falls back to a default YouTube profile. Command `links` returns `{ok, links: links_.status().toJson()}`; `status` embeds `links` too.
  - `MainPanel::startRecording` calls `app_.setConfig(c)` right before `app_.recorder().start(c, err)` so the hook sees the channel that is going live.

- [ ] **Step 1: Write the failing unit tests**

```cpp
TEST_CASE("YouTube share link helpers") {
  CHECK(youtubeChannelId("UCDSLIDCQKYVHFLqKEb9CMqw") == "UCDSLIDCQKYVHFLqKEb9CMqw");
  CHECK(youtubeChannelId("https://studio.youtube.com/channel/UCDSLIDCQKYVHFLqKEb9CMqw/editing/profile") == "UCDSLIDCQKYVHFLqKEb9CMqw");
  CHECK(youtubeChannelId("https://studio.youtube.com/").empty());
  _wputenv(L"VV_YOUTUBE_BASE=");
  CHECK(youtubeLiveUrl("UCDSLIDCQKYVHFLqKEb9CMqw") == "https://www.youtube.com/channel/UCDSLIDCQKYVHFLqKEb9CMqw/live");
  CHECK(youtubeLiveUrl("").empty());
  CHECK(extractCanonicalVideoId("<html><link rel=\"canonical\" href=\"https://www.youtube.com/watch?v=dQw4w9WgXcQ\"><body>") == "dQw4w9WgXcQ");
  CHECK(extractCanonicalVideoId("<link rel=\"canonical\" href=\"https://www.youtube.com/channel/UCx\">").empty());
  CHECK(twitchLiveUrl("voloire") == "https://www.twitch.tv/voloire");
}
TEST_CASE("LinkTracker without a YouTube channel id ends at once with a hint") {
  StreamChannel ch;  // default profile, studioUrl https://studio.youtube.com/
  LinkTracker t;
  t.begin(ch, "");
  LinkStatus s = t.status();
  CHECK(s.state == "ended");
  CHECK(s.liveUrl.empty());
  CHECK(s.error.find("channel ID") != std::string::npos);
}
```

- [ ] **Step 2: Run, expect compile failure. Implement `links.cpp` and the controller wiring. Run the unit tests, expect PASS.**

- [ ] **Step 3: Write the failing scenario** `sc_youtube_links(out)` (register `"youtube_links"`): `FakeTwitch` (for `/yt`), `RtmpIngest`, `Subject`, then write `d/settings/ui.json` **before** starting the Gui with `{"channels":[{"id":"default","label":"Main Channel","platform":"youtube","studioUrl":"UCfakefakefakefakefakefa1","streamUrl": ing.url}]}`; Gui with `env_extra=fake.env`. Checks:
1. `ui_override` config `{outDir, name, mic none, sys none, fps 30, telemetryDir}`; `ui_trigger ["Source", <subject item>]`, `["Output", "Stream (live)"]` (menu text after Task 7; until then use the current text and update the string in Task 7), `set_stream_key key=...`, `["Go live"]`; wait `ui_state.state == "recording"`.
2. `links` -> `links.platform == "youtube"`, `links.liveUrl == fake.base + "/yt/channel/UCfakefakefakefakefakefa1/live"`, `links.state == "watching"`, `vodUrl == ""`.
3. `fake.set_youtube_video("UCfakefakefakefakefakefa1", "dQw4w9WgXcQ")`; within 10 s `links.vodUrl == "https://youtu.be/dQw4w9WgXcQ"`, `state == "found"`.
4. `["End stream"]`; within 20 s `state == "idle"`; `links.state == "ended"`, `vodUrl` still there.
5. `status` response contains the same `links` object.

- [ ] **Step 4: Run `--only youtube_links`, make it pass. Commit**

```powershell
git add src/core/links.h src/core/links.cpp src/core/CMakeLists.txt src/core/app_controller.h src/core/app_controller.cpp src/ui/panel.cpp tests/unit_tests.cpp tests/e2e/run_e2e.py
git commit -m "feat(core): share link tracker for the live page and the VOD"
```

---

### Task 6: TWITCH STREAM tab and platform-aware panel

**Files:**
- Modify: `src/ui/panel.h`, `src/ui/panel.cpp` (tab after `YOUTUBE STREAM`, lines ~483-806; Recording-tab stream card 400-480; `syncKeyUi` 1011; `refreshChannelCombos` 1062; `promptAddChannel` 1125; `openYouTubeStudio` 1160; `startRecording` 1410; `poll` 1454-1503)
- Modify: `src/ui/main.cpp:108-150` (`ui_tab`), `src/ui/tray.cpp:130` (`clipboardText`, `links`, `twitch` in `state()`)
- Test: `tests/e2e/run_e2e.py` (scenario `twitch_live`)

**Interfaces:**
- Consumes: pipe commands of Task 4 and 5 through `app_.handle(...)` (never `TwitchClient` directly from the UI).
- Produces, public on `MainPanel`:
  ```cpp
  QString activeChannelPlatform() const;        // "youtube" | "twitch"
  vv::json twitchStatus();                       // app_.handle twitch_status
  void twitchConnect();                          // CONNECT: twitch_auth_start, shows the code dialog, opens the browser, polls every 1 s until done, then reloads prefs_
  void twitchSignOut();
  void twitchFetchKey();                         // worker thread; on ok syncKeyUi()
  void twitchApplyInfo(std::function<void(bool)> done);  // reads the four fields, twitch_set_info on a worker thread
  void twitchPromptTitle();                      // QInputDialog, then twitchApplyInfo (menu entry)
  void twitchPromptCategory();                   // QInputDialog text, twitch_search_categories, picks the first match, then twitchApplyInfo
  void openStudio();                             // YouTube Studio or Twitch stream manager for the active channel (replaces openYouTubeStudio; update control_menu.cpp:249)
  void openVideoProducer();                      // https://dashboard.twitch.tv/u/<login>/content/video-producer
  vv::json shareLinks();                         // app_.handle links
  void copyLiveLink(); void copyVodLink(); void openLiveLink();
  static void openExternal(const QUrl& u);       // QDesktopServices::openUrl unless VV_NO_BROWSER is set
  ```
  Widgets (`panel.h`, all `nullptr` initialised): `twClientIdEdit_`, `twConnectBtn_`, `twSignOutBtn_`, `twStatusLabel_`, `twChannelLabel_`, `twIngestCombo_`, `twIngestRefreshBtn_`, `twKeyStateLabel_`, `twFetchKeyBtn_`, `twKeyEdit_`, `twKeySave_`, `twKeyClear_`, `twTitleEdit_`, `twTitleCount_`, `twCategoryEdit_`, `twCategorySearchBtn_`, `twCategoryCombo_`, `twTagsEdit_`, `twLangCombo_`, `twLoadInfoBtn_`, `twApplyBtn_`, `twApplyOnLiveCheck_`, `twGoLiveBtn_`, `twLiveLinkLabel_`, `twVodLinkLabel_`, `twCopyLiveBtn_`, `twOpenLiveBtn_`, `twCopyVodBtn_`, `twOpenVodBtn_`, `twVideoProducerBtn_`, `twLocalCopyCheck_`, `twOpenFolderBtn_`, `twDashboardBtn_`; plus `QTimer* twAuthTimer_`.

  Tab layout, three cards:
  1. **TWITCH ACCOUNT** - row: `CLIENT ID` edit (placeholder `Client ID of your Twitch app (dev.twitch.tv/console/apps, Public)`, saved to `prefs_->twitchClientId` on `editingFinished`), `CONNECT` / `SIGN OUT`, status label `NOT CONNECTED` or `SIGNED IN AS voloire`. Row: `INGEST SERVER` combo (first item `Default (rtmp://live.twitch.tv/app)`, then the fetched list `name (url)`, last `Custom`) + `REFRESH` + a `QLineEdit` showing the URL (writes `channel.streamUrl`, mirrors into `urlEdit_` exactly like `ytUrlEdit_` does); `STREAM KEY` row: state label, `FETCH KEY FROM TWITCH`, a masked paste field + `SAVE` + `CLEAR` as fallback; `DASHBOARD` button (`openStudio`). Hint label (objectName `sub`): `Encoding and audio are shared with the Recording tab. Twitch accepts up to 6000 kbps for non-partners.`
  2. **BROADCAST INFO (SET BEFORE GOING LIVE)** - `TITLE` edit (`setMaxLength(140)`, counter label `n/140`), `CATEGORY / GAME` search edit + `SEARCH` + results combo (data = id), `TAGS (COMMA SEPARATED, MAX 10)` edit, `LANGUAGE` combo (`it`, `en`, `es`, `fr`, `de`, `pt`, `other`), buttons `LOAD FROM TWITCH`, `APPLY`, checkbox `Apply this info when going live` (bound to `channel.applyInfoOnGoLive`), button `APPLY & GO LIVE` (apply, then `startRecording` when ok). The fields load from the Twitch profile in `refreshChannelCombos` and save into it on `editingFinished`.
  3. **SHARE** - `LIVE LINK` label (selectable text) + `COPY` + `OPEN`; `VOD LINK` label (`not published yet` until found) + `COPY` + `OPEN`; `DOWNLOAD VOD (VIDEO PRODUCER)` button; `Save local .mkv copy while live` checkbox mirrored with `localCopyCheck_` (same pattern as `ytLocalCopyCheck_`) + `OPEN FOLDER` (`outDir`). Refreshed from `shareLinks()` inside `poll()` only while `outputKind() == "stream"` and at most once per second.

  Behaviour rules:
  - `twitchConnect`: `twitch_auth_start` with the field's Client ID; on `ok` show a non-modal `QMessageBox` "Confirm the code **ABCD-1234** in the browser, then come back." with the URI as text, call `openExternal(verificationUri)`, start `twAuthTimer_` (1 s) polling `twitch_status` until `auth.state` is `ok` or `error`; on `ok` do `*prefs_ = vv::loadUiPrefs()` then `refreshChannelCombos()`, `selectChannel("tw_<userId>")`, `syncKeyUi()`; on `error` `showError(...)`.
  - `refreshChannelCombos`: items read `[YT] label  [KEY SET]` / `[TW] label  [NO KEY]`; the Twitch tab fields load only when the active channel is a Twitch one; when no Twitch profile exists the Twitch tab shows the account card enabled and the other two cards disabled.
  - `promptAddChannel` stays YouTube only (a Twitch profile is created by sign-in). `deleteActiveChannel` works for both.
  - `syncKeyUi`: Twitch labels mirror the YouTube ones (`Stream key: SET for 'voloire'`), `studioBtn_` text `STUDIO (...)` for YouTube and `DASHBOARD (...)` for Twitch, Recording-tab card title stays `STREAM`, `outputCombo_` item text `Stream (live)` (`panel.cpp:343`) and `ytOutputCombo_` `Stream (LIVE)` (`panel.cpp:514`), `recBtn_` text in `poll()` unchanged (`GO LIVE (<label>)`).
  - `startRecording`: when `c.output == "stream"`, the active channel is Twitch and `applyInfoOnGoLive` is on, the start thread first calls `app_.handle({{"cmd","twitch_set_info"}, ...fields...})`; a failure posts `showError("Twitch: " + error)` and does not call `recorder().start`. The `starting_` flag covers both steps.
  - `poll()` adds the new editable widgets to the `canEdit` list (`panel.cpp:1502`).
  - `main.cpp` `ui_tab {index}` -> `panel.setActiveTab(index)`; `tray.cpp state()` adds `"clipboardText"` (`QApplication::clipboard()->text()`), `"links"` (`panel_->shareLinks()["links"]`) and `"twitch"` (`panel_->twitchStatus()` minus nothing: it never carries tokens).

- [ ] **Step 1: Write the failing scenario** `sc_twitch_live(out)` (register `"twitch_live"`): `FakeTwitch`, `RtmpIngest(d, app="app", key=fake.stream_key)`, `Subject`, Gui with `env_extra=fake.env`. Checks:
1. `connect_twitch(c, fake)` -> `signedIn`; `ui_state.twitch.login == "voloire"`; `ui_state.twitch` dumped has no token (scan at the end).
2. `twitch_fetch_key channel=tw_123456` -> ok. `ui_override` config `{outDir, name "twlive", mic none, sys none, fps 30, streamUrl: ing.url, recordWhileStreaming: True, telemetryDir}`.
3. `ui_trigger ["Twitch", "voloire"]` (channel entry, Task 7) and `["Output", "Stream (live)"]`; `ui_state.menu` has `Go live` enabled.
4. `twitch_set_info title="Late session" categoryId="509658" language="it" tags=["gaming","chill"]` -> ok; `fake.patches[-1] == {"title": "Late session", "game_id": "509658", "broadcaster_language": "it", "tags": ["gaming", "chill"]}`; `ui.json` channel `tw_123456` has `title == "Late session"`, `categoryId == "509658"`.
5. `twitch_set_info` with 11 tags -> `ok False`, error contains `"10 tags"`, `len(fake.patches)` unchanged.
6. `twitch_search_categories query="just"` -> `[{"id": "509658", "name": "Just Chatting"}]`; `twitch_ingests` -> urls `["rtmp://live.twitch.tv/app", "rtmp://mil.contribute.live-video.net/app"]`.
7. Rejected info path: `fake.reject_patch(409, "title too long")`; `ui_trigger ["Go live"]`; within 5 s `ui_state.state == "idle"`, `ui_state.error` contains `"Twitch"` and `"title too long"`, `ing.received` does not exist.
8. `ui_trigger ["Go live"]`; within 20 s `state == "recording"`, hud `label == "LIVE"`; `ing.wait_connected(20)`; `fake.patches[-1]["title"] == "Late session"` (applied on go live); `ui_state.links.liveUrl == "https://www.twitch.tv/voloire"`, `links.state == "watching"`.
9. `fake.set_live("999"); fake.add_vod("41234567", "999")`; within 10 s `links.vodUrl == "https://www.twitch.tv/videos/41234567"`, `state == "found"`.
10. `ui_trigger ["Share link", "Copy video link"]` -> `ui_state.clipboardText == "https://www.twitch.tv/videos/41234567"`; `["Share link", "Copy live link"]` -> `clipboardText == "https://www.twitch.tv/voloire"`.
11. `ui_tab index=2` then `ui_screenshot dir=<d/shots>` -> the panel file exists and is over 10 kB.
12. `["End stream"]`; idle within 20 s; `links.state == "ended"`; `d/rec/twlive.mkv` exists; `ing.stop()`, `ing.received` decodes clean.
13. `scan_for_secrets([telemetry, settings, *dumps], [all fake access and refresh tokens, fake.stream_key])` -> no hits.

- [ ] **Step 2: Run `--only twitch_live`, expect failures from check 3 on (menu entries and `Stream (live)` do not exist yet).**

- [ ] **Step 3: Implement the tab and the panel changes.** Keep each Twitch call on a worker thread; keep the `loading_` guard pattern for every new `connect`.

- [ ] **Step 4: Build, run `--only twitch_live` (checks 1, 2, 4-9, 11-13 pass; 3 and 10 wait for Task 7), run `--only ui,ui_stream,hud` to confirm the existing UI scenarios still pass (they will need the `Stream (live)` string: update `run_e2e.py:1118-1119` now).**

- [ ] **Step 5: Commit**

```powershell
git add src/ui/panel.h src/ui/panel.cpp src/ui/main.cpp src/ui/tray.cpp tests/e2e/run_e2e.py
git commit -m "feat(ui): Twitch Stream tab with browser sign-in, broadcast info and share links"
```

---

### Task 7: Right-click menu of the status bar and the tray

**Files:**
- Modify: `src/ui/control_menu.cpp:220-269` (Output, YouTube submenu), `:324-337` (sync lambda)
- Test: `tests/e2e/run_e2e.py` (`twitch_live` checks 3 and 10; `sc_ui_stream` strings; one new block in `sc_hud` or `sc_ui` listing the entries)

**Interfaces:**
- Consumes: `MainPanel` methods of Task 6.
- Produces, in `buildControlMenu`, this exact menu text (the harness matches on it):
  - `Output`: `Record to file`, `Stream (live)` (was `Stream to YouTube`).
  - `Share link` submenu, right after `Output`: `Live: <liveUrl or "not available">` (disabled info line), `Copy live link` (enabled when `liveUrl` non-empty, keepOpen), `Copy video link` (enabled when `vodUrl` non-empty, keepOpen), `Open live page` (enabled when `liveUrl` non-empty), `Open video page` (enabled when `vodUrl` non-empty).
  - `YouTube` submenu unchanged except `Open YouTube Studio` calls `openStudio()` and the channel list shows only `platform == "youtube"` profiles.
  - `Twitch` submenu after `YouTube`: info line `Signed in as <displayName>` or `Not connected` (disabled), `Connect Twitch account...` (enabled when idle and not signed in), `Sign out` (idle and signed in), separator, `Stream key: set` / `not set` for the Twitch profile (disabled info), `Fetch stream key` (idle, signed in), separator, `Set title...` (`twitchPromptTitle`, signed in), `Set category...` (`twitchPromptCategory`, signed in), `Apply info when going live` (checkable, keepOpen, bound to `applyInfoOnGoLive`), separator, `Open stream manager` (signed in), `Open video producer` (signed in), separator, `Destination channel:` + one checkable entry per `platform == "twitch"` profile (label text, selects it, keepOpen).
  - The sync lambda also refreshes the `Share link` info line and the enabled state of its four actions from `p->shareLinks()`.
  - `Go live` enabled rule unchanged (`streamKeySet()` for the active channel).

- [ ] **Step 1: Add the failing checks**: in `sc_twitch_live` checks 3 and 10 already exist; add to `sc_ui` (tray menu) a block: with no Twitch profile, `Twitch` submenu has `Not connected` first and `Connect Twitch account...` enabled; `Share link` submenu has `Copy live link` disabled while idle. Update `sc_ui_stream` to `["Output", "Stream (live)"]`.

- [ ] **Step 2: Run `--only twitch_live,ui,ui_stream,hud`, expect the menu checks to fail.**

- [ ] **Step 3: Implement the menu changes.**

- [ ] **Step 4: Run `--only twitch_live,ui,ui_stream,hud,youtube_links`, expect every check to pass. Commit**

```powershell
git add src/ui/control_menu.cpp tests/e2e/run_e2e.py
git commit -m "feat(ui): Twitch and Share link entries in the status bar and tray menus"
```

---

### Task 8: YouTube share-link card, docs, pull request

**Files:**
- Modify: `src/ui/panel.cpp` (YOUTUBE STREAM tab: card 4 `SHARE`), `src/ui/panel.h`
- Modify: `README.md:3,17,46,67,70,78`
- Test: `tests/e2e/run_e2e.py` (`youtube_links` gains the panel checks)

- [ ] **Step 1: Add the failing checks to `sc_youtube_links`**: after go live, `ui_tab index=1` + `ui_screenshot` produces a panel file; `ui_trigger ["Share link", "Copy live link"]` -> `clipboardText == links.liveUrl`; after the fake publishes the video, `["Share link", "Copy video link"]` -> `clipboardText == "https://youtu.be/dQw4w9WgXcQ"`. Also a profile without a channel id: write `ui.json` with `studioUrl: "https://studio.youtube.com/"` in a second Gui run, go live, `links.state == "ended"` and `links.error` contains `"channel ID"`; the `Share link` info line reads `Live: not available`.

- [ ] **Step 2: Add the `SHARE` card to the YouTube tab** with the same five widgets as the Twitch share card (`ytLiveLinkLabel_`, `ytVodLinkLabel_`, copy/open buttons) bound to the same `copyLiveLink/copyVodLink/openLiveLink` and refreshed in the same `poll()` block; the `CHANNEL ID (UC...) OR STUDIO URL` placeholder gains `(needed for the share link)`.

- [ ] **Step 3: README.** Line 3: "to a file or live to YouTube or Twitch". Line 17: add the Twitch sentence (browser sign-in with a Public app's Client ID, key fetched from the account, title/category/tags/language set before going live, share links). Line 46: a new paragraph "To go live on Twitch: register a Public app at https://dev.twitch.tv/console/apps once and paste its Client ID in the Twitch tab; `CONNECT` opens the browser, confirm the code; `FETCH KEY FROM TWITCH`; fill title, category, tags; `GO LIVE`. The live link is on the `Share link` menu at once, the VOD link as soon as Twitch publishes it (turn on *Store past broadcasts* in the channel's stream settings); `DOWNLOAD VOD` opens the Video Producer page. Keep *Save local .mkv copy while live* on to have the file to upload to YouTube." Line 67: scenario count +3. Line 70: name the three scenarios. Line 78: add the new pipe commands.

- [ ] **Step 4: Full local verification**

```powershell
scripts\build.ps1
build\bin\vv_unit_tests.exe
python tests\e2e\run_e2e.py --only harness_selftest,twitch_auth,twitch_live,youtube_links,ui,ui_stream,hud,stream_rtmp,stream_stored_key
```
Expected: unit tests `all tests passed`; every scenario reports 0 failed checks.

- [ ] **Step 5: Commit, push, open the pull request**

```powershell
git add src/ui/panel.h src/ui/panel.cpp README.md tests/e2e/run_e2e.py
git commit -m "feat(ui): YouTube share links card and docs for Twitch streaming"
git push -u origin feature/twitch-stream
gh pr create --title "Twitch streaming tab with browser sign-in and share links (1.4.0)" --body-file docs/superpowers/plans/2026-10-25-twitch-stream.md
```
Merge when CI is green (the GUI scenarios run locally only, as today; paste their summary in the PR description). Then `git worktree remove ..\voloire-video-twitch` after the merge.

---

## Verification (end to end, by hand, after the merge)

1. Register the Public app once at https://dev.twitch.tv/console/apps (name `Voloire-video`, category Application Integration, client type Public, OAuth redirect `http://localhost`), copy the Client ID.
2. Start the app, Twitch tab: paste the Client ID, `CONNECT`, confirm the code in Brave, see `SIGNED IN AS voloire`; `FETCH KEY FROM TWITCH` -> `Stream key: SET`.
3. Fill title, search `Just Chatting`, tags `gaming,chill`, language `it`, `APPLY`; check them on https://dashboard.twitch.tv/u/voloire/stream-manager.
4. Recording tab: `Output > Stream (live)`, channel `[TW] voloire`, `GO LIVE`. Right-click the bar: `Share link > Copy live link` gives `https://www.twitch.tv/voloire`; a few minutes in, `Copy video link` gives the `twitch.tv/videos/...` URL (requires *Store past broadcasts* on).
5. `End stream`; the `.mkv` is in the recordings folder; `DOWNLOAD VOD` opens the Video Producer page.
6. YouTube profile with the `UC...` id set: go live, `Share link > Copy live link` gives the `/channel/<id>/live` URL at once; `Copy video link` gives `youtu.be/...` once YouTube publishes the broadcast page.
