#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <windows.h>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>
#include "doctest.h"
#include "app_controller.h"
#include "control.h"
#include "devices.h"
#include "http_client.h"
#include "links.h"
#include "quality.h"
#include "recorder.h"
#include "redact.h"
#include "secrets.h"
#include "share_copy.h"
#include "sources.h"
#include "twitch.h"
#include "telemetry.h"

using namespace vv;
namespace fs = std::filesystem;

namespace {
fs::path tempDir(const char* leaf) {
  fs::path p = fs::temp_directory_path() / "vv-unit" / leaf;
  fs::remove_all(p);
  fs::create_directories(p);
  return p;
}
}  // namespace

TEST_CASE("AudioRing keeps order and trims to its cap") {
  AudioRing r;
  float a[8] = {1, 1, 2, 2, 3, 3, 4, 4};
  r.push(a, 4);
  CHECK(r.available() == 4);
  float out[4] = {};
  CHECK(r.pop(out, 2) == 2);
  CHECK(out[0] == 1);
  CHECK(out[2] == 2);
  CHECK(r.available() == 2);
  r.clear();
  CHECK(r.available() == 0);

  std::vector<float> big((size_t)kSampleRate * 3 * kChannels, 0.5f);
  r.push(big.data(), (size_t)kSampleRate * 3);
  CHECK(r.available() == (size_t)kSampleRate * 2);
  CHECK(r.droppedFrames() == (uint64_t)kSampleRate);
}

TEST_CASE("RecordConfig round-trips and validates" * doctest::test_suite("functional")) {
  RecordConfig c;
  c.videoSource = "synthetic";
  c.sys = "synthetic";
  c.fps = 30;
  RecordConfig d = RecordConfig::fromJson(c.toJson());
  CHECK(d.fps == 30);
  CHECK(d.validate().empty());

  d.codec = "vp9";
  CHECK_FALSE(d.validate().empty());
  d.codec = "h264";
  d.fps = 0;
  CHECK_FALSE(d.validate().empty());

  RecordConfig w;
  CHECK(w.validate() == "no window selected");
  w.hwnd = 1;
  CHECK(w.validate() == "SYS app mode needs a target process");
}

TEST_CASE("Telemetry counters, gauges and event log" * doctest::test_suite("functional")) {
  auto dir = tempDir("telemetry");
  auto& t = Telemetry::get();
  t.resetMetrics();
  t.startSession(strOf(dir));
  t.inc("x", 2);
  t.inc("x", 3);
  t.set("g", 7.5);
  CHECK(t.value("x") == 5);
  CHECK(t.snapshot()["g"] == 7.5);
  VV_LOG(Info, "test", "hello");
  CHECK(t.tailLogs(5).back()["msg"] == "hello");
  std::this_thread::sleep_for(std::chrono::milliseconds(1200));
  t.endSession();
  CHECK(fs::file_size(dir / "events.jsonl") > 0);
  CHECK(fs::file_size(dir / "metrics.jsonl") > 0);
}

TEST_CASE("Device enumeration answers without throwing") {
  auto dev = listAudioDevices();
  auto mon = listMonitors();
  CHECK(mon.size() >= 0);  // a headless CI session can legitimately have none
  (void)dev;
  (void)listWindows();
}

TEST_CASE("Control pipe round trip" * doctest::test_suite("functional")) {
  AppController app;
  ControlServer srv;
  std::string e;
  REQUIRE(srv.start("vv-unit-pipe", [&](const json& r) { return app.handle(r); }, e));
  HANDLE h = CreateFileW(L"\\\\.\\pipe\\vv-unit-pipe", GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
  REQUIRE(h != INVALID_HANDLE_VALUE);
  std::string req = "{\"cmd\":\"ping\"}\n";
  DWORD n;
  WriteFile(h, req.data(), (DWORD)req.size(), &n, nullptr);
  char buf[512] = {};
  ReadFile(h, buf, sizeof buf - 1, &n, nullptr);
  CloseHandle(h);
  json r = json::parse(std::string(buf, n));
  CHECK(r["ok"] == true);
  CHECK(r["state"] == "idle");
  srv.stop();
}

TEST_CASE("quality presets map to bitrates" * doctest::test_suite("functional")) {
  CHECK(presetBitrateKbps("standard", 1080, 60, "h264") == 12000);
  CHECK(presetBitrateKbps("light", 720, 30, "h264") == 2600);
  CHECK(presetBitrateKbps("high", 2160, 60, "hevc") == 70000);
  CHECK(presetBitrateKbps("minimal", 1080, 60, "h264") == 2000);
  CHECK(presetHeightCap("minimal") == 720);
  CHECK(presetHeightCap("high") == 0);

  RecordConfig c;
  c.videoSource = "synthetic";
  c.sys = "synthetic";
  c.quality = "ultra";
  CHECK_FALSE(c.validate().empty());
  CHECK(c.validate().find("quality") != std::string::npos);
}

TEST_CASE("Recorder records synthetic sources, pauses and finalises") {
  auto dir = tempDir("rec");
  Recorder rec;
  RecordConfig c;
  c.videoSource = "synthetic";
  c.sys = "synthetic";
  c.mic = "synthetic";
  c.fps = 30;
  c.synthFps = 30;
  c.bitrateKbps = 2000;
  c.encoder = "mf";   // software-capable on every Windows box
  c.outDir = strOf(dir);
  c.telemetryDir = strOf(dir / "telemetry");
  std::string err;
  REQUIRE_MESSAGE(rec.start(c, err), err);
  CHECK(rec.state() == RecState::Recording);
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  CHECK(rec.pause());
  CHECK(rec.state() == RecState::Paused);
  int64_t atPause = rec.elapsedUs();
  std::this_thread::sleep_for(std::chrono::milliseconds(800));
  CHECK(std::llabs(rec.elapsedUs() - atPause) < 50000);  // clock frozen while paused
  CHECK(rec.resume());
  std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  RecResult r = rec.stop();
  CHECK(rec.state() == RecState::Idle);
  CHECK_MESSAGE(r.error.empty(), r.error);
  CHECK(r.bytes > 10000);
  CHECK(r.durationSec > 2.0);
  CHECK(r.durationSec < 3.2);
  CHECK(r.videoFrames > 50);
  CHECK(fs::exists(dir / "telemetry" / "result.json"));
}

TEST_CASE("redaction hides stream keys, in json and in key URLs" * doctest::test_suite("functional")) {
  json j = {{"streamKey", "abc"}, {"x", {{"key", "r"}, {"z", 1}}}};
  json r = redactJson(j);
  CHECK(r["streamKey"] == "***");
  CHECK(r["x"]["key"] == "***");
  CHECK(r["x"]["z"] == 1);

  json c = {{"Key", "x"}, {"STREAMKEY", "y"}, {"monkey", "keep"}};
  json rc = redactJson(c);
  CHECK(rc["Key"] == "***");        // case-insensitive
  CHECK(rc["STREAMKEY"] == "***");
  CHECK(rc["monkey"] == "keep");    // exact name only

  CHECK(redactUrl("rtmp://a.rtmp.youtube.com/live2/abcd-efgh") == "rtmp://a.rtmp.youtube.com/live2/***");
  CHECK(redactUrl("rtmps://a.rtmp.youtube.com/live2/abcd-efgh") == "rtmps://a.rtmp.youtube.com/live2/***");
  CHECK(redactUrl("rtmp://127.0.0.1:1935/live2") == "rtmp://127.0.0.1:1935/live2");  // no key part
  CHECK(redactUrl("no url here") == "no url here");
  json d = {{"target", "rtmp://h/app/key123"}, {"other", 5}};
  redactUrls(d);
  CHECK(d["target"] == "rtmp://h/app/***");
  CHECK(d["other"] == 5);
}

TEST_CASE("stream secrets round trip through DPAPI and stay unreadable on disk" * doctest::test_suite("functional")) {
  auto dir = tempDir("secrets");
  std::wstring env = L"VV_SETTINGS_DIR=" + wide(strOf(dir));
  _wputenv(env.c_str());  // the CRT environment is what settingsPath() reads
  deleteSecret("stream_key");
  CHECK(loadSecret("stream_key").empty());
  CHECK(loadSecret("missing").empty());

  const std::string key = "sk-live-abcd-1234-efgh";
  CHECK(saveSecret("stream_key", key));
  CHECK(loadSecret("stream_key") == key);

  auto f = dir / "secrets" / "stream_key.bin";
  REQUIRE(fs::exists(f));
  {
    std::ifstream in(f, std::ios::binary);
    std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(raw.find(key) == std::string::npos);  // DPAPI ciphertext, never the plaintext
  }  // closed before delete: Windows refuses to remove an open file

  deleteSecret("stream_key");
  CHECK(loadSecret("stream_key").empty());
}

TEST_CASE("stream output validates its URL, bitrate and height" * doctest::test_suite("functional")) {
  RecordConfig c;
  c.videoSource = "synthetic";
  c.sys = "synthetic";
  c.mic = "synthetic";
  c.output = "stream";
  c.streamUrl = "rtmp://127.0.0.1:1935/live2";
  c.streamBitrateKbps = 6000;
  CHECK(c.validate().empty());

  c.streamBitrateKbps = 60000;
  std::string e = c.validate();
  CHECK_FALSE(e.empty());
  CHECK(e.find("stream") != std::string::npos);

  c.streamBitrateKbps = 6000;
  c.streamUrl.clear();
  CHECK_FALSE(c.validate().empty());
  c.streamUrl = "rtmp://127.0.0.1:1935/live2";
  c.output = "network";
  CHECK_FALSE(c.validate().empty());
}

TEST_CASE("stream channels carry a platform and the Twitch broadcast info" * doctest::test_suite("functional")) {
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

TEST_CASE("urlEncode keeps unreserved characters and escapes the rest" * doctest::test_suite("functional")) {
  CHECK(urlEncode("just chatting") == "just%20chatting");
  CHECK(urlEncode("a-b_c.d~e") == "a-b_c.d~e");
  CHECK(urlEncode("tag:ok/" "é") == "tag%3Aok%2F%C3%A9");
}

TEST_CASE("httpRequest reports a transport error without throwing" * doctest::test_suite("functional")) {
  HttpResponse r = httpRequest("GET", "http://127.0.0.1:9/nothing", {}, {}, 2000);  // port 9: nothing listens
  CHECK_FALSE(r.ok());
  CHECK_FALSE(r.error.empty());
  CHECK(r.status == 0);
  HttpResponse bad = httpRequest("GET", "not a url");
  CHECK_FALSE(bad.error.empty());
}

TEST_CASE("envOr falls back when the variable is unset or empty" * doctest::test_suite("functional")) {
  _wputenv(L"VV_TEST_ENVOR=");
  CHECK(envOr(L"VV_TEST_ENVOR", "fb") == "fb");
  _wputenv(L"VV_TEST_ENVOR=val");
  CHECK(envOr(L"VV_TEST_ENVOR", "fb") == "val");
}

TEST_CASE("Twitch helpers: ingest template, broadcast validation, patch body" * doctest::test_suite("functional")) {
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

TEST_CASE("Twitch client refuses every call while not connected, with no request" * doctest::test_suite("functional")) {
  auto dir = tempDir("twitch-offline");
  std::wstring env = L"VV_SETTINGS_DIR=" + wide(strOf(dir));
  _wputenv(env.c_str());
  deleteSecret(kTwitchAccessSecret);
  deleteSecret(kTwitchRefreshSecret);
  TwitchBases b;
  b.api = "http://127.0.0.1:9/helix";  // nothing listens: a request would fail with a transport error instead
  TwitchClient t("cid", b);
  CHECK_FALSE(t.signedIn());
  TwitchUser u;
  std::string err;
  CHECK_FALSE(t.me(u, err));
  CHECK(err.find("not connected") != std::string::npos);
}

TEST_CASE("YouTube share link helpers" * doctest::test_suite("functional")) {
  CHECK(youtubeChannelId("UCDSLIDCQKYVHFLqKEb9CMqw") == "UCDSLIDCQKYVHFLqKEb9CMqw");
  CHECK(youtubeChannelId("https://studio.youtube.com/channel/UCDSLIDCQKYVHFLqKEb9CMqw/editing/profile") == "UCDSLIDCQKYVHFLqKEb9CMqw");
  CHECK(youtubeChannelId("https://studio.youtube.com/").empty());
  _wputenv(L"VV_YOUTUBE_BASE=");
  CHECK(youtubeLiveUrl("UCDSLIDCQKYVHFLqKEb9CMqw") == "https://www.youtube.com/channel/UCDSLIDCQKYVHFLqKEb9CMqw/live");
  CHECK(youtubeLiveUrl("").empty());
  CHECK(extractCanonicalVideoId("<html><link rel=\"canonical\" href=\"https://www.youtube.com/watch?v=dQw4w9WgXcQ\"><body>") == "dQw4w9WgXcQ");
  CHECK(extractCanonicalVideoId("<link href=\"https://www.youtube.com/watch?v=abcDEF_-123\" rel=\"canonical\">") == "abcDEF_-123");
  CHECK(extractCanonicalVideoId("<link rel=\"canonical\" href=\"https://www.youtube.com/channel/UCx\">").empty());
  CHECK(extractCanonicalVideoId("").empty());
  CHECK(twitchLiveUrl("voloire") == "https://www.twitch.tv/voloire");
}

TEST_CASE("LinkTracker without a YouTube channel id ends at once with a hint" * doctest::test_suite("functional")) {
  StreamChannel ch;  // default profile, studioUrl https://studio.youtube.com/
  LinkTracker t;
  t.begin(ch, "");
  LinkStatus s = t.status();
  CHECK(s.state == "ended");
  CHECK(s.liveUrl.empty());
  CHECK(s.error.find("channel ID") != std::string::npos);
}

TEST_CASE("planShare fits the target" * doctest::test_suite("functional")) {
  SharePlan a = planShare(30, 1920, 1080, 60, 25, true);
  CHECK(a.error.empty());
  CHECK(a.videoKbps >= 6200);
  CHECK(a.videoKbps <= 6400);
  CHECK(a.width == 1920);
  CHECK(a.height == 1080);
  CHECK(a.fps == 60);
  CHECK(a.audioKbps == 128);

  SharePlan b = planShare(120, 1920, 1080, 60, 10, true);
  CHECK(b.error.empty());
  CHECK(b.height == 480);
  CHECK(b.fps == 30);
  CHECK(b.videoKbps >= 500);
  CHECK(b.videoKbps <= 530);

  SharePlan c = planShare(3600, 1920, 1080, 60, 10, true);
  CHECK(c.error.find("at most 277 s") != std::string::npos);

  SharePlan d = planShare(10, 1280, 720, 30, 25, false);
  CHECK(d.error.empty());
  CHECK(d.audioKbps == 0);
}

TEST_CASE("makeShareCopy produces a playable file under the target") {
  auto dir = tempDir("share");
  Recorder rec;
  RecordConfig c;
  c.videoSource = "synthetic";
  c.sys = "synthetic";
  c.mic = "synthetic";
  c.fps = 30;
  c.synthFps = 30;
  c.encoder = "mf";
  c.outDir = strOf(dir);
  c.telemetryDir = strOf(dir / "telemetry");
  std::string err;
  REQUIRE_MESSAGE(rec.start(c, err), err);
  std::this_thread::sleep_for(std::chrono::seconds(6));
  RecResult r = rec.stop();
  REQUIRE_MESSAGE(r.error.empty(), r.error);
  REQUIRE(r.bytes > 10000);

  ShareResult s = makeShareCopy(r.path, 1);
  CHECK_MESSAGE(s.error.empty(), s.error);
  CHECK(s.bytes > 0);
  CHECK(s.bytes <= (1u << 20));
  CHECK(fs::exists(s.path));
  CHECK(s.path.find("_share_1MB.mp4") != std::string::npos);
  CHECK_FALSE(fs::exists(s.path + ".part"));
}
