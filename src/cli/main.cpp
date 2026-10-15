// vvcli: headless front end of Voloire-video, and the entry point of the end-to-end harness.
#include <windows.h>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <thread>
#include "app_controller.h"
#include "control.h"
#include "crash_handler.h"
#include "devices.h"
#include "telemetry.h"

using namespace vv;

namespace {

void usage() {
  std::puts(
      "vvcli " VV_VERSION "\n"
      "  vvcli list windows|monitors|devices\n"
      "  vvcli record [options]\n"
      "  vvcli ctl <pipe> '<json request>'\n"
      "record options:\n"
      "  --window-title T | --window-exe E | --pid N | --hwnd N | --foreground | --monitor N | --dxgi N | --synthetic\n"
      "  --sys app|system|none|synthetic   --mic default|none|synthetic|device:<id>\n"
      "  --duration S  --out DIR  --name N  --fps N  --height N  --bitrate KBPS\n"
      "  --codec h264|hevc|av1  --container mkv|mp4  --encoder auto|nvenc|amf|qsv|mf\n"
      "  --pipe NAME (control server)  --telemetry-dir DIR  --synth-sys-hz F  --synth-mic-hz F  --verbose");
}

int ctl(const std::string& pipe, const std::string& req) {
  std::string path = "\\\\.\\pipe\\" + pipe;
  HANDLE h = INVALID_HANDLE_VALUE;
  for (int i = 0; i < 50 && h == INVALID_HANDLE_VALUE; ++i) {
    h = CreateFileW(wide(path).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) Sleep(100);
  }
  if (h == INVALID_HANDLE_VALUE) { std::fprintf(stderr, "cannot open %s\n", path.c_str()); return 2; }
  std::string out = req + "\n";
  DWORD n;
  WriteFile(h, out.data(), (DWORD)out.size(), &n, nullptr);
  std::string acc;
  char buf[4096];
  while (acc.find('\n') == std::string::npos && ReadFile(h, buf, sizeof buf, &n, nullptr) && n) acc.append(buf, n);
  CloseHandle(h);
  std::puts(acc.c_str());
  return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  SetConsoleOutputCP(CP_UTF8);
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);  // physical pixel sizes everywhere
  installCrashHandler();
  std::vector<std::string> a;
  for (int i = 1; i < argc; ++i) a.push_back(utf8(argv[i]));
  if (a.empty() || a[0] == "--help" || a[0] == "-h") { usage(); return a.empty() ? 1 : 0; }
  if (a[0] == "--version") { std::puts(VV_VERSION); return 0; }

  AppController app;
  if (a[0] == "list" && a.size() >= 2) {
    json r = app.handle({{"cmd", a[1] == "windows" ? "list_windows" : a[1] == "monitors" ? "list_monitors" : "list_devices"}});
    std::puts(r.dump(2).c_str());
    return 0;
  }
  if (a[0] == "ctl" && a.size() >= 3) return ctl(a[1], a[2]);
  if (a[0] != "record") { usage(); return 1; }

  RecordConfig c;
  c.outDir = defaultOutDir();
  json hints = json::object();
  double duration = 10;
  std::string pipe;
  bool verbose = false, sysSet = false, micSet = false;
  for (size_t i = 1; i < a.size(); ++i) {
    auto need = [&](const char* name) -> std::string {
      if (i + 1 >= a.size()) { std::fprintf(stderr, "%s needs a value\n", name); std::exit(1); }
      return a[++i];
    };
    const std::string& k = a[i];
    if (k == "--window-title") hints["windowTitle"] = need("--window-title");
    else if (k == "--window-exe") hints["windowExe"] = need("--window-exe");
    else if (k == "--foreground") hints["foreground"] = true;
    else if (k == "--pid") c.pid = (uint32_t)std::stoul(need("--pid"));
    else if (k == "--hwnd") c.hwnd = std::stoll(need("--hwnd"), nullptr, 0);
    else if (k == "--monitor") { c.videoSource = "monitor"; c.monitor = std::stoi(need("--monitor")); }
    else if (k == "--dxgi") { c.videoSource = "dxgi"; c.monitor = std::stoi(need("--dxgi")); }
    else if (k == "--synthetic") { c.videoSource = "synthetic"; }
    else if (k == "--sys") { c.sys = need("--sys"); sysSet = true; }
    else if (k == "--mic") {
      micSet = true;
      std::string m = need("--mic");
      if (m.rfind("device:", 0) == 0) { c.mic = "device"; c.micDevice = m.substr(7); } else c.mic = m;
    }
    else if (k == "--duration") duration = std::stod(need("--duration"));
    else if (k == "--out") c.outDir = need("--out");
    else if (k == "--name") c.name = need("--name");
    else if (k == "--fps") c.fps = std::stoi(need("--fps"));
    else if (k == "--height") c.outHeight = std::stoi(need("--height"));
    else if (k == "--bitrate") c.bitrateKbps = std::stoi(need("--bitrate"));
    else if (k == "--codec") c.codec = need("--codec");
    else if (k == "--container") c.container = need("--container");
    else if (k == "--encoder") c.encoder = need("--encoder");
    else if (k == "--telemetry-dir") c.telemetryDir = need("--telemetry-dir");
    else if (k == "--synth-sys-hz") c.synthSysHz = std::stod(need("--synth-sys-hz"));
    else if (k == "--synth-mic-hz") c.synthMicHz = std::stod(need("--synth-mic-hz"));
    else if (k == "--pipe") pipe = need("--pipe");
    else if (k == "--verbose") verbose = true;
    else { std::fprintf(stderr, "unknown option %s\n", k.c_str()); return 1; }
  }
  if (c.videoSource == "synthetic" && !sysSet) c.sys = "synthetic";
  if (c.videoSource == "synthetic" && !micSet) c.mic = "synthetic";
  Telemetry::get().setConsoleEcho(verbose);

  std::atomic<bool> quit{false};
  app.onShutdown = [&] { quit = true; };
  ControlServer server;
  if (!pipe.empty()) {
    std::string e;
    if (!server.start(pipe, [&](const json& r) { return app.handle(r); }, e)) {
      std::fprintf(stderr, "%s\n", e.c_str());
      return 2;
    }
  }

  std::string err = app.resolveTarget(c, hints);
  if (!err.empty()) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
  app.setConfig(c);
  if (!app.recorder().start(c, err)) { std::fprintf(stderr, "start failed: %s\n", err.c_str()); return 3; }
  std::fprintf(stderr, "recording to %s\n", app.recorder().status()["path"].get<std::string>().c_str());

  while (!quit && app.recorder().state() != RecState::Idle) {
    if (duration > 0 && app.recorder().elapsedUs() >= (int64_t)(duration * 1e6)) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  RecResult r;
  if (app.recorder().state() != RecState::Idle) r = app.recorder().stop();
  else r = app.recorder().lastResult();
  server.stop();
  json out = r.toJson();
  std::puts(out.dump().c_str());
  return r.error.empty() && r.bytes > 0 ? 0 : 4;
}
