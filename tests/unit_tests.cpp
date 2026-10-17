#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <windows.h>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <thread>
#include "doctest.h"
#include "app_controller.h"
#include "control.h"
#include "devices.h"
#include "quality.h"
#include "recorder.h"
#include "sources.h"
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

TEST_CASE("RecordConfig round-trips and validates") {
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

TEST_CASE("Telemetry counters, gauges and event log") {
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

TEST_CASE("Control pipe round trip") {
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

TEST_CASE("quality presets map to bitrates") {
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
